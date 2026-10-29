// SPDX-License-Identifier: LGPL-2.1-only
// Split from the former monolithic test_dxgi.cpp; the shared fixture lives
// in dxgi_test_fixture.hpp.

#include "dxgi_test_fixture.hpp"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace vitreo::winapi::dxgi_test {
namespace {

// SetPrivateDataInterface holds a real reference on a guest-owned interface.
//
// The reference is the whole point. A frontend that stored the pointer without
// AddRef would pass any test that only checked the pointer comes back, and would
// read freed guest memory the moment the guest dropped its own reference. So the
// test builds a guest vtable in memory, routes the injected caller through it,
// and counts the AddRef/Release calls that actually arrive.
TEST_F(DxgiManagerTest,
       SetPrivateDataInterfaceAddRefsAndReleasesThroughTheGuestVtable) {
    constexpr GuestAddr GUEST_VTABLE = DATA_BASE + 0x800;
    constexpr GuestAddr GUEST_IFACE_A = DATA_BASE + 0x880;
    constexpr GuestAddr GUEST_IFACE_B = DATA_BASE + 0x890;
    constexpr GuestAddr GUEST_ADDREF = 0x71000010;
    constexpr GuestAddr GUEST_RELEASE = 0x71000020;

    // A guest interface is a pointer to a vtable; slot 1 is AddRef, slot 2 is
    // Release.
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 0 * 8, 0x71000000).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 1 * 8, GUEST_ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 2 * 8, GUEST_RELEASE).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_IFACE_A, GUEST_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_IFACE_B, GUEST_VTABLE).is_ok());

    struct Call {
        GuestAddr function{0};
        GuestAddr self{0};
    };
    std::vector<Call> calls;
    manager.set_guest_function_caller(
        [&calls](GuestAddr function, const u64* args, usize arg_count) -> u64 {
            EXPECT_EQ(arg_count, 1u) << "IUnknown methods take only `this`";
            calls.push_back(
                {function,
                 static_cast<GuestAddr>(arg_count > 0 ? args[0] : 0)});
            return 1;  // a plausible non-zero reference count
        });
    const auto count_of = [&calls](GuestAddr function, GuestAddr self) {
        usize total = 0;
        for (const Call& call : calls) {
            if (call.function == function && call.self == self) ++total;
        }
        return total;
    };

    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const std::array<u8, 16> guid{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01};
    ASSERT_TRUE(memory.write_bytes(
        PRIVATE_GUID_ADDRESS, reinterpret_cast<const Byte*>(guid.data()),
        guid.size()).is_ok());

    const auto set_interface = [&](GuestAddr object, GuestAddr interface_ptr) {
        cpu.gpr.rcx = object;
        cpu.gpr.rdx = PRIVATE_GUID_ADDRESS;
        cpu.gpr.r8 = interface_ptr;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_SetPrivateDataInterface(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto get_private = [&]() {
        (void)memory.write_u32(PRIVATE_SIZE_ADDRESS, 8);
        (void)memory.write_u64(PRIVATE_OUTPUT_ADDRESS, 0);
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = PRIVATE_GUID_ADDRESS;
        cpu.gpr.r8 = PRIVATE_SIZE_ADDRESS;
        cpu.gpr.r9 = PRIVATE_OUTPUT_ADDRESS;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_GetPrivateData(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };

    // Setting the interface takes exactly one reference.
    calls.clear();
    EXPECT_EQ(set_interface(factory, GUEST_IFACE_A), dxgi::S_OK);
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE_A), 1u)
        << "the object must AddRef what it now holds";
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE_A), 0u);

    // GetPrivateData hands the interface back, AddRef'd -- that reference
    // belongs to the caller, so releasing it must not drop the object's own.
    calls.clear();
    EXPECT_EQ(get_private(), dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(PRIVATE_OUTPUT_ADDRESS).value(), GUEST_IFACE_A);
    EXPECT_EQ(memory.read_u32(PRIVATE_SIZE_ADDRESS).value(), 8u)
        << "an interface entry requires one pointer";
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE_A), 1u)
        << "GetPrivateData gives the caller its own reference";

    // Replacing under the same GUID releases the old interface and AddRefs the
    // new one, exactly once each.
    calls.clear();
    EXPECT_EQ(set_interface(factory, GUEST_IFACE_B), dxgi::S_OK);
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE_B), 1u);
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE_A), 1u)
        << "the replaced interface must be released, or the guest can never "
           "free it";
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE_B), 0u);

    // Setting the same pointer again leaves the object holding one reference.
    calls.clear();
    EXPECT_EQ(set_interface(factory, GUEST_IFACE_B), dxgi::S_OK);
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE_B), 1u);
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE_B), 1u)
        << "one AddRef and one Release: the count must not accumulate";

    // NULL releases what was held but keeps a NULL entry (measured, a12
    // [pd] "SetPrivateDataInterface(NULL) ... then get hr=0x00000000
    // size=8"): Get answers S_OK with a NULL pointer and AddRefs nothing.
    calls.clear();
    EXPECT_EQ(set_interface(factory, 0), dxgi::S_OK);
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE_B), 1u);
    calls.clear();
    EXPECT_EQ(get_private(), dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(PRIVATE_OUTPUT_ADDRESS).value(), 0u);
    EXPECT_TRUE(calls.empty());

    // Destroying the object releases everything it still holds. Learn: the
    // object releases the interface when it is destroyed.
    EXPECT_EQ(set_interface(factory, GUEST_IFACE_A), dxgi::S_OK);
    calls.clear();
    cpu.gpr.rcx = factory;
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_Release(marshaler);
    }
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE_A), 1u)
        << "an interface still held at destruction must be released, or the "
           "guest object leaks for the life of the process";
}

