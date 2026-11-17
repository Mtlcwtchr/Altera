// altera_gpu_layer - a Vulkan layer that times the GPU work of any Vulkan
// program, by itself: no markers in the program, no vendor counters, no admin.
//
// It sits between the program and the driver (enabled by environment, see
// tools/gpu_profile.py) and writes a timestamp query before each piece of
// work - a run of draws with one pipeline, a dispatch, a copy, a render pass's
// load and store - into the program's own command buffers. Results are read
// back frames later, without waiting. A segment is named after what it ran:
// the shader entry points of the bound pipeline ("AdaptiveTerrainVS /
// TerrainPagePS"), read from vkCreateGraphicsPipelines, so time lands on
// shaders the way a GPU profiler shows it.
//
// Timestamps are written at the bottom of the pipe: a segment is the time from
// the previous piece of work finishing to this one finishing. Overlap between
// neighbours on the card makes that an attribution, not a stopwatch - the same
// approximation every timestamp profiler makes - and frame totals are exact.
//
// Environment:
//   ALTERA_GPU_PROFILE_OUT=file.json   where the report goes (at device destruction)
//   ALTERA_GPU_PROFILE_LIVE=1          a one-line summary on stderr every second
#define VK_NO_PROTOTYPES
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define ALTERA_EXPORT extern "C" __declspec(dllexport)
#else
#define ALTERA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr const char* kLayerName = "VK_LAYER_ALTERA_gpu_profile";
constexpr std::uint32_t kSliceSize = 1024;   // timestamps a command buffer can hold
constexpr std::uint32_t kSlices = 128;       // command buffers measured at once

using Clock = std::chrono::steady_clock;

void* key(const void* dispatchable) { return *static_cast<void* const*>(dispatchable); }

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
    PFN_vkGetPhysicalDeviceProperties properties = nullptr;
};

struct Pending {
    std::uint32_t slice = 0;
    std::uint32_t used = 0;
    std::vector<std::string> labels;
    std::uint64_t frame = 0;
};

struct FrameStats {
    std::map<std::string, double> millis;
    double total = 0;
    int buffers = 0;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr next = nullptr;
    double period = 1.0;   // ns per tick
    VkQueryPool pool = VK_NULL_HANDLE;
    std::vector<bool> busy = std::vector<bool>(kSlices, false);

    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkCreateQueryPool createQueryPool = nullptr;
    PFN_vkDestroyQueryPool destroyQueryPool = nullptr;
    PFN_vkGetQueryPoolResults getQueryPoolResults = nullptr;
    PFN_vkCmdResetQueryPool cmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp cmdWriteTimestamp = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdBeginRenderPass cmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass cmdEndRenderPass = nullptr;
    PFN_vkCmdDraw cmdDraw = nullptr;
    PFN_vkCmdDrawIndexed cmdDrawIndexed = nullptr;
    PFN_vkCmdDrawIndirect cmdDrawIndirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect cmdDrawIndexedIndirect = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkCmdDispatchIndirect cmdDispatchIndirect = nullptr;
    PFN_vkCmdCopyBuffer cmdCopyBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkCmdBlitImage cmdBlitImage = nullptr;
    PFN_vkCmdClearColorImage cmdClearColorImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueuePresentKHR queuePresent = nullptr;

    std::unordered_map<VkShaderModule, std::vector<std::string>> moduleEntries;
    std::unordered_map<VkPipeline, std::string> pipelineNames;
    std::deque<Pending> pending;
    std::map<std::uint64_t, FrameStats> open;   // frames whose buffers are still coming in
    std::vector<FrameStats> history;
    std::uint64_t frame = 0;
    Clock::time_point lastPresent{};
    bool presented = false;
    Clock::time_point lastLive = Clock::now();
};

struct CommandBufferState {
    DeviceData* device = nullptr;
    int slice = -1;
    std::uint32_t used = 0;
    std::vector<std::string> labels;
    std::string graphics = "draw", compute = "dispatch";
    bool renderPass = false;
};

