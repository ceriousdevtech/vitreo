// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/vulkan/vulkan.hpp"

#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_texture.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/common/display_device_identity.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace vitreo::winapi {
namespace {

/// Every entry point vulkan-1.dll exposes. vkGetInstanceProcAddr resolves
/// against this list, so a name absent here returns null - which is the
/// contract: a null return means "this loader does not provide that function",
/// and volk-style loaders handle it by leaving their slot empty rather than
/// crashing. Returning a bogus non-null pointer instead would turn a clean
/// capability check into a jump to garbage.
constexpr const char* VULKAN_ENTRY_POINTS[] = {
    "vkGetInstanceProcAddr",
    "vkEnumerateInstanceVersion",
    "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateInstanceLayerProperties",
    "vkCreateInstance",
    "vkDestroyInstance",
    "vkEnumeratePhysicalDevices",
    "vkGetPhysicalDeviceProperties",
    "vkGetPhysicalDeviceFeatures",
    "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceMemoryProperties",
    "vkEnumerateDeviceExtensionProperties",
    "vkEnumerateDeviceLayerProperties",
    "vkGetPhysicalDeviceFormatProperties",
    "vkGetPhysicalDeviceImageFormatProperties",
    "vkGetPhysicalDeviceSparseImageFormatProperties",
    "vkEnumeratePhysicalDeviceGroups",
    "vkGetPhysicalDeviceExternalBufferProperties",
    "vkGetPhysicalDeviceExternalFenceProperties",
    "vkGetPhysicalDeviceExternalSemaphoreProperties",
    "vkGetPhysicalDeviceFeatures2",
    "vkGetPhysicalDeviceFormatProperties2",
    "vkGetPhysicalDeviceImageFormatProperties2",
    "vkGetPhysicalDeviceMemoryProperties2",
    "vkGetPhysicalDeviceProperties2",
    "vkGetPhysicalDeviceQueueFamilyProperties2",
    "vkGetPhysicalDeviceSparseImageFormatProperties2",
    "vkCreateDevice",
    "vkDestroyDevice",
    "vkGetDeviceProcAddr",
    "vkGetDeviceQueue",
    "vkDeviceWaitIdle",
    "vkQueueWaitIdle",
    "vkQueueSubmit",
    "vkCreateCommandPool",
    "vkDestroyCommandPool",
    "vkResetCommandPool",
    "vkAllocateCommandBuffers",
    "vkFreeCommandBuffers",
    "vkBeginCommandBuffer",
    "vkEndCommandBuffer",
    "vkResetCommandBuffer",
    "vkAllocateMemory",
    "vkFreeMemory",
    "vkMapMemory",
    "vkUnmapMemory",
    "vkFlushMappedMemoryRanges",
    "vkInvalidateMappedMemoryRanges",
    "vkCreateBuffer",
    "vkDestroyBuffer",
    "vkGetBufferMemoryRequirements",
    "vkBindBufferMemory",
    "vkCreateImage",
    "vkDestroyImage",
    "vkGetImageMemoryRequirements",
    "vkBindImageMemory",
    "vkCreateImageView",
    "vkDestroyImageView",
    "vkCreateRenderPass",
    "vkDestroyRenderPass",
    "vkCreateShaderModule",
    "vkDestroyShaderModule",
    "vkCreatePipelineLayout",
    "vkDestroyPipelineLayout",
    "vkCreateGraphicsPipelines",
    "vkDestroyPipeline",
    "vkCreatePipelineCache",
    "vkDestroyPipelineCache",
    "vkCreateFramebuffer",
    "vkDestroyFramebuffer",
    "vkCreateDebugReportCallbackEXT",
    "vkDestroyDebugReportCallbackEXT",
};

[[nodiscard]] bool is_known_entry_point(const std::string& name) {
    for (const char* entry : VULKAN_ENTRY_POINTS) {
        if (name == entry) return true;
    }
    return false;
}

/// Write `count` into the guest's count pointer and report whether the caller's
/// buffer can hold everything. This two-call idiom (query count, then fill) is
/// used by every vkEnumerate*/vkGet*Properties function, and VK_INCOMPLETE on a
/// short buffer is part of the contract rather than an error.
struct EnumerationRequest {
    bool valid{false};
    u32 capacity{0};
};

/// Read a non-dispatchable Vulkan handle from the argument list.
///
/// These are always 64-bit, on both bitnesses - unlike dispatchable handles
/// (VkInstance, VkDevice, VkCommandBuffer) which are pointer-width. On x86
/// stdcall a 64-bit argument occupies two stack slots, so reading one with a
/// single arg() takes only its low half *and* leaves every later argument
/// index off by one. Both failures are silent.
[[nodiscard]] u64 read_handle64(Marshaler& m, usize& index) {
    if (m.cpu_is_64bit()) return m.arg(index++);
    const u64 low = m.arg(index++) & 0xFFFFFFFFULL;
    const u64 high = m.arg(index++) & 0xFFFFFFFFULL;
    return low | (high << 32);
}

[[nodiscard]] EnumerationRequest read_enumeration_request(
    Marshaler& m, GuestAddr count_ptr, GuestAddr array_ptr, u32 available) {
    EnumerationRequest request;
    if (count_ptr == 0) return request;
    request.valid = true;

    if (array_ptr == 0) {
        // Count query: report how many are available and write nothing else.
        (void)m.write_u32(count_ptr, available);
        request.capacity = 0;
        return request;
    }
    const auto capacity = m.memory().read_u32(count_ptr);
    request.capacity = capacity.is_ok() ? capacity.value() : 0;
    return request;
}

} // namespace

VulkanManager::VulkanManager() = default;
VulkanManager::~VulkanManager() = default;

std::string VulkanManager::metal_device_name() const {
    const std::string name = graphics::MetalDevice::instance().device_name();
    return name.empty() ? std::string("Apple GPU") : name;
}

// =============================================================================
// Loader / instance
// =============================================================================

void VulkanManager::impl_vkGetInstanceProcAddr(Marshaler& m) {
    // PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char* pName)
    resolve_proc_addr(m, 1);
}

void VulkanManager::impl_vkGetDeviceProcAddr(Marshaler& m) {
    // PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice, const char* pName)
    // Same resolution as the instance form: Vitreo has one dispatch table, so
    // there is no per-device specialisation to distinguish.
    resolve_proc_addr(m, 1);
}

void VulkanManager::resolve_proc_addr(Marshaler& m, usize name_arg_index) {
    const GuestAddr name_ptr = m.arg_ptr(name_arg_index);
    if (name_ptr == 0 || m_registry == nullptr) {
        m.set_return(0);
        return;
    }

    auto name_result = m.read_ansi_string(name_ptr);
    if (name_result.is_err()) {
        m.set_return(0);
        return;
    }
    const std::string name = name_result.value();

    if (!is_known_entry_point(name)) {
        // Not an error: the caller is probing for a function this loader does
        // not implement, and null is how Vulkan says so.
        VITREO_TRACE("[vulkan] procaddr(\"%s\") -> null (not implemented)\n",
                     name.c_str());
        m.set_return(0);
        return;
    }

    auto address = m_registry->get_thunk_address("vulkan-1.dll", name.c_str());
    if (address.is_err()) {
        VITREO_TRACE("[vulkan] procaddr(\"%s\") -> no thunk address\n", name.c_str());
        m.set_return(0);
        return;
    }
    VITREO_TRACE("[vulkan] procaddr(\"%s\") -> 0x%llx\n", name.c_str(),
                 static_cast<unsigned long long>(address.value()));
    m.set_return(address.value());
}

