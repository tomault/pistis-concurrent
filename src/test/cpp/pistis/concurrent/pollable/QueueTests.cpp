#include <pistis/concurrent/pollable/Queue.hpp>
#include <pistis/concurrent/EpollSet.hpp>
#include <pistis/concurrent/TimeUtils.hpp>
#include <pistis/concurrent/WorkerThread.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <vector>

using namespace pistis::concurrent;
using namespace pistis::concurrent::pollable;
using namespace pistis::exceptions;

namespace {
  bool waitForStage(std::atomic<int>& stage, int desired, int64_t timeout) {
    auto now = std::chrono::system_clock::now();
    auto deadline = now + toMs(timeout);

    while ((stage.load() != desired) && (now < deadline)) {
      std::this_thread::sleep_for(toMs(10));
      now = std::chrono::system_clock::now();
    }

    return stage.load() == desired;
  }

  template <typename T>
  void consumeAll(WorkerThread& thread, Queue<T>& q, std::vector<T>& items,
		  std::atomic<int>& producerStage) {
    thread.setState(ThreadState::WAITING);
    if (!waitForStage(producerStage, 1, 1000)) {
      thread.addError("Did not enter producer stage 1 within 1s");
      thread.setState(ThreadState::DONE);
      return;
    }

    thread.setState(ThreadState::RUNNING);
    T item;
    while (q.get(item, 0)) {
      items.push_back(item);
    }

    thread.setState(ThreadState::DONE);
  }

  template <typename T>
  void putAll(WorkerThread& thread, Queue<T>& q, const std::vector<T>& items,
	      std::atomic<int>& producerStage) {
    thread.setState(ThreadState::WAITING);
    if (!waitForStage(producerStage, 1, 1000)) {
      thread.addError("Did not enter producer stage 1 within 1s");
      thread.setState(ThreadState::DONE);
      return;
    }

    thread.setState(ThreadState::RUNNING);
    for (const auto& item : items) {
      q.put(item);
    }

    thread.setState(ThreadState::DONE);
  }

  template <typename T>
  void waitOnQueue(WorkerThread& thread, Queue<T>& q, QueueEventType event,
		   size_t& finalSize) {
    thread.setState(ThreadState::WAITING);
    if (!q.wait(1000, event)) {
      thread.addError("Waiting did not terminate within 1s");
      return;
    }

    finalSize = q.size();
    thread.setState(ThreadState::DONE);
  }

  template <typename T>
  void pollQueue(WorkerThread& thread, Queue<T>& q, QueueEventType event,
		 size_t& finalSize) {
    typename Queue<T>::Guard guard(q, event);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);
    
    thread.setState(ThreadState::WAITING);
    if (!epollSet.wait(1000)) {
      thread.addError("Waiting did not terminate within 1s");
      return;
    }

    finalSize = q.size();
    thread.setState(ThreadState::DONE);
  }

  ::testing::AssertionResult verifyEpollEvent(const EpollEvent& event,
					      int expectedFd,
					      EpollEventType expectedEvents) {
    if (event.fd() != expectedFd) {
      return ::testing::AssertionFailure()
	  << "File descriptor is not correct.  It is " << event.fd()
	  << ". but it should be " << expectedFd;
    }

    if (event.events() != expectedEvents) {
      return ::testing::AssertionFailure()
	  << "Signaled events are not correct.  They are "
	  << (int)event.events() << ", but they should be "
	  << (int)expectedEvents;
    }
    return ::testing::AssertionSuccess();
  }
  
}

TEST(QueueTests, PutOneGetOne) {
  Queue<int> q;
  WorkerThread thread;
  std::vector<int> items{ 1, 2, 3, 4 };
  std::vector<int> read;
  std::atomic<int> producerStage(0);

  thread.start(
      [&](WorkerThread& t) { consumeAll(t, q, read, producerStage); }
  );

  for (int i : items) {
    q.put(i);
  }
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  producerStage.store(1);
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  thread.join();

  std::vector<int> truth{ 1, 2, 3, 4 };
  EXPECT_EQ(truth, read);
  EXPECT_EQ(std::vector<std::string>(), thread.errors());
}

