/* Copyright (c) 2008 - 2021 Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#ifndef MONITOR_HPP_
#define MONITOR_HPP_

#include "top.hpp"
#include "utils/flags.hpp"
#include "thread/semaphore.hpp"
#include "thread/thread.hpp"
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <tuple>
#include <utility>

namespace amd {

/*! \addtogroup Threads
 *  @{
 *
 *  \addtogroup Synchronization
 *  @{
 */

namespace details {

template <class T, class AllocClass = HeapObject> struct SimplyLinkedNode : public AllocClass {
  typedef SimplyLinkedNode<T, AllocClass> Node;

 protected:
  std::atomic<Node*> next_; /*!< \brief The next element. */
  T volatile item_;

 public:
  //! \brief Return the next element in the linked-list.
  Node* next() const { return next_; }
  //! \brief Return the item.
  T item() const { return item_; }

  //! \brief Set the next element pointer.
  void setNext(Node* next) { next_ = next; }
  //! \brief Set the item.
  void setItem(T item) { item_ = item; }

  //! \brief Swap the next element pointer.
  Node* swapNext(Node* next) { return next_.swap(next); }

  //! \brief Compare and set the next element pointer.
  bool compareAndSetNext(Node* compare, Node* next) {
    return next_.compare_exchange_strong(compare, next);
  }
};

}  // namespace details

  /*

class MonitorBase {
public:
  MonitorBase() {}
  virtual ~MonitorBase() = 0;
  virtual bool tryLock() = 0;
  virtual void lock() = 0;
  virtual void unlock() = 0;
  virtual void wait() = 0;
  virtual void notify() = 0;
  virtual void notifyAll() = 0;
};
  */

namespace legacy_monitor {
class Monitor: public HeapObject /*, public MonitorBase*/ {
  typedef details::SimplyLinkedNode<Semaphore*, StackObject> LinkedNode;

 private:
  static constexpr intptr_t kLockBit = 0x1;

  static constexpr int kMaxSpinIter = 55;      //!< Total number of spin iterations.
  static constexpr int kMaxReadSpinIter = 50;  //!< Read iterations before yielding

  /*! Linked list of semaphores the contending threads are waiting on
   *  and main lock.
   */
  std::atomic_intptr_t contendersList_;

  //! Semaphore of the next thread to contend for the lock.
  std::atomic_intptr_t onDeck_;
  //! Linked list of the suspended threads resume semaphores.
  LinkedNode* volatile waitersList_;

  //! Thread owning this monitor.
  Thread* volatile owner_;
  //! The amount of times this monitor was acquired by the owner.
  uint32_t lockCount_;
  //! True if this is a recursive mutex, false otherwise.
  const bool recursive_;

 private:
  //! Finish locking the mutex (contented case).
  void finishLock();
  //! Finish unlocking the mutex (contented case).
  void finishUnlock();

 protected:
  //! Try to spin-acquire the lock, return true if successful.
  bool trySpinLock();

  /*! \brief Return true if the lock is owned.
   *
   *  \note The user is responsible for the memory ordering.
   */
  bool isLocked() const { return (contendersList_ & kLockBit) != 0; }

  //! Return this monitor's owner thread (NULL if unlocked).
  Thread* owner() const { return owner_; }

  //! Set the owner.
  void setOwner(Thread* thread) { owner_ = thread; }

 public:
  explicit Monitor(bool recursive = false);
  ~Monitor() {}

  //! Try to acquire the lock, return true if successful.
  bool tryLock();

  //! Acquire the lock or suspend the calling thread.
  void lock();

  //! Release the lock and wake a single waiting thread if any.
  void unlock();

  /*! \brief Give up the lock and go to sleep.
   *
   *  Calling wait() causes the current thread to go to sleep until
   *  another thread calls notify()/notifyAll().
   *
   *  \note The monitor must be owned before calling wait().
   */
  void wait();
  /*! \brief Wake up a single thread waiting on this monitor.
   *
   *  \note The monitor must be owned before calling notify().
   */
  void notify();
  /*! \brief Wake up all threads that are waiting on this monitor.
   *
   *  \note The monitor must be owned before calling notifyAll().
   */
  void notifyAll();
};


} // namespace legacy_monitor