// Without the host->guest call wired, the frontend must refuse rather than keep
// a pointer it never took a reference on.
TEST_F(DxgiManagerTest, SetPrivateDataInterfaceRefusesWithoutAGuestCaller) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const std::array<u8, 16> guid{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    ASSERT_TRUE(memory.write_bytes(
        PRIVATE_GUID_ADDRESS, reinterpret_cast<const Byte*>(guid.data()),
        guid.size()).is_ok());
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = PRIVATE_GUID_ADDRESS;
    cpu.gpr.r8 = DATA_BASE + 0x880;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_SetPrivateDataInterface(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_NOTIMPL)
        << "storing the pointer without the AddRef is how a frontend ends up "
           "reading freed guest memory";
}

TEST_F(DxgiManagerTest, PrivateDataUsesSizeQueryAndCopiesBytes) {
    const GuestAddr factory = create_factory();
    write_guid(PRIVATE_GUID_ADDRESS, PRIVATE_DATA_GUID);
    constexpr std::array<u8, 4> value{1, 3, 5, 7};
    ASSERT_TRUE(memory.write_bytes(
        PRIVATE_VALUE_ADDRESS, value.data(), value.size()).is_ok());

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = PRIVATE_GUID_ADDRESS;
    cpu.gpr.r8 = value.size();
    cpu.gpr.r9 = PRIVATE_VALUE_ADDRESS;
    Marshaler set(memory);
    set.attach_cpu(cpu);
    manager.impl_SetPrivateData(set);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);

    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, 0).is_ok());
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = PRIVATE_GUID_ADDRESS;
    cpu.gpr.r8 = PRIVATE_SIZE_ADDRESS;
    cpu.gpr.r9 = 0;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_GetPrivateData(query);
    // A NULL pData is the size query, and it succeeds (measured, a12 [pd]
    // IDXGIAdapter / IDXGIFactory "get size-query ... hr=0x00000000").
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(PRIVATE_SIZE_ADDRESS).value(), value.size());

    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, value.size()).is_ok());
    cpu.gpr.r9 = PRIVATE_OUTPUT_ADDRESS;
    Marshaler get(memory);
    get.attach_cpu(cpu);
    manager.impl_GetPrivateData(get);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    std::array<u8, 4> copied{};
    ASSERT_TRUE(memory.read_bytes(
        PRIVATE_OUTPUT_ADDRESS, copied.data(), copied.size()).is_ok());
    EXPECT_EQ(copied, value);
}

