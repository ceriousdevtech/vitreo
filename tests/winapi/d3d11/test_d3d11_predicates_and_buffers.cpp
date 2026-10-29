// SPDX-License-Identifier: LGPL-2.1-only
//
// Predicates, append-counter copies and buffer object lifetime/validation.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, PredicateCreationAndPredicationStateRoundTrip) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_NE(context, 0u);
    constexpr GuestAddr SCRATCH = GPU_BUFFER_DESC + 0x500;
    constexpr GuestAddr DESC = SCRATCH + 0x00;
    constexpr GuestAddr OUT = SCRATCH + 0x20;
    constexpr GuestAddr VALUE_OUT = SCRATCH + 0x40;
    constexpr GuestAddr DATA = SCRATCH + 0x60;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_QUERY_DESC { Query, MiscFlags }.
    const auto create_predicate = [&](u32 type, u32 misc, GuestAddr out) {
        EXPECT_TRUE(memory.write_u32(DESC, type).is_ok());
        EXPECT_TRUE(memory.write_u32(DESC + 4, misc).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, out);
        d3d11.impl_DeviceMethod(marshaler, 25);
        return cpu.gpr.rax == d3d11::S_OK && out != 0
                   ? memory.read_u64(out).value() : GuestAddr{0};
    };

    // Learn (CreatePredicate): the type "must be a
    // D3D11_QUERY_SO_OVERFLOW_PREDICATE or D3D11_QUERY_OCCLUSION_PREDICATE".
    EXPECT_EQ(create_predicate(D3D11_QUERY_OCCLUSION, 0, OUT), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG)
        << "a plain query type is a caller error, not a gap";
    // The overflow predicate used to decline because there was no
    // stream-output stage to overrun. 11.18 built one with exact counts, so it
    // is created now; D3D11StreamOutTest checks what it reports.
    EXPECT_NE(create_predicate(D3D11_QUERY_SO_OVERFLOW_PREDICATE, 0, OUT), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // D3D11_QUERY_MISC_PREDICATEHINT (1) is legal on an occlusion predicate
    // and nothing else is (measured, d3d11_a10_queries_probe [create]; this
    // test used to expect the hint refused).
    EXPECT_NE(create_predicate(D3D11_QUERY_OCCLUSION_PREDICATE, 1, OUT), 0u);
    EXPECT_EQ(create_predicate(D3D11_QUERY_OCCLUSION_PREDICATE, 2, OUT), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // A null out-parameter is the documented "could this be created" probe.
    (void)create_predicate(D3D11_QUERY_OCCLUSION_PREDICATE, 0, 0);
    EXPECT_EQ(cpu.gpr.rax, 1u) << "S_FALSE for the probe, as CreateQuery does";

    const GuestAddr predicate =
        create_predicate(D3D11_QUERY_OCCLUSION_PREDICATE, 0, OUT);
    ASSERT_NE(predicate, 0u);

    // A plain query is not a predicate, and must not become one.
    EXPECT_TRUE(memory.write_u32(DESC, D3D11_QUERY_OCCLUSION).is_ok());
    EXPECT_TRUE(memory.write_u32(DESC + 4, 0).is_ok());
    set_arg(0, device);
    set_arg(1, DESC);
    set_arg(2, OUT);
    d3d11.impl_DeviceMethod(marshaler, 24);  // CreateQuery
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr plain_query = memory.read_u64(OUT).value();
    ASSERT_NE(plain_query, 0u);

    const auto get_predication = [&]() {
        EXPECT_TRUE(memory.write_u64(OUT, 0xA5A5A5A5ull).is_ok());
        EXPECT_TRUE(memory.write_u32(VALUE_OUT, 0xEEEEEEEEu).is_ok());
        set_arg(0, context);
        set_arg(1, OUT);
        set_arg(2, VALUE_OUT);
        d3d11.impl_ContextMethod(marshaler, 86);
        return std::pair<GuestAddr, u32>{memory.read_u64(OUT).value(),
                                         memory.read_u32(VALUE_OUT).value()};
    };
    const auto set_predication = [&](GuestAddr which, u32 value) {
        set_arg(0, context);
        set_arg(1, which);
        set_arg(2, value);
        d3d11.impl_ContextMethod(marshaler, 30);
    };

    // Default state: nothing set.
    EXPECT_EQ(get_predication().first, 0u);

    set_predication(predicate, 1u);
    auto got = get_predication();
    EXPECT_EQ(got.first, predicate)
        << "GetPredication must return what SetPredication was given";
    EXPECT_EQ(got.second, 1u);
    // GetPredication takes a reference, so dropping it leaves the predicate
    // alive for the context's own reference.
    set_arg(0, predicate);
    d3d11.impl_QueryMethod(marshaler, 2);
    EXPECT_GT(cpu.gpr.rax, 0u) << "GetPredication must AddRef";

    // A plain query passed to SetPredication is dropped, and must not replace
    // the predicate that is set.
    set_predication(plain_query, 0u);
    EXPECT_EQ(get_predication().first, predicate)
        << "a non-predicate must be refused, not installed";

    // Learn: "While the predicate is set for predication, calls to Begin and
    // End are invalid." Re-measuring the predicate that is predicating is the
    // case that cannot be allowed.
    set_arg(0, context);
    set_arg(1, predicate);
    d3d11.impl_ContextMethod(marshaler, 27);  // Begin
    set_arg(0, context);
    set_arg(1, predicate);
    d3d11.impl_ContextMethod(marshaler, 28);  // End
    set_arg(0, context);
    set_arg(1, predicate);
    set_arg(2, DATA);
    set_arg(3, 4);
    d3d11.impl_ContextMethod(marshaler, 29);  // GetData
    EXPECT_EQ(cpu.gpr.rax, DXGI_ERROR_INVALID_CALL)
        << "the dropped Begin/End left the predicate never issued, which "
           "Windows answers with DXGI_ERROR_INVALID_CALL (measured) -- "
           "proving End really was refused";

    // Learn: a NULL predicate means no predication, and "the value of
    // PredicateValue is irrelevant but will be preserved for GetPredication".
    set_predication(0, 1u);
    got = get_predication();
    EXPECT_EQ(got.first, 0u);
    EXPECT_EQ(got.second, 1u) << "the value is preserved even with no predicate";

    // Now that nothing is predicating, Begin/End work. The bracket drew
    // nothing, so the predicate is FALSE (measured: "empty bracket ...
    // PREDICATE value=0"; this used to report a fabricated TRUE).
    set_arg(0, context);
    set_arg(1, predicate);
    d3d11.impl_ContextMethod(marshaler, 27);
    set_arg(0, context);
    set_arg(1, predicate);
    d3d11.impl_ContextMethod(marshaler, 28);
    ASSERT_TRUE(memory.write_u32(DATA, 0xEEEEEEEEu).is_ok());
    set_arg(0, context);
    set_arg(1, predicate);
    set_arg(2, DATA);
    set_arg(3, 4);
    d3d11.impl_ContextMethod(marshaler, 29);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DATA).value(), 0u) << "no samples: FALSE";

    // ID3D11Predicate {9eb576dd-9f77-4d86-81aa-8bab5fe490e2}. It derives from
    // ID3D11Query and adds no methods, so a predicate answers to both -- but a
    // plain query must not claim the interface, or a guest would pass it to
    // SetPredication after a successful QueryInterface.
    constexpr std::array<u8, 16> kPredicateIid{
        0xDD, 0x76, 0xB5, 0x9E, 0x77, 0x9F, 0x86, 0x4D,
        0x81, 0xAA, 0x8B, 0xAB, 0x5F, 0xE4, 0x90, 0xE2};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(kPredicateIid.data()),
        kPredicateIid.size()).is_ok());
    const auto query_interface = [&](GuestAddr object) {
        EXPECT_TRUE(memory.write_u64(OUT, 0xA5A5A5A5ull).is_ok());
        set_arg(0, object);
        set_arg(1, DESC);
        set_arg(2, OUT);
        d3d11.impl_QueryMethod(marshaler, 0);
        return cpu.gpr.rax;
    };
    EXPECT_EQ(query_interface(predicate), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(OUT).value(), predicate);
    set_arg(0, predicate);
    d3d11.impl_QueryMethod(marshaler, 2);  // drop the QI reference
    EXPECT_EQ(query_interface(plain_query), d3d11::E_NOINTERFACE);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);

    // ClearState returns the context to its default state, and the default is
    // no predication -- value included.
    set_predication(predicate, 1u);
    ASSERT_EQ(get_predication().first, predicate);
    set_arg(0, context);
    d3d11.impl_ContextMethod(marshaler, 110);  // ClearState
    got = get_predication();
    EXPECT_EQ(got.first, 0u);
    EXPECT_EQ(got.second, 0u)
        << "ClearState must not leave a predicate value from before";
}

