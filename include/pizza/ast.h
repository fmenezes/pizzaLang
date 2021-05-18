#pragma once

#include <vector>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>


class ExprAST
{
public:
  virtual ~ExprAST() {}
  virtual const std::string dump() const = 0;
  virtual llvm::Value *codegen() = 0;
};

class NumberExprAST : public ExprAST
{
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}

  llvm::Value *codegen() override;

  const std::string dump() const override;
};

class VariableExprAST : public ExprAST
{
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}

  llvm::Value *codegen() override;

  const std::string getName() const
  {
    return Name;
  }

  const std::string dump() const override;
};

class BinaryExprAST : public ExprAST
{
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

  llvm::Value *codegen() override;

  const std::string dump() const override;
};

class CallExprAST : public ExprAST
{
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}

  const std::string dump() const override;

  llvm::Value *codegen() override;
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

  const std::string dump();

  llvm::Function *codegen();
};

class FunctionAST
{
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;
  std::string Name;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body))
  {
    Name = this->Proto->getName();
  }

  const std::string &getName()
  {
    return Name;
  }

  const std::string dump();

  llvm::Function *codegen();
};

class IfExprAST : public ExprAST
{
  std::unique_ptr<ExprAST> Cond, Then, Else;

public:
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
      : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}

  llvm::Value *codegen() override;

  const std::string dump() const override;
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

  const std::string dump() const override;

  llvm::Value *codegen() override;
};

class UnaryExprAST : public ExprAST
{
  char Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
      : Opcode(Opcode), Operand(std::move(Operand)) {}

  const std::string dump() const override;

  llvm::Value *codegen() override;
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

  const std::string dump() const override;

  llvm::Value *codegen() override;
};

class ScopeExprAST : public ExprAST
{
  std::vector<std::unique_ptr<ExprAST>> Body;

public:
  ScopeExprAST(std::vector<std::unique_ptr<ExprAST>> Body)
      : Body(std::move(Body)) {}

  const std::string dump() const override;

  llvm::Value *codegen() override;
};

namespace Pizza
{
  namespace AST
  {
    struct Options
    {
      bool repl;
      std::string srcPath;
      std::string jsonPath;
      std::string llPath;
    };
    int Run(const struct Options &opt);
  }
}
