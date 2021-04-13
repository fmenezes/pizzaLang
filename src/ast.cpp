#include <iostream>
#include <vector>
#include <string>
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

#include "ast.h"

using namespace llvm;

enum Token
{
  tok_eof = -1,
  tok_base = -2,
  tok_topping = -3,
  tok_identifier = -4,
  tok_number = -5
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

static LLVMContext TheContext;
static IRBuilder<> Builder(TheContext);
static std::unique_ptr<Module> TheModule = std::make_unique<Module>("my cool jit", TheContext);
static std::map<std::string, Value *> NamedValues;

namespace
{
  Value *LogErrorV(const char *Str);
  static int CurTok;
  static int getNextToken();
  static std::map<char, int> BinopPrecedence;

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

    Value *codegen() override
    {
      return ConstantFP::get(TheContext, APFloat(Val));
    }

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

    Value *codegen() override
    {
      Value *V = NamedValues[Name];
      if (!V)
        LogErrorV("Unknown variable name");
      return V;
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

    Value *codegen() override
    {
      Value *L = LHS->codegen();
      Value *R = RHS->codegen();
      if (!L || !R)
        return nullptr;

      switch (Op)
      {
      case '+':
        return Builder.CreateFAdd(L, R, "addtmp");
      case '-':
        return Builder.CreateFSub(L, R, "subtmp");
      case '*':
        return Builder.CreateFMul(L, R, "multmp");
      case '/':
        return Builder.CreateFDiv(L, R, "divtmp");
      case '<':
        L = Builder.CreateFCmpULT(L, R, "cmptmp");
        // Convert bool 0/1 to double 0.0 or 1.0
        return Builder.CreateUIToFP(L, Type::getDoubleTy(TheContext),
                                    "booltmp");
      default:
        return LogErrorV("invalid binary operator");
      }
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

    Value *codegen() override
    {
      // Look up the name in the global module table.
      Function *CalleeF = TheModule->getFunction(Callee);
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

      return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
    }
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

    Function *codegen()
    {
      // Make the function type:  double(double,double) etc.
      std::vector<Type *> Doubles(Args.size(),
                                  Type::getDoubleTy(TheContext));
      FunctionType *FT =
          FunctionType::get(Type::getDoubleTy(TheContext), Doubles, false);

      Function *F =
          Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

      unsigned Idx = 0;
      for (auto &Arg : F->args())
        Arg.setName(Args[Idx++]);

      return F;
    }
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

    Function *codegen()
    {
      Function *TheFunction = TheModule->getFunction(Proto->getName());

      if (!TheFunction)
        TheFunction = Proto->codegen();

      if (!TheFunction)
        return nullptr;

      if (!TheFunction->empty())
        return (Function *)LogErrorV("Function cannot be redefined.");

      BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
      Builder.SetInsertPoint(BB);

      NamedValues.clear();
      for (auto &Arg : TheFunction->args())
        NamedValues[std::string(Arg.getName())] = &Arg;

      if (Value *RetVal = Body->codegen())
      {
        // Finish off the function.
        Builder.CreateRet(RetVal);

        // Validate the generated code, checking for consistency.
        verifyFunction(*TheFunction);

        return TheFunction;
      }

      TheFunction->eraseFromParent();
      return nullptr;
    }
  };

  static std::unique_ptr<ExprAST> ParseExpression();
  std::unique_ptr<ExprAST> LogError(const char *Str);
  std::unique_ptr<PrototypeAST> LogErrorP(const char *Str);
  static std::unique_ptr<ExprAST> ParsePrimary();
  static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                                std::unique_ptr<ExprAST> LHS);
  static std::unique_ptr<ExprAST> ParseParenExpr();
  static std::unique_ptr<ExprAST> ParseNumberExpr();

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
      auto Proto = std::make_unique<PrototypeAST>("", std::vector<std::string>());
      return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
  }

  static void HandleTopLevelExpression()
  {
    if (auto FnAST = ParseTopLevelExpr())
    {
      if (auto *FnIR = FnAST->codegen())
      {
        fprintf(stderr, "Read top-level expression:\n");
        FnIR->print(errs());
        fprintf(stderr, "\n");

        // Remove the anonymous expression.
        FnIR->eraseFromParent();
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
        fprintf(stderr, "Read function definition:\n");
        FnIR->print(errs());
        fprintf(stderr, "\n");
      }
    }
    else
    {
      getNextToken();
    }
  }

  static void InitializeModule()
  {
    TheModule = std::make_unique<Module>("my cool jit", TheContext);
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
      default:
        HandleTopLevelExpression();
        break;
      }
    }
  }
}

namespace AST
{
  void Run(const std::string &filePath)
  {
    BinopPrecedence['<'] = 10;
    BinopPrecedence['+'] = 20;
    BinopPrecedence['-'] = 20;
    BinopPrecedence['*'] = 40;

    file = fopen(filePath.c_str(), "r");

    getNextToken();

    MainLoop();

    fclose(file);
  }
}
