#ifndef __PISTIS__CONCURRENT__EPOLLEVENTTYPE_HPP__
#define __PISTIS__CONCURRENT__EPOLLEVENTTYPE_HPP__

#include <ostream>

namespace pistis {
  namespace concurrent {

    enum class EpollEventType {
      NONE = 0,
      READ = 1,
      WRITE = 2,
      READ_HANGUP = 4,
      HANGUP = 8,
      PRIORITY = 16,
      ERROR = 32
    };

    inline EpollEventType operator&(EpollEventType left, EpollEventType right) {
      return (EpollEventType)((uint32_t)left & (uint32_t)right);
    }

    inline EpollEventType& operator&=(EpollEventType& left,
				      EpollEventType right) {
      (uint32_t&)left &= (uint32_t)right;
      return left;
    }

    inline EpollEventType operator|(EpollEventType left, EpollEventType right) {
      return (EpollEventType)((uint32_t)left | (uint32_t)right);
    }

    inline EpollEventType& operator|=(EpollEventType& left,
				      EpollEventType right) {
      (uint32_t&)left |= (uint32_t)right;
      return left;
    }

    inline EpollEventType operator~(EpollEventType flags) {
      return EpollEventType(~(uint32_t)flags & 0x3F);
    }

    std::ostream& operator<<(std::ostream& out, EpollEventType events);

  }
}

#endif
