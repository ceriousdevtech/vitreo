/// @file test_handle.cpp
/// @brief Unit tests for HandleTable and CloseHandle

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synthetic_handle_registry.hpp>
#include <winapi/kernel32/shared_resource_handle.hpp>
#include <winapi/kernel32/error.hpp>
#include "runtime/execution/inherited_handles.hpp"
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/memory/memory_region.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <cctype>
#include <memory>
#include <string>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// HandleTable Basic Tests
// =============================================================================

class HandleTableTest : public ::testing::Test {
protected:
    HandleTable table;
};

TEST_F(HandleTableTest, InitiallyEmpty) {
    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.count(), 0u);
}

TEST_F(HandleTableTest, AllocateReturnsValidHandle) {
    Handle h = table.allocate(HandleType::File, nullptr);
    
    EXPECT_NE(h, NULL_HANDLE);
    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_TRUE(table.is_valid(h));
    EXPECT_EQ(table.count(), 1u);
}

TEST_F(HandleTableTest, AllocateMultipleHandles) {
    Handle h1 = table.allocate(HandleType::File, nullptr);
    Handle h2 = table.allocate(HandleType::Process, nullptr);
    Handle h3 = table.allocate(HandleType::Thread, nullptr);
    
    // All handles should be unique
    EXPECT_NE(h1, h2);
    EXPECT_NE(h2, h3);
    EXPECT_NE(h1, h3);
    
    // All handles should be valid
    EXPECT_TRUE(table.is_valid(h1));
    EXPECT_TRUE(table.is_valid(h2));
    EXPECT_TRUE(table.is_valid(h3));
    
    EXPECT_EQ(table.count(), 3u);
}

TEST_F(HandleTableTest, AllocateAtPreservesInheritedHandleValue) {
    int object = 42;
    ASSERT_TRUE(table.allocate_at(0x80, HandleType::Event, &object,
                                  HandleFlags::Inherit));

    EXPECT_TRUE(table.is_valid(0x80));
    EXPECT_EQ(table.get(0x80, HandleType::Event), &object);
    EXPECT_EQ(table.get_flags(0x80), HandleFlags::Inherit);
    EXPECT_GT(table.allocate(HandleType::File, nullptr), 0x80u);
    EXPECT_FALSE(table.allocate_at(0x80, HandleType::Event, &object));
}

TEST_F(HandleTableTest, HandleValuesAreMultiplesOfFour) {
    Handle h1 = table.allocate(HandleType::File, nullptr);
    Handle h2 = table.allocate(HandleType::File, nullptr);
    Handle h3 = table.allocate(HandleType::File, nullptr);
    
    // Windows convention: handles are multiples of 4
    EXPECT_EQ(h1 % 4, 0u);
    EXPECT_EQ(h2 % 4, 0u);
    EXPECT_EQ(h3 % 4, 0u);
}

TEST_F(HandleTableTest, InheritedValuesSurviveStartupAndRetainAccess) {
    // CreateProcessW guarantees the inherited value and access rights.
    // Child startup must not claim these numbers before importing them.
    const std::array<Handle, 3> inherited{4, 8, 92};
    HandleTable child(inherited);
    int local_object = 1, inherited_object = 2;
    std::vector<Handle> local;
    for (int i = 0; i < 32; ++i) {
        const Handle h = child.allocate(HandleType::Event, &local_object);
        ASSERT_NE(h, NULL_HANDLE);
        EXPECT_EQ(std::count(inherited.begin(), inherited.end(), h), 0);
        local.push_back(h);
    }
    for (Handle h : inherited) {
        EXPECT_FALSE(child.is_valid(h));
        EXPECT_FALSE(child.close(h));
        ASSERT_TRUE(child.allocate_at(h, HandleType::Pipe, &inherited_object,
                                     HandleFlags::Inherit, 0x80000000u));
        EXPECT_EQ(child.get(h), &inherited_object);
        EXPECT_EQ(child.get_flags(h), HandleFlags::Inherit);
        EXPECT_EQ(child.get_access(h), 0x80000000u);
        EXPECT_FALSE(child.allocate_at(h, HandleType::Event, &local_object));
    }
    for (Handle h : local) EXPECT_EQ(child.get(h), &local_object);
}

TEST_F(HandleTableTest, InheritedHighValueDoesNotOverwriteAfterWrap) {
    int first = 1, last = 2;
    ASSERT_TRUE(table.allocate_at(4, HandleType::Event, &first));
    ASSERT_TRUE(table.allocate_at(0xfffffffcu, HandleType::Pipe, &last));
    const Handle next = table.allocate(HandleType::Event, &first);
    EXPECT_EQ(next, 8u);
    EXPECT_EQ(table.get(4), &first);
    EXPECT_EQ(table.get(0xfffffffcu), &last);
}

