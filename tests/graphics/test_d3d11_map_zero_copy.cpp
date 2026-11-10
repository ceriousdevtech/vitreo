// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_map_zero_copy.cpp
/// @brief WS6: ID3D11DeviceContext::Map / Unmap of DYNAMIC and STAGING
///        buffers on the zero-copy ring, against the real Metal device.
///
/// Expected values come from Windows: the MAP matrix, re-Map and STAGING
/// rows of tests/oracle_data/d3d11_a06_resource_ops_probe.expected (read
/// here line by line), d3d11_buffer_map_pitch_probe (RowPitch == DepthPitch
/// == ByteWidth rounded up to 4), d3d11_deferred_rules_probe ([map] 0x887C0004,
/// [exec]: every Execute uploads again), and Learn (D3D11_MAP,
/// ID3D11DeviceContext::Map, D3D11_USAGE, CopyResource). Every contract test
/// runs twice: arm A with the ring switched off (the shadow path, the
/// VITREO_WS6=0 kill switch) and arm B with it on. The GPU reader is
/// vtfb_cs_fill: u0[i] = 3 * i + cb0[0].x.

#include <gtest/gtest.h>

#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "winapi/graphics/d3d11/d3d11_buffer_ring.hpp"

#include "../winapi/d3d11/d3d11_test_metal_vt_feedback.hpp"
#include "../winapi/d3d11/vt_feedback_shaders.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

constexpr u32 kRead = D3D11_MAP_READ;
constexpr u32 kWrite = D3D11_MAP_WRITE;
constexpr u32 kReadWrite = D3D11_MAP_READ_WRITE;
constexpr u32 kDiscard = D3D11_MAP_WRITE_DISCARD;
constexpr u32 kNoOverwrite = D3D11_MAP_WRITE_NO_OVERWRITE;
constexpr u32 kDoNotWait = D3D11_MAP_FLAG_DO_NOT_WAIT;
constexpr u32 kCpuRead = D3D11_CPU_ACCESS_READ;
constexpr u32 kCpuWrite = D3D11_CPU_ACCESS_WRITE;
constexpr u32 kStructured = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
/// winerror.h D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD.
constexpr u32 kDeferredMapWithoutDiscard = 0x887C0004u;

}  // namespace

class D3D11MapZeroCopyTest : public VtFeedbackTest {
protected:
    void TearDown() override {
        if (hold) (void)hold->signal_cpu(~0ull >> 1);
        graphics::MetalSubmitQueue::instance().drain();
        d3d11_ws6_override_for_test(std::nullopt);
        VtFeedbackTest::TearDown();
    }

    /// @p body once with the ring off (arm A), once on (arm B).
    void both_arms(const std::function<void(bool ring)>& body) {
        for (const bool ring : {false, true}) {
            SCOPED_TRACE(ring ? "arm B (zero-copy ring)" : "arm A (shadow)");
            d3d11_ws6_override_for_test(ring);
            body(ring);
            if (hold) {
                (void)hold->signal_cpu(~0ull >> 1);
                hold.reset();
            }
            graphics::MetalSubmitQueue::instance().drain();
        }
    }
    /// Everything recorded from here waits until release().
    void gate() {
        auto& backend = graphics::MetalDevice::instance();
        ASSERT_TRUE(backend.supports_fences());
        hold = backend.create_fence(0);
        ASSERT_TRUE(hold && hold->valid());
        ASSERT_TRUE(hold->wait_gpu(1));
    }
    void release() {
        ASSERT_TRUE(hold);
        ASSERT_TRUE(hold->signal_cpu(1));
    }
    std::thread release_later(std::atomic<bool>& released, int ms = 300) {
        return std::thread([this, &released, ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            released.store(true);
            (void)hold->signal_cpu(1);
        });
    }

