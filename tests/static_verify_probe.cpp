// Negative fixture for scripts/verify_static_binary.sh: built without the
// SAN4_STATIC link flags, so it depends on the shared C++ runtime and must be
// rejected by the portability verifier.
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  std::cout << std::string(argc > 0 ? argv[0] : "probe") << "\n";
  return 0;
}