std::mutex gLock;
std::unordered_map<void*, InstanceData> gInstances;          // by dispatch key
std::unordered_map<void*, DeviceData*> gDevices;             // by dispatch key
std::unordered_map<VkCommandBuffer, CommandBufferState> gBuffers;

DeviceData* deviceOf(const void* dispatchable) {
    const auto found = gDevices.find(key(dispatchable));
    return found == gDevices.end() ? nullptr : found->second;
}

// Entry point names from SPIR-V (OpEntryPoint), for pipelines whose stage
// names say "main".
std::vector<std::string> entryPoints(const std::uint32_t* code, std::size_t bytes) {
    std::vector<std::string> names;
    const std::size_t words = bytes / 4;
    if (words < 5 || code[0] != 0x07230203u) return names;
    for (std::size_t at = 5; at < words;) {
        const std::uint32_t op = code[at] & 0xffffu, count = code[at] >> 16;
        if (count == 0) break;
        if (op == 15 && count > 3) names.emplace_back(reinterpret_cast<const char*>(&code[at + 3]));
        at += count;
    }
    return names;
}

std::string stageName(DeviceData& d, const VkPipelineShaderStageCreateInfo& stage) {
    std::string name = stage.pName ? stage.pName : "?";
    if (name == "main") {
        const auto found = d.moduleEntries.find(stage.module);
        if (found != d.moduleEntries.end() && !found->second.empty()) name = found->second.front();
    }
    return name;
}

void mark(CommandBufferState& cb, VkCommandBuffer buffer, const std::string& label) {
    if (cb.slice < 0 || cb.used + 1 >= kSliceSize) return;
    if (!cb.labels.empty() && cb.labels.back() == label) return;   // the same piece of work goes on
    DeviceData& d = *cb.device;
    d.cmdWriteTimestamp(buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.pool,
                        std::uint32_t(cb.slice) * kSliceSize + cb.used);
    cb.labels.push_back(label);
    ++cb.used;
}

void markIn(VkCommandBuffer buffer, const char* what, bool compute = false) {
    const auto found = gBuffers.find(buffer);
    if (found == gBuffers.end()) return;
    CommandBufferState& cb = found->second;
    if (what) mark(cb, buffer, what);
    else mark(cb, buffer, compute ? cb.compute : cb.graphics);
}

void finishFrames(DeviceData& d, bool all) {
    const char* live = std::getenv("ALTERA_GPU_PROFILE_LIVE") ? std::getenv("ALTERA_GPU_PROFILE_LIVE") : std::getenv("CAMPFIRE_GPU_PROFILE_LIVE");
    while (!d.open.empty() && (all || d.open.begin()->first + 12 < d.frame)) {
        FrameStats stats = std::move(d.open.begin()->second);
        d.open.erase(d.open.begin());
        for (const auto& [label, ms] : stats.millis) stats.total += ms;
        d.history.push_back(std::move(stats));
        if (d.history.size() > 20000) d.history.erase(d.history.begin(), d.history.begin() + 5000);
    }
    if (live && *live == '1' && Clock::now() - d.lastLive > std::chrono::seconds(1) && !d.history.empty()) {
        d.lastLive = Clock::now();
        // The last second's frames, averaged.
        std::map<std::string, double> sum;
        double total = 0;
        int frames = 0;
        for (auto it = d.history.rbegin(); it != d.history.rend() && frames < 120; ++it, ++frames) {
            total += it->total;
            for (const auto& [label, ms] : it->millis) sum[label] += ms;
        }
        std::vector<std::pair<double, std::string>> top;
        for (const auto& [label, ms] : sum) top.emplace_back(ms / frames, label);
        std::sort(top.rbegin(), top.rend());
        std::fprintf(stderr, "[gpu] %.2f ms/frame:", total / frames);
        for (std::size_t i = 0; i < top.size() && i < 6; ++i)
            std::fprintf(stderr, "  %s %.2f", top[i].second.c_str(), top[i].first);
        std::fprintf(stderr, "\n");
    }
}

