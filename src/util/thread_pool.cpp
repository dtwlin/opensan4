#include "util/thread_pool.h"

#include <algorithm>
#include <iostream>
#include <utility>

namespace san4 {
namespace {

constexpr unsigned kMaxThreads = 64u;

// Set while a thread executes pool work: always on workers, and on a
// `parallel_for` caller while it participates in its own batch.
thread_local bool tls_in_pool_work = false;

class ScopedPoolWork {
 public:
  ScopedPoolWork() : prev_(tls_in_pool_work) { tls_in_pool_work = true; }
  ~ScopedPoolWork() { tls_in_pool_work = prev_; }
  ScopedPoolWork(const ScopedPoolWork&) = delete;
  ScopedPoolWork& operator=(const ScopedPoolWork&) = delete;

 private:
  bool prev_;
};

}  // namespace

unsigned ThreadPool::default_thread_count() {
  return std::clamp(std::thread::hardware_concurrency(), 4u, 16u);
}

bool ThreadPool::in_pool_work() { return tls_in_pool_work; }

ThreadPool::ThreadPool(unsigned threads) {
  const unsigned n =
      std::min(threads == 0u ? default_thread_count() : threads, kMaxThreads);
  workers_.reserve(n);
  try {
    for (unsigned i = 0; i < n; ++i) {
      workers_.emplace_back([this]() { worker_loop(); });
    }
  } catch (...) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_work_.notify_all();
    for (auto& th : workers_) {
      th.join();
    }
    throw;
  }
}

ThreadPool::~ThreadPool() {
  // Dropped jobs are destroyed outside mu_: a capture whose destructor calls
  // enqueue() must not self-deadlock (it sees stop_ and is ignored).
  std::deque<std::function<void()>> dropped;
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
    dropped.swap(jobs_);
  }
  dropped.clear();
  cv_work_.notify_all();
  for (auto& th : workers_) {
    if (th.joinable()) {
      th.join();
    }
  }
}

void ThreadPool::run_chunks(Batch& batch) {
  while (true) {
    const int b = batch.next.fetch_add(batch.grain, std::memory_order_relaxed);
    if (b >= batch.end) {
      return;
    }
    const int e = (batch.end - b > batch.grain) ? b + batch.grain : batch.end;
    try {
      batch.fn(b, e);
    } catch (...) {
      std::lock_guard<std::mutex> lk(mu_);
      if (!batch.error) {
        batch.error = std::current_exception();
      }
    }
  }
}

void ThreadPool::worker_loop() {
  tls_in_pool_work = true;
  std::uint64_t seen_epoch = 0;
  std::unique_lock<std::mutex> lk(mu_);
  while (true) {
    cv_work_.wait(lk, [&]() {
      return stop_ || (batch_ != nullptr && batch_epoch_ != seen_epoch) ||
             (batch_ == nullptr && !jobs_.empty());
    });
    if (stop_) {
      return;
    }
    if (batch_ != nullptr) {
      seen_epoch = batch_epoch_;
      Batch* batch = batch_;
      ++workers_in_batch_;
      lk.unlock();
      run_chunks(*batch);
      lk.lock();
      if (--workers_in_batch_ == 0) {
        cv_done_.notify_all();
      }
      continue;
    }
    std::function<void()> job = std::move(jobs_.front());
    jobs_.pop_front();
    lk.unlock();
    try {
      job();
    } catch (const std::exception& ex) {
      std::cerr << "[san4-pool] async job threw: " << ex.what() << "\n";
    } catch (...) {
      std::cerr << "[san4-pool] async job threw a non-standard exception\n";
    }
    job = nullptr;
    lk.lock();
  }
}

void ThreadPool::parallel_for(int begin, int end, int grain,
                              FunctionRef<void(int, int)> fn) {
  if (end <= begin) {
    return;
  }
  grain = std::max(grain, 1);
  if (end - begin <= grain || workers_.size() <= 1u || tls_in_pool_work) {
    fn(begin, end);
    return;
  }

  std::lock_guard<std::mutex> dispatch_lk(dispatch_mutex_);
  Batch batch{fn, end, grain, {begin}, nullptr};
  {
    std::lock_guard<std::mutex> lk(mu_);
    batch_ = &batch;
    ++batch_epoch_;
  }
  cv_work_.notify_all();

  {
    ScopedPoolWork scope;
    run_chunks(batch);
  }

  bool jobs_pending = false;
  std::exception_ptr error;
  {
    // Every chunk has been claimed; unpublish the batch so no new worker
    // joins, then wait for the workers still running claimed chunks.
    std::unique_lock<std::mutex> lk(mu_);
    batch_ = nullptr;
    cv_done_.wait(lk, [&]() { return workers_in_batch_ == 0; });
    error = batch.error;
    jobs_pending = !jobs_.empty();
  }
  if (jobs_pending) {
    cv_work_.notify_all();
  }
  if (error) {
    std::rethrow_exception(error);
  }
}

void ThreadPool::enqueue(std::function<void()> job) {
  if (!job) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_) {
      return;
    }
    jobs_.push_back(std::move(job));
  }
  cv_work_.notify_one();
}

}  // namespace san4