    GuestAddr dynamic_cb(u32 bytes, const void* init = nullptr) {
        return buffer(bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                      kCpuWrite, 0, 0, init);
    }
    GuestAddr uav_buffer(u32 elements) {
        return buffer(elements * 4, D3D11_USAGE_DEFAULT,
                      D3D11_BIND_UNORDERED_ACCESS, 0, kStructured, 4, nullptr);
    }
    GuestAddr compute_shader() {
        if (m_cs == 0) m_cs = shader(18, vtfb_cs_fill, sizeof(vtfb_cs_fill));
        EXPECT_NE(m_cs, 0u);
        return m_cs;
    }
    /// CS: vtfb_cs_fill with @p cb at b0 and @p uav at u0, one group.
    void dispatch(GuestAddr cb, GuestAddr uav, GuestAddr ctx = 0) {
        const GuestAddr target = ctx != 0 ? ctx : context;
        // One scratch pair for every call: a loop of thousands of
        // dispatches must not use up the fixture's data area.
        if (m_scratch == 0) m_scratch = put(std::array<u64, 2>{0, 0});
        EXPECT_TRUE(memory.write_u64(m_scratch, cb).is_ok());
        EXPECT_TRUE(memory.write_u64(m_scratch + 8, uav).is_ok());
        call(target, 69, {compute_shader(), 0, 0});  // CSSetShader
        call(target, 71, {0, 1, m_scratch});         // CSSetConstantBuffers
        call(target, 68, {0, 1, m_scratch + 8, 0});  // CSSetUnorderedAccessViews
        call(target, 41, {1, 1, 1});                 // Dispatch
    }
    u64 call(GuestAddr ctx, usize slot, std::initializer_list<u64> args) {
        set_arg(0, ctx);
        usize index = 1;
        for (const u64 arg : args) set_arg(index++, arg);
        d3d11.impl_ContextMethod(marshaler, slot);
        return cpu.gpr.rax;
    }
    /// Map(WRITE_DISCARD) @p resource, word 0 = @p value, Unmap.
    GuestAddr discard_write(GuestAddr resource, u32 value) {
        const Mapped m = map(resource, 0, kDiscard);
        EXPECT_EQ(m.hr, d3d11::S_OK);
        EXPECT_TRUE(memory.write_u32(m.data, value).is_ok());
        unmap(resource, 0);
        return m.data;
    }
    void write_bytes(GuestAddr at, u8 value, usize count) {
        std::vector<u8> bytes(count, value);
        ASSERT_TRUE(memory.write_bytes(at, bytes.data(), count).is_ok());
    }
    std::vector<u8> bytes_at(GuestAddr at, usize count) {
        std::vector<u8> out(count, 0xCD);
        EXPECT_TRUE(memory.read_bytes(at, out.data(), count).is_ok());
        return out;
    }
    void release_object(GuestAddr object) {
        set_arg(0, object);
        d3d11.impl_BufferMethod(marshaler, 2);  // IUnknown::Release
    }

    std::shared_ptr<graphics::BackendFence> hold;
    GuestAddr m_cs{0};
    GuestAddr m_scratch{0};
};

namespace {

/// One "MAP buffer <usage> <class> <variant> -> ..." row of the a06 probe.
struct MatrixRow {
    std::string usage;
    std::string cls;
    std::string variant;
    u32 hr{0};
    bool null_out{false};
    bool data_non_null{false};
    u32 row{0};
    u32 depth{0};
};

std::vector<MatrixRow> matrix_rows() {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) +
                     "/d3d11_a06_resource_ops_probe.expected");
    std::vector<MatrixRow> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("MAP buffer ", 0) != 0) continue;
        const usize arrow = line.find("->");
        if (arrow == std::string::npos) continue;
        std::istringstream head(line.substr(11, arrow - 11));
        MatrixRow row;
        head >> row.usage >> row.cls;
        std::string word;
        while (head >> word) {
            row.variant += row.variant.empty() ? word : " " + word;
        }
        std::istringstream tail(line.substr(arrow + 2));
        std::string hr;
        tail >> hr;
        row.hr = static_cast<u32>(std::stoul(hr, nullptr, 16));
        std::string rest;
        std::getline(tail, rest);
        row.null_out = rest.find("(NULL out)") != std::string::npos;
        row.data_non_null = rest.find("pData=ptr") != std::string::npos;
        const usize r = rest.find("Row=");
        const usize d = rest.find("Depth=");
        if (r != std::string::npos) row.row = std::stoul(rest.substr(r + 4));
        if (d != std::string::npos) row.depth = std::stoul(rest.substr(d + 6));
        rows.push_back(row);
    }
    return rows;
}

u32 type_named(const std::string& name) {
    if (name == "READ") return kRead;
    if (name == "WRITE") return kWrite;
    if (name == "READ_WRITE") return kReadWrite;
    if (name == "WRITE_DISCARD") return kDiscard;
    if (name == "WRITE_NO_OVERWRITE") return kNoOverwrite;
    return 0xFFFFFFFFu;
}

}  // namespace

