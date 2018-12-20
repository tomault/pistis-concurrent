#ifndef __PISTIS__CONCURRENT__POLLABLE__QUEUE_HPP__
#define __PISTIS__CONCURRENT__POLLABLE__QUEUE_HPP__

#include <pistis/concurrent/pollable/Condition.hpp>
#include <pistis/concurrent/pollable/ReadWriteToggle.hpp>
#include <pistis/concurrent/TimeUtils.hpp>
#include <pistis/exceptions/IllegalValueError.hpp>
#include <deque>
#include <memory>

namespace pistis {
  namespace concurrent {
    namespace pollable {
      
      /** @brief Events one can wait for or observe */
      enum class QueueEventType {
	/** @brief Queue goes from being not empty to empty */
	EMPTY,

	/** @brief Queue goes from being empty to not empty */
	NOT_EMPTY,

	/** @brief Queue goes from being not full to being full */
	FULL,

	/** @brief Queue goes from being full to being not full */
	NOT_FULL,
	
	/** @brief Queue crosses the high water mark from below
	 *
	 *  Specifically, this event occurs when the queue size goes from
	 *  being less than or equal to the high water mark to being
	 *  greater than it.
	 */
	HIGH_WATER_MARK,

	/** @brief Queue crosses the low water mark from above
	 *
	 *  Specifically, this event occurs when the queue size goes from
	 *  being greater than the low water mark to being equal to or
	 *  less than it.
	 */
	LOW_WATER_MARK,
      };

      template <typename Item, typename Allocator = std::allocator<Item> >
      class Queue {
      public:
	typedef Item ItemType;
	typedef Allocator AllocatorType;
      
	static const size_t MAX_QUEUE_SIZE = (size_t)-1;

	class Guard {
	public:
	  Guard(Queue& queue, QueueEventType eventType):
	      q_(&queue), t_(eventType), fd_(q_->observe(eventType)) {
	  }
	  Guard(const Guard&) = delete;
	  Guard(Guard&& other):
	      q_(other.q_), t_(other.t_), fd_(other.fd_) {
	    other.q_ = nullptr;
	    fd_ = -1;
	  }
	  ~Guard() { stop(); }

	  bool active() const { return (bool)q_; }
	  int fd() const { return fd_; }
	  void ack() { q_->ack(fd_, t_); }
	  void stop() {
	    if (active()) {
	      q_->stopObserving(fd_, t_);
	      q_ = nullptr;
	      fd_ = -1;
	    }
	  }

	  Guard& operator=(const Guard&) = delete;
	  Guard& operator=(Guard&& other) {
	    if (q_ != other.q_) {
	      q_ = other.q_;
	      other.q_ = nullptr;
	      fd_ = other.fd_;
	      other.fd_ = -1;
	    }
	    return *this;
	  }

	private:
	  Queue* q_;
	  QueueEventType t_;
	  int fd_;
	};

      private:
	typedef std::unique_lock<std::mutex> Lock_;

      public:
	Queue(const Allocator& allocator = Allocator()):
	    Queue(MAX_QUEUE_SIZE, allocator) { }

	Queue(size_t maxSize, const Allocator& allocator = Allocator()):
	    Queue(maxSize, maxSize, maxSize, allocator) {
	}

	Queue(size_t maxSize, size_t lowWaterMark, size_t highWaterMark,
	      const Allocator& allocator = Allocator()):
	    maxSize_(maxSize), lowWaterMark_(lowWaterMark),
	    highWaterMark_(highWaterMark), q_(allocator),
	    highWaterCrossed_(false) {
	  if (highWaterMark > maxSize) {
	    throw pistis::exceptions::IllegalValueError(
		"Illegal value for high water mark (> max queue size)",
		PISTIS_EX_HERE
	    );
	  }
	  if (lowWaterMark > highWaterMark) {
	    throw pistis::exceptions::IllegalValueError(
		"Illegal value for low water mark (> high water mark)",
		PISTIS_EX_HERE
	    );
	  }
	  queueState_.setState(ReadWriteToggle::WRITE_ONLY);
	}
      
