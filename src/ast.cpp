#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <stdio.h>
#include <fstream>
#include <stack>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils.h>

#include "pizza/ast.h"
#include "pizza/jit.h"

using namespace llvm;

enum Token
{
  tok_eof = -1,
  tok_base = -2,
  tok_topping = -3,
  tok_identifier = -4,
  tok_number = -5,
  tok_sauce = -6,
  tok_if = -7,
  tok_then = -8,
  tok_else = -9,
  tok_for = -10,
  tok_in = -11,
  tok_binary = -12,
  tok_unary = -13
};

static bool replMode;
static FILE *srcFile;
static std::ofstream jsonFile;
static std::unique_ptr<raw_fd_ostream> llFile;
static std::string IdentifierStr;
static double NumVal;

static int getNextChar()
{
  if (replMode)
    return getchar();
  else
    return fgetc(srcFile);
}

static int gettok()
{
  static int LastChar = ' ';

  while (isspace(LastChar))
    LastChar = getNextChar();

  if (isalpha(LastChar))
  {
    IdentifierStr = LastChar;
    while (isalnum((LastChar = getNextChar())))
      IdentifierStr += LastChar;

    if (IdentifierStr == "base")
      return tok_base;
    if (IdentifierStr == "topping")
      return tok_topping;
    if (IdentifierStr == "sauce")
      return tok_sauce;
    if (IdentifierStr == "if")
      return tok_if;
    if (IdentifierStr == "then")
      return tok_then;
    if (IdentifierStr == "else")
      return tok_else;
    if (IdentifierStr == "for")
      return tok_for;
    if (IdentifierStr == "in")
      return tok_in;
    if (IdentifierStr == "binary")
      return tok_binary;
    if (IdentifierStr == "unary")
      return tok_unary;
    if (IdentifierStr == "topping")
      return tok_topping;
    return tok_identifier;
  }

  if (isdigit(LastChar) || LastChar == '.')
  { // Number: [0-9.]+
    std::string NumStr;
    do
    {
      NumStr += LastChar;
      LastChar = getNextChar();
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), nullptr);
    return tok_number;
  }

  if (LastChar == '#')
  {
    do
      LastChar = getNextChar();
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF)
      return gettok();
  }

  // Check for end of file.  Don't eat the EOF.
  if (LastChar == EOF)
    return tok_eof;

  // Otherwise, just return the character as its ascii value.
  int ThisChar = LastChar;
  LastChar = getNextChar();
  return ThisChar;
}

namespace
{
  Value *LogErrorV(const char *Str);
  static int CurTok;
  static int getNextToken();
  static std::map<char, int> BinopPrecedence;
  static std::unique_ptr<LLVMContext> TheContext;
  static std::unique_ptr<Module> TheModule;
  static std::unique_ptr<IRBuilder<>> Builder;
  static std::unique_ptr<legacy::FunctionPassManager> TheFPM;
  static std::unique_ptr<Pizza::JIT> TheJIT;
  Function *getFunction(const std::string &Name);

  class ExprAST
  {
  public:
    virtual ~ExprAST() {}
    virtual const std::string dump() const = 0;
    virtual Value *codegen() = 0;
  };

  class NumberExprAST : public ExprAST
  {
    double Val;

  public:
    NumberExprAST(double Val) : Val(Val) {}

    Value *codegen() override;

    const std::string dump() const override
    {
      return "{\"num\":" + std::to_string(Val) + "}";
    }
  };

  class VariableExprAST : public ExprAST
  {
    std::string Name;

  public:
    VariableExprAST(const std::string &Name) : Name(Name) {}

    Value *codegen() override;

    const std::string getName() const
    {
      return Name;
    }

    const std::string dump() const override
    {
      std::string str = "{\"var\":\"";
      str += this->Name;
      str += "\"}";

      return str;
    }
  };

  class BinaryExprAST : public ExprAST
  {
    char Op;
    std::unique_ptr<ExprAST> LHS, RHS;

  public:
    BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
                  std::unique_ptr<ExprAST> RHS)
        : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

    Value *codegen() override;