// Every (usage, bind/CPU access, D3D11_MAP, flag) combination of the a06
// probe, with Windows' HRESULT, pData and pitches; a failed Map writes NULL
// to pData and leaves the pitches as the caller left them.
TEST_F(D3D11MapZeroCopyTest, MapLegalityMatrixMatchesWindows) {
    const auto rows = matrix_rows();
    ASSERT_EQ(rows.size(), 150u) << "the a06 MAP matrix";
    both_arms([&](bool) {
        for (const MatrixRow& row : rows) {
            SCOPED_TRACE(row.usage + " " + row.cls + " " + row.variant);
            u32 usage = 0, bind = 0, cpu_access = 0, natural = kRead;
            if (row.usage == "DEFAULT") {
                bind = D3D11_BIND_VERTEX_BUFFER;
            } else if (row.usage == "IMMUTABLE") {
                usage = D3D11_USAGE_IMMUTABLE;
                bind = D3D11_BIND_VERTEX_BUFFER;
            } else if (row.usage == "DYNAMIC") {
                usage = D3D11_USAGE_DYNAMIC;
                cpu_access = kCpuWrite;
                natural = kDiscard;
                bind = row.cls == "VB"       ? D3D11_BIND_VERTEX_BUFFER
                       : row.cls == "IB"     ? D3D11_BIND_INDEX_BUFFER
                       : row.cls == "CB"     ? D3D11_BIND_CONSTANT_BUFFER
                       : row.cls == "SRV"    ? D3D11_BIND_SHADER_RESOURCE
                                             : D3D11_BIND_SHADER_RESOURCE |
                                                D3D11_BIND_VERTEX_BUFFER;
            } else {
                usage = D3D11_USAGE_STAGING;
                cpu_access = row.cls == "R"   ? kCpuRead
                             : row.cls == "W" ? kCpuWrite
                                              : kCpuRead | kCpuWrite;
                natural = row.cls == "R"   ? kRead
                          : row.cls == "W" ? kWrite
                                           : kReadWrite;
            }
            std::vector<u8> init(256, 0x3C);
            const GuestAddr b = buffer(256, usage, bind, cpu_access, 0, 0,
                                       init.data());
            ASSERT_NE(b, 0u);
            u32 type = natural, sub = 0, flags = 0;
            bool null_out = false;
            std::istringstream words(row.variant);
            std::string first, second;
            words >> first >> second;
            if (first == "type") {
                type = static_cast<u32>(std::stoul(second));
            } else if (first == "sub") {
                sub = static_cast<u32>(std::stoul(second));
            } else if (first == "flags") {
                flags = static_cast<u32>(std::stoul(second, nullptr, 16));
            } else if (first == "NULL") {
                null_out = true;
            } else {
                type = type_named(first);
                ASSERT_NE(type, 0xFFFFFFFFu);
                if (second == "DO_NOT_WAIT") flags = kDoNotWait;
            }
            const GuestAddr out = put(std::array<u64, 2>{
                0xAAAAAAAAAAAAAAAAull, 0xAAAAAAAAAAAAAAAAull});
            const u32 hr = static_cast<u32>(context_call(
                14, {b, sub, type, flags, null_out ? 0 : out}));
            EXPECT_EQ(hr, row.hr);
            if (!null_out) {
                const u64 data = memory.read_u64(out).value();
                EXPECT_EQ(data != 0, row.data_non_null);
                EXPECT_EQ(memory.read_u32(out + 8).value(), row.row);
                EXPECT_EQ(memory.read_u32(out + 12).value(), row.depth);
            }
            if (hr == d3d11::S_OK) unmap(b, sub);
            release_object(b);
        }
    });
}

// d3d11_buffer_map_pitch_probe: RowPitch == DepthPitch == ByteWidth rounded
// up to 4.
TEST_F(D3D11MapZeroCopyTest, PitchRoundsUpToFour) {
    both_arms([&](bool) {
        for (const auto& [width, pitch] :
             {std::pair<u32, u32>{6, 8}, {1001, 1004}, {256, 256}}) {
            const GuestAddr b = buffer(width, D3D11_USAGE_DYNAMIC,
                                       D3D11_BIND_VERTEX_BUFFER, kCpuWrite, 0,
                                       0, nullptr);
            const Mapped m = map(b, 0, kDiscard);
            ASSERT_EQ(m.hr, d3d11::S_OK);
            EXPECT_EQ(m.row_pitch, pitch) << width;
            EXPECT_EQ(m.depth_pitch, pitch) << width;
            unmap(b, 0);
        }
    });
}

TEST_F(D3D11MapZeroCopyTest, DynamicMapPointsIntoTheRingAndUnmapPublishesNothing) {
    d3d11_ws6_override_for_test(true);
    const GuestAddr cb = dynamic_cb(65536);
    const GuestAddr uav = buffer_uav(uav_buffer(64), 0, 64);
    const GuestAddr ring_address = D3D11Ws6TestPeer::guest(d3d11, cb);
    ASSERT_NE(ring_address, 0u) << "a DYNAMIC buffer lives on the ring";
    gate();
    dispatch(cb, uav);  // the slice is in use from here
    const auto pool_before = graphics::metal_discard_pool_stats();
    const u64 hazards_before = graphics::metal_buffer_hazard_writes();
    const Mapped m = map(cb, 0, kDiscard);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    EXPECT_EQ(m.data, D3D11Ws6TestPeer::guest(d3d11, cb))
        << "pData is the slice the GPU will read";
    EXPECT_NE(m.data, ring_address) << "the busy slice was left to the GPU";
    write_bytes(m.data, 0x42, 65536);
    unmap(cb, 0);
    const auto pool_after = graphics::metal_discard_pool_stats();
    EXPECT_EQ(pool_after.bytes, pool_before.bytes)
        << "Unmap renamed nothing into the DISCARD pool";
    EXPECT_EQ(graphics::metal_buffer_hazard_writes(), hazards_before);
    const Mapped again = map(cb, 0, kNoOverwrite);
    ASSERT_EQ(again.hr, d3d11::S_OK);
    EXPECT_EQ(again.data, m.data) << "NO_OVERWRITE maps the current slice";
    unmap(cb, 0);
    release();
}

