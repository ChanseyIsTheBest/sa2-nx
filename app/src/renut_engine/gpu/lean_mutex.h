/**
 * @file    gpu/lean_mutex.h
 * @brief   blur-nx (2026-11-17, the Renut audit's #7 and #8): a reader-writer lock that can be a plain mutex.
 *
 * On the Switch std::shared_mutex is libnx's rwlock: a mutex, two condition variables and counters; each lock and
 * each unlock takes the inner mutex, and an unlock with a waiter wakes it through a condition variable. Blur's race had
 * 0.5-0.7 % of the Game Master in pthread_rwlock_unlock from the two shared_mutexes of Renut's renderer, whose readers
 * are rare (the guest-record registry: read without a lock since rr6-nx's port 47; the in-place shaders: read through
 * a per-thread memo since blur_renut_in_place_memo). With blur_renut_lean_rwlock on (the default), every lock, shared
 * or not, takes one std::mutex: the same exclusion for writers, the rare readers serialised.
 */
#pragma once

#include <mutex>
#include <shared_mutex>

namespace renut::gpu {

// blur_renut_lean_rwlock (guest/guest_records.cpp), read once (the toml is applied before the game takes these locks,
// as for every other key read into a function-local static).
bool LeanRwlockOn();

class LeanSharedMutex {
 public:
  void lock() {
    if (LeanRwlockOn())
      m_.lock();
    else
      rw_.lock();
  }
  void unlock() {
    if (LeanRwlockOn())
      m_.unlock();
    else
      rw_.unlock();
  }
  void lock_shared() {
    if (LeanRwlockOn())
      m_.lock();
    else
      rw_.lock_shared();
  }
  void unlock_shared() {
    if (LeanRwlockOn())
      m_.unlock();
    else
      rw_.unlock_shared();
  }

 private:
  std::mutex m_;
  std::shared_mutex rw_;
};

}  // namespace renut::gpu
