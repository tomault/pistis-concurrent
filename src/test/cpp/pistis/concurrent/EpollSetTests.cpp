#include <pistis/concurrent/EpollSet.hpp>
#include <pistis/exceptions/ItemExistsError.hpp>
#include <pistis/exceptions/NoSuchItem.hpp>
#include <pistis/exceptions/SystemError.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

#include <sys/eventfd.h>
#include <unistd.h>

using namespace pistis::concurrent;
using namespace pistis::exceptions;

namespace {
  auto toMs(uint32_t v) { return std::chrono::milliseconds(v); }
  
  enum class ThreadState {
    NOT_STARTED,
    STARTED,
    WAITING,
    DONE
  };

  std::ostream& operator<<(std::ostream& out, ThreadState s) {
    switch (s) {
      case ThreadState::NOT_STARTED: return out << "NOT_STARTED";
      case ThreadState::STARTED: return out << "STARTED";
      case ThreadState::WAITING: return out << "WAITING";
      case ThreadState::DONE: return out << "DONE";
      default: return out << "**UNKNOWN**";
    }
  }

  class EventFd {
  public:
    EventFd(int flags = EFD_SEMAPHORE): fd_(::eventfd(0, flags)) { }
    ~EventFd() { ::close(fd_); }

    int fd() { return fd_; }
    uint64_t read() {
      uint64_t v;
      int rc = ::read(fd(), &v, 8);
      if (rc < 0) {
	throw SystemError::fromSystemCode("Read from event fd failed: #ERR#",
					  errno, PISTIS_EX_HERE);
      } else if (rc != 8) {
	std::ostringstream msg;
	msg << "Read " << rc << " bytes from event fd, expected 8";
	throw SystemError(msg.str(), PISTIS_EX_HERE);
      }
      return v;
    }

    void write(uint64_t v = 1) {
      int rc = ::write(fd(), &v, 8);
      if (rc < 0) {
	throw SystemError::fromSystemCode("Write to event fd failed: #ERR#",
					  errno, PISTIS_EX_HERE);
      } else if (rc != 8) {
	std::ostringstream msg;
	msg << "Wrote " << rc << " bytes to event fd.  Tried to write 8.";
	throw SystemError(msg.str(), PISTIS_EX_HERE);
      }
    }

  private:
    int fd_;
  };

  class WorkerThread {
  public:
    WorkerThread(EventFd& eventFd):
        thread_(), evt_(eventFd), state_(ThreadState::NOT_STARTED),
	events_(), errors_() {
    }
    ~WorkerThread() {
      if (thread_.joinable()) {
	thread_.detach();
      }
    }

    bool hasErrors() const { return !errors_.empty(); }
    bool joinable() const { return thread_.joinable(); }
    EventFd& evt() { return evt_; }
    ThreadState state() const { return  state_; }
    const EpollEventList& events() const { return events_; }
    const std::vector<std::string> errors() const { return errors_; }

    void setState(ThreadState s) { state_ = s; }
    void setEvents(const EpollEventList& events) { events_ = events; }
    void addError(const std::string& err) { errors_.push_back(err); }
    
    void start(std::function<void (WorkerThread&)> f) {
      thread_ = std::thread(WorkerThread::run_, this, f);
    }
    void join() { thread_.join(); }
    void detach() { thread_.detach(); }

