// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file vulkan.hpp
/// @brief vulkan-1.dll loader and instance/physical-device layer.
///
/// On Windows `vulkan-1.dll` is the Khronos loader, shipped by the GPU driver
/// rather than by the application. Guests reach it two ways: a link-time import,
/// or - far more commonly - `LoadLibrary("vulkan-1.dll")` followed by
/// `GetProcAddress("vkGetInstanceProcAddr")` and dynamic resolution of
/// everything else through that one entry point. A dynamic loader takes the
/// second path.
///
/// Scope: this layer reports what Vitreo can actually back. The physical device
/// is the real Metal GPU, with its real name and unified-memory topology.
/// Instance/device enumeration, one Metal-backed queue, command-buffer
/// lifecycle, memory, buffers, a limited image path, render-pass objects,
/// and opaque shader-module / pipeline-layout / pipeline handles are
/// implemented.
/// Formats, extensions, sparse resources, descriptors, draw commands,
/// surfaces, and presentation remain unavailable unless explicitly
/// implemented; their probes return null, zero capabilities, or the relevant
/// Vulkan failure instead of claiming support.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <memory>
#include <unordered_map>

namespace vitreo { class GuestMemory; }
namespace vitreo::graphics {
class MetalBuffer;
class MetalTexture;
} // namespace vitreo::graphics

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// Vulkan constants
// =============================================================================

constexpr u32 VK_SUCCESS = 0;
constexpr u32 VK_INCOMPLETE = 5;
constexpr i32 VK_ERROR_OUT_OF_HOST_MEMORY = -1;
constexpr i32 VK_ERROR_INITIALIZATION_FAILED = -3;
constexpr i32 VK_ERROR_LAYER_NOT_PRESENT = -6;
constexpr i32 VK_ERROR_EXTENSION_NOT_PRESENT = -7;
constexpr i32 VK_ERROR_FORMAT_NOT_SUPPORTED = -11;
constexpr i32 VK_ERROR_INCOMPATIBLE_DRIVER = -9;

/// VK_MAKE_API_VERSION(0, 1, 2, 0). A GPU-topology query requires a 1.2
/// loader and physical device before it will inspect the device. Keep the
/// advertised version tied to the topology/property surface implemented here;
/// unsupported optional features and extensions remain disabled.
constexpr u32 VK_API_VERSION_1_2 = (1u << 22) | (2u << 12);

constexpr u32 VK_MAX_PHYSICAL_DEVICE_NAME_SIZE = 256;
constexpr u32 VK_MAX_MEMORY_TYPES = 32;
constexpr u32 VK_MAX_MEMORY_HEAPS = 16;
constexpr u32 VK_UUID_SIZE = 16;

/// VkPhysicalDeviceType
constexpr u32 VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1;

/// Apple's PCI vendor ID, as Vulkan drivers on Apple hardware report it.
constexpr u32 VK_VENDOR_ID_APPLE = 0x106B;

/// VkQueueFlagBits
constexpr u32 VK_QUEUE_GRAPHICS_BIT = 0x1;
constexpr u32 VK_QUEUE_COMPUTE_BIT = 0x2;
constexpr u32 VK_QUEUE_TRANSFER_BIT = 0x4;

/// VkMemoryPropertyFlagBits. Apple silicon is unified memory, so a single
/// heap that is device-local *and* host-visible is the accurate description.
constexpr u32 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x1;
constexpr u32 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2;
constexpr u32 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4;
constexpr u32 VK_MEMORY_HEAP_DEVICE_LOCAL_BIT = 0x1;

/// Byte offsets within VkPhysicalDeviceProperties. The struct is identical on
/// 32- and 64-bit because every member is a fixed-width scalar or array.
constexpr u32 VK_PDP_OFFSET_API_VERSION = 0;
constexpr u32 VK_PDP_OFFSET_DRIVER_VERSION = 4;
constexpr u32 VK_PDP_OFFSET_VENDOR_ID = 8;
constexpr u32 VK_PDP_OFFSET_DEVICE_ID = 12;
constexpr u32 VK_PDP_OFFSET_DEVICE_TYPE = 16;
constexpr u32 VK_PDP_OFFSET_DEVICE_NAME = 20;
constexpr u32 VK_PDP_OFFSET_PIPELINE_CACHE_UUID = 276;
constexpr u32 VK_PDP_OFFSET_LIMITS = 292;
/// sizeof(VkPhysicalDeviceLimits) == 504, sparseProperties adds 5 x VkBool32.
constexpr u32 VK_PDP_SIZE_LIMITS = 504;
constexpr u32 VK_PDP_SIZE = VK_PDP_OFFSET_LIMITS + VK_PDP_SIZE_LIMITS + 20;