	Queue(const Queue&) = delete;

	Queue(Queue&& other):
	    maxSize_(other.maxSize_), lowWaterMark_(other.lowWaterMark_),
	    highWaterMark_(other.highWaterMark_), q_(std::move(other.q_)),
	    highWaterCrossed_(other.highWaterCrossed_) {
	  other.highWaterCrossed_ = false;
	  other.queueState_.setState(ReadWriteToggle::WRITE_ONLY);
	}

	bool empty() const { return !size(); }

	size_t size() const {
	  Lock_ lock(sync_);
	  return q_.size();
	}
	size_t maxSize() const { return maxSize_; }
	
	size_t lowWaterMark() const {
	  Lock_ lock(sync_);
	  return lowWaterMark_;
	}
      
	size_t highWaterMark() const {
	  Lock_ lock(sync_);
	  return highWaterMark_;
	}
      
	Allocator allocator() const { return q_.get_allocator(); }
      
	bool aboveHighWaterMark() const {
	  Lock_ lock(sync_);
	  return q_.size() > highWaterMark_;
	}
	
	bool atOrBelowLowWaterMark() const {
	  Lock_ lock(sync_);
	  return q_.size() <= lowWaterMark_;
	}

	void setLowWaterMark(size_t value) {
	  Lock_ lock(sync_);
	  if (value > highWaterMark_) {
	    throw pistis::exceptions::IllegalValueError(
	        "Illegal value for low water mark (> high water mark)",
		PISTIS_EX_HERE
	    );
	  }
	  lowWaterMark_ = value;
	}

	void setHighWaterMark(size_t value) {
	  Lock_ lock(sync_);
	  if (value > maxSize_) {
	    throw pistis::exceptions::IllegalValueError(
	        "Illegal value for high water mark (> max queue size)",
		PISTIS_EX_HERE
	    );
	  }
	  if (value < lowWaterMark_) {
	    throw pistis::exceptions::IllegalValueError(
		"Illegal value for high water mark (< low water mark)",
		PISTIS_EX_HERE
	    );
	  }
	  highWaterMark_ = value;
	}

	Item get() {
	  Lock_ lock(sync_);
	  while (q_.empty()) {
	    waitUntilNotEmpty_(-1, lock);
	  }
	  Item item(q_.front());
	  q_.pop_front();
	  issueNotifications_(q_.size() + 1, q_.size());
	  return std::move(item);
	}
      
	bool get(Item& result, int64_t timeout = 0) {
	  if (timeout < 0) {
	    result = get();
	    return true;
	  } else {
	    Lock_ lock(sync_);
	    auto now = std::chrono::system_clock::now();
	    auto deadline = now + toMs(timeout);

	    while (q_.empty() && (now < deadline)) {
	      waitUntilNotEmpty_(toMs(deadline - now), lock);
	      now = std::chrono::system_clock::now();
	    }

	    if (q_.empty()) {
	      return false;
	    } else {
	      result = q_.front();
	      q_.pop_front();
	      issueNotifications_(q_.size() + 1, q_.size());
	      return true;
	    }
	  }
	}

	std::deque<Item, Allocator> getAll() {
	  Lock_ lock(sync_);
	  std::deque<Item> result(std::move(q_));
	  issueNotifications_(result.size(), 0);
	  return std::move(result);
	}

	bool put(const Item& item, int64_t timeout = -1) {
	  return executePut_(timeout, [&]() { q_.push_back(item); });
	}
      
	bool put(Item&& item, int64_t timeout = -1) {
	  return executePut_(timeout, [&]() {
	      q_.push_back(std::move(item));
	  });
	}

	template <typename... Args>
	void emplace(Args&&... args) {
	  this->put(Item(std::forward<Args>(args)...));
	}

	template <typename... Args>
	void tryEmplace(int64_t timeout, Args&&... args) {
	  this->put(Item(std::forward<Args>(args)...), timeout);
	}

	void clear() {
	  Lock_ lock(sync_);
	  size_t oldSize = q_.size();
	  q_.clear();
	  issueNotifications_(oldSize, 0);
	}

