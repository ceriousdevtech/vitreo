/// @file test_d3dkmt_dxcore.cpp
/// @brief PEAK-imported D3DKMT ordinals on ext-ms-win-dx-d3dkmt-dxcore-*.
///
/// Ordinal → name identity from dumpbin /exports gdi32.dll on Windows
/// (forwards to ext-ms-win-dx-d3dkmt-dxcore-l1-1-5.#N). Bodies are the
/// documented loud failure: STATUS_NOT_IMPLEMENTED (no WDDM kernel).

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/d3dkmt/d3dkmt_dxcore.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

const ExportEntry* by_ordinal(const DllExports& e, u16 ordinal) {
    for (const auto& x : e.entries) if (x.ordinal == ordinal) return &x;
    return nullptr;
}

} // namespace

TEST(D3dkmtDxcoreTest, PeakOrdinalsResolveAndRefuseLikeNoKernel) {
    // Spot-check one ordinal from each PEAK-imported contract version plus
    // the adapter open that D3D12Core hits early (#124).
    struct Case { const char* dll; u16 ordinal; const char* name; };
    static constexpr Case kCases[] = {
        {"ext-ms-win-dx-d3dkmt-dxcore-l1-1-0.dll", 124, "D3DKMTOpenAdapterFromLuid"},
        {"ext-ms-win-dx-d3dkmt-dxcore-l1-1-0.dll", 36, "D3DKMTCloseAdapter"},
        {"ext-ms-win-dx-d3dkmt-dxcore-l1-1-0.dll", 2, "D3DKMTCreateAllocation2"},
        {"ext-ms-win-dx-d3dkmt-dxcore-l1-1-1.dll", 220, "D3DKMTPinResources"},
        {"ext-ms-win-dx-d3dkmt-dxcore-l1-1-3.dll", 227, "D3DKMTCancelPresents"},
        {"ext-ms-win-dx-d3dkmt-dxcore-l1-1-4.dll", 229, "D3DKMTCreateNativeFence"},
        {"ext-ms-win-dx-d3dkmt-dxcore-l1-1-5.dll", 235, "D3DKMTIsFeatureEnabled"},
    };

    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);

    for (const Case& c : kCases) {
        const DllExports e = create_d3dkmt_dxcore_thunks(c.dll);
        const ExportEntry* f = by_ordinal(e, c.ordinal);
        ASSERT_NE(f, nullptr) << c.dll << " #" << c.ordinal;
        EXPECT_EQ(f->name, c.name);
        EXPECT_EQ(f->stdcall_bytes, 4u);
        cpu.gpr.rax = 0;
        f->handler(m);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xC0000002u)  // STATUS_NOT_IMPLEMENTED
            << c.name;
    }

    // Full PEAK l1-1-0 import set is registered (90 ordinals).
    const DllExports all = create_d3dkmt_dxcore_thunks(
        "ext-ms-win-dx-d3dkmt-dxcore-l1-1-0.dll");
    EXPECT_EQ(all.entries.size(), 90u);
    std::vector<u16> seen;
    for (const auto& entry : all.entries) {
        ASSERT_NE(entry.ordinal, 0);
        EXPECT_EQ(std::count(seen.begin(), seen.end(), entry.ordinal), 0)
            << "ordinal " << entry.ordinal << " duplicated";
        seen.push_back(entry.ordinal);
    }
}
