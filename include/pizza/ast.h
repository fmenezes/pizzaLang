#pragma once

namespace Pizza
{
  namespace AST
  {
    struct Options
    {
      bool repl;
      std::string srcPath;
      std::string jsonPath;
      std::string llPath;
    };
    int Run(const struct Options &opt);
  }
}