// =============================================================================
// VulkanManager
// =============================================================================

class VulkanManager {
public:
    VulkanManager();
    /// Defined out of line: the allocation table holds unique_ptr<MetalBuffer>,
    /// and MetalBuffer is only forward-declared here to keep Metal headers out
    /// of everything that includes this one.
    ~VulkanManager();

    VulkanManager(const VulkanManager&) = delete;
    VulkanManager& operator=(const VulkanManager&) = delete;

    void set_memory(GuestMemory* memory) noexcept { m_memory = memory; }

    /// vkGetInstanceProcAddr resolves names to guest-callable addresses, which
    /// in Vitreo are thunk addresses, so the loader needs the registry.
    void set_thunk_registry(ThunkRegistry* registry) noexcept {
        m_registry = registry;
    }

    // -- Loader / instance --
    void impl_vkGetInstanceProcAddr(Marshaler& m);
    void impl_vkEnumerateInstanceVersion(Marshaler& m);
    void impl_vkEnumerateInstanceExtensionProperties(Marshaler& m);
    void impl_vkEnumerateInstanceLayerProperties(Marshaler& m);
    void impl_vkCreateInstance(Marshaler& m);
    void impl_vkDestroyInstance(Marshaler& m);

    // -- Physical device --
    void impl_vkEnumeratePhysicalDevices(Marshaler& m);
    void impl_vkGetPhysicalDeviceProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceFeatures(Marshaler& m);
    void impl_vkGetPhysicalDeviceQueueFamilyProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceMemoryProperties(Marshaler& m);
    void impl_vkEnumerateDeviceExtensionProperties(Marshaler& m);
    void impl_vkEnumerateDeviceLayerProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceFormatProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceImageFormatProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceSparseImageFormatProperties(Marshaler& m);
    void impl_vkEnumeratePhysicalDeviceGroups(Marshaler& m);
    void impl_vkGetPhysicalDeviceExternalBufferProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceExternalFenceProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceExternalSemaphoreProperties(Marshaler& m);
    void impl_vkGetPhysicalDeviceFeatures2(Marshaler& m);
    void impl_vkGetPhysicalDeviceFormatProperties2(Marshaler& m);
    void impl_vkGetPhysicalDeviceImageFormatProperties2(Marshaler& m);
    void impl_vkGetPhysicalDeviceMemoryProperties2(Marshaler& m);
    void impl_vkGetPhysicalDeviceProperties2(Marshaler& m);
    void impl_vkGetPhysicalDeviceQueueFamilyProperties2(Marshaler& m);
    void impl_vkGetPhysicalDeviceSparseImageFormatProperties2(Marshaler& m);

    // -- Logical device --
    void impl_vkCreateDevice(Marshaler& m);
    void impl_vkDestroyDevice(Marshaler& m);
    void impl_vkGetDeviceProcAddr(Marshaler& m);
    void impl_vkGetDeviceQueue(Marshaler& m);
    void impl_vkDeviceWaitIdle(Marshaler& m);
    void impl_vkQueueWaitIdle(Marshaler& m);
    void impl_vkQueueSubmit(Marshaler& m);

    // -- Device memory and buffers --
    void impl_vkAllocateMemory(Marshaler& m);
    void impl_vkFreeMemory(Marshaler& m);
    void impl_vkMapMemory(Marshaler& m);
    void impl_vkUnmapMemory(Marshaler& m);
    void impl_vkFlushMappedMemoryRanges(Marshaler& m);
    void impl_vkInvalidateMappedMemoryRanges(Marshaler& m);
    void impl_vkCreateBuffer(Marshaler& m);
    void impl_vkDestroyBuffer(Marshaler& m);
    void impl_vkGetBufferMemoryRequirements(Marshaler& m);
    void impl_vkBindBufferMemory(Marshaler& m);