    ::testing::AssertionResult waitForState(ThreadState desired,
					    int64_t timeout) {
      auto now = std::chrono::system_clock::now();
      auto deadline = now + toMs(timeout);
      
      while ((state() != desired) && (now < deadline)) {
	std::this_thread::sleep_for(toMs(10));
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

  private:
    std::thread thread_;
    EventFd& evt_;
    ThreadState state_;
    EpollEventList events_;
    std::vector<std::string> errors_;

    static void run_(WorkerThread* thread,
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
  };
  
  void readFromEventFd(WorkerThread& thread) {
    EpollSet epollSet(thread.evt().fd(), EpollEventType::READ);

    thread.setState(ThreadState::WAITING);
    epollSet.wait();

    thread.setEvents(epollSet.events());
    thread.evt().read();
    thread.setState(ThreadState::DONE);
  }

  void writeToEventFd(WorkerThread& thread) {
    EpollSet epollSet(thread.evt().fd(), EpollEventType::WRITE);

    thread.setState(ThreadState::WAITING);
    epollSet.wait();

    thread.setEvents(epollSet.events());
    thread.evt().write();
    thread.setState(ThreadState::DONE);
  }

  void waitOnEpollSet(WorkerThread& thread, EpollSet& epollSet) {
    thread.setState(ThreadState::WAITING);
    epollSet.wait();
    thread.setEvents(epollSet.events());
    thread.setState(ThreadState::DONE);
  }
}

TEST(EpollSetTests, Add) {
  EpollSet epollSet;

  ASSERT_TRUE(epollSet.fd() >= 0);
  EXPECT_EQ(0, epollSet.numTargets());
  EXPECT_EQ(0, epollSet.events().size());

  EventFd fd1;
  epollSet.add(fd1.fd(), EpollEventType::READ|EpollEventType::READ_HANGUP,
	       EpollTrigger::EDGE);
  EXPECT_EQ(1, epollSet.numTargets());

  EventFd fd2;
  epollSet.add(fd2.fd(),
	       EpollEventType::WRITE|EpollEventType::PRIORITY|
	           EpollEventType::ERROR,
	       EpollTrigger::LEVEL, EpollRepeat::ONE_SHOT);
  EXPECT_EQ(2, epollSet.numTargets());

  EXPECT_THROW(epollSet.add(fd1.fd(), EpollEventType::WRITE),
	       ItemExistsError); 
}

TEST(EpollSetTests, Modify) {
  EpollSet epollSet;

  ASSERT_TRUE(epollSet.fd() >= 0);
  EXPECT_EQ(0, epollSet.numTargets());
  EXPECT_EQ(0, epollSet.events().size());

  EventFd fd1;
  epollSet.add(fd1.fd(), EpollEventType::READ|EpollEventType::READ_HANGUP,
	       EpollTrigger::EDGE);
  EXPECT_EQ(1, epollSet.numTargets());

  EventFd fd2;
  epollSet.add(fd2.fd(),
	       EpollEventType::WRITE|EpollEventType::PRIORITY|
	           EpollEventType::ERROR,
	       EpollTrigger::LEVEL, EpollRepeat::ONE_SHOT);
  EXPECT_EQ(2, epollSet.numTargets());

  epollSet.modify(fd1.fd(), EpollEventType::READ|EpollEventType::WRITE,
		  EpollTrigger::LEVEL, EpollRepeat::ONE_SHOT);
  EXPECT_EQ(2, epollSet.numTargets());

  EventFd fd3;
  EXPECT_THROW(epollSet.modify(fd3.fd(), EpollEventType::WRITE,
			       EpollTrigger::LEVEL, EpollRepeat::REPEATING),
	       NoSuchItem);
}

TEST(EpollSetTests, Remove) {
  EpollSet epollSet;

  ASSERT_TRUE(epollSet.fd() >= 0);
  EXPECT_EQ(0, epollSet.numTargets());
  EXPECT_EQ(0, epollSet.events().size());

  EventFd fd1;
  epollSet.add(fd1.fd(), EpollEventType::READ|EpollEventType::READ_HANGUP,
	       EpollTrigger::EDGE);
  EXPECT_EQ(1, epollSet.numTargets());

  EventFd fd2;
  epollSet.add(fd2.fd(),
	       EpollEventType::WRITE|EpollEventType::PRIORITY|
	           EpollEventType::ERROR,
	       EpollTrigger::LEVEL, EpollRepeat::ONE_SHOT);
  EXPECT_EQ(2, epollSet.numTargets());

  epollSet.remove(fd1.fd());
  EXPECT_EQ(1, epollSet.numTargets());

  epollSet.remove(fd2.fd());
  EXPECT_EQ(0, epollSet.numTargets());

  EXPECT_THROW(epollSet.remove(fd1.fd()), NoSuchItem);
}

TEST(EpollSetTests, WaitForRead) {
  EventFd evtFd;
  ASSERT_TRUE(evtFd.fd() >= 0);

  WorkerThread readThread(evtFd);

  readThread.start(readFromEventFd);
  std::this_thread::sleep_for(toMs(50));

  ASSERT_TRUE(readThread.waitForState(ThreadState::WAITING, 100));
  evtFd.write();

  ASSERT_TRUE(readThread.waitForState(ThreadState::DONE, 100));
  readThread.join();

  EpollEventList events = readThread.events();
  ASSERT_EQ(1, events.size());
  EXPECT_EQ(evtFd.fd(), events[0].fd());
  EXPECT_EQ(EpollEventType::READ, events[0].events());
  EXPECT_EQ(0, readThread.errors().size());
}

TEST(EpollSetTests, WaitForWrite) {
  EventFd evtFd;
  ASSERT_TRUE(evtFd.fd() >= 0);

  WorkerThread writeThread(evtFd);

  // Max value for eventfd forces next write to block
  evtFd.write((uint64_t)-2);

  writeThread.start(writeToEventFd);
  std::this_thread::sleep_for(toMs(50));

  ASSERT_TRUE(writeThread.waitForState(ThreadState::WAITING, 100));
  evtFd.read();

  ASSERT_TRUE(writeThread.waitForState(ThreadState::DONE, 100));
  writeThread.join();

  EpollEventList events = writeThread.events();
  ASSERT_EQ(1, events.size());
  EXPECT_EQ(evtFd.fd(), events[0].fd());
  EXPECT_EQ(EpollEventType::WRITE, events[0].events());
  EXPECT_EQ(0, writeThread.errors().size());
}

TEST(EpollSetTests, MoveConstruction) {
  EpollSet epollSet;
  
  EventFd fd1;
  epollSet.add(fd1.fd(), EpollEventType::READ|EpollEventType::READ_HANGUP);

  EventFd fd2;
  epollSet.add(fd2.fd(), EpollEventType::READ);

  EXPECT_EQ(2, epollSet.numTargets());

  WorkerThread waitingThread(fd1);
  waitingThread.start(
      [&epollSet](WorkerThread& t) { waitOnEpollSet(t, epollSet); }
  );
  std::this_thread::sleep_for(toMs(50));
  ASSERT_TRUE(waitingThread.waitForState(ThreadState::WAITING, 100));

  fd1.write();
  ASSERT_TRUE(waitingThread.waitForState(ThreadState::DONE, 100));
  waitingThread.join();

  EXPECT_EQ(1, epollSet.events().size());
  const int originalFd = epollSet.fd();

  EpollSet moved(std::move(epollSet));

  EXPECT_EQ(originalFd, moved.fd());
  EXPECT_EQ(2, moved.numTargets());
  ASSERT_EQ(1, moved.events().size());
  EXPECT_EQ(fd1.fd(), moved.events()[0].fd());
  EXPECT_EQ(EpollEventType::READ, moved.events()[0].events());

  EXPECT_EQ(-1, epollSet.fd());
  EXPECT_EQ(0, epollSet.numTargets());
  EXPECT_EQ(0, epollSet.events().size());
}

TEST(EpollSetTests, MoveAssignment) {
  EpollSet epollSet;
  EpollSet target;
  
  EventFd fd1;
  epollSet.add(fd1.fd(), EpollEventType::READ|EpollEventType::READ_HANGUP);

  EventFd fd2;
  epollSet.add(fd2.fd(), EpollEventType::READ);

  EXPECT_EQ(2, epollSet.numTargets());

  EventFd fd3;
  target.add(fd3.fd(), EpollEventType::READ);
  EXPECT_EQ(1, target.numTargets());

  WorkerThread waitingThread(fd1);
  waitingThread.start(
      [&epollSet](WorkerThread& t) { waitOnEpollSet(t, epollSet); }
  );
  std::this_thread::sleep_for(toMs(50));
  ASSERT_TRUE(waitingThread.waitForState(ThreadState::WAITING, 100));

  fd1.write();
  ASSERT_TRUE(waitingThread.waitForState(ThreadState::DONE, 100));
  waitingThread.join();

  EXPECT_EQ(1, epollSet.events().size());
  const int originalFd = epollSet.fd();

  target = std::move(epollSet);
  EXPECT_EQ(originalFd, target.fd());
  EXPECT_EQ(2, target.numTargets());
  ASSERT_EQ(1, target.events().size());
  EXPECT_EQ(fd1.fd(), target.events()[0].fd());
  EXPECT_EQ(EpollEventType::READ, target.events()[0].events());

  EXPECT_EQ(-1, epollSet.fd());
  EXPECT_EQ(0, epollSet.numTargets());
  EXPECT_EQ(0, epollSet.events().size());
}
