#pragma once

/// @file inproc_servers.hpp
/// @brief LoadLibrary-visible in-proc DLLs whose COM/codec engines are absent.
///
/// Each DLL registers the exports Windows loaders resolve. Unimplemented
/// factories and compile/capture APIs fail with documented HRESULTs
/// (CLASS_E_CLASSNOTAVAILABLE, E_NOTIMPL) rather than fabricated objects.

#include "winapi/core/thunk_registry.hpp"

#include <functional>

namespace vitreo::winapi {

DllExports create_quartz_thunks();
DllExports create_devenum_thunks();
DllExports create_dxdiagn_thunks();
DllExports create_d3dxof_thunks();
DllExports create_d3dcompiler_47_thunks();
DllExports create_msacm32_thunks();
DllExports create_avicap32_thunks();
/// CoTaskMemAlloc / CoTaskMemFree, injected so this layer (below COM) can hand
/// out task memory the guest frees with CoTaskMemFree.
struct TaskMemoryFns {
    std::function<GuestAddr(u64 size)> allocate;
    std::function<void(GuestAddr address)> free;
};

/// msdmo.dll: the DMO_MEDIA_TYPE helpers (Mo*MediaType) are implemented in
/// full; DMOEnum/registration have no DMO registry behind them.
DllExports create_msdmo_thunks(TaskMemoryFns task_memory);
DllExports create_wbemprox_thunks();
DllExports create_wbemdisp_thunks();
DllExports create_mmdevapi_thunks(std::function<u32(Marshaler&)> activate_async = {});

} // namespace vitreo::winapi
