#pragma once
// Independent work items on every core, for the loaders: thousands of PNG
// decodes or per-model geometry that a single thread takes seconds over.
// Blocks until every item is done; items must not depend on one another.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace engine {

template <class Work>
void parallelFor(std::size_t count, Work&& work) {
    if (count == 0) return;
    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (std::size_t i = next++; i < count; i = next++) work(i);
    };
    const std::size_t threads = std::min<std::size_t>(std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 16), count);
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (std::size_t t = 1; t < threads; ++t) pool.emplace_back(run);
    run();
    for (auto& thread : pool) thread.join();
}

} // namespace engine
