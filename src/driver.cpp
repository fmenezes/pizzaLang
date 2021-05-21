#include <iostream>

#include "driver.hpp"
#include "parser.hpp"

driver::driver()
    : trace_parsing(false), trace_scanning(false)
{
}

const double &driver::getResult()
{
  return result;
}

void driver::setResult(const double &d)
{
  result = d;
  if (file.empty() || file == "-")
    std::cout << result << std::endl
              << "ready> ";
}

int driver::parse(const std::string &f)
{
  if (file.empty() || file == "-")
    std::cout << "ready> ";

  file = f;
  location.initialize(&file);
  scan_begin();
  yy::parser parser(*this);
  parser.set_debug_level(trace_parsing);
  int res = parser.parse();
  scan_end();
  return res;
}
