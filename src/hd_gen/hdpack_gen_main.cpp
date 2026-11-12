#include <iostream>
#include <string>
#include <vector>

#include "hd_gen/hd_pack_generator.h"
#include "util/win32_platform.h"

int main(int argc, char** argv) {
  san4::win32_platform::init_process();
  const std::vector<std::string> utf8_args =
      san4::win32_platform::get_utf8_args(argc, argv);
  const char* fallback_prog =
      (argc > 0 && argv != nullptr && argv[0] != nullptr) ? argv[0]
                                                          : "san4-hdpack-gen";
  return san4::hd_gen::run_hdpack_gen_cli(utf8_args, fallback_prog, {},
                                          std::cout, std::cerr, true);
}
