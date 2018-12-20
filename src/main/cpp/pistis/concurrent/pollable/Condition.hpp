#ifndef __PISTIS__CONCURRENT__POLLABLE__CONDITION_HPP__
#define __PISTIS__CONCURRENT__POLLABLE__CONDITION_HPP__

#include <pistis/exceptions/NoSuchItem.hpp>
#include <pistis/concurrent/pollable/Semaphore.hpp>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace pistis {
  namespace concurrent {
    namespace pollable {

      /** @brief A condition variable whose state can be monitored with poll(),
       *         epoll() or select()
       *
       *  In addition to the standard condition variable interface consisting
       *  of wait(), notifuOne() and notifyAll(), the pollable::Condition
       *  class provides an interface for applications to wait for the
       *  condition variable to be signaled with poll(), epoll() or the like.
       *  This interface consists of three functions:
       *  - observe():  Returns a file descriptor the caller can use to
       *                monitor the condition variable.
       *  - ack(int):   Acknowledges the occurence of the condition and
       *                resets the file descriptor.
       *  - stopObserving(int):  Stop observing the condition variable state.
       *
       *  The contract between the condition variable and the observer is
       *  as follows:
       *  -  Calling observe() returns a file descriptor the condition
       *     variable will use to notify the observer the condition represented
       *     by the condition variable has occurred.  The condition variable
       *     does so by putting the file descriptor in the readable state.
       *     Until the condition occurs, the file descriptor is not readable.
       *
       *  -  Once the condition variable notifies the caller the condition it
       *     represents has occurred, the file descriptor will remain in the
       *     readable state until the observer calls ack() on it.
       *
       *  -  When the observer calls ack() on a file descriptor in the 
       *     readable state, the condition variable will "reset" that
       *     file descriptor so it is no longer readable and the observer
       *     becomes eligibile to receive notifications of the occurence of
       *     the condition represented by the conition variable.  The condition
       *     variable sends such notifications by making the file descriptor
       *     readable.
       *
       *  -  Calling ack() on a file descriptor not in the readable state
       *     will cause the caller to block until the file descriptor
       *     enters the readable state.
       *
       *  -  The observer calls stopObserving() on a file descriptor when
       *     it no longer wishes to receive notifications from the condition
       *     variable through that file descriptor.  That file descritpor
       *     must have been obtained from the condition variable through a
       *     call to observe().  Once the observer calls stopObserving() on
       *     a file descriptor, the observer will no longer interact with]
       *     that file descriptor, and the condition variable is free to do
       *     as it wishes with that file descriptor, including disposing of
       *     it or re-using it.  Continued interaction with the file descriptor
       *     by the observer produces undefined behavior.
       *
       *  -  The stopObserving() member function will be called only once
       *     on each file descriptor returned by observe().  Calling
       *     stopObserving() more than once on a file descriptor or on
       *     a file descriptor not obtained from observe() produces
       *     undefined behavior.
       *
       *  -  Although a file descriptor provided by the condition variable
       *     as a notification channel will enter and leave the readable
       *     state, the observer should never actually read from that file
       *     descriptor.  Nor should it write to it, close it, or perform any
       *     other operation other than using it in a call to poll(), epoll(),
       *     select() or the equivalent.  Performing operations on the file
       *     descriptor other than as described above produces undefined
       *     behavior.  Only the condition variable itself may read from,
       *     write to, or otherwise perform operations on the file descriptor
       *     except as described above.
       *
       *  -  File descriptors returned by observe() are "owned" by the
       *     condition variable.  The lifetime of such file descriptors,
       *     as measured between the call to observe() that provides the
       *     file descriptor to the observer and the call to stopObserving()
       *     that returns it to the condition variable, must not exceed the
       *     lifetime of the condition variable itself.  If such a file
       *     descriptor "outlives" its condition variable, undefined behavior
       *     results.
       *
       *  Condition variables are movable but not copyable.  Moving a condition
       *  variable is not thread-safe, and moving a condition variable with
       *  any threads waiting on it or any observers produces undefined
       *  behavior.  Destroying a condition variable with threads waiting on
       *  it or active observers produces undefined behavior.
       */
      class Condition {
      public:
	/** @brief Obtains a monitoring file descriptor from a Condition
	 *         and releases it (by calling stopObserving()) when the
	 *         Guard goes out-of-scope. 
	 */
	class Guard {
	public:
	  /** @brief Construct a new Guard object and start observing the
	   *         given condition.
	   *
	   *  Calls Condition::observe() as part of construction.
	   *
	   *  @param c  The condition to observe
	   */
	  Guard(Condition& c): c_(&c), fd_(c_->observe()) { }

	  /** @brief Guard instances are not copyable */
	  Guard(const Guard&) = delete;

	  /** @brief Move a Guard instance into a new variable */
	  Guard(Guard&& other):
	      c_(other.c_), fd_(other.fd_) {
	    other.c_ = nullptr;
	    other.fd_ = -1;
	  }

	  /** @brief Releases the notification file descriptor acquired
	   *         by this guard during construction
	   *
	   *  Calls stopObserving() on its Condition instance.
	   */
	  ~Guard() { stop(); }

	  /** @brief True if the guard is observing its condition */
	  bool active() const { return (bool)c_; }

	  /** @brief The notification file descriptor acquired by the
	   *         Guard for its condition.
	   *
	   *  Will be less than zero if the Guard is not active.
	   *  
	   *  @returns  The notification file descriptor
	   */
	  int fd() const { return fd_; }

	  /** @brief Call ack() on the condition variable the Guard observes.
	   *
	   *  See Condition::ack(int) for the behavior of this method.
	   *  Calling ack() on an inactive Guard results in undefined behavior.
	   */
	  void ack() { c_->ack(fd_); }

	  /** @brief Stop observing the Guard's condition.
	   *
	   *  Calls Condition::stopObserving().  After this call, calls to
	   *  Guard::active() will return false, calls to Guard::fd() will
	   *  return a value less than zero, and calls to Guard::ack() will
	   *  produce undefined behavior.
	   */
	  void stop() {
	    if (active()) {
	      c_->stopObserving(fd_);
	      c_ = nullptr;
	      fd_ = -1;
	    }
	  }

	  Guard& operator=(const Guard&) = delete;
	  Guard& operator=(Guard&& other) {
	    if (c_ != other.c_) {
	      c_ = other.c_;
	      other.c_ = nullptr;
	      fd_ = other.fd_;
	      other.fd_ = -1;
	    }
	    return *this;
	  }

	private:
	  Condition* c_;  ///< The condition the guard observes
	  int fd_;        ///< The notification file descriptor
	};
	
      public:
	Condition(): queue_(), observers_(), sync_() { }
	Condition(Condition&&) = default;

	/** @brief Block the calling thread until the condition variable
	 *         notifies it.
	 *
	 *  @throws pistis::exceptions::SystemError if an internal error
	 *          occurs.
	 */
	void wait() {
	  std::unique_lock<std::mutex> lock(sync_);
	  std::shared_ptr<Semaphore> s(new Semaphore);

	  queue_.push_back(s);
	  lock.unlock();
	  s->down();
	}

	/** @brief Block the calling thread until the condition variable
	 *         notifies it or the given timeout (in ms) expires.
	 *
	 *  @param timeout  Timeout in milliseconds
	 *  @returns  True if the wait terminated because the condition
	 *            variable notified the waiting thread, false if the
	 *            timeout expired.
	 *  @throws   pistis::exceptions::SystemError if an internal error
	 *            occurs.
	 */
	bool wait(int64_t timeout) {
	  std::unique_lock<std::mutex> lock(sync_);
	  std::shared_ptr<Semaphore> s(new Semaphore);

	  queue_.push_back(s);
	  lock.unlock();

	  return s->down(timeout);
	}

	/** @brief Returns a file descriptor the condition variable can use
	 *         to send notifications that the condition represented by
	 *         the condition variable has occurred.
	 *
	 *  The condition variable sends notifications by placing the
	 *  file descriptor in the readable state.  Calling observe() makes
	 *  the caller an "observer" of the condition variable, and the
	 *  caller must abide by the contract specified in the class
	 *  documentation.  The only permitted operations on a file
	 *  descriptor returned by observe() are:
	 *  - To monitor it with select(), poll(), epoll() or the equivalent.
	 *  - To acknowledge a notification by calling the condition variable's
	 *    ack() member function.
	 *  - To return it to the condition variable by calling stopMonitoring()
	 *  Performing any other operation on the file descriptor (such as
	 *  reading, writing, closing, etc.) produces undefined behavior.
	 *
	 *  Once the condition variable issues a notification by placing the
	 *  file descriptor in the readable state, it will not issue further
	 *  notifications until ack() is called on that file descriptor to
	 *  "reset" it.  The file descriptor will remain in the readable
	 *  state until ack() is called on it.
	 *
	 *  When an observe is done monitoring a condition variable, it should
	 *  call stopObserving() to return the file descriptor to the 
	 *  condition variable.  Once an observer has called stopObserving(),
	 *  it should no longer perform any operations on that file descriptor,
	 *  including any of the operations permitted above.  Doing so
	 *  produces undefined behavior.  Failing to return file descriptors
	 *  to the condition variable will leak file descriptors.
	 *
	 *  The normal usage pattern for monitoring a condition variable
	 *  is to call observe() to obtain a file descriptor and then using
	 *  that file descriptor in a call to select(), poll(), etc. to wait
	 *  until it becomes readable.  Once this occurs, the observer should
	 *  either call ack() or stopObserving() and then react to the
	 *  notification from the condition variable.
	 *
	 *  @returns   A file descriptor that becomes readable when the
	 *             condition variable notifies the observer.
	 *  @throws    pistis::exceptions::SystemError if an internal error
	 *             occurs.
	 */
	int observe() {
	  std::unique_lock<std::mutex> lock(sync_);
	  std::shared_ptr<Semaphore> s(new Semaphore);

	  queue_.push_back(s);
	  observers_.insert(std::make_pair(s->fd(), s));
	  return s->fd();
	}

	/** @brief "Reset" a file descriptor that has received a notification
	 *         (e.g. is readable) so it can transmit further notifications.
	 *
	 *  When an observer calls ack() on a file descriptor that has
	 *  received a notification (e.g. it is in the readable state), the
	 *  condition variable "resets" it by taking it out of the readable
	 *  state.  Once reset, the file descriptor becomes eligible to
	 *  receive future notifications from the condition variable.  Until\
	 *  ack() is called, a file descriptor in the readable state will
	 *  remain in the readable state and will not receive further
	 *  notifications.
	 *
	 *  Calling ack() on a file descriptor that has not received a
	 *  notification will block the caller until that file descriptor
	 *  receives a notification, at which point, ack() will reset the
	 *  file descriptor as described above.  Calling ack() on a file
	 *  descriptor not obtained from this condition variable's observe()
	 *  method produces undefined behavior.  Although the current
	 *  implementation throws pistis::exceptions::NoSuchItem in this case,
	 *  that behavior is a detail of the current implementation and
	 *  should not be relied upon.
	 *
	 *  @param fd  The file descriptor to reset
	 *  @throws pistis::exceptions::NoSuchItem if fd was not obtained from
	 *          this condition variable or if stopObserving() has been
	 *          called on it.
	 *  @throws pistis::exception::SystemError if an internal error
	 *          occurs
	 */
	void ack(int fd) {
	  std::unique_lock<std::mutex> lock(sync_);
	  std::shared_ptr<Semaphore> s = lookup_(fd)->second;
	  lock.unlock();
	  s->down();
	  lock.lock();
	  queue_.push_back(s);
	}

	/** @brief Return a file descriptor obtained from observe() to the
	 *         condition variable
	 *
	 *  Once an observer calls stopObserving() on a file descriptor, it
	 *  should not perform any other operations on it.  Doing so will
	 *  produce undefined behavior.  Once a file descriptor is returned
	 *  to its condition variable, the condition variable is free to
	 *  do as it pleases with it.  Every file descriptor obtained from
	 *  observe() should have stopObserving() called on it when the
	 *  observer no longer needs it.
	 *
	 *  Calling stopObserving() on a file descriptor not obtained from
	 *  this file descriptor's observe() method produces undefined
	 *  behavior.  The current implementation will throw
	 *  pistis::exceptions::NoSuchItem, but that is a detail of the
	 *  current implementation, and future implementations may behave
	 *  differently, so caller should not rely on it.
	 *
	 *  @param fd   The file descriptor to return
	 *  @throws pistis::exception::NoSuchItem  if fd did not originate
	 *          from this condition variable or if stopObserving() has
	 *          already been called on it.
	 *  @throws pistis::exception::SystemError if an internal error
	 *          occurs.
	 */
	void stopObserving(int fd) {
	  std::unique_lock<std::mutex> lock(sync_);
	  auto i = lookup_(fd);
	  observers_.erase(i);
	}

	/** @brief Notify one waiting thread or observer the condition
	 *         represented by this condition variable has occured
	 *
	 *  @throws pistis::exceptions::SystemError if an internal error
	 *          occurs.
	 */
	void notifyOne() {
	  std::unique_lock<std::mutex> lock(sync_);
	  if (queue_.size()) {
	    queue_.back()->up();
	    queue_.pop_back();
	  }
	}

	/** @brief Notify all waiting threads and observers the condition
	 *         represented by this condition variable has occurred.
	 *
	 *  @throws pistis::exceptions::SystemError if an internal error
	 *          occurs.
	 */
	void notifyAll() {
	  std::unique_lock<std::mutex> lock(sync_);
	  while (queue_.size()) {
	    queue_.back()->up();
	    queue_.pop_back();
	  }
	}
	
	Condition& operator=(Condition&&) = default;

      private:
	std::deque< std::shared_ptr<Semaphore> > queue_;
	std::unordered_map<int, std::shared_ptr<Semaphore> > observers_;
	std::mutex sync_;

	std::unordered_map< int, std::shared_ptr<Semaphore> >::iterator
	    lookup_(int fd) {
	  auto i = observers_.find(fd);
	  if (i == observers_.end()) {
	    throw pistis::exceptions::NoSuchItem(
		"file descriptor",
		"pistis::concurrent::pollable::Condition variable",
		PISTIS_EX_HERE
	    );
	  }
	  return i;
	}
      };
    }
  }
}
#endif
