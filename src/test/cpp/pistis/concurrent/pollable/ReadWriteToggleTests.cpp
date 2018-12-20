#include <pistis/concurrent/pollable/ReadWriteToggle.hpp>
#include <pistis/concurrent/EpollSet.hpp>
#include <gtest/gtest.h>

using namespace pistis::concurrent;
using namespace pistis::concurrent::pollable;

namespace {
  EpollEventType epollEventsFor(ReadWriteToggle::State s) {
    switch(s) {
      case ReadWriteToggle::READ_ONLY: return EpollEventType::READ;
      case ReadWriteToggle::WRITE_ONLY: return EpollEventType::WRITE;
      case ReadWriteToggle::READ_WRITE:
        return EpollEventType::READ|EpollEventType::WRITE;
      default:
	return EpollEventType::NONE;
    }
  }
  
  ::testing::AssertionResult verifyState(ReadWriteToggle& toggle,
					 ReadWriteToggle::State expectedState) {
    if (toggle.state() != expectedState) {
      return ::testing::AssertionFailure()
	  << "Toggle is in state " << toggle.state()
	  << ", but it should be in state " << expectedState;
    } else {
      EpollSet epollSet(toggle.fd(),
			EpollEventType::READ|EpollEventType::WRITE);
      EpollEventType desiredEvents = epollEventsFor(expectedState);
      
      if (!epollSet.wait(0)) {
	return ::testing::AssertionFailure()
	    << "EpollSet::wait(0) returned false";
      }

      const EpollEventList& events = epollSet.events();
      if (events.size() != 1) {
	return ::testing::AssertionFailure()
	    << "EpollSet::wait() returned " << events.size()
	    << " events.  There should be only one event.";
      }
      if (events[0].fd() != toggle.fd()) {
	return ::testing::AssertionFailure()
	    << "Event returned by EpollSet::wait() has incorrect fd.  "
	    << "It is " << events[0].fd() << ", but it should be "
	    << toggle.fd();
      }
      if (events[0].events() != desiredEvents) {
	return ::testing::AssertionFailure()
	    << "Event returned by EpollSet::wait() has incorrect event flags."
	    << "  They are " << (int)events[0].events()
	    << ", but they should be " << (int)desiredEvents;
      }
      return ::testing::AssertionSuccess();
    }
  }

  ::testing::AssertionResult verifyTransition(ReadWriteToggle& toggle,
					      ReadWriteToggle::State initial,
					      ReadWriteToggle::State final) {
    toggle.setState(initial);
    ::testing::AssertionResult r(verifyState(toggle, initial));
    if (r) {
      toggle.setState(final);
      return verifyState(toggle, final);
    }
    return r;
  }
}

TEST(ReadWriteToggleTests, Create) {
  ReadWriteToggle toggle;

  EXPECT_LE(0, toggle.fd());
  EXPECT_EQ(ReadWriteToggle::READ_WRITE, toggle.state());

  EpollSet epollSet;
  epollSet.add(toggle.fd(), EpollEventType::READ|EpollEventType::WRITE);

  ASSERT_TRUE(epollSet.wait(0));
  const EpollEventList& events = epollSet.events();
  ASSERT_EQ(1, events.size());
  EXPECT_EQ(toggle.fd(), events[0].fd());
  EXPECT_EQ(EpollEventType::READ|EpollEventType::WRITE, events[0].events());
}

TEST(ReadWriteToggleTests, ReadOnlyToReadWrite) {
  ReadWriteToggle toggle;
  ASSERT_TRUE(verifyTransition(toggle, ReadWriteToggle::READ_ONLY,
			       ReadWriteToggle::READ_WRITE));
}

TEST(ReadWriteToggleTests, ReadOnlyToWriteOnly) {
  ReadWriteToggle toggle;
  ASSERT_TRUE(verifyTransition(toggle, ReadWriteToggle::READ_ONLY,
			       ReadWriteToggle::WRITE_ONLY));  
}

TEST(ReadWriteToggleTests, ReadWriteToReadOnly) {
  ReadWriteToggle toggle;
  ASSERT_TRUE(verifyTransition(toggle, ReadWriteToggle::READ_WRITE,
			       ReadWriteToggle::READ_ONLY));  
}

TEST(ReadWriteToggleTests, ReadWriteToWriteOnly) {
  ReadWriteToggle toggle;
  ASSERT_TRUE(verifyTransition(toggle, ReadWriteToggle::READ_WRITE,
			       ReadWriteToggle::WRITE_ONLY));  
}

TEST(ReadWriteToggleTests, WriteOnlyToReadOnly) {
  ReadWriteToggle toggle;
  ASSERT_TRUE(verifyTransition(toggle, ReadWriteToggle::WRITE_ONLY,
			       ReadWriteToggle::READ_ONLY));  
}

TEST(ReadWriteToggleTests, WriteOnlyToReadWrite) {
  ReadWriteToggle toggle;
  ASSERT_TRUE(verifyTransition(toggle, ReadWriteToggle::WRITE_ONLY,
			       ReadWriteToggle::READ_WRITE));  
}

