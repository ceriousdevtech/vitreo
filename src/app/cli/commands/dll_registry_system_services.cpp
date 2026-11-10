// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: bcrypt, winmm, winhttp, wininet, msi, opengl32 and the printing / network / power DLLs

#include "dll_registry_internal.hpp"
#include "winapi/system/activeds/activeds.hpp"
#include "winapi/system/mscms/mscms.hpp"

#include "winapi/kernel32/error.hpp"
#include "winapi/graphics/gdi32/pixel_format_api.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/backend_swap_chain.hpp"
#include "winapi/system/bcrypt/bcrypt.hpp"
#include "winapi/networking/wsock32/wsock32.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include "winapi/networking/winhttp/winhttp.hpp"
#include "winapi/networking/wininet/wininet.hpp"
#include "winapi/graphics/msimg32/msimg32.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_search.hpp"
#include "winapi/system/msi/msi_product.hpp"
#include "winapi/media/inproc_servers/inproc_servers.hpp"
#include "winapi/system/winspool/winspool.hpp"
#include "winapi/system/wtsapi32/wtsapi32.hpp"
#include "winapi/ui/imm32/imm32.hpp"
#include "winapi/networking/iphlpapi/iphlpapi.hpp"
#include "winapi/networking/tapi32/tapi32.hpp"
#include "winapi/networking/dnsapi/dnsapi.hpp"
#include "winapi/networking/wldap32/wldap32.hpp"
#include "winapi/system/powrprof/powrprof.hpp"
#include "winapi/networking/secur32/secur32.hpp"

#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::cli {