// Learn (D3D11_FEATURE_DATA_D3D11_OPTIONS1, MapOnDefaultBuffers) and
// d3d11_capsmall_probe "mapdef": a DEFAULT buffer with CPU access maps as
// the buffer's own memory. WRITE_DISCARD is not one of those map types.
// The ring is that memory, so Unmap has nothing to diff or upload, and a
// later copy reads the bytes the guest wrote.
TEST_F(D3D11MapZeroCopyTest, MappableDefaultBufferUsesTheRing) {
    both_arms([&](bool ring) {
        std::vector<u8> init(256, 0x11);
        const GuestAddr buf = buffer(256, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_SHADER_RESOURCE, kCpuWrite, 0,
                                     0, init.data());
        const GuestAddr plain = buffer(256, D3D11_USAGE_DEFAULT,
                                       D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                       nullptr);
        EXPECT_EQ(D3D11Ws6TestPeer::guest(d3d11, plain), 0u)
            << "a DEFAULT buffer with no CPU access is not mappable";
        const GuestAddr ring_address = D3D11Ws6TestPeer::guest(d3d11, buf);
        if (ring) {
            EXPECT_NE(ring_address, 0u);
        } else {
            EXPECT_EQ(ring_address, 0u);
        }

        const Mapped discard = map(buf, 0, kDiscard);
        EXPECT_EQ(discard.hr, d3d11::E_INVALIDARG);

        const u64 hazards_before = graphics::metal_buffer_hazard_writes();
        const Mapped m = map(buf, 0, kNoOverwrite);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        if (ring) EXPECT_EQ(m.data, ring_address);
        EXPECT_EQ(bytes_at(m.data, 16), std::vector<u8>(16, 0x11))
            << "the mapping shows the buffer's current contents";
        write_bytes(m.data, 0x5A, 64);
        unmap(buf, 0);
        if (ring) {
            EXPECT_EQ(graphics::metal_buffer_hazard_writes(), hazards_before)
                << "Unmap of ring storage uploads nothing";
        }
        const Mapped again = map(buf, 0, kNoOverwrite);
        ASSERT_EQ(again.hr, d3d11::S_OK);
        if (ring) EXPECT_EQ(again.data, m.data);
        EXPECT_EQ(bytes_at(again.data, 64), std::vector<u8>(64, 0x5A));
        unmap(buf, 0);

        const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0, kCpuRead,
                                         0, 0, nullptr);
        context_call(47, {staging, buf});
        const Mapped read = map(staging, 0, kRead);
        ASSERT_EQ(read.hr, d3d11::S_OK);
        EXPECT_EQ(bytes_at(read.data, 64), std::vector<u8>(64, 0x5A))
            << "the GPU copy reads the bytes the guest wrote";
        EXPECT_EQ(bytes_at(read.data + 64, 192), std::vector<u8>(192, 0x11));
        unmap(staging, 0);
    });
}

// a06 gpu_semantics "WDS dynamic CB": each dispatch reads the constants its
// own DISCARD wrote, with all three queued before any runs.
TEST_F(D3D11MapZeroCopyTest, DiscardRenameIsVisibleToEachDispatch) {
    both_arms([&](bool) {
        const GuestAddr cb = dynamic_cb(256);
        std::array<GuestAddr, 3> outs{};
        for (auto& o : outs) o = uav_buffer(64);
        gate();
        const std::array<u32, 3> values{111, 222, 333};
        for (usize k = 0; k < 3; ++k) {
            discard_write(cb, values[k]);
            dispatch(cb, buffer_uav(outs[k], 0, 64));
        }
        release();
        for (usize k = 0; k < 3; ++k) {
            const auto words = read_buffer(outs[k], 256);
            ASSERT_EQ(words.size(), 64u);
            for (u32 i = 0; i < 64; ++i) {
                ASSERT_EQ(words[i], 3 * i + values[k]) << "dispatch " << k;
            }
        }
    });
}