TEST_F(D3D11ManagerTest, AppendUavCounterIsCopiedByCopyStructureCount) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    // Laid out well clear of the fixture's other addresses: an earlier version
    // put the initial-counts array on top of the bound-view array, so the count
    // read back was the low word of a guest pointer.
    constexpr GuestAddr SCRATCH = GPU_BUFFER_DESC + 0x400;
    constexpr GuestAddr UAV_DESC = SCRATCH + 0x00;
    constexpr GuestAddr UAV_OUT = SCRATCH + 0x40;
    constexpr GuestAddr DST_DESC = SCRATCH + 0x80;
    constexpr GuestAddr DST_OUT = SCRATCH + 0xC0;
    constexpr GuestAddr UAV_ARRAY = SCRATCH + 0x100;
    constexpr GuestAddr COUNTS = SCRATCH + 0x140;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto make_buffer = [&](const std::array<u32, 6>& desc,
                                 GuestAddr desc_at, GuestAddr out) {
        EXPECT_TRUE(memory.write_bytes(
            desc_at, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, desc_at);
        set_arg(2, 0);
        set_arg(3, out);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(out).value();
    };
    // ByteWidth 32, DEFAULT, bind UAV, no CPU access, MISC_STRUCTURED,
    // stride 4 -- eight 32-bit records.
    const GuestAddr buffer =
        make_buffer({32, 0, 0x80, 0, 0x40, 4}, GPU_BUFFER_DESC, GPU_BUFFER_OUT);
    ASSERT_NE(buffer, 0u);
    // The destination is an ordinary buffer -- Learn: "any buffer resource that
    // other copy commands ... are able to write to". 16 bytes, so an offset of
    // 12 is the last dword that fits and 16 is one past the end.
    const GuestAddr dst =
        make_buffer({16, 0, 0x80, 0, 0, 0}, DST_DESC, DST_OUT);
    ASSERT_NE(dst, 0u);

    const auto create_uav = [&](const std::array<u32, 5>& uav_desc) {
        EXPECT_TRUE(memory.write_bytes(
            UAV_DESC, reinterpret_cast<const Byte*>(uav_desc.data()),
            sizeof(uav_desc)).is_ok());
        set_arg(0, device);
        set_arg(1, buffer);
        set_arg(2, UAV_DESC);
        set_arg(3, UAV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 8);
        return cpu.gpr.rax == d3d11::S_OK ? memory.read_u64(UAV_OUT).value()
                                          : GuestAddr{0};
    };
    // Format UNKNOWN, BUFFER, first 0, 8 elements, flags.
    const GuestAddr append_uav = create_uav({0, 1, 0, 8, 2});
    ASSERT_NE(append_uav, 0u) << "APPEND on a structured buffer is legal";
    const GuestAddr plain_uav = create_uav({0, 1, 0, 8, 0});
    ASSERT_NE(plain_uav, 0u);

    // Validation, all from Learn.
    // RAW over a structured buffer: measured on Windows as E_INVALIDARG, not
    // a not-implemented report. Windows judges the combination before support,
    // and a raw view needs ALLOW_RAW_VIEWS plus R32_TYPELESS.
    EXPECT_EQ(create_uav({0, 1, 0, 8, 1}), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(create_uav({0, 1, 0, 8, 6}), 0u);  // APPEND|COUNTER
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG)
        << "APPEND and COUNTER are alternatives, not a combination";
    EXPECT_EQ(create_uav({42, 1, 0, 8, 4}), 0u);  // COUNTER with a format
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG)
        << "a structured view names DXGI_FORMAT_UNKNOWN";
    EXPECT_EQ(create_uav({0, 1, 0, 8, 8}), 0u);  // undefined flag bit
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_NE(context, 0u);
    const auto copy_structure_count = [&](GuestAddr destination, u32 offset,
                                          GuestAddr view) {
        backend.state.buffer_region_copies.clear();
        set_arg(0, context);
        set_arg(1, destination);
        set_arg(2, offset);
        set_arg(3, view);
        d3d11.impl_ContextMethod(marshaler, 49);
    };

    copy_structure_count(dst, 8, append_uav);
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u)
        << "CopyStructureCount must reach the backend, or the guest reads a "
           "destination nothing wrote";
    const auto& copy = backend.state.buffer_region_copies.front();
    EXPECT_EQ(copy.dst_offset, 8u) << "the guest's offset, not zero";
    EXPECT_EQ(copy.src_offset, 0u);
    EXPECT_EQ(copy.bytes, 4u) << "the count is a 32-bit UINT";
    EXPECT_EQ(copy.src_byte_width, 4u)
        << "the source is the four-byte hidden counter, not the 32-byte "
           "structured buffer it counts";

    // Returns void, so an argument it cannot serve is dropped with a
    // diagnostic. Each of these would otherwise write a wrong value.
    copy_structure_count(dst, 8, plain_uav);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty())
        << "a UAV without APPEND or COUNTER has no counter to copy";
    copy_structure_count(dst, 6, append_uav);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty())
        << "a misaligned offset would split the count across two dwords";
    copy_structure_count(dst, 16, append_uav);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty())
        << "the four bytes must fit in the destination";
    copy_structure_count(dst, 12, append_uav);
    EXPECT_EQ(backend.state.buffer_region_copies.size(), 1u)
        << "the last dword that fits is still in range";
    copy_structure_count(0, 0, append_uav);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty());
    copy_structure_count(dst, 0, 0);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty());

    // pUAVInitialCounts: Learn says -1 keeps the current offset and the value
    // is only relevant for APPEND/COUNTER views. The counter is the only
    // four-byte buffer here, so a write to it is identifiable by its length.
    const auto bind_compute_uav = [&](GuestAddr view, u32 initial) {
        EXPECT_TRUE(memory.write_u32(COUNTS, initial).is_ok());
        backend.state.buffer_updates = 0;
        backend.state.last_buffer_update.clear();
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 1);
        set_arg(3, UAV_ARRAY);
        set_arg(4, COUNTS);
        EXPECT_TRUE(memory.write_u64(UAV_ARRAY, view).is_ok());
        d3d11.impl_ContextMethod(marshaler, 68);
    };

    // The initial count is written on the GPU timeline, as a copy into the
    // counter, so a CopyStructureCount recorded before the rebind still reads
    // the old count (a09b probe [append/consume]: 9, not the new 7).
    backend.state.buffer_region_copies.clear();
    bind_compute_uav(append_uav, 5u);
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u)
        << "the initial count must be written to the counter";
    EXPECT_EQ(backend.state.buffer_region_copies[0].bytes, 4u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, 0u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_contents,
              (std::vector<u8>{5, 0, 0, 0}));
    EXPECT_EQ(backend.state.buffer_updates, 0u)
        << "never a CPU write, which would overtake queued copies";

    backend.state.buffer_region_copies.clear();
    bind_compute_uav(append_uav, 0xFFFFFFFFu);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty())
        << "-1 keeps the current offset; rewriting it would reset a count the "
           "shader had already moved";

    bind_compute_uav(plain_uav, 7u);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty())
        << "a UAV without a counter has nothing to initialise, and the runtime "
           "ignores the value rather than failing";
}