    const std::string dump() const override
    {
      std::string str = "{\"op\":\"";
      str += this->Op;
      str += "\",\"lhs\":";
      str += this->LHS->dump();
      str += ",\"rhs\":";
      str += this->RHS->dump();
      str += "}";

      return str;
    }
  };

  class CallExprAST : public ExprAST
  {
    std::string Callee;
    std::vector<std::unique_ptr<ExprAST>> Args;

  public:
    CallExprAST(const std::string &Callee,
                std::vector<std::unique_ptr<ExprAST>> Args)
        : Callee(Callee), Args(std::move(Args)) {}

    const std::string dump() const override
    {
      std::string str = "{\"callee\":\"";
      str += this->Callee;
      str += "\",\"args\":[";

      for (const auto &arg : this->Args)
      {
        str += arg->dump() + ",";
      }
      if (this->Args.size() > 0)
      {
        str = str.substr(0, str.size() - 1);
      }
      str += "]}";

      return str;
    }

    Value *codegen() override;
  };

  class PrototypeAST
  {
    std::string Name;
    std::vector<std::string> Args;
    bool IsOperator;
    unsigned Precedence; // Precedence if a binary op.

  public:
    PrototypeAST(const std::string &name, std::vector<std::string> Args, bool IsOperator = false, unsigned Prec = 0)
        : Name(name), Args(std::move(Args)), IsOperator(IsOperator), Precedence(Prec) {}

    const std::string &getName() const { return Name; }

    bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
    bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

    char getOperatorName() const
    {
      assert(isUnaryOp() || isBinaryOp());
      return Name[Name.size() - 1];
    }

    unsigned getBinaryPrecedence() const { return Precedence; }

    const std::string dump()
    {
      std::string str = "{\"name\":";
      if (this->Name.size() > 0)
      {
        str += "\"" + this->Name + "\"";
      }
      else
      {
        str += "null";
      }
      str += ",\"args\":[";

      for (const auto &arg : this->Args)
      {
        str += "\"" + arg + "\",";
      }
      if (this->Args.size() > 0)
      {
        str = str.substr(0, str.size() - 1);
      }
      str += "]}";

      return str;
    }

    Function *codegen();
  };

  class FunctionAST
  {
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;
    std::string Name;

  public:
    FunctionAST(std::unique_ptr<PrototypeAST> Proto,
                std::unique_ptr<ExprAST> Body)
        : Proto(std::move(Proto)), Body(std::move(Body)) {
          Name = this->Proto->getName();
        }

    const std::string& getName() {
      return Name;
    }

    const std::string dump()
    {
      std::string str = "{\"function\":{\"proto\":";
      str += this->Proto->dump();
      str += ",\"body\":";
      str += this->Body->dump();
      str += "}}";
      return str;
    }

    Function *codegen();
  };

  class IfExprAST : public ExprAST
  {
    std::unique_ptr<ExprAST> Cond, Then, Else;

  public:
    IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
              std::unique_ptr<ExprAST> Else)
        : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}

    Value *codegen() override;

    const std::string dump() const override
    {
      std::string str = "{\"if\":{\"cond\":";
      str += this->Cond->dump();
      str += ",\"then\":";
      str += this->Then->dump();
      str += ",\"else\":";
      str += this->Else->dump();
      str += "}}";
      return str;
    }
  };

  class ForExprAST : public ExprAST
  {
    std::string VarName;
    std::unique_ptr<ExprAST> Start, End, Step, Body;

  public:
    ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Start,
               std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step,
               std::unique_ptr<ExprAST> Body)
        : VarName(VarName), Start(std::move(Start)), End(std::move(End)),
          Step(std::move(Step)), Body(std::move(Body)) {}

    const std::string dump() const override
    {
      std::string str = "{\"for\":{\"var\":\"";
      str += this->VarName;
      str += "\",\"start\":";
      str += this->Start->dump();
      str += ",\"end:\":";
      str += this->End->dump();
      if (this->Step)
      {
        str += ",\"step:\":";
        str += this->Step->dump();
      }
      str += ",\"body:\":";
      str += this->Body->dump();
      str += "}}";
      return str;
    }

    Value *codegen() override;
  };

  class UnaryExprAST : public ExprAST
  {
    char Opcode;
    std::unique_ptr<ExprAST> Operand;

  public:
    UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
        : Opcode(Opcode), Operand(std::move(Operand)) {}

    const std::string dump() const override
    {
      std::string str = "{\"unary\":{\"opcode\":\"";
      str += Opcode;
      str += "\",\"operand\":";
      str += Operand->dump();
      str += "}}";
      return str;
    }

    Value *codegen() override;
  };

  class VarExprAST : public ExprAST
  {
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
    std::unique_ptr<ExprAST> Body;

  public:
    VarExprAST(std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames,
               std::unique_ptr<ExprAST> Body)
        : VarNames(std::move(VarNames)), Body(std::move(Body)) {}

    VarExprAST(std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames)
        : VarNames(std::move(VarNames)) {}

    const std::string dump() const override
    {
      std::string str = "{\"var\":{\"names\":[";
      for (const auto &VarName : this->VarNames)
      {
        str += "{\"name\":\"" + VarName.first + "\"";
        if (VarName.second)
          str += ",\"value\":" + VarName.second->dump();
        str += "},";
      }
      if (this->VarNames.size() > 0)
      {
        str = str.substr(0, str.size() - 1);
      }
      str += "]";
      if (Body) {
        str += ",\"body\":";
        str += Body->dump();
      }
      str += "}}";
      return str;
    }

    Value *codegen() override;
  };

  class ScopeExprAST : public ExprAST
  {
    std::vector<std::unique_ptr<ExprAST>> Body;

  public:
    ScopeExprAST(std::vector<std::unique_ptr<ExprAST>> Body)
        : Body(std::move(Body)) {}

    const std::string dump() const override
    {
      std::string str = "{\"scope\":[";
      for (const auto &e : Body)
      {
        str += e->dump();
        str += ",";
      }
      if (Body.size() > 0)
      {
        str = str.substr(0, str.size() - 1);
      }
      str += "]}";
      return str;
    }

    Value *codegen() override;
  };

  static std::stack<std::map<std::string, AllocaInst *>> NamedValuesFrame;
  static std::map<std::string, AllocaInst *> NamedValues;
  static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
  std::unique_ptr<ExprAST> LogError(const char *Str);
  static std::unique_ptr<ExprAST> ParseExpression();
  static std::unique_ptr<ExprAST> ParseUnary();
  std::unique_ptr<PrototypeAST> LogErrorP(const char *Str);
  static std::unique_ptr<ExprAST> ParsePrimary();
  static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                                std::unique_ptr<ExprAST> LHS);
  static std::unique_ptr<ExprAST> ParseParenExpr();
  static std::unique_ptr<ExprAST> ParseScopeExpr();
  static std::unique_ptr<ExprAST> ParseNumberExpr();
  void InitializeModuleAndPassManager(void);

  void StoreNamedValues(bool copy = true) {
    NamedValuesFrame.push(std::move(NamedValues));
    if (copy)
      NamedValues = std::move(std::map<std::string, AllocaInst *>(NamedValuesFrame.top()));
    else
      NamedValues = std::move(std::map<std::string, AllocaInst *>());
  }

  void RestoreNamedValues()
  {
    NamedValues = std::move(std::map<std::string, AllocaInst *>(NamedValuesFrame.top()));
    NamedValuesFrame.pop();
  }

  static AllocaInst *CreateEntryBlockAlloca(Function *TheFunction,
                                            const std::string &VarName)
  {
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    return TmpB.CreateAlloca(Type::getDoubleTy(*TheContext), 0,
                             VarName.c_str());
  }

  Value *VarExprAST::codegen()
  {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    // Register all variables and emit their initializer.
    Value *LastInitVal;
    for (unsigned i = 0, e = VarNames.size(); i != e; ++i)
    {
      const std::string &VarName = VarNames[i].first;
      ExprAST *Init = VarNames[i].second.get();
      Value *InitVal;
      if (Init)
      {
        InitVal = Init->codegen();
        if (!InitVal)
          return nullptr;
      }
      else
      { // If not specified, use 0.0.
        InitVal = ConstantFP::get(*TheContext, APFloat(0.0));
      }

      AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);
      Builder->CreateStore(InitVal, Alloca);

      LastInitVal = InitVal;

      // Remember this binding.
      NamedValues[VarName] = std::move(Alloca);
    }

    if (Body) {
      Value *BodyVal = Body->codegen();
      if (!BodyVal)
        return nullptr;

      // Return the body computation.
      return BodyVal;
    } else {
      return LastInitVal;
    }
  }

  Value *VariableExprAST::codegen()
  {
    Value *V = NamedValues[Name];
    if (!V)
    {
      using namespace std::string_literals;
      return LogErrorV(("Unknown variable name "s + Name).c_str());
    }
    return Builder->CreateLoad(V, Name.c_str());
  }

  Value *NumberExprAST::codegen()
  {
    return ConstantFP::get(*TheContext, APFloat(Val));
  }

  Value *BinaryExprAST::codegen()
  {
    if (Op == '=')
    {
      // Assignment requires the LHS to be an identifier.
      VariableExprAST *LHSE = dynamic_cast<VariableExprAST *>(LHS.get());
      if (!LHSE)
        return LogErrorV("destination of '=' must be a variable");

      Value *Val = RHS->codegen();
      if (!Val)
        return nullptr;

      // Look up the name.
      Value *Variable = NamedValues[LHSE->getName()];
      if (!Variable) {
        using namespace std::string_literals;
        return LogErrorV(("Unknown variable name "s + LHSE->getName()).c_str());
      }

      Builder->CreateStore(Val, Variable);
      return Val;
    }

    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    if (!L || !R)
      return nullptr;

    switch (Op)
    {
    case '+':
      return Builder->CreateFAdd(L, R, "addtmp");
    case '-':
      return Builder->CreateFSub(L, R, "subtmp");
    case '*':
      return Builder->CreateFMul(L, R, "multmp");
    case '/':
      return Builder->CreateFDiv(L, R, "divtmp");
    case '<':
      L = Builder->CreateFCmpULT(L, R, "cmptmp");
      // Convert bool 0/1 to double 0.0 or 1.0
      return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext),
                                   "booltmp");
    default:
      break;
    }

    Function *F = getFunction(std::string("binary") + Op);
    assert(F && "binary operator not found!");

    Value *Ops[2] = {L, R};
    return Builder->CreateCall(F, Ops, "binop");
  }

  Value *CallExprAST::codegen()
  {
    // Look up the name in the global module table.
    Function *CalleeF = getFunction(Callee);
    if (!CalleeF) {
      using namespace std::string_literals;
      return LogErrorV(("Unknown function referenced "s + Callee).c_str());
    }

    // If argument mismatch error.
    if (CalleeF->arg_size() != Args.size())
      return LogErrorV("Incorrect # arguments passed");

    std::vector<Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; ++i)
    {
      ArgsV.push_back(Args[i]->codegen());
      if (!ArgsV.back())
        return nullptr;
    }

    return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
  }

  Function *PrototypeAST::codegen()
  {
    // Make the function type:  double(double,double) etc.
    std::vector<Type *> Doubles(Args.size(),
                                Type::getDoubleTy(*TheContext));
    FunctionType *FT =
        FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);

    Function *F =
        Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

    unsigned Idx = 0;
    for (auto &Arg : F->args())
      Arg.setName(Args[Idx++]);

    return F;
  }

  Function *FunctionAST::codegen()
  {
    auto &P = *Proto;
    FunctionProtos[P.getName()] = std::move(Proto);
    Function *TheFunction = getFunction(P.getName());

    if (!TheFunction)
      return nullptr;

    if (P.isBinaryOp())
      BinopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();

    BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
    Builder->SetInsertPoint(BB);

    StoreNamedValues(false);
    for (auto &Arg : TheFunction->args())
    {
      AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, std::string(Arg.getName()));
      Builder->CreateStore(&Arg, Alloca);
      NamedValues[std::string(Arg.getName())] = std::move(Alloca);
    }

    if (Value *RetVal = Body->codegen())
    {
      // Finish off the function.
      Builder->CreateRet(RetVal);

      // Validate the generated code, checking for consistency.
      verifyFunction(*TheFunction);

      // Optimize the function.
      TheFPM->run(*TheFunction);

      RestoreNamedValues();

      return TheFunction;
    }

    RestoreNamedValues();

    TheFunction->eraseFromParent();
    return nullptr;
  }

  Value *IfExprAST::codegen()
  {
    Value *CondV = Cond->codegen();
    if (!CondV)
      return nullptr;

    CondV = Builder->CreateFCmpONE(
        CondV, ConstantFP::get(*TheContext, APFloat(0.0)), "ifcond");

    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    BasicBlock *ThenBB =
        BasicBlock::Create(*TheContext, "then", TheFunction);
    BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    Builder->CreateCondBr(CondV, ThenBB, ElseBB);
    Builder->SetInsertPoint(ThenBB);

    Value *ThenV = Then->codegen();
    if (!ThenV)
      return nullptr;

    Builder->CreateBr(MergeBB);

    ThenBB = Builder->GetInsertBlock();
    TheFunction->getBasicBlockList().push_back(ElseBB);
    Builder->SetInsertPoint(ElseBB);

    Value *ElseV = Else->codegen();
    if (!ElseV)
      return nullptr;

    Builder->CreateBr(MergeBB);
    // codegen of 'Else' can change the current block, update ElseBB for the PHI.
    ElseBB = Builder->GetInsertBlock();
    TheFunction->getBasicBlockList().push_back(MergeBB);
    Builder->SetInsertPoint(MergeBB);
    PHINode *PN =
        Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, "iftmp");

    PN->addIncoming(ThenV, ThenBB);
    PN->addIncoming(ElseV, ElseBB);
    return PN;
  }

  Value *ForExprAST::codegen()
  {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    StoreNamedValues();

    Value *StartVal = Start->codegen();
    if (!StartVal) {
      RestoreNamedValues();
      return nullptr;
    }

    AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);
    Builder->CreateStore(StartVal, Alloca);
    NamedValues[VarName] = std::move(Alloca);

    BasicBlock *LoopBB =
        BasicBlock::Create(*TheContext, "loop", TheFunction);
    Builder->CreateBr(LoopBB);
    Builder->SetInsertPoint(LoopBB);
    if (!Body->codegen()) {
      RestoreNamedValues();
      return nullptr;
    }
    Value *StepVal = nullptr;
    if (Step)
    {
      StepVal = Step->codegen();
      if (!StepVal)
      {
        RestoreNamedValues();
        return nullptr;
      }
    }
    else
    {
      StepVal = ConstantFP::get(*TheContext, APFloat(1.0));
    }

    Value *EndCond = End->codegen();
    if (!EndCond)
    {
      RestoreNamedValues();
      return nullptr;
    }
    Value *CurVar =
        Builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
    Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
    Builder->CreateStore(NextVar, Alloca);
    EndCond = Builder->CreateFCmpONE(
        EndCond, ConstantFP::get(*TheContext, APFloat(0.0)), "loopcond");
    BasicBlock *AfterBB =
        BasicBlock::Create(*TheContext, "afterloop", TheFunction);
    Builder->CreateCondBr(EndCond, LoopBB, AfterBB);
    Builder->SetInsertPoint(AfterBB);
    RestoreNamedValues();
    return Constant::getNullValue(Type::getDoubleTy(*TheContext));
  }

  Value *UnaryExprAST::codegen()
  {
    Value *OperandV = Operand->codegen();
    if (!OperandV)
      return nullptr;

    Function *F = getFunction(std::string("unary") + Opcode);
    if (!F) {
      using namespace std::string_literals;
      return LogErrorV(("Unknown unary operator "s + Opcode).c_str());
    }

    return Builder->CreateCall(F, OperandV, "unop");
  }

  Value *ScopeExprAST::codegen()
  {
    StoreNamedValues();
    Value *last;
    bool anyEmpty = false;
    std::for_each(Body.begin(), Body.end(), [&last,&anyEmpty](const auto &e) {
      Value *V = e->codegen();
      if (!V) {
        anyEmpty = true;
      }
      last = V;
    });
    RestoreNamedValues();
    if (anyEmpty) {
      return nullptr;
    }
    return last;
  }

  static std::unique_ptr<ExprAST> ParseIfExpr()
  {
    getNextToken();

    // condition.
    auto Cond = ParseExpression();
    if (!Cond)
      return nullptr;

    if (CurTok != tok_then)
      return LogError("expected then");
    getNextToken(); // eat the then

    auto Then = ParseExpression();
    if (!Then)
      return nullptr;

    if (CurTok != tok_else)
      return LogError("expected else");

    getNextToken();

    auto Else = ParseExpression();
    if (!Else)
      return nullptr;

    return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                       std::move(Else));
  }

  Function *getFunction(const std::string &Name)
  {
    // First, see if the function has already been added to the current module.
    if (auto *F = TheModule->getFunction(Name))
      return F;

    // If not, check whether we can codegen the declaration from some existing
    // prototype.
    auto FI = FunctionProtos.find(Name);
    if (FI != FunctionProtos.end())
      return FI->second->codegen();

    // If no existing prototype exists, return null.
    return nullptr;
  }

  static int getNextToken()
  {
    return CurTok = gettok();
  }

  std::unique_ptr<ExprAST> LogError(const char *Str)
  {
    fprintf(stderr, "LogError: %s\n", Str);
    return nullptr;
  }

  std::unique_ptr<PrototypeAST> LogErrorP(const char *Str)
  {
    LogError(Str);
    return nullptr;
  }

  Value *LogErrorV(const char *Str)
  {
    LogError(Str);
    return nullptr;
  }

  static int GetTokPrecedence()
  {
    if (!isascii(CurTok))
      return -1;

    // Make sure it's a declared binop.
    int TokPrec = BinopPrecedence[CurTok];
    if (TokPrec <= 0)
      return -1;
    return TokPrec;
  }

  static std::unique_ptr<ExprAST> ParseExpression()
  {
    auto LHS = ParseUnary();
    if (!LHS)
      return nullptr;

    return ParseBinOpRHS(0, std::move(LHS));
  }

  static std::unique_ptr<ExprAST> ParseIdentifierExpr()
  {
    std::string IdName = IdentifierStr;

    getNextToken(); // eat identifier.

    if (CurTok != '(') // Simple variable ref.
      return std::make_unique<VariableExprAST>(IdName);

    // Call.
    getNextToken(); // eat (
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != ')')
    {
      while (1)
      {
        if (auto Arg = ParseExpression())
          Args.push_back(std::move(Arg));
        else
          return nullptr;

        if (CurTok == ')')
          break;

        if (CurTok != ',')
          return LogError("Expected ')' or ',' in argument list");
        getNextToken();
      }
    }

    // Eat the ')'.
    getNextToken();

    return std::make_unique<CallExprAST>(IdName, std::move(Args));
  }

  static std::unique_ptr<ExprAST> ParseForExpr()
  {
    getNextToken(); // eat the for.

    if (CurTok != tok_identifier)
      return LogError("expected identifier after for");

    std::string IdName = IdentifierStr;
    getNextToken(); // eat identifier.

    if (CurTok != '=')
      return LogError("expected '=' after for");
    getNextToken(); // eat '='.

    auto Start = ParseExpression();
    if (!Start)
      return nullptr;
    if (CurTok != ',')
      return LogError("expected ',' after for start value");
    getNextToken();

    auto End = ParseExpression();
    if (!End)
      return nullptr;

    // The step value is optional.
    std::unique_ptr<ExprAST> Step;
    if (CurTok == ',')
    {
      getNextToken();
      Step = ParseExpression();
      if (!Step)
        return nullptr;
    }

    if (CurTok != tok_in)
      return LogError("expected 'in' after for");
    getNextToken(); // eat 'in'.

    auto Body = ParseExpression();
    if (!Body)
      return nullptr;

    return std::make_unique<ForExprAST>(IdName, std::move(Start),
                                        std::move(End), std::move(Step),
                                        std::move(Body));
  }

  static std::unique_ptr<ExprAST> ParseVarExpr()
  {
    getNextToken();

    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

    // At least one variable name is required.
    if (CurTok != tok_identifier)
      return LogError("expected identifier after var");

    while (1)
    {

      std::string Name = IdentifierStr;
      getNextToken();

      // Read the optional initializer.
      std::unique_ptr<ExprAST> Init;
      if (CurTok == '=')
      {
        getNextToken(); // eat the '='.

        Init = ParseExpression();
        if (!Init)
          return nullptr;
      }

      VarNames.push_back(std::make_pair(Name, std::move(Init)));

      // End of var list, exit loop.
      if (CurTok != ',')
        break;
      getNextToken(); // eat the ','.

      if (CurTok != tok_identifier)
        return LogError("expected identifier list after topping");
    }
    if (CurTok == tok_in)
    {
      getNextToken(); // eat 'in'.

      auto Body = ParseExpression();
      return std::make_unique<VarExprAST>(std::move(VarNames),
                                          std::move(Body));
    } else {
      return std::make_unique<VarExprAST>(std::move(VarNames));
    }
  }

  static std::unique_ptr<ExprAST> ParsePrimary()
  {
    switch (CurTok)
    {
    default:
      return LogError("unknown token when expecting an expression");
    case tok_identifier:
      return ParseIdentifierExpr();
    case tok_number:
      return ParseNumberExpr();
    case '(':
      return ParseParenExpr();
    case '{':
      return ParseScopeExpr();
    case tok_if:
      return ParseIfExpr();
    case tok_for:
      return ParseForExpr();
    case tok_topping:
      return ParseVarExpr();
    }
  }

  static std::unique_ptr<ExprAST> ParseUnary()
  {
    // If the current token is not an operator, it must be a primary expr.
    if (!isascii(CurTok) || CurTok == '(' || CurTok == ',' || CurTok == '{')
      return ParsePrimary();

    // If this is a unary operator, read it.
    int Opc = CurTok;
    getNextToken();
    if (auto Operand = ParseUnary())
      return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));
    return nullptr;
  }

  static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                                std::unique_ptr<ExprAST> LHS)
  {
    while (1)
    {
      int TokPrec = GetTokPrecedence();
      if (TokPrec < ExprPrec)
        return LHS;

      int BinOp = CurTok;
      getNextToken();

      auto RHS = ParseUnary();
      if (!RHS)
        return nullptr;

      int NextPrec = GetTokPrecedence();
      if (TokPrec < NextPrec)
      {
        RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
        if (!RHS)
          return nullptr;
      }
      LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS),
                                            std::move(RHS));
    }
  }

  static std::unique_ptr<ExprAST> ParseNumberExpr()
  {
    auto Result = std::make_unique<NumberExprAST>(NumVal);
    getNextToken();
    return std::move(Result);
  }

  static std::unique_ptr<ExprAST> ParseParenExpr()
  {
    getNextToken();
    auto V = ParseExpression();
    if (!V)
      return nullptr;

    if (CurTok != ')')
      return LogError("expected ')'");
    getNextToken();
    return V;
  }

  static std::unique_ptr<ExprAST> ParseScopeExpr()
  {
    std::vector<std::unique_ptr<ExprAST>> v;
    getNextToken();
    while (CurTok != '}')
    {
      auto V = ParseExpression();
      if (!V)
        return nullptr;
      v.push_back(std::move(V));
      getNextToken();
    };
    getNextToken();

    return std::make_unique<ScopeExprAST>(std::move(v));
  }

  static std::unique_ptr<PrototypeAST> ParsePrototype()
  {
    std::string FnName;

    unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
    unsigned BinaryPrecedence = 30;

    switch (CurTok)
    {
    default:
      return LogErrorP("Expected function name in prototype");
    case tok_identifier:
      FnName = IdentifierStr;
      Kind = 0;
      getNextToken();
      break;
    case tok_unary:
      getNextToken();
      if (!isascii(CurTok))
        return LogErrorP("Expected unary operator");
      FnName = "unary";
      FnName += (char)CurTok;
      Kind = 1;
      getNextToken();
      break;
    case tok_binary:
      getNextToken();
      if (!isascii(CurTok))
        return LogErrorP("Expected binary operator");
      FnName = "binary";
      FnName += (char)CurTok;
      Kind = 2;
      getNextToken();

      // Read the precedence if present.
      if (CurTok == tok_number)
      {
        if (NumVal < 1 || NumVal > 100)
          return LogErrorP("Invalid precedence: must be 1..100");
        BinaryPrecedence = (unsigned)NumVal;
        getNextToken();
      }
      break;
    }

    if (CurTok != '(')
      return LogErrorP("Expected '(' in prototype");

    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier)
      ArgNames.push_back(IdentifierStr);
    if (CurTok != ')')
      return LogErrorP("Expected ')' in prototype");

    // success.
    getNextToken(); // eat ')'.

    // Verify right number of names for operator.
    if (Kind && ArgNames.size() != Kind)
      return LogErrorP("Invalid number of operands for operator");

    return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames), Kind != 0,
                                          BinaryPrecedence);
  }

  static std::unique_ptr<FunctionAST> ParseDefinition()
  {
    getNextToken();

    auto Proto = ParsePrototype();
    if (!Proto)
      return nullptr;

    auto E = ParseExpression();
    if (!E)
      return nullptr;

    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }

  static std::unique_ptr<FunctionAST> ParseTopLevelExpr()
  {
    if (auto E = ParseExpression())
    {
      auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
      return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
  }

  static std::unique_ptr<PrototypeAST> ParseExtern()
  {
    getNextToken();
    return ParsePrototype();
  }

  static void HandleTopLevelExpression()
  {
    if (auto FnAST = ParseTopLevelExpr())
    {
      if (jsonFile.is_open())
        jsonFile << "," << FnAST->dump() << std::endl;
      if (auto *FnIR = FnAST->codegen())
      {
        if (llFile)
          FnIR->print(*llFile);

        auto H = TheJIT->addModule(std::move(TheModule));
        InitializeModuleAndPassManager();
        auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
        assert(ExprSymbol && "Function not found");
        double (*FP)() = (double (*)())(intptr_t)cantFail(ExprSymbol.getAddress());
        if (replMode)
          fprintf(stderr, "Evaluated to %f\n", FP());
        else
          FP();
        TheJIT->removeModule(H);
      }
    }
    else
    {
      // Skip token for error recovery.
      getNextToken();
    }
  }

  static void HandleDefinition()
  {
    if (auto FnAST = ParseDefinition())
    {
      if (jsonFile.is_open())
        jsonFile << "," << FnAST->dump() << std::endl;

      if (auto *FnIR = FnAST->codegen())
      {
        if (llFile)
          FnIR->print(*llFile);

        if (replMode)
          fprintf(stderr, "New base '%s' available\n", FnAST->getName().c_str());
        TheJIT->addModule(std::move(TheModule));
        InitializeModuleAndPassManager();
      }
    }
    else
    {
      getNextToken();
    }
  }

  static void HandleExtern()
  {
    if (auto ProtoAST = ParseExtern())
    {
      if (jsonFile.is_open())
        jsonFile << ",{\"extern\":" << ProtoAST->dump() << "}" << std::endl;

      if (auto *FnIR = ProtoAST->codegen())
      {
        if (llFile)
          FnIR->print(*llFile);

        if (replMode)
          fprintf(stderr, "New sauce '%s' available\n", ProtoAST->getName().c_str());
        FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
      }
    }
    else
    {
      // Skip token for error recovery.
      getNextToken();
    }
  }

  void InitializeModuleAndPassManager(void)
  {
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("my cool jit", *TheContext);
    TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());
    Builder = std::make_unique<IRBuilder<>>(*TheContext);
    TheFPM = std::make_unique<legacy::FunctionPassManager>(TheModule.get());
    TheFPM->add(createPromoteMemoryToRegisterPass());
    TheFPM->add(createInstructionCombiningPass());
    TheFPM->add(createReassociatePass());
    TheFPM->add(createGVNPass());
    TheFPM->add(createCFGSimplificationPass());

    TheFPM->doInitialization();
  }

  static void MainLoop()
  {
    while (replMode || CurTok != tok_eof)
    {
      switch (CurTok)
      {
      case tok_eof:
        return;
      case ';':
        if (replMode)
          fprintf(stderr, "ready> ");
        getNextToken();
        break;
      case tok_base:
        HandleDefinition();
        break;
      case tok_sauce:
        HandleExtern();
        break;
      default:
        HandleTopLevelExpression();
        break;
      }
    }
  }
}

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

