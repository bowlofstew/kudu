// Copyright (c) 2014, Cloudera, inc.
//
// Copied from Impala. Changes include:
// - Namespace + imports.
// - Adapted to Kudu metrics library.
// - Removal of ThreadGroups.
// - Switched from promise to spinlock in SuperviseThread to RunThread
//   communication.
// - Fixes for cpplint.

#ifndef KUDU_UTIL_THREAD_H
#define KUDU_UTIL_THREAD_H

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <string>

#include "gutil/atomicops.h"
#include "gutil/ref_counted.h"
#include "util/status.h"

namespace kudu {

class MetricRegistry;
class Webserver;

// Thin wrapper around boost::thread that can register itself with the singleton ThreadMgr
// (a private class implemented in thread.cc entirely, which tracks all live threads so
// that they may be monitored via the debug webpages). This class has a limited subset of
// boost::thread's API. Construction is almost the same, but clients must supply a
// category and a name for each thread so that they can be identified in the debug web
// UI. Otherwise, Join() is the only supported method from boost::thread.
//
// Each Thread object knows its operating system thread ID (tid), which can be used to
// attach debuggers to specific threads, to retrieve resource-usage statistics from the
// operating system, and to assign threads to resource control groups.
//
// TODO: Consider allowing fragment IDs as category parameters.
class Thread {
 public:
  // This constructor pattern mimics that in boost::thread. There is
  // one constructor for each number of arguments that the thread
  // function accepts. To extend the set of acceptable signatures, add
  // another constructor with <class F, class A1.... class An>.
  //
  // In general:
  //  - category: string identifying the thread category to which this thread belongs,
  //    used for organising threads together on the debug UI.
  //  - name: name of this thread. Will be appended with "-<thread-id>" to ensure
  //    uniqueness.
  //  - F - a method type that supports operator(), and the instance passed to the
  //    constructor is executed immediately in a separate thread.
  //  - A1...An - argument types whose instances are passed to f(...)
  template <class F>
  Thread(const std::string& category, const std::string& name, const F& f)
      : category_(category), name_(name), tid_(UNINITIALISED_THREAD_ID) {
    StartThread(f);
  }

  template <class F, class A1>
  Thread(const std::string& category, const std::string& name, const F& f, const A1& a1)
      : category_(category), name_(name), tid_(UNINITIALISED_THREAD_ID) {
    StartThread(boost::bind(f, a1));
  }

  template <class F, class A1, class A2>
  Thread(const std::string& category, const std::string& name, const F& f,
      const A1& a1, const A2& a2)
      : category_(category), name_(name), tid_(UNINITIALISED_THREAD_ID) {
    StartThread(boost::bind(f, a1, a2));
  }

  template <class F, class A1, class A2, class A3>
  Thread(const std::string& category, const std::string& name, const F& f,
      const A1& a1, const A2& a2, const A3& a3)
      : category_(category), name_(name), tid_(UNINITIALISED_THREAD_ID) {
    StartThread(boost::bind(f, a1, a2, a3));
  }

  template <class F, class A1, class A2, class A3, class A4>
  Thread(const std::string& category, const std::string& name, const F& f,
      const A1& a1, const A2& a2, const A3& a3, const A4& a4)
      : category_(category), name_(name), tid_(UNINITIALISED_THREAD_ID) {
    StartThread(boost::bind(f, a1, a2, a3, a4));
  }

  template <class F, class A1, class A2, class A3, class A4, class A5>
  Thread(const std::string& category, const std::string& name, const F& f,
      const A1& a1, const A2& a2, const A3& a3, const A4& a4, const A5& a5)
      : category_(category), name_(name), tid_(UNINITIALISED_THREAD_ID) {
    StartThread(boost::bind(f, a1, a2, a3, a4, a5));
  }

  template <class F, class A1, class A2, class A3, class A4, class A5, class A6>
  Thread(const std::string& category, const std::string& name, const F& f,
      const A1& a1, const A2& a2, const A3& a3, const A4& a4, const A5& a5, const A6& a6)
      : category_(category), name_(name), tid_(UNINITIALISED_THREAD_ID) {
    StartThread(boost::bind(f, a1, a2, a3, a4, a5, a6));
  }

  // Blocks until this thread finishes execution. Once this method returns, the thread
  // will be unregistered with the ThreadMgr and will not appear in the debug UI.
  void Join() const { thread_->join(); }

  // The thread ID assigned to this thread by the operating system. If the OS does not
  // support retrieving the tid, returns Thread::INVALID_THREAD_ID.
  int64_t tid() const { return tid_; }

  static const int64_t INVALID_THREAD_ID = -1;

 private:
  // To distinguish between a thread ID that can't be determined, and one that hasn't been
  // assigned. Since tid_ is set in the constructor, this value will never be seen by
  // clients of this class.
  static const int64_t UNINITIALISED_THREAD_ID = -2;

  // Function object that wraps the user-supplied function to run in a separate thread.
  typedef boost::function<void ()> ThreadFunctor;

  // The actual thread object that runs the user's method via SuperviseThread().
  boost::scoped_ptr<boost::thread> thread_;

  // Name and category for this thread
  const std::string category_;
  const std::string name_;

  // OS-specific thread ID. Set to UNINITIALISED_THREAD_ID initially, but once the
  // constructor returns from StartThread() the tid_ is guaranteed to be set either to a
  // non-negative integer, or INVALID_THREAD_ID.
  int64_t tid_;

  // Starts the thread running SuperviseThread(), and returns once that thread has
  // initialised and its TID read. Waits for notification from the started thread that
  // initialisation is complete before returning.
  void StartThread(const ThreadFunctor& functor);

  // Wrapper for the user-supplied function. Always invoked from thread_. Executes the
  // method in functor_, but before doing so registers with the global ThreadMgr and reads
  // the thread's system TID. After the method terminates, it is unregistered.
  //
  // SuperviseThread() notifies StartThread() when thread initialisation is completed via
  // the c_p_tid parameter, which is set to the new thread's system ID. After this point,
  // it is no longer safe for SuperviseThread() to refer to parameters passed by reference
  // or pointer to this method, because of a wrinkle in the lifecycle of boost threads: if
  // the thread object representing a thread should be destroyed, the actual
  // operating-system thread continues to run (the thread is detached, not
  // terminated). Therefore it's not safe to make reference to the Thread object or any of
  // its members in SuperviseThread() after it notifies the caller via thread_started that
  // initialisation is completed.  An alternative is to join() in the destructor of
  // Thread, but that's not the same semantics as boost::thread, which we are trying to
  // emulate here.
  //
  // As a result, the 'functor' parameter is deliberately copied into this method, since
  // it is used after the notification completes.h The tid parameter is written to exactly
  // once before SuperviseThread() notifies the caller.
  static void SuperviseThread(const std::string& name, const std::string& category,
      ThreadFunctor functor, Atomic64* c_p_tid);
};

// Initialises the threading subsystem. Must be called before a Thread is created.
void InitThreading();

// Registers /threadz with the debug webserver, and creates thread-tracking metrics under
// the "thread-manager." prefix
Status StartThreadInstrumentation(MetricRegistry* registry, Webserver* webserver);
} // namespace kudu

#endif /* KUDU_UTIL_THREAD_H */