    // -- Images and image views --
    void impl_vkCreateImage(Marshaler& m);
    void impl_vkDestroyImage(Marshaler& m);
    void impl_vkGetImageMemoryRequirements(Marshaler& m);
    void impl_vkBindImageMemory(Marshaler& m);
    void impl_vkCreateImageView(Marshaler& m);
    void impl_vkDestroyImageView(Marshaler& m);

    // -- Render passes and pipeline objects --
    void impl_vkCreateRenderPass(Marshaler& m);
    void impl_vkDestroyRenderPass(Marshaler& m);
    void impl_vkCreateShaderModule(Marshaler& m);
    void impl_vkDestroyShaderModule(Marshaler& m);
    void impl_vkCreatePipelineLayout(Marshaler& m);
    void impl_vkDestroyPipelineLayout(Marshaler& m);
    void impl_vkCreateGraphicsPipelines(Marshaler& m);
    void impl_vkDestroyPipeline(Marshaler& m);
    void impl_vkCreatePipelineCache(Marshaler& m);
    void impl_vkDestroyPipelineCache(Marshaler& m);
    void impl_vkCreateFramebuffer(Marshaler& m);
    void impl_vkDestroyFramebuffer(Marshaler& m);
    void impl_vkCreateDebugReportCallbackEXT(Marshaler& m);
    void impl_vkDestroyDebugReportCallbackEXT(Marshaler& m);

    // -- Command pools and buffers --
    void impl_vkCreateCommandPool(Marshaler& m);
    void impl_vkDestroyCommandPool(Marshaler& m);
    void impl_vkResetCommandPool(Marshaler& m);
    void impl_vkAllocateCommandBuffers(Marshaler& m);
    void impl_vkFreeCommandBuffers(Marshaler& m);
    void impl_vkBeginCommandBuffer(Marshaler& m);
    void impl_vkEndCommandBuffer(Marshaler& m);
    void impl_vkResetCommandBuffer(Marshaler& m);

    /// Test observability.
    [[nodiscard]] usize device_count() const noexcept { return m_devices.size(); }
    [[nodiscard]] usize command_buffer_count() const noexcept {
        return m_command_buffers.size();
    }
    [[nodiscard]] usize allocation_count() const noexcept {
        return m_allocations.size();
    }
    [[nodiscard]] usize buffer_count() const noexcept { return m_buffers.size(); }
    [[nodiscard]] usize image_count() const noexcept { return m_images.size(); }
    [[nodiscard]] usize image_view_count() const noexcept {
        return m_image_views.size();
    }
    [[nodiscard]] usize render_pass_count() const noexcept {
        return m_render_passes.size();
    }
    [[nodiscard]] usize shader_module_count() const noexcept {
        return m_shader_modules.size();
    }
    [[nodiscard]] usize pipeline_layout_count() const noexcept {
        return m_pipeline_layouts.size();
    }
    [[nodiscard]] usize pipeline_count() const noexcept {
        return m_pipelines.size();
    }
    [[nodiscard]] usize pipeline_cache_count() const noexcept {
        return m_pipeline_caches.size();
    }
    [[nodiscard]] usize framebuffer_count() const noexcept {
        return m_framebuffers.size();
    }

private:
    /// Vulkan defines a command buffer lifecycle and rejects out-of-order
    /// transitions. Tracking it is what turns "record twice without ending"
    /// from silent corruption into VK_ERROR_VALIDATION-shaped refusal.
    enum class CommandBufferState { Initial, Recording, Executable };

    struct DeviceState {
        u64 physical_device{0};
        u64 queue_handle{0};
    };

    struct CommandPoolState {
        u64 device{0};
        u32 queue_family{0};
    };

    struct CommandBufferState_ {
        u64 pool{0};
        u64 device{0};
        CommandBufferState state{CommandBufferState::Initial};
    };

    /// A VkDeviceMemory allocation is an MTLBuffer in shared storage. Mapping
    /// binds that buffer's host pointer straight into the guest address space,
    /// so a guest write lands in the memory the GPU reads - no staging copy.
    /// That is the accurate model for Apple silicon, and it is what the memory
    /// properties this layer reports (DEVICE_LOCAL | HOST_VISIBLE |
    /// HOST_COHERENT) actually promise.
    struct MemoryAllocation {
        std::unique_ptr<graphics::MetalBuffer> buffer;
        u64 size{0};
        GuestAddr mapped_addr{0};  // non-zero only while mapped
        usize mapped_size{0};
    };