namespace mutex_monitor {
class Monitor final: public HeapObject /*, public MonitorBase*/ {
 public:
  explicit Monitor(bool recursive = false)
      : recursive_(recursive) {
    if (recursive)
      new (&rec_mutex_) std::recursive_mutex();
    else
      new (&mutex_) std::mutex();
  }

  ~Monitor() {
    // Caller must make sure the mutext is unlocked.
    if (recursive_)
      rec_mutex_.~recursive_mutex();
    else
      mutex_.~mutex();
  }

  //! Try to acquire the lock, return true if successful, false if failed.
  bool tryLock() {
    return recursive_ ? rec_mutex_.try_lock() : mutex_.try_lock();
  }

  //! Acquire the lock or suspend the calling thread.
  void lock() {
    recursive_ ? rec_mutex_.lock() : mutex_.lock();
  }

  //! Release the lock and wake a single waiting thread if any.
  void unlock() {
    recursive_ ? rec_mutex_.unlock() : mutex_.unlock();
  }

  /*! \brief Give up the lock and go to sleep.
   *
   *  Calling wait() causes the current thread to go to sleep until
   *  another thread calls notify()/notifyAll().
   *
   *  \note The monitor must be owned before calling wait().
   */
  void wait() {
    assert(recursive_ == false && "wait() doesn't support recursive mode");
    // the mutex must be locked by caller
    std::unique_lock lk(mutex_, std::adopt_lock);
    cv_.wait(lk);
    // the mutex is locked again
    lk.release();  // Release the ownership so that the caller should unlock the mutex
  }

  /*! \brief Wake up a single thread waiting on this monitor.
   *
   *  \note The monitor may or may not be owned before calling notify().
   */
  void notify() { cv_.notify_one(); }

  /*! \brief Wake up all threads that are waiting on this monitor.
   *
   *  \note The monitor may or may not be owned before calling notifyAll().
   */
  void notifyAll() { cv_.notify_all(); }

 private:
  union {
    std::mutex mutex_;
    std::recursive_mutex rec_mutex_;
  };
  std::condition_variable cv_; //!< The condition variable for sync on the mutex
  const bool recursive_; //!< True if this is a recursive mutex, false otherwise.
};
} // namespace mutex_monitor

// Monitor API wrapper to user
class Monitor: public legacy_monitor::Monitor {
public:
  explicit Monitor(bool recursive = false) : legacy_monitor::Monitor(recursive) {}
};
/*
class Monitor {
public:
  explicit Monitor(bool recursive = false) {
    if (DEBUG_CLR_USE_STDMUTEX_IN_AMD_MONITOR) {
      monitor_ = new mutex_monitor::Monitor(recursive);
    }
    else {
      monitor_ = new legacy_monitor::Monitor(recursive);
    }
  }
  inline ~Monitor() { delete monitor_; };
  inline bool tryLock() { return monitor_->tryLock(); }
  inline void lock() { monitor_->lock(); }
  inline void unlock() { monitor_->unlock(); }
  inline void wait() { monitor_->wait(); }
  inline void notify() { monitor_->notify(); }
  inline void notifyAll() { monitor_->notifyAll(); }

private:
  MonitorBase* monitor_;
};
*/
class ScopedLock : StackObject {
 public:
  ScopedLock(Monitor& lock) : lock_(&lock) { lock_->lock(); }

  ScopedLock(Monitor* lock) : lock_(lock) {
    if (lock_) lock_->lock();
  }

  ~ScopedLock() {
    if (lock_) lock_->unlock();
  }

 private:
  Monitor* lock_;
};

}  // namespace amd

#endif /*MONITOR_HPP_*/
