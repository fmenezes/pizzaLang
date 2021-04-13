#include <string>

#include "ast.h"

int main(int argc, const char *argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "Invalid arguments\nusage: bake filePath\n");
    return 1;
  }

  AST::Run(argv[1]);

  return 0;
}
