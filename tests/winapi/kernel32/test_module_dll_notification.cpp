/// @file test_module_dll_notification.cpp
/// @brief Tests for ModuleManager LdrRegisterDllNotification registration and dispatch

#include "module_test_fixture.hpp"

namespace {

std::u16string read_notification_string(
    GuestMemory& memory, GuestAddr descriptor, bool is_64bit) {
    const u16 byte_length = memory.read_u16(descriptor).value();
    const GuestAddr buffer = is_64bit
        ? memory.read_u64(descriptor + 8).value()
        : memory.read_u32(descriptor + 4).value();
    std::u16string value;
    for (u16 offset = 0; offset < byte_length; offset += sizeof(u16)) {
        value.push_back(static_cast<char16_t>(
            memory.read_u16(buffer + offset).value()));
    }
    return value;
}

struct CapturedDllNotification {
    u32 reason{};
    u32 flags{};
    GuestAddr image_base{};
    u32 image_size{};
    GuestAddr context{};
    std::u16string full_name;
    std::u16string base_name;
};

}  // namespace

TEST_F(ModuleManagerTest, DllNotificationRegistrationValidatesArguments) {
    constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000Du;
    GuestAddr cookie = 0;

    EXPECT_EQ(mm_->register_dll_notification(1, 0x1234, 0, &cookie),
              STATUS_INVALID_PARAMETER);
    EXPECT_EQ(mm_->register_dll_notification(0, 0, 0, &cookie),
              STATUS_INVALID_PARAMETER);
    EXPECT_EQ(mm_->register_dll_notification(0, 0x1234, 0, nullptr),
              STATUS_INVALID_PARAMETER);
    EXPECT_EQ(cookie, 0u);
}

TEST_F(ModuleManagerTest, DllNotificationCookiesAreUniqueAndUnregisterOnce) {
    constexpr u32 STATUS_SUCCESS = 0x00000000u;
    constexpr u32 STATUS_DLL_NOT_FOUND = 0xC0000135u;
    GuestAddr first_cookie = 0;
    GuestAddr second_cookie = 0;

    ASSERT_EQ(mm_->register_dll_notification(
                  0, 0x1234, 0xAABB, &first_cookie),
              STATUS_SUCCESS);
    ASSERT_EQ(mm_->register_dll_notification(
                  0, 0x5678, 0xCCDD, &second_cookie),
              STATUS_SUCCESS);
    EXPECT_NE(first_cookie, 0u);
    EXPECT_NE(second_cookie, 0u);
    EXPECT_NE(first_cookie, second_cookie);

    EXPECT_EQ(mm_->unregister_dll_notification(first_cookie), STATUS_SUCCESS);
    EXPECT_EQ(mm_->unregister_dll_notification(first_cookie),
              STATUS_DLL_NOT_FOUND);
    EXPECT_EQ(mm_->unregister_dll_notification(second_cookie), STATUS_SUCCESS);
}

