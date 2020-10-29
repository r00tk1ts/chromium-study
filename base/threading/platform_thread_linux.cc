// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "base/threading/platform_thread.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>

#include "base/debug/activity_tracker.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/threading/platform_thread_internal_posix.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/threading/thread_id_name_manager.h"
#include "build/build_config.h"

#include "base/posix/can_lower_nice_to.h"

#include <sys/resource.h>
#include <sys/syscall.h>

namespace base {

size_t GetDefaultThreadStackSize(const pthread_attr_t& attributes);

namespace {
struct ThreadParams {
  ThreadParams() : delegate(nullptr), joinable(false), priority(ThreadPriority::NORMAL) {}

  PlatformThread::Delegate* delegate;
  bool joinable;
  ThreadPriority priority;
};

void* ThreadFunc(void* params) {
  PlatformThread::Delegate* delegate = nullptr;

  {
    std::unique_ptr<ThreadParams> thread_params(static_cast<ThreadParams*>(params));

    delegate = thread_params->delegate;
    if (!thread_params->joinable) base::ThreadRestrictions::SetSingletonAllowed(false);

    // Threads on linux/android may inherit their priority from the thread
    // where they were created. This explicitly sets the priority of all new
    // threads.
    PlatformThread::SetCurrentThreadPriority(thread_params->priority);
  }

  ThreadIdNameManager::GetInstance()->RegisterThread(
      PlatformThread::CurrentHandle().platform_handle(), PlatformThread::CurrentId());

  delegate->ThreadMain();

  ThreadIdNameManager::GetInstance()->RemoveName(PlatformThread::CurrentHandle().platform_handle(),
                                                 PlatformThread::CurrentId());

  return nullptr;
}

bool CreateThread(size_t stack_size, bool joinable, PlatformThread::Delegate* delegate,
                  PlatformThreadHandle* thread_handle, ThreadPriority priority) {
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);

  // Pthreads are joinable by default, so only specify the detached
  // attribute if the thread should be non-joinable.
  if (!joinable) pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

  // Get a better default if available.
  if (stack_size == 0) stack_size = base::GetDefaultThreadStackSize(attributes);

  if (stack_size > 0) pthread_attr_setstacksize(&attributes, stack_size);

  std::unique_ptr<ThreadParams> params(new ThreadParams);
  params->delegate = delegate;
  params->joinable = joinable;
  params->priority = priority;

  pthread_t handle;
  int err = pthread_create(&handle, &attributes, ThreadFunc, params.get());
  bool success = !err;
  if (success) {
    // ThreadParams should be deleted on the created thread after used.
    ignore_result(params.release());
  } else {
    // Value of |handle| is undefined if pthread_create fails.
    handle = 0;
    errno = err;
  }
  *thread_handle = PlatformThreadHandle(handle);

  pthread_attr_destroy(&attributes);

  return success;
}

// Store the thread ids in local storage since calling the SWI can
// expensive and PlatformThread::CurrentId is used liberally. Clear
// the stored value after a fork() because forking changes the thread
// id. Forking without going through fork() (e.g. clone()) is not
// supported, but there is no known usage. Using thread_local is
// fine here (despite being banned) since it is going to be allowed
// but is blocked on a clang bug for Mac (https://crbug.com/829078)
// and we can't use ThreadLocalStorage because of re-entrancy due to
// CHECK/DCHECKs.
thread_local pid_t g_thread_id = -1;

class InitAtFork {
 public:
  InitAtFork() { pthread_atfork(nullptr, nullptr, internal::ClearTidCache); }
};

const FilePath::CharType kCgroupDirectory[] = FILE_PATH_LITERAL("/sys/fs/cgroup");

FilePath ThreadPriorityToCgroupDirectory(const FilePath& cgroup_filepath, ThreadPriority priority) {
  switch (priority) {
    case ThreadPriority::NORMAL:
      return cgroup_filepath;
    case ThreadPriority::BACKGROUND:
      return cgroup_filepath.Append(FILE_PATH_LITERAL("non-urgent"));
    case ThreadPriority::DISPLAY:
    case ThreadPriority::REALTIME_AUDIO:
      return cgroup_filepath.Append(FILE_PATH_LITERAL("urgent"));
  }
  NOTREACHED();
  return FilePath();
}

void SetThreadCgroup(PlatformThreadId thread_id, const FilePath& cgroup_directory) {
  FilePath tasks_filepath = cgroup_directory.Append(FILE_PATH_LITERAL("tasks"));
  std::string tid = NumberToString(thread_id);
  int bytes_written = WriteFile(tasks_filepath, tid.c_str(), tid.size());
  if (bytes_written != static_cast<int>(tid.size())) {
    DVLOG(1) << "Failed to add " << tid << " to " << tasks_filepath.value();
  }
}

void SetThreadCgroupForThreadPriority(PlatformThreadId thread_id, const FilePath& cgroup_filepath,
                                      ThreadPriority priority) {
  // Append "chrome" suffix.
  FilePath cgroup_directory = ThreadPriorityToCgroupDirectory(
      cgroup_filepath.Append(FILE_PATH_LITERAL("chrome")), priority);

  // Silently ignore request if cgroup directory doesn't exist.
  if (!DirectoryExists(cgroup_directory)) return;

  SetThreadCgroup(thread_id, cgroup_directory);
}

void SetThreadCgroupsForThreadPriority(PlatformThreadId thread_id, ThreadPriority priority) {
  FilePath cgroup_filepath(kCgroupDirectory);
  SetThreadCgroupForThreadPriority(thread_id, cgroup_filepath.Append(FILE_PATH_LITERAL("cpuset")),
                                   priority);
  SetThreadCgroupForThreadPriority(
      thread_id, cgroup_filepath.Append(FILE_PATH_LITERAL("schedtune")), priority);
}
}  // namespace

