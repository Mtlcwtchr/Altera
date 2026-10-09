#include "engine/core/profiler.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

#if !defined(_WIN32)
#include <cxxabi.h>
#include <dlfcn.h>
#include <pthread.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/mach_vm.h>
#endif
#else
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace engine::profile {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kStart = Clock::now();

std::atomic<bool> gHooked{false}, gCounting{false};
std::atomic<std::uint64_t> gAllocations{0}, gAllocatedBytes{0}, gFrees{0};
std::atomic<std::int64_t> gLive{0};

constexpr std::size_t kMaxDepth = 128;
// A frame's samples beyond this are dropped: a stalled main loop must not
// grow it without end (forty threads at a kilohertz for half a minute).
constexpr std::size_t kMaxPendingSamples = 1u << 20;

struct Profiler {
    std::mutex guard;   // pending, threads, frames
    std::vector<Sample> pendingSamples;
    std::vector<std::uintptr_t> pendingFrames;
    struct Known { std::uint64_t systemId; ThreadInfo info; };
    std::vector<Known> threads;
    std::unordered_map<std::uint64_t, std::uint16_t> threadIndex;   // system id -> index into threads
    std::deque<std::shared_ptr<const FrameRecord>> frames;
    std::int64_t frameBegin = 0;
    std::uint64_t frameIndex = 0;
    std::atomic<std::uint64_t> mainId{0};   // the system's id of the thread that calls frame()

    std::atomic<bool> running{false};
    std::atomic<std::int64_t> intervalNs{1000000};
    std::thread sampler;
    std::mutex wake;
    std::condition_variable stop;

    std::mutex symbolGuard;
    std::unordered_map<std::uintptr_t, Symbol> symbols;
    std::deque<std::string> names;   // what Symbol::name points into
    std::string ownImage;
};

Profiler& profiler() {
    static Profiler* p = new Profiler();   // outlives every static destructor
    return *p;
}

std::uint64_t residentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof info)) return info.WorkingSetSize;
#endif
    return 0;
}

#if defined(__APPLE__) && defined(__aarch64__)
#define ASR_SAMPLER 1

// One thread's look, in the sampler's own preallocated buffers.
struct Look {
    std::uint64_t id = 0;
    // From the kernel (THREAD_EXTENDED_INFO), not from the thread's pthread:
    // a thread that is exiting has a pthread already freed, and reading it is
    // a crash in the profiler - the engine starts and ends threads all the time.
    char name[64]{};
    std::int64_t time = 0;
    bool waiting = false;
    std::uint32_t depth = 0;
    std::array<std::uintptr_t, kMaxDepth> stack{};
};