TEST(InheritedHandleValues, CoversEveryTransportBeforeStartup) {
    const auto values = runtime::inherited_handle_values(
        "F,4,10,1,1;N,8,11,name;A,12,12,1,0",
        "I,16,encoded;E,20,name;P,24,123", "M,28,4,1,6,name,visible");
    ASSERT_TRUE(values);
    EXPECT_EQ(*values, (std::vector<Handle>{4, 8, 12, 16, 20, 24, 28}));
    EXPECT_TRUE(runtime::inherited_handle_values({}, {}, {}));
    for (const auto invalid : {"A,0,1,1,0", "A,4294967295,1,1,0",
             "A,4294967296,1,1,0", "A,-4,1,1,0", "A,4x,1,1,0",
             "A,,1,1,0", "A,4", "A,4,1,1,0;", ";A,4,1,1,0",
             "X,4,1,1,0", "A,4,1,1,0;A,4,2,0,1"}) {
        EXPECT_FALSE(runtime::inherited_handle_values(invalid, {}, {})) << invalid;
    }
    EXPECT_FALSE(runtime::inherited_handle_values("A,4,1,1,0", "I,4,name", {}));
    EXPECT_FALSE(runtime::inherited_handle_values({}, "M,4,name", {}));
    EXPECT_FALSE(runtime::inherited_handle_values({}, {}, "I,4,name"));
}

TEST_F(HandleTableTest, CloseValidHandle) {
    Handle h = table.allocate(HandleType::File, nullptr);
    EXPECT_TRUE(table.is_valid(h));
    
    bool result = table.close(h);
    
    EXPECT_TRUE(result);
    EXPECT_FALSE(table.is_valid(h));
    EXPECT_EQ(table.count(), 0u);
}

TEST_F(HandleTableTest, CloseInvalidHandleFails) {
    bool result = table.close(12345);
    
    EXPECT_FALSE(result);
}

TEST_F(HandleTableTest, CloseNullHandleFails) {
    bool result = table.close(NULL_HANDLE);
    
    EXPECT_FALSE(result);
}

TEST_F(HandleTableTest, DoubleCloseFails) {
    Handle h = table.allocate(HandleType::File, nullptr);
    
    EXPECT_TRUE(table.close(h));
    EXPECT_FALSE(table.close(h)); // Second close should fail
}

// =============================================================================
// HandleTable Object Association Tests
// =============================================================================

TEST_F(HandleTableTest, GetReturnsAssociatedObject) {
    int dummy_object = 42;
    Handle h = table.allocate(HandleType::File, &dummy_object);
    
    void* obj = table.get(h);
    
    EXPECT_EQ(obj, &dummy_object);
}

TEST_F(HandleTableTest, GetWithTypeCheck) {
    int dummy_object = 42;
    Handle h = table.allocate(HandleType::File, &dummy_object);
    
    // Correct type
    void* obj1 = table.get(h, HandleType::File);
    EXPECT_EQ(obj1, &dummy_object);
    
    // Wrong type
    void* obj2 = table.get(h, HandleType::Process);
    EXPECT_EQ(obj2, nullptr);
    
    // Any type (Invalid means no type check)
    void* obj3 = table.get(h, HandleType::Invalid);
    EXPECT_EQ(obj3, &dummy_object);
}

TEST_F(HandleTableTest, GetAsTyped) {
    int dummy_object = 42;
    Handle h = table.allocate(HandleType::File, &dummy_object);
    
    int* ptr = table.get_as<int>(h, HandleType::File);
    EXPECT_EQ(ptr, &dummy_object);
    EXPECT_EQ(*ptr, 42);
}

TEST_F(HandleTableTest, GetInvalidHandleReturnsNull) {
    void* obj = table.get(12345);
    EXPECT_EQ(obj, nullptr);
}

// =============================================================================
// HandleTable Type Tests
// =============================================================================

TEST_F(HandleTableTest, GetTypeReturnsCorrectType) {
    Handle h1 = table.allocate(HandleType::File, nullptr);
    Handle h2 = table.allocate(HandleType::Process, nullptr);
    Handle h3 = table.allocate(HandleType::Event, nullptr);
    
    EXPECT_EQ(table.get_type(h1), HandleType::File);
    EXPECT_EQ(table.get_type(h2), HandleType::Process);
    EXPECT_EQ(table.get_type(h3), HandleType::Event);
}

