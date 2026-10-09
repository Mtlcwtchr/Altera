#pragma once
// A few threads kept for the frame's own data-parallel loops (culling and
// level selection over a hundred thousand placed objects): a range is cut into
// chunks and the caller works alongside them until every chunk is done.
// Unlike parallelFor (parallel.hpp, for loaders) no thread is created per
// call - at sixty calls a second that cost more than the loops saved.
//
// The caller waits for the chunks, never for the workers: a worker the system
// has not woken yet (the terrain bakers hold half the machine) finds the
// chunks taken and does nothing. Waiting for every worker to have run was a
// hundred-millisecond frame whenever one of them was slow to be scheduled.
//
// Workers are a few. Ranges no larger than `grain` items run inline.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace engine {

class FrameJobs {
public:
    using Work = std::function<void(std::size_t, std::size_t, std::size_t)>;
    static FrameJobs& get() {
        static FrameJobs jobs;
        return jobs;
    }
    std::size_t lanes() const { return workers_.size() + 1; }

    // work(begin, end, lane) over [0, count), in chunks of about `grain`;
    // lane < lanes() names the thread, for per-lane scratch.
    void run(std::size_t count, std::size_t grain, const Work& work) {
        if (count == 0) return;
        grain = std::max<std::size_t>(grain, 1);
        if (workers_.empty() || count <= grain) {
            work(0, count, 0);
            return;
        }
        auto job = std::make_shared<Job>();
        job->work = &work;
        job->count = count;
        job->grain = grain;
        job->chunks = (count + grain - 1) / grain;
        {
            const std::lock_guard<std::mutex> lock(guard_);
            job_ = job;
            ++generation_;
        }
        wake_.notify_all();
        drain(*job, 0);
        // The last chunks may still be running on a worker.
        std::unique_lock<std::mutex> lock(job->guard);
        job->finished.wait(lock, [&] { return job->done.load() == job->chunks; });
    }

    ~FrameJobs() {
        {
            const std::lock_guard<std::mutex> lock(guard_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
    }

private:
    struct Job {
        const Work* work = nullptr;
        std::size_t count = 0, grain = 1, chunks = 0;
        std::atomic<std::size_t> next{0}, done{0};
        std::mutex guard;
        std::condition_variable finished;
    };

    FrameJobs() {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const unsigned n = std::clamp(cores / 4, 0u, 4u);
        for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this, i] { loop(i + 1); });
    }
    static void drain(Job& job, std::size_t lane) {
        for (;;) {
            const std::size_t chunk = job.next.fetch_add(1);
            if (chunk >= job.chunks) return;
            const std::size_t begin = chunk * job.grain;
            (*job.work)(begin, std::min(job.count, begin + job.grain), lane);
            if (job.done.fetch_add(1) + 1 == job.chunks) {
                const std::lock_guard<std::mutex> lock(job.guard);
                job.finished.notify_all();
            }
        }
    }
    void loop(std::size_t lane) {
        std::uint64_t seen = 0;
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(guard_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                job = job_;
            }
            // A job already finished is a no-op here: its chunks are taken.
            if (job) drain(*job, lane);
        }
    }

    std::vector<std::thread> workers_;
    std::mutex guard_;
    std::condition_variable wake_;
    std::shared_ptr<Job> job_;
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

} // namespace engine
