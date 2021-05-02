%option noyywrap

%top{
	
	#pragma warning (disable: 4005)

	#include "parser.hpp"
}

NUMB	([0-9]+)|([0-9]*\.[0-9]+)
EXIT    exit

%%

{NUMB}			{ yylval = atof(yytext); return NUM; }
";"	            { return SEMICOLON; }
[+*/()-]    	{ return yytext[0]; }
[ \t]			{ /* empty */ }
{EXIT}			{ return EXIT; }
.				/* nothing, eat up */
