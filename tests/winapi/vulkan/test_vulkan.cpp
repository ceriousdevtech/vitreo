/// @file test_vulkan.cpp
/// @brief Unit tests for the vulkan-1.dll loader and physical-device layer.
///
/// The entry points read their arguments through Marshaler, which pulls them
/// from the guest stack (x86 stdcall) or registers (x64). The fixture below
/// pushes arguments the same way a guest caller would, so these tests exercise
/// the real marshaling path rather than calling past it.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <vitreo/common.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/graphics/vulkan/vulkan.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

class VulkanManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MEM_BASE, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK_BASE, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.init_protected_mode();
        mgr_.set_memory(&memory_);
    }

    /// Lay out a 32-bit stdcall frame: ESP points at a return address, with
    /// arguments following at ESP+4. Mirrors what the guest pushes.
    void set_args(const std::vector<u32>& args) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        (void)memory_.write_u32(frame, 0xDEADBEEF);  // return address
        for (usize index = 0; index < args.size(); ++index) {
            (void)memory_.write_u32(
                frame + 4 + static_cast<GuestAddr>(index * 4), args[index]);
        }
    }

    [[nodiscard]] Marshaler make_marshaler() {
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        return m;
    }

    [[nodiscard]] u32 read_u32(GuestAddr addr) {
        auto result = memory_.read_u32(addr);
        return result.is_ok() ? result.value() : 0xFFFFFFFFu;
    }

    [[nodiscard]] u32 return_value() const {
        return static_cast<u32>(cpu_.gpr.rax & 0xFFFFFFFFu);
    }

    void write_ansi(GuestAddr addr, const std::string& text) {
        (void)memory_.write_bytes(addr,
                                  reinterpret_cast<const Byte*>(text.c_str()),
                                  text.size() + 1);
    }

    /// The physical-device handle the manager hands out, discovered the same
    /// way a guest would rather than hard-coded from the implementation.
    [[nodiscard]] u32 physical_device() {
        (void)memory_.write_u32(OUT_ADDR, 1);
        set_args({0, OUT_ADDR, STRUCT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkEnumeratePhysicalDevices(m);
        return read_u32(STRUCT_ADDR);
    }

    [[nodiscard]] bool create_device() {
        set_args({physical_device(), 0, 0, OUT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkCreateDevice(m);
        device_handle_ = read_u32(OUT_ADDR);
        return return_value() == VK_SUCCESS && device_handle_ != 0;
    }

    [[nodiscard]] u64 create_command_pool() {
        std::vector<u8> info(32, 0);
        (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
        set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkCreateCommandPool(m);
        if (return_value() != VK_SUCCESS) return 0;
        auto handle = memory_.read_u64(OUT_ADDR);
        return handle.is_ok() ? handle.value() : 0;
    }

    [[nodiscard]] u64 allocate_command_buffer(u64 pool) {
        // VkCommandBufferAllocateInfo on 32-bit: sType +0, pNext +4,
        // commandPool +8 (64-bit, 8-aligned), level +16, count +20.
        std::vector<u8> info(32, 0);
        (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
        (void)memory_.write_u64(STRUCT_ADDR + 8, pool);
        (void)memory_.write_u32(STRUCT_ADDR + 20, 1);  // commandBufferCount

        set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, OUT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkAllocateCommandBuffers(m);
        if (return_value() != VK_SUCCESS) return 0;
        return read_u32(OUT_ADDR);
    }

    [[nodiscard]] u64 allocate_memory(u64 size) {
        // VkMemoryAllocateInfo on 32-bit: sType +0, pNext +4,
        // allocationSize +8 (64-bit, 8-aligned), memoryTypeIndex +16.
        std::vector<u8> info(32, 0);
        (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
        (void)memory_.write_u64(STRUCT_ADDR + 8, size);
        (void)memory_.write_u32(STRUCT_ADDR + 16, 0);

        set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkAllocateMemory(m);
        if (return_value() != VK_SUCCESS) return 0;
        auto handle = memory_.read_u64(OUT_ADDR);
        return handle.is_ok() ? handle.value() : 0;
    }

    /// vkMapMemory takes three 64-bit values, so on x86 each occupies two
    /// stack slots and everything after shifts accordingly.
    [[nodiscard]] GuestAddr map_memory(u64 memory_handle, u64 offset, u64 size) {
        set_args({static_cast<u32>(device_handle_),
                  static_cast<u32>(memory_handle & 0xFFFFFFFFu),
                  static_cast<u32>(memory_handle >> 32),
                  static_cast<u32>(offset & 0xFFFFFFFFu),
                  static_cast<u32>(offset >> 32),
                  static_cast<u32>(size & 0xFFFFFFFFu),
                  static_cast<u32>(size >> 32),
                  0,          // flags
                  OUT_ADDR});  // ppData
        auto m = make_marshaler();
        mgr_.impl_vkMapMemory(m);
        if (return_value() != VK_SUCCESS) return 0;
        return static_cast<GuestAddr>(read_u32(OUT_ADDR));
    }

    [[nodiscard]] u64 create_buffer(u64 size) {
        // VkBufferCreateInfo on 32-bit: sType +0, pNext +4, flags +8,
        // pad +12, size +16 (64-bit, 8-aligned), usage +24.
        std::vector<u8> info(48, 0);
        (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
        (void)memory_.write_u64(STRUCT_ADDR + 16, size);

        set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkCreateBuffer(m);
        if (return_value() != VK_SUCCESS) return 0;
        auto handle = memory_.read_u64(OUT_ADDR);
        return handle.is_ok() ? handle.value() : 0;
    }

    [[nodiscard]] bool bind_buffer(u64 buffer, u64 memory_handle, u64 offset) {
        set_args({static_cast<u32>(device_handle_),
                  static_cast<u32>(buffer & 0xFFFFFFFFu),
                  static_cast<u32>(buffer >> 32),
                  static_cast<u32>(memory_handle & 0xFFFFFFFFu),
                  static_cast<u32>(memory_handle >> 32),
                  static_cast<u32>(offset & 0xFFFFFFFFu),
                  static_cast<u32>(offset >> 32)});
        auto m = make_marshaler();
        mgr_.impl_vkBindBufferMemory(m);
        return return_value() == VK_SUCCESS;
    }

    /// VkImageCreateInfo on 32-bit: sType 0, pNext 4, flags 8, imageType 12,
    /// format 16, extent 20..31, mipLevels 32, arrayLayers 36, samples 40,
    /// tiling 44, usage 48.
    [[nodiscard]] u64 create_image(u32 width, u32 height, u32 vk_format,
                                   bool linear) {
        std::vector<u8> info(80, 0);
        (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
        (void)memory_.write_u32(STRUCT_ADDR + 16, vk_format);
        (void)memory_.write_u32(STRUCT_ADDR + 20, width);
        (void)memory_.write_u32(STRUCT_ADDR + 24, height);
        (void)memory_.write_u32(STRUCT_ADDR + 28, 1);   // extent.depth
        (void)memory_.write_u32(STRUCT_ADDR + 32, 1);   // mipLevels
        (void)memory_.write_u32(STRUCT_ADDR + 36, 1);   // arrayLayers
        (void)memory_.write_u32(STRUCT_ADDR + 44, linear ? 1u : 0u);  // tiling
        (void)memory_.write_u32(STRUCT_ADDR + 48, 0x10);  // COLOR_ATTACHMENT

        set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkCreateImage(m);
        if (return_value() != VK_SUCCESS) return 0;
        auto handle = memory_.read_u64(OUT_ADDR);
        return handle.is_ok() ? handle.value() : 0;
    }

    [[nodiscard]] bool bind_image(u64 image, u64 memory_handle, u64 offset) {
        set_args({static_cast<u32>(device_handle_),
                  static_cast<u32>(image & 0xFFFFFFFFu),
                  static_cast<u32>(image >> 32),
                  static_cast<u32>(memory_handle & 0xFFFFFFFFu),
                  static_cast<u32>(memory_handle >> 32),
                  static_cast<u32>(offset & 0xFFFFFFFFu),
                  static_cast<u32>(offset >> 32)});
        auto m = make_marshaler();
        mgr_.impl_vkBindImageMemory(m);
        return return_value() == VK_SUCCESS;
    }

    /// VkImageViewCreateInfo on 32-bit: sType 0, pNext 4, flags 8, pad 12,
    /// image 16 (64-bit, 8-aligned).
    [[nodiscard]] u64 create_image_view(u64 image) {
        std::vector<u8> info(80, 0);
        (void)memory_.write_bytes(STRUCT_ADDR + 0x200, info.data(), info.size());
        (void)memory_.write_u64(STRUCT_ADDR + 0x200 + 16, image);

        set_args({static_cast<u32>(device_handle_), STRUCT_ADDR + 0x200, 0,
                  OUT_ADDR});
        auto m = make_marshaler();
        mgr_.impl_vkCreateImageView(m);
        if (return_value() != VK_SUCCESS) return 0;
        auto handle = memory_.read_u64(OUT_ADDR);
        return handle.is_ok() ? handle.value() : 0;
    }

    u64 device_handle_{0};

    static constexpr GuestAddr MEM_BASE = 0x10000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr OUT_ADDR = MEM_BASE + 0x100;
    static constexpr GuestAddr NAME_ADDR = MEM_BASE + 0x200;
    static constexpr GuestAddr STRUCT_ADDR = MEM_BASE + 0x1000;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    VulkanManager mgr_;
};

// -----------------------------------------------------------------------------
// Loader
// -----------------------------------------------------------------------------

TEST_F(VulkanManagerTest, EnumerateInstanceVersionReportsOnePointTwo) {
    set_args({OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkEnumerateInstanceVersion(m);

    EXPECT_EQ(return_value(), VK_SUCCESS);
    // VK_MAKE_API_VERSION(0, 1, 2, 0): major 1, minor 2.
    const u32 version = read_u32(OUT_ADDR);
    EXPECT_EQ((version >> 22) & 0x7F, 1u) << "major version";
    EXPECT_EQ((version >> 12) & 0x3FF, 2u) << "minor version";
}

TEST_F(VulkanManagerTest, EnumerateInstanceVersionRejectsNullOutput) {
    set_args({0});
    auto m = make_marshaler();
    mgr_.impl_vkEnumerateInstanceVersion(m);
    EXPECT_NE(return_value(), VK_SUCCESS);
}

TEST_F(VulkanManagerTest, NoInstanceExtensionsOrLayersAreClaimed) {
    // Claiming an extension we cannot back would strand the caller later, at
    // surface or swapchain creation, rather than here.
    set_args({0, OUT_ADDR, 0});
    auto ext = make_marshaler();
    mgr_.impl_vkEnumerateInstanceExtensionProperties(ext);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_EQ(read_u32(OUT_ADDR), 0u);

    set_args({OUT_ADDR, 0});
    auto layers = make_marshaler();
    mgr_.impl_vkEnumerateInstanceLayerProperties(layers);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_EQ(read_u32(OUT_ADDR), 0u);
}

TEST_F(VulkanManagerTest, GetInstanceProcAddrReturnsNullWithoutRegistry) {
    // No registry attached: there are no guest-callable addresses to hand out,
    // and null is the documented "not provided" answer. Returning anything
    // else would send the caller jumping into a non-address.
    write_ansi(NAME_ADDR, "vkCreateInstance");
    set_args({0, NAME_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetInstanceProcAddr(m);
    EXPECT_EQ(return_value(), 0u);
}

TEST_F(VulkanManagerTest, GetInstanceProcAddrRejectsNullName) {
    set_args({0, 0});
    auto m = make_marshaler();
    mgr_.impl_vkGetInstanceProcAddr(m);
    EXPECT_EQ(return_value(), 0u);
}

// -----------------------------------------------------------------------------
// Instance
// -----------------------------------------------------------------------------

TEST_F(VulkanManagerTest, CreateInstanceSucceedsAndWritesHandle) {
    set_args({0, 0, OUT_ADDR});  // null create info is tolerated
    auto m = make_marshaler();
    mgr_.impl_vkCreateInstance(m);

    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_NE(read_u32(OUT_ADDR), 0u) << "instance handle must be non-null";
}

TEST_F(VulkanManagerTest, CreateInstanceRejectsNullOutputHandle) {
    set_args({0, 0, 0});
    auto m = make_marshaler();
    mgr_.impl_vkCreateInstance(m);
    EXPECT_NE(return_value(), VK_SUCCESS);
}

TEST_F(VulkanManagerTest, CreateInstanceRefusesRequestedExtensions) {
    // VkInstanceCreateInfo::enabledExtensionCount is at offset 20 on 32-bit.
    // Accepting an extension we do not implement is the failure mode this
    // guards: the caller would proceed believing it had a surface.
    std::vector<u8> create_info(64, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, create_info.data(), create_info.size());
    (void)memory_.write_u32(STRUCT_ADDR + 20, 1);  // enabledExtensionCount

    set_args({STRUCT_ADDR, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreateInstance(m);

    EXPECT_EQ(static_cast<i32>(return_value()), VK_ERROR_EXTENSION_NOT_PRESENT);
}

// -----------------------------------------------------------------------------
// Physical device
// -----------------------------------------------------------------------------

TEST_F(VulkanManagerTest, EnumeratePhysicalDevicesCountQuery) {
    // Passing a null array is the "how many are there" form, and must not
    // touch the array pointer.
    set_args({0, OUT_ADDR, 0});
    auto m = make_marshaler();
    mgr_.impl_vkEnumeratePhysicalDevices(m);

    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_EQ(read_u32(OUT_ADDR), 1u) << "one device: the Metal GPU";
}

TEST_F(VulkanManagerTest, EnumeratePhysicalDevicesFillsHandle) {
    (void)memory_.write_u32(OUT_ADDR, 1);  // capacity
    set_args({0, OUT_ADDR, STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkEnumeratePhysicalDevices(m);

    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_EQ(read_u32(OUT_ADDR), 1u);
    EXPECT_NE(read_u32(STRUCT_ADDR), 0u) << "device handle must be non-null";
}

TEST_F(VulkanManagerTest, EnumeratePhysicalDevicesReportsIncompleteOnShortBuffer) {
    (void)memory_.write_u32(OUT_ADDR, 0);  // capacity zero, array non-null
    set_args({0, OUT_ADDR, STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkEnumeratePhysicalDevices(m);

    // VK_INCOMPLETE is part of the contract, not an error: it tells the caller
    // to re-query with a larger buffer.
    EXPECT_EQ(return_value(), VK_INCOMPLETE);
}

TEST_F(VulkanManagerTest, PhysicalDevicePropertiesReportRealDeviceName) {
    set_args({0, STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceProperties(m);

    EXPECT_EQ(read_u32(STRUCT_ADDR + VK_PDP_OFFSET_VENDOR_ID), VK_VENDOR_ID_APPLE);
    EXPECT_EQ(read_u32(STRUCT_ADDR + VK_PDP_OFFSET_DEVICE_TYPE),
              VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        << "Apple silicon shares memory between CPU and GPU";
    EXPECT_EQ(read_u32(STRUCT_ADDR + VK_PDP_OFFSET_API_VERSION),
              VK_API_VERSION_1_2);

    std::vector<u8> name(VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, 0);
    ASSERT_TRUE(memory_.read_bytes(STRUCT_ADDR + VK_PDP_OFFSET_DEVICE_NAME,
                                   reinterpret_cast<Byte*>(name.data()),
                                   name.size()).is_ok());
    const std::string device_name(reinterpret_cast<const char*>(name.data()));
    EXPECT_FALSE(device_name.empty()) << "device name must be populated";
    EXPECT_LT(device_name.size(), VK_MAX_PHYSICAL_DEVICE_NAME_SIZE)
        << "name must be null-terminated within the array";
}

TEST_F(VulkanManagerTest, PhysicalDeviceLimitsAreLeftZeroRatherThanInvented) {
    // Deliberate: nothing enforces a limit yet, and a fabricated number would
    // be sized against by a caller. Zero is obviously unusable; 4096 is not.
    set_args({0, STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceProperties(m);

    for (u32 offset = 0; offset < 64; offset += 4) {
        EXPECT_EQ(read_u32(STRUCT_ADDR + VK_PDP_OFFSET_LIMITS + offset), 0u)
            << "limit at +" << offset << " should be zero until it is backed";
    }
}

TEST_F(VulkanManagerTest, QueueFamilyReportsOneCombinedFamily) {
    set_args({0, OUT_ADDR, 0});
    auto count = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceQueueFamilyProperties(count);
    EXPECT_EQ(read_u32(OUT_ADDR), 1u);

    (void)memory_.write_u32(OUT_ADDR, 1);
    set_args({0, OUT_ADDR, STRUCT_ADDR});
    auto fill = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceQueueFamilyProperties(fill);

    // Metal's single queue serves graphics, compute and blit.
    const u32 flags = read_u32(STRUCT_ADDR);
    EXPECT_TRUE(flags & VK_QUEUE_GRAPHICS_BIT);
    EXPECT_TRUE(flags & VK_QUEUE_COMPUTE_BIT);
    EXPECT_TRUE(flags & VK_QUEUE_TRANSFER_BIT);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 4), 1u) << "queueCount";
    EXPECT_EQ(read_u32(STRUCT_ADDR + 12), 1u) << "transfer granularity width";
}

TEST_F(VulkanManagerTest, MemoryPropertiesDescribeUnifiedMemory) {
    set_args({0, STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceMemoryProperties(m);

    EXPECT_EQ(read_u32(STRUCT_ADDR), 1u) << "memoryTypeCount";
    const u32 flags = read_u32(STRUCT_ADDR + 4);
    // Unified memory is genuinely device-local and host-visible at once; this
    // is an accurate description rather than a simplification.
    EXPECT_TRUE(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    EXPECT_TRUE(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    EXPECT_TRUE(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

TEST_F(VulkanManagerTest, PhysicalDeviceProperties2WrapsBaseProperties) {
    const u32 device = physical_device();
    std::vector<u8> props(VK_PDP_SIZE + 8, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, props.data(), props.size());
    (void)memory_.write_u32(STRUCT_ADDR, 1000059001);  // structure type
    (void)memory_.write_u32(STRUCT_ADDR + 4, 0x12345678);  // pNext

    set_args({device, STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceProperties2(m);

    EXPECT_EQ(read_u32(STRUCT_ADDR), 1000059001u);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 4), 0x12345678u);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 8 + VK_PDP_OFFSET_API_VERSION),
              VK_API_VERSION_1_2);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 8 + VK_PDP_OFFSET_VENDOR_ID),
              VK_VENDOR_ID_APPLE);
}

TEST_F(VulkanManagerTest, QueueFamilyProperties2WrapsCombinedMetalQueue) {
    const u32 device = physical_device();
    (void)memory_.write_u32(OUT_ADDR, 1);
    std::vector<u8> props(32, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, props.data(), props.size());
    (void)memory_.write_u32(STRUCT_ADDR, 1000059005);  // structure type

    set_args({device, OUT_ADDR, STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceQueueFamilyProperties2(m);

    EXPECT_EQ(read_u32(OUT_ADDR), 1u);
    EXPECT_EQ(read_u32(STRUCT_ADDR), 1000059005u);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 8),
              VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                  VK_QUEUE_TRANSFER_BIT);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 12), 1u);
}

TEST_F(VulkanManagerTest, DeviceGroupsContainTheSinglePhysicalDevice) {
    const u32 expected_device = physical_device();
    set_args({0, OUT_ADDR, 0});
    auto count = make_marshaler();
    mgr_.impl_vkEnumeratePhysicalDeviceGroups(count);
    ASSERT_EQ(return_value(), VK_SUCCESS);
    ASSERT_EQ(read_u32(OUT_ADDR), 1u);

    (void)memory_.write_u32(OUT_ADDR, 1);
    std::vector<u8> group(144, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, group.data(), group.size());
    (void)memory_.write_u32(STRUCT_ADDR, 1000070000);  // structure type
    set_args({0, OUT_ADDR, STRUCT_ADDR});
    auto fill = make_marshaler();
    mgr_.impl_vkEnumeratePhysicalDeviceGroups(fill);

    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_EQ(read_u32(STRUCT_ADDR), 1000070000u);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 8), 1u);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 12), expected_device);
    EXPECT_EQ(read_u32(STRUCT_ADDR + 140), 0u);
}

TEST_F(VulkanManagerTest, UnsupportedFormatsAndExtensionsFailHonestly) {
    (void)memory_.write_u32(OUT_ADDR, 99);
    set_args({physical_device(), 0, OUT_ADDR, STRUCT_ADDR});
    auto extensions = make_marshaler();
    mgr_.impl_vkEnumerateDeviceExtensionProperties(extensions);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_EQ(read_u32(OUT_ADDR), 0u);

    std::vector<u8> properties(32, 0xCC);
    (void)memory_.write_bytes(STRUCT_ADDR, properties.data(), properties.size());
    set_args({physical_device(), 44, 2, 0, 0, 0, STRUCT_ADDR});
    auto image_format = make_marshaler();
    mgr_.impl_vkGetPhysicalDeviceImageFormatProperties(image_format);
    EXPECT_EQ(static_cast<i32>(return_value()),
              VK_ERROR_FORMAT_NOT_SUPPORTED);
    for (u32 offset = 0; offset < 32; offset += 4) {
        EXPECT_EQ(read_u32(STRUCT_ADDR + offset), 0u);
    }
}

TEST_F(VulkanManagerTest, ThunksExposeRequiredOnePointTwoTopologySurface) {
    const auto exports = create_vulkan_thunks(mgr_);
    const std::vector<std::string> required = {
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
    };
    for (const auto& name : required) {
        EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const auto& entry) {
                                   return entry.name == name;
                               }),
                  exports.entries.end())
            << name;
    }
}

// -----------------------------------------------------------------------------
// Logical device
// -----------------------------------------------------------------------------

TEST_F(VulkanManagerTest, CreateDeviceRejectsUnknownPhysicalDevice) {
    (void)memory_.write_u32(OUT_ADDR, 0xCCCCCCCC);  // poison
    set_args({0xBADBAD, 0, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreateDevice(m);

    EXPECT_EQ(static_cast<i32>(return_value()), VK_ERROR_INITIALIZATION_FAILED);
    EXPECT_EQ(read_u32(OUT_ADDR), 0u) << "handle must be cleared on failure";
}

TEST_F(VulkanManagerTest, CreateDeviceRefusesRequestedExtensions) {
    // VkDeviceCreateInfo::enabledExtensionCount is at +28 on 32-bit. Accepting
    // an extension we do not implement would let the caller proceed believing
    // it had, say, a swapchain.
    std::vector<u8> info(64, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
    (void)memory_.write_u32(STRUCT_ADDR + 28, 1);

    set_args({physical_device(), STRUCT_ADDR, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreateDevice(m);
    EXPECT_EQ(static_cast<i32>(return_value()), VK_ERROR_EXTENSION_NOT_PRESENT);
}

// -----------------------------------------------------------------------------
// Device, queue and command buffers
// -----------------------------------------------------------------------------

TEST_F(VulkanManagerTest, CreateDeviceSucceedsOnTheRealPhysicalDevice) {
    EXPECT_EQ(create_device(), true);
    EXPECT_NE(device_handle_, 0u);
}

TEST_F(VulkanManagerTest, GetDeviceQueueReturnsQueueForFamilyZero) {
    ASSERT_TRUE(create_device());
    set_args({static_cast<u32>(device_handle_), 0, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetDeviceQueue(m);
    EXPECT_NE(read_u32(OUT_ADDR), 0u);
}

TEST_F(VulkanManagerTest, GetDeviceQueueRejectsFamiliesThatWereNeverAdvertised) {
    ASSERT_TRUE(create_device());
    // Only family 0 / queue 0 exists, per the queue-family properties. Handing
    // back a queue for family 3 would contradict our own enumeration.
    set_args({static_cast<u32>(device_handle_), 3, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetDeviceQueue(m);
    EXPECT_EQ(read_u32(OUT_ADDR), 0u);
}

TEST_F(VulkanManagerTest, CommandPoolAndBufferRoundTrip) {
    ASSERT_TRUE(create_device());
    const u64 pool = create_command_pool();
    ASSERT_NE(pool, 0u);

    const u64 buffer = allocate_command_buffer(pool);
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(mgr_.command_buffer_count(), 1u);
}

TEST_F(VulkanManagerTest, CommandBufferStateMachineIsEnforced) {
    ASSERT_TRUE(create_device());
    const u64 buffer = allocate_command_buffer(create_command_pool());
    ASSERT_NE(buffer, 0u);

    // Ending a buffer that never began is a use error, not a no-op.
    set_args({static_cast<u32>(buffer)});
    auto premature_end = make_marshaler();
    mgr_.impl_vkEndCommandBuffer(premature_end);
    EXPECT_NE(return_value(), VK_SUCCESS);

    set_args({static_cast<u32>(buffer), 0});
    auto begin = make_marshaler();
    mgr_.impl_vkBeginCommandBuffer(begin);
    EXPECT_EQ(return_value(), VK_SUCCESS);

    // Beginning twice means the app has lost track of its own state; accepting
    // it silently hides that until the recorded commands come out wrong.
    set_args({static_cast<u32>(buffer), 0});
    auto double_begin = make_marshaler();
    mgr_.impl_vkBeginCommandBuffer(double_begin);
    EXPECT_NE(return_value(), VK_SUCCESS);

    set_args({static_cast<u32>(buffer)});
    auto end = make_marshaler();
    mgr_.impl_vkEndCommandBuffer(end);
    EXPECT_EQ(return_value(), VK_SUCCESS);
}

TEST_F(VulkanManagerTest, ResetCommandPoolReturnsBuffersToInitialWithoutFreeing) {
    ASSERT_TRUE(create_device());
    const u64 pool = create_command_pool();
    const u64 buffer = allocate_command_buffer(pool);
    ASSERT_NE(buffer, 0u);

    set_args({static_cast<u32>(buffer), 0});
    auto begin = make_marshaler();
    mgr_.impl_vkBeginCommandBuffer(begin);
    ASSERT_EQ(return_value(), VK_SUCCESS);

    // A 64-bit pool handle occupies two x86 stack slots; passing it as one
    // would reset nothing and silently succeed.
    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(pool & 0xFFFFFFFFu),
              static_cast<u32>(pool >> 32), 0});
    auto reset = make_marshaler();
    mgr_.impl_vkResetCommandPool(reset);
    EXPECT_EQ(return_value(), VK_SUCCESS);

    // Reset returns buffers to Initial but does not free them - that is the
    // distinction from vkDestroyCommandPool.
    EXPECT_EQ(mgr_.command_buffer_count(), 1u);
    set_args({static_cast<u32>(buffer), 0});
    auto begin_again = make_marshaler();
    mgr_.impl_vkBeginCommandBuffer(begin_again);
    EXPECT_EQ(return_value(), VK_SUCCESS) << "buffer should be recordable again";
}

TEST_F(VulkanManagerTest, DestroyCommandPoolFreesItsBuffers) {
    ASSERT_TRUE(create_device());
    const u64 pool = create_command_pool();
    ASSERT_NE(allocate_command_buffer(pool), 0u);
    ASSERT_EQ(mgr_.command_buffer_count(), 1u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(pool & 0xFFFFFFFFu),
              static_cast<u32>(pool >> 32), 0});
    auto m = make_marshaler();
    mgr_.impl_vkDestroyCommandPool(m);
    EXPECT_EQ(mgr_.command_buffer_count(), 0u)
        << "destroying a pool frees every buffer allocated from it";
}

TEST_F(VulkanManagerTest, DestroyDeviceDropsItsPoolsAndBuffers) {
    ASSERT_TRUE(create_device());
    ASSERT_NE(allocate_command_buffer(create_command_pool()), 0u);
    ASSERT_EQ(mgr_.device_count(), 1u);

    set_args({static_cast<u32>(device_handle_), 0});
    auto m = make_marshaler();
    mgr_.impl_vkDestroyDevice(m);
    EXPECT_EQ(mgr_.device_count(), 0u);
    EXPECT_EQ(mgr_.command_buffer_count(), 0u);
}

// -----------------------------------------------------------------------------
// Device memory and buffers
// -----------------------------------------------------------------------------

TEST_F(VulkanManagerTest, AllocateMemorySucceedsAndFreeReleasesIt) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(4096);
    ASSERT_NE(memory, 0u);
    EXPECT_EQ(mgr_.allocation_count(), 1u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(memory & 0xFFFFFFFFu),
              static_cast<u32>(memory >> 32), 0});
    auto m = make_marshaler();
    mgr_.impl_vkFreeMemory(m);
    EXPECT_EQ(mgr_.allocation_count(), 0u);
}

TEST_F(VulkanManagerTest, AllocateMemoryRejectsUnadvertisedMemoryType) {
    ASSERT_TRUE(create_device());
    // Only memory type 0 is advertised; anything else means the caller ignored
    // its own vkGetPhysicalDeviceMemoryProperties result.
    std::vector<u8> info(32, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
    (void)memory_.write_u64(STRUCT_ADDR + 8, 4096);
    (void)memory_.write_u32(STRUCT_ADDR + 16, 3);  // memoryTypeIndex

    set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkAllocateMemory(m);
    EXPECT_NE(return_value(), VK_SUCCESS);
}

TEST_F(VulkanManagerTest, MappedMemoryIsGuestWritableAndZeroCopy) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(4096);
    ASSERT_NE(memory, 0u);

    const GuestAddr mapped = map_memory(memory, 0, 4096);
    ASSERT_NE(mapped, 0u) << "vkMapMemory must hand back a guest address";

    // The point of binding the MTLBuffer's own storage into the guest address
    // space: a guest write is immediately visible to the GPU with no staging
    // copy. If this were a staged buffer the write would not be observable
    // until an explicit flush, which HOST_COHERENT promises is unnecessary.
    ASSERT_TRUE(memory_.write_u32(mapped, 0xCAFEBABE).is_ok());
    EXPECT_EQ(read_u32(mapped), 0xCAFEBABEu);
    ASSERT_TRUE(memory_.write_u32(mapped + 4092, 0x12345678).is_ok());
    EXPECT_EQ(read_u32(mapped + 4092), 0x12345678u) << "end of range must be mapped";
}

TEST_F(VulkanManagerTest, DoubleMapIsRejected) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(4096);
    ASSERT_NE(map_memory(memory, 0, 4096), 0u);

    // Vulkan forbids mapping an already-mapped allocation.
    EXPECT_EQ(map_memory(memory, 0, 4096), 0u);
}

TEST_F(VulkanManagerTest, UnmapAllowsRemapping) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(4096);
    ASSERT_NE(map_memory(memory, 0, 4096), 0u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(memory & 0xFFFFFFFFu),
              static_cast<u32>(memory >> 32)});
    auto unmap = make_marshaler();
    mgr_.impl_vkUnmapMemory(unmap);

    EXPECT_NE(map_memory(memory, 0, 4096), 0u) << "remapping after unmap must work";
}

TEST_F(VulkanManagerTest, MapRejectsRangesPastTheAllocation) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(4096);
    // Offset plus size must stay inside the allocation, or the guest would get
    // a mapping that runs off the end of the MTLBuffer.
    EXPECT_EQ(map_memory(memory, 2048, 4096), 0u);
    EXPECT_EQ(map_memory(memory, 8192, 16), 0u);
}

TEST_F(VulkanManagerTest, BufferMemoryRequirementsReportSizeAndTypeMask) {
    ASSERT_TRUE(create_device());
    const u64 buffer = create_buffer(1024);
    ASSERT_NE(buffer, 0u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(buffer & 0xFFFFFFFFu),
              static_cast<u32>(buffer >> 32), STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetBufferMemoryRequirements(m);

    auto size = memory_.read_u64(STRUCT_ADDR);
    ASSERT_TRUE(size.is_ok());
    EXPECT_EQ(size.value(), 1024u);
    // Only memory type 0 exists, so bit 0 is the whole mask.
    EXPECT_EQ(read_u32(STRUCT_ADDR + 16), 0x1u);
}

TEST_F(VulkanManagerTest, BindBufferMemorySucceedsOnceAndRejectsRebinding) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(4096);
    const u64 buffer = create_buffer(1024);
    ASSERT_NE(memory, 0u);
    ASSERT_NE(buffer, 0u);

    EXPECT_TRUE(bind_buffer(buffer, memory, 0));
    // Rebinding would leave descriptors pointing at the previous allocation.
    EXPECT_FALSE(bind_buffer(buffer, memory, 0));
}

TEST_F(VulkanManagerTest, BindBufferMemoryRejectsOffsetsThatOverrunTheAllocation) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(1024);
    const u64 buffer = create_buffer(1024);
    ASSERT_NE(memory, 0u);
    ASSERT_NE(buffer, 0u);

    // offset 512 + size 1024 exceeds the 1024-byte allocation.
    EXPECT_FALSE(bind_buffer(buffer, memory, 512));
}

TEST_F(VulkanManagerTest, DestroyBufferReleasesIt) {
    ASSERT_TRUE(create_device());
    const u64 buffer = create_buffer(256);
    ASSERT_EQ(mgr_.buffer_count(), 1u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(buffer & 0xFFFFFFFFu),
              static_cast<u32>(buffer >> 32), 0});
    auto m = make_marshaler();
    mgr_.impl_vkDestroyBuffer(m);
    EXPECT_EQ(mgr_.buffer_count(), 0u);
}

// -----------------------------------------------------------------------------
// Images and image views
// -----------------------------------------------------------------------------

TEST_F(VulkanManagerTest, CreateImageAcceptsASupportedFormat) {
    ASSERT_TRUE(create_device());
    // VK_FORMAT_B8G8R8A8_UNORM
    EXPECT_NE(create_image(64, 64, 44, /*linear=*/true), 0u);
    EXPECT_EQ(mgr_.image_count(), 1u);
}

TEST_F(VulkanManagerTest, CreateImageRefusesAnUnsupportedFormat) {
    ASSERT_TRUE(create_device());
    // Substituting a near-miss format would give the caller wrong colours
    // everywhere with nothing pointing at the cause.
    EXPECT_EQ(create_image(64, 64, /*vk_format=*/9999, true), 0u);
}

TEST_F(VulkanManagerTest, ImageMemoryRequirementsAlignRowPitch) {
    ASSERT_TRUE(create_device());
    const u64 image = create_image(100, 10, 44, true);
    ASSERT_NE(image, 0u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(image & 0xFFFFFFFFu),
              static_cast<u32>(image >> 32), STRUCT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkGetImageMemoryRequirements(m);

    auto size = memory_.read_u64(STRUCT_ADDR);
    ASSERT_TRUE(size.is_ok());
    // 100 px * 4 bytes = 400, aligned up to 512; times 10 rows.
    EXPECT_EQ(size.value(), 512u * 10u)
        << "row pitch must be 256-aligned for buffer-backed textures";
}

TEST_F(VulkanManagerTest, LinearImageAliasesTheMemoryTheGuestMapped) {
    ASSERT_TRUE(create_device());
    const u32 width = 64, height = 4;
    const u64 memory = allocate_memory(256 * height);
    const u64 image = create_image(width, height, 44, /*linear=*/true);
    ASSERT_NE(memory, 0u);
    ASSERT_NE(image, 0u);
    ASSERT_TRUE(bind_image(image, memory, 0));

    // This is the whole point of a buffer-backed texture: the guest maps the
    // allocation, writes pixels, and those are the bytes the GPU samples. If
    // the texture had its own storage the write would go nowhere.
    const GuestAddr mapped = map_memory(memory, 0, 256 * height);
    ASSERT_NE(mapped, 0u);
    ASSERT_TRUE(memory_.write_u32(mapped, 0xFF0000FF).is_ok());
    EXPECT_EQ(read_u32(mapped), 0xFF0000FFu);
}

TEST_F(VulkanManagerTest, BindImageMemoryRejectsUndersizedAllocations) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(256);  // one row
    const u64 image = create_image(64, 8, 44, true);  // needs eight
    ASSERT_NE(memory, 0u);
    ASSERT_NE(image, 0u);
    EXPECT_FALSE(bind_image(image, memory, 0));
}

TEST_F(VulkanManagerTest, BindImageMemoryRejectsRebinding) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(4096);
    const u64 image = create_image(64, 4, 44, true);
    ASSERT_TRUE(bind_image(image, memory, 0));
    EXPECT_FALSE(bind_image(image, memory, 0));
}

TEST_F(VulkanManagerTest, OptimalTilingImageBindsWithPrivateStorage) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(65536);
    // Optimal tiling is opaque by definition, so it need not alias guest
    // memory - but binding must still succeed.
    const u64 image = create_image(64, 64, 44, /*linear=*/false);
    ASSERT_NE(image, 0u);
    EXPECT_TRUE(bind_image(image, memory, 0));
}

TEST_F(VulkanManagerTest, ImageViewRequiresABoundImage) {
    ASSERT_TRUE(create_device());
    const u64 image = create_image(64, 64, 44, true);
    ASSERT_NE(image, 0u);

    // A view onto an image with no storage would reference nothing.
    EXPECT_EQ(create_image_view(image), 0u);

    const u64 memory = allocate_memory(65536);
    ASSERT_TRUE(bind_image(image, memory, 0));
    EXPECT_NE(create_image_view(image), 0u);
}

TEST_F(VulkanManagerTest, DestroyImageDropsItsViews) {
    ASSERT_TRUE(create_device());
    const u64 memory = allocate_memory(65536);
    const u64 image = create_image(64, 64, 44, true);
    ASSERT_TRUE(bind_image(image, memory, 0));
    ASSERT_NE(create_image_view(image), 0u);
    ASSERT_EQ(mgr_.image_view_count(), 1u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(image & 0xFFFFFFFFu),
              static_cast<u32>(image >> 32), 0});
    auto m = make_marshaler();
    mgr_.impl_vkDestroyImage(m);
    EXPECT_EQ(mgr_.image_count(), 0u);
    EXPECT_EQ(mgr_.image_view_count(), 0u)
        << "views must not outlive the image they reference";
}

TEST_F(VulkanManagerTest, CommandPoolRejectsUnknownDevice) {
    set_args({0xBADBAD, 0, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreateCommandPool(m);
    EXPECT_NE(return_value(), VK_SUCCESS);
}

TEST_F(VulkanManagerTest, CreateRenderPassWritesHandle) {
    ASSERT_TRUE(create_device());
    std::vector<u8> info(32, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
    set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreateRenderPass(m);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    auto handle = memory_.read_u64(OUT_ADDR);
    ASSERT_TRUE(handle.is_ok());
    EXPECT_NE(handle.value(), 0u);
    EXPECT_EQ(mgr_.render_pass_count(), 1u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(handle.value() & 0xFFFFFFFFu),
              static_cast<u32>(handle.value() >> 32), 0});
    auto destroy = make_marshaler();
    mgr_.impl_vkDestroyRenderPass(destroy);
    EXPECT_EQ(mgr_.render_pass_count(), 0u);
}

TEST_F(VulkanManagerTest, CreateRenderPassRejectsUnknownDevice) {
    std::vector<u8> info(32, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
    set_args({0xBADBAD, STRUCT_ADDR, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreateRenderPass(m);
    EXPECT_NE(return_value(), VK_SUCCESS);
    EXPECT_EQ(mgr_.render_pass_count(), 0u);
}

TEST_F(VulkanManagerTest, CreateShaderModuleWritesHandle) {
    ASSERT_TRUE(create_device());
    std::vector<u8> info(32, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
    set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreateShaderModule(m);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    auto handle = memory_.read_u64(OUT_ADDR);
    ASSERT_TRUE(handle.is_ok());
    EXPECT_NE(handle.value(), 0u);
    EXPECT_EQ(mgr_.shader_module_count(), 1u);

    set_args({static_cast<u32>(device_handle_),
              static_cast<u32>(handle.value() & 0xFFFFFFFFu),
              static_cast<u32>(handle.value() >> 32), 0});
    auto destroy = make_marshaler();
    mgr_.impl_vkDestroyShaderModule(destroy);
    EXPECT_EQ(mgr_.shader_module_count(), 0u);
}

TEST_F(VulkanManagerTest, CreatePipelineLayoutAndGraphicsPipelineWriteHandles) {
    ASSERT_TRUE(create_device());
    std::vector<u8> info(32, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
    set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
    auto layout_m = make_marshaler();
    mgr_.impl_vkCreatePipelineLayout(layout_m);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    EXPECT_EQ(mgr_.pipeline_layout_count(), 1u);

    // x86: device, cache.lo, cache.hi, count, infos, allocator, out
    set_args({static_cast<u32>(device_handle_), 0, 0, 1, STRUCT_ADDR, 0,
              OUT_ADDR});
    auto pipe_m = make_marshaler();
    mgr_.impl_vkCreateGraphicsPipelines(pipe_m);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    auto handle = memory_.read_u64(OUT_ADDR);
    ASSERT_TRUE(handle.is_ok());
    EXPECT_NE(handle.value(), 0u);
    EXPECT_EQ(mgr_.pipeline_count(), 1u);
}

TEST_F(VulkanManagerTest, CreatePipelineCacheWritesHandle) {
    ASSERT_TRUE(create_device());
    std::vector<u8> info(32, 0);
    (void)memory_.write_bytes(STRUCT_ADDR, info.data(), info.size());
    set_args({static_cast<u32>(device_handle_), STRUCT_ADDR, 0, OUT_ADDR});
    auto m = make_marshaler();
    mgr_.impl_vkCreatePipelineCache(m);
    EXPECT_EQ(return_value(), VK_SUCCESS);
    auto handle = memory_.read_u64(OUT_ADDR);
    ASSERT_TRUE(handle.is_ok());
    EXPECT_NE(handle.value(), 0u);
    EXPECT_EQ(mgr_.pipeline_cache_count(), 1u);
}