// Every thread but this one, looked at. Nothing here allocates while a
// thread is stopped - a stopped thread may hold the allocator's lock - and
// nothing is read but the stopped thread's own stack, inside its bounds.
std::size_t sampleOnce(std::uint64_t mainId, std::vector<Look>& looks) {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return 0;
    const thread_act_t self = mach_thread_self();
    std::size_t used = 0;
    for (mach_msg_type_number_t i = 0; i < count && used < looks.size(); ++i) {
        const thread_act_t t = list[i];
        if (t == self) continue;
        thread_extended_info_data_t basic{};
        mach_msg_type_number_t basicCount = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(t, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&basic), &basicCount) != KERN_SUCCESS)
            continue;
        thread_identifier_info_data_t ident{};
        mach_msg_type_number_t identCount = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(t, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident), &identCount) != KERN_SUCCESS)
            continue;
        Look& look = looks[used++];
        look.id = ident.thread_id;
        std::memcpy(look.name, basic.pth_name, sizeof look.name);
        look.name[sizeof look.name - 1] = 0;
        look.waiting = basic.pth_run_state != TH_STATE_RUNNING;
        look.depth = 0;
        look.time = now();
        // A waiting thread is only counted as waiting - unless it is the main
        // one: what the frame waits on (the GPU, a lock) is the question.
        if (look.waiting && look.id != mainId) continue;
        if (thread_suspend(t) != KERN_SUCCESS) continue;
        arm_thread_state64_t state{};
        mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(t, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &stateCount) == KERN_SUCCESS) {
            constexpr std::uintptr_t kAddress = 0x0000FFFFFFFFFFFFull;   // no pointer signatures
            look.stack[look.depth++] = std::uintptr_t(arm_thread_state64_get_pc(state)) & kAddress;
            std::uintptr_t fp = std::uintptr_t(arm_thread_state64_get_fp(state)) & kAddress;
            // The walk stays inside the mapping the stack pointer is in, as
            // the kernel reports it: a stopped thread's stack cannot go away
            // under it, and nothing outside it is read.
            std::uintptr_t low = 0, high = 0;
            {
                mach_vm_address_t at = std::uintptr_t(arm_thread_state64_get_sp(state)) & kAddress;
                mach_vm_size_t size = 0;
                vm_region_basic_info_data_64_t region{};
                mach_msg_type_number_t regionCount = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t object = MACH_PORT_NULL;
                const mach_vm_address_t sp = at;
                if (mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
                                   reinterpret_cast<vm_region_info_t>(&region), &regionCount, &object) == KERN_SUCCESS &&
                    at <= sp && (region.protection & VM_PROT_READ)) {
                    low = std::uintptr_t(sp);
                    high = std::uintptr_t(at + size);
                }
                if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
            }
            while (look.depth < kMaxDepth && fp >= low && fp + 16 <= high && (fp & 7) == 0) {
                const auto* record = reinterpret_cast<const std::uintptr_t*>(fp);
                const std::uintptr_t next = record[0] & kAddress, ret = record[1] & kAddress;
                if (!ret) break;
                look.stack[look.depth++] = ret - 1;   // the call, not the instruction after it
                if (next <= fp) break;
                fp = next;
            }
        }
        thread_resume(t);
    }
    mach_port_deallocate(mach_task_self(), self);
    for (mach_msg_type_number_t i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), vm_address_t(list), vm_size_t(count * sizeof(thread_act_t)));
    return used;
}
#endif

#if defined(_WIN32) && defined(_M_X64)
#define ASR_SAMPLER 1

struct Look {
    std::uint64_t id = 0;
    char name[64]{};
    std::int64_t time = 0;
    bool waiting = false;
    std::uint32_t depth = 0;
    std::array<std::uintptr_t, kMaxDepth> stack{};
};

// What the sampler keeps between looks: the thread's handle (opened once) and
// how many cycles it had run, which is how a thread that has not run since the
// last look is told apart from one that has - Windows has no run state to ask.
struct Watched {
    HANDLE handle = nullptr;
    std::uint64_t cycles = 0;
    std::uint64_t seen = 0;
    char name[64]{};
};

std::unordered_map<DWORD, Watched>& watched() {
    static auto* w = new std::unordered_map<DWORD, Watched>();
    return *w;
}