TEST(QueueTests, PutOneGetMany) {
  static const uint32_t NUM_CONSUMERS = 4;
  static const uint32_t VALUES_PER_CONSUMER = 1024;
  Queue<int> q;
  std::vector<WorkerThread> threads;
  std::vector< std::vector<int> > buffers;
  std::atomic<int> producerStage(0);
  int i;

  for (i = 0; i < NUM_CONSUMERS; ++i) {
    threads.emplace_back();
    buffers.push_back(std::vector<int>());
  }
  
  for (i = 0; i < NUM_CONSUMERS; ++i) {
    threads[i].start([&, i](WorkerThread& t) {
	consumeAll(t, q, buffers[i], producerStage);
    });
  }

  auto now = std::chrono::system_clock::now();
  auto deadline = now + toMs(100);

  i = 0;
  while ((i < NUM_CONSUMERS) && (now < deadline)) {
    ASSERT_TRUE(threads[i].waitForState(ThreadState::WAITING,
					toMs(deadline - now)));
    now = std::chrono::system_clock::now();
    ++i;
  }
  ASSERT_EQ(NUM_CONSUMERS, i);

  for (i = 0; i < NUM_CONSUMERS * VALUES_PER_CONSUMER; ++i) {
    q.put(i);
  }

  producerStage.store(1);

  now = std::chrono::system_clock::now();
  deadline = now + toMs(100);
  i = 0;
  while ((i < NUM_CONSUMERS) && (now < deadline)) {
    ASSERT_TRUE(threads[i].waitForState(ThreadState::DONE,
					toMs(deadline - now)));
    ++i;
    now = std::chrono::system_clock::now();
  }
  ASSERT_EQ(NUM_CONSUMERS, i);

  for (i = 0; i < NUM_CONSUMERS; ++i) {
    threads[i].join();
    EXPECT_EQ(std::vector<std::string>(), threads[i].errors());
  }

  // All consumers have finished.  Look at the results
  std::vector<int> collected;
  for (auto& buffer : buffers) {
    std::copy(buffer.begin(), buffer.end(), std::back_inserter(collected));
  }
  std::sort(collected.begin(), collected.end());
  for (i = 0; i < NUM_CONSUMERS * VALUES_PER_CONSUMER; ++i) {
    if (i >= collected.size()) {
      FAIL() << "Value at index " << i << " is missing";
    } else if (collected[i] != i) {
      FAIL() << "Value at index " << i << " is incorrect.  It is "
	     << collected[i] << ", but it should be " << i;
    }
  }
  EXPECT_EQ(NUM_CONSUMERS * VALUES_PER_CONSUMER, collected.size());
}

TEST(QueueTests, PutManyGetOne) {
  static const uint32_t NUM_PRODUCERS = 4;
  static const uint32_t VALUES_PER_PRODUCER = 1024;
  Queue<int> q;
  std::vector<WorkerThread> threads;
  std::vector< std::vector<int> > buffers;
  std::atomic<int> producerStage(0);
  int i;

  for (i = 0; i < NUM_PRODUCERS; ++i) {
    threads.emplace_back();
    buffers.push_back(std::vector<int>());
    for (int j = i * VALUES_PER_PRODUCER;
	 j < (i + 1) * VALUES_PER_PRODUCER;
	 ++j) {
      buffers.back().push_back(j);
    }
  }
  
  for (i = 0; i < NUM_PRODUCERS; ++i) {
    threads[i].start([&, i](WorkerThread& t) {
	putAll(t, q, buffers[i], producerStage);
    });
  }

  auto now = std::chrono::system_clock::now();
  auto deadline = now + toMs(100);

  i = 0;
  while ((i < NUM_PRODUCERS) && (now < deadline)) {
    ASSERT_TRUE(threads[i].waitForState(ThreadState::WAITING,
					toMs(deadline - now)));
    now = std::chrono::system_clock::now();
    ++i;
  }
  ASSERT_EQ(NUM_PRODUCERS, i);

  producerStage.store(1);

  now = std::chrono::system_clock::now();
  deadline = now + toMs(100);
  i = 0;
  while ((i < NUM_PRODUCERS) && (now < deadline)) {
    ASSERT_TRUE(threads[i].waitForState(ThreadState::DONE,
					toMs(deadline - now)));
    ++i;
    now = std::chrono::system_clock::now();
  }
  ASSERT_EQ(NUM_PRODUCERS, i);

  for (i = 0; i < NUM_PRODUCERS; ++i) {
    threads[i].join();
    EXPECT_EQ(std::vector<std::string>(), threads[i].errors());
  }

  // All producers have finished.  Look at the results
  std::vector<int> collected;
  int item;
  while (q.get(item, 0)) {
    collected.push_back(item);
  }
  std::sort(collected.begin(), collected.end());
  for (i = 0; i < NUM_PRODUCERS * VALUES_PER_PRODUCER; ++i) {
    if (i >= collected.size()) {
      FAIL() << "Value at index " << i << " is missing";
    } else if (collected[i] != i) {
      FAIL() << "Value at index " << i << " is incorrect.  It is "
	     << collected[i] << ", but it should be " << i;
    }
  }
  EXPECT_EQ(NUM_PRODUCERS * VALUES_PER_PRODUCER, collected.size());
}

