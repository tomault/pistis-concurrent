#ifndef __PISTIS__CONCURRENT__TIMEUTILS_HPP__
#define __PISTIS__CONCURRENT__TIMEUTILS_HPP__

#include <chrono>
#include <stdint.h>

namespace pistis {
  namespace concurrent {

    template <typename Rep, typename Duration>
    inline int64_t toMs(const std::chrono::duration<Rep, Duration>& timeout) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(timeout)
	         .count();
    }
    
    inline int64_t toMs(const std::chrono::system_clock::time_point& deadline) {
      return toMs(deadline - std::chrono::system_clock::now());
    }

    inline decltype(std::chrono::milliseconds(0)) toMs(int64_t duration) {
      return std::chrono::milliseconds(duration);
    }
    
  }
}
#endif

