#include "OnExecMode.hpp"

using namespace pistis::concurrent;

std::ostream& operator<<(std::ostream& out, OnExecMode mode) {
  if (mode == OnExecMode::KEEP) {
    return out << "KEEP";
  } else if (mode == OnExecMode::CLOSE) {
    return out << "CLOSE";
  } else {
    return out << "**UNKNOWN**";
  }
}
