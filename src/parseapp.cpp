#include <iostream>

#include "parser.hpp"

int main(int argc, char* argv[])
{
    yyparse();

    return 0;
}

void exiting()
{
    exit(0);
}

void yyerror(const char *msg)
{
    std::cout << "Error: " << msg << std::endl;
}