// Every thread of the process but this one, looked at. Everything that may
// allocate (the thread list, names, the table above) happens before a thread is
// stopped; between SuspendThread and ResumeThread only its registers, its own
// stack and the images' unwind tables are read.
std::size_t sampleOnce(std::uint64_t mainId, std::vector<Look>& looks) {
    static std::uint64_t pass = 0;
    ++pass;
    const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    auto& known = watched();
    // The thread list is a snapshot of every thread on the machine - slow, so
    // taken every 64th look; between, the threads already known are looked at
    // (one that has ended fails to stop and is skipped until the next list).
    static std::vector<DWORD> ids;
    const bool listing = pass % 64 == 1 || ids.empty();
    if (listing) {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        ids.clear();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof entry;
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) ids.push_back(entry.th32ThreadID);
        CloseHandle(snapshot);
    }

    std::size_t used = 0;
    for (const DWORD id : ids) {
        if (used >= looks.size()) break;
        Watched& w = known[id];
        if (!w.handle) {
            w.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
            if (!w.handle) { known.erase(id); continue; }
            PWSTR wide = nullptr;
            if (SUCCEEDED(GetThreadDescription(w.handle, &wide)) && wide) {
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, w.name, int(sizeof w.name), nullptr, nullptr);
                LocalFree(wide);
            }
        }
        w.seen = pass;
        ULONG64 cycles = 0;
        QueryThreadCycleTime(w.handle, &cycles);
        Look& look = looks[used++];
        look.id = id;
        std::memcpy(look.name, w.name, sizeof look.name);
        look.waiting = w.cycles != 0 && cycles == w.cycles;
        w.cycles = cycles;
        look.depth = 0;
        look.time = now();
        // A waiting thread is only counted as waiting - unless it is the main
        // one: what the frame waits on (the GPU, a lock) is the question.
        if (look.waiting && look.id != mainId) continue;
        if (SuspendThread(w.handle) == DWORD(-1)) continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(w.handle, &context)) {
            // The walk stays inside the stack mapping the stack pointer is in.
            MEMORY_BASIC_INFORMATION region{};
            std::uintptr_t low = 0, high = 0;
            if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &region, sizeof region)) {
                low = std::uintptr_t(context.Rsp);
                high = std::uintptr_t(region.BaseAddress) + region.RegionSize;
            }
            look.stack[look.depth++] = std::uintptr_t(context.Rip);
            while (look.depth < kMaxDepth && context.Rip && context.Rsp >= low && context.Rsp < high) {
                DWORD64 base = 0;
                const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
                if (function) {
                    PVOID handler = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler, &frame, nullptr);
                } else {
                    // A leaf: the return address is on top of the stack.
                    if (context.Rsp + 8 > high) break;
                    context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                }
                if (!context.Rip) break;
                look.stack[look.depth++] = std::uintptr_t(context.Rip) - 1;   // the call, not the instruction after it
            }
        }
        ResumeThread(w.handle);
    }
    // Threads that have ended (gone from a fresh list): their handles go.
    if (listing)
        for (auto it = known.begin(); it != known.end();) {
            if (it->second.seen != pass) { CloseHandle(it->second.handle); it = known.erase(it); }
            else ++it;
        }
    return used;
}
#endif

#ifdef ASR_SAMPLER
void samplerLoop(Profiler& p) {
    std::vector<Look> looks(512);
    auto next = Clock::now();
    while (p.running.load(std::memory_order_relaxed)) {
        const std::size_t n = sampleOnce(p.mainId.load(std::memory_order_relaxed), looks);
        {
            // Every thread is running again: now it may be written down.
            const std::lock_guard<std::mutex> lock(p.guard);
            for (std::size_t i = 0; i < n; ++i) {
                const Look& look = looks[i];
                const auto found = p.threadIndex.find(look.id);
                std::size_t index = found == p.threadIndex.end() ? p.threads.size() : found->second;
                if (index == p.threads.size()) {
                    if (index > 0xFFFE) continue;
                    p.threadIndex.emplace(look.id, std::uint16_t(index));
                    Profiler::Known k{look.id, {}};
                    const char* name = look.name;
                    k.info.main = look.id == p.mainId.load();
                    k.info.name = k.info.main ? std::string("main") : name[0] ? std::string(name) : "thread " + std::to_string(index);
                    p.threads.push_back(std::move(k));
                }
                if (p.pendingSamples.size() >= kMaxPendingSamples || index > 0xFFFE) continue;
                Sample s;
                s.time = look.time;
                s.thread = std::uint16_t(index);
                s.waiting = look.waiting;
                s.first = std::uint32_t(p.pendingFrames.size());
                s.depth = look.depth;
                p.pendingFrames.insert(p.pendingFrames.end(), look.stack.begin(), look.stack.begin() + look.depth);
                p.pendingSamples.push_back(s);
            }
        }
        next += std::chrono::nanoseconds(p.intervalNs.load(std::memory_order_relaxed));
        const auto at = Clock::now();
        if (next < at) next = at;   // fell behind: no burst to catch up
        std::unique_lock<std::mutex> lock(p.wake);
        p.stop.wait_until(lock, next, [&] { return !p.running.load(std::memory_order_relaxed); });
    }
}
#endif

