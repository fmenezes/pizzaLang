%skeleton "lalr1.cc"
%require "3.2"
%defines

%define api.token.constructor
%define api.value.type variant
%define parse.assert

%code requires {
  # include <string>
  class driver;
}

%param { driver& drv }

%locations

%define parse.trace
%define parse.error verbose

%{
	#include <iostream>
%}

%code {
# include "driver.hpp"
}

%define api.token.prefix {TOK_}

%token
  END  0   "end of file"
  MINUS    "-"
  PLUS     "+"
  STAR     "*"
  SLASH    "/"
  LPAREN   "("
  RPAREN   ")"
;

%token <int> NUMBER "number"
%token SEMICOLON
%type  <int> exp

%printer { yyoutput << $$; } <*>;

%%
%start program;

program:
  stmts;

%left "+" "-";
%left "*" "/";

stmts:
  %empty        { }
| stmt stmts    { };

stmt:
  exp SEMICOLON { drv.result = $1; std::cout << $1 << std::endl; };

exp:
  exp "+" exp   { $$ = $1 + $3; }
| exp "-" exp   { $$ = $1 - $3; }
| exp "*" exp   { $$ = $1 * $3; }
| exp "/" exp   { $$ = $1 / $3; }
| "(" exp ")"   { $$ = $2; }
| "number"      { $$ = $1; };
%%

void yy::parser::error (const location_type& l, const std::string& m)
{
  std::cerr << l << ": " << m << '\n';
}