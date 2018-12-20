#include <pistis/concurrent/pollable/Semaphore.hpp>
#include <pistis/concurrent/WorkerThread.hpp>
#include <gtest/gtest.h>

#include <thread>

using namespace pistis::concurrent;
using namespace pistis::concurrent::pollable;

namespace {
  void goDown(WorkerThread& thread, Semaphore& s) {
    thread.setState(ThreadState::WAITING);
    s.down();
    thread.setState(ThreadState::DONE);
  }

  void goDownWithTimeout(WorkerThread& thread, Semaphore& s, int64_t timeout,
			 bool& result) {
    thread.setState(ThreadState::WAITING);
    result = s.down(timeout);
    thread.setState(ThreadState::DONE);
  }

  void goUp(WorkerThread& thread, Semaphore& s) {
    thread.setState(ThreadState::WAITING);
    s.up();
    thread.setState(ThreadState::DONE);
  }

  void goUpWithTimeout(WorkerThread& thread, Semaphore& s, int64_t timeout,
		       bool& result) {
    thread.setState(ThreadState::WAITING);
    result = s.up(1, timeout);
    thread.setState(ThreadState::DONE);
  }
}

TEST(SemaphoreTests, UpDown) {
  Semaphore s;

  ASSERT_TRUE(s.fd() >= 0);

  WorkerThread downThread;
  downThread.start([&s](WorkerThread& t) { goDown(t, s); });
  ASSERT_TRUE(downThread.waitForState(ThreadState::WAITING, 100));

  s.up();
  ASSERT_TRUE(downThread.waitForState(ThreadState::DONE, 100));
  downThread.join();
}

TEST(SemaphoreTests, DownWithTimeout) {
  Semaphore s;
  bool signaled = false;

  ASSERT_TRUE(s.fd() >= 0);

  WorkerThread downThread;
  downThread.start([&](WorkerThread& t) {
      goDownWithTimeout(t, s, 1000, signaled);
  });
  ASSERT_TRUE(downThread.waitForState(ThreadState::WAITING, 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  s.up();
  
  ASSERT_TRUE(downThread.waitForState(ThreadState::DONE, 100));
  downThread.join();
  EXPECT_TRUE(signaled);  
}

TEST(SemaphoreTests, DownTimesOut) {
  Semaphore s;
  bool signaled = true;

  ASSERT_TRUE(s.fd() >= 0);

  WorkerThread downThread;
  downThread.start([&](WorkerThread& t) {
      goDownWithTimeout(t, s, 50, signaled);
  });
  ASSERT_TRUE(downThread.waitForState(ThreadState::WAITING, 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  s.up();
  
  ASSERT_TRUE(downThread.waitForState(ThreadState::DONE, 100));
  downThread.join();
  EXPECT_FALSE(signaled);
}

TEST(SemaphoreTests, UpWithoutTimeout) {
  Semaphore s;
  
  ASSERT_TRUE(s.fd() >= 0);
  s.up((uint64_t)-2);
  
  WorkerThread upThread;
  upThread.start([&](WorkerThread& t) { goUp(t, s); });
  ASSERT_TRUE(upThread.waitForState(ThreadState::WAITING, 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  s.down();
  
  ASSERT_TRUE(upThread.waitForState(ThreadState::DONE, 100));
  upThread.join();
}

TEST(SemaphoreTests, UpWithTimeout) {
  Semaphore s;
  bool signaled = false;

  ASSERT_TRUE(s.fd() >= 0);
  s.up((uint64_t)-2);

  WorkerThread upThread;
  upThread.start([&](WorkerThread& t) {
      goUpWithTimeout(t, s, 1000, signaled);
  });
  ASSERT_TRUE(upThread.waitForState(ThreadState::WAITING, 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  s.down();
  
  ASSERT_TRUE(upThread.waitForState(ThreadState::DONE, 100));
  upThread.join();
  EXPECT_TRUE(signaled);
}

TEST(SemaphoreTests, UpTimesOut) {
  Semaphore s;
  bool signaled = true;

  ASSERT_TRUE(s.fd() >= 0);
  s.up((uint64_t)-2);

  WorkerThread upThread;
  upThread.start([&](WorkerThread& t) {
      goUpWithTimeout(t, s, 50, signaled);
  });
  ASSERT_TRUE(upThread.waitForState(ThreadState::WAITING, 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  s.down();
  
  ASSERT_TRUE(upThread.waitForState(ThreadState::DONE, 100));
  upThread.join();
  EXPECT_FALSE(signaled);
}