std::string shorten(std::string name) {
    // The arguments go: "Plan::cut(Job&) const" is "Plan::cut". Templates
    // keep theirs, and so does an operator().
    int angle = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '<') ++angle;
        else if (name[i] == '>') --angle;
        else if (name[i] == '(' && angle == 0 && i > 0 && !(i >= 8 && name.compare(i - 8, 8, "operator") == 0)) {
            name.resize(i);
            break;
        }
    }
    if (name.size() > 160) name = name.substr(0, 157) + "...";
    return name;
}

} // namespace

std::int64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - kStart).count(); }

bool enabled() { return profiler().running.load(std::memory_order_relaxed); }

void setEnabled(bool on) {
    auto& p = profiler();
#ifdef ASR_SAMPLER
    if (on == enabled()) return;
    if (on) {
        {
            const std::lock_guard<std::mutex> lock(p.guard);
            p.frameBegin = now();
            p.pendingSamples.clear();
            p.pendingFrames.clear();
        }
        // ASR_PROFILE_NO_ALLOC: samples alone, the allocations left uncounted
        // (counting asks each block its size, which skews the profile).
        gCounting.store(!std::getenv("ASR_PROFILE_NO_ALLOC"), std::memory_order_relaxed);
        p.running.store(true);
        p.sampler = std::thread([&p] {
#if defined(_WIN32)
            SetThreadDescription(GetCurrentThread(), L"profiler sampler");
#else
            pthread_setname_np("profiler sampler");
#endif
            samplerLoop(p);
        });
    } else {
        p.running.store(false);
        { std::lock_guard<std::mutex> lock(p.wake); }
        p.stop.notify_all();
        if (p.sampler.joinable()) p.sampler.join();
        gCounting.store(false, std::memory_order_relaxed);
    }
#else
    (void)on;
    (void)p;
#endif
}

void setInterval(std::chrono::microseconds interval) {
    profiler().intervalNs.store(std::clamp<std::int64_t>(interval.count(), 100, 100000) * 1000);
}
std::chrono::microseconds interval() { return std::chrono::microseconds(profiler().intervalNs.load() / 1000); }

void frame() {
    auto& p = profiler();
    if (!p.mainId.load(std::memory_order_relaxed)) {
        std::uint64_t id = 0;
#if defined(_WIN32)
        id = GetCurrentThreadId();
#else
        pthread_threadid_np(nullptr, &id);
#endif
        p.mainId.store(id);
        // Seen before it was known to be the main one: say so now.
        const std::lock_guard<std::mutex> lock(p.guard);
        if (const auto it = p.threadIndex.find(id); it != p.threadIndex.end()) {
            p.threads[it->second].info.main = true;
            p.threads[it->second].info.name = "main";
        }
    }
    if (!enabled()) return;
    auto record = std::make_shared<FrameRecord>();
    record->end = now();
    record->interval = p.intervalNs.load(std::memory_order_relaxed);
    record->allocations = gAllocations.exchange(0, std::memory_order_relaxed);
    record->allocatedBytes = gAllocatedBytes.exchange(0, std::memory_order_relaxed);
    record->frees = gFrees.exchange(0, std::memory_order_relaxed);
    record->liveBytes = gLive.load(std::memory_order_relaxed);
    record->residentBytes = residentBytes();
    const std::lock_guard<std::mutex> lock(p.guard);
    record->samples.swap(p.pendingSamples);
    record->frames.swap(p.pendingFrames);
    p.pendingSamples.reserve(record->samples.size());
    p.pendingFrames.reserve(record->frames.size());
    record->begin = p.frameBegin;
    record->index = p.frameIndex++;
    p.frameBegin = record->end;
    p.frames.push_back(std::move(record));
    while (p.frames.size() > kKeptFrames) p.frames.pop_front();
}

std::vector<ThreadInfo> threads() {
    auto& p = profiler();
    const std::lock_guard<std::mutex> lock(p.guard);
    std::vector<ThreadInfo> out;
    for (const auto& k : p.threads) out.push_back(k.info);
    return out;
}

