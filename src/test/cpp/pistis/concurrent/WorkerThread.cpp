#include "WorkerThread.hpp"
#include <chrono>

using namespace pistis::concurrent;

namespace pistis {
  namespace concurrent {
    std::ostream& operator<<(std::ostream& out, ThreadState s) {
      switch (s) {
        case ThreadState::NOT_STARTED: return out << "NOT_STARTED";
        case ThreadState::STARTED: return out << "STARTED";
        case ThreadState::WAITING: return out << "WAITING";
        case ThreadState::DONE: return out << "DONE";
        default: return out << "**UNKNOWN**";
      }
    }
  }
}

WorkerThread::WorkerThread():
    thread_(), state_(ThreadState::NOT_STARTED), errors_() {
}

WorkerThread::~WorkerThread() {
  if (thread_.joinable()) {
    thread_.detach();
  }
}

::testing::AssertionResult WorkerThread::waitForState(ThreadState desired,
						      int64_t timeout) {
  static const auto SPIN_DELAY = std::chrono::milliseconds(10);
  
  auto now = std::chrono::system_clock::now();
  auto deadline = now + std::chrono::milliseconds(timeout);
      
  while ((state() != desired) && (now < deadline)) {
    std::this_thread::sleep_for(SPIN_DELAY);
    now = std::chrono::system_clock::now();
  }

  if (state() == desired) {
    return ::testing::AssertionSuccess();
  } else {
    return ::testing::AssertionFailure()
        << "Failed to enter " << desired << " state within " << timeout
	<< " ms";
  }
}

::testing::AssertionResult WorkerThread::remainsInState(ThreadState desired,
							int64_t duration) {

  static const auto SPIN_DELAY = std::chrono::milliseconds(10);
  
  auto now = std::chrono::system_clock::now();
  auto deadline = now + std::chrono::milliseconds(duration);
      
  while ((state() == desired) && (now < deadline)) {
    std::this_thread::sleep_for(SPIN_DELAY);
    now = std::chrono::system_clock::now();
  }

  if (state() == desired) {
    return ::testing::AssertionSuccess();
  } else {
    return ::testing::AssertionFailure()
        << "Did not remain in " << desired << " state for " << duration
	<< " ms";
  }
}

void WorkerThread::run_(WorkerThread* thread,
			std::function<void (WorkerThread&)> f) {
  try {
    f(*thread);
  } catch(const std::exception& e) {
    thread->addError(e.what());
    thread->setState(ThreadState::DONE);
  } catch(...) {
    std::ostringstream msg;
    thread->addError("Unknown exception caught");
    thread->setState(ThreadState::DONE);
  }
}