TEST_F(D3D11MapZeroCopyTest, NoOverwriteWritesTheCurrentSliceInPlace) {
    both_arms([&](bool) {
        const GuestAddr cb = dynamic_cb(256);
        const GuestAddr u0 = uav_buffer(64);
        const GuestAddr u1 = uav_buffer(64);
        const GuestAddr p = discard_write(cb, 1);
        dispatch(cb, buffer_uav(u0, 0, 64));
        const Mapped m = map(cb, 0, kNoOverwrite);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        EXPECT_EQ(m.data, p) << "WRITE_NO_OVERWRITE keeps the storage";
        EXPECT_EQ(memory.read_u32(m.data).value(), 1u)
            << "the current contents stay visible";
        ASSERT_TRUE(memory.write_u32(m.data + 16, 999).is_ok());
        unmap(cb, 0);
        dispatch(cb, buffer_uav(u1, 0, 64));
        for (const GuestAddr u : {u0, u1}) {
            const auto words = read_buffer(u, 256);
            for (u32 i = 0; i < 64; ++i) ASSERT_EQ(words[i], 3 * i + 1) << i;
        }
    });
}

// a06 "dynamic buffer remap while mapped": S_OK twice, one pointer, one
// Unmap.
TEST_F(D3D11MapZeroCopyTest, RemapWhileMappedReturnsTheSamePointer) {
    both_arms([&](bool) {
        const GuestAddr cb = dynamic_cb(256);
        const GuestAddr u = uav_buffer(64);
        discard_write(cb, 5);
        gate();
        dispatch(cb, buffer_uav(u, 0, 64));
        const Mapped first = map(cb, 0, kDiscard);
        ASSERT_EQ(first.hr, d3d11::S_OK);
        ASSERT_TRUE(memory.write_u32(first.data, 6).is_ok());
        const Mapped second = map(cb, 0, kDiscard);
        ASSERT_EQ(second.hr, d3d11::S_OK);
        EXPECT_EQ(second.data, first.data);
        EXPECT_EQ(memory.read_u32(second.data).value(), 6u);
        unmap(cb, 0);
        release();
        const auto words = read_buffer(u, 256);
        EXPECT_EQ(words[0], 5u) << "the queued dispatch kept its constants";
    });
}

// a06 "staging init data": the initial data is the first contents.
TEST_F(D3D11MapZeroCopyTest, InitialDataIsTheFirstSlicesContents) {
    both_arms([&](bool) {
        std::array<u32, 64> init{};
        init[0] = 42;
        const GuestAddr cb = dynamic_cb(256, init.data());
        const GuestAddr u = uav_buffer(64);
        dispatch(cb, buffer_uav(u, 0, 64));
        const auto words = read_buffer(u, 256);
        for (u32 i = 0; i < 64; ++i) ASSERT_EQ(words[i], 3 * i + 42) << i;
        std::vector<u8> bytes(256);
        for (usize i = 0; i < bytes.size(); ++i) bytes[i] = u8(10 + i);
        const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0, kCpuRead,
                                         0, 0, bytes.data());
        const Mapped m = map(staging, 0, kRead);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        EXPECT_EQ(bytes_at(m.data, 256), bytes);
        unmap(staging, 0);
        const GuestAddr zeroed = buffer(256, D3D11_USAGE_STAGING, 0, kCpuRead,
                                        0, 0, nullptr);
        const Mapped z = map(zeroed, 0, kRead);
        ASSERT_EQ(z.hr, d3d11::S_OK);
        EXPECT_EQ(bytes_at(z.data, 256), std::vector<u8>(256, 0));
        unmap(zeroed, 0);
    });
}

TEST_F(D3D11MapZeroCopyTest, StagingReadAfterCopyNeedsNoReadback) {
    both_arms([&](bool ring) {
        const GuestAddr u = uav_buffer(64);
        const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0,
                                         kCpuRead | kCpuWrite, 0, 0, nullptr);
        const GuestAddr cb = dynamic_cb(256);
        discard_write(cb, 77);
        dispatch(cb, buffer_uav(u, 0, 64));
        context_call(47, {staging, u});  // CopyResource
        const Mapped m = map(staging, 0, kRead);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        if (ring) EXPECT_EQ(m.data, D3D11Ws6TestPeer::guest(d3d11, staging));
        const auto words = words_at(m.data, 64);
        for (u32 i = 0; i < 64; ++i) ASSERT_EQ(words[i], 3 * i + 77) << i;
        unmap(staging, 0);
    });
}