std::vector<std::shared_ptr<const FrameRecord>> frames(std::size_t count) {
    auto& p = profiler();
    const std::lock_guard<std::mutex> lock(p.guard);
    const std::size_t n = std::min(count, p.frames.size());
    return {p.frames.end() - std::ptrdiff_t(n), p.frames.end()};
}

void clear() {
    auto& p = profiler();
    const std::lock_guard<std::mutex> lock(p.guard);
    p.frames.clear();
}

Symbol symbolOf(std::uintptr_t address) {
    auto& p = profiler();
    const std::lock_guard<std::mutex> lock(p.symbolGuard);
    if (const auto it = p.symbols.find(address); it != p.symbols.end()) return it->second;

    Symbol s;
    s.start = address;
#if !defined(_WIN32)
    if (p.ownImage.empty()) {
        Dl_info self{};
        if (dladdr(reinterpret_cast<const void*>(&now), &self) && self.dli_fname) p.ownImage = self.dli_fname;
    }
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(address), &info)) {
        std::string name;
        if (info.dli_sname) {
            int status = 0;
            char* readable = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
            name = shorten(status == 0 && readable ? readable : info.dli_sname);
            std::free(readable);
            s.start = std::uintptr_t(info.dli_saddr);
        } else {
            const char* image = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
            char buf[64];
            std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(address - std::uintptr_t(info.dli_fbase)));
            name = std::string(image ? image + 1 : "?") + buf;
        }
        s.own = info.dli_fname && !p.ownImage.empty() && p.ownImage == info.dli_fname;
        p.names.push_back(std::move(name));
        s.name = p.names.back().c_str();
    }
#else
    // DbgHelp reads the PDB beside the executable (RelWithDebInfo writes one).
    static const HANDLE process = [] {
        const HANDLE h = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(h, nullptr, TRUE);
        return h;
    }();
    const auto moduleOf = [](const void* at) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(at), &m);
        return m;
    };
    static const HMODULE own = moduleOf(reinterpret_cast<const void*>(&now));
    const HMODULE module = moduleOf(reinterpret_cast<const void*>(address));
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512]{};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(storage);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string name;
    if (SymFromAddr(process, address, &displacement, info)) {
        name = shorten(info->Name);
        s.start = address - std::uintptr_t(displacement);
    } else {
        char image[MAX_PATH] = "?";
        if (module) GetModuleFileNameA(module, image, MAX_PATH);
        const char* slash = std::strrchr(image, '\\');
        char buf[64];
        std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(address - std::uintptr_t(module)));
        name = std::string(slash ? slash + 1 : image) + buf;
    }
    s.own = module && module == own;
    p.names.push_back(std::move(name));
    s.name = p.names.back().c_str();
#endif
    p.symbols.emplace(address, s);
    return s;
}

void noteAllocation(std::size_t bytes) {
    gHooked.store(true, std::memory_order_relaxed);
    if (!gCounting.load(std::memory_order_relaxed)) return;
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    gAllocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
    gLive.fetch_add(std::int64_t(bytes), std::memory_order_relaxed);
}

void noteFree(std::size_t bytes) {
    if (!gCounting.load(std::memory_order_relaxed)) return;
    gFrees.fetch_add(1, std::memory_order_relaxed);
    gLive.fetch_sub(std::int64_t(bytes), std::memory_order_relaxed);
}

bool allocationsHooked() { return gHooked.load(std::memory_order_relaxed); }
bool countingAllocations() { return gCounting.load(std::memory_order_relaxed); }