TEST_F(HandleTableTest, GetTypeInvalidHandleReturnsInvalid) {
    HandleType type = table.get_type(12345);
    EXPECT_EQ(type, HandleType::Invalid);
}

// =============================================================================
// HandleTable Flags Tests
// =============================================================================

TEST_F(HandleTableTest, DefaultFlagsAreZero) {
    Handle h = table.allocate(HandleType::File, nullptr);
    
    u32 flags = table.get_flags(h);
    EXPECT_EQ(flags, 0u);
}

TEST_F(HandleTableTest, AllocateWithFlags) {
    Handle h = table.allocate(HandleType::File, nullptr, HandleFlags::Inherit);
    
    u32 flags = table.get_flags(h);
    EXPECT_EQ(flags, HandleFlags::Inherit);
}

TEST_F(HandleTableTest, SetFlags) {
    Handle h = table.allocate(HandleType::File, nullptr);
    
    bool result = table.set_flags(h, HandleFlags::Inherit, HandleFlags::Inherit);
    
    EXPECT_TRUE(result);
    EXPECT_EQ(table.get_flags(h), HandleFlags::Inherit);
}

TEST_F(HandleTableTest, SetFlagsWithMask) {
    Handle h = table.allocate(HandleType::File, nullptr, HandleFlags::Inherit);
    
    // Add ProtectFromClose, keep Inherit
    table.set_flags(h, HandleFlags::ProtectFromClose, HandleFlags::ProtectFromClose);
    
    u32 flags = table.get_flags(h);
    EXPECT_EQ(flags, HandleFlags::Inherit | HandleFlags::ProtectFromClose);
}

TEST_F(HandleTableTest, ClearFlags) {
    Handle h = table.allocate(HandleType::File, nullptr, 
                              HandleFlags::Inherit | HandleFlags::ProtectFromClose);
    
    // Clear Inherit flag
    table.set_flags(h, HandleFlags::Inherit, 0);
    
    EXPECT_EQ(table.get_flags(h), HandleFlags::ProtectFromClose);
}

TEST_F(HandleTableTest, ProtectFromCloseBlocksClose) {
    Handle h = table.allocate(HandleType::File, nullptr, HandleFlags::ProtectFromClose);
    
    bool result = table.close(h);
    
    EXPECT_FALSE(result);
    EXPECT_TRUE(table.is_valid(h)); // Handle still exists
}

TEST_F(HandleTableTest, RemoveProtectThenClose) {
    Handle h = table.allocate(HandleType::File, nullptr, HandleFlags::ProtectFromClose);
    
    // Remove protection
    table.set_flags(h, HandleFlags::ProtectFromClose, 0);
    
    bool result = table.close(h);
    
    EXPECT_TRUE(result);
    EXPECT_FALSE(table.is_valid(h));
}

// =============================================================================
// HandleTable Callback Tests
// =============================================================================

TEST(HandleTableCallbackTest, CloseCallbackInvoked) {
    bool callback_called = false;
    Handle closed_handle = 0;
    HandleType closed_type = HandleType::Invalid;
    void* closed_object = nullptr;
    
    HandleTable table([&](Handle h, HandleType type, void* obj) -> bool {
        callback_called = true;
        closed_handle = h;
        closed_type = type;
        closed_object = obj;
        return true;
    });
    
    int dummy = 123;
    Handle h = table.allocate(HandleType::File, &dummy);
    
    table.close(h);
    
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(closed_handle, h);
    EXPECT_EQ(closed_type, HandleType::File);
    EXPECT_EQ(closed_object, &dummy);
}

TEST(HandleTableCallbackTest, SetCloseCallbackLater) {
    HandleTable table;
    
    bool callback_called = false;
    table.set_close_callback([](Handle, HandleType, void*) -> bool {
        return true;
    });
    
    // Use a different lambda to detect callback
    int call_count = 0;
    table.set_close_callback([&](Handle, HandleType, void*) -> bool {
        call_count++;
        return true;
    });
    
    Handle h = table.allocate(HandleType::Process, nullptr);
    table.close(h);
    
    EXPECT_EQ(call_count, 1);
}

TEST(HandleTableCallbackTest, DestructorClosesAllHandles) {
    int close_count = 0;
    
    {
        HandleTable table([&](Handle, HandleType, void*) -> bool {
            close_count++;
            return true;
        });
        
        table.allocate(HandleType::File, nullptr);
        table.allocate(HandleType::Process, nullptr);
        table.allocate(HandleType::Thread, nullptr);
        
        // table goes out of scope here
    }
    
    EXPECT_EQ(close_count, 3);
}