void register_system_services_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;
    (void)reg.register_dll("activeds.dll", winapi::create_activeds_thunks());

    // Process-owned ICC profiles and transforms; file access remains in capsule.
    (void)reg.register_dll("mscms.dll", winapi::create_mscms_thunks(&ctx.vfs));

    // -- bcrypt.dll --
    {
        auto bcrypt_exports = winapi::create_bcrypt_thunks(ctx.bcrypt_mgr);
        (void)reg.register_dll("bcrypt.dll", std::move(bcrypt_exports));

        auto bcryptprimitives_exports = winapi::create_bcrypt_thunks(ctx.bcrypt_mgr);
        (void)reg.register_dll("bcryptprimitives.dll", std::move(bcryptprimitives_exports));
    }

    // -- wsock32.dll --
    // Shares ws2_32's socket implementation (ctx.ws2_mgr) but uses wsock32's
    // own ordinal numbering, which differs for inet_addr/inet_ntoa/ioctlsocket.
    {
        auto wsock32_exports = winapi::create_wsock32_thunks(ctx.ws2_mgr);
        (void)reg.register_dll("wsock32.dll", std::move(wsock32_exports));
    }

    // -- winmm.dll --
    {
        // mmio: the HMMIO's MMIOINFO and MMIO_ALLOCBUF buffers are process-
        // heap blocks; the DOS IOProc's adwInfo[0] is a kernel32 file HANDLE
        // (Learn mmioOpen). Guest MMIOPROC calls are wired with the CPU in
        // run_callbacks_media.cpp.
        winapi::WinmmManager::MmioServices mmio = ctx.winmm_mgr.mmio_services();
        mmio.alloc = [&ctx](u64 size) {
            return static_cast<GuestAddr>(ctx.heap_mgr.heap_alloc(
                ctx.heap_mgr.get_process_heap(), winapi::HEAP_ZERO_MEMORY, size));
        };
        mmio.realloc = [&ctx](GuestAddr block, u64 size) {
            return static_cast<GuestAddr>(ctx.heap_mgr.heap_realloc(
                ctx.heap_mgr.get_process_heap(), winapi::HEAP_ZERO_MEMORY, block, size));
        };
        mmio.free = [&ctx](GuestAddr block) {
            (void)ctx.heap_mgr.heap_free(ctx.heap_mgr.get_process_heap(), 0, block);
        };
        mmio.adopt_file = [&ctx](u32 vfs_handle, std::string_view path, u32 access) -> u32 {
            const winapi::Handle h = ctx.file_mgr.adopt_vfs_handle(vfs_handle, path, access, true);
            return h == winapi::INVALID_HANDLE ? 0u : h;
        };
        mmio.vfs_handle_for = [&ctx](u32 handle) -> u32 {
            return ctx.file_mgr.vfs_handle_for(handle);
        };
        mmio.close_file = [&ctx](u32 handle) { (void)ctx.file_mgr.close_file(handle); };
        ctx.winmm_mgr.set_mmio_services(std::move(mmio));
        auto winmm_exports = winapi::create_winmm_thunks(ctx.winmm_mgr);
        (void)reg.register_dll("winmm.dll", std::move(winmm_exports));
        const auto dos_proc = ctx.thunk_registry.get_thunk_address(
            "winmm.dll", winapi::kMmioDosIOProcExport, true);
        const auto mem_proc = ctx.thunk_registry.get_thunk_address(
            "winmm.dll", winapi::kMmioMemIOProcExport, true);
        ctx.winmm_mgr.set_mmio_builtin_procs(dos_proc.is_ok() ? dos_proc.value() : 0,
                                             mem_proc.is_ok() ? mem_proc.value() : 0);
    }

    // -- winhttp.dll --
    {
        // GMEM_FIXED process-heap blocks: strings the guest GlobalFrees,
        // proxy results and callback status blocks.
        ctx.winhttp_mgr.set_guest_heap(
            [&ctx](u64 size) { return static_cast<GuestAddr>(ctx.gl_mgr.global_alloc(0, size)); },
            [&ctx](GuestAddr block) { (void)ctx.gl_mgr.global_free(block); });
        auto winhttp_exports = winapi::create_winhttp_thunks(ctx.winhttp_mgr);
        (void)reg.register_dll("winhttp.dll", std::move(winhttp_exports));
    }

    // -- wininet.dll --
    {
        ctx.wininet_mgr.set_guest_allocator(
            [&ctx](u64 size) { return static_cast<GuestAddr>(ctx.gl_mgr.global_alloc(0, size)); });
        auto wininet_exports = winapi::create_wininet_thunks(ctx.wininet_mgr);
        {
            // Audit completion: Vitreo keeps no WinINet URL cache.
            winapi::DllExports audit;
            audit.entries.push_back({"GetUrlCacheEntryInfoExW", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(1);
                    (void)m.arg(2);
                    (void)m.arg(3);
                    (void)m.arg(4);
                    (void)m.arg(5);
                    (void)m.arg(6);
                    // Learn GetUrlCacheEntryInfoEx: a URL that is not in
                    // the cache fails with ERROR_FILE_NOT_FOUND.
                    winapi::set_last_error(2u);
                    m.set_return(0u);
                }, 28});
            merge_exports(wininet_exports, std::move(audit));
        }
        (void)reg.register_dll("wininet.dll", std::move(wininet_exports));
    }

    // -- msimg32.dll --
    {
        ctx.msimg32_mgr.set_gdi_manager(&ctx.gdi_mgr);
        auto msimg32_exports = winapi::create_msimg32_thunks(ctx.msimg32_mgr);
        (void)reg.register_dll("msimg32.dll", std::move(msimg32_exports));
    }

    // -- msi.dll --
    {
        ctx.msi_mgr.set_vfs(&ctx.vfs);
        ctx.msi_mgr.set_registry(&ctx.registry);
        // MSI user registration belongs to the capsule identity exposed by
        // TokenUser, never to the host macOS account or a copied SID literal.
        const auto& sid_bytes = winapi::SecurityStubs::synthetic_sid();
        u64 authority = 0;
        for (usize i = 2; i < 8; ++i) authority = (authority << 8) | sid_bytes[i];
        std::wstring sid = L"S-" + std::to_wstring(sid_bytes[0]) + L"-" + std::to_wstring(authority);
        for (usize i = 0; i < sid_bytes[1]; ++i) {
            u32 value = 0;
            for (usize b = 0; b < 4; ++b) value |= static_cast<u32>(sid_bytes[8 + i * 4 + b]) << (8 * b);
            sid += L"-" + std::to_wstring(value);
        }
        ctx.msi_mgr.set_user_sid(sid);
        // The built-in installer properties describe this process's system
        // state: its environment, processor level, computer name and profile.
        {
            winapi::msi::MachineState machine = winapi::msi::default_machine_state(&ctx.registry, sid);
            auto env = [&ctx](const std::string& name) -> std::optional<std::string> {
                auto v = ctx.env_mgr.get_variable(name);
                if (v.is_err()) return std::nullopt;
                return v.value();
            };
            machine.environment = env;
            machine.processor_level = ctx.sysinfo_mgr.get_system_info().processor_level;
            machine.computer_name = ctx.sysinfo_mgr.get_computer_name();
            if (auto v = env("USERNAME")) machine.logon_user = *v;
            if (auto v = env("USERPROFILE")) machine.profile_dir = *v;
            if (auto v = env("TEMP")) machine.temp_dir = *v;
            if (auto v = env("ProgramData")) machine.program_data = *v;
            if (auto v = env("SystemRoot")) machine.windows_dir = *v;
            // kernel32 keeps the process current directory in the VFS.
            machine.current_directory_source = [&ctx] { return ctx.vfs.cwd(); };
            ctx.msi_mgr.set_machine_state(std::move(machine));
        }
        // Product operations: MsiBeginTransaction's change-of-owner event is a
        // kernel32 event; MsiGetFileSignatureInformation hands back a crypt32
        // certificate context the caller frees with CertFreeCertificateContext.
        {
            winapi::msi::ProductOpsPlatform platform;
            platform.create_event = [&ctx](bool manual_reset, bool initial_state) -> u64 {
                return ctx.sync_mgr.create_event(manual_reset, initial_state, std::string());
            };
            platform.set_event = [&ctx](u64 handle) { (void)ctx.sync_mgr.set_event(static_cast<winapi::Handle>(handle)); };
            platform.create_cert_context = [&ctx](winapi::Marshaler& m, std::span<const u8> der) -> u64 {
                return ctx.crypt32_mgr.create_cert_context_from_der(m, der);
            };
            winapi::msi::set_product_ops_platform(ctx.msi_mgr, std::move(platform));
        }
        // msiexec.exe is a runtime system program (module/system_programs):
        // a real guest process of its directory's bitness that runs the
        // engine and its custom actions (dll_registry_system_programs.cpp).
        // InstallServices / StartServices / StopServices / DeleteServices go
        // through this process's Service Control Manager.
        winapi::msi::set_service_control_manager(&ctx.scm_mgr);
        auto msi_exports = winapi::create_msi_thunks(ctx.msi_mgr);
        (void)reg.register_dll("msi.dll", std::move(msi_exports));
    }

    // -- cabinet.dll --
    // FDI is how installers unpack .cab payloads. Callers delay-load this
    // library, so an absent registration surfaces as 0xC06D007E
    // (DELAYLOAD_MOD_NOT_FOUND) and a call through a null pointer rather than
    // as a missing export.
    {
        ctx.cabinet_mgr.set_thunk_registry(&ctx.thunk_registry);
        const GuestAddr cab_scratch = reserve_scratch_region(
            ctx.vmm, "cabinet FDI",
            winapi::CabinetManager::CABINET_SCRATCH_BASE,
            winapi::CabinetManager::CABINET_SCRATCH_SIZE);
        if (cab_scratch != 0) ctx.cabinet_mgr.set_scratch_base(cab_scratch);
        auto cabinet_exports = winapi::create_cabinet_thunks(ctx.cabinet_mgr);
        (void)reg.register_dll("cabinet.dll", std::move(cabinet_exports));
    }

    // -- msxml3.dll / msxml6.dll --
    // MSXML ships with Windows, so the DOM component is part of the platform
    // rather than something an application installs. Both library names expose
    // the same coclasses; guests bind to whichever version they were built
    // against, and CoCreateInstance resolves the CLSID either way.
    {
        ctx.msxml_mgr.set_thunk_registry(&ctx.thunk_registry);
        ctx.msxml_mgr.set_vfs(&ctx.vfs);
        const GuestAddr msxml_scratch = reserve_scratch_region(
            ctx.vmm, "msxml COM",
            winapi::MsxmlManager::MSXML_SCRATCH_BASE,
            winapi::MsxmlManager::MSXML_SCRATCH_SIZE);
        if (msxml_scratch != 0) ctx.msxml_mgr.set_scratch_base(msxml_scratch);
        // BSTR storage belongs to oleaut32; pass the allocator in rather than
        // giving the DOM component a dependency on that manager's type.
        ctx.msxml_mgr.set_bstr_allocator(
            [&oleaut = ctx.oleaut32_mgr](winapi::Marshaler& m,
                                         const std::vector<u16>& units)
                -> GuestAddr {
                std::vector<vitreo::Byte> bytes(units.size() * 2);
                for (usize i = 0; i < units.size(); ++i) {
                    bytes[i * 2]     = static_cast<vitreo::Byte>(units[i] & 0xFFu);
                    bytes[i * 2 + 1] =
                        static_cast<vitreo::Byte>((units[i] >> 8) & 0xFFu);
                }
                return oleaut.allocate_bstr_bytes(m, bytes);
            });

        winapi::DllExports msxml;
        winapi::register_msxml_thunks(msxml, ctx.msxml_mgr);
        const auto server_exports = [&ctx](winapi::DllExports& exports,
                                               vitreo::msxml::MsxmlVersion version) {
            exports.entries.push_back(winapi::ExportEntry{
                "DllCanUnloadNow", 0, [&ctx, version](winapi::Marshaler& m) {
                    m.set_return(ctx.msxml_mgr.can_unload_now(version));
                }, 0});
            exports.entries.push_back(winapi::ExportEntry{
                "DllGetClassObject", 0, [&ctx, version](winapi::Marshaler& m) {
                    auto clsid = m.read_struct<winapi::Guid>(m.arg_ptr(0));
                    auto iid = m.read_struct<winapi::Guid>(m.arg_ptr(1));
                    const GuestAddr out = m.arg_ptr(2);
                    if (out) (void)m.write_ptr(out, 0);
                    m.set_return(clsid.is_err() || iid.is_err() ? winapi::com::E_INVALIDARG :
                        ctx.msxml_mgr.get_class_object(m, clsid.value(), iid.value(), out, version));
                }, 12});
        };
        auto msxml6 = msxml;
        server_exports(msxml, vitreo::msxml::MsxmlVersion::V3);
        server_exports(msxml6, vitreo::msxml::MsxmlVersion::V6);
        (void)reg.register_dll("msxml3.dll", std::move(msxml));
        (void)reg.register_dll("msxml6.dll", std::move(msxml6));
        ctx.com_mgr.set_external_class_object_provider(
            [&ctx](winapi::Marshaler& m, const winapi::Guid& clsid,
                   const winapi::Guid& iid, GuestAddr out) -> std::optional<vitreo::u32> {
                if (winapi::MsxmlManager::is_dom_document_clsid(clsid) ||
                    winapi::MsxmlManager::is_unimplemented_msxml_clsid(clsid))
                    return ctx.msxml_mgr.get_class_object(m, clsid, iid, out);
                return std::nullopt;
            });

        ctx.com_mgr.set_external_class_factory(
            [&ctx](winapi::Marshaler& m, const winapi::Guid& clsid,
                   const winapi::Guid& iid,
                   GuestAddr ppv) -> std::optional<vitreo::u32> {
                if (winapi::MsxmlManager::is_dom_document_clsid(clsid)) {
                    return ctx.msxml_mgr.create_document(m, clsid, iid, ppv);
                }
                if (winapi::MsxmlManager::is_unimplemented_msxml_clsid(clsid)) {
                    // XSLT and schema caching are real MSXML coclasses Vitreo
                    // does not implement. Fail as an unavailable class rather
                    // than as an unregistered one, so the caller's own
                    // diagnostics point at the right thing.
                    if (ppv != 0) (void)m.write_ptr(ppv, 0);
                    return vitreo::u32{0x80040111u}; // CLASS_E_CLASSNOTAVAILABLE
                }
                return std::nullopt;
            });
    }

    // -- opengl32.dll --
    {
        ctx.opengl32_mgr.set_thunk_registry(&ctx.thunk_registry);
        ctx.opengl32_mgr.set_pixel_format_query([&ctx](u64 hdc) {
            return ctx.gdi_mgr.pixel_format(static_cast<winapi::Handle>(hdc));
        });
        ctx.opengl32_mgr.set_pixel_format_api(winapi::Opengl32Manager::PixelFormatApi{
            [&ctx](winapi::Marshaler& m) { winapi::pixel_format_choose(ctx.gdi_mgr, m); },
            [&ctx](winapi::Marshaler& m) { winapi::pixel_format_describe(ctx.gdi_mgr, m); },
            [&ctx](winapi::Marshaler& m) { winapi::pixel_format_get(ctx.gdi_mgr, m); },
            [&ctx](winapi::Marshaler& m) { winapi::pixel_format_set(ctx.gdi_mgr, m); }});
        // The window behind a DC and its client size (a memory DC: its bitmap),
        // for a context's first wglMakeCurrent. The client rect is copied under
        // HwndManager's lock: a render thread asks while the UI thread may be
        // destroying the window (final review, Finding 8).
        ctx.opengl32_mgr.set_drawable_queries(
            [&ctx](u64 hdc) -> u64 {
                return ctx.gdi_mgr.window_from_dc(static_cast<winapi::Handle>(hdc));
            },
            [&ctx](u64 hdc, vitreo::i32& width, vitreo::i32& height) {
                const auto dc = static_cast<winapi::Handle>(hdc);
                const auto hwnd = static_cast<windowing::HWND>(ctx.gdi_mgr.window_from_dc(dc));
                if (hwnd != windowing::HWND_DESKTOP) {
                    if (const std::optional<windowing::Rect> client = ctx.hwnd_mgr.client_rect(hwnd)) {
                        width = client->width();
                        height = client->height();
                        return true;
                    }
                }
                width = ctx.gdi_mgr.device_caps(dc, winapi::HORZRES);
                height = ctx.gdi_mgr.device_caps(dc, winapi::VERTRES);
                return width > 0 && height > 0;
            });
        ctx.opengl32_mgr.set_pixel_format_ever_set_query(
            [&ctx] { return ctx.gdi_mgr.pixel_format_ever_set(); });
        // A GL window presents into the same native view D3D9's swap chain
        // uses for a top-level guest window, through the shared backend.
        ctx.opengl32_mgr.set_swap_chain_factory(
            [&ctx](vitreo::u64 hwnd, vitreo::u32 width,
                   vitreo::u32 height) -> std::unique_ptr<graphics::BackendSwapChain> {
                graphics::GraphicsBackendDevice* device = graphics::active_backend_device();
                if (device == nullptr || hwnd == 0) return nullptr;
                void* view = ctx.cocoa_bridge.native_presentation_view(
                    static_cast<windowing::HWND>(hwnd));
                if (view == nullptr) return nullptr;
                graphics::BackendSwapChainDesc desc{};
                desc.native_view = view;
                desc.width = width;
                desc.height = height;
                desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
                desc.buffer_count = 2;
                desc.vsync = true;
                // Opaque: no Windows contract composites a GL window's alpha
                // over the desktop (PFD_SUPPORT_COMPOSITION only allows the
                // DWM to compose the window), so alpha-8 formats present opaque.
                desc.premultiplied_alpha = false;
                auto chain = device->create_swap_chain(desc);
                if (chain && !chain->valid()) chain.reset();
                return chain;
            });
        // gdi32!SwapBuffers is opengl32's swap.
        ctx.gdi_mgr.set_swap_buffers_handler(
            [&ctx](winapi::Handle hdc) { return ctx.opengl32_mgr.swap_buffers(hdc); });
        auto opengl32_exports = winapi::create_opengl32_thunks(ctx.opengl32_mgr);
        (void)reg.register_dll("opengl32.dll", std::move(opengl32_exports));
        // Map scratch buffer for glGetString results.
        if (const vitreo::GuestAddr base = reserve_scratch_region(
                ctx.vmm, "opengl32.dll", winapi::OPENGL32_SCRATCH_BASE,
                winapi::OPENGL32_SCRATCH_SIZE);
            base != 0) {
            ctx.opengl32_mgr.set_scratch_base(base);
        }
    }

    // -- glu32.dll --
    {
        auto glu32_exports = winapi::create_glu32_thunks(ctx.glu32_mgr);
        (void)reg.register_dll("glu32.dll", std::move(glu32_exports));
        // The strings live in this region, addressed from wherever it landed;
        // 0 when it could not be reserved anywhere (the string exports then
        // return NULL rather than write memory glu32 does not own).
        ctx.glu32_mgr.set_scratch_base(reserve_scratch_region(
            ctx.vmm, "glu32.dll", winapi::GLU32_SCRATCH_BASE, winapi::GLU32_SCRATCH_SIZE));
        // A GLUquadric is a process-heap allocation, as in Windows' glu32.
        ctx.glu32_mgr.set_heap(winapi::Glu32Manager::Heap{
            [&ctx](u64 size) -> vitreo::GuestAddr {
                return static_cast<vitreo::GuestAddr>(
                    ctx.heap_mgr.heap_alloc(ctx.heap_mgr.get_process_heap(), 0, size));
            },
            [&ctx](vitreo::GuestAddr address) {
                (void)ctx.heap_mgr.heap_free(ctx.heap_mgr.get_process_heap(), 0, address);
            }});
    }

    // -- msvfw32.dll --
    {
        ctx.msvfw32_mgr.set_module_manager(&ctx.module_mgr);
        ctx.msvfw32_mgr.set_registry(&ctx.registry);
        auto msvfw32_exports = winapi::create_msvfw32_thunks(ctx.msvfw32_mgr);
        (void)reg.register_dll("msvfw32.dll", std::move(msvfw32_exports));
    }

    // -- avifil32.dll --
    {
        ctx.avifil32_mgr.set_thunk_registry(&ctx.thunk_registry);
        ctx.avifil32_mgr.set_msvfw32(&ctx.msvfw32_mgr);
        ctx.avifil32_mgr.use_vfs(ctx.vfs);
        ctx.avifil32_mgr.set_heap(winapi::Avifil32Manager::Heap{
            [&ctx](u64 size) -> vitreo::GuestAddr {
                return static_cast<vitreo::GuestAddr>(
                    ctx.heap_mgr.heap_alloc(ctx.heap_mgr.get_process_heap(), 0, size));
            },
            [&ctx](vitreo::GuestAddr address) {
                (void)ctx.heap_mgr.heap_free(ctx.heap_mgr.get_process_heap(), 0, address);
            }});
        ctx.avifil32_mgr.set_com(winapi::Avifil32Manager::Com{
            [&ctx](winapi::Marshaler& m) { return ctx.com_mgr.thread_in_apartment(m); },
            [&ctx](winapi::Marshaler& m) { (void)ctx.com_mgr.ole_initialize(m); },
            [&ctx](winapi::Marshaler& m) { ctx.com_mgr.ole_uninitialize(m); }});
        (void)reg.register_dll("avifil32.dll", winapi::create_avifil32_thunks(ctx.avifil32_mgr));
        // IID_IAVIEditStream / IID_IAVIFile / IID_IAVIStream / IID_IGetFrame
        // are data exports: the GUIDs themselves.
        const auto& iids = winapi::avifil32_data_exports();
        if (const auto block = static_cast<vitreo::GuestAddr>(ctx.vmm.virtual_alloc(
                0, iids.size() * 16, 0x3000 /*MEM_COMMIT|MEM_RESERVE*/, 0x04 /*PAGE_READWRITE*/));
            block != 0) {
            for (usize i = 0; i < iids.size(); ++i) {
                const auto guid = winapi::avifil32::ole_guid(iids[i].data1);
                const vitreo::GuestAddr at = block + static_cast<vitreo::GuestAddr>(i * 16);
                if (ctx.heap_mgr.guest_memory()
                        .write_bytes(at, reinterpret_cast<const Byte*>(guid.data()), guid.size())
                        .is_ok()) {
                    ctx.thunk_registry.register_data_export("avifil32.dll", iids[i].name, at,
                                                            iids[i].ordinal);
                }
            }
            // .rdata in the real DLL.
            u32 old_protect = 0;
            (void)ctx.vmm.virtual_protect(block, iids.size() * 16, 0x02 /*PAGE_READONLY*/,
                                          &old_protect);
        }
    }

    // DirectShow / diagnostics / ACM / capture / WMI / WASAPI: LoadLibrary
    // succeeds; factories that have no engine return CLASS_E_CLASSNOTAVAILABLE
    // or E_NOTIMPL (no fabricated WMI or IMMDevice objects).
    {
        (void)reg.register_dll("quartz.dll", winapi::create_quartz_thunks());
        (void)reg.register_dll("devenum.dll", winapi::create_devenum_thunks());
        (void)reg.register_dll("dxdiagn.dll", winapi::create_dxdiagn_thunks());
        (void)reg.register_dll("d3dxof.dll", winapi::create_d3dxof_thunks());
        (void)reg.register_dll("d3dcompiler_47.dll",
                               winapi::create_d3dcompiler_47_thunks());
        (void)reg.register_dll("msacm32.dll", winapi::create_msacm32_thunks());
        (void)reg.register_dll("avicap32.dll", winapi::create_avicap32_thunks());
        (void)reg.register_dll("msdmo.dll", winapi::create_msdmo_thunks({
            [&ctx](u64 size) { return ctx.com_task_memory_mgr.allocate(size); },
            [&ctx](GuestAddr p) { ctx.com_task_memory_mgr.free(p); }}));
        (void)reg.register_dll("wbemprox.dll", winapi::create_wbemprox_thunks());
        (void)reg.register_dll("wbemdisp.dll", winapi::create_wbemdisp_thunks());
        (void)reg.register_dll("mmdevapi.dll", winapi::create_mmdevapi_thunks(
            [&ctx](winapi::Marshaler& m) { return ctx.com_mgr.activate_audio_interface_async(m); }));
    }
}