// Learn (D3D11_MAP_FLAG_DO_NOT_WAIT): DXGI_ERROR_WAS_STILL_DRAWING with
// pData NULL while the copy is queued; a poll sees it retire.
TEST_F(D3D11MapZeroCopyTest, StagingDoNotWaitReportsStillDrawingUntilTheCopyRetires) {
    both_arms([&](bool) {
        const GuestAddr u = uav_buffer(64);
        const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0, kCpuRead,
                                         0, 0, nullptr);
        const GuestAddr cb = dynamic_cb(256);
        discard_write(cb, 31);
        gate();
        dispatch(cb, buffer_uav(u, 0, 64));
        context_call(47, {staging, u});
        for (int poll = 0; poll < 5; ++poll) {
            const Mapped early = map(staging, 0, kRead, kDoNotWait);
            ASSERT_EQ(early.hr, kDxgiErrorWasStillDrawing) << poll;
            EXPECT_EQ(early.data, 0u);
        }
        release();
        const Mapped m = poll_map(staging, 0);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        const auto words = words_at(m.data, 64);
        for (u32 i = 0; i < 64; ++i) ASSERT_EQ(words[i], 3 * i + 31) << i;
        unmap(staging, 0);
    });
}

// More DISCARD bytes than the ring's bound while the GPU runs nothing:
// Map waits for slices to retire and never hands out one a queued dispatch
// still reads.
TEST_F(D3D11MapZeroCopyTest, DiscardPastTheBudgetWaitsAndEveryDispatchSeesItsOwnConstants) {
    both_arms([&](bool ring) {
        constexpr u32 kIterations = 4200;
        const GuestAddr cb = dynamic_cb(65536);
        const GuestAddr u = uav_buffer(64);
        const GuestAddr view = buffer_uav(u, 0, 64);
        const GuestAddr results = buffer(kIterations * 4, D3D11_USAGE_DEFAULT,
                                         0, 0, 0, 0, nullptr);
        const u64 waits_before = D3D11Ws6TestPeer::waits(d3d11);
        const std::array<u32, 6> box{0, 0, 0, 4, 1, 1};
        const GuestAddr box_address = put(box);
        gate();
        std::atomic<bool> released{false};
        std::thread releaser = release_later(released);
        for (u32 i = 0; i < kIterations; ++i) {
            discard_write(cb, i);
            dispatch(cb, view);
            // CopySubresourceRegion(results, 0, i * 4, 0, 0, u, 0, [0, 4)).
            context_call(46, {results, 0, i * 4u, 0, 0, u, 0, box_address});
        }
        releaser.join();
        if (ring) {
            EXPECT_GT(D3D11Ws6TestPeer::waits(d3d11), waits_before)
                << "the ring's bound was reached and waited";
        }
        const auto words = read_buffer(results, kIterations * 4);
        ASSERT_EQ(words.size(), kIterations);
        for (u32 i = 0; i < kIterations; ++i) {
            ASSERT_EQ(words[i], i) << "dispatch " << i << " saw other constants";
        }
    });
}

TEST_F(D3D11MapZeroCopyTest, ReleasingARingBufferLeavesOtherSlicesMapped) {
    both_arms([&](bool ring) {
        const GuestAddr a = dynamic_cb(256);
        const GuestAddr b = dynamic_cb(256);
        if (ring) {
            EXPECT_EQ(D3D11Ws6TestPeer::guest(d3d11, b) & ~GuestAddr{0xFFFFF},
                      D3D11Ws6TestPeer::guest(d3d11, a) & ~GuestAddr{0xFFFFF})
                << "both slices share a slab";
        }
        discard_write(a, 1);
        release_object(a);
        const GuestAddr p = discard_write(b, 64);
        EXPECT_TRUE(memory.is_mapped(p, 256));
        const GuestAddr u = uav_buffer(64);
        dispatch(b, buffer_uav(u, 0, 64));
        const auto words = read_buffer(u, 256);
        for (u32 i = 0; i < 64; ++i) ASSERT_EQ(words[i], 3 * i + 64) << i;
    });
}

// d3d11_share_ers_probe: a shared buffer keeps its cross-process store.
TEST_F(D3D11MapZeroCopyTest, SharedBuffersStayOnTheirStore) {
    d3d11_ws6_override_for_test(true);
    const GuestAddr shared =
        buffer(256, D3D11_USAGE_STAGING, 0, kCpuRead | kCpuWrite,
               D3D11_RESOURCE_MISC_SHARED, 0, nullptr);
    ASSERT_NE(shared, 0u);
    EXPECT_EQ(D3D11Ws6TestPeer::guest(d3d11, shared), 0u);
    const Mapped m = map(shared, 0, kWrite);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    ASSERT_TRUE(memory.write_u32(m.data, 0x1234).is_ok());
    unmap(shared, 0);
    const Mapped r = map(shared, 0, kRead);
    ASSERT_EQ(r.hr, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(r.data).value(), 0x1234u);
    unmap(shared, 0);
}