TEST_P(DllNotificationDispatchTest,
       MarshalsLoadAndFinalUnloadAndHonorsCallbackUnregister) {
    const bool is_64bit = GetParam();
    char root_template[] = "/tmp/vitreo_dll_notify_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    const auto dll_bytes = build_notification_test_dll(is_64bit);
    {
        std::ofstream output(root_path / "notice.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(
        is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, is_64bit);

    constexpr GuestAddr first_callback = 0x1111;
    constexpr GuestAddr second_callback = 0x2222;
    constexpr GuestAddr context = 0xAABBCCDD;
    GuestAddr first_cookie = 0;
    GuestAddr second_cookie = 0;
    ASSERT_EQ(modules.register_dll_notification(
                  0, first_callback, context, &first_cookie),
              0u);
    ASSERT_EQ(modules.register_dll_notification(
                  0, second_callback, 0, &second_cookie),
              0u);

    std::vector<CapturedDllNotification> captured;
    bool second_callback_called = false;
    modules.set_guest_call(
        [&](GuestAddr callback, const u64* args, usize argument_count,
            bool callback_is_64bit) -> u64 {
            EXPECT_EQ(argument_count, 3u);
            EXPECT_EQ(callback_is_64bit, is_64bit);
            if (callback == second_callback) {
                second_callback_called = true;
                return 1;
            }
            EXPECT_EQ(callback, first_callback);

            const GuestAddr data = args[1];
            const GuestAddr pointer_offset = is_64bit ? 8 : 4;
            const GuestAddr full_name = is_64bit
                ? memory.read_u64(data + pointer_offset).value()
                : memory.read_u32(data + pointer_offset).value();
            const GuestAddr base_name = is_64bit
                ? memory.read_u64(data + pointer_offset * 2).value()
                : memory.read_u32(data + pointer_offset * 2).value();
            const GuestAddr image_base = is_64bit
                ? memory.read_u64(data + pointer_offset * 3).value()
                : memory.read_u32(data + pointer_offset * 3).value();
            const GuestAddr size_offset = pointer_offset * 4;
            captured.push_back(CapturedDllNotification{
                .reason = static_cast<u32>(args[0]),
                .flags = memory.read_u32(data).value(),
                .image_base = image_base,
                .image_size = memory.read_u32(data + size_offset).value(),
                .context = args[2],
                .full_name = read_notification_string(
                    memory, full_name, is_64bit),
                .base_name = read_notification_string(
                    memory, base_name, is_64bit),
            });
            if (captured.size() == 1) {
                EXPECT_EQ(modules.unregister_dll_notification(second_cookie),
                          0u);
            }
            return 1;
        });

    const GuestAddr module = modules.load_library("C:\\notice.dll");
    ASSERT_NE(module, 0u);
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_FALSE(second_callback_called);
    EXPECT_TRUE(modules.free_library(module));
    ASSERT_EQ(captured.size(), 2u);

    EXPECT_EQ(captured[0].reason, 1u);
    EXPECT_EQ(captured[1].reason, 2u);
    for (const auto& notification : captured) {
        EXPECT_EQ(notification.flags, 0u);
        EXPECT_EQ(notification.image_base, module);
        EXPECT_EQ(notification.image_size, 0x2000u);
        EXPECT_EQ(notification.context, context);
        EXPECT_EQ(notification.full_name, u"C:\\notice.dll");
        EXPECT_EQ(notification.base_name, u"notice.dll");
    }

    EXPECT_EQ(modules.unregister_dll_notification(first_cookie), 0u);
    std::filesystem::remove_all(root_path);
}


namespace {
// A self-contained PE with real function/DATA EAT entries. Addresses and names
// are fixture choices; visibility/order expectations come from the identical
// Windows ldr_visibility_probe{32,64}.expected measurements.
std::vector<u8> build_visibility_test_dll(bool x64) {
    auto bytes = build_notification_test_dll(x64);
    constexpr u32 pe_offset = 0x80, raw = 0x200, section_rva = 0x1000;
    constexpr u32 exports_rva = 0x1100;
    const usize optional_offset = pe_offset + sizeof(u32) + sizeof(pe::CoffHeader);
    auto* coff = reinterpret_cast<pe::CoffHeader*>(bytes.data() + pe_offset + sizeof(u32));
    if (x64) {
        auto* optional = reinterpret_cast<pe::OptionalHeader64*>(bytes.data() + optional_offset);
        optional->AddressOfEntryPoint = section_rva;
        optional->DataDirectory[pe::DIR_EXPORT] = {exports_rva, 0x100};
    } else {
        auto* optional = reinterpret_cast<pe::OptionalHeader32*>(bytes.data() + optional_offset);
        optional->AddressOfEntryPoint = section_rva;
        optional->DataDirectory[pe::DIR_EXPORT] = {exports_rva, 0x100};
    }
    auto* section = reinterpret_cast<pe::SectionHeader*>(bytes.data() + optional_offset + coff->SizeOfOptionalHeader);
    section->VirtualSize = section->SizeOfRawData;
    section->Characteristics |= pe::SCN_MEM_WRITE;
    auto put = [&](u32 rva, const void* value, usize size) {
        std::memcpy(bytes.data() + raw + rva - section_rva, value, size);
    };
    pe::ExportDirectory directory{};
    directory.Name = 0x1180;
    directory.Base = 1;
    directory.NumberOfFunctions = 2;
    directory.NumberOfNames = 2;
    directory.AddressOfFunctions = 0x1130;
    directory.AddressOfNames = 0x1138;
    directory.AddressOfNameOrdinals = 0x1140;
    put(exports_rva, &directory, sizeof directory);
    const u32 functions[] = {section_rva, 0x1014};
    const u32 names[] = {0x1144, 0x1154};
    const u16 ordinals[] = {1, 0}; // Names are sorted: Constant, Function.
    put(directory.AddressOfFunctions, functions, sizeof functions);
    put(directory.AddressOfNames, names, sizeof names);
    put(directory.AddressOfNameOrdinals, ordinals, sizeof ordinals);
    constexpr char constant_name[] = "Constant", function_name[] = "Function", dll_name[] = "visibility.dll";
    put(names[0], constant_name, sizeof constant_name);
    put(names[1], function_name, sizeof function_name);
    put(directory.Name, dll_name, sizeof dll_name);
    constexpr u32 value = 0x13579bdf;
    put(functions[1], &value, sizeof value);
    return bytes;
}
} // namespace

TEST_P(DllNotificationDispatchTest,
       ExportsAndIdentityAreVisibleBeforeAttachAndThroughSuccessfulOrFailedUnload) {
    const bool x64 = GetParam();
    for (bool explicit_path : {false, true}) for (bool reject_attach : {false, true}) {
        SCOPED_TRACE(std::string(explicit_path ? "explicit" : "search") + (reject_attach ? " rejected" : " accepted"));
        char temporary[] = "/tmp/vitreo_ldr_visibility_XXXXXX";
        const char* root = ::mkdtemp(temporary);
        ASSERT_NE(root, nullptr);
        const std::filesystem::path root_path(root);
        write_test_file(root_path / "visibility.dll", build_visibility_test_dll(x64));
        GuestMemory memory;
        capsule::Vfs vfs;
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
        ThunkRegistry registry(x64 ? 0x00007ffb00000000ull : 0x70000000ull);
        ModuleManager modules(registry);
        modules.attach_dynamic_loader(memory, vfs, x64);
        modules.set_current_directory("C:\\");
        constexpr GuestAddr callback = 0x1111;
        GuestAddr cookie = 0, image = 0;
        std::vector<u32> reasons;
        ASSERT_EQ(modules.register_dll_notification(0, callback, 0, &cookie), 0u);
        modules.set_guest_call([&](GuestAddr function, const u64* args, usize count, bool bits) -> u64 {
            EXPECT_EQ(count, 3u);
            EXPECT_EQ(bits, x64);
            if (function != callback) {
                // Existing test-dispatch seam drives the real PE's DllMain
                // boundary. Oracle fixture measures actual Windows DllMain.
                EXPECT_EQ(function, args[0] + 0x1000);
                EXPECT_TRUE(memory.write_u32(args[0] + 0x1010, args[1] == 1 ? 1 : 2).is_ok());
                return args[1] == 1 && reject_attach ? 0 : 1;
            }
            const GuestAddr pointer_width = x64 ? sizeof(u64) : sizeof(u32);
            image = x64 ? memory.read_u64(args[1] + 3 * pointer_width).value()
                        : memory.read_u32(args[1] + 3 * pointer_width).value();
            reasons.push_back(static_cast<u32>(args[0]));
            EXPECT_EQ(modules.get_module_handle("visibility.dll"), image);
            EXPECT_EQ(modules.get_proc_address(image, "Function"), image + 0x1000);
            EXPECT_EQ(modules.get_proc_address_by_ordinal(image, 1), image + 0x1000);
            const GuestAddr constant = modules.get_proc_address(image, "Constant");
            EXPECT_EQ(constant, image + 0x1014);
            EXPECT_EQ(modules.get_proc_address_by_ordinal(image, 2), image + 0x1014);
            if (constant) EXPECT_EQ(memory.read_u32(constant).value(), 0x13579bdfu);
            EXPECT_EQ(memory.read_u32(image + 0x1010).value(), args[0] == 1 ? 0u : 2u);
            char filename[260]{};
            EXPECT_NE(modules.get_module_filename(image, filename, sizeof filename), 0u);
            EXPECT_EQ(std::string(filename), "C:\\visibility.dll");
            return 1;
        });
        const GuestAddr loaded = modules.load_library(explicit_path ? "C:\\visibility.dll" : "visibility.dll");
        if (reject_attach) {
            EXPECT_EQ(loaded, 0u);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_DLL_INIT_FAILED);
        } else {
            ASSERT_NE(loaded, 0u);
            EXPECT_EQ(memory.read_u32(loaded + 0x1010).value(), 1u);
            EXPECT_TRUE(modules.free_library(loaded));
        }
        EXPECT_EQ(reasons, (std::vector<u32>{1, 2}));
        EXPECT_EQ(modules.get_module_handle("visibility.dll"), 0u);
        EXPECT_EQ(modules.unregister_dll_notification(cookie), 0u);
        std::filesystem::remove_all(root_path);
    }
}

INSTANTIATE_TEST_SUITE_P(
    ProcessBitness, DllNotificationDispatchTest, ::testing::Values(false, true));