void collect(DeviceData& d) {
    std::vector<std::uint64_t> ticks(kSliceSize);
    for (auto it = d.pending.begin(); it != d.pending.end();) {
        const VkResult result = d.getQueryPoolResults(d.device, d.pool, it->slice * kSliceSize, it->used,
                                                      it->used * sizeof(std::uint64_t), ticks.data(),
                                                      sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result == VK_NOT_READY && it->frame + 64 > d.frame) { ++it; continue; }
        if (result == VK_SUCCESS) {
            FrameStats& stats = d.open[it->frame];
            ++stats.buffers;
            for (std::uint32_t i = 0; i + 1 < it->used; ++i)
                if (ticks[i + 1] >= ticks[i])
                    stats.millis[it->labels[i]] += double(ticks[i + 1] - ticks[i]) * d.period * 1e-6;
        }
        d.busy[it->slice] = false;
        it = d.pending.erase(it);
    }
    finishFrames(d, false);
}

void writeReport(DeviceData& d) {
    finishFrames(d, true);
    const char* path = std::getenv("ALTERA_GPU_PROFILE_OUT") ? std::getenv("ALTERA_GPU_PROFILE_OUT") : std::getenv("CAMPFIRE_GPU_PROFILE_OUT");
    std::ofstream out(path && *path ? path : "altera_gpu_profile.json", std::ios::trunc);
    if (!out) return;
    const auto escape = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if (std::uint8_t(c) >= 0x20) o += c;
        }
        return o;
    };
    out << "{\"layer\":\"" << kLayerName << "\",\"period_ns\":" << d.period << ",\"frames\":[\n";
    for (std::size_t i = 0; i < d.history.size(); ++i) {
        const FrameStats& f = d.history[i];
        out << (i ? ",\n" : "") << "{\"total_ms\":" << f.total << ",\"buffers\":" << f.buffers << ",\"ms\":{";
        bool first = true;
        for (const auto& [label, ms] : f.millis) {
            out << (first ? "" : ",") << "\"" << escape(label) << "\":" << ms;
            first = false;
        }
        out << "}}";
    }
    out << "\n]}\n";
}

