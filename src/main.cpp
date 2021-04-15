#include <string>

#include "pizza/ast.h"

int main(int argc, const char *argv[])
{
  if (argc < 2 || argc > 4)
  {
    fprintf(stderr, "Invalid arguments\nusage: bake --repl|srcPath [jsonPath] [llPath]\n");
    return 1;
  }

  std::string srcPath = argv[1];
  bool repl = srcPath == "--repl";
  std::string jsonPath;
  std::string llPath;

  if (repl)
  {
    srcPath = "";
  }

  if (argc >= 3)
  {
    jsonPath = argv[2];
  }

  if (argc >= 4)
  {
    llPath = argv[3];
  }

  struct Pizza::AST::Options opt = {repl, srcPath, jsonPath, llPath};

  return Pizza::AST::Run(opt);
}
