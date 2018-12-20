#ifndef __PISTIS__CONCURRENT__BLOCKINGMODE_HPP__
#define __PISTIS__CONCURRENT__BLOCKINGMODE_HPP__

#include <ostream>

namespace pistis {
  namespace concurrent {

    /** @brief Whether calls should block or not.
     *
     *  @todo  Move this to a more general location, since it is useful
     *         for files, sockets, etc.
     */
    enum class BlockingMode {
      BLOCK= 0,
      DONT_BLOCK = 1
    };

    std::ostream& operator<<(std::ostream& out, BlockingMode mode);

  }
}
#endif
