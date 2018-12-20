#ifndef __TESTING__PISTIS__CONCURRENT__WORKERTHREAD_HPP__
#define __TESTING__PISTIS__CONCURRENT__WORKERTHREAD_HPP__

#include <gtest/gtest.h>
#include <functional>
#include <ostream>
#include <thread>

namespace pistis {
  namespace concurrent {

    enum class ThreadState {
      NOT_STARTED,
      STARTED,
      WAITING,
      RUNNING,
      DONE
    };

    std::ostream& operator<<(std::ostream& out, ThreadState s);

    class WorkerThread {
    public:
      WorkerThread();
      WorkerThread(WorkerThread&&) = default;
      ~WorkerThread();
		    
      bool hasErrors() const { return !errors_.empty(); }
      bool joinable() const { return thread_.joinable(); }
      ThreadState state() const { return  state_; }
      const std::vector<std::string> errors() const { return errors_; }

      void setState(ThreadState s) { state_ = s; }
      void addError(const std::string& err) { errors_.push_back(err); }
    
      void start(std::function<void (WorkerThread&)> f) {
	thread_ = std::thread(WorkerThread::run_, this, f);
      }
      void join() { thread_.join(); }
      void detach() { thread_.detach(); }

      ::testing::AssertionResult waitForState(ThreadState desired,
					      int64_t timeout);
      ::testing::AssertionResult remainsInState(ThreadState desired,
					       int64_t duration);

      WorkerThread& operator=(WorkerThread&&) = default;
    private:
      std::thread thread_;
      ThreadState state_;
      std::vector<std::string> errors_;

      static void run_(WorkerThread* thread,
		       std::function<void (WorkerThread&)> f);
    };

  }
}

#endif