	bool wait(int64_t timeout, QueueEventType eventType) {
	  Lock_ lock(sync_);
	  switch(eventType) {
	    case QueueEventType::EMPTY:
	      return waitUntilEmpty_(timeout, lock);
	    
	    case QueueEventType::NOT_EMPTY:
	      return waitUntilNotEmpty_(timeout, lock);
	    
	    case QueueEventType::FULL:
	      return waitUntilFull_(timeout, lock);
	    
	    case QueueEventType::NOT_FULL:
	      return waitUntilNotFull_(timeout, lock);
	    
	    case QueueEventType::HIGH_WATER_MARK:
	      return waitUntilHighWaterMark_(timeout, lock);
	    
	    case QueueEventType::LOW_WATER_MARK:
	      return waitUntilLowWaterMark_(timeout, lock);
	    
	    default:
	      throw pistis::exceptions::IllegalValueError(
		  "Illegal value for \"eventType\"", PISTIS_EX_HERE
	      );
	  }
	}
      
	int observe(QueueEventType eventType) {
	  return selectCv_(eventType).observe();
	}

	void ack(int fd, QueueEventType eventType) {
	  selectCv_(eventType).ack(fd);
	}

	void stopObserving(int fd, QueueEventType eventType) {
	  selectCv_(eventType).stopObserving(fd);
	}

	int queueStateFd() { return queueState_.fd(); }
      
	Queue& operator=(const Queue&) = delete;
	Queue& operator=(Queue&& other) {
	  if (this != &other) {
	    maxSize_ = other.maxSize_;
	    lowWaterMark_ = other.lowWaterMark_;
	    highWaterMark_ = other.highWaterMark_;
	    highWaterCrossed_ = other.highWaterCrossed_;
	    other.highWaterCrossed_ = false;
	    
	    q_ = std::move(other.q_);

	    other.queueState_.setState(ReadWriteToggle::WRITE_ONLY);
	    if (!q_.size()) {
	      queueState_.setState(ReadWriteToggle::WRITE_ONLY);
	    } else if (q_.size() < maxSize_) {
	      queueState_.setState(ReadWriteToggle::READ_WRITE);
	    } else {
	      queueState_.setState(ReadWriteToggle::READ_ONLY);
	    }
	  }
	  return *this;
	}

      private:
	size_t maxSize_;
	size_t lowWaterMark_;
	size_t highWaterMark_;
	std::deque<Item, Allocator> q_;
	mutable std::mutex sync_;
	Condition emptyCv_;
	Condition notEmptyCv_;
	Condition fullCv_;
	Condition notFullCv_;
	Condition lowWaterMarkCv_;
	Condition highWaterMarkCv_;
	ReadWriteToggle queueState_;
	bool highWaterCrossed_;

	template <typename PutItemFunction>
	bool executePut_(int64_t timeout, PutItemFunction putItem) {
	  Lock_ lock(sync_);
	  if (timeout < 0) {
	    while (q_.size() >= maxSize_) {
	      waitUntilNotFull_(-1, lock);
	    }
	  } else {
	    auto now = std::chrono::system_clock::now();
	    auto deadline = now + toMs(timeout);

	    while ((q_.size() >= maxSize_) && (now < deadline)) {
	      waitUntilNotFull_(toMs(deadline - now), lock);
	      now = std::chrono::system_clock::now();
	    }

	    if (q_.size() >= maxSize_) {
	      return false;
	    }
	  }

	  // At this point, this thread owns the lock and q_.size() < maxSize_
	  putItem();
	  issueNotifications_(q_.size() - 1, q_.size());
	  return true;
	}

	bool waitUntilEmpty_(int64_t timeout, Lock_& lock) {
	  return waitForInvariant_(timeout, lock, emptyCv_,
				   [=]() { return q_.empty(); });
	}
      
	bool waitUntilNotEmpty_(int64_t timeout, Lock_& lock) {
	  return waitForInvariant_(timeout, lock, notEmptyCv_,
				   [=]() { return !q_.empty(); });
	}