TEST_F(D3D11ManagerTest, CreatesImmutableMetalBackedBuffer) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 6> desc{
        32, 1, 1, 0, 0, 0,
    };
    const std::array<u8, 32> initial_data{
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23,
        24, 25, 26, 27, 28, 29, 30, 31,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DATA,
        reinterpret_cast<const Byte*>(initial_data.data()),
        initial_data.size()).is_ok());
    ASSERT_TRUE(memory.write_u64(
        GPU_BUFFER_INITIAL, GPU_BUFFER_DATA).is_ok());
    ASSERT_TRUE(memory.write_u32(GPU_BUFFER_INITIAL + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(GPU_BUFFER_INITIAL + 12, 0).is_ok());

    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, GPU_BUFFER_INITIAL);
    set_arg(3, GPU_BUFFER_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(backend.state.buffer_creations, 1u);
    EXPECT_EQ(backend.state.last_buffer_desc.byte_width, 32u);
    EXPECT_TRUE(backend.state.last_buffer_desc.vertex_buffer);
    EXPECT_EQ(backend.state.last_buffer_initial_data,
              std::vector<u8>(initial_data.begin(), initial_data.end()));

    set_arg(0, buffer);
    set_arg(1, GPU_BUFFER_DESC);
    d3d11.impl_BufferMethod(marshaler, 10);
    EXPECT_EQ(memory.read_u32(GPU_BUFFER_DESC).value(), 32u);
    EXPECT_EQ(memory.read_u32(GPU_BUFFER_DESC + 4).value(), 1u);
    EXPECT_EQ(memory.read_u32(GPU_BUFFER_DESC + 8).value(), 1u);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11BUFFER),
        sizeof(IID_ID3D11BUFFER)).is_ok());
    set_arg(0, buffer);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_BufferMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(INTERFACE_OUT).value(), buffer);
}

