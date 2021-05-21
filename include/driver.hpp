#pragma once

#include <string>
#include <map>
#include "parser.hpp"

#define YY_DECL \
  yy::parser::symbol_type yylex(driver &drv)
YY_DECL;

class driver
{
private:
  double result;

public:
  driver();

  std::string file;
  bool trace_parsing;
  bool trace_scanning;
  yy::location location;

  int parse(const std::string &f);

  const double &getResult();
  void setResult(const double &d);

  void scan_begin();
  void scan_end();
};
