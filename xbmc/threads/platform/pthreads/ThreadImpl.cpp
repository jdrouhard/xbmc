/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <limits.h>
#if defined(TARGET_ANDROID)
#include <unistd.h>
#else
#include <sys/syscall.h>
#endif
#include <sys/resource.h>
#include <string.h>
#ifdef TARGET_FREEBSD
#include <sys/param.h>
#include <pthread_np.h>
#endif

#include <signal.h>
#include "utils/log.h"

namespace XbmcThreads
{
// ==========================================================
static pthread_mutexattr_t recursiveAttr;

static bool SetRecursiveAttr()
{
  static bool alreadyCalled = false; // initialized to 0 in the data segment prior to startup init code running
  if (!alreadyCalled)
  {
    pthread_mutexattr_init(&recursiveAttr);
    pthread_mutexattr_settype(&recursiveAttr, PTHREAD_MUTEX_RECURSIVE);
#if !defined(TARGET_ANDROID)
    pthread_mutexattr_setprotocol(&recursiveAttr, PTHREAD_PRIO_INHERIT);
#endif
    alreadyCalled = true;
  }
  return true; // note, we never call destroy.
}

static bool recursiveAttrSet = SetRecursiveAttr();

pthread_mutexattr_t* CRecursiveMutex::getRecursiveAttr()
{
  if (!recursiveAttrSet) // this is only possible in the single threaded startup code
    recursiveAttrSet = SetRecursiveAttr();
  return &recursiveAttr;
}
// ==========================================================
}

static pid_t GetCurrentThreadPid_()
{
#ifdef TARGET_FREEBSD
  return pthread_getthreadid_np();
#elif defined(TARGET_ANDROID)
  return gettid();
#elif TARGET_DARWIN
  return pthread_mach_thread_np(pthread_self());
#else
  return syscall(SYS_gettid);
#endif
}

#ifdef RLIMIT_NICE
// We need to return what the best number than can be passed
// to SetPriority is. It will basically be relative to the
// the main thread's nice level, inverted (since "higher" priority
// nice levels are actually lower numbers).
static int GetUserMaxPriority(int maxPriority)
{
  // if we're root, then we can do anything. So we'll allow
  // max priority.
  if (geteuid() == 0)
    return maxPriority;

  // get user max prio
  struct rlimit limit;
  if (getrlimit(RLIMIT_NICE, &limit) == 0)
  {
    const int appNice = getpriority(PRIO_PROCESS, getpid());
    const int rlimVal = limit.rlim_cur;

    // according to the docs, limit.rlim_cur shouldn't be zero, yet, here we are.
    // if a user has no entry in limits.conf rlim_cur is zero. In this case the best
    //   nice value we can hope to achieve is '0' as a regular user
    const int userBestNiceValue = (rlimVal == 0) ? 0 : (20 - rlimVal);

    //          running the app with nice -n 10 ->
    // e.g.         +10                 10    -     0   // default non-root user.
    // e.g.         +30                 10    -     -20 // if root with rlimits set.
    //          running the app default ->
    // e.g.          0                  0    -     0   // default non-root user.
    // e.g.         +20                 0    -     -20 // if root with rlimits set.
    const int bestUserSetPriority = appNice - userBestNiceValue; // nice is inverted from prio.
    return std::min(maxPriority, bestUserSetPriority); //
  }
  else
    // If we fail getting the limit for nice we just assume we can't raise the priority
    return 0;
}
#endif

void CThread::SetThreadInfo()
{
  m_lwpId = GetCurrentThreadPid_();

#if defined(TARGET_DARWIN)
  pthread_setname_np(m_ThreadName.c_str());
#elif defined(TARGET_LINUX) && defined(__GLIBC__)
  // mthread must be set by here.
  pthread_setname_np(m_thread->native_handle(), m_ThreadName.c_str());
#endif

#ifdef RLIMIT_NICE
  // get user max prio
  int userMaxPrio = GetUserMaxPriority(GetMaxPriority());

  // if the user does not have an entry in limits.conf the following
  // call will fail
  if (userMaxPrio > 0)
  {
    // start thread with nice level of application
    int appNice = getpriority(PRIO_PROCESS, getpid());
    if (setpriority(PRIO_PROCESS, m_lwpId, appNice) != 0)
      CLog::Log(LOGERROR, "{}: error {}", __FUNCTION__, strerror(errno));
  }
#endif
}

std::uintptr_t CThread::GetCurrentThreadNativeHandle()
{
#if defined(TARGET_DARWIN) || defined(TARGET_FREEBSD)
  return reinterpret_cast<std::uintptr_t>(pthread_self());
#else
  return pthread_self();
#endif
}

uint64_t CThread::GetCurrentThreadNativeId()
{
  return static_cast<uint64_t>(GetCurrentThreadPid_());
}

int CThread::GetMinPriority(void)
{
  // one level lower than application
  return -1;
}

int CThread::GetMaxPriority(void)
{
  // one level higher than application
  return 1;
}

int CThread::GetNormalPriority(void)
{
  // same level as application
  return 0;
}

bool CThread::SetPriority(const int iPriority)
{
  bool bReturn = false;

  CSingleLock lockIt(m_CriticalSection);

  pid_t tid = static_cast<pid_t>(m_lwpId);

  if (!tid)
    bReturn = false;
#ifdef RLIMIT_NICE
  else
  {
    // get user max prio given max prio (will take the min)
    int userMaxPrio = GetUserMaxPriority(GetMaxPriority());

    // keep priority in bounds
    int prio = iPriority;
    if (prio >= GetMaxPriority())
      prio = userMaxPrio; // this is already the min of GetMaxPriority and what the user can set.
    if (prio < GetMinPriority())
      prio = GetMinPriority();

    // nice level of application
    const int appNice = getpriority(PRIO_PROCESS, getpid());
    const int newNice = appNice - prio;

    if (setpriority(PRIO_PROCESS, m_lwpId, newNice) == 0)
      bReturn = true;
    else
      CLog::Log(LOGERROR, "{}: error {}", __FUNCTION__, strerror(errno));
  }
#endif

  return bReturn;
}

int CThread::GetPriority()
{
  int iReturn;

  int appNice = getpriority(PRIO_PROCESS, getpid());
  int prio = getpriority(PRIO_PROCESS, m_lwpId);
  iReturn = appNice - prio;

  return iReturn;
}