namespace internal {

void ClearTidCache() { g_thread_id = -1; }

const struct sched_param kRealTimePrio = {8};

const ThreadPriorityToNiceValuePair kThreadPriorityToNiceValueMap[4] = {
    {ThreadPriority::BACKGROUND, 10},
    {ThreadPriority::NORMAL, 0},
    {ThreadPriority::DISPLAY, -8},
    {ThreadPriority::REALTIME_AUDIO, -10},
};

Optional<bool> CanIncreaseCurrentThreadPriorityForPlatform(ThreadPriority priority) {
  // A non-zero soft-limit on RLIMIT_RTPRIO is required to be allowed to invoke
  // pthread_setschedparam in SetCurrentThreadPriorityForPlatform().
  struct rlimit rlim;
  if (priority == ThreadPriority::REALTIME_AUDIO && getrlimit(RLIMIT_RTPRIO, &rlim) != 0 &&
      rlim.rlim_cur != 0) {
    return base::make_optional(true);
  }

  return base::nullopt;
}

bool SetCurrentThreadPriorityForPlatform(ThreadPriority priority) {
  SetThreadCgroupsForThreadPriority(PlatformThread::CurrentId(), priority);
  return priority == ThreadPriority::REALTIME_AUDIO &&
         pthread_setschedparam(pthread_self(), SCHED_RR, &kRealTimePrio) == 0;
}

Optional<ThreadPriority> GetCurrentThreadPriorityForPlatform() {
  int maybe_sched_rr = 0;
  struct sched_param maybe_realtime_prio = {0};
  if (pthread_getschedparam(pthread_self(), &maybe_sched_rr, &maybe_realtime_prio) == 0 &&
      maybe_sched_rr == SCHED_RR &&
      maybe_realtime_prio.sched_priority == kRealTimePrio.sched_priority) {
    return base::make_optional(ThreadPriority::REALTIME_AUDIO);
  }

  return base::nullopt;
}

}  // namespace internal

// static
PlatformThreadId PlatformThread::CurrentId() {
  // Pthreads doesn't have the concept of a thread ID, so we have to reach down
  // into the kernel.
  static NoDestructor<InitAtFork> init_at_fork;
  if (g_thread_id == -1) {
    g_thread_id = syscall(__NR_gettid);
  } else {
    DCHECK_EQ(g_thread_id, syscall(__NR_gettid))
        << "Thread id stored in TLS is different from thread id returned by "
           "the system. It is likely that the process was forked without going "
           "through fork().";
  }
  return g_thread_id;
}

// static
PlatformThreadRef PlatformThread::CurrentRef() { return PlatformThreadRef(pthread_self()); }

// static
void PlatformThread::YieldCurrentThread() { sched_yield(); }

// static
void PlatformThread::Sleep(TimeDelta duration) {
  struct timespec sleep_time, remaining;

  // Break the duration into seconds and nanoseconds.
  // NOTE: TimeDelta's microseconds are int64s while timespec's
  // nanoseconds are longs, so this unpacking must prevent overflow.
  sleep_time.tv_sec = duration.InSeconds();
  duration -= TimeDelta::FromSeconds(sleep_time.tv_sec);
  sleep_time.tv_nsec = duration.InMicroseconds() * 1000;  // nanoseconds

  while (nanosleep(&sleep_time, &remaining) == -1 && errno == EINTR) sleep_time = remaining;
}

// static
const char* PlatformThread::GetName() {
  return ThreadIdNameManager::GetInstance()->GetName(CurrentId());
}

// static
bool PlatformThread::CreateWithPriority(size_t stack_size, Delegate* delegate,
                                        PlatformThreadHandle* thread_handle,
                                        ThreadPriority priority) {
  return CreateThread(stack_size, true /* joinable thread */, delegate, thread_handle, priority);
}

// static
bool PlatformThread::CreateNonJoinable(size_t stack_size, Delegate* delegate) {
  return CreateNonJoinableWithPriority(stack_size, delegate, ThreadPriority::NORMAL);
}

// static
bool PlatformThread::CreateNonJoinableWithPriority(size_t stack_size, Delegate* delegate,
                                                   ThreadPriority priority) {
  PlatformThreadHandle unused;

  bool result =
      CreateThread(stack_size, false /* non-joinable thread */, delegate, &unused, priority);
  return result;
}