	bool waitUntilFull_(int64_t timeout, Lock_& lock) {
	  return waitForInvariant_(timeout, lock, fullCv_,
				   [=]() { return q_.size() >= maxSize_; });
	}
      
	bool waitUntilNotFull_(int64_t timeout, Lock_& lock) {
	  return waitForInvariant_(timeout, lock, notFullCv_,
				   [=]() { return q_.size() < maxSize_; });
	}
      
	bool waitUntilLowWaterMark_(int64_t timeout, Lock_& lock) {
	  auto start = std::chrono::system_clock::now();

	  if (!waitForInvariant_(timeout, lock, highWaterMarkCv_,
				 [=]() { return highWaterCrossed_; })) {
	    return false;
	  }

	  auto now = std::chrono::system_clock::now();
	  int64_t timeLeft = std::max((int64_t)0, toMs(now - start));

	  return waitForInvariant_(timeLeft, lock, lowWaterMarkCv_,
				   [=]() {
	      return (q_.size() <= lowWaterMark_);
	  });
	}

	bool waitUntilHighWaterMark_(int64_t timeout, Lock_& lock) {
	  auto start = std::chrono::system_clock::now();

	  if (!waitForInvariant_(timeout, lock, lowWaterMarkCv_,
				 [=]() { return !highWaterCrossed_; })) {
	    return false;
	  }

	  auto now = std::chrono::system_clock::now();
	  int64_t timeLeft = std::max((int64_t)0, toMs(now - start));
	  
	  return waitForInvariant_(timeout, lock, highWaterMarkCv_,
				   [=]() {
	      return (q_.size() > highWaterMark_);
	  });
	}

	template <typename Invariant>
	static bool waitForInvariant_(int64_t timeout, Lock_& lock,
				      Condition& condition,
				      Invariant invariant) {
	  bool haveTime = true;
	  while (!invariant() && haveTime) {
	    lock.unlock();
	    haveTime = condition.wait(timeout);
	    lock.lock();
	  }
	  return invariant();
	}

	Condition& selectCv_(QueueEventType eventType) {
	  switch(eventType) {
	    case QueueEventType::EMPTY: return emptyCv_;
	    case QueueEventType::NOT_EMPTY: return notEmptyCv_;
	    case QueueEventType::FULL: return fullCv_;
	    case QueueEventType::NOT_FULL: return notFullCv_;
	    case QueueEventType::HIGH_WATER_MARK: return highWaterMarkCv_;
	    case QueueEventType::LOW_WATER_MARK: return lowWaterMarkCv_;
	    default:
	      throw pistis::exceptions::IllegalValueError(
		  "Illegal value for \"eventType\"", PISTIS_EX_HERE
	      );
	  }
	}
      
	void issueNotifications_(size_t oldSize, size_t newSize) {
	  if (!oldSize && newSize) {
	    notEmptyCv_.notifyAll();
	    queueState_.setState(ReadWriteToggle::READ_WRITE);
	  }
	  if (oldSize && !newSize) {
	    emptyCv_.notifyAll();
	    queueState_.setState(ReadWriteToggle::WRITE_ONLY);
	  }
	  if ((oldSize >= maxSize_) && (newSize < maxSize_)) {
	    notFullCv_.notifyAll();
	    queueState_.setState(ReadWriteToggle::READ_WRITE);
	  }
	  if ((oldSize < maxSize_) && (newSize >= maxSize_)) {
	    fullCv_.notifyAll();
	    queueState_.setState(ReadWriteToggle::READ_ONLY);
	  }
	  if ((oldSize <= highWaterMark_) && (newSize > highWaterMark_) &&
	      !highWaterCrossed_) {
	    highWaterMarkCv_.notifyAll();
	    highWaterCrossed_ = true;
	  }
	  if ((oldSize > lowWaterMark_) && (newSize <= lowWaterMark_) &&
	      highWaterCrossed_) {
	    lowWaterMarkCv_.notifyAll();
	    highWaterCrossed_ = false;
	  }
	}

      };

    }
  }
}
#endif
