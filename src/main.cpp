#include <iostream>
#include <string>
#include <vector>

#include "runtime_cli.h"
#include "util/win32_platform.h"

int main(int argc, char** argv) {
  san4::win32_platform::init_process();
  san4::win32_platform::attach_parent_console_if_needed();
  const std::vector<std::string> utf8_args =
      san4::win32_platform::get_utf8_args(argc, argv);
  const char* fallback_prog =
      (argc > 0 && argv != nullptr && argv[0] != nullptr) ? argv[0]
                                                          : "san4-linux";
  return san4::run_runtime_cli(utf8_args, fallback_prog, {}, std::cout,
                               std::cerr, true);
}
