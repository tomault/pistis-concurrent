#include "ReadWriteToggle.hpp"
#include <pistis/exceptions/SystemError.hpp>
#include <sys/eventfd.h>
#include <stdint.h>
#include <unistd.h>

using namespace pistis::concurrent;
using namespace pistis::concurrent::pollable;
using namespace pistis::exceptions;

namespace {
  static const uint64_t STATE_VALUES[] = { (uint64_t)-2, 0, 1 };

  // TODO: Create common code for creating event fd's
  static int createEventFd(OnExecMode onExec) {
    const int flags = onExec == OnExecMode::CLOSE ? EFD_CLOEXEC : 0;
    const int fd = ::eventfd(STATE_VALUES[(int)ReadWriteToggle::READ_WRITE],
			     flags);
    if (fd < 0) {
      throw SystemError::fromSystemCode("Failed to create event fd: #ERR#",
					errno, PISTIS_EX_HERE);
    }
  }

  static uint64_t readValue(int fd) {
    uint64_t v;
    int rc = ::read(fd, &v, 8);
    if (rc < 0) {
      throw SystemError::fromSystemCode("Failed to read from event fd: #ERR#",
					errno, PISTIS_EX_HERE);
    } else if (rc != 8) {
      throw SystemError("Failed to read from event fd: short read",
			PISTIS_EX_HERE);
    }
    return v;
  }

  static void writeValue(int fd, const uint64_t v) {
    int rc = ::write(fd, &v, 8);
    if (rc < 0) {
      throw SystemError::fromSystemCode("Failed to write to event fd: #ERR#",
					errno, PISTIS_EX_HERE);
    } else if (rc != 8) {
      throw SystemError("Failed to write to event fd: short write",
			PISTIS_EX_HERE);
    }
  }
  
}

ReadWriteToggle::ReadWriteToggle(OnExecMode onExec):
    fd_(createEventFd(onExec)), state_(READ_WRITE) {
}

ReadWriteToggle::ReadWriteToggle(ReadWriteToggle&& other):
    fd_(other.fd_), state_(other.state_) {
  other.fd_ = -1;
}

ReadWriteToggle::~ReadWriteToggle() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

ReadWriteToggle& ReadWriteToggle::operator=(ReadWriteToggle&& other) {
  if (fd_ != other.fd_) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
    state_ = other.state_;
  }
  return *this;
}

void ReadWriteToggle::changeState_(State newState) {
  const uint64_t oldValue = STATE_VALUES[(int)state_];
  const uint64_t newValue = STATE_VALUES[(int)newState];
  if (newValue > oldValue) {
    writeValue(fd_, newValue - oldValue);
  } else if (newValue < oldValue) {
    readValue(fd_);  // Reset to zero
    if (newValue) {
      writeValue(fd_, newValue);
    }
  }
  state_ = newState;
}
