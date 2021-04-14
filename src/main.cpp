#include <string>

#include "pizza/ast.h"

int main(int argc, const char *argv[])
{
  if (argc < 2 || argc > 5)
  {
    fprintf(stderr, "Invalid arguments\nusage: bake srcPath [jsonPath] [llPath] [objPath]\n");
    return 1;
  }

  struct Pizza::AST::Options opt =
      {
          argv[1],
          argc >= 3 ? argv[2] : nullptr,
          argc >= 4 ? argv[3] : nullptr};

  return Pizza::AST::Run(opt);
}
