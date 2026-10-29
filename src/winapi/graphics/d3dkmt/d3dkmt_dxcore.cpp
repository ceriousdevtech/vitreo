/// @file d3dkmt_dxcore.cpp
/// @brief D3DKMT ordinals on the dxcore API-set contracts.
///
/// Identity source: dumpbin /exports gdi32.dll on Windows — each D3DKMT*
/// export is a forward to ext-ms-win-dx-d3dkmt-dxcore-l1-1-5.#N. The same
/// ordinal numbers are imported from the older l1-1-{0,1,3,4} contracts.
///
/// Vitreo has no WDDM kernel / dxgkrnl path. Bodies return
/// STATUS_NOT_IMPLEMENTED and log once per API so GetProcAddress succeeds
/// while callers see an honest failure (never a fake success).

#include "d3dkmt_dxcore.hpp"

#include "winapi/core/marshaler.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>

namespace vitreo::winapi {
namespace {

constexpr u32 STATUS_NOT_IMPLEMENTED = 0xC0000002u;

struct OrdinalName {
    u16 ordinal;
    const char* name;
};

// clang-format off
static constexpr OrdinalName kD3dkmtOrdinals[] = {
// AUTO-GENERATED from dumpbin /exports C:\Windows\System32\gdi32.dll
// on Windows (Hostx64 dumpbin, 2026-09-23). Ordinal identity comes from
// gdi32 forward entries "D3DKMT* (forwarded to ext-ms-win-dx-d3dkmt-dxcore-l1-1-5.#N)".
// Do not hand-type ordinals; re-run the dumpbin parse to refresh.
    {2, "D3DKMTCreateAllocation2"},
    {3, "D3DKMTQueryResourceInfo"},
    {4, "D3DKMTQueryResourceInfoFromNtHandle"},
    {5, "D3DKMTShareObjects"},
    {6, "D3DKMTOpenNtHandleFromName"},
    {7, "D3DKMTOpenResourceFromNtHandle"},
    {9, "D3DKMTOpenResource"},
    {11, "D3DKMTDestroyAllocation"},
    {12, "D3DKMTSetAllocationPriority"},
    {13, "D3DKMTQueryAllocationResidency"},
    {14, "D3DKMTCreateDevice"},
    {15, "D3DKMTDestroyDevice"},
    {16, "D3DKMTCreateContext"},
    {17, "D3DKMTDestroyContext"},
    {18, "D3DKMTCreateSynchronizationObject"},
    {19, "D3DKMTCreateSynchronizationObject2"},
    {20, "D3DKMTOpenSynchronizationObject"},
    {21, "D3DKMTDestroySynchronizationObject"},
    {22, "D3DKMTWaitForSynchronizationObject"},
    {23, "D3DKMTWaitForSynchronizationObject2"},
    {24, "D3DKMTSignalSynchronizationObject"},
    {25, "D3DKMTSignalSynchronizationObject2"},
    {26, "D3DKMTLock"},
    {27, "D3DKMTUnlock"},
    {30, "D3DKMTGetMultisampleMethodList"},
    {31, "D3DKMTPresent"},
    {32, "D3DKMTRender"},
    {34, "D3DKMTQueryAdapterInfo"},
    {36, "D3DKMTCloseAdapter"},
    {38, "D3DKMTEscape"},
    {49, "D3DKMTGetDeviceState"},
    {51, "D3DKMTSetContextSchedulingPriority"},
    {57, "D3DKMTSetQueuedLimit"},
    {65, "D3DKMTSetDisplayPrivateDriverFormat"},
    {69, "D3DKMTCreateKeyedMutex2"},
    {71, "D3DKMTOpenKeyedMutex2"},
    {72, "D3DKMTDestroyKeyedMutex"},
    {74, "D3DKMTAcquireKeyedMutex2"},
    {76, "D3DKMTReleaseKeyedMutex2"},
    {80, "D3DKMTOfferAllocations"},
    {100, "D3DKMTDestroyAllocation2"},
    {101, "D3DKMTMakeResident"},
    {102, "D3DKMTEvict"},
    {103, "D3DKMTUpdateAllocationProperty"},
    {104, "D3DKMTWaitForSynchronizationObjectFromCpu"},
    {105, "D3DKMTSignalSynchronizationObjectFromCpu"},
    {106, "D3DKMTWaitForSynchronizationObjectFromGpu"},
    {107, "D3DKMTSignalSynchronizationObjectFromGpu"},
    {108, "D3DKMTSignalSynchronizationObjectFromGpu2"},
    {109, "D3DKMTCreatePagingQueue"},
    {110, "D3DKMTDestroyPagingQueue"},
    {111, "D3DKMTLock2"},
    {112, "D3DKMTUnlock2"},
    {113, "D3DKMTInvalidateCache"},
    {114, "D3DKMTMapGpuVirtualAddress"},
    {115, "D3DKMTReserveGpuVirtualAddress"},
    {116, "D3DKMTFreeGpuVirtualAddress"},
    {117, "D3DKMTUpdateGpuVirtualAddress"},
    {119, "D3DKMTCreateContextVirtual"},
    {120, "D3DKMTSubmitCommand"},
    {121, "D3DKMTReclaimAllocations2"},
    {124, "D3DKMTOpenAdapterFromLuid"},
    {126, "D3DKMTMarkDeviceAsError"},
    {129, "D3DKMTOpenSyncObjectFromNtHandle2"},
    {130, "D3DKMTOpenSyncObjectNtHandleFromName"},
    {141, "D3DKMTSetStablePowerState"},
    {142, "D3DKMTQueryClockCalibration"},
    {160, "D3DKMTCreateHwQueue"},
    {161, "D3DKMTDestroyHwQueue"},
    {162, "D3DKMTSubmitCommandToHwQueue"},
    {163, "D3DKMTSubmitWaitForSyncObjectsToHwQueue"},
    {164, "D3DKMTSubmitSignalSyncObjectsToHwQueue"},
    {169, "D3DKMTGetAllocationPriority"},
    {184, "D3DKMTCreateProtectedSession"},
    {185, "D3DKMTDestroyProtectedSession"},
    {186, "D3DKMTQueryProtectedSessionStatus"},
    {187, "D3DKMTQueryProtectedSessionInfoFromNtHandle"},
    {188, "D3DKMTOpenProtectedSessionFromNtHandle"},
    {189, "D3DKMTCreateBundleObject"},
    {190, "D3DKMTExtractBundleObject"},
    {193, "D3DKMTPresentRedirected"},
    {204, "D3DKMTOpenBundleObjectNtHandleFromName"},
    {205, "D3DKMTSetContextInProcessSchedulingPriority"},
    {206, "D3DKMTSubmitPresentToHwQueue"},
    {207, "D3DKMTCreateTrackedWorkload"},
    {208, "D3DKMTDestroyTrackedWorkload"},
    {209, "D3DKMTGetAvailableTrackedWorkloadIndex"},
    {210, "D3DKMTUpdateTrackedWorkload"},
    {212, "D3DKMTGetTrackedWorkloadStatistics"},
    {213, "D3DKMTResetTrackedWorkloadStatistics"},
    {220, "D3DKMTPinResources"},
    {221, "D3DKMTUnpinResources"},
    {227, "D3DKMTCancelPresents"},
    {229, "D3DKMTCreateNativeFence"},
    {230, "D3DKMTOpenNativeFenceFromNtHandle"},
    {235, "D3DKMTIsFeatureEnabled"},
};
// clang-format on

[[nodiscard]] const char* name_for(u16 ordinal) {
    for (const auto& e : kD3dkmtOrdinals) {
        if (e.ordinal == ordinal) return e.name;
    }
    return nullptr;
}

void unimplemented_kmt(Marshaler& m, const char* api) {
    static std::unordered_set<std::string> logged;
    if (logged.insert(api).second) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED %s: no WDDM/dxgkrnl kernel graphics path\n",
            api);
    }
    m.set_return(STATUS_NOT_IMPLEMENTED);
}