// =============================================================================
// HandleTable Entry Access Tests
// =============================================================================

TEST_F(HandleTableTest, GetEntryReturnsEntry) {
    int dummy = 99;
    Handle h = table.allocate(HandleType::Event, &dummy, HandleFlags::Inherit);
    
    const HandleEntry* entry = table.get_entry(h);
    
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->type, HandleType::Event);
    EXPECT_EQ(entry->object, &dummy);
    EXPECT_EQ(entry->flags, HandleFlags::Inherit);
    EXPECT_TRUE(entry->is_valid());
}

TEST_F(HandleTableTest, GetEntryInvalidHandleReturnsNull) {
    const HandleEntry* entry = table.get_entry(12345);
    EXPECT_EQ(entry, nullptr);
}

// =============================================================================
// HandleType String Conversion Tests
// =============================================================================

TEST(HandleTypeTest, HandleTypeToString) {
    EXPECT_STREQ(handle_type_to_string(HandleType::Invalid), "Invalid");
    EXPECT_STREQ(handle_type_to_string(HandleType::File), "File");
    EXPECT_STREQ(handle_type_to_string(HandleType::Process), "Process");
    EXPECT_STREQ(handle_type_to_string(HandleType::Thread), "Thread");
    EXPECT_STREQ(handle_type_to_string(HandleType::Event), "Event");
    EXPECT_STREQ(handle_type_to_string(HandleType::Mutex), "Mutex");
}

// =============================================================================
// Handle Constants Tests
// =============================================================================

TEST(HandleConstantsTest, InvalidHandleValue) {
    // INVALID_HANDLE should be -1 cast to u32 (0xFFFFFFFF)
    EXPECT_EQ(INVALID_HANDLE, 0xFFFFFFFFu);
}

TEST(HandleConstantsTest, NullHandleValue) {
    EXPECT_EQ(NULL_HANDLE, 0u);
}

// =============================================================================
// impl_CloseHandle Tests (integration with Marshaler)
// =============================================================================

class CloseHandleTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create guest memory with a mapped region for testing
        guest_memory_ = std::make_unique<GuestMemory>();
        auto map_result = guest_memory_->map_region(
            0x1000,     // base address
            0x10000,    // size: 64KB
            MemoryProtection::ReadWrite
        );
        ASSERT_TRUE(map_result.is_ok());
        
        marshaler_ = std::make_unique<Marshaler>(*guest_memory_);
        marshaler_->attach_cpu(cpu_);
    }
    
    std::unique_ptr<GuestMemory> guest_memory_;
    std::unique_ptr<Marshaler> marshaler_;
    cpu::CpuState cpu_;
    HandleTable table_;
};

TEST_F(CloseHandleTest, CloseValidHandleReturnsTrue) {
    // Allocate a handle
    Handle h = table_.allocate(HandleType::File, nullptr);
    
    // Set handle as arg(0) via RCX (64-bit fastcall)
    cpu_.gpr.rcx = h;
    
    // Call impl_CloseHandle
    impl_CloseHandle(*marshaler_, table_);
    
    // Return value in RAX
    EXPECT_EQ(cpu_.gpr.rax, 1u); // TRUE
    
    // Handle should be closed
    EXPECT_FALSE(table_.is_valid(h));
}

TEST_F(CloseHandleTest, CloseInvalidHandleReturnsFalse) {
    // Set invalid handle as arg(0)
    cpu_.gpr.rcx = 12345;
    
    // Call impl_CloseHandle
    impl_CloseHandle(*marshaler_, table_);
    
    // Return value
    EXPECT_EQ(cpu_.gpr.rax, 0u); // FALSE
}

TEST_F(CloseHandleTest, CloseNullHandleReturnsFalse) {
    // Set NULL handle
    cpu_.gpr.rcx = NULL_HANDLE;
    
    // Call impl_CloseHandle
    impl_CloseHandle(*marshaler_, table_);
    
    // Return value
    EXPECT_EQ(cpu_.gpr.rax, 0u); // FALSE
}

TEST_F(CloseHandleTest, CloseINVALID_HANDLE_VALUESucceedsAsPseudoHandleNoop) {
    // INVALID_HANDLE_VALUE (-1) is the GetCurrentProcess() pseudo handle;
    // Windows CloseHandle on a pseudo handle is a successful no-op.
    cpu_.gpr.rcx = INVALID_HANDLE;

    impl_CloseHandle(*marshaler_, table_);

    EXPECT_EQ(cpu_.gpr.rax, 1u); // TRUE
}

