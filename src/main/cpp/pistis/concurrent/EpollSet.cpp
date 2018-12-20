#include "EpollSet.hpp"
#include <pistis/exceptions/ItemExistsError.hpp>
#include <pistis/exceptions/NoSuchItem.hpp>
#include <pistis/exceptions/SystemError.hpp>
#include <sys/epoll.h>
#include <unistd.h>

using namespace pistis::concurrent;
using namespace pistis::exceptions;

namespace {
  static const uint32_t EPOLL_TRIGGER_FLAGS[] = { 0, EPOLLET };
  static const uint32_t EPOLL_REPEAT_FLAGS[] = { 0, EPOLLONESHOT };

  static const std::vector< std::pair<uint32_t, EpollEventType> >
      EVENT_FLAG_MAP = {
    { EPOLLIN, EpollEventType::READ }, { EPOLLOUT, EpollEventType::WRITE },
    { EPOLLRDHUP, EpollEventType::READ_HANGUP },
    { EPOLLHUP, EpollEventType::HANGUP },
    { EPOLLPRI, EpollEventType::PRIORITY },
    { EPOLLERR, EpollEventType::ERROR }
  };
  
  static int createEpollFd(OnExecMode onExec) {
    const int flags = onExec == OnExecMode::CLOSE ? EPOLL_CLOEXEC : 0;
    int fd = ::epoll_create1(flags);
    if (fd < 0) {
      throw SystemError::fromSystemCode("Call to epoll_create1_failed: #ERR#",
					errno, PISTIS_EX_HERE);
    }
    return fd;
  }

  
  static uint32_t epollFlags(EpollEventType t) {
    int flags = 0;
    for (const auto& i : EVENT_FLAG_MAP) {
      if ((uint32_t)i.second & (uint32_t)t) {
	flags |= i.first;
      }
    }
    return flags;
  }

  static uint32_t epollFlags(EpollTrigger t) {
    return EPOLL_TRIGGER_FLAGS[(int)t];
  }

  static uint32_t epollFlags(EpollRepeat r) {
    return EPOLL_REPEAT_FLAGS[(int)r];
  }
  
  static struct epoll_event createEpollEvent(int fd, EpollEventType events,
					     EpollTrigger trigger,
					     EpollRepeat repeat) {
    struct epoll_event evt;
    evt.data.fd = fd;
    evt.events = epollFlags(events) | epollFlags(trigger) | epollFlags(repeat);
    return evt;
  }

  static EpollEventType translateEpollEventFlags(uint32_t flags) {
    EpollEventType events = EpollEventType::NONE;
    for (const auto& i : EVENT_FLAG_MAP) {
      if (flags & i.first) {
	events |= i.second;
      }
    }
    return events;
  }
}

EpollSet::EpollSet(OnExecMode onExec):
    onExec_(onExec), fd_(createEpollFd(onExec)), numFds_(0), events_() {
}

EpollSet::EpollSet(int fd, EpollEventType events, EpollTrigger trigger,
		   EpollRepeat repeat, OnExecMode onExec):
    onExec_(onExec), fd_(createEpollFd(onExec)), numFds_(1), events_() {
  addEvent_(fd_, fd, events, trigger, repeat);
}

EpollSet::EpollSet(EpollSet&& other):
    onExec_(other.onExec_), fd_(other.fd_), numFds_(other.numFds_),
    events_(std::move(other.events_)) {
  other.fd_ = -1;
  other.numFds_ = 0;
}

EpollSet::~EpollSet() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void EpollSet::add(int fd, EpollEventType events, EpollTrigger trigger,
		   EpollRepeat repeat) {
  addEvent_(fd_, fd, events, trigger, repeat);
  ++numFds_;
}

void EpollSet::modify(int fd, EpollEventType events, EpollTrigger trigger,
		      EpollRepeat repeat) {
  struct epoll_event info = createEpollEvent(fd, events, trigger, repeat);
  int rc = ::epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &info);
  if (rc < 0) {
    if (errno == ENOENT) {
      throw NoSuchItem("file descriptor", "epoll set", PISTIS_EX_HERE);
    } else {
      throw SystemError::fromSystemCode(
	  "Could not modify fd in epoll set: #ERR#", errno, PISTIS_EX_HERE
      );
    }
  }
}

void EpollSet::remove(int fd) {
  int rc = ::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);
  if (rc >= 0) {
    --numFds_;
  } else if (errno != ENOENT) {
    throw SystemError::fromSystemCode(
        "Could not remove fd from epoll set: #ERR", errno, PISTIS_EX_HERE
    );
  } else {
    throw NoSuchItem("file descriptor", "epoll set", PISTIS_EX_HERE);
  }
}

void EpollSet::clear() {
  if (fd_ > 0) {
    ::close(fd_);
  }
  fd_ = createEpollFd(onExec_);
}

bool EpollSet::wait(int64_t timeout, uint32_t maxEvents) {
  uint32_t numEventsToPoll = maxEvents ? maxEvents : numFds_;
  std::unique_ptr<struct epoll_event[]> eventData(
      new struct epoll_event[numEventsToPoll]
  );
  int rc = -1;
  while (rc < 0) {
    rc = ::epoll_wait(fd_, eventData.get(), numEventsToPoll, timeout);
    if ((rc < 0) && (errno != EINTR)) {
      throw SystemError::fromSystemCode("Error in epoll_wait(): #ERR#", errno,
					PISTIS_EX_HERE);
    }
  }

  events_.clear();
  for (int i = 0; i < rc; ++i) {
    struct epoll_event& evt = eventData[i];
    EpollEventType eventTypes = translateEpollEventFlags(evt.events);
    events_.push_back(EpollEvent(evt.data.fd, eventTypes));
  }
  return (bool)rc;
}

EpollSet& EpollSet::operator=(EpollSet&& other) {
  if (fd_ != other.fd_) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    onExec_ = other.onExec_;
    fd_ = other.fd_;
    other.fd_ = -1;
    numFds_ = other.numFds_;
    other.numFds_ = 0;
    events_ = std::move(other.events_);
  }
  return *this;
}

void EpollSet::addEvent_(int epollFd, int eventFd, EpollEventType events,
			 EpollTrigger trigger, EpollRepeat repeat) {
  struct epoll_event evt = createEpollEvent(eventFd, events, trigger, repeat);
  int rc = ::epoll_ctl(epollFd, EPOLL_CTL_ADD, eventFd, &evt);
  if (rc < 0) {
    if (errno == EEXIST) {
      throw ItemExistsError("file descriptor", "epoll set", PISTIS_EX_HERE);
    } else {
      throw SystemError::fromSystemCode("Cannot add fd to epoll set: #ERR#",
					errno, PISTIS_EX_HERE);
    }
  }
}

