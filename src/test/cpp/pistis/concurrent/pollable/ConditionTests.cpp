#include <pistis/concurrent/pollable/Condition.hpp>
#include <pistis/concurrent/EpollSet.hpp>
#include <pistis/concurrent/WorkerThread.hpp>
#include <gtest/gtest.h>

using namespace pistis::concurrent;
using namespace pistis::concurrent::pollable;
using namespace pistis::exceptions;

namespace {
  void waitForCondition(WorkerThread& thread, Condition& condition) {
    thread.setState(ThreadState::WAITING);
    condition.wait();
    thread.setState(ThreadState::DONE);
  }

  void waitForCondition(WorkerThread& thread, Condition& condition,
			int64_t timeout, bool& triggered) {
    thread.setState(ThreadState::WAITING);
    triggered = condition.wait(timeout);
    thread.setState(ThreadState::DONE);
  }

  void pollCondition(WorkerThread& thread, Condition& condition) {
    int cFd = condition.observe();
    try {
      EpollSet epollSet(cFd, EpollEventType::READ);
      thread.setState(ThreadState::WAITING);
      epollSet.wait();
      thread.setState(ThreadState::DONE);
    } catch(...) {
      condition.stopObserving(cFd);
      throw;
    }

    
    condition.stopObserving(cFd);
  }

  void pollCondition(WorkerThread& thread, Condition& condition,
		     int64_t timeout, bool& triggered) {
    int cFd = condition.observe();
    try {
      EpollSet epollSet(cFd, EpollEventType::READ);
      thread.setState(ThreadState::WAITING);
      triggered = epollSet.wait(timeout);
      thread.setState(ThreadState::DONE);
    } catch(...) {
      condition.stopObserving(cFd);
      throw;
    }
    condition.stopObserving(cFd);
  }

  void pollWithAck(WorkerThread& thread, Condition& condition, int& stage) {
    int cFd = condition.observe();
    try {
      EpollSet epollSet(cFd, EpollEventType::READ);

      stage = 1;
      thread.setState(ThreadState::WAITING);
      epollSet.wait();

      stage = 2;
      condition.ack(cFd);

      stage = 3;
      epollSet.wait();

      stage = 4;
      thread.setState(ThreadState::DONE);
    } catch(...) {
      condition.stopObserving(cFd);
      throw;
    }
    condition.stopObserving(cFd);    
  }
		   

  void pollWithGuard(WorkerThread& thread, Condition& condition, int& stage) {
    Condition::Guard guard(condition);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);

    stage = 1;
    thread.setState(ThreadState::WAITING);
    epollSet.wait();

    stage = 2;
    guard.ack();

    stage = 3;
    epollSet.wait();

    stage = 4;
    thread.setState(ThreadState::DONE);
  }
  
  bool oneWaitingOneDone(ThreadState s1, ThreadState s2) {
    return ((s1 == ThreadState::DONE) && (s2 == ThreadState::WAITING)) ||
           ((s1 == ThreadState::WAITING) && (s2 == ThreadState::DONE));
  }

  bool bothDone(ThreadState s1, ThreadState s2) {
    return (s1 == ThreadState::DONE) && (s2 == ThreadState::DONE);
  }
}

TEST(ConditionTests, NotifyOne) {
  Condition cv;
  WorkerThread thread_1;
  WorkerThread thread_2;

  thread_1.start([&](WorkerThread& t) { waitForCondition(t, cv); });
  thread_2.start([&](WorkerThread& t) { waitForCondition(t, cv); });
  ASSERT_TRUE(thread_1.waitForState(ThreadState::WAITING, 100));
  ASSERT_TRUE(thread_2.waitForState(ThreadState::WAITING, 100));

  cv.notifyOne();

  auto now = std::chrono::system_clock::now();
  auto deadline = now + std::chrono::milliseconds(100);
  bool success = (thread_1.state() == ThreadState::DONE) ||
                 (thread_2.state() == ThreadState::DONE);
  while ((now < deadline) && !success) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    success = (thread_1.state() == ThreadState::DONE) ||
              (thread_2.state() == ThreadState::DONE);
    now = std::chrono::system_clock::now();
  }

  ASSERT_PRED2(oneWaitingOneDone, thread_1.state(), thread_2.state());

  cv.notifyOne();
  ASSERT_TRUE(thread_1.waitForState(ThreadState::DONE, 100));
  ASSERT_TRUE(thread_2.waitForState(ThreadState::DONE, 100));

  thread_1.join();
  thread_2.join();
}