extern "C" DLLEXPORT double print(double X)
{
  if (replMode)
    fprintf(stderr, "%f\n", X);
  else
    fprintf(stdout, "%f\n", X);
  return 0;
}

extern "C" DLLEXPORT double printchar(double X)
{
  if (replMode)
    fputc((char)X, stderr);
  else
    fputc((char)X, stdout);
  return 0;
}

namespace Pizza
{
  namespace AST
  {

    int Run(const struct Options &opt)
    {
      replMode = opt.repl;
      if (!replMode)
      {
        srcFile = fopen(opt.srcPath.c_str(), "r");
        if (srcFile == nullptr)
        {
          fprintf(stderr, "Could not open file %s\n", opt.srcPath.c_str());
          return 1;
        }
      }

      if (opt.jsonPath.size() > 0)
      {
        jsonFile.open(opt.jsonPath, std::ios::trunc);
        jsonFile << "{\"ast\":[\"start\"" << std::endl;
        if (jsonFile.fail())
        {
          if (!replMode)
            fclose(srcFile);

          fprintf(stderr, "Could not open file %s\n", opt.jsonPath.c_str());
          return 1;
        }
      }

      if (opt.llPath.size() > 0)
      {
        std::error_code EC;
        llFile = std::make_unique<raw_fd_ostream>(opt.llPath, EC, sys::fs::OF_None);

        if (EC)
        {
          if (!replMode)
            fclose(srcFile);
          jsonFile.close();
          errs() << "Could not open file: " << EC.message() << "\n";
          return 1;
        }
      }

      InitializeNativeTarget();
      InitializeNativeTargetAsmPrinter();
      InitializeNativeTargetAsmParser();

      BinopPrecedence['='] = 2;
      BinopPrecedence['<'] = 10;
      BinopPrecedence['+'] = 20;
      BinopPrecedence['-'] = 20;
      BinopPrecedence['*'] = 40;
      BinopPrecedence['/'] = 40;

      if (replMode)
        fprintf(stderr, "ready> ");

      getNextToken();

      TheJIT = std::make_unique<Pizza::JIT>();
      InitializeModuleAndPassManager();
      StoreNamedValues(); //avoid getting empty;
      MainLoop();

      if (opt.jsonPath.size() > 0)
      {
        jsonFile << ",\"end\"]}" << std::endl;
        jsonFile.close();
      }

      if (opt.llPath.size() > 0)
      {
        llFile->close();
      }

      fclose(srcFile);

      return 0;
    }
  }
}
