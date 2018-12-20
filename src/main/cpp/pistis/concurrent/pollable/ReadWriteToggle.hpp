#ifndef __PISTIS__CONCURRENT__POLLABLE__READWRITETOGGLE_HPP__
#define __PISTIS__CONCURRENT__POLLABLE__READWRITETOGGLE_HPP__

#include <pistis/concurrent/OnExecMode.hpp>

namespace pistis {
  namespace concurrent {
    namespace pollable {

      /** @brief A "toggle" that provides explicit control over whether
       *         a file descriptor is readable, writable or both.
       *
       *  The purpose behind the ReadWriteToggle is to allow one thread
       *  to signal another thread waiting in select(), poll(), epoll() or
       *  the equivalent by explicitly setting the state of a file
       *  descriptor to readable, writeable or readable and writeable.
       *  The intended application is for pollable containers such as queues
       *  that indicate when the container has available items by setting
       *  the file descriptor to be readable and has available space to
       *  store items by setting the file descriptor to be writeable.
       *
       *  Applications are only permitted to monitor the toggle's file
       *  descriptor using poll(), select() or the equivalent.  Under no
       *  circumstances should an application read or write from
       *  the toggle's file descriptor.  Doing so produces undefined behavior
       *  and probably a deadlock at some point.  
       *
       *  Because of limitations in the implementation (the current
       *  implementation uses an eventfd), it is not possible to make
       *  the toggle's file descriptor neither readable nor writable.
       *  However, the current set of three states is enough for the toggle's
       *  intended application.
       *
       *  Likewise, limitations in the current implementation prevent the
       *  toggle from transitioning directly from a READ_ONLY state to a
       *  READ_WRITE state.  Specifically, the value needed to inhibit writes
       *  on an eventfd is so large that configuring the eventfd as a
       *  semaphore and decrementing it down to a value that would enable
       *  simultaneously reading and writing or only writing would not
       *  be practical.  Instead, the eventfd is configured to reset to
       *  zero on read, and then a write is done to put the file descriptor
       *  into a state where it can be read from and written to without
       *  blocking.  The impact of this limitation is that transitions from
       *  READ_ONLY to READ_WRITE will incorrectly trigger a call epoll_wait()
       *  that has been configured to respond to an edge-triggered change in
       *  the readable status of the toggle's file descriptor, even though
       *  no change in the readability status of the toggle has taken place.
       */
      class ReadWriteToggle {
      public:
	enum State {
	  /** @brief Toggle's file descriptor is readable. */
	  READ_ONLY,

	  /** @brief Toggle's file descriptor is writeable */
	  WRITE_ONLY,

	  /** @brief Toggle's file descriptor is readable and writable */
	  READ_WRITE
	};
	
      public:
	ReadWriteToggle(OnExecMode onExec = OnExecMode::CLOSE);
	ReadWriteToggle(const ReadWriteToggle&) = delete;
	ReadWriteToggle(ReadWriteToggle&& other);
	~ReadWriteToggle();

	int fd() const { return fd_; }
	State state() const { return state_; }

	void setState(State newState) {
	  if (newState != state_) {
	    changeState_(newState);
	  }
	}

	ReadWriteToggle& operator=(const ReadWriteToggle&) = delete;
	ReadWriteToggle& operator=(ReadWriteToggle&& other);
	
      private:
	int fd_;
	State state_;

	void changeState_(State newState);

      };

    }
  }
}
#endif