// --- hooks: command buffers ----------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
    std::lock_guard<std::mutex> lock(gLock);
    DeviceData* d = deviceOf(buffer);
    const VkResult result = d->beginCommandBuffer(buffer, info);
    if (result != VK_SUCCESS) return result;
    CommandBufferState& cb = gBuffers[buffer];
    if (cb.slice >= 0) d->busy[cb.slice] = false;   // recorded again without being submitted
    cb = CommandBufferState{};
    cb.device = d;
    if (d->pool == VK_NULL_HANDLE) return result;
    for (std::uint32_t s = 0; s < kSlices; ++s)
        if (!d->busy[s]) { d->busy[s] = true; cb.slice = int(s); break; }
    if (cb.slice < 0) return result;   // every slice out: this one goes unmeasured
    d->cmdResetQueryPool(buffer, d->pool, std::uint32_t(cb.slice) * kSliceSize, kSliceSize);
    mark(cb, buffer, "command buffer start");
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer buffer) {
    std::lock_guard<std::mutex> lock(gLock);
    DeviceData* d = deviceOf(buffer);
    const auto found = gBuffers.find(buffer);
    if (found != gBuffers.end() && found->second.slice >= 0) {
        CommandBufferState& cb = found->second;
        if (cb.used < kSliceSize) {
            d->cmdWriteTimestamp(buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d->pool,
                                 std::uint32_t(cb.slice) * kSliceSize + cb.used);
            cb.labels.push_back("end");
            ++cb.used;
        }
    }
    return d->endCommandBuffer(buffer);
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer buffer, VkPipelineBindPoint point, VkPipeline pipeline) {
    std::lock_guard<std::mutex> lock(gLock);
    DeviceData* d = deviceOf(buffer);
    const auto found = gBuffers.find(buffer);
    if (found != gBuffers.end()) {
        const auto name = d->pipelineNames.find(pipeline);
        const std::string label = name != d->pipelineNames.end() ? name->second : "pipeline";
        if (point == VK_PIPELINE_BIND_POINT_COMPUTE) found->second.compute = "compute " + label;
        else found->second.graphics = label;
    }
    d->cmdBindPipeline(buffer, point, pipeline);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info,
                                              VkSubpassContents contents) {
    std::lock_guard<std::mutex> lock(gLock);
    markIn(buffer, "render pass load/clear");
    if (const auto found = gBuffers.find(buffer); found != gBuffers.end()) found->second.renderPass = true;
    deviceOf(buffer)->cmdBeginRenderPass(buffer, info, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer buffer) {
    std::lock_guard<std::mutex> lock(gLock);
    markIn(buffer, "render pass store/resolve");
    deviceOf(buffer)->cmdEndRenderPass(buffer);
}

#define ALTERA_DRAW(Name, Args, Call, What, Compute)               \
    VKAPI_ATTR void VKAPI_CALL Name Args {                            \
        std::lock_guard<std::mutex> lock(gLock);                      \
        markIn(buffer, What, Compute);                                \
        deviceOf(buffer)->Call;                                       \
    }

ALTERA_DRAW(CmdDraw, (VkCommandBuffer buffer, uint32_t a, uint32_t b, uint32_t c, uint32_t e),
              cmdDraw(buffer, a, b, c, e), nullptr, false)
ALTERA_DRAW(CmdDrawIndexed, (VkCommandBuffer buffer, uint32_t a, uint32_t b, uint32_t c, int32_t e, uint32_t f),
              cmdDrawIndexed(buffer, a, b, c, e, f), nullptr, false)
ALTERA_DRAW(CmdDrawIndirect, (VkCommandBuffer buffer, VkBuffer a, VkDeviceSize b, uint32_t c, uint32_t e),
              cmdDrawIndirect(buffer, a, b, c, e), nullptr, false)
ALTERA_DRAW(CmdDrawIndexedIndirect, (VkCommandBuffer buffer, VkBuffer a, VkDeviceSize b, uint32_t c, uint32_t e),
              cmdDrawIndexedIndirect(buffer, a, b, c, e), nullptr, false)
ALTERA_DRAW(CmdDispatch, (VkCommandBuffer buffer, uint32_t a, uint32_t b, uint32_t c),
              cmdDispatch(buffer, a, b, c), nullptr, true)
ALTERA_DRAW(CmdDispatchIndirect, (VkCommandBuffer buffer, VkBuffer a, VkDeviceSize b),
              cmdDispatchIndirect(buffer, a, b), nullptr, true)
ALTERA_DRAW(CmdCopyBuffer, (VkCommandBuffer buffer, VkBuffer a, VkBuffer b, uint32_t c, const VkBufferCopy* e),
              cmdCopyBuffer(buffer, a, b, c, e), "copy buffer", false)
ALTERA_DRAW(CmdCopyBufferToImage,
              (VkCommandBuffer buffer, VkBuffer a, VkImage b, VkImageLayout c, uint32_t e, const VkBufferImageCopy* f),
              cmdCopyBufferToImage(buffer, a, b, c, e, f), "upload image", false)
ALTERA_DRAW(CmdCopyImage,
              (VkCommandBuffer buffer, VkImage a, VkImageLayout b, VkImage c, VkImageLayout e, uint32_t f,
               const VkImageCopy* g),
              cmdCopyImage(buffer, a, b, c, e, f, g), "copy image", false)
ALTERA_DRAW(CmdCopyImageToBuffer,
              (VkCommandBuffer buffer, VkImage a, VkImageLayout b, VkBuffer c, uint32_t e, const VkBufferImageCopy* f),
              cmdCopyImageToBuffer(buffer, a, b, c, e, f), "read back image", false)
ALTERA_DRAW(CmdBlitImage,
              (VkCommandBuffer buffer, VkImage a, VkImageLayout b, VkImage c, VkImageLayout e, uint32_t f,
               const VkImageBlit* g, VkFilter h),
              cmdBlitImage(buffer, a, b, c, e, f, g, h), "blit / mipmaps", false)
ALTERA_DRAW(CmdClearColorImage,
              (VkCommandBuffer buffer, VkImage a, VkImageLayout b, const VkClearColorValue* c, uint32_t e,
               const VkImageSubresourceRange* f),
              cmdClearColorImage(buffer, a, b, c, e, f), "clear image", false)

// --- hooks: pipelines and shaders (names) --------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
                                                  const VkAllocationCallbacks* allocator, VkShaderModule* module) {
    DeviceData* d;
    { std::lock_guard<std::mutex> lock(gLock); d = deviceOf(device); }
    const VkResult result = d->createShaderModule(device, info, allocator, module);
    if (result == VK_SUCCESS) {
        auto names = entryPoints(info->pCode, info->codeSize);
        std::lock_guard<std::mutex> lock(gLock);
        d->moduleEntries[*module] = std::move(names);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo* infos,
                                                       const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    DeviceData* d;
    { std::lock_guard<std::mutex> lock(gLock); d = deviceOf(device); }
    const VkResult result = d->createGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
    std::lock_guard<std::mutex> lock(gLock);
    for (uint32_t i = 0; i < count; ++i) {
        if (pipelines[i] == VK_NULL_HANDLE) continue;
        std::string vertex, fragment;
        for (uint32_t s = 0; s < infos[i].stageCount; ++s) {
            const auto& stage = infos[i].pStages[s];
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) vertex = stageName(*d, stage);
            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) fragment = stageName(*d, stage);
        }
        d->pipelineNames[pipelines[i]] = vertex + (fragment.empty() ? "" : " / " + fragment);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo* infos,
                                                      const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    DeviceData* d;
    { std::lock_guard<std::mutex> lock(gLock); d = deviceOf(device); }
    const VkResult result = d->createComputePipelines(device, cache, count, infos, allocator, pipelines);
    std::lock_guard<std::mutex> lock(gLock);
    for (uint32_t i = 0; i < count; ++i)
        if (pipelines[i] != VK_NULL_HANDLE) d->pipelineNames[pipelines[i]] = stageName(*d, infos[i].stage);
    return result;
}