void add_ordinal(DllExports& exports, u16 ordinal) {
    const char* name = name_for(ordinal);
    if (name == nullptr) return;
    const std::string tag = std::string("ext-ms-win-dx-d3dkmt-dxcore!") + name;
    // D3DKMT* take a single pointer argument (d3dkmthk.h); x86 stdcall pops 4.
    exports.entries.push_back({name, ordinal,
        [tag](Marshaler& m) { unimplemented_kmt(m, tag.c_str()); }, 4});
}

} // namespace

DllExports create_d3dkmt_dxcore_thunks(std::string_view contract_dll) {
    DllExports exports;
    if (contract_dll == "ext-ms-win-dx-d3dkmt-dxcore-l1-1-0.dll") {
        add_ordinal(exports, 2);
        add_ordinal(exports, 3);
        add_ordinal(exports, 4);
        add_ordinal(exports, 5);
        add_ordinal(exports, 6);
        add_ordinal(exports, 7);
        add_ordinal(exports, 9);
        add_ordinal(exports, 11);
        add_ordinal(exports, 12);
        add_ordinal(exports, 13);
        add_ordinal(exports, 14);
        add_ordinal(exports, 15);
        add_ordinal(exports, 16);
        add_ordinal(exports, 17);
        add_ordinal(exports, 18);
        add_ordinal(exports, 19);
        add_ordinal(exports, 20);
        add_ordinal(exports, 21);
        add_ordinal(exports, 22);
        add_ordinal(exports, 23);
        add_ordinal(exports, 24);
        add_ordinal(exports, 25);
        add_ordinal(exports, 26);
        add_ordinal(exports, 27);
        add_ordinal(exports, 30);
        add_ordinal(exports, 31);
        add_ordinal(exports, 32);
        add_ordinal(exports, 34);
        add_ordinal(exports, 36);
        add_ordinal(exports, 38);
        add_ordinal(exports, 49);
        add_ordinal(exports, 51);
        add_ordinal(exports, 57);
        add_ordinal(exports, 65);
        add_ordinal(exports, 69);
        add_ordinal(exports, 71);
        add_ordinal(exports, 72);
        add_ordinal(exports, 74);
        add_ordinal(exports, 76);
        add_ordinal(exports, 80);
        add_ordinal(exports, 100);
        add_ordinal(exports, 101);
        add_ordinal(exports, 102);
        add_ordinal(exports, 103);
        add_ordinal(exports, 104);
        add_ordinal(exports, 105);
        add_ordinal(exports, 106);
        add_ordinal(exports, 107);
        add_ordinal(exports, 108);
        add_ordinal(exports, 109);
        add_ordinal(exports, 110);
        add_ordinal(exports, 111);
        add_ordinal(exports, 112);
        add_ordinal(exports, 113);
        add_ordinal(exports, 114);
        add_ordinal(exports, 115);
        add_ordinal(exports, 116);
        add_ordinal(exports, 117);
        add_ordinal(exports, 119);
        add_ordinal(exports, 120);
        add_ordinal(exports, 121);
        add_ordinal(exports, 124);
        add_ordinal(exports, 126);
        add_ordinal(exports, 129);
        add_ordinal(exports, 130);
        add_ordinal(exports, 141);
        add_ordinal(exports, 142);
        add_ordinal(exports, 160);
        add_ordinal(exports, 161);
        add_ordinal(exports, 162);
        add_ordinal(exports, 163);
        add_ordinal(exports, 164);
        add_ordinal(exports, 169);
        add_ordinal(exports, 184);
        add_ordinal(exports, 185);
        add_ordinal(exports, 186);
        add_ordinal(exports, 187);
        add_ordinal(exports, 188);
        add_ordinal(exports, 189);
        add_ordinal(exports, 190);
        add_ordinal(exports, 193);
        add_ordinal(exports, 204);
        add_ordinal(exports, 205);
        add_ordinal(exports, 206);
        add_ordinal(exports, 207);
        add_ordinal(exports, 208);
        add_ordinal(exports, 209);
        add_ordinal(exports, 210);
        add_ordinal(exports, 212);
        add_ordinal(exports, 213);
        return exports;
    }
    if (contract_dll == "ext-ms-win-dx-d3dkmt-dxcore-l1-1-1.dll") {
        add_ordinal(exports, 220);
        add_ordinal(exports, 221);
        return exports;
    }
    if (contract_dll == "ext-ms-win-dx-d3dkmt-dxcore-l1-1-3.dll") {
        add_ordinal(exports, 227);
        return exports;
    }
    if (contract_dll == "ext-ms-win-dx-d3dkmt-dxcore-l1-1-4.dll") {
        add_ordinal(exports, 229);
        add_ordinal(exports, 230);
        return exports;
    }
    if (contract_dll == "ext-ms-win-dx-d3dkmt-dxcore-l1-1-5.dll") {
        add_ordinal(exports, 235);
        return exports;
    }
    return exports;
}

} // namespace vitreo::winapi