void register_printing_network_power_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    // -- winspool.drv --
    {
        auto winspool_exports = winapi::create_winspool_thunks(ctx.winspool_mgr);
        (void)reg.register_dll("winspool.drv", std::move(winspool_exports));
        // Some loaders normalize WINSPOOL.DRV to a module base named
        // "winspool" and then ask the thunk registry for winspool.dll.
        auto winspool_dll_exports = winapi::create_winspool_thunks(ctx.winspool_mgr);
        (void)reg.register_dll("winspool.dll", std::move(winspool_dll_exports));
    }

    // -- wtsapi32.dll --
    {
        auto wts = winapi::create_wtsapi32_thunks(ctx.gl_mgr, ctx.env_mgr);
        // BOOL WTSRegisterSessionNotification(HWND, DWORD)
        wts.entries.push_back({"WTSRegisterSessionNotification", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0); (void)m.arg(1);
                m.set_return(1);
            }, 8});
        // BOOL WTSUnRegisterSessionNotification(HWND)
        wts.entries.push_back({"WTSUnRegisterSessionNotification", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                m.set_return(1);
            }, 4});
        (void)reg.register_dll("wtsapi32.dll", std::move(wts));
    }

    // -- imm32.dll --
    {
        auto imm32_exports = winapi::create_imm32_thunks(ctx.imm32_mgr);
        (void)reg.register_dll("imm32.dll", std::move(imm32_exports));
    }

    // -- iphlpapi.dll --
    {
        // GetIfTable2 allocates on the process heap (FreeMibTable releases it);
        // FIXED_INFO.HostName is the name GetComputerNameEx reports.
        ctx.iphlpapi_mgr.set_heap_manager(&ctx.heap_mgr);
        ctx.iphlpapi_mgr.set_host_name_provider(
            [&ctx]() { return ctx.sysinfo_mgr.get_computer_name(); });
        auto iphlpapi_exports = winapi::create_iphlpapi_thunks(ctx.iphlpapi_mgr);
        (void)reg.register_dll("iphlpapi.dll", std::move(iphlpapi_exports));
    }

    // -- tapi32.dll --
    // Windows always ships Tapi32.dll. A PE that statically imports it
    // (standard.snp's modem provider) must still LoadLibrary; there is no
    // telephony hardware, so the line* exports fail with LINEERR_*.
    {
        (void)reg.register_dll("tapi32.dll", winapi::create_tapi32_thunks());
    }

    // -- dnsapi.dll (Qt5Network DnsQuery_W) --
    {
        auto dnsapi_exports = winapi::create_dnsapi_thunks(ctx.dnsapi_mgr);
        (void)reg.register_dll("dnsapi.dll", std::move(dnsapi_exports));
    }

    // -- wldap32.dll (synchronous LDAP client; libcurl's LDAP backend
    //    imports it by ordinal) --
    {
        ctx.wldap32_mgr.set_heap_manager(&ctx.heap_mgr);
        (void)reg.register_dll("wldap32.dll", winapi::create_wldap32_thunks(ctx.wldap32_mgr));
    }

    // -- sensapi.dll (IsNetworkAlive) --
    {
        winapi::DllExports sens;
        // BOOL IsNetworkAlive(LPDWORD lpdwFlags); NETWORK_ALIVE_LAN = 1
        sens.entries.push_back({"IsNetworkAlive", 0,
            [](winapi::Marshaler& m) {
                GuestAddr flags = m.arg_ptr(0);
                if (flags) (void)m.write_u32(flags, 1u);
                m.set_return(1);
            },
            4});
        (void)reg.register_dll("sensapi.dll", std::move(sens));
    }

    // -- powrprof.dll --
    {
        auto powrprof_exports =
            winapi::create_powrprof_thunks(ctx.sysinfo_mgr);
        {
            // Audit completions. Power schemes belong to the host OS; the platform role is real.
            winapi::DllExports audit;
            // DWORD PowerGetActiveScheme(HKEY UserRootPowerKey, GUID** ActivePolicyGuid)
            // Vitreo presents the stock Windows power configuration: the
            // Balanced plan {381b4222-f694-41f0-9685-ff5bb260df2e} is active
            // (measured on Windows 11: ERROR_SUCCESS, Data1 381b4222). The
            // GUID is LocalAlloc'd; the caller frees it with LocalFree.
            audit.entries.push_back({"PowerGetActiveScheme", 0,
                [&gl = ctx.gl_mgr](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    const GuestAddr out = m.arg_ptr(1);
                    if (out == 0) {
                        m.set_return(87u);  // ERROR_INVALID_PARAMETER
                        return;
                    }
                    const u64 guid = gl.global_alloc(winapi::LPTR, 16, true);
                    if (guid == 0) {
                        m.set_return(8u);  // ERROR_NOT_ENOUGH_MEMORY
                        return;
                    }
                    static constexpr u8 kBalanced[16] = {
                        0x22, 0x42, 0x1b, 0x38, 0x94, 0xf6, 0xf0, 0x41,
                        0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e};
                    (void)m.memory().write_bytes(guid, kBalanced, sizeof(kBalanced));
                    (void)m.write_ptr(out, guid);
                    m.set_return(0u);
                }, 8});
            audit.entries.push_back({"PowerReadACValue", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(1);
                    (void)m.arg(2);
                    (void)m.arg(3);
                    (void)m.arg(4);
                    // No power-setting store: fail without touching
                    // *BufferSize (Learn fills it only on success or
                    // ERROR_MORE_DATA).
                    static std::once_flag once;
                    std::call_once(once, [] {
                        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED powrprof!PowerReadACValue: "
                                             "no power-setting store\n");
                    });
                    m.set_return(50u);
                }, 28});
            audit.entries.push_back({"PowerReadDCValue", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(1);
                    (void)m.arg(2);
                    (void)m.arg(3);
                    (void)m.arg(4);
                    // No power-setting store: fail without touching
                    // *BufferSize (Learn fills it only on success or
                    // ERROR_MORE_DATA).
                    static std::once_flag once;
                    std::call_once(once, [] {
                        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED powrprof!PowerReadDCValue: "
                                             "no power-setting store\n");
                    });
                    m.set_return(50u);
                }, 28});
            // POWER_PLATFORM_ROLE PowerDeterminePlatformRoleEx(ULONG Version)
            // Learn: "If no batteries are available, the function returns
            // PlatformRoleDesktop." Vitreo reports AC power with no battery
            // (GetSystemPowerStatus, SystemBatteryState). Measured: the
            // Version argument is not validated (0, 2 and 3 all answer).
            audit.entries.push_back({"PowerDeterminePlatformRoleEx", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg32(0);
                    m.set_return(1u);  // PlatformRoleDesktop
                }, 4});
            merge_exports(powrprof_exports, std::move(audit));
        }
        (void)reg.register_dll("powrprof.dll", std::move(powrprof_exports));
    }

    // -- secur32.dll / security.dll (shared Schannel SSPI package) --
    // curl's schannel backend LoadLibrary-s security.dll and calls through the
    // table InitSecurityInterface returns, so both names expose one export set.
    {
        ctx.secur32_mgr.set_module_manager(&ctx.module_mgr);
        ctx.secur32_mgr.set_heap_manager(&ctx.heap_mgr);
        ctx.secur32_mgr.set_crypt32_manager(&ctx.crypt32_mgr);
        (void)reg.register_dll("secur32.dll", winapi::create_secur32_thunks(ctx.secur32_mgr));
        (void)reg.register_dll("security.dll", winapi::create_secur32_thunks(ctx.secur32_mgr));
    }

    // -- dhcpcsvc.dll (DHCP client API over the host's cached lease) --
    (void)reg.register_dll("dhcpcsvc.dll", winapi::create_dhcpcsvc_thunks());
}

} // namespace vitreo::cli
