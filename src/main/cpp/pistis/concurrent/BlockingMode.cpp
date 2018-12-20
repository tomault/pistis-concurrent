#include "BlockingMode.hpp"

using namespace pistis::concurrent;

std::ostream& operator<<(std::ostream& out, BlockingMode mode) {
  if (mode == BlockingMode::BLOCK) {
    return out << "BLOCK";
  } else if (mode == BlockingMode::DONT_BLOCK) {
    return out << "DONT_BLOCK";
  } else {
    return out << "**UNKNOWN**";
  }
}