// --- hooks: queues (what went to the card, and when a frame ends) --------------

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    std::lock_guard<std::mutex> lock(gLock);
    DeviceData* d = deviceOf(queue);
    bool drew = false;
    for (uint32_t s = 0; s < count; ++s)
        for (uint32_t b = 0; b < submits[s].commandBufferCount; ++b) {
            const auto found = gBuffers.find(submits[s].pCommandBuffers[b]);
            if (found == gBuffers.end()) continue;
            CommandBufferState& cb = found->second;
            drew = drew || cb.renderPass;
            if (cb.slice < 0) continue;
            d->pending.push_back({std::uint32_t(cb.slice), cb.used, std::move(cb.labels), d->frame});
            cb.slice = -1;   // the slice belongs to the pending readback now
            cb.labels.clear();
        }
    const VkResult result = d->queueSubmit(queue, count, submits, fence);
    // A program that never presents (an offscreen run) has no frame boundary
    // of its own: there, a submission that drew something ends a frame.
    if (drew && (!d->presented || Clock::now() - d->lastPresent > std::chrono::seconds(2))) ++d->frame;
    // Finished work is read back at every submission, not only at frame ends:
    // a program that loads for a while submits hundreds of buffers first.
    collect(*d);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceData* d;
    {
        std::lock_guard<std::mutex> lock(gLock);
        d = deviceOf(queue);
        d->presented = true;
        d->lastPresent = Clock::now();
        ++d->frame;
        collect(*d);
    }
    return d->queuePresent(queue, info);
}

// --- hooks: instance and device --------------------------------------------------

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name);

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                              VkInstance* instance) {
    auto* chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS) return result;
    InstanceData data;
    data.instance = *instance;
    data.next = next;
    data.destroy = reinterpret_cast<PFN_vkDestroyInstance>(next(*instance, "vkDestroyInstance"));
    data.properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(next(*instance, "vkGetPhysicalDeviceProperties"));
    std::lock_guard<std::mutex> lock(gLock);
    gInstances[key(*instance)] = data;
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    PFN_vkDestroyInstance destroy;
    {
        std::lock_guard<std::mutex> lock(gLock);
        destroy = gInstances[key(instance)].destroy;
        gInstances.erase(key(instance));
    }
    destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr nextInstance = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr nextDevice = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    InstanceData instance;
    {
        std::lock_guard<std::mutex> lock(gLock);
        instance = gInstances[key(physical)];
    }
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(nextInstance(instance.instance, "vkCreateDevice"));
    const VkResult result = create(physical, info, allocator, device);
    if (result != VK_SUCCESS) return result;

    auto* d = new DeviceData();
    d->device = *device;
    d->next = nextDevice;
    VkPhysicalDeviceProperties properties{};
    if (instance.properties) instance.properties(physical, &properties);
    d->period = properties.limits.timestampPeriod > 0 ? double(properties.limits.timestampPeriod) : 1.0;