TEST_F(DxgiManagerTest, PrivateInterfaceLeaseSurvivesConcurrentRemovalDuringAddRef) {
    for (const bool is64 : {false, true}) {
    SCOPED_TRACE(is64 ? "x64" : "x86");
    DxgiManager target(memory, is64, dxgi::AdapterInfo{.description = "Test GPU"});
    target.set_method_thunks(method_thunks);
    cpu.mode = is64 ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
    const usize pointer_size = is64 ? 8 : 4;
    const auto write_pointer = [&](GuestAddr address, GuestAddr value) {
        return is64 ? memory.write_u64(address, value).is_ok()
                    : memory.write_u32(address, static_cast<u32>(value)).is_ok();
    };
    constexpr GuestAddr table = DATA_BASE + 0x800;
    constexpr GuestAddr iface = DATA_BASE + 0x880;
    constexpr GuestAddr addref = 0x71000010;
    constexpr GuestAddr release = 0x71000020;
    ASSERT_TRUE(write_pointer(table + COM_SLOT_ADD_REF * pointer_size, addref));
    ASSERT_TRUE(write_pointer(table + COM_SLOT_RELEASE * pointer_size, release));
    ASSERT_TRUE(write_pointer(iface, table));
    write_guid(PRIVATE_GUID_ADDRESS, PRIVATE_DATA_GUID);
    const auto exports = create_dxgi_thunks(target);
    const auto call = [&](const char* name, cpu::CpuState& state,
                          std::initializer_list<u64> args) {
        usize index = 0;
        for (const u64 arg : args) {
            if (is64) {
                switch (index) {
                case 0: state.gpr.rcx = arg; break;
                case 1: state.gpr.rdx = arg; break;
                case 2: state.gpr.r8 = arg; break;
                case 3: state.gpr.r9 = arg; break;
                }
            } else {
                (void)memory.write_u32(state.gpr.rsp + 4 + index * 4,
                                       static_cast<u32>(arg));
            }
            ++index;
        }
        Marshaler marshaler(memory);
        marshaler.attach_cpu(state);
        for (const auto& entry : exports.entries) {
            if (entry.name == name) { call_export(entry, marshaler); return; }
        }
        ADD_FAILURE() << name;
    };
    call("CreateDXGIFactory1", cpu, {IID_ADDRESS, OUTPUT_ADDRESS});
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr factory = is64 ? memory.read_u64(OUTPUT_ADDRESS).value()
                                  : memory.read_u32(OUTPUT_ADDRESS).value();
    std::atomic<u32> references{1};
    bool getting = false;
    target.set_guest_function_caller(
        [&](GuestAddr function, const u64* args, usize count) -> u64 {
            EXPECT_EQ(count, 1u);
            EXPECT_EQ(args[0], iface);
            if (function == release) return --references;
            EXPECT_EQ(function, addref);
            if (getting) {
                // The guest callback must permit another thread to call DXGI.
                // try_lock makes the failure bounded even without the repair.
                std::thread remover([&] {
                    if (!target.api_mutex().try_lock()) {
                        ADD_FAILURE() << "graphics lock held across guest AddRef";
                        return;
                    }
                    target.api_mutex().unlock();
                    cpu::CpuState other = cpu;
                    other.gpr.rsp = STACK_BASE + 0x500;
                    call("_DXGI_SetPrivateData", other,
                         {factory, PRIVATE_GUID_ADDRESS, 0, 0});
                    EXPECT_EQ(other.gpr.rax, dxgi::S_OK);
                });
                remover.join();
                EXPECT_EQ(references.load(), 2u)
                    << "stored reference released before GetPrivateData took its reference";
            }
            return ++references;
        });
    call("_DXGI_SetPrivateDataInterface", cpu,
         {factory, PRIVATE_GUID_ADDRESS, iface});
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    ASSERT_EQ(references.load(), 2u);
    getting = true;
    ASSERT_TRUE(memory.write_u32(PRIVATE_SIZE_ADDRESS, static_cast<u32>(pointer_size)).is_ok());
    call("_DXGI_GetPrivateData", cpu,
         {factory, PRIVATE_GUID_ADDRESS, PRIVATE_SIZE_ADDRESS, PRIVATE_OUTPUT_ADDRESS});
    EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(is64 ? memory.read_u64(PRIVATE_OUTPUT_ADDRESS).value()
                   : memory.read_u32(PRIVATE_OUTPUT_ADDRESS).value(), iface);
    EXPECT_EQ(references.load(), 2u); // guest owner and returned pointer
    --references;                   // caller's Release
    EXPECT_EQ(references.load(), 1u);
    target.set_guest_function_caller({});
    }
}

}  // namespace
}  // namespace vitreo::winapi::dxgi_test