TEST(ConditionTests, NotifyAll) {
  Condition cv;
  WorkerThread thread_1;
  WorkerThread thread_2;

  thread_1.start([&](WorkerThread& t) { waitForCondition(t, cv); });
  thread_2.start([&](WorkerThread& t) { waitForCondition(t, cv); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread_1.waitForState(ThreadState::WAITING, 100));
  ASSERT_TRUE(thread_2.waitForState(ThreadState::WAITING, 100));

  cv.notifyAll();
  ASSERT_TRUE(thread_1.waitForState(ThreadState::DONE, 100));
  ASSERT_TRUE(thread_2.waitForState(ThreadState::DONE, 100));

  thread_1.join();
  thread_2.join();
}

TEST(ConditionTests, WaitWithTimeout) {
  Condition cv;
  WorkerThread thread;
  bool triggered = false;

  thread.start([&](WorkerThread& t) {
      waitForCondition(t, cv, 1000, triggered);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  cv.notifyAll();
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();

  EXPECT_TRUE(triggered);
}

TEST(ConditionTests, WaitTimesOut) {
  Condition cv;
  WorkerThread thread;
  bool triggered = true;

  thread.start([&](WorkerThread& t) {
      waitForCondition(t, cv, 100, triggered);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  cv.notifyAll();
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();

  EXPECT_FALSE(triggered);
}

TEST(ConditionTests, Poll) {
  Condition cv;
  WorkerThread thread;

  thread.start([&](WorkerThread& t) { pollCondition(t, cv); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  cv.notifyAll();
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();
}

TEST(ConditionTests, PollWithAck) {
  Condition cv;
  WorkerThread thread;
  int state = 0;

  thread.start([&](WorkerThread& t) { pollWithAck(t, cv, state); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  EXPECT_EQ(1, state);
  
  cv.notifyAll();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto now = std::chrono::system_clock::now();
  auto deadline = now + std::chrono::milliseconds(100);
  while ((now < deadline) && (state != 3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    now = std::chrono::system_clock::now();
  }
  ASSERT_TRUE(state == 3);
  EXPECT_EQ(ThreadState::WAITING, thread.state());

  cv.notifyAll();
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();
}

TEST(ConditionTests, PollWithTimeout) {
  Condition cv;
  WorkerThread thread;
  bool triggered = false;

  thread.start([&](WorkerThread& t) { pollCondition(t, cv, 1000, triggered); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  cv.notifyAll();
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();
  EXPECT_TRUE(triggered);
}

TEST(ConditionTests, PollTimesOut) {
  Condition cv;
  WorkerThread thread;
  bool triggered = false;

  thread.start([&](WorkerThread& t) { pollCondition(t, cv, 100, triggered); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  cv.notifyAll();
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();
  EXPECT_FALSE(triggered);
}

TEST(ConditionTests, Guard) {
  Condition cv;
  WorkerThread thread;
  int state = 0;

  thread.start([&](WorkerThread& t) { pollWithGuard(t, cv, state); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  EXPECT_EQ(1, state);
  
  cv.notifyAll();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto now = std::chrono::system_clock::now();
  auto deadline = now + std::chrono::milliseconds(100);
  while ((now < deadline) && (state != 3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    now = std::chrono::system_clock::now();
  }
  ASSERT_TRUE(state == 3);
  EXPECT_EQ(ThreadState::WAITING, thread.state());

  cv.notifyAll();
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();
}