TEST_F(CloseHandleTest, NeverIssuedFaceRangeFailsInvalidHandle) {
    // Windows synthetic_handle_probe: CloseHandle(0xFACE0001) → FALSE + 6.
    set_last_error(0xDEAD);
    cpu_.gpr.rcx = 0xFACE0001u;
    impl_CloseHandle(*marshaler_, table_);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(CloseHandleTest, IssuedSyntheticFaceClosesOnce) {
    synthetic_handles::note_issued(0xFACE0042u);
    set_last_error(0xDEAD);
    cpu_.gpr.rcx = 0xFACE0042u;
    impl_CloseHandle(*marshaler_, table_);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu); // success leaves last-error

    set_last_error(0xDEAD);
    cpu_.gpr.rcx = 0xFACE0042u;
    impl_CloseHandle(*marshaler_, table_);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// DXGI shared-resource NT handles (winapi/kernel32/shared_resource_handle.hpp)
// =============================================================================

// Learn IDXGIResource1::CreateSharedHandle: the handle is an NT handle usable
// with CloseHandle and DuplicateHandle; measured (d3d11_share_probe
// [xproc] "dup"): a handle DuplicateHandle puts in another process opens
// there, and its name travels with it.
TEST(SharedResourceHandleTest, TransferCarriesTheObjectAndItsName) {
    HandleTable source;
    HandleTable target;
    const auto release = [](Handle, HandleType type, void* object) {
        if (type == HandleType::SharedResource) {
            release_shared_resource_handle_object(object);
        }
        return true;
    };
    source.set_close_callback(release);
    target.set_close_callback(release);
    set_shared_resource_retainer(
        [](u32 surface) { return std::make_shared<u32>(surface); });

    const std::string name =
        "Local\\vitreo_handle_test_" + std::to_string(::getpid());
    auto object = std::make_shared<SharedResourceObject>();
    object->surface_id = 0x1234;
    object->nonce = 0xABCDEF0123456789ull;
    std::string key;
    ASSERT_EQ(register_shared_resource_name(name, 0x1234, object->nonce,
                                            object->name, key),
              SharedResourceNameResult::Registered);
    object->name_key = key;
    const Handle handle =
        allocate_shared_resource_handle(source, object, 0x80000001u, true);
    object.reset();
    ASSERT_NE(handle, NULL_HANDLE);
    EXPECT_EQ(source.get_type(handle), HandleType::SharedResource);
    EXPECT_EQ(source.get_flags(handle) & 1u, 1u);

    const auto descriptor =
        prepare_shared_resource_transfer(source, handle, 0x80000000u, false);
    ASSERT_TRUE(descriptor.has_value());
    const Handle imported = import_shared_resource_transfer(target, *descriptor);
    ASSERT_NE(imported, NULL_HANDLE);
    u32 access = 0;
    auto seen = shared_resource_object(target, imported, &access);
    ASSERT_NE(seen, nullptr);
    EXPECT_EQ(seen->surface_id, 0x1234u);
    EXPECT_EQ(seen->nonce, 0xABCDEF0123456789ull);
    EXPECT_NE(seen->keepalive, nullptr);
    EXPECT_EQ(access, 0x80000000u);
    EXPECT_EQ(target.get_flags(imported) & 1u, 0u);

    // The name lives while any handle to the object does, in any process.
    ASSERT_TRUE(source.close(handle));
    auto found = lookup_shared_resource_name(name);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->surface_id, 0x1234u);
    // Case-insensitive, "Local\" optional (measured).
    std::string upper = name;
    for (char& c : upper) c = static_cast<char>(std::toupper(c));
    EXPECT_TRUE(lookup_shared_resource_name(upper).has_value());
    EXPECT_TRUE(lookup_shared_resource_name(name.substr(6)).has_value());
    seen.reset();
    ASSERT_TRUE(target.close(imported));
    EXPECT_FALSE(lookup_shared_resource_name(name).has_value());

    // Same-process DuplicateHandle names the same object.
    auto plain = std::make_shared<SharedResourceObject>();
    plain->surface_id = 7;
    const Handle first =
        allocate_shared_resource_handle(source, plain, 0x80000000u, false);
    const Handle dup =
        duplicate_shared_resource_handle(source, first, 0x10000000u, false);
    ASSERT_NE(dup, NULL_HANDLE);
    EXPECT_EQ(shared_resource_object(source, dup).get(),
              shared_resource_object(source, first).get());
    EXPECT_EQ(source.get_access(dup), 0x10000000u);
    set_shared_resource_retainer({});
}
