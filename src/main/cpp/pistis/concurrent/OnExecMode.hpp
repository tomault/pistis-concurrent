#ifndef __PISTIS__CONCURRENT__ONEXECMODE_HPP__
#define __PISTIS__CONCURRENT__ONEXECMODE_HPP__

#include <iostream>

namespace pistis {
  namespace concurrent {

    /** @brief How to propagate a primitive after a call to exec()
     *
     *  @todo  Move this to a more general location, since it is useful
     *         for files, sockets, etc.
     */
    enum class OnExecMode {
      KEEP = 0,
      CLOSE = 1
    };

    std::ostream& operator<<(std::ostream& out, OnExecMode mode);
  }
}

#endif

