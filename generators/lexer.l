%{
# include <cerrno>
# include <cfloat>
# include <cstdlib>
# include <string>
# include "driver.hpp"
# include "parser.hpp"
%}

%option noyywrap nounput batch debug noinput

num   [0-9]+(\.[0-9]+)?
blank [ \t]

%{
  # define YY_USER_ACTION  loc.columns (yyleng);
%}
%%
%{
  yy::location& loc = drv.location;
  loc.step ();
%}
{blank}+   loc.step ();
[\n]+      loc.lines (yyleng); loc.step ();

"-"      return yy::parser::make_MINUS     (loc);
"+"      return yy::parser::make_PLUS      (loc);
"*"      return yy::parser::make_STAR      (loc);
"/"      return yy::parser::make_SLASH     (loc);
"("      return yy::parser::make_LPAREN    (loc);
")"      return yy::parser::make_RPAREN    (loc);
";"      return yy::parser::make_SEMICOLON (loc);

{num}      {
  errno = 0;
  double n = strtod (yytext, NULL);
  if (! (DBL_MIN <= n && n <= DBL_MAX && errno != ERANGE))
    throw yy::parser::syntax_error (loc, "double is out of range: "
                                    + std::string(yytext));
  return yy::parser::make_NUMBER (n, loc);
}
.          {
             throw yy::parser::syntax_error
               (loc, "invalid character: " + std::string(yytext));
}
<<EOF>>    return yy::parser::make_END (loc);
%%

void driver::scan_begin()
{
  yy_flex_debug = trace_scanning;
  if (file.empty() || file == "-")
    yyin = stdin;
  else if (!(yyin = fopen (file.c_str (), "r")))
  {
	std::cerr << "cannot open " << file << ": " << strerror(errno) << '\n';
	exit (EXIT_FAILURE);
  }
}

void driver::scan_end()
{
  if (!file.empty() && file != "-")
    fclose (yyin);
}
