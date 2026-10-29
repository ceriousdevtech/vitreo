// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: avrt, qwave, hid, setupapi, pdh, wevtapi, bthprops and the diagnostic DLLs

#include "dll_registry_internal.hpp"

#include "hardware/contracts/bluetooth_host.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/hid/hid.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "winapi/ui/hhctrl/hhctrl.hpp"
#include "winapi/system/ktmw32/ktmw32.hpp"
#include "winapi/system/wer/wer.hpp"
#include "winapi/system/advpack/advpack.hpp"
#include "winapi/system/ncrypt/ncrypt.hpp"
#include "winapi/system/setupapi/setupapi.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace vitreo::cli {

void register_mmcss_qos_probe_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    // -- avrt.dll (MMCSS). Vitreo does not change host scheduling priority;
    // the Windows-visible contract is the handle lifecycle callers use to
    // bracket multimedia work. Returning a process-local pseudo-handle lets
    // AvRevert / AvSetMmThreadPriority succeed without fabricating audio
    // deadlines.
    {
        winapi::DllExports av;
        static constexpr u32 kAvrtHandle = 0x41565254u; // 'AVRT'
        av.entries.push_back({"AvSetMmThreadCharacteristicsW", 0,
            [](winapi::Marshaler& m){
                (void)m.arg_ptr(0);
                GuestAddr index = m.arg_ptr(1);
                if (index) (void)m.write_u32(index, 0);
                VITREO_TRACE("[avrt] AvSetMmThreadCharacteristicsW -> 0x%x "
                             "(no host priority change)\n", kAvrtHandle);
                m.set_return(kAvrtHandle);
            }, 8});
        // The ANSI form is the same contract with an ANSI task name. A guest
        // SDK imports A, not W, and an unresolved import fails the whole DLL
        // load.
        av.entries.push_back({"AvSetMmThreadCharacteristicsA", 0,
            [](winapi::Marshaler& m){
                (void)m.arg_ptr(0);
                GuestAddr index = m.arg_ptr(1);
                if (index) (void)m.write_u32(index, 0);
                VITREO_TRACE("[avrt] AvSetMmThreadCharacteristicsA -> 0x%x "
                             "(no host priority change)\n", kAvrtHandle);
                m.set_return(kAvrtHandle);
            }, 8});
        av.entries.push_back({"AvRevertMmThreadCharacteristics", 0,
            [](winapi::Marshaler& m){
                u32 h = static_cast<u32>(m.arg(0));
                if (h != kAvrtHandle) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_HANDLE);
                    m.set_return(0);
                    return;
                }
                m.set_return(1);
            }, 4});
        av.entries.push_back({"AvSetMmThreadPriority", 0,
            [](winapi::Marshaler& m){
                u32 h = static_cast<u32>(m.arg(0));
                (void)m.arg(1);
                if (h != kAvrtHandle) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_HANDLE);
                    m.set_return(0);
                    return;
                }
                m.set_return(1);
            }, 8});
        (void)reg.register_dll("avrt.dll", std::move(av));
    }

    // -- qwave.dll (QWAVE / QoS). Quality-of-service is ADVISORY traffic
    // shaping: an application asks the stack to mark and pace a socket's
    // traffic, and Windows itself silently does nothing when the network path
    // does not honour DSCP. Vitreo has no QoS subsystem, so the honest
    // implementation is the part that is observable to the caller — the handle
    // and flow LIFECYCLE — with no shaping applied. Add/Remove pair up, a flow
    // id round-trips, and a stale handle is refused, which is what a caller can
    // actually test. A guest partner SDK imports all four.
    {
        winapi::DllExports qos;
        struct QosState {
            std::mutex mutex;
            std::unordered_set<u32> handles;
            std::unordered_map<u32, u32> flow_socket; // flow id -> socket
            u32 next_handle = 0x514F5301u;            // 'QOS' + serial
            u32 next_flow = 1;
        };
        static QosState qos_state;

        // BOOL QOSCreateHandle(PQOS_VERSION Version, PHANDLE QOSHandle)
        qos.entries.push_back({"QOSCreateHandle", 0,
            [](winapi::Marshaler& m){
                const GuestAddr version = m.arg_ptr(0);
                const GuestAddr out = m.arg_ptr(1);
                // QOS_VERSION is two USHORTs; only 1.0 exists and Windows
                // rejects anything else rather than negotiating down.
                const u32 major = version
                    ? m.memory().read_u16(version).value_or(0xFFFF) : 0xFFFF;
                const u32 minor = version
                    ? m.memory().read_u16(version + 2).value_or(0xFFFF) : 0xFFFF;
                if (version == 0 || out == 0 || major != 1 || minor != 0) {
                    winapi::set_last_error(
                        winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                u32 handle;
                {
                    std::lock_guard<std::mutex> lock(qos_state.mutex);
                    handle = qos_state.next_handle++;
                    qos_state.handles.insert(handle);
                }
                if (m.write_ptr(out, handle).is_err()) {
                    std::lock_guard<std::mutex> lock(qos_state.mutex);
                    qos_state.handles.erase(handle);
                    winapi::set_last_error(winapi::error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                VITREO_TRACE("[qwave] QOSCreateHandle -> 0x%x\n", handle);
                winapi::set_last_error(winapi::error_codes::ERROR_SUCCESS);
                m.set_return(1);
            }, 8});

        // BOOL QOSCloseHandle(HANDLE QOSHandle)
        qos.entries.push_back({"QOSCloseHandle", 0,
            [](winapi::Marshaler& m){
                const u32 handle = static_cast<u32>(m.arg(0));
                std::lock_guard<std::mutex> lock(qos_state.mutex);
                if (qos_state.handles.erase(handle) == 0) {
                    winapi::set_last_error(
                        winapi::error_codes::ERROR_INVALID_HANDLE);
                    m.set_return(0);
                    return;
                }
                VITREO_TRACE("[qwave] QOSCloseHandle(0x%x)\n", handle);
                winapi::set_last_error(winapi::error_codes::ERROR_SUCCESS);
                m.set_return(1);
            }, 4});

        // BOOL QOSAddSocketToFlow(HANDLE, SOCKET, PSOCKADDR, QOS_TRAFFIC_TYPE,
        //                        DWORD Flags, PQOS_FLOWID FlowId)
        qos.entries.push_back({"QOSAddSocketToFlow", 0,
            [](winapi::Marshaler& m){
                const u32 handle = static_cast<u32>(m.arg(0));
                const u32 socket = static_cast<u32>(m.arg(1));
                (void)m.arg_ptr(2);                      // destination address
                const u32 traffic = static_cast<u32>(m.arg(3));
                (void)m.arg(4);                          // flags
                const GuestAddr flow_out = m.arg_ptr(5);
                {
                    std::lock_guard<std::mutex> lock(qos_state.mutex);
                    if (!qos_state.handles.contains(handle)) {
                        winapi::set_last_error(
                            winapi::error_codes::ERROR_INVALID_HANDLE);
                        m.set_return(0);
                        return;
                    }
                }
                if (flow_out == 0) {
                    winapi::set_last_error(
                        winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                // FlowId is IN/OUT: zero asks for a new flow, non-zero joins an
                // existing one and must name a flow that exists.
                u32 flow = m.memory().read_u32(flow_out).value_or(0);
                {
                    std::lock_guard<std::mutex> lock(qos_state.mutex);
                    if (flow == 0) {
                        flow = qos_state.next_flow++;
                    } else if (!qos_state.flow_socket.contains(flow)) {
                        winapi::set_last_error(
                            winapi::error_codes::ERROR_INVALID_PARAMETER);
                        m.set_return(0);
                        return;
                    }
                    qos_state.flow_socket[flow] = socket;
                }
                if (m.write_u32(flow_out, flow).is_err()) {
                    winapi::set_last_error(winapi::error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                VITREO_TRACE("[qwave] QOSAddSocketToFlow(h=0x%x sock=0x%x "
                             "traffic=%u) -> flow=%u (no shaping applied)\n",
                             handle, socket, traffic, flow);
                winapi::set_last_error(winapi::error_codes::ERROR_SUCCESS);
                m.set_return(1);
            }, 24});

        // BOOL QOSRemoveSocketFromFlow(HANDLE, SOCKET, QOS_FLOWID, DWORD)
        qos.entries.push_back({"QOSRemoveSocketFromFlow", 0,
            [](winapi::Marshaler& m){
                const u32 handle = static_cast<u32>(m.arg(0));
                (void)m.arg(1);                          // socket
                const u32 flow = static_cast<u32>(m.arg(2));
                (void)m.arg(3);                          // flags
                std::lock_guard<std::mutex> lock(qos_state.mutex);
                if (!qos_state.handles.contains(handle)) {
                    winapi::set_last_error(
                        winapi::error_codes::ERROR_INVALID_HANDLE);
                    m.set_return(0);
                    return;
                }
                if (flow == 0 || qos_state.flow_socket.erase(flow) == 0) {
                    winapi::set_last_error(
                        winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                VITREO_TRACE("[qwave] QOSRemoveSocketFromFlow(flow=%u)\n", flow);
                winapi::set_last_error(winapi::error_codes::ERROR_SUCCESS);
                m.set_return(1);
            }, 16});
        (void)reg.register_dll("qwave.dll", std::move(qos));
    }

    // -- hid.dll -- (no HID devices on Vitreo; see winapi/system/hid/hid.hpp)
    {
        (void)reg.register_dll("hid.dll", winapi::create_hid_thunks(&ctx.handle_table));
    }

    // -- ktmw32.dll --
    {
        auto ktmw32_exports = winapi::create_ktmw32_thunks();
        (void)reg.register_dll("ktmw32.dll", std::move(ktmw32_exports));
    }

    // -- wer.dll -- (exclusion list on the capsule registry; see winapi/system/wer/wer.hpp)
    {
        (void)reg.register_dll("wer.dll", winapi::create_wer_thunks(ctx.registry));
        (void)reg.register_dll("faultrep.dll", winapi::create_faultrep_thunks(ctx.registry));
    }

    // -- advpack.dll -- (IsNTAdmin; see winapi/system/advpack/advpack.hpp)
    {
        (void)reg.register_dll("advpack.dll",
                               winapi::create_advpack_thunks(&ctx.elevation_state));
    }

    // -- ncrypt.dll --
    {
        // NCryptOpenKey lives in create_ncrypt_thunks() (ncrypt.cpp).
        auto ncrypt_exports = winapi::create_ncrypt_thunks();
        (void)reg.register_dll("ncrypt.dll", std::move(ncrypt_exports));
    }

    // -- hhctrl.ocx --
    {
        auto hhctrl_exports = winapi::create_hhctrl_thunks();
        (void)reg.register_dll("hhctrl.ocx", std::move(hhctrl_exports));
    }

    // Optional probe DLLs used by apps like CPU-Z.
    // Provide minimal exports so GetProcAddress succeeds and callers can handle
    // explicit failure codes instead of spinning on NULL function pointers.
    {
        // setupapi.dll and cfgmgr32.dll share one device tree
        // (winapi/system/setupapi/setupapi.hpp); setupapi.dll re-exports the
        // CM_* names the real DLL forwards to cfgmgr32.
        (void)reg.register_dll("setupapi.dll", winapi::create_setupapi_thunks(
            {}, &ctx.vfs, &ctx.reg_thunks, &ctx.elevation_state,
            &ctx.setupapi_services));
        (void)reg.register_dll("cfgmgr32.dll", winapi::create_cfgmgr32_thunks(
            {}, &ctx.reg_thunks, &ctx.elevation_state));
    }
}

void register_perf_event_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    {
        // pdh.dll. The guest delay-loads PdhOpenQueryW, and an overlay
        // process builds counter queries. Measured
        // (tools/oracle_probe/s7_pdh_probe.expected):
        //  - PdhOpenQuery hands out a real query handle (ERROR_SUCCESS); a NULL
        //    out-pointer is PDH_INVALID_ARGUMENT.
        //  - Every call with an unknown/closed query or counter handle
        //    (including NULL) is PDH_INVALID_HANDLE.
        //  - A path not of the form \[\\machine]\object[(instance)]\counter is
        //    PDH_CSTATUS_BAD_COUNTERNAME; an absent performance object is
        //    PDH_CSTATUS_NO_OBJECT with *phCounter NULL.
        //  - PdhCollectQueryData on a query with no counters is PDH_NO_DATA.
        // Vitreo publishes no performance-counter objects (the state of a
        // Windows install whose counters are unavailable), so no counter
        // handle can ever exist.
        struct PdhQueries {
            std::mutex mutex;
            std::unordered_set<u64> open;
            u64 next{0x5D40};
        };
        static PdhQueries pdh_queries;
        constexpr u32 PDH_CSTATUS_NO_OBJECT = 0xC0000BB8u;
        constexpr u32 PDH_INVALID_HANDLE = 0xC0000BBCu;
        constexpr u32 PDH_INVALID_ARGUMENT = 0xC0000BBDu;
        constexpr u32 PDH_CSTATUS_BAD_COUNTERNAME = 0xC0000BC0u;
        constexpr u32 PDH_NO_DATA = 0x800007D5u;
        const auto pdh_query_open = [](u64 query) {
            std::lock_guard lock(pdh_queries.mutex);
            return query != 0 && pdh_queries.open.contains(query);
        };
        winapi::DllExports pdh;
        auto pdh_open_query = [](winapi::Marshaler& m) {
            const GuestAddr ph = m.arg_ptr(2);
            if (ph == 0) {
                m.set_return(PDH_INVALID_ARGUMENT);
                return;
            }
            u64 query = 0;
            {
                std::lock_guard lock(pdh_queries.mutex);
                query = pdh_queries.next;
                pdh_queries.next += 4;
                pdh_queries.open.insert(query);
            }
            (void)m.write_ptr(ph, query);
            m.set_return(0u);
        };
        pdh.entries.push_back({"PdhOpenQueryA", 0, pdh_open_query, 12});
        pdh.entries.push_back({"PdhOpenQueryW", 0, pdh_open_query, 12});
        pdh.entries.push_back({"PdhCloseQuery", 0,
            [](winapi::Marshaler& m) {
                std::lock_guard lock(pdh_queries.mutex);
                m.set_return(pdh_queries.open.erase(m.arg(0)) != 0 ? 0u : PDH_INVALID_HANDLE);
            }, 4});
        const auto pdh_add_counter = [pdh_query_open](winapi::Marshaler& m, bool wide) {
            const u64 query = m.arg(0);
            const GuestAddr path_ptr = m.arg_ptr(1);
            const GuestAddr ph = m.arg_ptr(3);
            if (ph != 0) (void)m.write_ptr(ph, 0);
            if (!pdh_query_open(query)) {
                m.set_return(PDH_INVALID_HANDLE);
                return;
            }
            std::string path;
            if (path_ptr != 0) {
                path = wide ? m.read_wide_string(path_ptr, 4096).value_or("")
                            : m.read_ansi_string(path_ptr, 4096).value_or("");
            }
            // \[\\machine]\object[(parent/instance#index)]\counter
            usize at = 0;
            bool valid = path.size() >= 2 && path[0] == '\\';
            if (valid && path[1] == '\\') {
                const auto machine_end = path.find('\\', 2);
                valid = machine_end != std::string::npos && machine_end > 2;
                at = valid ? machine_end : 0;
            }
            if (valid) {
                const auto counter_sep = path.find('\\', at + 1);
                valid = counter_sep != std::string::npos && counter_sep > at + 1 &&
                        counter_sep + 1 < path.size();
            }
            if (!valid) {
                m.set_return(PDH_CSTATUS_BAD_COUNTERNAME);
                return;
            }
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED pdh!PdhAddCounter: "
                                     "no performance-counter objects are published\n");
            });
            m.set_return(PDH_CSTATUS_NO_OBJECT);
        };
        pdh.entries.push_back({"PdhAddCounterA", 0,
            [pdh_add_counter](winapi::Marshaler& m) { pdh_add_counter(m, false); }, 16});
        pdh.entries.push_back({"PdhAddCounterW", 0,
            [pdh_add_counter](winapi::Marshaler& m) { pdh_add_counter(m, true); }, 16});
        pdh.entries.push_back({"PdhAddEnglishCounterA", 0,
            [pdh_add_counter](winapi::Marshaler& m) { pdh_add_counter(m, false); }, 16});
        pdh.entries.push_back({"PdhAddEnglishCounterW", 0,
            [pdh_add_counter](winapi::Marshaler& m) { pdh_add_counter(m, true); }, 16});
        pdh.entries.push_back({"PdhCollectQueryData", 0,
            [pdh_query_open](winapi::Marshaler& m) {
                // Open queries never hold counters here (see above).
                m.set_return(pdh_query_open(m.arg(0)) ? PDH_NO_DATA : PDH_INVALID_HANDLE);
            }, 4});
        // No counter handle can exist, so every counter-handle call is
        // PDH_INVALID_HANDLE (measured for NULL and unknown handles).
        pdh.entries.push_back({"PdhGetFormattedCounterValue", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_INVALID_HANDLE); }, 16});
        // PDH_STATUS PdhGetFormattedCounterArrayA(PDH_HCOUNTER, DWORD,
        //   LPDWORD lpdwBufferSize, LPDWORD lpdwItemCount,
        //   PPDH_FMT_COUNTERVALUE_ITEM_A): x86 20. Measured: sizes untouched.
        pdh.entries.push_back({"PdhGetFormattedCounterArrayA", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_INVALID_HANDLE); }, 20});
        pdh.entries.push_back({"PdhGetFormattedCounterArrayW", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_INVALID_HANDLE); }, 20});
        pdh.entries.push_back({"PdhGetRawCounterValue", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_INVALID_HANDLE); }, 12});
        // Object/machine enumeration: no objects are published.
        // x86 sizes from libpdh.a: PdhEnumObjectItemsA@36, PdhEnumMachinesA@12.
        pdh.entries.push_back({"PdhEnumObjectItemsA", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_CSTATUS_NO_OBJECT); }, 36});
        pdh.entries.push_back({"PdhEnumMachinesA", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_CSTATUS_NO_OBJECT); }, 12});
        pdh.entries.push_back({"PdhEnumObjectsA", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_CSTATUS_NO_OBJECT); }, 24});
        pdh.entries.push_back({"PdhConnectMachineA", 0,
            [](winapi::Marshaler& m) {
                if (m.arg(0) == 0) { winapi::set_last_error(winapi::error_codes::ERROR_FILE_NOT_FOUND); m.set_return(0u); return; }
                m.set_return(PDH_CSTATUS_NO_OBJECT);
            }, 4});
        pdh.entries.push_back({"PdhLookupPerfNameByIndexA", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_CSTATUS_NO_OBJECT); }, 16});
        pdh.entries.push_back({"PdhLookupPerfIndexByNameA", 0,
            [](winapi::Marshaler& m) { m.set_return(PDH_CSTATUS_NO_OBJECT); }, 12});
        (void)reg.register_dll("pdh.dll", std::move(pdh));

        // wevtapi.dll
        winapi::DllExports wevtapi;
        // The capsule has no Windows event log. Measured
        // (tools/oracle_probe/s7_misc_probe.expected): a channel that does not
        // exist is ERROR_EVT_CHANNEL_NOT_FOUND, a missing .evtx file the file
        // system error, and every call on a handle that was never issued
        // (NULL included) is ERROR_INVALID_HANDLE with outputs untouched.
        wevtapi.entries.push_back({"EvtQuery", 0,
            [](winapi::Marshaler& m) {
                constexpr u32 EvtQueryFilePath = 0x2;
                constexpr u32 ERROR_EVT_CHANNEL_NOT_FOUND = 15007;
                const u32 flags = m.arg32(3);
                winapi::set_last_error((flags & EvtQueryFilePath) != 0
                                           ? winapi::error_codes::ERROR_PATH_NOT_FOUND
                                           : ERROR_EVT_CHANNEL_NOT_FOUND);
                m.set_return(0u);
            }, 16});
        const auto evt_invalid_handle = [](winapi::Marshaler& m) {
            winapi::set_last_error(winapi::error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0u);
        };
        wevtapi.entries.push_back({"EvtClose", 0, evt_invalid_handle, 4});
        wevtapi.entries.push_back({"EvtNext", 0, evt_invalid_handle, 24});
        // BOOL EvtRender(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, PVOID, PDWORD,
        //                PDWORD): x86 stdcall 28 (libwevtapi.a _EvtRender@28).
        wevtapi.entries.push_back({"EvtRender", 0, evt_invalid_handle, 28});
        {
            // Audit completion: no event log channel to render from.
            winapi::DllExports audit;
            audit.entries.push_back({"EvtCreateRenderContext", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(1);
                    (void)m.arg(2);
                    winapi::set_last_error(50u);
                    m.set_return(0u);
                }, 12});
            merge_exports(wevtapi, std::move(audit));
        }
        (void)reg.register_dll("wevtapi.dll", std::move(wevtapi));
    }
}

