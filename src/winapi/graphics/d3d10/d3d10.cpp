// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10.cpp
/// @brief d3d10.dll, d3d10_1.dll, d3d10core.dll and d3d10_1core.dll: the
/// module exports.
///
/// Names, ordinals and x86 stdcall cleanup come from the SDK import libraries
/// (d3d10.lib / d3d10_1.lib, dumpbin /headers) and the real DLLs' export
/// tables on Windows (dumpbin /exports, x86 and x64 agree); the core DLLs'
/// cleanup from their x86 disassembly (`ret N`). The device, object, blob and
/// state-block entry points run in D3D11Manager (d3d10_device.cpp); what
/// needs no device state is here.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace vitreo::winapi {
namespace {

constexpr u32 kSOk = 0x00000000u;
constexpr u32 kENotImpl = 0x80004001u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kStatusSuccess = 0x00000000u;
constexpr u32 kStatusUnsuccessful = 0xC0000001u;
constexpr u32 kStatusNotImplemented = 0xC0000002u;
constexpr u32 kStatusInvalidParameter = 0xC000000Du;

/// One `[vitreo] UNIMPLEMENTED` line per entry point.
void unimplemented_once(const char* dll, const char* api, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::scoped_lock lock(mutex);
    if (!reported.insert(api).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s!%s: %s\n", dll, api,
                 what);
}

/// A fail-loudly entry point: NULL every COM out-parameter (at the argument
/// indices given) and return E_NOTIMPL.
ApiHandler fail_loudly(
    const char* dll, const char* api, const char* what,
    std::initializer_list<usize> outputs) {
    std::vector<usize> out(outputs);
    return [dll, api, what, out](Marshaler& m) {
        for (const usize index : out) {
            const GuestAddr address = m.arg_ptr(index);
            if (address != 0) (void)m.write_ptr(address, 0);
        }
        unimplemented_once(dll, api, what);
        m.set_return(kENotImpl);
    };
}

// -- D3D10StateBlockMask* (d3d10effect.h) -------------------------------------
//
// Pure functions of a D3D10_STATE_BLOCK_MASK, measured on Windows
// (d3d10_contract_probe [sb]): every NULL mask is E_INVALIDARG; Enable/
// DisableCapture take a state type 1..24 and an entry range inside it (count
// 0, or one past the type's last entry, is E_INVALIDARG and changes
// nothing); GetSetting answers the raw masked bit (0x80 for entry 7) and 0
// for anything invalid; Difference is XOR, Intersect AND, Union OR.

[[nodiscard]] bool read_mask(GuestMemory& memory, GuestAddr address,
                             std::array<u8, d3d10::STATE_BLOCK_MASK_BYTES>&
                                 mask) {
    return address != 0 &&
           memory.read_bytes(address, mask.data(), mask.size()).is_ok();
}

void mask_all(Marshaler& m, u8 value) {
    const GuestAddr mask = m.arg_ptr(0);
    if (mask == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    std::array<u8, d3d10::STATE_BLOCK_MASK_BYTES> bytes{};
    bytes.fill(value);
    m.set_return(m.memory().write_bytes(mask, bytes.data(), bytes.size())
                         .is_ok()
                     ? kSOk
                     : kEInvalidArg);
}

void mask_capture(Marshaler& m, bool enable) {
    const GuestAddr mask = m.arg_ptr(0);
    const u32 type = m.arg32(1);
    const u32 start = m.arg32(2);
    const u32 count = m.arg32(3);
    std::array<u8, d3d10::STATE_BLOCK_MASK_BYTES> bytes{};
    if (!read_mask(m.memory(), mask, bytes) || type == 0 ||
        type >= d3d10::kStateMaskRanges.size() || count == 0 ||
        start >= d3d10::kStateMaskRanges[type].entries ||
        count > d3d10::kStateMaskRanges[type].entries - start) {
        m.set_return(kEInvalidArg);
        return;
    }
    const d3d10::StateMaskRange range = d3d10::kStateMaskRanges[type];
    for (u32 index = start; index < start + count; ++index) {
        u8& byte = bytes[range.offset + index / 8];
        const u8 bit = static_cast<u8>(1u << (index % 8));
        byte = enable ? static_cast<u8>(byte | bit)
                      : static_cast<u8>(byte & ~bit);
    }
    m.set_return(
        m.memory().write_bytes(mask, bytes.data(), bytes.size()).is_ok()
            ? kSOk
            : kEInvalidArg);
}

void mask_get_setting(Marshaler& m) {
    std::array<u8, d3d10::STATE_BLOCK_MASK_BYTES> bytes{};
    const u32 type = m.arg32(1);
    const u32 index = m.arg32(2);
    if (!read_mask(m.memory(), m.arg_ptr(0), bytes) || type == 0 ||
        type >= d3d10::kStateMaskRanges.size() ||
        index >= d3d10::kStateMaskRanges[type].entries) {
        m.set_return(0);
        return;
    }
    const d3d10::StateMaskRange range = d3d10::kStateMaskRanges[type];
    m.set_return(bytes[range.offset + index / 8] & (1u << (index % 8)));
}

enum class MaskOp { Difference, Intersect, Union };

void mask_binary(Marshaler& m, MaskOp op) {
    std::array<u8, d3d10::STATE_BLOCK_MASK_BYTES> x{}, y{};
    const GuestAddr result = m.arg_ptr(2);
    if (!read_mask(m.memory(), m.arg_ptr(0), x) ||
        !read_mask(m.memory(), m.arg_ptr(1), y) || result == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    for (usize i = 0; i < x.size(); ++i) {
        switch (op) {
        case MaskOp::Difference: x[i] = static_cast<u8>(x[i] ^ y[i]); break;
        case MaskOp::Intersect: x[i] = static_cast<u8>(x[i] & y[i]); break;
        case MaskOp::Union: x[i] = static_cast<u8>(x[i] | y[i]); break;
        }
    }
    m.set_return(m.memory().write_bytes(result, x.data(), x.size()).is_ok()
                     ? kSOk
                     : kEInvalidArg);
}

/// The exports d3d10.dll and d3d10_1.dll share, at their ordinals (both
/// modules number the same 29 names alphabetically; ordinals 4 and 5 are
/// D3D10CreateDevice(AndSwapChain) in d3d10.dll and the `1` pair in
/// d3d10_1.dll).
DllExports common_exports(D3D11Manager& manager, const char* dll,
                          bool d3d10_1) {
    DllExports exports;
    const auto add = [&](const char* name, u16 ordinal, u16 stdcall_bytes,
                         ApiHandler handler) {
        exports.entries.push_back(
            {name, ordinal, std::move(handler), stdcall_bytes});
    };
    const auto device = [&manager](void (D3D11Manager::*method)(Marshaler&)) {
        return [&manager, method](Marshaler& m) { (manager.*method)(m); };
    };
    add("D3D10CompileEffectFromMemory", 1, 36,
        fail_loudly(dll, "D3D10CompileEffectFromMemory",
                    "the Effects compiler is not provided", {7, 8}));
    add("D3D10CompileShader", 2, 40,
        fail_loudly(dll, "D3D10CompileShader",
                    "the HLSL compiler is not provided", {8, 9}));
    add("D3D10CreateBlob", 3, 8, device(&D3D11Manager::impl_D3D10CreateBlob));
    if (d3d10_1) {
        add("D3D10CreateDevice1", 4, 28,
            device(&D3D11Manager::impl_D3D10CreateDevice1));
        add("D3D10CreateDeviceAndSwapChain1", 5, 36,
            device(&D3D11Manager::impl_D3D10CreateDeviceAndSwapChain1));
    } else {
        add("D3D10CreateDevice", 4, 24,
            device(&D3D11Manager::impl_D3D10CreateDevice));
        add("D3D10CreateDeviceAndSwapChain", 5, 32,
            device(&D3D11Manager::impl_D3D10CreateDeviceAndSwapChain));
    }
    add("D3D10CreateEffectFromMemory", 6, 24,
        fail_loudly(dll, "D3D10CreateEffectFromMemory",
                    "the Effects framework is not provided", {5}));
    add("D3D10CreateEffectPoolFromMemory", 7, 20,
        fail_loudly(dll, "D3D10CreateEffectPoolFromMemory",
                    "the Effects framework is not provided", {4}));
    add("D3D10CreateStateBlock", 8, 12,
        device(&D3D11Manager::impl_D3D10CreateStateBlock));
    add("D3D10DisassembleEffect", 9, 12,
        fail_loudly(dll, "D3D10DisassembleEffect",
                    "the Effects framework is not provided", {2}));
    add("D3D10DisassembleShader", 10, 20,
        fail_loudly(dll, "D3D10DisassembleShader",
                    "the shader disassembler is not provided", {4}));
    add("D3D10GetGeometryShaderProfile", 11, 4,
        [&manager](Marshaler& m) { manager.impl_D3D10GetShaderProfile(m, 'g'); });
    add("D3D10GetInputAndOutputSignatureBlob", 12, 12,
        [&manager](Marshaler& m) {
            manager.impl_D3D10GetSignatureBlob(m, true, true);
        });
    add("D3D10GetInputSignatureBlob", 13, 12, [&manager](Marshaler& m) {
        manager.impl_D3D10GetSignatureBlob(m, true, false);
    });
    add("D3D10GetOutputSignatureBlob", 14, 12, [&manager](Marshaler& m) {
        manager.impl_D3D10GetSignatureBlob(m, false, true);
    });
    add("D3D10GetPixelShaderProfile", 15, 4,
        [&manager](Marshaler& m) { manager.impl_D3D10GetShaderProfile(m, 'p'); });
    add("D3D10GetShaderDebugInfo", 16, 12,
        fail_loudly(dll, "D3D10GetShaderDebugInfo",
                    "shader debug information is not provided", {2}));
    // d3d10.dll's export jumps to d3d10core!D3D10CoreGetVersion: a 64-bit
    // result.
    add("D3D10GetVersion", 17, 0,
        [](Marshaler& m) { m.set_return_i64(d3d10::RUNTIME_VERSION); });
    add("D3D10GetVertexShaderProfile", 18, 4,
        [&manager](Marshaler& m) { manager.impl_D3D10GetShaderProfile(m, 'v'); });
    add("D3D10PreprocessShader", 19, 28,
        fail_loudly(dll, "D3D10PreprocessShader",
                    "the HLSL preprocessor is not provided", {5, 6}));
    add("D3D10ReflectShader", 20, 12,
        fail_loudly(dll, "D3D10ReflectShader",
                    "shader reflection is not provided", {2}));
    // Measured: E_NOTIMPL on Windows too (the export jumps to
    // d3d10core!D3D10CoreRegisterLayers, "mov eax,80004001h; ret").
    add("D3D10RegisterLayers", 21, 0,
        [](Marshaler& m) { m.set_return(kENotImpl); });
    add("D3D10StateBlockMaskDifference", 22, 12,
        [](Marshaler& m) { mask_binary(m, MaskOp::Difference); });
    add("D3D10StateBlockMaskDisableAll", 23, 4,
        [](Marshaler& m) { mask_all(m, 0x00); });
    add("D3D10StateBlockMaskDisableCapture", 24, 16,
        [](Marshaler& m) { mask_capture(m, false); });
    add("D3D10StateBlockMaskEnableAll", 25, 4,
        [](Marshaler& m) { mask_all(m, 0xFF); });
    add("D3D10StateBlockMaskEnableCapture", 26, 16,
        [](Marshaler& m) { mask_capture(m, true); });
    add("D3D10StateBlockMaskGetSetting", 27, 12,
        [](Marshaler& m) { mask_get_setting(m); });
    add("D3D10StateBlockMaskIntersect", 28, 12,
        [](Marshaler& m) { mask_binary(m, MaskOp::Intersect); });
    add("D3D10StateBlockMaskUnion", 29, 12,
        [](Marshaler& m) { mask_binary(m, MaskOp::Union); });
    // One device lock for every entry, as d3d11.dll's: the device entry
    // points touch D3D11Manager.
    for (ExportEntry& entry : exports.entries) {
        entry.handler = [&manager, inner = std::move(entry.handler)](
                            Marshaler& m) {
            std::scoped_lock lock(manager.api_mutex());
            ++manager.api_lock_depth();
            struct DepthGuard {
                D3D11Manager& manager;
                ~DepthGuard() { --manager.api_lock_depth(); }
            } depth_guard{manager};
            inner(m);
        };
    }
    return exports;
}

} // namespace

namespace d3d10 {

bool d3d10_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D10");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

} // namespace d3d10

DllExports create_d3d10_thunks(D3D11Manager& manager) {
    return common_exports(manager, "d3d10", false);
}

DllExports create_d3d10_1_thunks(D3D11Manager& manager) {
    return common_exports(manager, "d3d10_1", true);
}

DllExports create_d3d10core_thunks(bool d3d10_1) {
    // d3d10core.dll / d3d10_1core.dll on Windows 8+ are thunks: D3D10Core*
    // answer E_NOTIMPL, D3D10CoreGetVersion the runtime version, and the
    // D3DKMT* exports are the stubs below (x86 disassembly on Windows; the
    // x64 build answers the same values, d3d10_contract_probe notes).
    DllExports exports;
    const char* dll = d3d10_1 ? "d3d10_1core" : "d3d10core";
    const auto add = [&](const char* name, u16 ordinal, u16 stdcall_bytes,
                         ApiHandler handler) {
        exports.entries.push_back(
            {name, ordinal, std::move(handler), stdcall_bytes});
    };
    const auto returns = [](u32 value) {
        return [value](Marshaler& m) { m.set_return(value); };
    };
    if (d3d10_1) {
        add("D3D10CoreCreateDevice1", 14, 28, returns(kENotImpl));
    } else {
        add("D3D10CoreCreateDevice", 14, 20, returns(kENotImpl));
    }
    add("D3D10CoreGetSupportedVersions", 15, 12, returns(kENotImpl));
    add("D3D10CoreGetVersion", 16, 0,
        [](Marshaler& m) { m.set_return_i64(d3d10::RUNTIME_VERSION); });
    add("D3D10CoreRegisterLayers", 17, 0, returns(kENotImpl));
    // The shared stub: "xor eax,eax; ret 4".
    struct Stub {
        const char* name;
        u16 ordinal;
    };
    for (const Stub stub : {
             Stub{"D3DKMTCloseAdapter", 1},
             Stub{"D3DKMTCreateAllocation", 18},
             Stub{"D3DKMTCreateContext", 19},
             Stub{"D3DKMTCreateDevice", 20},
             Stub{"D3DKMTCreateSynchronizationObject", 21},
             Stub{"D3DKMTDestroyAllocation", 2},
             Stub{"D3DKMTDestroyContext", 3},
             Stub{"D3DKMTDestroyDevice", 4},
             Stub{"D3DKMTDestroySynchronizationObject", 5},
             Stub{"D3DKMTEscape", 22},
             Stub{"D3DKMTGetContextSchedulingPriority", 23},
             Stub{"D3DKMTGetDisplayModeList", 25},
             Stub{"D3DKMTGetMultisampleMethodList", 26},
             Stub{"D3DKMTGetRuntimeData", 27},
             Stub{"D3DKMTGetSharedPrimaryHandle", 28},
             Stub{"D3DKMTLock", 29},
             Stub{"D3DKMTPresent", 6},
             Stub{"D3DKMTQueryAllocationResidency", 32},
             Stub{"D3DKMTRender", 34},
             Stub{"D3DKMTSetAllocationPriority", 35},
             Stub{"D3DKMTSetContextSchedulingPriority", 36},
             Stub{"D3DKMTSetDisplayMode", 37},
             Stub{"D3DKMTSetDisplayPrivateDriverFormat", 8},
             Stub{"D3DKMTSetGammaRamp", 38},
             Stub{"D3DKMTSetVidPnSourceOwner", 39},
             Stub{"D3DKMTSignalSynchronizationObject", 9},
             Stub{"D3DKMTUnlock", 10},
             Stub{"D3DKMTWaitForSynchronizationObject", 11},
             Stub{"D3DKMTWaitForVerticalBlankEvent", 40}}) {
        add(stub.name, stub.ordinal, 4, returns(kStatusSuccess));
    }
    // D3DKMTGetDeviceState(D3DKMT_GETDEVICESTATE*): StateType (+4) 1
    // (EXECUTION) writes D3DKMT_DEVICEEXECUTION_ACTIVE at +8; 2 (PRESENT)
    // zeroes the 40-byte present state at +8; anything else
    // STATUS_UNSUCCESSFUL.
    add("D3DKMTGetDeviceState", 24, 4, [](Marshaler& m) {
        const GuestAddr state = m.arg_ptr(0);
        const auto type = m.read_u32(state + 4);
        if (state == 0 || type.is_err()) {
            m.set_return(kStatusInvalidParameter);
            return;
        }
        if (type.value() == 1) {
            (void)m.write_u32(state + 8, 1);
            m.set_return(kStatusSuccess);
        } else if (type.value() == 2) {
            const std::array<u8, 40> zero{};
            (void)m.memory().write_bytes(state + 8, zero.data(), zero.size());
            m.set_return(kStatusSuccess);
        } else {
            m.set_return(kStatusUnsuccessful);
        }
    });
    // D3DKMTOpenAdapterFromHdc(D3DKMT_OPENADAPTERFROMHDC*): hAdapter =
    // 0xF0F and a fresh LUID (AllocateLocallyUniqueId), STATUS_SUCCESS. The
    // fields follow the HDC, so they sit 4 bytes later on x64.
    add("D3DKMTOpenAdapterFromHdc", 30, 4, [](Marshaler& m) {
        static std::atomic<u32> next_luid{0x1000};
        const GuestAddr open = m.arg_ptr(0);
        if (open == 0) {
            m.set_return(kStatusInvalidParameter);
            return;
        }
        const GuestAddr adapter = open + (m.cpu_is_64bit() ? 8u : 4u);
        (void)m.write_u32(adapter, 0xF0Fu);
        (void)m.write_u32(adapter + 4, next_luid.fetch_add(1));
        (void)m.write_u32(adapter + 8, 0);
        m.set_return(kStatusSuccess);
    });
    add("D3DKMTOpenResource", 31, 4, returns(kStatusInvalidParameter));
    add("D3DKMTQueryResourceInfo", 33, 4, returns(kStatusInvalidParameter));
    // D3DKMTQueryAdapterInfo(D3DKMT_QUERYADAPTERINFO*): type 1
    // (KMTQAITYPE_UMDRIVERNAME) answers the module's own file name, which
    // this runtime has no guest path for; type 11 writes 0 to the private
    // data; every other type succeeds without writing.
    add("D3DKMTQueryAdapterInfo", 7, 4, [dll](Marshaler& m) {
        const GuestAddr query = m.arg_ptr(0);
        const auto type = m.read_u32(query + 4);
        if (query == 0 || type.is_err()) {
            m.set_return(kStatusInvalidParameter);
            return;
        }
        const auto data = m.read_ptr(query + 8);
        if (type.value() == 1) {
            unimplemented_once(dll, "D3DKMTQueryAdapterInfo",
                               "KMTQAITYPE_UMDRIVERNAME");
            m.set_return(kStatusNotImplemented);
            return;
        }
        if (type.value() == 11 && data.is_ok() && data.value() != 0) {
            (void)m.write_u32(static_cast<GuestAddr>(data.value()), 0);
        }
        m.set_return(kStatusSuccess);
    });
    // OpenAdapter10 / OpenAdapter10_2 forward to d3d11.dll's exports of the
    // same names, which this runtime's d3d11.dll does not provide; the thunk
    // then answers E_NOTIMPL itself.
    add("OpenAdapter10", 12, 4, returns(kENotImpl));
    add("OpenAdapter10_2", 13, 4, returns(kENotImpl));
    return exports;
}

} // namespace vitreo::winapi