#define ALTERA_LOAD(member, name) d->member = reinterpret_cast<PFN_##name>(nextDevice(*device, #name))
    ALTERA_LOAD(destroyDevice, vkDestroyDevice);
    ALTERA_LOAD(createQueryPool, vkCreateQueryPool);
    ALTERA_LOAD(destroyQueryPool, vkDestroyQueryPool);
    ALTERA_LOAD(getQueryPoolResults, vkGetQueryPoolResults);
    ALTERA_LOAD(cmdResetQueryPool, vkCmdResetQueryPool);
    ALTERA_LOAD(cmdWriteTimestamp, vkCmdWriteTimestamp);
    ALTERA_LOAD(createShaderModule, vkCreateShaderModule);
    ALTERA_LOAD(createGraphicsPipelines, vkCreateGraphicsPipelines);
    ALTERA_LOAD(createComputePipelines, vkCreateComputePipelines);
    ALTERA_LOAD(beginCommandBuffer, vkBeginCommandBuffer);
    ALTERA_LOAD(endCommandBuffer, vkEndCommandBuffer);
    ALTERA_LOAD(cmdBindPipeline, vkCmdBindPipeline);
    ALTERA_LOAD(cmdBeginRenderPass, vkCmdBeginRenderPass);
    ALTERA_LOAD(cmdEndRenderPass, vkCmdEndRenderPass);
    ALTERA_LOAD(cmdDraw, vkCmdDraw);
    ALTERA_LOAD(cmdDrawIndexed, vkCmdDrawIndexed);
    ALTERA_LOAD(cmdDrawIndirect, vkCmdDrawIndirect);
    ALTERA_LOAD(cmdDrawIndexedIndirect, vkCmdDrawIndexedIndirect);
    ALTERA_LOAD(cmdDispatch, vkCmdDispatch);
    ALTERA_LOAD(cmdDispatchIndirect, vkCmdDispatchIndirect);
    ALTERA_LOAD(cmdCopyBuffer, vkCmdCopyBuffer);
    ALTERA_LOAD(cmdCopyBufferToImage, vkCmdCopyBufferToImage);
    ALTERA_LOAD(cmdCopyImage, vkCmdCopyImage);
    ALTERA_LOAD(cmdCopyImageToBuffer, vkCmdCopyImageToBuffer);
    ALTERA_LOAD(cmdBlitImage, vkCmdBlitImage);
    ALTERA_LOAD(cmdClearColorImage, vkCmdClearColorImage);
    ALTERA_LOAD(queueSubmit, vkQueueSubmit);
    ALTERA_LOAD(queuePresent, vkQueuePresentKHR);
#undef ALTERA_LOAD
    VkQueryPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    pool.queryType = VK_QUERY_TYPE_TIMESTAMP;
    pool.queryCount = kSliceSize * kSlices;
    if (properties.limits.timestampComputeAndGraphics &&
        d->createQueryPool(*device, &pool, nullptr, &d->pool) != VK_SUCCESS)
        d->pool = VK_NULL_HANDLE;
    std::fprintf(stderr, "[gpu] %s on %s: %s, %.3f ns a tick\n", kLayerName, properties.deviceName,
                 d->pool ? "timing every command buffer" : "no timestamps on this device", d->period);
    std::lock_guard<std::mutex> lock(gLock);
    gDevices[key(*device)] = d;
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    DeviceData* d;
    {
        std::lock_guard<std::mutex> lock(gLock);
        d = deviceOf(device);
        gDevices.erase(key(device));
        for (auto it = gBuffers.begin(); it != gBuffers.end();)
            it = it->second.device == d ? gBuffers.erase(it) : std::next(it);
    }
    if (!d) return;
    collect(*d);
    writeReport(*d);
    if (d->pool) d->destroyQueryPool(device, d->pool, nullptr);
    const PFN_vkDestroyDevice destroy = d->destroyDevice;
    delete d;
    destroy(device, allocator);
}

