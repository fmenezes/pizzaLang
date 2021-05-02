%{
	#include <iostream>
	extern int yylex();
	void yyerror(const char *msg);
	void exiting();
%}
%define api.value.type { double }

%token NUM
%token SEMICOLON
%token EXIT

%left '-' '+'
%left '*' '/'

%%		/* the grammars here */

stmts: %empty
	| stmt stmts
	;

stmt: SEMICOLON
	| exp SEMICOLON { std::cout << " =>" << $1; }
	| EXIT SEMICOLON { exiting(); }
	; 

exp: NUM		    { $$ = $1; }
	| exp '+' exp   { $$ = $1 + $3; }
	| exp '-' exp   { $$ = $1 - $3; }
	| exp '*' exp   { $$ = $1 * $3; }
	| exp '/' exp   { $$ = $1 / $3; }
	| '(' exp ')'   { $$ = $2; }
	;

%%
