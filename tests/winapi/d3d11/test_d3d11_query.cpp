// SPDX-License-Identifier: LGPL-2.1-only
//
// Queries: creation matrix, GetData state machine, occlusion, predication, pipeline statistics.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// Queries and predication (area 10). Every expected HRESULT and value is a
// line of tools/oracle_probe/d3d11_a10_queries_probe.expected or
// d3d11_a10_query_rules_probe.expected (Windows, WARP), or a D3D11.3 rule
// cited where it is used.
// ---------------------------------------------------------------------------

class D3D11QueryTest : public D3D11ManagerTest {
protected:
    void SetUp() override {
        D3D11ManagerTest::SetUp();
        ASSERT_TRUE(memory.map_region(Q_BASE, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        create_default_device();
        device = memory.read_u64(DEVICE_OUT).value();
        context = memory.read_u64(CONTEXT_OUT).value();
        ASSERT_NE(context, 0u);
        marshaler.attach_cpu(cpu);
    }

    u32 device_call(usize slot, std::initializer_list<u64> args) {
        set_arg(0, device);
        usize i = 1;
        for (const u64 arg : args) set_arg(i++, arg);
        d3d11.impl_DeviceMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 context_call(usize slot, std::initializer_list<u64> args) {
        set_arg(0, context);
        usize i = 1;
        for (const u64 arg : args) set_arg(i++, arg);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 query_call(GuestAddr query, usize slot,
                   std::initializer_list<u64> args = {}) {
        set_arg(0, query);
        usize i = 1;
        for (const u64 arg : args) set_arg(i++, arg);
        d3d11.impl_QueryMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }

    /// CreateQuery (slot 24) or CreatePredicate (25); 0 on failure.
    GuestAddr create(u32 type, u32 misc = 0, usize slot = 24) {
        EXPECT_TRUE(memory.write_u32(DESC, type).is_ok());
        EXPECT_TRUE(memory.write_u32(DESC + 4, misc).is_ok());
        EXPECT_TRUE(memory.write_u64(OUT, 0).is_ok());
        last = device_call(slot, {DESC, OUT});
        return last == d3d11::S_OK ? memory.read_u64(OUT).value() : 0;
    }
    void begin(GuestAddr query) { (void)context_call(27, {query}); }
    void end(GuestAddr query) { (void)context_call(28, {query}); }
    /// GetData(query, data, size, flags) with DATA pre-filled with 0xEE.
    u32 get(GuestAddr query, GuestAddr data, u32 size, u32 flags = 0) {
        for (u32 i = 0; i < 96; i += 8) {
            EXPECT_TRUE(memory.write_u64(DATA + i, 0xEEEEEEEEEEEEEEEEull)
                            .is_ok());
        }
        set_arg(0, context);
        set_arg(1, query);
        set_arg(2, data);
        set_arg(3, size);
        set_arg(4, flags);
        d3d11.impl_ContextMethod(marshaler, 29);
        return static_cast<u32>(cpu.gpr.rax);
    }
    [[nodiscard]] bool data_untouched() {
        return memory.read_u64(DATA).value() == 0xEEEEEEEEEEEEEEEEull;
    }
    void set_predication(GuestAddr predicate, u32 value) {
        (void)context_call(30, {predicate, value});
    }

    /// A 16x16 R8G8B8A8_UNORM render target bound with the oracle VS/PS and a
    /// triangle list, so Draw reaches the backend.
    void setup_draw() {
        const std::array<u32, 11> rt_desc{16, 16, 1, 1, 28, 1, 0, 0, 0x20, 0,
                                          0};
        ASSERT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(rt_desc.data()),
            sizeof(rt_desc)).is_ok());
        ASSERT_EQ(device_call(5, {TEXTURE_DESC, 0, Q_TEXTURE_OUT}),
                  d3d11::S_OK);
        ASSERT_EQ(device_call(9, {memory.read_u64(Q_TEXTURE_OUT).value(), 0,
                                  Q_RTV_OUT}),
                  d3d11::S_OK);
        rtv = memory.read_u64(Q_RTV_OUT).value();
        ASSERT_TRUE(memory.write_bytes(Q_VS, vs_main, sizeof(vs_main)).is_ok());
        ASSERT_EQ(device_call(12, {Q_VS, sizeof(vs_main), 0, Q_VS_OUT}),
                  d3d11::S_OK);
        ASSERT_TRUE(
            memory.write_bytes(Q_PS, ps_color, sizeof(ps_color)).is_ok());
        ASSERT_EQ(device_call(15, {Q_PS, sizeof(ps_color), 0, Q_PS_OUT}),
                  d3d11::S_OK);
        pixel_shader = memory.read_u64(Q_PS_OUT).value();
        (void)context_call(11, {memory.read_u64(Q_VS_OUT).value(), 0, 0});
        (void)context_call(9, {pixel_shader, 0, 0});
        ASSERT_TRUE(memory.write_u64(Q_BIND, rtv).is_ok());
        (void)context_call(33, {1, Q_BIND, 0});  // OMSetRenderTargets
        (void)context_call(24, {4});  // IASetPrimitiveTopology TRIANGLELIST
    }
    void draw(u32 vertices = 3) { (void)context_call(13, {vertices, 0}); }

    static constexpr GuestAddr Q_BASE = 0x10000;
    static constexpr GuestAddr DESC = Q_BASE + 0x00;
    static constexpr GuestAddr OUT = Q_BASE + 0x40;
    static constexpr GuestAddr DATA = Q_BASE + 0x80;
    static constexpr GuestAddr Q_TEXTURE_OUT = Q_BASE + 0x100;
    static constexpr GuestAddr Q_RTV_OUT = Q_BASE + 0x108;
    static constexpr GuestAddr Q_VS_OUT = Q_BASE + 0x110;
    static constexpr GuestAddr Q_PS_OUT = Q_BASE + 0x118;
    static constexpr GuestAddr Q_BIND = Q_BASE + 0x120;
    static constexpr GuestAddr Q_DSS_DESC = Q_BASE + 0x140;
    static constexpr GuestAddr Q_DSS_OUT = Q_BASE + 0x180;
    static constexpr GuestAddr Q_CS_OUT = Q_BASE + 0x188;
    static constexpr GuestAddr Q_VS = Q_BASE + 0x400;
    static constexpr GuestAddr Q_PS = Q_BASE + 0x1400;
    static constexpr GuestAddr Q_CS = Q_BASE + 0x2800;

    Marshaler marshaler{memory};
    GuestAddr device{0};
    GuestAddr context{0};
    GuestAddr rtv{0};
    GuestAddr pixel_shader{0};
    u32 last{0};
};

// Learn (ID3D11DeviceContext::Flush): the driver may send queued commands at
// any time; Flush only makes it happen now. The immediate context submits on
// its own once at least 256 draws are recorded AND the backend says the GPU is
// running dry -- never while it is busy, and never a smaller chunk.
TEST_F(D3D11QueryTest, ImmediateDrawsSubmitEarlyOnlyWhenTheGpuRunsDry) {
    setup_draw();
    const unsigned draws_before = backend.state.draws;
    draw();
    ASSERT_EQ(backend.state.draws, draws_before + 1) << "the draw must reach "
                                                        "the backend";
    backend.state.gpu_wants_work = false;
    const unsigned before = backend.state.flushes;
    for (int i = 0; i < 300; ++i) draw();
    EXPECT_EQ(backend.state.flushes, before) << "a busy GPU gets no early "
                                                "submit";
    backend.state.gpu_wants_work = true;
    draw();
    EXPECT_EQ(backend.state.flushes, before + 1)
        << "past the chunk with the GPU running dry: submit now";
    for (int i = 0; i < 255; ++i) draw();
    EXPECT_EQ(backend.state.flushes, before + 1) << "a chunk is 256 draws";
    draw();
    EXPECT_EQ(backend.state.flushes, before + 2);
    backend.state.gpu_wants_work = false;
}

TEST_F(D3D11QueryTest, CreationMatrixMatchesWindows) {
    // [create]: CreateQuery accepts every D3D11_QUERY value with MiscFlags 0;
    // PREDICATEHINT (1) only on OCCLUSION_PREDICATE; any other bit or value
    // is E_INVALIDARG with *ppQuery NULL -- never E_NOTIMPL. A NULL out
    // parameter answers S_FALSE after the same validation.
    const std::array<u32, 16> size{4, 8, 8, 16, 88, 4, 16, 4,
                                   16, 4, 16, 4, 16, 4, 16, 4};
    constexpr std::array<u8, 16> kPredicateIid{
        0xDD, 0x76, 0xB5, 0x9E, 0x77, 0x9F, 0x86, 0x4D,
        0x81, 0xAA, 0x8B, 0xAB, 0x5F, 0xE4, 0x90, 0xE2};
    for (const u32 type : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u,
                           12u, 13u, 14u, 15u, 16u, 0xFFFFFFFFu}) {
        for (const u32 misc : {0u, 1u, 2u}) {
            const bool ok = type < 16 && (misc == 0 || (misc == 1 && type == 5));
            ASSERT_TRUE(memory.write_u64(OUT, 0x1111).is_ok());
            ASSERT_TRUE(memory.write_u32(DESC, type).is_ok());
            ASSERT_TRUE(memory.write_u32(DESC + 4, misc).is_ok());
            EXPECT_EQ(device_call(24, {DESC, OUT}),
                      ok ? d3d11::S_OK : d3d11::E_INVALIDARG)
                << "CreateQuery type " << type << " misc " << misc;
            const GuestAddr query = memory.read_u64(OUT).value();
            if (!ok) {
                EXPECT_EQ(query, 0u) << "out NULL on failure";
            } else {
                EXPECT_EQ(query_call(query, 7), size[type])
                    << "GetDataSize type " << type;
                EXPECT_EQ(query_call(query, 8, {DESC + 0x20}), 0u);
                EXPECT_EQ(memory.read_u32(DESC + 0x20).value(), type);
                EXPECT_EQ(memory.read_u32(DESC + 0x24).value(), misc);
                ASSERT_TRUE(memory.write_bytes(
                    DESC + 0x30,
                    reinterpret_cast<const Byte*>(kPredicateIid.data()),
                    kPredicateIid.size()).is_ok());
                const bool predicate = type == 5 || (type >= 7 && (type & 1));
                EXPECT_EQ(query_call(query, 0, {DESC + 0x30, DESC + 0x28}),
                          predicate ? d3d11::S_OK : d3d11::E_NOINTERFACE)
                    << "QI(ID3D11Predicate) type " << type;
                if (predicate) (void)query_call(query, 2);
                EXPECT_EQ(query_call(query, 2), 0u);
            }
            EXPECT_EQ(device_call(24, {DESC, 0}),
                      ok ? 1u : d3d11::E_INVALIDARG)
                << "CreateQuery(NULL out) type " << type;

            // CreatePredicate takes OCCLUSION_PREDICATE (misc 0 or HINT) and
            // SO_OVERFLOW_PREDICATE (misc 0) only -- not the per-stream
            // overflow predicates. *ppPredicate is left untouched for a type it
            // does not take and set NULL for a predicate type with bad flags.
            const bool pred_type = type == 5 || type == 7;
            const bool pred_ok =
                (type == 5 && misc <= 1) || (type == 7 && misc == 0);
            ASSERT_TRUE(memory.write_u64(OUT, 0x1111).is_ok());
            EXPECT_EQ(device_call(25, {DESC, OUT}),
                      pred_ok ? d3d11::S_OK : d3d11::E_INVALIDARG)
                << "CreatePredicate type " << type << " misc " << misc;
            if (pred_ok) {
                EXPECT_EQ(query_call(memory.read_u64(OUT).value(), 2), 0u);
            } else {
                EXPECT_EQ(memory.read_u64(OUT).value(),
                          pred_type ? 0u : 0x1111u)
                    << "CreatePredicate out, type " << type;
            }
            EXPECT_EQ(device_call(25, {DESC, 0}),
                      pred_ok ? 1u : d3d11::E_INVALIDARG);
        }
    }
    // [null]: CreateQuery(NULL) -> out NULL; CreatePredicate(NULL) -> out
    // untouched.
    ASSERT_TRUE(memory.write_u64(OUT, 0x1111).is_ok());
    EXPECT_EQ(device_call(24, {0, OUT}), d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    ASSERT_TRUE(memory.write_u64(OUT, 0x1111).is_ok());
    EXPECT_EQ(device_call(25, {0, OUT}), d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0x1111u);

    // d3d11_a10_query_rules_probe [create1]: ContextType 2..4 is refused from
    // PIPELINE_STATISTICS on, accepted for EVENT..TIMESTAMP_DISJOINT.
    for (u32 type = 0; type < 16; ++type) {
        for (u32 context_type = 0; context_type < 5; ++context_type) {
            ASSERT_TRUE(memory.write_u32(DESC, type).is_ok());
            ASSERT_TRUE(memory.write_u32(DESC + 4, 0).is_ok());
            ASSERT_TRUE(memory.write_u32(DESC + 8, context_type).is_ok());
            ASSERT_TRUE(memory.write_u64(OUT, 0x1111).is_ok());
            const bool ok = type < 4 || context_type <= 1;
            EXPECT_EQ(device_call(60, {DESC, OUT}),
                      ok ? d3d11::S_OK : d3d11::E_INVALIDARG)
                << "CreateQuery1 type " << type << " ctx " << context_type;
            if (ok) {
                EXPECT_EQ(query_call(memory.read_u64(OUT).value(), 2), 0u);
            } else {
                EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
            }
        }
    }
}

TEST_F(D3D11QueryTest, GetDataStateMachineMatchesWindows) {
    // [getdata], no draws anywhere, for every type.
    constexpr u32 kInvalidCall = DXGI_ERROR_INVALID_CALL;
    for (u32 type = 0; type < 16; ++type) {
        SCOPED_TRACE(type);
        const u32 size = query_data_size(type);
        const GuestAddr query = create(type);
        ASSERT_NE(query, 0u);
        const bool begin_ignored = type == 0 || type == 2;
        const auto complete = [&] {
            for (auto& point : backend.state.gpu_points) point->is_ready = true;
        };
        // Never issued, and building: DXGI_ERROR_INVALID_CALL, nothing written.
        EXPECT_EQ(get(query, DATA, size), kInvalidCall);
        EXPECT_TRUE(data_untouched());
        EXPECT_EQ(get(query, 0, 0), kInvalidCall);
        begin(query);
        EXPECT_EQ(get(query, 0, 0), kInvalidCall);
        EXPECT_EQ(get(query, DATA, size), kInvalidCall);
        end(query);
        complete();
        EXPECT_EQ(get(query, DATA, size), d3d11::S_OK);
        if (type == 1) EXPECT_EQ(memory.read_u64(DATA).value(), 0u);
        if (type == 0) EXPECT_EQ(memory.read_u32(DATA).value(), 1u);
        if (type >= 4 && type != 3) {
            // Empty bracket: every count zero (OCCLUSION 0, predicates FALSE,
            // SO {0,0}, PIPELINE_STATISTICS all 0).
            for (u32 i = 0; i < size; i += 4) {
                EXPECT_EQ(memory.read_u32(DATA + i).value(), 0u) << i;
            }
        }
        // Signalled: DataSize 0 is a status poll whatever pData is; any size
        // but the exact one, (NULL, size) and an undefined flag are
        // E_INVALIDARG and write nothing; DONOTFLUSH reads.
        EXPECT_EQ(get(query, 0, 0), d3d11::S_OK);
        EXPECT_EQ(get(query, DATA, 0), d3d11::S_OK);
        EXPECT_TRUE(data_untouched());
        for (const u32 wrong : {size - 1, size + 1, size * 2, 4u, 8u}) {
            if (wrong == size) continue;
            EXPECT_EQ(get(query, DATA, wrong), d3d11::E_INVALIDARG) << wrong;
            EXPECT_TRUE(data_untouched());
        }
        EXPECT_EQ(get(query, 0, size), d3d11::E_INVALIDARG);
        EXPECT_EQ(get(query, DATA, size, 1), d3d11::S_OK);
        EXPECT_FALSE(data_untouched());
        EXPECT_EQ(get(query, DATA, size, 2), d3d11::E_INVALIDARG);
        EXPECT_TRUE(data_untouched());
        // Begin again: building again, except where Begin is ignored.
        begin(query);
        EXPECT_EQ(get(query, DATA, size),
                  begin_ignored ? d3d11::S_OK : kInvalidCall);
        (void)query_call(query, 2);
        // End without Begin: S_OK, an empty range (D3D11.3 20.3.4).
        const GuestAddr lone = create(type);
        end(lone);
        complete();
        EXPECT_EQ(get(lone, DATA, size), d3d11::S_OK);
        if (type == 1) EXPECT_EQ(memory.read_u64(DATA).value(), 0u);
        (void)query_call(lone, 2);
    }
    // d3d11_a10_query_rules_probe [order]: arguments are validated before the
    // state, so a never-issued query answers E_INVALIDARG for them.
    const GuestAddr fresh = create(1);
    EXPECT_EQ(get(fresh, DATA, 8, 2), d3d11::E_INVALIDARG);
    EXPECT_EQ(get(fresh, DATA, 4), d3d11::E_INVALIDARG);
    EXPECT_EQ(get(fresh, 0, 8), d3d11::E_INVALIDARG);
    EXPECT_EQ(get(fresh, DATA, 0, 2), d3d11::E_INVALIDARG);
    // [hint]: a PREDICATEHINT predicate never returns data, not even status.
    const GuestAddr hint = create(5, 1, 25);
    ASSERT_NE(hint, 0u);
    EXPECT_EQ(get(hint, 0, 0), d3d11::E_INVALIDARG);
    begin(hint);
    end(hint);
    EXPECT_EQ(get(hint, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(get(hint, DATA, 4), d3d11::E_INVALIDARG);
}

TEST_F(D3D11QueryTest, PollingFlushesOnlyWhileTheQueryIsUnsubmitted) {
    // Learn (D3D11_ASYNC_GETDATA_FLAG): GetData without DONOTFLUSH flushes so
    // that a query polled in a loop completes; DONOTFLUSH can loop forever
    // because the commands the query waits on are never sent. Once they are,
    // a further flush cannot help the query, and each one was a submission:
    // UE polls between draws, and every submission past two in flight waited
    // for the GPU.
    setup_draw();
    backend.state.generations = true;
    const GuestAddr occlusion = create(1);
    const GuestAddr event = create(0);
    ASSERT_TRUE(occlusion != 0 && event != 0);

    begin(occlusion);
    draw();
    end(occlusion);
    const unsigned flushes = backend.state.flushes;
    EXPECT_EQ(get(occlusion, DATA, 8), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 1)
        << "the counted draw is still in the open recording";
    EXPECT_EQ(get(occlusion, DATA, 8), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 1)
        << "already submitted: the query completes without another flush";
    backend.state.occlusion_counters.back()->count = 7;
    backend.state.completed_gen = backend.state.recording_gen;
    EXPECT_EQ(get(occlusion, DATA, 8), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 7u);

    // EVENT is answered by a point in the command stream, not by counted
    // work: the same rule applies to the generation the point went into.
    end(event);
    ASSERT_FALSE(backend.state.gpu_points.empty());
    TestGpuPoint& point = *backend.state.gpu_points.back();
    EXPECT_EQ(get(event, DATA, 4), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 2)
        << "the point is still in the open recording";
    EXPECT_EQ(get(event, DATA, 4), 1u);
    EXPECT_EQ(get(event, DATA, 4, 1), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 2);
    point.is_ready = true;
    EXPECT_EQ(get(event, DATA, 4), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u);

    // Work recorded after the End does not make the submitted point pending.
    end(event);
    EXPECT_EQ(get(event, DATA, 4), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 3);
    draw();
    EXPECT_EQ(get(event, DATA, 4), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 3);
}

TEST_F(D3D11QueryTest, OcclusionBracketsSumExactlyTheirOwnIntervals) {
    setup_draw();
    backend.state.generations = true;
    const GuestAddr q1 = create(1);
    const GuestAddr q2 = create(1);
    const GuestAddr p1 = create(5, 0, 25);
    ASSERT_TRUE(q1 != 0 && q2 != 0 && p1 != 0);

    // [occlusion] "overlap q1{full,half} q2{half,quarter}": Begin q1; full;
    // Begin q2; half; End q1; quarter; End q2 -> q1 6144, q2 3072 on a 64x64
    // target. Each draw here lands in its own interval, and the test plays
    // the GPU by giving each interval the probe's per-draw sample count.
    const usize passes = backend.state.pass_occlusion_counters.size();
    draw();  // outside every bracket: counts into nothing
    ASSERT_EQ(backend.state.pass_occlusion_counters.size(), passes + 1);
    EXPECT_EQ(backend.state.pass_occlusion_counters.back(), nullptr);
    begin(q1);
    begin(p1);
    draw();  // full
    begin(q2);
    draw();  // half
    end(q1);
    end(p1);
    draw();  // quarter
    end(q2);
    ASSERT_EQ(backend.state.occlusion_counters.size(), 3u)
        << "one GPU counter per interval with draws";
    auto& counters = backend.state.occlusion_counters;
    const auto& pass_counters = backend.state.pass_occlusion_counters;
    EXPECT_EQ(pass_counters[passes + 1], counters[0].get());
    EXPECT_EQ(pass_counters[passes + 2], counters[1].get());
    EXPECT_EQ(pass_counters[passes + 3], counters[2].get());
    counters[0]->count = 4096;
    counters[1]->count = 2048;
    counters[2]->count = 1024;

    // Final only once the generation holding the draws has completed; a
    // polling GetData flushes, DONOTFLUSH does not.
    const unsigned flushes = backend.state.flushes;
    EXPECT_EQ(get(q1, DATA, 8), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 1);
    EXPECT_EQ(get(q1, DATA, 8, 1), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 1);
    EXPECT_TRUE(data_untouched());
    backend.state.completed_gen = backend.state.recording_gen;
    EXPECT_EQ(get(q1, DATA, 8), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 6144u);
    EXPECT_EQ(get(q2, DATA, 8), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 3072u);
    EXPECT_EQ(get(p1, DATA, 4), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u);

    // "Begin; full; Begin; half; End" = 2048: a second Begin restarts.
    begin(q1);
    draw();
    begin(q1);
    draw();
    end(q1);
    ASSERT_EQ(counters.size(), 5u);
    counters[3]->count = 4096;
    counters[4]->count = 2048;
    backend.state.completed_gen = backend.state.recording_gen;
    EXPECT_EQ(get(q1, DATA, 8), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 2048u);

    // "empty bracket" and "ClearRTV + ClearDSV only": 0 and FALSE.
    begin(q1);
    begin(p1);
    ASSERT_TRUE(memory.write_bytes(COLOR,
        reinterpret_cast<const Byte*>(std::array<float, 4>{}.data()), 16)
                    .is_ok());
    (void)context_call(50, {rtv, COLOR});
    end(q1);
    end(p1);
    EXPECT_EQ(counters.size(), 5u) << "a clear opens no interval";
    EXPECT_EQ(get(q1, DATA, 8), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 0u);
    EXPECT_EQ(get(p1, DATA, 4), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DATA).value(), 0u);
}

// Learn (GetData): S_OK "indicates that the data at pData is available".
// The count is final once the generation the counted pass was RECORDED in
// has completed, not the one that was open when the draw started: another
// thread can submit that recording before the pass joins it, and the pass
// then lands in the next one (the backend's counter reports which). Here the
// "other thread" submits exactly in that gap, every time.
TEST_F(D3D11QueryTest, OcclusionWaitsForTheGenerationItsPassWasRecordedIn) {
    setup_draw();
    backend.state.generations = true;
    const GuestAddr query = create(1);           // OCCLUSION
    const GuestAddr predicate = create(5, 0, 25);  // guaranteed predicate
    ASSERT_TRUE(query != 0 && predicate != 0);

    begin(query);
    begin(predicate);
    const u64 open = backend.state.recording_gen;
    backend.state.before_counted_pass = [&] {
        ++backend.state.recording_gen;  // `open` handed to the GPU
    };
    draw();
    backend.state.before_counted_pass = nullptr;
    end(query);
    end(predicate);
    ASSERT_EQ(backend.state.recording_gen, open + 1);
    ASSERT_EQ(backend.state.occlusion_counters.size(), 1u);
    backend.state.occlusion_counters.back()->count = 4096;

    // The other thread's list retired; the pass's own has not.
    backend.state.completed_gen = open;
    const unsigned flushes = backend.state.flushes;
    EXPECT_EQ(get(query, DATA, 8, 1), 1u)
        << "S_FALSE: the generation holding the counted pass is not done";
    EXPECT_TRUE(data_untouched());
    EXPECT_EQ(get(query, DATA, 8), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 1)
        << "the pass is in the open recording: polling must send it";

    // Guaranteed predication waits for that same generation, not `open`.
    const unsigned draws = backend.state.draws;
    set_predication(predicate, 1);
    draw();
    set_predication(0, 0);
    ASSERT_FALSE(backend.state.generation_waits.empty());
    EXPECT_EQ(backend.state.generation_waits.back(), open + 1);
    EXPECT_EQ(backend.state.draws, draws)
        << "the predicate's data (TRUE: samples passed) equals the "
           "PredicateValue, so the draw is skipped";

    backend.state.completed_gen = open + 1;
    EXPECT_EQ(get(query, DATA, 8), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 4096u);
}

TEST_F(D3D11QueryTest, PredicationSkipsTheListedOperationsWhenDataEqualsValue) {
    setup_draw();
    backend.state.generations = true;
    // Two occlusion predicates the "GPU" answers: pF saw nothing, pT samples.
    const GuestAddr pF = create(5, 0, 25);
    const GuestAddr pT = create(5, 0, 25);
    const GuestAddr hint = create(5, 1, 25);
    ASSERT_TRUE(pF != 0 && pT != 0 && hint != 0);
    begin(pF);
    end(pF);
    begin(pT);
    draw();
    end(pT);
    begin(hint);
    draw();
    end(hint);
    ASSERT_EQ(backend.state.occlusion_counters.size(), 2u);
    backend.state.occlusion_counters[0]->count = 4096;
    backend.state.occlusion_counters[1]->count = 0;

    const auto runs = [&](GuestAddr predicate, u32 value) {
        const unsigned draws = backend.state.draws;
        set_predication(predicate, value);
        draw();
        set_predication(0, 0);
        return backend.state.draws != draws;
    };
    // [predication] Draw: skipped for (pF,FALSE) and (pT,TRUE), ran for
    // (pF,TRUE) and (pT,FALSE). pT is not final yet: guaranteed predication
    // waits for the generation holding its draw (never runs on a guess).
    EXPECT_TRUE(backend.state.generation_waits.empty());
    EXPECT_FALSE(runs(pT, 1));
    EXPECT_FALSE(backend.state.generation_waits.empty())
        << "an unfinished occlusion predicate is waited for";
    EXPECT_TRUE(runs(pT, 0));
    EXPECT_FALSE(runs(pF, 0));
    EXPECT_TRUE(runs(pF, 1));

    // A PREDICATEHINT predicate whose result is not final draws anyway
    // (Learn D3D11_QUERY_MISC_FLAG); once final it is applied.
    begin(hint);
    draw();
    end(hint);
    backend.state.occlusion_counters.back()->count = 0;
    const usize waits = backend.state.generation_waits.size();
    EXPECT_TRUE(runs(hint, 0)) << "not final: a hint does not wait";
    EXPECT_EQ(backend.state.generation_waits.size(), waits);
    backend.state.completed_gen = backend.state.recording_gen;
    EXPECT_FALSE(runs(hint, 0)) << "final: FALSE == FALSE skips";

    // d3d11_a10_query_rules_probe [predicate]: a predicate with no data (never
    // issued) reads FALSE.
    const GuestAddr fresh = create(5, 0, 25);
    EXPECT_FALSE(runs(fresh, 0));
    EXPECT_TRUE(runs(fresh, 1));

    // The other listed operations are skipped too; Map is not predicated.
    const unsigned clears = backend.state.texture_clears;
    set_predication(pF, 0);
    ASSERT_TRUE(memory.write_bytes(COLOR,
        reinterpret_cast<const Byte*>(std::array<float, 4>{}.data()), 16)
                    .is_ok());
    (void)context_call(50, {rtv, COLOR});  // ClearRenderTargetView
    EXPECT_EQ(backend.state.texture_clears, clears);
    set_predication(pF, 1);
    (void)context_call(50, {rtv, COLOR});
    EXPECT_GT(backend.state.texture_clears, clears);
    set_predication(0, 0);

    // [predication] "OCCLUSION bracket around a predicated-off Draw" = 0.
    const GuestAddr q = create(1);
    set_predication(pF, 0);
    begin(q);
    draw();
    end(q);
    set_predication(0, 0);
    EXPECT_EQ(get(q, DATA, 8), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 0u);

    // The slot table is Learn's SetPredication list (the audit script checks
    // the names against d3d11_3.h); Begin/End/GetData/Map are not on it.
    for (const usize slot : {12u, 13u, 20u, 21u, 38u, 39u, 40u, 41u, 42u, 46u,
                             47u, 48u, 49u, 50u, 51u, 52u, 53u, 54u, 57u,
                             115u, 116u, 132u}) {
        EXPECT_TRUE(predicated_context_slot(slot)) << slot;
    }
    for (const usize slot : {14u, 15u, 27u, 28u, 29u, 30u, 86u}) {
        EXPECT_FALSE(predicated_context_slot(slot)) << slot;
    }
}

TEST_F(D3D11QueryTest, PipelineStatisticsCountOnTheCpuAndTheGpuTimeline) {
    setup_draw();
    const GuestAddr stats = create(4);
    ASSERT_NE(stats, 0u);
    // [pipeline] "empty bracket": every counter 0.
    begin(stats);
    end(stats);
    ASSERT_EQ(get(stats, DATA, 88), d3d11::S_OK);
    for (u32 i = 0; i < 88; i += 8) {
        EXPECT_EQ(memory.read_u64(DATA + i).value(), 0u) << i;
    }

    // [pipeline] "Draw(6) full, NULL PS" with depth off: IAV=6 IAP=2 VS=6,
    // everything else 0 -- D3D11.3 16.9.1: a NULL pixel shader with depth
    // and stencil disabled disables rasterization, so CInvocations,
    // CPrimitives and PSInvocations do not move.
    // D3D11_DEPTH_STENCIL_DESC: DepthEnable FALSE, WriteMask ALL, LESS,
    // StencilEnable FALSE, masks 0xFF, faces KEEP/KEEP/KEEP/ALWAYS.
    const std::array<u32, 13> dss{0, 1, 2, 0, 0xFFFF, 1, 1, 1, 8, 1, 1, 1, 8};
    ASSERT_TRUE(memory.write_bytes(
        Q_DSS_DESC, reinterpret_cast<const Byte*>(dss.data()),
        sizeof(dss)).is_ok());
    ASSERT_EQ(device_call(21, {Q_DSS_DESC, Q_DSS_OUT}), d3d11::S_OK);
    (void)context_call(36, {memory.read_u64(Q_DSS_OUT).value(), 0});
    (void)context_call(9, {0, 0, 0});  // PSSetShader(NULL)
    begin(stats);
    draw(6);
    end(stats);
    ASSERT_EQ(get(stats, DATA, 88), d3d11::S_OK);
    const std::array<u64, 11> null_ps{6, 2, 6, 0, 0, 0, 0, 0, 0, 0, 0};
    for (u32 i = 0; i < 11; ++i) {
        EXPECT_EQ(memory.read_u64(DATA + i * 8).value(), null_ps[i])
            << PIPELINE_STATISTIC_NAMES[i];
    }

    // With a pixel shader the rest is counted on the GPU timeline: the
    // pixel shader variant adds PSInvocations to the interval's record, the
    // vertex shader captures its outputs while it draws, and
    // vitreo_stats_primitives clips them (CPrimitives). GetData answers
    // S_FALSE until that work retired, never an estimate. This backend runs
    // no shader, so the GPU's counters read 0 here (test_pipeline_stats_gpu
    // counts them on Metal); the CPU's are [pipeline] "Draw(3) diagonal
    // triangle": IAV=3 IAP=1 VS=3 C=1, laid out in d3d11.h's field order
    // (IAVertices, IAPrimitives, VSInvocations, GSInvocations, GSPrimitives,
    // CInvocations, CPrimitives, PSInvocations, HSInvocations,
    // DSInvocations, CSInvocations -- this test used to expect D3D11.3's
    // listing order, which put CInvocations at offset 56).
    backend.state.live_buffers = true;
    backend.state.generations = true;
    (void)context_call(9, {pixel_shader, 0, 0});
    const usize dispatches = backend.state.dispatched_entries.size();
    begin(stats);
    draw(3);
    end(stats);
    EXPECT_NE(backend.state.last_draw_call.fragment_statistics_buffer,
              nullptr);
    EXPECT_NE(backend.state.last_draw_call.vertex_capture_buffer, nullptr)
        << "the drawn vertex shader captures its outputs";
    EXPECT_NE(backend.state.last_draw_pipeline_desc.fragment_shader_msl.find(
                  "vitreo_pipeline_statistics"),
              std::string::npos);
    ASSERT_GT(backend.state.dispatched_entries.size(), dispatches);
    EXPECT_EQ(backend.state.dispatched_entries.back(),
              graphics::kStatsPrimitivesEntryPoint);
    EXPECT_EQ(get(stats, DATA, 88, 1), 1u) << "S_FALSE until the GPU retired";
    backend.state.completed_gen = backend.state.recording_gen;
    ASSERT_EQ(get(stats, DATA, 88), d3d11::S_OK);
    const std::array<u64, 11> triangle{3, 1, 3, 0, 0, 1, 0, 0, 0, 0, 0};
    for (u32 i = 0; i < 11; ++i) {
        EXPECT_EQ(memory.read_u64(DATA + i * 8).value(), triangle[i])
            << PIPELINE_STATISTIC_NAMES[i];
    }
    backend.state.live_buffers = false;
    backend.state.generations = false;

    // CSInvocations = groups x threads (D3D11.3 20.4.7); cs_lifetime is
    // numthreads(1,1,1).
    ASSERT_TRUE(
        memory.write_bytes(Q_CS, cs_lifetime, sizeof(cs_lifetime)).is_ok());
    ASSERT_EQ(device_call(18, {Q_CS, sizeof(cs_lifetime), 0, Q_CS_OUT}),
              d3d11::S_OK);
    (void)context_call(69, {memory.read_u64(Q_CS_OUT).value(), 0, 0});
    begin(stats);
    (void)context_call(41, {2, 3, 1});
    end(stats);
    ASSERT_EQ(get(stats, DATA, 88), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA + 10 * 8).value(), 6u);
    EXPECT_EQ(memory.read_u64(DATA).value(), 0u);
}

// Learn (GetData): S_OK "indicates that the data at pData is available". The
// counts the GPU adds to a PIPELINE_STATISTICS bracket (PSInvocations from
// the draw itself, CPrimitives from the dispatch after it) are final once
// the generation the draw was RECORDED in has completed -- not the one open
// while the frontend prepared it (its statistics record, its capture):
// another thread can submit that recording before the draw joins it, and
// the draw then lands in the next one. Here the "other thread" submits
// exactly there.
TEST_F(D3D11QueryTest, PipelineStatisticsWaitForTheGenerationTheirDrawWasRecordedIn) {
    setup_draw();
    backend.state.live_buffers = true;
    backend.state.generations = true;
    const GuestAddr stats = create(4);
    ASSERT_NE(stats, 0u);
    begin(stats);
    const u64 open = backend.state.recording_gen;
    bool fired = false;
    backend.state.before_draw = [&] {
        if (fired) return;
        fired = true;
        ++backend.state.recording_gen;  // `open` handed to the GPU
    };
    draw(3);
    backend.state.before_draw = nullptr;
    end(stats);
    ASSERT_TRUE(fired);
    ASSERT_EQ(backend.state.recording_gen, open + 1);
    ASSERT_NE(backend.state.last_draw_call.fragment_statistics_buffer, nullptr)
        << "the draw counts its pixel-shader invocations on the GPU";

    // The other thread's list retired; the draw's own has not.
    backend.state.completed_gen = open;
    EXPECT_EQ(get(stats, DATA, 88, 1), 1u)
        << "S_FALSE: the generation holding the counted draw is not done";
    backend.state.completed_gen = open + 1;
    ASSERT_EQ(get(stats, DATA, 88), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 3u) << "IAVertices";
    backend.state.live_buffers = false;
    backend.state.generations = false;
}

// The skip tally counts from process start, switch on or off: a draw a
// guest issues once at startup (UE's LUT bake) and that is skipped then must
// still be in the report when the marker is created later. Only the skip
// formats count; any other diagnostic line does not.
TEST(D3D11SkipStats, SkipFormatsCountFromProcessStartSwitchOnOrOff) {
    const u64 before = d3d11_skip_events();
    d3d11_skip_stat("ID3D11DeviceContext::Draw vertices=%u (test, not a skip)");
    EXPECT_EQ(d3d11_skip_events(), before);
    static const char kSkip[] = "Draw skipped: the tally's own test";
    d3d11_skip_stat(kSkip);
    d3d11_skip_stat(kSkip);
    d3d11_skip_stat("Draw failed: the tally's own test");
    EXPECT_EQ(d3d11_skip_events(), before + 3);
}

// d3d11.h's D3D11_QUERY_DATA_PIPELINE_STATISTICS keeps D3D10's eight fields
// first; tools/audit_d3d11_query_constants.py checks the enum against the
// header, this the byte offsets GetData writes through it.
TEST(D3D11PipelineStatistics, FieldOrderIsTheSdkStruct) {
    EXPECT_EQ(static_cast<u32>(PipelineStatistic::GSInvocations) * 8, 24u);
    EXPECT_EQ(static_cast<u32>(PipelineStatistic::CInvocations) * 8, 40u);
    EXPECT_EQ(static_cast<u32>(PipelineStatistic::PSInvocations) * 8, 56u);
    EXPECT_EQ(static_cast<u32>(PipelineStatistic::HSInvocations) * 8, 64u);
    EXPECT_EQ(static_cast<u32>(PipelineStatistic::DSInvocations) * 8, 72u);
    EXPECT_EQ(static_cast<u32>(PipelineStatistic::CSInvocations) * 8, 80u);
    EXPECT_STREQ(PIPELINE_STATISTIC_NAMES[3], "GSInvocations");
    EXPECT_STREQ(PIPELINE_STATISTIC_NAMES[8], "HSInvocations");
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