PFN_vkVoidFunction hook(const char* name) {
#define ALTERA_HOOK(fn, ours) if (std::strcmp(name, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ours)
    ALTERA_HOOK(vkGetDeviceProcAddr, GetDeviceProcAddr);
    ALTERA_HOOK(vkDestroyDevice, DestroyDevice);
    ALTERA_HOOK(vkCreateShaderModule, CreateShaderModule);
    ALTERA_HOOK(vkCreateGraphicsPipelines, CreateGraphicsPipelines);
    ALTERA_HOOK(vkCreateComputePipelines, CreateComputePipelines);
    ALTERA_HOOK(vkBeginCommandBuffer, BeginCommandBuffer);
    ALTERA_HOOK(vkEndCommandBuffer, EndCommandBuffer);
    ALTERA_HOOK(vkCmdBindPipeline, CmdBindPipeline);
    ALTERA_HOOK(vkCmdBeginRenderPass, CmdBeginRenderPass);
    ALTERA_HOOK(vkCmdEndRenderPass, CmdEndRenderPass);
    ALTERA_HOOK(vkCmdDraw, CmdDraw);
    ALTERA_HOOK(vkCmdDrawIndexed, CmdDrawIndexed);
    ALTERA_HOOK(vkCmdDrawIndirect, CmdDrawIndirect);
    ALTERA_HOOK(vkCmdDrawIndexedIndirect, CmdDrawIndexedIndirect);
    ALTERA_HOOK(vkCmdDispatch, CmdDispatch);
    ALTERA_HOOK(vkCmdDispatchIndirect, CmdDispatchIndirect);
    ALTERA_HOOK(vkCmdCopyBuffer, CmdCopyBuffer);
    ALTERA_HOOK(vkCmdCopyBufferToImage, CmdCopyBufferToImage);
    ALTERA_HOOK(vkCmdCopyImage, CmdCopyImage);
    ALTERA_HOOK(vkCmdCopyImageToBuffer, CmdCopyImageToBuffer);
    ALTERA_HOOK(vkCmdBlitImage, CmdBlitImage);
    ALTERA_HOOK(vkCmdClearColorImage, CmdClearColorImage);
    ALTERA_HOOK(vkQueueSubmit, QueueSubmit);
    ALTERA_HOOK(vkQueuePresentKHR, QueuePresentKHR);
#undef ALTERA_HOOK
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    if (PFN_vkVoidFunction ours = hook(name)) return ours;
    PFN_vkGetDeviceProcAddr next;
    {
        std::lock_guard<std::mutex> lock(gLock);
        DeviceData* d = deviceOf(device);
        if (!d) return nullptr;
        next = d->next;
    }
    return next(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&GetInstanceProcAddr);
    if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
    if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance);
    if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
    if (PFN_vkVoidFunction ours = hook(name)) return ours;
    if (!instance) return nullptr;
    PFN_vkGetInstanceProcAddr next;
    {
        std::lock_guard<std::mutex> lock(gLock);
        const auto found = gInstances.find(key(instance));
        if (found == gInstances.end()) return nullptr;
        next = found->second.next;
    }
    return next(instance, name);
}

} // namespace

ALTERA_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (version->loaderLayerInterfaceVersion >= 2) {
        version->pfnGetInstanceProcAddr = GetInstanceProcAddr;
        version->pfnGetDeviceProcAddr = GetDeviceProcAddr;
        version->pfnGetPhysicalDeviceProcAddr = nullptr;
        version->loaderLayerInterfaceVersion = 2;
    }
    return VK_SUCCESS;
}

ALTERA_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    return GetInstanceProcAddr(instance, name);
}

ALTERA_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    return GetDeviceProcAddr(device, name);
}
