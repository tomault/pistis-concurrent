#include "Semaphore.hpp"
#include <pistis/concurrent/EpollSet.hpp>
#include <pistis/exceptions/SystemError.hpp>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace pistis::exceptions;
using namespace pistis::concurrent;
using namespace pistis::concurrent::pollable;

namespace {
  inline int computeFlags(OnExecMode onExec) {
    return EFD_SEMAPHORE |
           (onExec == OnExecMode::CLOSE ? EFD_CLOEXEC : 0);
  }
  
  inline int createEventFd(uint64_t initialValue, OnExecMode onExec) {
    int fd = ::eventfd(initialValue, computeFlags(onExec));
    if (fd < 0) {
      throw SystemError::fromSystemCode("Failed to create event fd: #ERR#",
					errno, PISTIS_EX_HERE);
    }
    return fd;
  }
}

Semaphore::Semaphore(uint64_t initialValue, OnExecMode onExec):
    fd_(createEventFd(initialValue, onExec)) {
}

Semaphore::Semaphore(Semaphore&& other): fd_(other.fd_) {
  other.fd_ = -1;
}

Semaphore::~Semaphore() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool Semaphore::up(uint64_t v, int64_t timeout) {
  if (timeout < 0) {
    up(v);
    return true;
  } else {
    EpollSet pollSet(fd_, EpollEventType::WRITE);
    return pollSet.whenReady(timeout,
			     [this, v](const EpollEventList&) {
			       write_(v);
			       return true;
			     },
			     []() { return false; });
  }
}

bool Semaphore::down(int64_t timeout) {
  if (timeout < 0) {
    down();
    return true;
  } else {
    EpollSet pollSet(fd_, EpollEventType::READ);
    return pollSet.whenReady(timeout,
			     [this](const EpollEventList&) {
			       read_();
			       return true;
			     },
			     []() { return false; });
  }
}

Semaphore& Semaphore::operator=(Semaphore&& other) {
  if (fd_ != other.fd_) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

bool Semaphore::read_() {
  uint64_t unused;
  int rc = ::read(fd_, &unused, 8);
  if (rc >= 0) {
    return true;
  } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
    return false;
  } else {
    throw SystemError::fromSystemCode("Read from eventfd failed: #ERR#", errno,
				      PISTIS_EX_HERE);
  }
}

bool Semaphore::write_(uint64_t v) {
  int rc = ::write(fd_, &v, 8);
  if (rc >= 0) {
    return true;
  } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
    return false;
  } else {
    throw SystemError::fromSystemCode("Write to eventfd failed: #ERR#", errno,
				      PISTIS_EX_HERE);
  }
}