    struct BufferState {
        u64 device{0};
        u64 size{0};
        u32 usage{0};
        u64 bound_memory{0};
        u64 bound_offset{0};
        bool bound{false};
    };

    /// The MTLTexture is not created until memory is bound, because in Vulkan
    /// an image has no storage until vkBindImageMemory - and for linear tiling
    /// the texture must alias that specific allocation, which is not known at
    /// vkCreateImage time.
    struct ImageState {
        u64 device{0};
        u32 width{1};
        u32 height{1};
        u32 vk_format{0};
        u32 metal_format{0};
        u32 usage{0};
        bool linear_tiling{false};
        u64 bound_memory{0};
        u64 bound_offset{0};
        bool bound{false};
        std::unique_ptr<graphics::MetalTexture> texture;
    };

    struct ImageViewState {
        u64 device{0};
        u64 image{0};
    };

    struct RenderPassState {
        u64 device{0};
    };

    struct ShaderModuleState {
        u64 device{0};
    };

    struct PipelineLayoutState {
        u64 device{0};
    };

    struct PipelineState {
        u64 device{0};
    };

    struct PipelineCacheState {
        u64 device{0};
    };

    struct FramebufferState {
        u64 device{0};
    };
    /// Handle values are opaque to the guest; these are non-null sentinels that
    /// can be validated on the way back in rather than dereferenced.
    ///
    /// VkInstance and VkPhysicalDevice are *dispatchable* handles, so they are
    /// pointer-width - 32 bits on an x86 guest. A wider sentinel is silently
    /// truncated on the way out and then never matches on the way back in,
    /// which fails as "unknown physical device" a long way from the cause.
    static constexpr u64 INSTANCE_HANDLE = 0x564B0001ULL;         // 'VK' + 1
    static constexpr u64 PHYSICAL_DEVICE_HANDLE = 0x564B0002ULL;  // 'VK' + 2

    [[nodiscard]] std::string metal_device_name() const;
    /// Shared resolution for vkGetInstanceProcAddr and vkGetDeviceProcAddr:
    /// both map a name to a guest-callable thunk address, and both must return
    /// null for anything this loader does not provide.
    void resolve_proc_addr(Marshaler& m, usize name_arg_index);
    /// Writes VkPhysicalDeviceLimits fields that Metal can actually answer for.
    void write_backed_limits(Marshaler& m, GuestAddr limits) const;
    void write_physical_device_properties(Marshaler& m, GuestAddr props) const;
    void write_physical_device_features(Marshaler& m, GuestAddr features) const;
    void write_physical_device_memory_properties(Marshaler& m,
                                                 GuestAddr props) const;
    void write_queue_family_properties(Marshaler& m, GuestAddr props) const;

    GuestMemory* m_memory{nullptr};
    ThunkRegistry* m_registry{nullptr};
    bool m_instance_created{false};

    /// Handles are opaque to the guest, so a monotonic counter is enough and
    /// avoids handing out an address the guest could dereference.
    u64 m_next_handle{0x1000};
    [[nodiscard]] u64 allocate_handle() noexcept { return m_next_handle++; }

    std::unordered_map<u64, DeviceState> m_devices;
    std::unordered_map<u64, CommandPoolState> m_command_pools;
    std::unordered_map<u64, CommandBufferState_> m_command_buffers;
    std::unordered_map<u64, MemoryAllocation> m_allocations;
    std::unordered_map<u64, BufferState> m_buffers;
    std::unordered_map<u64, ImageState> m_images;
    std::unordered_map<u64, ImageViewState> m_image_views;
    std::unordered_map<u64, RenderPassState> m_render_passes;
    std::unordered_map<u64, ShaderModuleState> m_shader_modules;
    std::unordered_map<u64, PipelineLayoutState> m_pipeline_layouts;
    std::unordered_map<u64, PipelineState> m_pipelines;
    std::unordered_map<u64, PipelineCacheState> m_pipeline_caches;
    std::unordered_map<u64, FramebufferState> m_framebuffers;
    std::unordered_map<u64, u64> m_debug_report_callbacks;

    /// Where to start scanning for a free guest range to map an allocation
    /// into. Advanced past contended gaps so a retry does not rescan the same
    /// occupied range.
    GuestAddr m_next_map_hint{0x30000000};
};

/// Create vulkan-1.dll exports backed by the manager.
DllExports create_vulkan_thunks(VulkanManager& mgr);

} // namespace vitreo::winapi