// IDXGISurface::Map of a buffer's surface facet goes through the same Map.
TEST_F(D3D11MapZeroCopyTest, DxgiSurfaceMapOfARingBufferUsesTheRing) {
    d3d11_ws6_override_for_test(true);
    const GuestAddr b = buffer(256, D3D11_USAGE_DYNAMIC,
                               D3D11_BIND_VERTEX_BUFFER, kCpuWrite, 0, 0,
                               nullptr);
    const GuestAddr rect = put(std::array<u64, 2>{0, 0});
    set_arg(0, b + 0x30);
    set_arg(1, rect);
    set_arg(2, 2 | 4);  // DXGI_MAP_WRITE | DXGI_MAP_DISCARD
    d3d11.impl_DxgiSurfaceMethod(marshaler, 9);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(rect).value(), 256u);
    EXPECT_EQ(memory.read_u64(rect + 8).value(),
              D3D11Ws6TestPeer::guest(d3d11, b));
    set_arg(0, b + 0x30);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 10);
}

// -- GPU writes into ring buffers --------------------------------------------

// Learn (CopyResource): copies run in order. A copy into STAGING recorded
// after a copy out of it must not change what the earlier copy reads.
TEST_F(D3D11MapZeroCopyTest, CopyIntoStagingWhileQueuedCopyReadsItKeepsOldBytes) {
    both_arms([&](bool) {
        const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0,
                                         kCpuRead | kCpuWrite, 0, 0, nullptr);
        const Mapped w = map(staging, 0, kWrite);
        ASSERT_EQ(w.hr, d3d11::S_OK);
        write_bytes(w.data, 0x11, 256);
        unmap(staging, 0);
        const GuestAddr a = buffer(256, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                   nullptr);
        std::vector<u8> twos(256, 0x22);
        const GuestAddr b = buffer(256, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                   twos.data());
        gate();
        context_call(47, {a, staging});
        context_call(47, {staging, b});
        release();
        const auto copied = read_buffer(a, 256);
        for (u32 word : copied) ASSERT_EQ(word, 0x11111111u);
        const Mapped r = map(staging, 0, kRead);
        ASSERT_EQ(r.hr, d3d11::S_OK);
        EXPECT_EQ(bytes_at(r.data, 256), twos);
        unmap(staging, 0);
    });
}

TEST_F(D3D11MapZeroCopyTest, PartialCopyIntoABusyStagingBufferPreservesTheRest) {
    both_arms([&](bool) {
        const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0,
                                         kCpuRead | kCpuWrite, 0, 0, nullptr);
        std::vector<u8> pattern(256);
        for (usize i = 0; i < 256; ++i) pattern[i] = static_cast<u8>(i);
        const Mapped w = map(staging, 0, kWrite);
        ASSERT_EQ(w.hr, d3d11::S_OK);
        ASSERT_TRUE(memory.write_bytes(w.data, pattern.data(), 256).is_ok());
        unmap(staging, 0);
        const GuestAddr a = buffer(256, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                   nullptr);
        std::vector<u8> fill(32, 0xEE);
        const GuestAddr source = buffer(32, D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                        fill.data());
        const std::array<u32, 6> box{0, 0, 0, 32, 1, 1};
        gate();
        context_call(47, {a, staging});
        context_call(46, {staging, 0, 64, 0, 0, source, 0, put(box)});
        release();
        const auto copied = read_buffer(a, 256);
        for (u32 i = 0; i < 64; ++i) {
            const u32 want = u32(4 * i) | u32(4 * i + 1) << 8 |
                             u32(4 * i + 2) << 16 | u32(4 * i + 3) << 24;
            ASSERT_EQ(copied[i], want) << "the earlier copy read word " << i;
        }
        const Mapped r = map(staging, 0, kRead);
        ASSERT_EQ(r.hr, d3d11::S_OK);
        auto want = pattern;
        std::fill(want.begin() + 64, want.begin() + 96, 0xEE);
        EXPECT_EQ(bytes_at(r.data, 256), want);
        unmap(staging, 0);
    });
}

// Learn (D3D11_USAGE): copies between resources of the same usage are
// allowed; a NO_OVERWRITE map after a copy into a DYNAMIC buffer shows it.
TEST_F(D3D11MapZeroCopyTest, NoOverwriteAfterACopyIntoADynamicBufferSeesTheCopy) {
    both_arms([&](bool) {
        const GuestAddr dyn = buffer(256, D3D11_USAGE_DYNAMIC,
                                     D3D11_BIND_SHADER_RESOURCE, kCpuWrite, 0,
                                     0, nullptr);
        std::vector<u8> fill(256, 0x5C);
        const GuestAddr source = buffer(256, D3D11_USAGE_DYNAMIC,
                                        D3D11_BIND_SHADER_RESOURCE, kCpuWrite,
                                        0, 0, fill.data());
        const std::array<u32, 6> box{0, 0, 0, 64, 1, 1};
        gate();
        std::atomic<bool> released{false};
        context_call(46, {dyn, 0, 0, 0, 0, source, 0, put(box)});
        std::thread releaser = release_later(released, 150);
        const Mapped m = map(dyn, 0, kNoOverwrite);
        releaser.join();
        ASSERT_EQ(m.hr, d3d11::S_OK);
        EXPECT_EQ(bytes_at(m.data, 64), std::vector<u8>(64, 0x5C));
        unmap(dyn, 0);
    });
}

