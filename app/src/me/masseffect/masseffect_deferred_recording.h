// Deferred Vulkan command recording (masseffect_native_deferred_recording).
//
// The ring thread translates PM4 and records Vulkan commands; recording in NVK (state flushes, descriptor
// and constant buffer pushes, pushbuffer writes) was ~15 % of that thread. With this on, the ring thread
// only queues each vkCmd* call (arguments copied, arrays into an arena) and a worker thread replays the
// queue into the same command buffers, in the same order. The ring thread waits for the queue to empty
// before anything that needs the recorded state: beginning, ending or resetting a command buffer or pool,
// submitting, updating descriptor sets, destroying or freeing any object. The image is unchanged: the same
// commands reach the driver in the same order.
#pragma once

#include <rex/ui/vulkan/device.h>

namespace masseffect::native::deferred {

using Functions = rex::ui::vulkan::VulkanDevice::Functions;

// The function table the renderer uses: the real one when deferral is off, the proxy when it is on.
const Functions& Table(const Functions& real_fns);

// vkGetDeviceProcAddr through the proxy: vkCmd* functions the renderer fetches by name get wrappers too.
PFN_vkVoidFunction Proc(PFN_vkGetDeviceProcAddr real, VkDevice device, const char* name);

// Ring thread: waits until the worker has recorded everything queued (no-op when off).
void Drain();

// Ring thread, banjo-nx (the in-run A/B, banjo_ab_target = 16): true = the ring thread makes every call itself from
// here on, as with the layer off (the queue is emptied first); false = queued again. A no-op when the layer is off.
void SetDirect(bool direct);

// Ring thread: tells the worker about commands the batched mode (masseffect_deferred_native_fast) still holds
// back. Call before the ring thread idles; a no-op otherwise.
void Flush();

// banjo-nx (2026-10-16, banjo_deferred_packets): the draw path's per-draw commands, appended as packed records to
// one queue slot that holds many draws. Each returns false when the layer does not take it (off, packets off, not the
// ring thread, the in-run A/B's direct half): the caller then calls the Vulkan function itself through the table, as
// before. Always graphics bind point; viewport / scissor are slot 0 only; the stencil record sets reference, compare
// mask and write mask for both faces from front / back = reference | compare << 8 | write << 16.
bool PacketBindPipeline(VkCommandBuffer cb, VkPipeline pipeline);
bool PacketBindSets(VkCommandBuffer cb, VkPipelineLayout layout, uint32_t first, uint32_t n,
                    const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dynamic_offsets);
bool PacketPush(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                uint32_t size, const void* data);
bool PacketViewport(VkCommandBuffer cb, const VkViewport& v);
bool PacketScissor(VkCommandBuffer cb, const VkRect2D& r);
bool PacketBlend(VkCommandBuffer cb, const float k[4]);
bool PacketBias(VkCommandBuffer cb, float constant, float clamp, float slope);
bool PacketStencil(VkCommandBuffer cb, uint32_t front, uint32_t back);
bool PacketVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* buffers,
                         const VkDeviceSize* offsets);
bool PacketIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
bool PacketDrawIndexed(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, int32_t vertex_offset,
                       uint32_t first_instance);
bool PacketDraw(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance);

// banjo_diag_vk_calls: logs the Vulkan commands, barriers and render passes recorded since the last call, per
// Swap (the renderer's 10 s report calls it with the Swaps of the interval). Nothing when the key is off.
void ReportCalls(uint64_t swaps);

}  // namespace masseffect::native::deferred