TEST_F(D3D11ManagerTest, ReleasedBuffersDoNotExhaustObjectStorage) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 6> desc{32, 0, 1, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    for (unsigned iteration = 0; iteration < 8192; ++iteration) {
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << iteration;
        const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
        set_arg(0, buffer);
        d3d11.impl_BufferMethod(marshaler, 2);
        ASSERT_EQ(cpu.gpr.rax, 0u);
    }
    EXPECT_EQ(backend.state.buffer_creations, 8192u);
}

TEST_F(D3D11ManagerTest, ReleasedTexturesAndViewsDoNotExhaustObjectStorage) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{4, 4, 1, 1, 28, 1, 0, 0, 8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    for (unsigned iteration = 0; iteration < 8192; ++iteration) {
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, TEXTURE_OUT);
        d3d11.impl_DeviceMethod(marshaler, 5);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << iteration;
        const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, 0);
        set_arg(3, INTERFACE_OUT);
        d3d11.impl_DeviceMethod(marshaler, 7);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << iteration;
        const GuestAddr view = memory.read_u64(INTERFACE_OUT).value();
        // The view keeps the texture alive, but Release answers the
        // application's count (measured: "texture last external Release=0").
        set_arg(0, texture);
        d3d11.impl_Texture2DMethod(marshaler, 2);
        ASSERT_EQ(cpu.gpr.rax, 0u);
        set_arg(0, view);
        d3d11.impl_ShaderResourceViewMethod(marshaler, 2);
        ASSERT_EQ(cpu.gpr.rax, 0u);
    }
}

TEST_F(D3D11ManagerTest, ValidatesBufferWithoutBackendAllocation) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 6> default_desc{
        64, 0, 4, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC,
        reinterpret_cast<const Byte*>(default_desc.data()),
        sizeof(default_desc)).is_ok());

    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, 0);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 3);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(backend.state.buffer_creations, 0u);

    const std::array<u32, 6> immutable_desc{
        64, 1, 1, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC,
        reinterpret_cast<const Byte*>(immutable_desc.data()),
        sizeof(immutable_desc)).is_ok());
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(backend.state.buffer_creations, 0u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
