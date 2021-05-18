#include <string>

#include "pizza/ast.h"

const std::string ScopeExprAST::dump() const
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

const std::string VarExprAST::dump() const
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
  if (Body)
  {
    str += ",\"body\":";
    str += Body->dump();
  }
  str += "}}";
  return str;
}

const std::string UnaryExprAST::dump() const
{
  std::string str = "{\"unary\":{\"opcode\":\"";
  str += Opcode;
  str += "\",\"operand\":";
  str += Operand->dump();
  str += "}}";
  return str;
}

const std::string ForExprAST::dump() const
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

const std::string IfExprAST::dump() const
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

const std::string FunctionAST::dump()
{
  std::string str = "{\"function\":{\"proto\":";
  str += this->Proto->dump();
  str += ",\"body\":";
  str += this->Body->dump();
  str += "}}";
  return str;
}

const std::string PrototypeAST::dump()
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

const std::string CallExprAST::dump() const
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

const std::string BinaryExprAST::dump() const
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

const std::string VariableExprAST::dump() const
{
  std::string str = "{\"var\":\"";
  str += this->Name;
  str += "\"}";

  return str;
}

const std::string NumberExprAST::dump() const
{
  return "{\"num\":" + std::to_string(Val) + "}";
}