TEST_F(D3D11MapZeroCopyTest, CopyIntoAnIdleStagingBufferKeepsItsSlice) {
    both_arms([&](bool) {
        const GuestAddr staging = buffer(256, D3D11_USAGE_STAGING, 0, kCpuRead,
                                         0, 0, nullptr);
        const Mapped first = map(staging, 0, kRead);
        ASSERT_EQ(first.hr, d3d11::S_OK);
        unmap(staging, 0);
        std::vector<u8> fill(256, 0x61);
        const GuestAddr source = buffer(256, D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_SHADER_RESOURCE, 0, 0, 0,
                                        fill.data());
        context_call(47, {staging, source});
        const Mapped m = map(staging, 0, kRead);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        EXPECT_EQ(m.data, first.data) << "nothing read it: no rename";
        EXPECT_EQ(bytes_at(m.data, 256), fill);
        unmap(staging, 0);
    });
}

// -- Deferred contexts --------------------------------------------------------

// d3d11_deferred_rules_probe [map] / [exec]: a deferred Map hands out
// pitches 0, its bytes reach the resource only at ExecuteCommandList, and
// every execution uploads them again.
TEST_F(D3D11MapZeroCopyTest, CommandListExecutedTwiceUploadsEachTimeAndKeepsSlicesUntilRetired) {
    both_arms([&](bool) {
        std::array<u32, 64> init{};
        init[0] = 5;
        const GuestAddr cb = dynamic_cb(256, init.data());
        const GuestAddr u = uav_buffer(64);
        const GuestAddr view = buffer_uav(u, 0, 64);
        std::array<GuestAddr, 4> r{};
        for (auto& x : r) {
            x = buffer(256, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE,
                       0, 0, 0, nullptr);
        }
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(27, {0, out}), d3d11::S_OK);
        const GuestAddr deferred = memory.read_u64(out).value();
        const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
        ASSERT_EQ(call(deferred, 14, {cb, 0, kDiscard, 0, mapped}),
                  d3d11::S_OK);
        const GuestAddr data = memory.read_u64(mapped).value();
        ASSERT_NE(data, 0u);
        EXPECT_EQ(memory.read_u32(mapped + 8).value(), 0u);
        EXPECT_EQ(memory.read_u32(mapped + 12).value(), 0u);
        ASSERT_TRUE(memory.write_u32(data, 7).is_ok());
        call(deferred, 15, {cb, 0});
        dispatch(cb, view, deferred);
        ASSERT_EQ(call(deferred, 114, {0, out}), d3d11::S_OK);
        const GuestAddr list = memory.read_u64(out).value();
        gate();
        dispatch(cb, view);           // before Execute: the old contents
        context_call(47, {r[0], u});
        context_call(58, {list, 0});  // ExecuteCommandList
        context_call(47, {r[1], u});
        discard_write(cb, 9);
        dispatch(cb, view);
        context_call(47, {r[2], u});
        context_call(58, {list, 0});
        context_call(47, {r[3], u});
        release();
        const std::array<u32, 4> want{5, 7, 9, 7};
        for (usize k = 0; k < 4; ++k) {
            const auto words = read_buffer(r[k], 256);
            ASSERT_EQ(words.size(), 64u);
            for (u32 i = 0; i < 64; ++i) {
                ASSERT_EQ(words[i], 3 * i + want[k]) << "copy " << k;
            }
        }
        set_arg(0, list);
        d3d11.impl_CommandListMethod(marshaler, 2);  // Release
    });
}

TEST_F(D3D11MapZeroCopyTest, DeferredNoOverwriteBeforeDiscardIsRefused) {
    both_arms([&](bool) {
        const GuestAddr cb = dynamic_cb(256);
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(27, {0, out}), d3d11::S_OK);
        const GuestAddr deferred = memory.read_u64(out).value();
        const GuestAddr mapped = put(std::array<u64, 2>{0x1111, 0});
        EXPECT_EQ(static_cast<u32>(
                      call(deferred, 14, {cb, 0, kNoOverwrite, 0, mapped})),
                  kDeferredMapWithoutDiscard);
        EXPECT_EQ(memory.read_u64(mapped).value(), 0u);
    });
}

}  // namespace vitreo::winapi::d3d11_test