std::vector<Bar> flame(const FrameRecord& f, std::uint16_t thread, std::size_t maxDepth) {
    std::vector<Bar> bars;
    struct Open { std::uintptr_t function; const char* name; std::int64_t begin; };
    std::vector<Open> open;
    const std::int64_t half = f.interval / 2;
    std::int64_t lastEnd = 0;
    std::vector<Symbol> stack;
    const auto closeFrom = [&](std::size_t depth, std::int64_t at) {
        while (open.size() > depth) {
            const auto& o = open.back();
            bars.push_back({o.begin, at, std::uint16_t(open.size() - 1), o.name, o.function, false});
            open.pop_back();
        }
    };
    for (const auto& s : f.samples) {
        if (s.thread != thread) continue;
        const std::int64_t begin = s.time - half, end = s.time + half;
        // A gap (the thread waited, or a look was missed) ends every bar.
        if (!open.empty() && begin > lastEnd + half) closeFrom(0, lastEnd);
        if (s.depth == 0) {
            closeFrom(0, begin);
            if (s.waiting) bars.push_back({begin, end, 0, "(waiting)", 0, true});
            lastEnd = end;
            continue;
        }
        // Outermost first, from the first function of the game's own: the
        // system's thread starts are the same in every sample.
        stack.clear();
        for (std::uint32_t k = s.depth; k-- > 0;) stack.push_back(symbolOf(f.frames[s.first + k]));
        std::size_t skip = 0;
        while (skip < stack.size() && !stack[skip].own) ++skip;
        if (skip == stack.size()) skip = 0;
        const std::size_t depth = std::min(stack.size() - skip, maxDepth);
        std::size_t same = 0;
        while (same < open.size() && same < depth && open[same].function == stack[skip + same].start) ++same;
        closeFrom(same, begin);
        for (std::size_t d = same; d < depth; ++d) open.push_back({stack[skip + d].start, stack[skip + d].name, begin});
        lastEnd = end;
    }
    closeFrom(0, lastEnd);
    std::sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) {
        return a.depth != b.depth ? a.depth < b.depth : a.begin < b.begin;
    });
    return bars;
}

bool writeChromeTrace(const std::filesystem::path& file, const std::vector<std::shared_ptr<const FrameRecord>>& list,
                      std::string* why) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        if (why) *why = "cannot write " + file.string();
        return false;
    }
    const auto escape = [](const char* s) {
        std::string o;
        for (const char* c = s ? s : ""; *c; ++c) {
            if (*c == '"' || *c == '\\') o += '\\';
            if (std::uint8_t(*c) >= 0x20) o += *c;
        }
        return o;
    };
    out << "{\"traceEvents\":[\n";
    bool first = true;
    const auto comma = [&] { if (!first) out << ",\n"; first = false; };
    const auto names = threads();
    for (std::size_t t = 0; t < names.size(); ++t) {
        comma();
        out << "{\"ph\":\"M\",\"pid\":1,\"tid\":" << t << ",\"name\":\"thread_name\",\"args\":{\"name\":\""
            << escape(names[t].name.c_str()) << "\"}}";
    }
    char buf[64];
    const auto ts = [&](std::int64_t ns) { std::snprintf(buf, sizeof buf, "%.3f", double(ns) * 1e-3); return std::string(buf); };
    for (const auto& f : list) {
        comma();
        out << "{\"ph\":\"X\",\"pid\":2,\"tid\":0,\"name\":\"frame " << f->index << "\",\"ts\":" << ts(f->begin)
            << ",\"dur\":" << ts(f->end - f->begin) << "}";
        for (std::uint16_t t = 0; t < names.size(); ++t)
            for (const auto& b : flame(*f, t)) {
                comma();
                out << "{\"ph\":\"X\",\"pid\":1,\"tid\":" << t << ",\"name\":\"" << escape(b.name) << "\",\"ts\":"
                    << ts(b.begin) << ",\"dur\":" << ts(b.end - b.begin) << "}";
            }
        comma();
        out << "{\"ph\":\"C\",\"pid\":2,\"name\":\"memory\",\"ts\":" << ts(f->end) << ",\"args\":{\"heap MB\":"
            << double(f->liveBytes) / 1048576.0 << ",\"resident MB\":" << double(f->residentBytes) / 1048576.0 << "}}";
        comma();
        out << "{\"ph\":\"C\",\"pid\":2,\"name\":\"allocations\",\"ts\":" << ts(f->end) << ",\"args\":{\"count\":"
            << f->allocations << ",\"KB\":" << double(f->allocatedBytes) / 1024.0 << "}}";
    }
    out << "\n]}\n";
    return bool(out);
}

} // namespace engine::profile
