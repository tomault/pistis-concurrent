#ifndef __PISTIS__CONCURRENT__POLLABLE__SEMAPHORE_HPP__
#define __PISTIS__CONCURRENT__POLLABLE__SEMAPHORE_HPP__

#include <pistis/concurrent/BlockingMode.hpp>
#include <pistis/concurrent/OnExecMode.hpp>

namespace pistis {
  namespace concurrent {
    namespace pollable {

      class Semaphore {
      public:
	Semaphore(uint64_t initialValue = 0,
		  OnExecMode onExec = OnExecMode::CLOSE);
	Semaphore(OnExecMode onExec):
	    Semaphore(0, onExec) {
	}
	Semaphore(const Semaphore&) = delete;
	Semaphore(Semaphore&& other);
	~Semaphore();

	int fd() const { return fd_; }
	
	void up(uint64_t v = 1) {
	  while (!write_(v)) {
	  }
	}
	bool up(uint64_t v, int64_t timeout);
	
	void down() {
	  while (!read_()) {
	  }
	}
	bool down(int64_t timeout);
	
	Semaphore& operator=(const Semaphore&) = delete;
	Semaphore& operator=(Semaphore&& other);

      private:
	int fd_;

	bool read_();
	bool write_(uint64_t v);
      };
      
    }
  }
}

#endif
