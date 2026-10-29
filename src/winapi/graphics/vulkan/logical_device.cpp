// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/vulkan/vulkan.hpp"

#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_texture.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstring>
#include <string>

/// @file logical_device.cpp
/// @brief Logical device
///
/// Closed cut from vulkan.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Logical device
// =============================================================================

void VulkanManager::impl_vkCreateDevice(Marshaler& m) {
    // VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*,
    //                         const VkAllocationCallbacks*, VkDevice*)
    const GuestAddr physical_device = m.arg_ptr(0);
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_device = m.arg_ptr(3);
    if (out_device == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_ptr(out_device, 0);

    if (static_cast<u64>(physical_device) != PHYSICAL_DEVICE_HANDLE) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // A device that requests extensions we do not implement must be refused
    // here rather than created and failed later. VkDeviceCreateInfo:
    // sType, pNext, flags, queueCreateInfoCount, pQueueCreateInfos,
    // enabledLayerCount, ppEnabledLayerNames, enabledExtensionCount, ...
    // On 32-bit that puts enabledExtensionCount at +28; on 64-bit at +48.
    if (create_info != 0) {
        const u32 ext_offset = m.cpu_is_64bit() ? 48u : 28u;
        const auto ext_count = m.memory().read_u32(create_info + ext_offset);
        if (ext_count.is_ok() && ext_count.value() != 0) {
            VITREO_TRACE("[vulkan] vkCreateDevice requested %u device extension(s); "
                         "none are supported -> VK_ERROR_EXTENSION_NOT_PRESENT\n",
                         ext_count.value());
            m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_EXTENSION_NOT_PRESENT)));
            return;
        }
    }

    // Metal's command queue already exists for the process; a VkDevice is a
    // logical handle onto it rather than a separate allocation.
    if (graphics::MetalDevice::instance().command_queue_ptr() == nullptr) {
        VITREO_TRACE("[vulkan] vkCreateDevice: no Metal command queue available\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    DeviceState device;
    device.physical_device = PHYSICAL_DEVICE_HANDLE;
    device.queue_handle = allocate_handle();
    const u64 device_handle = allocate_handle();
    m_devices.emplace(device_handle, device);

    (void)m.write_ptr(out_device, device_handle);
    VITREO_TRACE("[vulkan] vkCreateDevice -> VK_SUCCESS (device=0x%llx)\n",
                 static_cast<unsigned long long>(device_handle));
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyDevice(Marshaler& m) {
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    // Command buffers and pools outliving their device would leave dangling
    // parents; Vulkan requires the app to have destroyed them, but dropping
    // them here keeps our own tables consistent if it did not.
    for (auto it = m_command_buffers.begin(); it != m_command_buffers.end();) {
        it = it->second.device == device ? m_command_buffers.erase(it) : std::next(it);
    }
    for (auto it = m_command_pools.begin(); it != m_command_pools.end();) {
        it = it->second.device == device ? m_command_pools.erase(it) : std::next(it);
    }
    for (auto it = m_render_passes.begin(); it != m_render_passes.end();) {
        it = it->second.device == device ? m_render_passes.erase(it) : std::next(it);
    }
    for (auto it = m_shader_modules.begin(); it != m_shader_modules.end();) {
        it = it->second.device == device ? m_shader_modules.erase(it) : std::next(it);
    }
    for (auto it = m_pipeline_layouts.begin(); it != m_pipeline_layouts.end();) {
        it = it->second.device == device ? m_pipeline_layouts.erase(it) : std::next(it);
    }
    for (auto it = m_pipelines.begin(); it != m_pipelines.end();) {
        it = it->second.device == device ? m_pipelines.erase(it) : std::next(it);
    }
    for (auto it = m_pipeline_caches.begin(); it != m_pipeline_caches.end();) {
        it = it->second.device == device ? m_pipeline_caches.erase(it) : std::next(it);
    }
    for (auto it = m_framebuffers.begin(); it != m_framebuffers.end();) {
        it = it->second.device == device ? m_framebuffers.erase(it) : std::next(it);
    }
    m_devices.erase(device);
}

void VulkanManager::impl_vkGetDeviceQueue(Marshaler& m) {
    // void vkGetDeviceQueue(VkDevice, uint32_t queueFamilyIndex,
    //                       uint32_t queueIndex, VkQueue* pQueue)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const u32 family = static_cast<u32>(m.arg(1));
    const u32 index = static_cast<u32>(m.arg(2));
    const GuestAddr out_queue = m.arg_ptr(3);
    if (out_queue == 0) return;
    (void)m.write_ptr(out_queue, 0);

    auto it = m_devices.find(device);
    // Only family 0, queue 0 exists - that is what the queue family properties
    // advertise, so anything else is the caller ignoring its own query.
    if (it == m_devices.end() || family != 0 || index != 0) {
        VITREO_TRACE("[vulkan] vkGetDeviceQueue(family=%u index=%u) -> null\n",
                     family, index);
        return;
    }
    (void)m.write_ptr(out_queue, it->second.queue_handle);
}

void VulkanManager::impl_vkDeviceWaitIdle(Marshaler& m) {
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    // Nothing is queued that is not already committed and waited on at submit
    // time, so idle is immediate. This becomes real work once submissions
    // carry recorded commands.
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkQueueWaitIdle(Marshaler& m) {
    (void)m.arg(0);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkQueueSubmit(Marshaler& m) {
    // VkResult vkQueueSubmit(VkQueue, uint32_t submitCount,
    //                        const VkSubmitInfo*, VkFence)
    const u32 submit_count = static_cast<u32>(m.arg(1));

    // Command buffers carry no recorded work yet - render passes, pipelines
    // and draws are not translated - so a submission is genuinely empty. An
    // empty command buffer is a legal Vulkan submission, and committing one to
    // Metal is the truthful equivalent rather than a no-op pretending to be a
    // submission.
    graphics::MetalCommandRecorder recorder(
        graphics::MetalDevice::instance().command_queue_ptr());
    for (u32 index = 0; index < submit_count; ++index) {
        recorder.begin();
        recorder.commit_and_wait();
    }
    VITREO_TRACE("[vulkan] vkQueueSubmit submitCount=%u (empty command buffers)\n",
                 submit_count);
    m.set_return(VK_SUCCESS);
}

} // namespace vitreo::winapi