// static
void PlatformThread::Join(PlatformThreadHandle thread_handle) {
  // Record the event that this thread is blocking upon (for hang diagnosis).
  base::debug::ScopedThreadJoinActivity thread_activity(&thread_handle);

  // Joining another thread may block the current thread for a long time, since
  // the thread referred to by |thread_handle| may still be running long-lived /
  // blocking tasks.
  base::internal::ScopedBlockingCallWithBaseSyncPrimitives scoped_blocking_call(
      FROM_HERE, base::BlockingType::MAY_BLOCK);
  CHECK_EQ(0, pthread_join(thread_handle.platform_handle(), nullptr));
}

// static
void PlatformThread::Detach(PlatformThreadHandle thread_handle) {
  CHECK_EQ(0, pthread_detach(thread_handle.platform_handle()));
}

// static
bool PlatformThread::CanIncreaseThreadPriority(ThreadPriority priority) {
  auto platform_specific_ability = internal::CanIncreaseCurrentThreadPriorityForPlatform(priority);
  if (platform_specific_ability) return platform_specific_ability.value();

  return internal::CanLowerNiceTo(internal::ThreadPriorityToNiceValue(priority));
}

// static
void PlatformThread::SetCurrentThreadPriorityImpl(ThreadPriority priority) {
  if (internal::SetCurrentThreadPriorityForPlatform(priority)) return;
  // setpriority(2) should change the whole thread group's (i.e. process)
  // priority. However, as stated in the bugs section of
  // http://man7.org/linux/man-pages/man2/getpriority.2.html: "under the current
  // Linux/NPTL implementation of POSIX threads, the nice value is a per-thread
  // attribute". Also, 0 is prefered to the current thread id since it is
  // equivalent but makes sandboxing easier (https://crbug.com/399473).
  const int nice_setting = internal::ThreadPriorityToNiceValue(priority);
  if (setpriority(PRIO_PROCESS, 0, nice_setting)) {
    DVPLOG(1) << "Failed to set nice value of thread (" << PlatformThread::CurrentId() << ") to "
              << nice_setting;
  }
}

// static
ThreadPriority PlatformThread::GetCurrentThreadPriority() {
  // Mirrors SetCurrentThreadPriority()'s implementation.
  auto platform_specific_priority = internal::GetCurrentThreadPriorityForPlatform();
  if (platform_specific_priority) return platform_specific_priority.value();

  // Need to clear errno before calling getpriority():
  // http://man7.org/linux/man-pages/man2/getpriority.2.html
  errno = 0;
  int nice_value = getpriority(PRIO_PROCESS, 0);
  if (errno != 0) {
    DVPLOG(1) << "Failed to get nice value of thread (" << PlatformThread::CurrentId() << ")";
    return ThreadPriority::NORMAL;
  }

  return internal::NiceValueToThreadPriority(nice_value);
}

// static
size_t PlatformThread::GetDefaultThreadStackSize() {
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  return base::GetDefaultThreadStackSize(attributes);
}

// static
void PlatformThread::SetName(const std::string& name) {
  ThreadIdNameManager::GetInstance()->SetName(name);

  // On linux we can get the thread names to show up in the debugger by setting
  // the process name for the LWP.  We don't want to do this for the main
  // thread because that would rename the process, causing tools like killall
  // to stop working.
  if (PlatformThread::CurrentId() == getpid()) return;

  // http://0pointer.de/blog/projects/name-your-threads.html
  // Set the name for the LWP (which gets truncated to 15 characters).
  // Note that glibc also has a 'pthread_setname_np' api, but it may not be
  // available everywhere and it's only benefit over using prctl directly is
  // that it can set the name of threads other than the current thread.
  int err = prctl(PR_SET_NAME, name.c_str());
  // We expect EPERM failures in sandboxed processes, just ignore those.
  if (err < 0 && errno != EPERM) DPLOG(ERROR) << "prctl(PR_SET_NAME)";
}

// static
void PlatformThread::SetThreadPriority(PlatformThreadId thread_id, ThreadPriority priority) {
  // Changing current main threads' priority is not permitted in favor of
  // security, this interface is restricted to change only non-main thread
  // priority.
  CHECK_NE(thread_id, getpid());

  SetThreadCgroupsForThreadPriority(thread_id, priority);

  const int nice_setting = internal::ThreadPriorityToNiceValue(priority);
  if (setpriority(PRIO_PROCESS, thread_id, nice_setting)) {
    DVPLOG(1) << "Failed to set nice value of thread (" << thread_id << ") to " << nice_setting;
  }
}

size_t GetDefaultThreadStackSize(const pthread_attr_t& attributes) {
#if !defined(THREAD_SANITIZER)
  return 0;
#else
  // ThreadSanitizer bloats the stack heavily. Evidence has been that the
  // default stack size isn't enough for some browser tests.
  return 2 * (1 << 23);  // 2 times 8192K (the default stack size on Linux).
#endif
}

}  // namespace base