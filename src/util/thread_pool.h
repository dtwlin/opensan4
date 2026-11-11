#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "util/function_ref.h"

namespace san4 {

// Shared worker pool for data-parallel frame work (`parallel_for`) and
// fire-and-forget background jobs (`enqueue`).
//
// `parallel_for` is synchronous: the calling thread claims chunks alongside
// the workers and returns once every chunk has run. It never heap-allocates.
// Calls made from inside pool work (a worker thread, an async job, or a
// `parallel_for` body running on the caller) execute inline, so nested or
// re-entrant use cannot deadlock. Concurrent callers from different threads
// are serialized.
class ThreadPool {
 public:
  // `threads == 0` selects clamp(std::thread::hardware_concurrency(), 4, 16).
  explicit ThreadPool(unsigned threads);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Number of worker threads.
  unsigned size() const { return static_cast<unsigned>(workers_.size()); }

  static unsigned default_thread_count();

  // True while the current thread is executing pool work.
  static bool in_pool_work();

  // Invokes `fn(chunk_begin, chunk_end)` over [begin, end) split into chunks
  // of at most `grain` items. Runs inline when the range fits in one chunk,
  // the pool has at most one worker, or the caller is already inside pool
  // work. Rethrows the first exception thrown by any chunk after all chunks
  // have finished.
  void parallel_for(int begin, int end, int grain,
                    FunctionRef<void(int, int)> fn);

  // Queues a background job. Workers drain the FIFO only while no
  // `parallel_for` batch is active. Jobs still queued at destruction are
  // dropped (destroyed without running, outside the pool lock), and jobs
  // enqueued once destruction has begun are ignored. A job that throws is
  // reported on stderr and discarded.
  void enqueue(std::function<void()> job);

 private:
  struct Batch {
    FunctionRef<void(int, int)> fn;
    int end = 0;
    int grain = 1;
    std::atomic<int> next{0};
    std::exception_ptr error;  // guarded by ThreadPool::mu_
  };

  void worker_loop();
  void run_chunks(Batch& batch);

  std::mutex dispatch_mutex_;  // serializes concurrent parallel_for callers
  std::mutex mu_;
  std::condition_variable cv_work_;
  std::condition_variable cv_done_;
  bool stop_ = false;
  Batch* batch_ = nullptr;
  std::uint64_t batch_epoch_ = 0;
  int workers_in_batch_ = 0;
  std::deque<std::function<void()>> jobs_;
  std::vector<std::thread> workers_;
};

}  // namespace san4
