// The profiler's allocation hooks (engine/core/profiler.hpp): the global
// operator new and delete, counted. Compiled into an executable that wants
// allocations in its profile - a static library's object holding these would
// never be linked, nothing refers to it. Counting costs one relaxed atomic
// load while the profiler is off.
#include <algorithm>
#include <cstdlib>
#include <new>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#define ASR_ALLOCATED_SIZE(p) malloc_size(p)
#elif defined(_WIN32)
// The CRT has no posix_memalign: aligned blocks come from _aligned_malloc and
// must go back through _aligned_free, so they get their own release below.
#include <malloc.h>
#define ASR_ALLOCATED_SIZE(p) _msize(p)
#else
#include <malloc.h>
#define ASR_ALLOCATED_SIZE(p) malloc_usable_size(p)
#endif

#include "engine/core/profiler.hpp"

namespace {
// The hooks are in: said once, at start-up (allocationsHooked).
const bool kHooked = (engine::profile::noteAllocation(0), true);

void* allocate(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    // The size only while counting: asking the heap takes its lock, and with
    // the profiler off that was a lock on every allocation of every frame.
    if (engine::profile::countingAllocations()) engine::profile::noteAllocation(ASR_ALLOCATED_SIZE(p));
    return p;
}
void* allocateAligned(std::size_t n, std::align_val_t align) {
    void* p = nullptr;
    const std::size_t a = std::max<std::size_t>(std::size_t(align), sizeof(void*));
#if defined(_WIN32)
    p = _aligned_malloc(n ? n : 1, a);
    if (!p) throw std::bad_alloc();
    if (engine::profile::countingAllocations()) engine::profile::noteAllocation(_aligned_msize(p, a, 0));
#else
    if (posix_memalign(&p, a, n ? n : 1) != 0) throw std::bad_alloc();
    if (engine::profile::countingAllocations()) engine::profile::noteAllocation(ASR_ALLOCATED_SIZE(p));
#endif
    return p;
}
void release(void* p) noexcept {
    if (!p) return;
    if (engine::profile::countingAllocations()) engine::profile::noteFree(ASR_ALLOCATED_SIZE(p));
    std::free(p);
}
void releaseAligned(void* p, std::align_val_t align) noexcept {
#if defined(_WIN32)
    if (!p) return;
    const std::size_t a = std::max<std::size_t>(std::size_t(align), sizeof(void*));
    if (engine::profile::countingAllocations()) engine::profile::noteFree(_aligned_msize(p, a, 0));
    _aligned_free(p);
#else
    (void)align;
    release(p);
#endif
}
} // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocate(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocate(n); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t a) { return allocateAligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocateAligned(n, a); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t a) noexcept { releaseAligned(p, a); }
void operator delete[](void* p, std::align_val_t a) noexcept { releaseAligned(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { releaseAligned(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { releaseAligned(p, a); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