TEST(QueueTests, WaitForEmpty) {
  Queue<int> q;
  WorkerThread thread;
  size_t finalSize = (size_t)-1;

  q.put(1);
  
  thread.start([&](WorkerThread& t) {
      waitOnQueue(t, q, QueueEventType::EMPTY, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  EXPECT_EQ(1, q.get());

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(0, finalSize);
}

TEST(QueueTests, WaitForNotEmpty) {
  Queue<int> q;
  WorkerThread thread;
  size_t finalSize = 0;

  thread.start([&](WorkerThread& t) {
      waitOnQueue(t, q, QueueEventType::NOT_EMPTY, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  q.put(1);

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(1, finalSize);
}

TEST(QueueTests, WaitForFull) {
  Queue<int> q(3);
  WorkerThread thread;
  size_t finalSize = 0;

  thread.start([&](WorkerThread& t) {
      waitOnQueue(t, q, QueueEventType::FULL, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  q.put(1);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(2);
  q.put(3);

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(3, finalSize);
}

TEST(QueueTests, WaitForNotFull) {
  Queue<int> q(3);
  WorkerThread thread;
  size_t finalSize = 0;

  q.put(1);
  q.put(2);
  q.put(3);

  thread.start([&](WorkerThread& t) {
      waitOnQueue(t, q, QueueEventType::NOT_FULL, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  q.get();
  
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(2, finalSize);
}

TEST(QueueTests, WaitForHighWaterMark) {
  Queue<int> q(10, 2, 4);
  WorkerThread thread;
  size_t finalSize = 0;

  thread.start([&](WorkerThread& t) {
      waitOnQueue(t, q, QueueEventType::HIGH_WATER_MARK, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  q.put(1);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(2);
  q.put(3);
  q.put(4);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(5);  // Crosses HWM here

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(5, finalSize);
}

TEST(QueueTests, WaitForSecondHighWaterMarkCrossing) {
  Queue<int> q(10, 2, 4);
  WorkerThread thread;
  size_t finalSize = 0;

  // This sequence of puts and gets will cross the high water mark
  // and then fall beneath it without reaching the low water mark.
  // The call to Queue::wait() should not return success until the number
  // of items in the queue first crosses the low water mark and then crosses
  // the high water mark again.
  q.put(1);
  q.put(2);
  q.put(3);
  q.put(4);
  q.put(5); // Crosses HWM first time
  q.get();  // Back beneath HWM

  thread.start([&](WorkerThread& t) {
      waitOnQueue(t, q, QueueEventType::HIGH_WATER_MARK, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  q.get();  // Still above low water mark
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(6);
  q.put(7); // Should not trigger, since haven't fallen to low water mark
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));

  q.get();
  q.get();
  q.get();  // Hit low water mark
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  
  q.put(8);
  q.put(9);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(10); // Crosses HWM second time

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(5, finalSize);
}

TEST(QueueTests, WaitForLowWaterMark) {
  Queue<int> q(10, 2, 4);
  WorkerThread thread;
  size_t finalSize = 0;

  // This sequence of puts and gets will does not cross the high-water mark.
  // The call to Queue::wait() should not return success until the number
  // of items in the queue crosses the high water mark and then falls down
  // to the low water mark
  q.put(1);
  q.put(2);
  q.put(3);
  q.put(4);

  thread.start([&](WorkerThread& t) {
      waitOnQueue(t, q, QueueEventType::LOW_WATER_MARK, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  // Because the previous sequence did not cross the high water mark, dropping
  // down to the low water mark here does not cause the worker thread to
  // stop waiting.
  q.get();
  q.get();
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));

  // This sequence will cross the high water mark, then fall to the low
  // water mark, causing the worker to stop waiting.
  q.put(5);
  q.put(6);
  q.put(7); // Crosses HWM here

  q.get();
  q.get();
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.get();  // Reaches LWM here
  
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(2, finalSize);
}

TEST(QueueTests, ObserveQueueState) {
  Queue<int> q(2);
  EpollSet epollSet(q.queueStateFd(),
		    EpollEventType::READ|EpollEventType::WRITE);

  EXPECT_TRUE(epollSet.wait(0));
  ASSERT_EQ(1, epollSet.events().size());
  EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], q.queueStateFd(),
			       EpollEventType::WRITE));

  q.put(1);
  EXPECT_TRUE(epollSet.wait(0));
  ASSERT_EQ(1, epollSet.events().size());
  EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], q.queueStateFd(),
			       EpollEventType::READ|EpollEventType::WRITE));

  q.put(2);
  EXPECT_TRUE(epollSet.wait(0));
  ASSERT_EQ(1, epollSet.events().size());
  EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], q.queueStateFd(),
			       EpollEventType::READ));
  q.get();
  EXPECT_TRUE(epollSet.wait(0));
  ASSERT_EQ(1, epollSet.events().size());
  EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], q.queueStateFd(),
			       EpollEventType::READ|EpollEventType::WRITE));  

  q.get();
  EXPECT_TRUE(epollSet.wait(0));
  ASSERT_EQ(1, epollSet.events().size());
  EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], q.queueStateFd(),
			       EpollEventType::WRITE));  
}

TEST(QueueTests, PollForEmpty) {
  Queue<int> q;
  WorkerThread thread;
  size_t finalSize = (size_t)-1;

  q.put(1);
  
  thread.start([&](WorkerThread& t) {
      pollQueue(t, q, QueueEventType::EMPTY, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  EXPECT_EQ(1, q.get());

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(0, finalSize);
}

TEST(QueueTests, PollForNotEmpty) {
  Queue<int> q;
  WorkerThread thread;
  size_t finalSize = 0;

  thread.start([&](WorkerThread& t) {
      pollQueue(t, q, QueueEventType::NOT_EMPTY, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  q.put(1);

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(1, finalSize);
}

TEST(QueueTests, PollForFull) {
  Queue<int> q(3);
  WorkerThread thread;
  size_t finalSize = 0;

  thread.start([&](WorkerThread& t) {
      pollQueue(t, q, QueueEventType::FULL, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  q.put(1);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(2);
  q.put(3);

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(3, finalSize);
}

TEST(QueueTests, PollForNotFull) {
  Queue<int> q(3);
  WorkerThread thread;
  size_t finalSize = 0;

  q.put(1);
  q.put(2);
  q.put(3);

  thread.start([&](WorkerThread& t) {
      pollQueue(t, q, QueueEventType::NOT_FULL, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));
  
  q.get();
  
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(2, finalSize);
}

TEST(QueueTests, PollForHighWaterMark) {
  Queue<int> q(10, 2, 4);
  WorkerThread thread;
  size_t finalSize = 0;

  thread.start([&](WorkerThread& t) {
      pollQueue(t, q, QueueEventType::HIGH_WATER_MARK, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  q.put(1);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(2);
  q.put(3);
  q.put(4);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(5);  // Crosses HWM here

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(5, finalSize);
}

TEST(QueueTests, PollForSecondHighWaterMarkCrossing) {
  Queue<int> q(10, 2, 4);
  WorkerThread thread;
  size_t finalSize = 0;

  // This sequence of puts and gets will cross the high water mark
  // and then fall beneath it without reaching the low water mark.
  // The call to Queue::wait() should not return success until the number
  // of items in the queue first crosses the low water mark and then crosses
  // the high water mark again.
  q.put(1);
  q.put(2);
  q.put(3);
  q.put(4);
  q.put(5); // Crosses HWM first time
  q.get();  // Back beneath HWM

  thread.start([&](WorkerThread& t) {
      pollQueue(t, q, QueueEventType::HIGH_WATER_MARK, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  q.get();  // Still above low water mark
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(6);
  q.put(7); // Should not trigger, since haven't fallen to low water mark
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));

  q.get();
  q.get();
  q.get();  // Hit low water mark
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  
  q.put(8);
  q.put(9);
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.put(10); // Crosses HWM second time

  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(5, finalSize);
}

TEST(QueueTests, PollForLowWaterMark) {
  Queue<int> q(10, 2, 4);
  WorkerThread thread;
  size_t finalSize = 0;

  // This sequence of puts and gets will does not cross the high-water mark.
  // The call to Queue::wait() should not return success until the number
  // of items in the queue crosses the high water mark and then falls down
  // to the low water mark
  q.put(1);
  q.put(2);
  q.put(3);
  q.put(4);

  thread.start([&](WorkerThread& t) {
      pollQueue(t, q, QueueEventType::LOW_WATER_MARK, finalSize);
  });
  ASSERT_TRUE(thread.waitForState(ThreadState::WAITING, 100));

  // Because the previous sequence did not cross the high water mark, dropping
  // down to the low water mark here does not cause the worker thread to
  // stop waiting.
  q.get();
  q.get();
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));

  // This sequence will cross the high water mark, then fall to the low
  // water mark, causing the worker to stop waiting.
  q.put(5);
  q.put(6);
  q.put(7); // Crosses HWM here

  q.get();
  q.get();
  ASSERT_TRUE(thread.remainsInState(ThreadState::WAITING, 50));
  q.get();  // Reaches LWM here
  
  ASSERT_TRUE(thread.waitForState(ThreadState::DONE, 100));
  EXPECT_EQ(2, finalSize);
}

TEST(QueueTests, ConstructByMove) {
  Queue<int> q(10, 2, 4);

  // This sequence sets q.highWaterCrossed_, which should be propagated
  // to the destination container and reset in the source.
  q.put(1);
  q.put(2);
  q.put(3);
  q.put(4);
  q.put(5);  // HWM crossed here
  q.get();
  q.get();

  Queue<int> copy(std::move(q));

  EXPECT_EQ(0, q.size());
  EXPECT_EQ(10, q.maxSize());
  EXPECT_EQ(2, q.lowWaterMark());
  EXPECT_EQ(4, q.highWaterMark());

  EXPECT_EQ(3, copy.size());
  EXPECT_EQ(10, copy.maxSize());
  EXPECT_EQ(2, copy.lowWaterMark());
  EXPECT_EQ(4, copy.highWaterMark());

  // Verify that copy.highWaterCrossed_ was copied from q and\
  // q.highWaterCrossed_ was reset to false by adding and removing items
  // to move above and below the low and high water marks and checking
  // the results of polling on the low-water and high-water events.

  {
    Queue<int>::Guard guard(q, QueueEventType::HIGH_WATER_MARK);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);
    q.put(-1);
    q.put(-2);
    q.put(-3);
    q.put(-4);
    q.put(-5); // Above HWM here

    // If q.highWaterCrossed_ did not get reset to false, crossing the
    // high water mark will not trigger the event.
    EXPECT_TRUE(epollSet.wait(0));
    ASSERT_EQ(1, epollSet.events().size());
    EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], guard.fd(),
				 EpollEventType::READ));
  }

  {
    Queue<int>::Guard guard(copy, QueueEventType::HIGH_WATER_MARK);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);
    copy.put(6);
    copy.put(7); // HWM crossed, but copy.highWaterCrossed_ set, so no event

    EXPECT_FALSE(epollSet.wait(0));
  }

  {
    Queue<int>::Guard guard(copy, QueueEventType::LOW_WATER_MARK);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);

    EXPECT_EQ(3, copy.get());
    EXPECT_EQ(4, copy.get());
    EXPECT_EQ(5, copy.get());  // LWM crossed

    // If copy.highWaterCrossed_ is set, the low-water mark event was
    // triggered when the low water mark was reached.
    EXPECT_TRUE(epollSet.wait(0));
    ASSERT_EQ(1, epollSet.events().size());
    EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], guard.fd(),
				 EpollEventType::READ));
  }

  // Verify that the rest of q's content got copied
  ASSERT_EQ(2, copy.size());
  EXPECT_EQ(6, copy.get());
  EXPECT_EQ(7, copy.get());
  EXPECT_EQ(0, copy.size());  
}

TEST(QueueTests, MoveAssignment) {
  Queue<int> q(10, 2, 4);
  Queue<int> copy;

  // This sequence sets q.highWaterCrossed_, which should be propagated
  // to the destination container and reset in the source.
  q.put(1);
  q.put(2);
  q.put(3);
  q.put(4);
  q.put(5);  // HWM crossed here
  q.get();
  q.get();

  copy = std::move(q);

  EXPECT_EQ(0, q.size());
  EXPECT_EQ(10, q.maxSize());
  EXPECT_EQ(2, q.lowWaterMark());
  EXPECT_EQ(4, q.highWaterMark());

  EXPECT_EQ(3, copy.size());
  EXPECT_EQ(10, copy.maxSize());
  EXPECT_EQ(2, copy.lowWaterMark());
  EXPECT_EQ(4, copy.highWaterMark());

  // Verify that copy.highWaterCrossed_ was copied from q and\
  // q.highWaterCrossed_ was reset to false by adding and removing items
  // to move above and below the low and high water marks and checking
  // the results of polling on the low-water and high-water events.

  {
    Queue<int>::Guard guard(q, QueueEventType::HIGH_WATER_MARK);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);
    q.put(-1);
    q.put(-2);
    q.put(-3);
    q.put(-4);
    q.put(-5); // Above HWM here

    // If q.highWaterCrossed_ did not get reset to false, crossing the
    // high water mark will not trigger the event.
    EXPECT_TRUE(epollSet.wait(0));
    ASSERT_EQ(1, epollSet.events().size());
    EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], guard.fd(),
				 EpollEventType::READ));
  }

  {
    Queue<int>::Guard guard(copy, QueueEventType::HIGH_WATER_MARK);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);
    copy.put(6);
    copy.put(7); // HWM crossed, but copy.highWaterCrossed_ set, so no event

    EXPECT_FALSE(epollSet.wait(0));
  }

  {
    Queue<int>::Guard guard(copy, QueueEventType::LOW_WATER_MARK);
    EpollSet epollSet(guard.fd(), EpollEventType::READ);

    EXPECT_EQ(3, copy.get());
    EXPECT_EQ(4, copy.get());
    EXPECT_EQ(5, copy.get());  // LWM crossed

    // If copy.highWaterCrossed_ is set, the low-water mark event was
    // triggered when the low water mark was reached.
    EXPECT_TRUE(epollSet.wait(0));
    ASSERT_EQ(1, epollSet.events().size());
    EXPECT_TRUE(verifyEpollEvent(epollSet.events()[0], guard.fd(),
				 EpollEventType::READ));
  }

  // Verify that the rest of q's content got copied
  ASSERT_EQ(2, copy.size());
  EXPECT_EQ(6, copy.get());
  EXPECT_EQ(7, copy.get());
  EXPECT_EQ(0, copy.size());  
}
