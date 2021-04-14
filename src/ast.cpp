#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <stdio.h>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>

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
};

static FILE *file;
static std::string IdentifierStr;
static double NumVal;

static int gettok(FILE *f)
{
  static int LastChar = ' ';

  while (isspace(LastChar))
    LastChar = fgetc(f);

  if (isalpha(LastChar))
  {
    IdentifierStr = LastChar;
    while (isalnum((LastChar = fgetc(f))))
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
    return tok_identifier;
  }

  if (isdigit(LastChar) || LastChar == '.')
  { // Number: [0-9.]+
    std::string NumStr;
    do
    {
      NumStr += LastChar;
      LastChar = fgetc(f);
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), nullptr);
    return tok_number;
  }

  if (LastChar == '#')
  {
    do
      LastChar = fgetc(f);
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF)
      return gettok(f);
  }

  // Check for end of file.  Don't eat the EOF.
  if (LastChar == EOF)
    return tok_eof;

  // Otherwise, just return the character as its ascii value.
  int ThisChar = LastChar;
  LastChar = fgetc(f);
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
  static std::map<std::string, Value *> NamedValues;
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
      std::string str = "{\"num\":";
      str += this->Val;
      str += "}";

      return str;
    }
  };

  class VariableExprAST : public ExprAST
  {
    std::string Name;

  public:
    VariableExprAST(const std::string &Name) : Name(Name) {}

    Value *codegen() override;

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

  public:
    PrototypeAST(const std::string &name, std::vector<std::string> Args)
        : Name(name), Args(std::move(Args)) {}

    const std::string &getName() const { return Name; }

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

  public:
    FunctionAST(std::unique_ptr<PrototypeAST> Proto,
                std::unique_ptr<ExprAST> Body)
        : Proto(std::move(Proto)), Body(std::move(Body)) {}

    const std::string dump()
    {
      std::string str = "{\"proto\":";
      str += this->Proto->dump();
      str += ",\"body\":";
      str += this->Body->dump();
      str += "}";
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

  static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
  std::unique_ptr<ExprAST> LogError(const char *Str);
  static std::unique_ptr<ExprAST> ParseExpression();
  std::unique_ptr<PrototypeAST> LogErrorP(const char *Str);
  static std::unique_ptr<ExprAST> ParsePrimary();
  static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                                std::unique_ptr<ExprAST> LHS);
  static std::unique_ptr<ExprAST> ParseParenExpr();
  static std::unique_ptr<ExprAST> ParseNumberExpr();
  void InitializeModuleAndPassManager(void);

  Value *VariableExprAST::codegen()
  {
    Value *V = NamedValues[Name];
    if (!V)
      LogErrorV("Unknown variable name");
    return V;
  }

  Value *NumberExprAST::codegen()
  {
    return ConstantFP::get(*TheContext, APFloat(Val));
  }

  Value *BinaryExprAST::codegen()
  {
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
      return LogErrorV("invalid binary operator");
    }
  }

  Value *CallExprAST::codegen()
  {
    // Look up the name in the global module table.
    Function *CalleeF = getFunction(Callee);
    if (!CalleeF)
      return LogErrorV("Unknown function referenced");

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
    const auto &Name = Proto->getName();
    FunctionProtos[Name] = std::move(Proto);
    Function *TheFunction = getFunction(Name);

    if (!TheFunction)
      return nullptr;

    if (!TheFunction->empty())
      return (Function *)LogErrorV("Function cannot be redefined.");

    BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
    Builder->SetInsertPoint(BB);

    NamedValues.clear();
    for (auto &Arg : TheFunction->args())
      NamedValues[std::string(Arg.getName())] = &Arg;

    if (Value *RetVal = Body->codegen())
    {
      // Finish off the function.
      Builder->CreateRet(RetVal);

      // Validate the generated code, checking for consistency.
      verifyFunction(*TheFunction);

      // Optimize the function.
      TheFPM->run(*TheFunction);

      return TheFunction;
    }

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
    return CurTok = gettok(file);
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
    auto LHS = ParsePrimary();
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
    case tok_if:
      return ParseIfExpr();
    }
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

      auto RHS = ParsePrimary();
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

  static std::unique_ptr<PrototypeAST> ParsePrototype()
  {
    if (CurTok != tok_identifier)
      return LogErrorP("Expected function name in prototype");

    std::string FnName = IdentifierStr;
    getNextToken();

    if (CurTok != '(')
      return LogErrorP("Expected '(' in prototype");

    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier)
      ArgNames.push_back(IdentifierStr);
    if (CurTok != ')')
      return LogErrorP("Expected ')' in prototype");

    getNextToken();

    return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
  }

  static std::unique_ptr<FunctionAST> ParseDefinition()
  {
    getNextToken();
    auto Proto = ParsePrototype();
    if (!Proto)
      return nullptr;

    if (auto E = ParseExpression())
      return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    return nullptr;
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
      if (auto *FnIR = FnAST->codegen())
      {
        auto H = TheJIT->addModule(std::move(TheModule));
        InitializeModuleAndPassManager();
        auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
        assert(ExprSymbol && "Function not found");
        double (*FP)() = (double (*)())(intptr_t)cantFail(ExprSymbol.getAddress());
        fprintf(stderr, "Evaluated to %f\n", FP());
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
      if (auto *FnIR = FnAST->codegen())
      {
        fprintf(stderr, "Read function definition:");
        FnIR->print(errs());
        fprintf(stderr, "\n");
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
      if (auto *FnIR = ProtoAST->codegen())
      {
        fprintf(stderr, "Read extern: ");
        FnIR->print(errs());
        fprintf(stderr, "\n");
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
    TheFPM->add(createInstructionCombiningPass());
    TheFPM->add(createReassociatePass());
    TheFPM->add(createGVNPass());
    TheFPM->add(createCFGSimplificationPass());
    TheFPM->doInitialization();
  }

  static void MainLoop()
  {
    while (CurTok != tok_eof)
    {
      switch (CurTok)
      {
      case ';':
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
  fprintf(stderr, "%f\n", X);
  return 0;
}

namespace Pizza
{
  namespace AST
  {

    int Run(const std::string &filePath)
    {
      file = fopen(filePath.c_str(), "r");
      if (file == nullptr)
      {
        fprintf(stderr, "Could not open file %s\n", filePath.c_str());
        return 1;
      }

      InitializeNativeTarget();
      InitializeNativeTargetAsmPrinter();
      InitializeNativeTargetAsmParser();

      BinopPrecedence['<'] = 10;
      BinopPrecedence['+'] = 20;
      BinopPrecedence['-'] = 20;
      BinopPrecedence['*'] = 40;
      BinopPrecedence['/'] = 40;

      TheJIT = std::make_unique<Pizza::JIT>();
      InitializeModuleAndPassManager();

      getNextToken();

      MainLoop();

      fclose(file);

      return 0;
    }
  }
}