void VulkanManager::impl_vkEnumerateInstanceVersion(Marshaler& m) {
    // VkResult vkEnumerateInstanceVersion(uint32_t* pApiVersion)
    const GuestAddr version_ptr = m.arg_ptr(0);
    if (version_ptr == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u32(version_ptr, VK_API_VERSION_1_2);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkEnumerateInstanceExtensionProperties(Marshaler& m) {
    // No instance extensions are supported yet. Reporting zero is accurate;
    // claiming VK_KHR_surface without a swapchain behind it would strand a
    // caller at surface creation instead of here.
    const GuestAddr count_ptr = m.arg_ptr(1);
    const GuestAddr props_ptr = m.arg_ptr(2);
    const auto request = read_enumeration_request(m, count_ptr, props_ptr, 0);
    if (!request.valid) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u32(count_ptr, 0);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkEnumerateInstanceLayerProperties(Marshaler& m) {
    const GuestAddr count_ptr = m.arg_ptr(0);
    const GuestAddr props_ptr = m.arg_ptr(1);
    const auto request = read_enumeration_request(m, count_ptr, props_ptr, 0);
    if (!request.valid) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u32(count_ptr, 0);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkCreateInstance(Marshaler& m) {
    // VkResult vkCreateInstance(const VkInstanceCreateInfo*,
    //                           const VkAllocationCallbacks*, VkInstance*)
    const GuestAddr create_info = m.arg_ptr(0);
    const GuestAddr out_instance = m.arg_ptr(2);
    if (out_instance == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    // VkInstanceCreateInfo::enabledExtensionCount sits at offset 32 on 64-bit
    // and 20 on 32-bit (sType, pNext, flags, pApplicationInfo, then the layer
    // pair). Any requested extension has to be refused: none are supported, and
    // VK_ERROR_EXTENSION_NOT_PRESENT is the documented answer.
    if (create_info != 0) {
        const u32 ext_count_offset = m.cpu_is_64bit() ? 32u : 20u;
        const auto ext_count = m.memory().read_u32(create_info + ext_count_offset);
        if (ext_count.is_ok() && ext_count.value() != 0) {
            VITREO_TRACE("[vulkan] vkCreateInstance requested %u extension(s); "
                         "none are supported -> VK_ERROR_EXTENSION_NOT_PRESENT\n",
                         ext_count.value());
            m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_EXTENSION_NOT_PRESENT)));
            return;
        }
    }

    (void)m.write_ptr(out_instance, INSTANCE_HANDLE);
    m_instance_created = true;
    VITREO_TRACE("[vulkan] vkCreateInstance -> VK_SUCCESS (instance=0x%llx)\n",
                 static_cast<unsigned long long>(INSTANCE_HANDLE));
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyInstance(Marshaler& m) {
    (void)m.arg(0);
    m_debug_report_callbacks.clear();
    m_instance_created = false;
}

// =============================================================================
// Physical device
// =============================================================================

void VulkanManager::impl_vkEnumeratePhysicalDevices(Marshaler& m) {
    // VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t*, VkPhysicalDevice*)
    const GuestAddr count_ptr = m.arg_ptr(1);
    const GuestAddr devices_ptr = m.arg_ptr(2);
    if (count_ptr == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    // Exactly one device: the Metal GPU this process is already running on.
    constexpr u32 DEVICE_COUNT = 1;
    if (devices_ptr == 0) {
        (void)m.write_u32(count_ptr, DEVICE_COUNT);
        m.set_return(VK_SUCCESS);
        return;
    }

    const auto capacity = m.memory().read_u32(count_ptr);
    if (capacity.is_err() || capacity.value() < DEVICE_COUNT) {
        (void)m.write_u32(count_ptr, 0);
        m.set_return(VK_INCOMPLETE);
        return;
    }
    (void)m.write_ptr(devices_ptr, PHYSICAL_DEVICE_HANDLE);
    (void)m.write_u32(count_ptr, DEVICE_COUNT);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkGetPhysicalDeviceProperties(Marshaler& m) {
    // void vkGetPhysicalDeviceProperties(VkPhysicalDevice,
    //                                    VkPhysicalDeviceProperties*)
    const GuestAddr props = m.arg_ptr(1);
    if (props == 0) return;
    write_physical_device_properties(m, props);
}

void VulkanManager::write_physical_device_properties(
    Marshaler& m, GuestAddr props) const {
    // Zero the whole struct first: limits are left at zero deliberately until
    // the device path can report values it actually enforces. A caller that
    // checks a limit gets an obviously-unusable zero rather than an invented
    // number it might size allocations against.
    std::vector<u8> zeroed(VK_PDP_SIZE, 0);
    (void)m.memory().write_bytes(props, zeroed.data(), zeroed.size());

    (void)m.write_u32(props + VK_PDP_OFFSET_API_VERSION, VK_API_VERSION_1_2);
    // The same configured adapter identity DXGI/D3D9/DDraw report. Vulkan's
    // vendorID is a PCI vendor ID when the device is a PCI device, so a
    // capsule that asserts a PCI identity must see it here too.
    const auto& identity = display_device_identity::adapter_identity();
    (void)m.write_u32(props + VK_PDP_OFFSET_DRIVER_VERSION, 1);
    (void)m.write_u32(props + VK_PDP_OFFSET_VENDOR_ID,
                      identity.vendor_id.value_or(VK_VENDOR_ID_APPLE));
    (void)m.write_u32(props + VK_PDP_OFFSET_DEVICE_ID,
                      identity.device_id.value_or(0));
    // Apple silicon shares memory between CPU and GPU, so INTEGRATED_GPU is the
    // honest classification even though the GPU is discrete-class in speed.
    (void)m.write_u32(props + VK_PDP_OFFSET_DEVICE_TYPE,
                      VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);

    const std::string name =
        identity.description.value_or(metal_device_name());
    const usize name_bytes =
        std::min<usize>(name.size(), VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    (void)m.memory().write_bytes(props + VK_PDP_OFFSET_DEVICE_NAME,
                                 reinterpret_cast<const Byte*>(name.data()),
                                 name_bytes);
    (void)m.write_u8(props + VK_PDP_OFFSET_DEVICE_NAME + name_bytes, 0);

    VITREO_TRACE("[vulkan] vkGetPhysicalDeviceProperties -> \"%s\"\n", name.c_str());
}

void VulkanManager::impl_vkGetPhysicalDeviceFeatures(Marshaler& m) {
    // VkPhysicalDeviceFeatures is 55 VkBool32s. All false: no feature can be
    // promised before a logical device can be created to exercise it.
    const GuestAddr features = m.arg_ptr(1);
    if (features == 0) return;
    write_physical_device_features(m, features);
}

void VulkanManager::write_physical_device_features(
    Marshaler& m, GuestAddr features) const {
    constexpr usize FEATURE_COUNT = 55;
    std::vector<u8> zeroed(FEATURE_COUNT * sizeof(u32), 0);
    (void)m.memory().write_bytes(features, zeroed.data(), zeroed.size());
}

void VulkanManager::impl_vkGetPhysicalDeviceQueueFamilyProperties(Marshaler& m) {
    // void vkGetPhysicalDeviceQueueFamilyProperties(
    //     VkPhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties*)
    //
    // VkQueueFamilyProperties { VkQueueFlags flags; uint32_t queueCount;
    //   uint32_t timestampValidBits; VkExtent3D minImageTransferGranularity; }
    // = 6 x uint32 = 24 bytes, same on both bitnesses.
    const GuestAddr count_ptr = m.arg_ptr(1);
    const GuestAddr props_ptr = m.arg_ptr(2);
    if (count_ptr == 0) return;

    constexpr u32 FAMILY_COUNT = 1;
    if (props_ptr == 0) {
        (void)m.write_u32(count_ptr, FAMILY_COUNT);
        return;
    }
    const auto capacity = m.memory().read_u32(count_ptr);
    if (capacity.is_err() || capacity.value() < FAMILY_COUNT) {
        (void)m.write_u32(count_ptr, 0);
        return;
    }

    write_queue_family_properties(m, props_ptr);
    (void)m.write_u32(count_ptr, FAMILY_COUNT);
}

void VulkanManager::write_queue_family_properties(
    Marshaler& m, GuestAddr props) const {
    // Metal exposes one command queue that serves graphics, compute and blit,
    // so one combined family is the accurate mapping.
    (void)m.write_u32(props + 0,
                      VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                          VK_QUEUE_TRANSFER_BIT);
    (void)m.write_u32(props + 4, 1);   // queueCount
    (void)m.write_u32(props + 8, 0);   // timestampValidBits: none yet
    (void)m.write_u32(props + 12, 1);  // minImageTransferGranularity.width
    (void)m.write_u32(props + 16, 1);  // .height
    (void)m.write_u32(props + 20, 1);  // .depth
}

void VulkanManager::impl_vkGetPhysicalDeviceMemoryProperties(Marshaler& m) {
    // VkPhysicalDeviceMemoryProperties {
    //   uint32_t memoryTypeCount;
    //   VkMemoryType memoryTypes[32];   // { flags, heapIndex } = 8 bytes each
    //   uint32_t memoryHeapCount;
    //   VkMemoryHeap memoryHeaps[16];   // { VkDeviceSize size, flags } = 16 bytes
    // }
    // VkMemoryHeap contains a VkDeviceSize (always 64-bit) followed by flags,
    // so it is 8-byte aligned and 16 bytes wide on both bitnesses.
    const GuestAddr props = m.arg_ptr(1);
    if (props == 0) return;
    write_physical_device_memory_properties(m, props);
}

void VulkanManager::write_physical_device_memory_properties(
    Marshaler& m, GuestAddr props) const {
    constexpr u32 TYPES_OFFSET = 4;
    constexpr u32 HEAP_COUNT_OFFSET = TYPES_OFFSET + VK_MAX_MEMORY_TYPES * 8;
    constexpr u32 HEAPS_OFFSET = HEAP_COUNT_OFFSET + 8;  // padded to 8-byte align
    constexpr u32 TOTAL_SIZE = HEAPS_OFFSET + VK_MAX_MEMORY_HEAPS * 16;

    std::vector<u8> zeroed(TOTAL_SIZE, 0);
    (void)m.memory().write_bytes(props, zeroed.data(), zeroed.size());

    // One unified heap, one memory type. Apple silicon genuinely has no
    // separate device-local pool, so a single device-local + host-visible +
    // host-coherent type is the accurate description rather than a
    // simplification.
    (void)m.write_u32(props + 0, 1);  // memoryTypeCount
    (void)m.write_u32(props + TYPES_OFFSET + 0,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    (void)m.write_u32(props + TYPES_OFFSET + 4, 0);  // heapIndex

    (void)m.write_u32(props + HEAP_COUNT_OFFSET, 1);  // memoryHeapCount
    // Heap size is left at zero rather than guessed: reporting a size we have
    // not queried would invite a caller to size an allocation against it.
    (void)m.write_u64(props + HEAPS_OFFSET + 0, 0);
    (void)m.write_u32(props + HEAPS_OFFSET + 8, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
}

void VulkanManager::impl_vkEnumerateDeviceExtensionProperties(Marshaler& m) {
    const GuestAddr count_ptr = m.arg_ptr(2);
    const GuestAddr props_ptr = m.arg_ptr(3);
    const auto request = read_enumeration_request(m, count_ptr, props_ptr, 0);
    if (!request.valid) {
        m.set_return(static_cast<u64>(
            static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u32(count_ptr, 0);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkEnumerateDeviceLayerProperties(Marshaler& m) {
    const GuestAddr count_ptr = m.arg_ptr(1);
    const GuestAddr props_ptr = m.arg_ptr(2);
    const auto request = read_enumeration_request(m, count_ptr, props_ptr, 0);
    if (!request.valid) {
        m.set_return(static_cast<u64>(
            static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u32(count_ptr, 0);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkGetPhysicalDeviceFormatProperties(Marshaler& m) {
    const GuestAddr props = m.arg_ptr(2);
    if (props == 0) return;
    // No format is advertised until its complete Vulkan usage set is mapped
    // to Metal. Zero feature masks are the specified unsupported answer.
    std::vector<u8> zeroed(12, 0);
    (void)m.memory().write_bytes(props, zeroed.data(), zeroed.size());
}

void VulkanManager::impl_vkGetPhysicalDeviceImageFormatProperties(Marshaler& m) {
    const GuestAddr props = m.arg_ptr(6);
    if (props != 0) {
        std::vector<u8> zeroed(32, 0);
        (void)m.memory().write_bytes(props, zeroed.data(), zeroed.size());
    }
    m.set_return(static_cast<u64>(
        static_cast<u32>(VK_ERROR_FORMAT_NOT_SUPPORTED)));
}

void VulkanManager::impl_vkGetPhysicalDeviceSparseImageFormatProperties(
    Marshaler& m) {
    const GuestAddr count_ptr = m.arg_ptr(6);
    if (count_ptr != 0) (void)m.write_u32(count_ptr, 0);
}

void VulkanManager::impl_vkEnumeratePhysicalDeviceGroups(Marshaler& m) {
    const GuestAddr count_ptr = m.arg_ptr(1);
    const GuestAddr groups_ptr = m.arg_ptr(2);
    if (count_ptr == 0) {
        m.set_return(static_cast<u64>(
            static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    if (groups_ptr == 0) {
        (void)m.write_u32(count_ptr, 1);
        m.set_return(VK_SUCCESS);
        return;
    }
    const auto capacity = m.memory().read_u32(count_ptr);
    if (capacity.is_err() || capacity.value() == 0) {
        (void)m.write_u32(count_ptr, 0);
        m.set_return(VK_INCOMPLETE);
        return;
    }

    const bool is64 = m.cpu_is_64bit();
    const GuestAddr count_offset = is64 ? 16 : 8;
    const GuestAddr devices_offset = is64 ? 24 : 12;
    const GuestAddr subset_offset = is64 ? 280 : 140;
    (void)m.write_u32(groups_ptr + count_offset, 1);
    (void)m.write_ptr(groups_ptr + devices_offset, PHYSICAL_DEVICE_HANDLE);
    (void)m.write_u32(groups_ptr + subset_offset, 0);
    (void)m.write_u32(count_ptr, 1);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkGetPhysicalDeviceExternalBufferProperties(
    Marshaler& m) {
    const GuestAddr props = m.arg_ptr(2);
    if (props == 0) return;
    const GuestAddr payload = props + (m.cpu_is_64bit() ? 16 : 8);
    std::vector<u8> zeroed(12, 0);
    (void)m.memory().write_bytes(payload, zeroed.data(), zeroed.size());
}

void VulkanManager::impl_vkGetPhysicalDeviceExternalFenceProperties(
    Marshaler& m) {
    impl_vkGetPhysicalDeviceExternalBufferProperties(m);
}

void VulkanManager::impl_vkGetPhysicalDeviceExternalSemaphoreProperties(
    Marshaler& m) {
    impl_vkGetPhysicalDeviceExternalBufferProperties(m);
}

void VulkanManager::impl_vkGetPhysicalDeviceFeatures2(Marshaler& m) {
    const GuestAddr props = m.arg_ptr(1);
    if (props == 0) return;
    write_physical_device_features(
        m, props + (m.cpu_is_64bit() ? 16 : 8));
}

void VulkanManager::impl_vkGetPhysicalDeviceFormatProperties2(Marshaler& m) {
    const GuestAddr props = m.arg_ptr(2);
    if (props == 0) return;
    const GuestAddr payload = props + (m.cpu_is_64bit() ? 16 : 8);
    std::vector<u8> zeroed(12, 0);
    (void)m.memory().write_bytes(payload, zeroed.data(), zeroed.size());
}

void VulkanManager::impl_vkGetPhysicalDeviceImageFormatProperties2(
    Marshaler& m) {
    const GuestAddr props = m.arg_ptr(2);
    if (props != 0) {
        const GuestAddr payload = props + (m.cpu_is_64bit() ? 16 : 8);
        std::vector<u8> zeroed(32, 0);
        (void)m.memory().write_bytes(payload, zeroed.data(), zeroed.size());
    }
    m.set_return(static_cast<u64>(
        static_cast<u32>(VK_ERROR_FORMAT_NOT_SUPPORTED)));
}

void VulkanManager::impl_vkGetPhysicalDeviceMemoryProperties2(Marshaler& m) {
    const GuestAddr props = m.arg_ptr(1);
    if (props == 0) return;
    write_physical_device_memory_properties(
        m, props + (m.cpu_is_64bit() ? 16 : 8));
}

void VulkanManager::impl_vkGetPhysicalDeviceProperties2(Marshaler& m) {
    const GuestAddr props = m.arg_ptr(1);
    if (props == 0) return;
    write_physical_device_properties(
        m, props + (m.cpu_is_64bit() ? 16 : 8));
}

void VulkanManager::impl_vkGetPhysicalDeviceQueueFamilyProperties2(
    Marshaler& m) {
    const GuestAddr count_ptr = m.arg_ptr(1);
    const GuestAddr props_ptr = m.arg_ptr(2);
    if (count_ptr == 0) return;
    if (props_ptr == 0) {
        (void)m.write_u32(count_ptr, 1);
        return;
    }
    const auto capacity = m.memory().read_u32(count_ptr);
    if (capacity.is_err() || capacity.value() == 0) {
        (void)m.write_u32(count_ptr, 0);
        return;
    }
    write_queue_family_properties(
        m, props_ptr + (m.cpu_is_64bit() ? 16 : 8));
    (void)m.write_u32(count_ptr, 1);
}

void VulkanManager::impl_vkGetPhysicalDeviceSparseImageFormatProperties2(
    Marshaler& m) {
    const GuestAddr count_ptr = m.arg_ptr(2);
    if (count_ptr != 0) (void)m.write_u32(count_ptr, 0);
}


// =============================================================================
// Device memory and buffers
// =============================================================================

void VulkanManager::impl_vkAllocateMemory(Marshaler& m) {
    // VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*,
    //                           const VkAllocationCallbacks*, VkDeviceMemory*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr info = m.arg_ptr(1);
    const GuestAddr out_memory = m.arg_ptr(3);
    if (out_memory == 0 || info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_memory, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // VkMemoryAllocateInfo { sType, pNext, VkDeviceSize allocationSize,
    //                        uint32_t memoryTypeIndex }
    // allocationSize is 64-bit and 8-aligned: +8 on 32-bit, +16 on 64-bit.
    const u32 size_offset = m.cpu_is_64bit() ? 16u : 8u;
    const auto size_result = m.memory().read_u64(info + size_offset);
    const auto type_result = m.memory().read_u32(info + size_offset + 8);
    if (size_result.is_err() || size_result.value() == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    // Only memory type 0 is advertised, so any other index is the caller
    // ignoring its own vkGetPhysicalDeviceMemoryProperties result.
    if (type_result.is_ok() && type_result.value() != 0) {
        VITREO_TRACE("[vulkan] vkAllocateMemory memoryTypeIndex=%u; only 0 exists\n",
                     type_result.value());
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    const u64 size = size_result.value();
    MemoryAllocation allocation;
    allocation.size = size;
    allocation.buffer = std::make_unique<graphics::MetalBuffer>(
        graphics::MetalDevice::instance().device_ptr(), static_cast<usize>(size),
        // Staging is the usage that guarantees MTLResourceStorageModeShared,
        // which is what makes map() return a pointer the CPU can write and the
        // GPU can read. A VkDeviceMemory allocation has no single Vulkan usage
        // - buffers bound to it declare their own - so the mappable storage
        // mode is the property that actually matters here.
        graphics::BufferUsage::Staging);
    if (allocation.buffer->handle() == nullptr) {
        VITREO_TRACE("[vulkan] vkAllocateMemory: MTLBuffer creation failed size=%llu\n",
                     static_cast<unsigned long long>(size));
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    const u64 handle = allocate_handle();
    m_allocations.emplace(handle, std::move(allocation));
    (void)m.write_u64(out_memory, handle);
    VITREO_TRACE("[vulkan] vkAllocateMemory size=%llu -> 0x%llx\n",
                 static_cast<unsigned long long>(size),
                 static_cast<unsigned long long>(handle));
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkFreeMemory(Marshaler& m) {
    usize index = 1;  // skip device
    const u64 memory_handle = read_handle64(m, index);
    auto it = m_allocations.find(memory_handle);
    if (it == m_allocations.end()) return;

    // Freeing while still mapped is legal in Vulkan only if the app unmapped
    // first, but a guest that skipped it would otherwise leave a guest range
    // pointing at a released MTLBuffer.
    if (it->second.mapped_addr != 0) {
        (void)m.memory().unmap_region(it->second.mapped_addr,
                                      it->second.mapped_size);
    }
    m_allocations.erase(it);
}

void VulkanManager::impl_vkMapMemory(Marshaler& m) {
    // VkResult vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize offset,
    //                      VkDeviceSize size, VkMemoryMapFlags, void** ppData)
    // Three 64-bit values in the middle, so on x86 every later argument sits
    // two slots further along than its parameter position.
    usize index = 1;
    const u64 memory_handle = read_handle64(m, index);
    const u64 offset = read_handle64(m, index);
    const u64 size = read_handle64(m, index);
    (void)m.arg(index++);  // flags
    const GuestAddr out_data = static_cast<GuestAddr>(m.arg(index));
    if (out_data == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_ptr(out_data, 0);

    auto it = m_allocations.find(memory_handle);
    if (it == m_allocations.end() || it->second.buffer == nullptr) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    if (it->second.mapped_addr != 0) {
        // Vulkan forbids mapping an allocation that is already mapped.
        VITREO_TRACE("[vulkan] vkMapMemory on an already-mapped allocation\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    if (offset > it->second.size) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // VK_WHOLE_SIZE maps from the offset to the end of the allocation.
    constexpr u64 VK_WHOLE_SIZE = ~0ULL;
    u64 map_size = (size == VK_WHOLE_SIZE) ? (it->second.size - offset) : size;
    if (map_size == 0 || offset + map_size > it->second.size) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    auto* host = static_cast<u8*>(it->second.buffer->map());
    if (host == nullptr) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // Bind the MTLBuffer's own storage into the guest address space, so guest
    // writes land directly in the memory the GPU will read.
    const usize aligned = (static_cast<usize>(map_size) + 0xFFFu) & ~usize{0xFFF};
    GuestAddr base = 0;
    for (int attempt = 0; attempt < 8 && base == 0; ++attempt) {
        GuestAddr candidate = m.memory().find_free_gap(m_next_map_hint, aligned);
        if (candidate == 0) candidate = m.memory().find_free_gap(0x10000000, aligned);
        if (candidate == 0) break;
        auto result = m.memory().map_region_backed(candidate, aligned, host + offset,
                                                   MemoryProtection::ReadWrite);
        if (result.is_ok()) {
            base = candidate;
        } else {
            // Another thread took the gap between scan and bind; move past it.
            m_next_map_hint = candidate + static_cast<GuestAddr>(aligned);
        }
    }
    if (base == 0) {
        VITREO_TRACE("[vulkan] vkMapMemory: no free guest range for %zu bytes\n",
                     aligned);
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    it->second.mapped_addr = base;
    it->second.mapped_size = aligned;
    (void)m.write_ptr(out_data, base);
    VITREO_TRACE("[vulkan] vkMapMemory 0x%llx -> guest 0x%llx (%llu bytes)\n",
                 static_cast<unsigned long long>(memory_handle),
                 static_cast<unsigned long long>(base),
                 static_cast<unsigned long long>(map_size));
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkUnmapMemory(Marshaler& m) {
    usize index = 1;
    const u64 memory_handle = read_handle64(m, index);
    auto it = m_allocations.find(memory_handle);
    if (it == m_allocations.end() || it->second.mapped_addr == 0) return;

    (void)m.memory().unmap_region(it->second.mapped_addr, it->second.mapped_size);
    it->second.mapped_addr = 0;
    it->second.mapped_size = 0;
}

void VulkanManager::impl_vkFlushMappedMemoryRanges(Marshaler& m) {
    // The advertised memory type is HOST_COHERENT, which means writes are
    // visible to the device without an explicit flush - and on unified memory
    // that is genuinely true rather than a simplification. Succeeding here is
    // correct, not a stub.
    (void)m.arg(0);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkInvalidateMappedMemoryRanges(Marshaler& m) {
    (void)m.arg(0);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkCreateBuffer(Marshaler& m) {
    // VkResult vkCreateBuffer(VkDevice, const VkBufferCreateInfo*,
    //                         const VkAllocationCallbacks*, VkBuffer*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr info = m.arg_ptr(1);
    const GuestAddr out_buffer = m.arg_ptr(3);
    if (out_buffer == 0 || info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_buffer, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // VkBufferCreateInfo { sType, pNext, flags, VkDeviceSize size,
    //                      VkBufferUsageFlags usage, ... }
    // size is 64-bit and 8-aligned: +16 on 32-bit (sType 0, pNext 4, flags 8,
    // pad 12), +24 on 64-bit.
    const u32 size_offset = m.cpu_is_64bit() ? 24u : 16u;
    const auto size_result = m.memory().read_u64(info + size_offset);
    const auto usage_result = m.memory().read_u32(info + size_offset + 8);
    if (size_result.is_err() || size_result.value() == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    BufferState buffer;
    buffer.device = device;
    buffer.size = size_result.value();
    buffer.usage = usage_result.is_ok() ? usage_result.value() : 0;

    const u64 handle = allocate_handle();
    m_buffers.emplace(handle, buffer);
    (void)m.write_u64(out_buffer, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyBuffer(Marshaler& m) {
    usize index = 1;
    const u64 buffer = read_handle64(m, index);
    m_buffers.erase(buffer);
}

void VulkanManager::impl_vkGetBufferMemoryRequirements(Marshaler& m) {
    // void vkGetBufferMemoryRequirements(VkDevice, VkBuffer,
    //                                    VkMemoryRequirements*)
    // VkMemoryRequirements { VkDeviceSize size; VkDeviceSize alignment;
    //                        uint32_t memoryTypeBits; }
    usize index = 1;
    const u64 buffer = read_handle64(m, index);
    const GuestAddr out_requirements = static_cast<GuestAddr>(m.arg(index));
    if (out_requirements == 0) return;

    auto it = m_buffers.find(buffer);
    const u64 size = it != m_buffers.end() ? it->second.size : 0;
    (void)m.write_u64(out_requirements + 0, size);
    // 256 bytes satisfies Metal's strictest constant-buffer offset alignment,
    // so an offset the guest picks against this is always bindable.
    (void)m.write_u64(out_requirements + 8, 256);
    (void)m.write_u32(out_requirements + 16, 0x1);  // only memory type 0 exists
}

void VulkanManager::impl_vkBindBufferMemory(Marshaler& m) {
    // VkResult vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory,
    //                             VkDeviceSize memoryOffset)
    usize index = 1;
    const u64 buffer = read_handle64(m, index);
    const u64 memory_handle = read_handle64(m, index);
    const u64 offset = read_handle64(m, index);

    auto buffer_it = m_buffers.find(buffer);
    auto memory_it = m_allocations.find(memory_handle);
    if (buffer_it == m_buffers.end() || memory_it == m_allocations.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    // Binding twice is a use error in Vulkan, and silently rebinding would
    // leave earlier descriptors pointing at the previous allocation.
    if (buffer_it->second.bound) {
        VITREO_TRACE("[vulkan] vkBindBufferMemory on an already-bound buffer\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    if (offset + buffer_it->second.size > memory_it->second.size) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    buffer_it->second.bound_memory = memory_handle;
    buffer_it->second.bound_offset = offset;
    buffer_it->second.bound = true;
    m.set_return(VK_SUCCESS);
}

// =============================================================================
// Images and image views
// =============================================================================

namespace {

/// VkFormat -> MTLPixelFormat for the formats this layer can back. Anything
/// absent returns 0, and image creation refuses rather than substituting a
/// near-miss format: a caller that asked for UNORM and silently received SRGB
/// gets wrong colours everywhere with nothing pointing at the cause.
struct FormatMapping {
    u32 vk_format;
    u32 metal_format;
    u32 bytes_per_pixel;
};

constexpr FormatMapping FORMAT_TABLE[] = {
    {  37, 70, 4},  // VK_FORMAT_R8G8B8A8_UNORM   -> MTLPixelFormatRGBA8Unorm
    {  43, 71, 4},  // VK_FORMAT_R8G8B8A8_SRGB    -> MTLPixelFormatRGBA8Unorm_sRGB
    {  44, 80, 4},  // VK_FORMAT_B8G8R8A8_UNORM   -> MTLPixelFormatBGRA8Unorm
    {  50, 81, 4},  // VK_FORMAT_B8G8R8A8_SRGB    -> MTLPixelFormatBGRA8Unorm_sRGB
    {   9, 10, 1},  // VK_FORMAT_R8_UNORM         -> MTLPixelFormatR8Unorm
    {  16, 30, 2},  // VK_FORMAT_R8G8_UNORM       -> MTLPixelFormatRG8Unorm
    { 109,115, 16}, // VK_FORMAT_R32G32B32A32_SFLOAT -> MTLPixelFormatRGBA32Float
};

[[nodiscard]] const FormatMapping* find_format(u32 vk_format) {
    for (const auto& entry : FORMAT_TABLE) {
        if (entry.vk_format == vk_format) return &entry;
    }
    return nullptr;
}

} // namespace

void VulkanManager::impl_vkCreateImage(Marshaler& m) {
    // VkResult vkCreateImage(VkDevice, const VkImageCreateInfo*,
    //                        const VkAllocationCallbacks*, VkImage*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr info = m.arg_ptr(1);
    const GuestAddr out_image = m.arg_ptr(3);
    if (out_image == 0 || info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_image, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // VkImageCreateInfo { sType, pNext, flags, imageType, format,
    //                     VkExtent3D extent{w,h,d}, mipLevels, arrayLayers,
    //                     samples, tiling, usage, ... }
    // On 32-bit: sType 0, pNext 4, flags 8, imageType 12, format 16,
    // extent 20..31, mipLevels 32, arrayLayers 36, samples 40, tiling 44,
    // usage 48. On 64-bit every field shifts by 4 from pNext being 8 wide.
    const u32 base = m.cpu_is_64bit() ? 20u : 16u;  // offset of `format`
    const auto format = m.memory().read_u32(info + base);
    const auto width = m.memory().read_u32(info + base + 4);
    const auto height = m.memory().read_u32(info + base + 8);
    const auto mips = m.memory().read_u32(info + base + 16);
    const auto layers = m.memory().read_u32(info + base + 20);
    const auto tiling = m.memory().read_u32(info + base + 28);
    const auto usage = m.memory().read_u32(info + base + 32);
    if (format.is_err() || width.is_err() || height.is_err()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    const FormatMapping* mapping = find_format(format.value());
    if (mapping == nullptr) {
        VITREO_TRACE("[vulkan] vkCreateImage: unsupported VkFormat %u\n",
                     format.value());
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    if (width.value() == 0 || height.value() == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    ImageState image;
    image.device = device;
    image.width = width.value();
    image.height = height.value();
    image.vk_format = format.value();
    image.metal_format = mapping->metal_format;
    image.usage = usage.is_ok() ? usage.value() : 0;
    // VK_IMAGE_TILING_LINEAR == 1. Linear images have a defined row-major
    // layout the guest may map and write; optimal tiling is opaque by
    // definition, so it does not need to alias guest-visible memory.
    image.linear_tiling = tiling.is_ok() && tiling.value() == 1;

    // Mip chains and array layers are not modelled: a buffer-backed texture
    // cannot carry them, and quietly ignoring the request would produce an
    // image missing levels the caller believes it has.
    if ((mips.is_ok() && mips.value() > 1) || (layers.is_ok() && layers.value() > 1)) {
        VITREO_TRACE("[vulkan] vkCreateImage: mipLevels/arrayLayers > 1 unsupported\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    const u64 handle = allocate_handle();
    m_images.emplace(handle, std::move(image));
    (void)m.write_u64(out_image, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyImage(Marshaler& m) {
    usize index = 1;
    const u64 image = read_handle64(m, index);
    // Views outliving their image would reference a destroyed texture.
    for (auto it = m_image_views.begin(); it != m_image_views.end();) {
        it = it->second.image == image ? m_image_views.erase(it) : std::next(it);
    }
    m_images.erase(image);
}

void VulkanManager::impl_vkGetImageMemoryRequirements(Marshaler& m) {
    usize index = 1;
    const u64 image = read_handle64(m, index);
    const GuestAddr out_requirements = static_cast<GuestAddr>(m.arg(index));
    if (out_requirements == 0) return;

    u64 size = 0;
    auto it = m_images.find(image);
    if (it != m_images.end()) {
        const FormatMapping* mapping = find_format(it->second.vk_format);
        const u32 bpp = mapping != nullptr ? mapping->bytes_per_pixel : 4;
        // Row pitch is aligned to 256, which is Metal's buffer-backed texture
        // requirement on Apple silicon. Reporting the unaligned size would
        // hand back an allocation that vkBindImageMemory then has to refuse.
        const u64 row_pitch = ((static_cast<u64>(it->second.width) * bpp) + 255) & ~255ULL;
        size = row_pitch * it->second.height;
    }
    (void)m.write_u64(out_requirements + 0, size);
    (void)m.write_u64(out_requirements + 8, 256);
    (void)m.write_u32(out_requirements + 16, 0x1);
}

void VulkanManager::impl_vkBindImageMemory(Marshaler& m) {
    // VkResult vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory,
    //                            VkDeviceSize memoryOffset)
    usize index = 1;
    const u64 image = read_handle64(m, index);
    const u64 memory_handle = read_handle64(m, index);
    const u64 offset = read_handle64(m, index);

    auto image_it = m_images.find(image);
    auto memory_it = m_allocations.find(memory_handle);
    if (image_it == m_images.end() || memory_it == m_allocations.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    if (image_it->second.bound) {
        VITREO_TRACE("[vulkan] vkBindImageMemory on an already-bound image\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    const FormatMapping* mapping = find_format(image_it->second.vk_format);
    const u32 bpp = mapping != nullptr ? mapping->bytes_per_pixel : 4;
    const u64 row_pitch =
        ((static_cast<u64>(image_it->second.width) * bpp) + 255) & ~255ULL;
    if (offset + row_pitch * image_it->second.height > memory_it->second.size) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    graphics::TextureDesc desc{};
    desc.width = image_it->second.width;
    desc.height = image_it->second.height;
    desc.pixel_format = image_it->second.metal_format;
    // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT == 0x10.
    desc.render_target = (image_it->second.usage & 0x10u) != 0;
    desc.usage = 0x01;  // MTLTextureUsageShaderRead

    if (image_it->second.linear_tiling) {
        // Alias the bound allocation so pixels the guest writes through a
        // mapping are the pixels the GPU samples.
        desc.storage_mode = 0;  // MTLStorageModeShared
        image_it->second.texture = std::make_unique<graphics::MetalTexture>(
            graphics::MetalDevice::instance().device_ptr(),
            memory_it->second.buffer->handle(), static_cast<usize>(offset),
            static_cast<usize>(row_pitch), desc);
    } else {
        // Optimal tiling has an implementation-defined layout, so Vulkan does
        // not permit the guest to interpret it; private storage is both legal
        // and faster.
        desc.storage_mode = 2;  // MTLStorageModePrivate
        image_it->second.texture = std::make_unique<graphics::MetalTexture>(
            graphics::MetalDevice::instance().device_ptr(), desc);
    }

    if (image_it->second.texture->handle() == nullptr) {
        image_it->second.texture.reset();
        VITREO_TRACE("[vulkan] vkBindImageMemory: texture creation failed\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    image_it->second.bound_memory = memory_handle;
    image_it->second.bound_offset = offset;
    image_it->second.bound = true;
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkCreateImageView(Marshaler& m) {
    // VkResult vkCreateImageView(VkDevice, const VkImageViewCreateInfo*,
    //                            const VkAllocationCallbacks*, VkImageView*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr info = m.arg_ptr(1);
    const GuestAddr out_view = m.arg_ptr(3);
    if (out_view == 0 || info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_view, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // VkImageViewCreateInfo { sType, pNext, flags, VkImage image, ... }
    // image is a 64-bit non-dispatchable handle, 8-aligned: +16 on 32-bit
    // (sType 0, pNext 4, flags 8, pad 12), +24 on 64-bit.
    const u32 image_offset = m.cpu_is_64bit() ? 24u : 16u;
    const auto image_result = m.memory().read_u64(info + image_offset);
    if (image_result.is_err()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }

    auto image_it = m_images.find(image_result.value());
    // A view onto an image with no storage would have nothing to reference;
    // Vulkan requires the image be bound before a view is created.
    if (image_it == m_images.end() || !image_it->second.bound) {
        VITREO_TRACE("[vulkan] vkCreateImageView: image missing or unbound\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    ImageViewState view;
    view.device = device;
    view.image = image_result.value();
    const u64 handle = allocate_handle();
    m_image_views.emplace(handle, view);
    (void)m.write_u64(out_view, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyImageView(Marshaler& m) {
    usize index = 1;
    const u64 view = read_handle64(m, index);
    m_image_views.erase(view);
}

// =============================================================================
// Render passes
// =============================================================================

void VulkanManager::impl_vkCreateRenderPass(Marshaler& m) {
    // VkResult vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo*,
    //                             const VkAllocationCallbacks*, VkRenderPass*)
    // Khronos: core 1.0. A loader that advertises Vulkan 1.0+ must resolve this
    // name; vkGetDeviceProcAddr returning null for a core command is what
    // sends volk-style tables into a jump to 0.
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_pass = m.arg_ptr(3);
    if (out_pass == 0 || create_info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_pass, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    RenderPassState pass;
    pass.device = device;
    const u64 handle = allocate_handle();
    m_render_passes.emplace(handle, pass);
    // VkRenderPass is non-dispatchable: always 64-bit, on both bitnesses.
    (void)m.write_u64(out_pass, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyRenderPass(Marshaler& m) {
    // vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*)
    usize index = 1;
    const u64 pass = read_handle64(m, index);
    if (pass == 0) return;
    m_render_passes.erase(pass);
}

void VulkanManager::impl_vkCreateShaderModule(Marshaler& m) {
    // VkResult vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*,
    //                               const VkAllocationCallbacks*, VkShaderModule*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_module = m.arg_ptr(3);
    if (out_module == 0 || create_info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_module, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    ShaderModuleState module;
    module.device = device;
    const u64 handle = allocate_handle();
    m_shader_modules.emplace(handle, module);
    (void)m.write_u64(out_module, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyShaderModule(Marshaler& m) {
    usize index = 1;
    const u64 module = read_handle64(m, index);
    if (module == 0) return;
    m_shader_modules.erase(module);
}

void VulkanManager::impl_vkCreatePipelineLayout(Marshaler& m) {
    // VkResult vkCreatePipelineLayout(VkDevice,
    //     const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*,
    //     VkPipelineLayout*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_layout = m.arg_ptr(3);
    if (out_layout == 0 || create_info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_layout, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    PipelineLayoutState layout;
    layout.device = device;
    const u64 handle = allocate_handle();
    m_pipeline_layouts.emplace(handle, layout);
    (void)m.write_u64(out_layout, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyPipelineLayout(Marshaler& m) {
    usize index = 1;
    const u64 layout = read_handle64(m, index);
    if (layout == 0) return;
    m_pipeline_layouts.erase(layout);
}

void VulkanManager::impl_vkCreateGraphicsPipelines(Marshaler& m) {
    // VkResult vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t,
    //     const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*,
    //     VkPipeline*)
    // VkPipelineCache is non-dispatchable (64-bit on both bitnesses), so on
    // x86 it occupies two stack slots and every later argument shifts.
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    usize index = 1;
    (void)read_handle64(m, index);  // pipelineCache, unused
    const u32 count = static_cast<u32>(m.arg(index++));
    (void)m.arg_ptr(index++);  // pCreateInfos
    (void)m.arg_ptr(index++);  // pAllocator
    const GuestAddr out_pipelines = m.arg_ptr(index++);
    if (out_pipelines == 0 || count == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        (void)m.write_u64(out_pipelines + static_cast<GuestAddr>(i) * 8u, 0);
    }
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    for (u32 i = 0; i < count; ++i) {
        PipelineState pipeline;
        pipeline.device = device;
        const u64 handle = allocate_handle();
        m_pipelines.emplace(handle, pipeline);
        (void)m.write_u64(out_pipelines + static_cast<GuestAddr>(i) * 8u, handle);
    }
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyPipeline(Marshaler& m) {
    usize index = 1;
    const u64 pipeline = read_handle64(m, index);
    if (pipeline == 0) return;
    m_pipelines.erase(pipeline);
}

void VulkanManager::impl_vkCreatePipelineCache(Marshaler& m) {
    // VkResult vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*,
    //                                const VkAllocationCallbacks*, VkPipelineCache*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_cache = m.arg_ptr(3);
    if (out_cache == 0 || create_info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_cache, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    PipelineCacheState cache;
    cache.device = device;
    const u64 handle = allocate_handle();
    m_pipeline_caches.emplace(handle, cache);
    (void)m.write_u64(out_cache, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyPipelineCache(Marshaler& m) {
    usize index = 1;
    const u64 cache = read_handle64(m, index);
    if (cache == 0) return;
    m_pipeline_caches.erase(cache);
}

void VulkanManager::impl_vkCreateFramebuffer(Marshaler& m) {
    // VkResult vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*,
    //                              const VkAllocationCallbacks*, VkFramebuffer*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_fb = m.arg_ptr(3);
    if (out_fb == 0 || create_info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_fb, 0);
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    FramebufferState fb;
    fb.device = device;
    const u64 handle = allocate_handle();
    m_framebuffers.emplace(handle, fb);
    (void)m.write_u64(out_fb, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyFramebuffer(Marshaler& m) {
    usize index = 1;
    const u64 fb = read_handle64(m, index);
    if (fb == 0) return;
    m_framebuffers.erase(fb);
}

void VulkanManager::impl_vkCreateDebugReportCallbackEXT(Marshaler& m) {
    // VkResult vkCreateDebugReportCallbackEXT(VkInstance,
    //     const VkDebugReportCallbackCreateInfoEXT*,
    //     const VkAllocationCallbacks*, VkDebugReportCallbackEXT*)
    const u64 instance = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_cb = m.arg_ptr(3);
    if (out_cb == 0 || create_info == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_cb, 0);
    if (!m_instance_created || instance != INSTANCE_HANDLE) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    const u64 handle = allocate_handle();
    m_debug_report_callbacks.emplace(handle, instance);
    (void)m.write_u64(out_cb, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyDebugReportCallbackEXT(Marshaler& m) {
    usize index = 1;
    const u64 cb = read_handle64(m, index);
    if (cb == 0) return;
    m_debug_report_callbacks.erase(cb);
}

// =============================================================================
// Command pools and buffers
// =============================================================================

void VulkanManager::impl_vkCreateCommandPool(Marshaler& m) {
    // VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*,
    //                              const VkAllocationCallbacks*, VkCommandPool*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr create_info = m.arg_ptr(1);
    const GuestAddr out_pool = m.arg_ptr(3);
    if (out_pool == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    (void)m.write_u64(out_pool, 0);

    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    CommandPoolState pool;
    pool.device = device;
    // VkCommandPoolCreateInfo::queueFamilyIndex follows sType, pNext, flags:
    // +16 on 64-bit, +12 on 32-bit.
    if (create_info != 0) {
        const u32 family_offset = m.cpu_is_64bit() ? 16u : 12u;
        const auto family = m.memory().read_u32(create_info + family_offset);
        if (family.is_ok()) pool.queue_family = family.value();
    }

    const u64 handle = allocate_handle();
    m_command_pools.emplace(handle, pool);
    // VkCommandPool is a non-dispatchable handle: always 64-bit, on both
    // bitnesses. Writing it as a pointer would truncate it on x86.
    (void)m.write_u64(out_pool, handle);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkDestroyCommandPool(Marshaler& m) {
    // vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*)
    usize index = 1;  // skip the device
    const u64 pool = read_handle64(m, index);
    // Destroying a pool frees every command buffer allocated from it.
    for (auto it = m_command_buffers.begin(); it != m_command_buffers.end();) {
        it = it->second.pool == pool ? m_command_buffers.erase(it) : std::next(it);
    }
    m_command_pools.erase(pool);
}

void VulkanManager::impl_vkResetCommandPool(Marshaler& m) {
    // vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags)
    usize index = 1;
    const u64 pool = read_handle64(m, index);
    if (m_command_pools.find(pool) == m_command_pools.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    // Resetting a pool returns every buffer from it to the initial state - it
    // does not free them, which is the distinction from vkDestroyCommandPool.
    for (auto& [handle, buffer] : m_command_buffers) {
        if (buffer.pool == pool) buffer.state = CommandBufferState::Initial;
    }
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkAllocateCommandBuffers(Marshaler& m) {
    // VkResult vkAllocateCommandBuffers(VkDevice,
    //     const VkCommandBufferAllocateInfo*, VkCommandBuffer*)
    const u64 device = static_cast<u64>(m.arg_ptr(0));
    const GuestAddr alloc_info = m.arg_ptr(1);
    const GuestAddr out_buffers = m.arg_ptr(2);
    if (alloc_info == 0 || out_buffers == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    if (m_devices.find(device) == m_devices.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    // VkCommandBufferAllocateInfo { sType, pNext, VkCommandPool commandPool,
    //   VkCommandBufferLevel level, uint32_t commandBufferCount }
    // commandPool is a non-dispatchable 64-bit handle, so it is 8-byte aligned:
    // +16 on 64-bit, +8 on 32-bit.
    const u32 pool_offset = m.cpu_is_64bit() ? 16u : 8u;
    const auto pool_result = m.memory().read_u64(alloc_info + pool_offset);
    const auto count_result = m.memory().read_u32(alloc_info + pool_offset + 12);
    if (pool_result.is_err() || count_result.is_err()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_OUT_OF_HOST_MEMORY)));
        return;
    }
    const u64 pool = pool_result.value();
    const u32 count = count_result.value();
    if (m_command_pools.find(pool) == m_command_pools.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }

    for (u32 index = 0; index < count; ++index) {
        CommandBufferState_ buffer;
        buffer.pool = pool;
        buffer.device = device;
        buffer.state = CommandBufferState::Initial;
        const u64 handle = allocate_handle();
        m_command_buffers.emplace(handle, buffer);
        // VkCommandBuffer is dispatchable, so it is pointer-width.
        (void)m.write_ptr(out_buffers + index * (m.cpu_is_64bit() ? 8u : 4u), handle);
    }
    VITREO_TRACE("[vulkan] vkAllocateCommandBuffers count=%u\n", count);
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkFreeCommandBuffers(Marshaler& m) {
    // void vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t count,
    //                           const VkCommandBuffer*)
    // The pool is 64-bit, so on x86 count and the array sit one slot later
    // than their parameter positions suggest.
    usize index = 1;
    (void)read_handle64(m, index);
    const u32 count = static_cast<u32>(m.arg(index++));
    const GuestAddr buffers = static_cast<GuestAddr>(m.arg(index));
    if (buffers == 0) return;
    const u32 stride = m.cpu_is_64bit() ? 8u : 4u;
    for (u32 slot = 0; slot < count; ++slot) {
        const auto handle = m.read_ptr(buffers + slot * stride);
        if (handle.is_ok()) m_command_buffers.erase(handle.value());
    }
}

void VulkanManager::impl_vkBeginCommandBuffer(Marshaler& m) {
    const u64 buffer = static_cast<u64>(m.arg_ptr(0));
    auto it = m_command_buffers.find(buffer);
    if (it == m_command_buffers.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    // Beginning a buffer that is already recording is a use error, not a
    // no-op: the application has lost track of its own state, and silently
    // accepting it hides that until the recorded commands come out wrong.
    if (it->second.state == CommandBufferState::Recording) {
        VITREO_TRACE("[vulkan] vkBeginCommandBuffer on a recording buffer\n");
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    it->second.state = CommandBufferState::Recording;
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkEndCommandBuffer(Marshaler& m) {
    const u64 buffer = static_cast<u64>(m.arg_ptr(0));
    auto it = m_command_buffers.find(buffer);
    if (it == m_command_buffers.end() ||
        it->second.state != CommandBufferState::Recording) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    it->second.state = CommandBufferState::Executable;
    m.set_return(VK_SUCCESS);
}

void VulkanManager::impl_vkResetCommandBuffer(Marshaler& m) {
    const u64 buffer = static_cast<u64>(m.arg_ptr(0));
    auto it = m_command_buffers.find(buffer);
    if (it == m_command_buffers.end()) {
        m.set_return(static_cast<u64>(static_cast<u32>(VK_ERROR_INITIALIZATION_FAILED)));
        return;
    }
    it->second.state = CommandBufferState::Initial;
    m.set_return(VK_SUCCESS);
}

// =============================================================================
// Thunk table
// =============================================================================

DllExports create_vulkan_thunks(VulkanManager& mgr) {
    DllExports exports;

    // Vulkan uses the C calling convention on Windows (VKAPI_CALL is __stdcall
    // on 32-bit), so stdcall_bytes must match each function's argument count.
    exports.entries.push_back({"vkGetInstanceProcAddr", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetInstanceProcAddr(m); }, 8});
    exports.entries.push_back({"vkEnumerateInstanceVersion", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEnumerateInstanceVersion(m); }, 4});
    exports.entries.push_back({"vkEnumerateInstanceExtensionProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEnumerateInstanceExtensionProperties(m); }, 12});
    exports.entries.push_back({"vkEnumerateInstanceLayerProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEnumerateInstanceLayerProperties(m); }, 8});
    exports.entries.push_back({"vkCreateInstance", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateInstance(m); }, 12});
    exports.entries.push_back({"vkDestroyInstance", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyInstance(m); }, 8});
    exports.entries.push_back({"vkEnumeratePhysicalDevices", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEnumeratePhysicalDevices(m); }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceProperties(m); }, 8});
    exports.entries.push_back({"vkGetPhysicalDeviceFeatures", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceFeatures(m); }, 8});
    exports.entries.push_back({"vkGetPhysicalDeviceQueueFamilyProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceQueueFamilyProperties(m); }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceMemoryProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceMemoryProperties(m); }, 8});
    exports.entries.push_back({"vkEnumerateDeviceExtensionProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEnumerateDeviceExtensionProperties(m); }, 16});
    exports.entries.push_back({"vkEnumerateDeviceLayerProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEnumerateDeviceLayerProperties(m); }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceFormatProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceFormatProperties(m); }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceImageFormatProperties", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceImageFormatProperties(m); }, 28});
    exports.entries.push_back({"vkGetPhysicalDeviceSparseImageFormatProperties", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceSparseImageFormatProperties(m);
        }, 32});
    exports.entries.push_back({"vkEnumeratePhysicalDeviceGroups", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEnumeratePhysicalDeviceGroups(m); }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceExternalBufferProperties", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceExternalBufferProperties(m);
        }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceExternalFenceProperties", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceExternalFenceProperties(m);
        }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceExternalSemaphoreProperties", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceExternalSemaphoreProperties(m);
        }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceFeatures2", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceFeatures2(m); }, 8});
    exports.entries.push_back({"vkGetPhysicalDeviceFormatProperties2", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceFormatProperties2(m);
        }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceImageFormatProperties2", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceImageFormatProperties2(m);
        }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceMemoryProperties2", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceMemoryProperties2(m);
        }, 8});
    exports.entries.push_back({"vkGetPhysicalDeviceProperties2", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetPhysicalDeviceProperties2(m); }, 8});
    exports.entries.push_back({"vkGetPhysicalDeviceQueueFamilyProperties2", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceQueueFamilyProperties2(m);
        }, 12});
    exports.entries.push_back({"vkGetPhysicalDeviceSparseImageFormatProperties2", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_vkGetPhysicalDeviceSparseImageFormatProperties2(m);
        }, 16});
    exports.entries.push_back({"vkCreateDevice", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateDevice(m); }, 16});
    exports.entries.push_back({"vkDestroyDevice", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyDevice(m); }, 8});
    exports.entries.push_back({"vkGetDeviceProcAddr", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetDeviceProcAddr(m); }, 8});
    exports.entries.push_back({"vkGetDeviceQueue", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetDeviceQueue(m); }, 16});
    exports.entries.push_back({"vkDeviceWaitIdle", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDeviceWaitIdle(m); }, 4});
    exports.entries.push_back({"vkQueueWaitIdle", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkQueueWaitIdle(m); }, 4});
    exports.entries.push_back({"vkQueueSubmit", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkQueueSubmit(m); }, 16});
    exports.entries.push_back({"vkCreateCommandPool", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateCommandPool(m); }, 16});
    exports.entries.push_back({"vkDestroyCommandPool", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyCommandPool(m); }, 16});
    exports.entries.push_back({"vkResetCommandPool", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkResetCommandPool(m); }, 16});
    exports.entries.push_back({"vkAllocateCommandBuffers", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkAllocateCommandBuffers(m); }, 12});
    exports.entries.push_back({"vkFreeCommandBuffers", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkFreeCommandBuffers(m); }, 20});
    exports.entries.push_back({"vkBeginCommandBuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkBeginCommandBuffer(m); }, 8});
    exports.entries.push_back({"vkEndCommandBuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkEndCommandBuffer(m); }, 4});
    exports.entries.push_back({"vkResetCommandBuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkResetCommandBuffer(m); }, 8});

    // Memory and buffers. The x86 byte counts include the extra slot each
    // 64-bit VkDeviceSize / non-dispatchable handle occupies.
    exports.entries.push_back({"vkAllocateMemory", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkAllocateMemory(m); }, 16});
    exports.entries.push_back({"vkFreeMemory", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkFreeMemory(m); }, 16});
    exports.entries.push_back({"vkMapMemory", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkMapMemory(m); }, 36});
    exports.entries.push_back({"vkUnmapMemory", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkUnmapMemory(m); }, 12});
    exports.entries.push_back({"vkFlushMappedMemoryRanges", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkFlushMappedMemoryRanges(m); }, 12});
    exports.entries.push_back({"vkInvalidateMappedMemoryRanges", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkInvalidateMappedMemoryRanges(m); }, 12});
    exports.entries.push_back({"vkCreateBuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateBuffer(m); }, 16});
    exports.entries.push_back({"vkDestroyBuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyBuffer(m); }, 16});
    exports.entries.push_back({"vkGetBufferMemoryRequirements", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetBufferMemoryRequirements(m); }, 16});
    exports.entries.push_back({"vkBindBufferMemory", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkBindBufferMemory(m); }, 28});

    // Images and image views.
    exports.entries.push_back({"vkCreateImage", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateImage(m); }, 16});
    exports.entries.push_back({"vkDestroyImage", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyImage(m); }, 16});
    exports.entries.push_back({"vkGetImageMemoryRequirements", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkGetImageMemoryRequirements(m); }, 16});
    exports.entries.push_back({"vkBindImageMemory", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkBindImageMemory(m); }, 28});
    exports.entries.push_back({"vkCreateImageView", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateImageView(m); }, 16});
    exports.entries.push_back({"vkDestroyImageView", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyImageView(m); }, 16});
    exports.entries.push_back({"vkCreateRenderPass", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateRenderPass(m); }, 16});
    exports.entries.push_back({"vkDestroyRenderPass", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyRenderPass(m); }, 16});
    exports.entries.push_back({"vkCreateShaderModule", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateShaderModule(m); }, 16});
    exports.entries.push_back({"vkDestroyShaderModule", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyShaderModule(m); }, 16});
    exports.entries.push_back({"vkCreatePipelineLayout", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreatePipelineLayout(m); }, 16});
    exports.entries.push_back({"vkDestroyPipelineLayout", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyPipelineLayout(m); }, 16});
    // VkPipelineCache is 64-bit, so x86 stdcall is 4+8+4+4+4+4 = 28.
    exports.entries.push_back({"vkCreateGraphicsPipelines", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateGraphicsPipelines(m); }, 28});
    exports.entries.push_back({"vkDestroyPipeline", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyPipeline(m); }, 16});
    exports.entries.push_back({"vkCreatePipelineCache", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreatePipelineCache(m); }, 16});
    exports.entries.push_back({"vkDestroyPipelineCache", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyPipelineCache(m); }, 16});
    exports.entries.push_back({"vkCreateFramebuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateFramebuffer(m); }, 16});
    exports.entries.push_back({"vkDestroyFramebuffer", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyFramebuffer(m); }, 16});
    exports.entries.push_back({"vkCreateDebugReportCallbackEXT", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkCreateDebugReportCallbackEXT(m); }, 16});
    exports.entries.push_back({"vkDestroyDebugReportCallbackEXT", 0,
        [&mgr](Marshaler& m) { mgr.impl_vkDestroyDebugReportCallbackEXT(m); }, 16});

    return exports;
}

} // namespace vitreo::winapi
