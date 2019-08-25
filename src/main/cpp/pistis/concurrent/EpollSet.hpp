#ifndef __PISTIS__CONCURRENT__EPOLLSET_HPP__
#define __PISTIS__CONCURRENT__EPOLLSET_HPP__

#include <pistis/concurrent/EpollEventType.hpp>
#include <pistis/concurrent/OnExecMode.hpp>
#include <vector>
#include <stdint.h>

namespace pistis {
  namespace concurrent {

    enum class EpollTrigger {
      LEVEL,
      EDGE
    };

    enum class EpollRepeat {
      REPEATING,
      ONE_SHOT
    };

    class EpollEvent {
    public:
      EpollEvent(int fd, EpollEventType events): fd_(fd), events_(events) { }

      int fd() const { return fd_; }
      EpollEventType events() const { return events_; }
      
    private:
      int fd_;
      EpollEventType events_;
    };

    typedef std::vector<EpollEvent> EpollEventList;

    class EpollSet {
    public:
      EpollSet(OnExecMode onExec = OnExecMode::CLOSE);
      EpollSet(int fd, EpollEventType events,
	       EpollTrigger trigger = EpollTrigger::LEVEL,
	       EpollRepeat repeat = EpollRepeat::REPEATING,
	       OnExecMode onExec = OnExecMode::CLOSE);
      EpollSet(const EpollSet&) = delete;
      EpollSet(EpollSet&& other);
      ~EpollSet();

      int fd() const { return fd_; }
      uint32_t numTargets() const { return numFds_; }
      const EpollEventList& events() const { return events_; }

      void add(int fd, EpollEventType events,
	       EpollTrigger trigger = EpollTrigger::LEVEL,
	       EpollRepeat repeat = EpollRepeat::REPEATING);
      void modify(int fd, EpollEventType events, EpollTrigger trigger,
		  EpollRepeat repeat);
      void remove(int fd);
      void clear();

      bool wait(int64_t timeout = -1, uint32_t maxEvents = 0);

      template <typename EventHandler>
      auto whenReady(EventHandler onTriggered, uint32_t maxEvents = 0) {
	wait(-1, maxEvents);
	return onTriggered(events_);
      }

      template <typename EventHandler, typename TimeoutHandler>
      auto whenReady(int64_t timeout, EventHandler onTriggered,
		     TimeoutHandler onTimeout, uint32_t maxEvents = 0) {
	if (wait(timeout, maxEvents)) {
	  return onTriggered(events_);
	} else {
	  return onTimeout();
	}
      }

      EpollSet& operator=(const EpollSet&) = delete;
      EpollSet& operator=(EpollSet&& other);

    private:
      OnExecMode onExec_;
      int fd_;
      uint32_t numFds_;
      EpollEventList events_;

      static void addEvent_(int epollFd, int eventFd, EpollEventType events,
			    EpollTrigger trigger, EpollRepeat repeat);
    };
    
  }
}
#endif