void register_device_diagnostic_dlls(RuntimeContext& ctx, GuestMemory& memory) {
    auto& reg = ctx.thunk_registry;

    // -- bthprops.cpl (the classic Bluetooth API) and winusb.dll --
    // winapi/system/hid/hid.hpp. The radio is the host controller behind the
    // BluetoothHost contract; BluetoothSdpEnumAttributes calls back into the
    // guest on the calling thread.
    {
        static std::unique_ptr<vitreo::hardware::BluetoothHost> bth_host =
            vitreo::hardware::make_platform_bluetooth_host();
        winapi::GuestFunctionCaller guest_caller =
            [&ctx, &memory](GuestAddr function, const u64* args, usize count) -> u64 {
                cpu::CpuState* cpu = active_guest_cpu();
                if (cpu == nullptr) return 0;
                return run_guest_callback(*cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                                          function, args, count, cpu->is_64bit());
            };
        (void)reg.register_dll("bthprops.cpl", winapi::create_bthprops_thunks(
            bth_host.get(), &ctx.handle_table, std::move(guest_caller)));
        (void)reg.register_dll("winusb.dll", winapi::create_winusb_thunks());
    }

    // -- ndfapi.dll --
    // Network diagnostics would launch the Windows troubleshooter UI.
    {
        constexpr u32 E_NOTIMPL_CODE = 0x80004001u;
        winapi::DllExports ndf;
        // No Network Diagnostics Framework on the host: creating or running an
        // incident fails where it is called.
        ndf.entries.push_back({"NdfCreateWebIncident", 0,
            [](winapi::Marshaler& m) {
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ndfapi!NdfCreateWebIncident: "
                                         "no Network Diagnostics Framework\n");
                });
                (void)m.arg_ptr(0);
                if (const GuestAddr out = m.arg_ptr(1); out != 0)
                    (void)m.write_ptr(out, 0);
                m.set_return(E_NOTIMPL_CODE);
            }, 8});
        ndf.entries.push_back({"NdfExecuteDiagnosis", 0,
            [](winapi::Marshaler& m) {
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ndfapi!NdfExecuteDiagnosis: "
                                         "no Network Diagnostics Framework\n");
                });
                (void)m.arg(0);
                (void)m.arg(1);
                m.set_return(E_NOTIMPL_CODE);
            }, 8});
        ndf.entries.push_back({"NdfCloseIncident", 0,
            [](winapi::Marshaler& m) { (void)m.arg(0); m.set_return(0u); }, 4});
        (void)reg.register_dll("ndfapi.dll", std::move(ndf));
    }

    // -- fontsub.dll --
    // Font subsetting is a real transformation; refusing is safer than
    // emitting a font file that a printer or PDF writer would then embed.
    {
        winapi::DllExports fontsub;
        fontsub.entries.push_back({"CreateFontPackage", 0,
            [](winapi::Marshaler& m) {
                for (u32 i = 0; i < 11; ++i) (void)m.arg(i);
                constexpr u32 ERR_GENERIC = 1000u;
                m.set_return(ERR_GENERIC);
            }, 44});
        (void)reg.register_dll("fontsub.dll", std::move(fontsub));
    }
}

} // namespace vitreo::cli
