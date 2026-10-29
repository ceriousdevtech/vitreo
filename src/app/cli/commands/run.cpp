// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002 / CLI-005: execute_pe - guest process launch orchestrator

#include "core/math/host_fenv.hpp"
#include "runtime/diagnostics/hang_dump.hpp"
#include "winapi/kernel32/thread/thread_context.hpp"
#include "run.hpp"
#include "run_internal.hpp"
#include "capsule/vfs/image_mount_store.hpp"
#include "runtime/process/runtime_context.hpp"
#include "runtime/execution/inherited_handles.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "platform/scoped_environment.hpp"
#include "dll_registry.hpp"
#include "external_app_mapping.hpp"
#include "import_setup.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "guest_thread_context.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/mapper/section_mapper.hpp"
#include "core/pe/mapper/image_placement.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/loader/load_config.hpp"
#include "winapi/kernel32/system/kuser_shared_data.hpp"
#include "winapi/msvcrt/guest_accel.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/pe/loader/cor_image.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cctype>
#include <span>
#include <vector>
#include <optional>
#include <sys/resource.h>
#include <unistd.h>

extern "C" void vitreo_hostcall_dump(const char* reason);

namespace vitreo::cli {

namespace {

// A Windows process can own far more kernel handles than macOS's default
// per-process soft limit of 256 file descriptors. Vitreo backs pipes, files,
// sockets, and shared sections with host descriptors, and guest child
// processes inherit this limit. Raise it before any guest-visible resource or
// child is created, while respecting a finite host hard limit.
void raise_host_file_descriptor_limit() {
    constexpr rlim_t kTarget = 8192;
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 ||
        limit.rlim_cur >= kTarget) {
        return;
    }

    const rlim_t target = limit.rlim_max == RLIM_INFINITY
        ? kTarget
        : std::min(kTarget, limit.rlim_max);
    if (target <= limit.rlim_cur) return;

    limit.rlim_cur = target;
    (void)::setrlimit(RLIMIT_NOFILE, &limit);
}

} // namespace

// =============================================================================
// Helpers
// =============================================================================

// =============================================================================
// execute_pe Implementation
// =============================================================================

RunResult execute_pe(const RunConfig& config) {
    raise_host_file_descriptor_limit();

    // Load-phase accounting touches several hot WinAPI/JIT paths and emits a
    // heartbeat from every guest process. Keep normal Release execution free
    // of that diagnostic work; tracing and performance-window runs opt in.
    if (std::getenv("VITREO_LOAD_PHASE") != nullptr ||
        std::getenv("VITREO_TRACE") != nullptr ||
        std::getenv("VITREO_PERF_WINDOW") != nullptr) {
        diagnostics::phase_clock_start();
    }

    // -------------------------------------------------------------------------
    // Step 0: Validate configuration
    // -------------------------------------------------------------------------
    if (config.pe_path.empty()) {
        return RunResult{1, 0, "No PE file path specified", false};
    }

    // -------------------------------------------------------------------------
    // Step 0.5: Resolve Windows paths to host capsule paths
    // -------------------------------------------------------------------------
    // If the user passes a Windows-style path (e.g. "C:\Program Files\App\app.exe"),
    // translate it to the corresponding host path inside the selected capsule.
    const std::string capsule_name = config.capsule_name.empty()
                                         ? std::string("default")
                                         : config.capsule_name;
    std::string capsule_root;
    {
        // Make sure the capsule directory exists so `--capsule <name>`
        // implicitly creates the capsule on first use.
        capsule_root = capsule_root_for(capsule_name);
        std::error_code ec;
        std::filesystem::create_directories(capsule_root + "/drives/c", ec);
        std::filesystem::create_directories(capsule_root + "/registry", ec);
    }
    // Named Windows kernel objects are visible across processes in one
    // capsule, but must never collide with the same guest-visible name in a
    // different capsule. ProcessSession children inherit this environment
    // entry before deriving POSIX shm and AF_UNIX resource names.
    ::setenv("VITREO_IPC_SCOPE", capsule_root.c_str(), 1);

    bool pe_from_image = false;
    const std::string resolved_path =
        resolve_pe_host_path(config.pe_path, capsule_root, pe_from_image);

    // An .msi or .msp is a Windows Installer package, not a PE. Explorer
    // opens those with msiexec; do that before the PE loader rejects them.
    if (auto installer = run_installer_package(
            config, capsule_root, capsule_name, resolved_path, pe_from_image)) {
        return *installer;
    }

    // -------------------------------------------------------------------------
    // Step 1: Read PE file from disk
    // -------------------------------------------------------------------------
    std::vector<u8> pe_data;
    auto read_result = pe_from_image
        ? read_image_file_bytes(capsule_root, config.pe_path, pe_data)
        : read_file_bytes(resolved_path, pe_data);
    if (!read_result.success) {
        return read_result;
    }
    // A managed executable (COR20 header). An IL-only one is started at
    // mscoree!_CorExeMain, not at its own entry, is not relocated or bound,
    // and an AnyCPU (PE32, no 32BITREQUIRED) one is a 64-bit process whose
    // image is mapped as PE32+ (core/pe/loader/cor_image.hpp).
    const bool il_only_exe = pe::is_il_only(pe::read_cor_image(pe_data));
    if (il_only_exe) {
        (void)pe::rewrite_il_only_pe32_as_pe32plus(pe_data);
    }

    // If we were spawned by ProcessSession, the host
    // path is a private snapshot copy under TMPDIR named "vitreo_launch_*.exe".
    // Now that the bytes are buffered in `pe_data`, the snapshot is no longer
    // needed on disk; remove it so we do not leave per-launch artefacts behind.
    {
        auto slash = resolved_path.find_last_of('/');
        std::string base = (slash == std::string::npos)
                               ? resolved_path
                               : resolved_path.substr(slash + 1);
        if (base.rfind("vitreo_launch_", 0) == 0 &&
            std::getenv("VITREO_KEEP_LAUNCH") == nullptr) {
            (void)::unlink(resolved_path.c_str());
        } else if (base.rfind("vitreo_launch_", 0) == 0) {
            VITREO_TRACE("[vitreo] KEEP_LAUNCH: preserving snapshot %s\n",
                         resolved_path.c_str());
        }
    }

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] Loaded %zu bytes from %s\n",
                     pe_data.size(), config.pe_path.c_str());
    }

    // -------------------------------------------------------------------------
    // Step 2: Parse PE headers
    // -------------------------------------------------------------------------
    auto parse_result = pe::parse_pe(std::span<const u8>(pe_data));
    if (parse_result.is_err()) {
        return RunResult{1, 0,
            std::string("PE parse error: ") +
                pe::parse_error_message(parse_result.error()),
            false};
    }

    const auto& pe = parse_result.value();
    std::string elevation_credential = config.elevation_credential;
    bool process_elevated =
        config.token_context ==
        ipc::WorkerTokenContext::LocalSystemService;
    if (auto failure = resolve_launch_elevation(
            config, pe, pe_data, resolved_path, elevation_credential,
            process_elevated)) {
        return *failure;
    }

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] PE parsed: %s, image_base=0x%llx, "
                     "entry_rva=0x%x, sections=%zu\n",
                     pe.is_64bit ? "PE32+" : "PE32",
                     static_cast<unsigned long long>(pe.image_base),
                     pe.entry_point_rva,
                     pe.sections.size());
    }

    // -------------------------------------------------------------------------
    // Step 3: Set up guest memory and map PE sections
    // -------------------------------------------------------------------------
    GuestMemory memory;
    cpu::jit::LinearReservationRegistration linear_fault_registration;

    configure_guest_memory_model(memory, linear_fault_registration, pe);

    if (!winapi::install_kuser_shared_data(memory, pe.is_64bit)) {
        return RunResult{1, 0, "Failed to map KUSER_SHARED_DATA", false};
    }

    // Under identity mapping the executable's preferred ImageBase may belong to
    // the host process (the main thread stack is immovable), so a relocatable
    // image is rebased rather than refused. With no reserved holes recorded —
    // every translated run — this returns the PE's own ImageBase unchanged.
    const auto placement = pe::select_image_base(memory, pe);
    if (!placement.ok()) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "Cannot load image at 0x%llx (SizeOfImage 0x%x): %s",
                      static_cast<unsigned long long>(pe.image_base),
                      pe.size_of_image, placement.failure);
        return RunResult{1, 0, detail, false};
    }
    if (placement.relocated) {
        std::fprintf(stderr,
                     "[native32] image rebased off host-owned memory: "
                     "0x%llx -> 0x%llx\n",
                     static_cast<unsigned long long>(pe.image_base),
                     static_cast<unsigned long long>(placement.base));
    }

    auto map_result =
        pe::map_pe(memory, pe, std::span<const u8>(pe_data), placement.base);
    if (map_result.is_err()) {
        return RunResult{1, 0,
            std::string("Section mapping error: ") +
                pe::map_error_message(map_result.error()),
            false};
    }

    const auto& image = map_result.value();

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] Mapped at base=0x%llx, "
                     "entry=0x%llx, size=0x%x, sections=%zu\n",
                     static_cast<unsigned long long>(image.base_address),
                     static_cast<unsigned long long>(image.entry_point),
                     image.image_size,
                     image.sections.size());
    }

    // -------------------------------------------------------------------------
    // Step 4: Apply relocations (if needed)
    // -------------------------------------------------------------------------
    if (il_only_exe) {
        // Not relocated: no code in the image runs (cor_loader_probe).
    } else if (auto failure = apply_main_image_relocations(
            config, memory, pe, pe_data, image, placement)) {
        return *failure;
    }

    // -------------------------------------------------------------------------
    // Step 5: Instantiate all WinAPI managers
    // -------------------------------------------------------------------------
    // Compute the Windows-side exe path so GetCommandLineW returns a proper
    // Windows path (e.g. "C:\Program Files\MyApp\Setup.exe") instead of a
    // host path.  The guest command line must look like a native Windows
    // invocation.
    //
    // If the exe already lives inside a capsule drive tree, derive the
    // Windows path directly (no copy needed). Otherwise copy it into
    // C:\Vitreo\Apps so every Win32 API can resolve it through the VFS.
    std::string win_exe_path;
    // Owns the C:\Vitreo\Apps entry that publishes an external executable's
    // host directory to the guest. Declared here because the guest command
    // line, the module manager, and every later VFS lookup must all agree on
    // the name it chose.
    ExternalAppMapping app_mapping;
    if (auto failure = resolve_windows_exe_path(
            config, capsule_root, capsule_name, resolved_path,
            pe_from_image, win_exe_path, app_mapping)) {
        return *failure;
    }

    // A CreateProcess-spawned child carries the full Windows command line
    // verbatim, because a browser subprocess needs its exact argv. Otherwise
    // reconstruct a "exe-path [args]" line from the resolved Windows exe path.
    std::string guest_cmdline = guest_command_line(config, win_exe_path);
    ipc::ipc_log("startup cmdline: %s", guest_cmdline.c_str());
    // Always name the process in its own log: a tree of identical host
    // binaries (--type=... children) is otherwise only told apart
    // by ps while it lives, and a crashed child's type is what a crash log
    // needs first. One line, bounded.
    std::fprintf(stderr, "[process] guest cmdline: %.600s%s\n",
                 guest_cmdline.c_str(),
                 guest_cmdline.size() > 600 ? " ..." : "");
    // VITREO_SCOPED_ENV=<substring>=><VAR>=<VALUE>[,<VAR>=<VALUE>...]:
    // diagnostics only. A whole guest process tree inherits one environment,
    // so a measurement knob meant for one process has no other way to reach
    // only it. Most knobs are read during static initialisation, so
    // main() has already re-executed a matching process with the values in its
    // environment (reexec_for_scoped_environment); what is reported here is
    // what each knob could see, and a value that arrived too late says so.
    // platform/scoped_environment.hpp. Nothing about any guest is named in the
    // source.
    platform::deliver_scoped_environment_for_guest(guest_cmdline, stderr);
    const bool console_attached = pe.subsystem == pe::Subsystem::WindowsCui;
    const auto inherited_handles = runtime::inherited_handle_values(
        config.inherited_pipes, config.inherited_sync, config.inherited_mappings);
    if (!inherited_handles) {
        return RunResult{1, 0, "Invalid inherited handle values", false};
    }
    RuntimeContext ctx(memory, guest_cmdline, capsule_name, pe.is_64bit,
                       pe.is_large_address_aware(), console_attached,
                       config.process_session, process_elevated,
                       elevation_credential,
                       config.token_context,
                       config.elevation_authorizer, *inherited_handles);
    if (auto failure = configure_process_context(
            ctx, config, pe, pe_data, win_exe_path)) {
        return *failure;
    }

    ensure_capsule_directory_skeleton(ctx);

    // Set VFS current working directory to the exe's directory
    {
        std::string child_cwd = config.guest_cwd;
        if (child_cwd.empty()) {
            child_cwd = win_exe_path;
            auto last_sep = child_cwd.find_last_of("\\/");
            if (last_sep != std::string::npos) {
                child_cwd = child_cwd.substr(0, last_sep);
            } else {
                child_cwd = "C:\\";
            }
        }
        ctx.vfs.set_cwd(child_cwd);
        ctx.k32_ext_mgr.set_current_directory(child_cwd);
        VITREO_TRACE("[vitreo] CWD: %s\n", child_cwd.c_str());
    }

    std::optional<ipc::ProcessRegistration> process_registration;

    register_main_module(ctx, memory, config, pe, pe_data, image,
                         win_exe_path, process_registration);

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] All WinAPI managers instantiated\n");
    }

    // The Windows loader patches the IAT while pages are writable. Some DLLs
    // (e.g. StdUtils.dll) place the IAT in a section that would otherwise be
    // mapped read-only (like .rdata). Temporarily make sections RW for import
    // patching, then restore the intended final protections.
    for (const auto& s : image.sections) {
        (void)memory.protect_region(s.guest_address, s.size, MemoryProtection::ReadWrite);
    }

    // -------------------------------------------------------------------------
    // Step 5b: Register all DLLs in ThunkRegistry
    // -------------------------------------------------------------------------
    register_all_dlls(ctx, memory, pe.is_64bit);
    ctx.fls_mgr.bind_runtime(memory, ctx.vmm,
        ctx.thunk_registry.get_thunk_address("kernel32.dll", "ExitThread").value());

    // The load configuration belongs to every mapped image, including the
    // process executable. Initialize its CFG pointer variables after the
    // Vitreo CFG gateways have been registered and before imports, TLS
    // callbacks, or the entry point can execute.
    {
        const auto check = ctx.thunk_registry.get_thunk_address(
            "vitreo-cfg.dll", "__guard_check_icall");
        const auto dispatch = ctx.thunk_registry.get_thunk_address(
            "vitreo-cfg.dll", "__guard_dispatch_icall");
        // Prefer the guest-code stubs: a CFG-compiled image routes EVERY
        // indirect call through the check pointer, and a host thunk target
        // costs a full dispatch round trip per call where a guest `ret` stays
        // inside generated code. The thunks remain the fallback.
        const auto stubs = pe::install_guard_cf_stubs(memory);
        const GuestAddr check_target =
            stubs.check != 0 ? stubs.check : check.value_or(0);
        const GuestAddr dispatch_target =
            stubs.dispatch != 0 ? stubs.dispatch : dispatch.value_or(0);
        if (check.is_err() || dispatch.is_err() ||
            !pe::initialize_load_config_guard_cf(
                memory, pe, image.base_address,
                check_target, dispatch_target)) {
            return RunResult{1, 0,
                "Invalid PE load-config Control Flow Guard pointers", false};
        }
        VITREO_TRACE(
            "[vitreo] Main-image GuardCF initialized: check=0x%llx "
            "dispatch=0x%llx\n",
            static_cast<unsigned long long>(check_target),
            static_cast<unsigned long long>(dispatch_target));
    }

    // Name thunk addresses in hang dumps. Without this every parked thread
    // reads as a bare 0x7d0…, and "which Win32 call is the process waiting in"
    // takes a second run to answer.
    diagnostics::set_hang_dump_symbolizer(
        [&ctx](u64 addr) -> std::string {
            return ctx.thunk_registry.resolve_name(
                static_cast<GuestAddr>(addr));
        });
    // x64 guest accelerators are reached through guest-facing resolution, not
    // a patched sled (guest_accel.hpp): register them before the synthetic
    // modules' export veneers are written and before Step 6 binds imports.
    if (pe.is_64bit) {
        (void)winapi::install_guest_x64_accelerators(memory,
                                                     ctx.thunk_registry);
    }
    ctx.module_mgr.materialize_synthetic_modules();

    map_thunk_region(memory, ctx, pe);

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] ThunkRegistry: %zu DLLs, %zu functions, "
                     "thunk region [0x%llx..0x%llx)\n",
                     ctx.thunk_registry.dll_count(),
                     ctx.thunk_registry.function_count(),
                     static_cast<unsigned long long>(ctx.thunk_registry.thunk_base()),
                     static_cast<unsigned long long>(ctx.thunk_registry.thunk_end()));
    }

    // -------------------------------------------------------------------------
    // Step 6: Resolve and patch imports
    // -------------------------------------------------------------------------
    GuestAddr process_start = image.entry_point;
    if (il_only_exe) {
        // The loader binds nothing in an IL-only image; it loads mscoree.dll
        // and starts the process at _CorExeMain (Learn _CorValidateImage:
        // "regardless of the entry point specified in the executable").
        auto cor_exe_main =
            ctx.thunk_registry.get_thunk_address("mscoree.dll", "_CorExeMain");
        if (cor_exe_main.is_err() ||
            ctx.module_mgr.load_library("mscoree.dll") == 0) {
            return RunResult{1, 0, "mscoree.dll!_CorExeMain unavailable", false};
        }
        process_start = cor_exe_main.value();
    } else {
        auto import_result = setup_imports(
            ctx, memory, pe, std::span<const u8>(pe_data),
            image.base_address, config.verbose);
        if (!import_result.success) {
            return import_result;
        }
    }

    for (const auto& s : image.sections) {
        (void)memory.protect_region(s.guest_address, s.size, s.protection);
    }
    // The loader's own writes (relocations, imports, load config) are done:
    // from here on the image's pages are clean until the guest changes them.
    memory.images().register_image(GuestImageIdentity{
        image.base_address, image.base_address + image.image_size,
        GuestImageRegistry::identity_from_file(
            std::span<const u8>(pe_data), pe.size_of_headers, pe.image_base),
        pe.image_base});

    // Windows CREATE_SUSPENDED exposes a fully initialized process address
    // space — image, ntdll, TEB/PEB — before the primary thread runs. A parent
    // writes PEB.pShimData via WriteProcessMemory while the child is still
    // suspended. Build that state first, then publish
    // the remote-memory endpoint and wait for ResumeThread.

    // -------------------------------------------------------------------------
    // Step 7: Set up CPU state
    // -------------------------------------------------------------------------
    cpu::CpuState cpu;
    if (pe.is_64bit) {
        cpu.init_long_mode();
    } else {
        cpu.init_protected_mode();
    }
    winapi::seed_windows_initial_fpu(cpu);
    // The guest's FP state is the one just seeded. Host start-up code on this
    // thread (framework initializers, Vitreo's own setup) can leave sticky
    // host FP exception flags set, and the first guest FSTSW/STMXCSR would
    // report them as its own (measured: CoreFoundation/IOKit left
    // FE_INEXACT set on the main thread). Threads Vitreo creates for guest
    // threads start with them clear.
    math::clear_host_fp_exceptions();
    cpu.rip = process_start;
    if (auto* main_thread =
            ctx.thread_mgr.get_thread(ctx.thread_mgr.main_thread_id())) {
        main_thread->start_address = process_start;
    }
    // CreateThread(dwStackSize = 0) takes the EXE's stack reserve/commit.
    ctx.thread_mgr.set_default_stack(pe.stack_reserve, pe.stack_commit);

    // -------------------------------------------------------------------------
    // Step 8: Allocate stack in guest memory
    // -------------------------------------------------------------------------
    const u64 effective_stack_size =
        (pe.is_64bit && config.stack_size < (4ULL * 1024ULL * 1024ULL))
            ? (4ULL * 1024ULL * 1024ULL)
            : config.stack_size;

    u64 stack_bottom = config.stack_base - effective_stack_size;
    const u64 stack_slack = pe.is_64bit ? 0x20000ULL : 0ULL;
    const u64 mapped_stack_bottom = stack_bottom - stack_slack;
    const u64 mapped_stack_size = effective_stack_size + stack_slack;
    const u64 stack_allocation = ctx.vmm.virtual_alloc(
        mapped_stack_bottom, mapped_stack_size,
        winapi::MEM_RESERVE | winapi::MEM_COMMIT,
        winapi::PAGE_READWRITE);

    if (stack_allocation != mapped_stack_bottom) {
        return RunResult{1, 0, "Failed to allocate stack memory", false};
    }

    // Stack overflow guard for the main thread, the same measured 16 KiB the
    // worker threads get (guest_thread_context.cpp,
    // tools/oracle_probe/stack_overflow_probe.c). The x64 slack below
    // StackLimit used to be RW all the way down, so a runaway recursion in
    // JIT'd code -- which never checks StackLimit -- ran through it into
    // unmapped memory, and exception dispatch had nowhere left to write its
    // frame: no handler ran, and the guest's crash reporting never happened.
    // With the guard, the fault arrives with room to spare and dispatch turns
    // it into STATUS_STACK_OVERFLOW. StackLimit itself is unchanged.
    u64 deallocation_stack = 0;
    if (pe.is_64bit) {
        constexpr u64 kStackOverflowGuardBytes = 0x4000;
        u32 old_protect = 0;
        if (ctx.vmm.virtual_protect(mapped_stack_bottom,
                                    kStackOverflowGuardBytes,
                                    winapi::PAGE_NOACCESS, &old_protect)) {
            deallocation_stack = mapped_stack_bottom;
        }
    }

    // Stack pointer starts at top of stack (grows downward)
    u64 stack_top;
    if (pe.is_64bit) {
        // x64: reserve shadow space + return address, then align.
        // The x64 ABI requires RSP to be 16-byte aligned BEFORE a CALL.
        // The CALL pushes an 8-byte return address, so at function entry
        // RSP ≡ 8 (mod 16). We simulate this by aligning to 16 first
        // (representing the caller's RSP) then subtracting 8.
        stack_top = config.stack_base - 0x28;
        stack_top &= ~0xFULL;               // 16-byte align (caller's RSP)
        stack_top -= 8;                      // after CALL: RSP ≡ 8 (mod 16)
    } else {
        // x86: reserve only 4 bytes for return address
        stack_top = config.stack_base - 0x04;
        stack_top &= ~0x3ULL;               // 4-byte align
    }
    cpu.set_sp(stack_top);

    // Seed the entry frame with a sentinel so a normal return from the
    // process entrypoint terminates the process cleanly.
    if (pe.is_64bit) {
        (void)memory.write_u64(stack_top, PROCESS_RETURN_SENTINEL);
    } else {
        (void)memory.write_u32(stack_top, static_cast<u32>(PROCESS_RETURN_SENTINEL));
    }

    // -------------------------------------------------------------------------
    // Step 8b: Set up Thread Environment Block (TEB)
    // -------------------------------------------------------------------------
    pe::TebConfig teb_config;
    teb_config.stack_base   = config.stack_base;
    teb_config.stack_bottom = stack_bottom;
    teb_config.deallocation_stack = deallocation_stack;
    teb_config.image_base   = image.base_address;
    teb_config.process_id = ctx.process_mgr.get_current_process_id();
    teb_config.thread_id = ctx.thread_mgr.main_thread_id();
    teb_config.is_64bit     = pe.is_64bit;
    teb_config.image_path   = win_exe_path;
    teb_config.command_line = guest_cmdline;
    teb_config.command_line_va = ctx.env_mgr.cmdline_w_addr();
    teb_config.command_line_bytes = ctx.env_mgr.cmdline_w_bytes();
    for (const GuestAddr module_base : ctx.module_mgr.get_all_module_handles()) {
        const auto* module = ctx.module_mgr.get_module_info_by_handle(module_base);
        if (module != nullptr) {
            pe::TebModuleInfo entry{module_base, module->path};
            entry.is_executable = module_base == image.base_address;
            // An IL-only executable's loader entry is NULL (cor_loader_probe).
            entry.entry_point = entry.is_executable && module->entry_point == 0
                ? (il_only_exe ? 0 : image.entry_point)
                : module->entry_point;
            // A synthetic (thunk-backed) module still occupies its slot, and a
            // guest resolving an address to its module tests
            // DllBase <= addr < DllBase + SizeOfImage. Reporting zero there
            // makes every system DLL invisible to that lookup.
            entry.size_of_image = module->image_end > module_base
                ? module->image_end - module_base
                : winapi::SYNTHETIC_MODULE_SLOT_SIZE;
            teb_config.loaded_modules.push_back(entry);
        }
    }

    auto teb_result = pe::setup_teb(memory, cpu, teb_config);
    if (teb_result.is_err()) {
        return RunResult{1, 0, "Failed to set up TEB/PEB", false};
    }

    const auto& teb_info = teb_result.value();

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] TEB at 0x%llx, PEB at 0x%llx, %s base set\n",
                     static_cast<unsigned long long>(teb_info.teb_address),
                     static_cast<unsigned long long>(teb_info.peb_address),
                     pe.is_64bit ? "GS" : "FS");
    }

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] CPU: RIP=0x%llx, RSP=0x%llx\n",
                     static_cast<unsigned long long>(cpu.rip),
                     static_cast<unsigned long long>(cpu.sp()));
    }

    // -------------------------------------------------------------------------
    // Step 8b2: Set up Thread Local Storage (TLS) from PE directory
    // -------------------------------------------------------------------------
    u64 pe_tls_data_start = 0;     // VA of TLS template in mapped PE image
    u32 pe_tls_data_size  = 0;     // size of TLS template data
    u64 pe_tls_callbacks_addr = 0; // VA of null-terminated TLS callback array
    if (auto failure = setup_main_image_tls(
            config, memory, pe, image, teb_info, pe_tls_data_start,
            pe_tls_data_size, pe_tls_callbacks_addr)) {
        return *failure;
    }

    // After TLS setup for the main EXE, hand the TEB to the ModuleManager so
    // LoadLibrary'd DLLs can install their own TLS slot indices (see
    // setup_dll_tls in src/winapi/kernel32/module.cpp). Without this, any DLL
    // with a PE TLS directory shares slot 0 with the main EXE — corrupting
    // MSVC's _Init_thread_* fast-path and crashing in DllMain static-init.
    ctx.module_mgr.set_main_teb(static_cast<GuestAddr>(teb_info.teb_address));
    ctx.module_mgr.set_main_peb(static_cast<GuestAddr>(teb_info.peb_address));
    // PEB->ProcessHeap must equal GetProcessHeap() (heap_contract_probe peb_eq).
    ctx.heap_mgr.bind_peb(teb_info.peb_address, pe.is_64bit);
    (void)ctx.heap_mgr.get_process_heap();
    ctx.module_mgr.register_thread_teb(
        static_cast<GuestAddr>(teb_info.teb_address),
        pe.is_64bit ? 64u : 32u);
    ctx.tls_mgr.register_teb(memory,
                             static_cast<GuestAddr>(teb_info.teb_address),
                             pe.is_64bit);

    // CREATE_SUSPENDED boundary: PE/modules/TEB/PEB are mapped; primary thread
    // has not executed. Brokers may now Read/WriteProcessMemory / VirtualAllocEx
    // (including PEB.pShimData zeroing) before ResumeThread.
    const bool primary_thread_suspended = [] {
        const char* value =
            std::getenv("VITREO_PRIMARY_THREAD_SUSPENDED");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool is_64bit = pe.is_64bit;
    const u64 peb_address = teb_info.peb_address;
    std::optional<GuestThreadContext> com_rpc_thread_context;
    bool com_rpc_jit_initialized = false;
    ipc::RemoteMemoryServer remote_memory_server(
        memory, ctx.vmm, &ctx.sync_mgr, &ctx.fmm_mgr, &ctx.handle_table,
        &ctx.file_mgr);
    install_remote_memory_handlers(
        remote_memory_server, ctx, memory, is_64bit, peb_address,
        pe_tls_data_start, pe_tls_data_size, com_rpc_thread_context,
        com_rpc_jit_initialized);
    if (!remote_memory_server.start(primary_thread_suspended)) {
        return RunResult{
            1, 0, "Failed to start guest process memory endpoint", false};
    }
    remote_memory_server.wait_for_initial_resume();

    // -------------------------------------------------------------------------
    // Step 8c: Set up wndproc callback for DispatchMessageW
    // -------------------------------------------------------------------------
    install_window_message_callbacks(cpu, memory, ctx, is_64bit, peb_address,
                                     pe_tls_data_start, pe_tls_data_size);
    install_thread_callbacks(memory, ctx, is_64bit, peb_address,
                             pe_tls_data_start, pe_tls_data_size);
    install_media_callbacks(cpu, memory, ctx, is_64bit, peb_address,
                            pe_tls_data_start, pe_tls_data_size);
    install_crt_and_exception_callbacks(cpu, memory, ctx, is_64bit);
    install_module_and_com_callbacks(cpu, memory, ctx);

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] WndProc callback configured\n");
    }

    // -------------------------------------------------------------------------
    // Step 9: Run interpreter with thunk dispatch
    // -------------------------------------------------------------------------
    RunResult result = run_guest_primary_thread(
        config, ctx, memory, cpu, image, is_64bit, pe_tls_callbacks_addr,
        win_exe_path);
    ipc::ipc_log(
        "guest execution finished image='%s' exit_code=0x%08x success=%d "
        "instructions=%llu rip=0x%llx process_exited=%d",
        config.pe_path.c_str(), static_cast<u32>(result.exit_code),
        result.success ? 1 : 0,
        static_cast<unsigned long long>(result.instructions_executed),
        static_cast<unsigned long long>(cpu.rip),
        ctx.process_mgr.has_exited() ? 1 : 0);
    std::fprintf(
        stderr,
        "[vitreo] guest-exit pid=%d image='%s' exit_code=0x%08x success=%d "
        "instructions=%llu rip=0x%llx process_exited=%d\n",
        static_cast<int>(::getpid()), config.pe_path.c_str(),
        static_cast<u32>(result.exit_code), result.success ? 1 : 0,
        static_cast<unsigned long long>(result.instructions_executed),
        static_cast<unsigned long long>(cpu.rip),
        ctx.process_mgr.has_exited() ? 1 : 0);
    cpu::jit::dump_stack_proof_stats();
    std::fflush(stderr);

    // VITREO_DUMP_ON_EXIT=1 dumps the hostcall ring whenever a guest process
    // finishes. A child that exits *cleanly* leaves no crash artifact, so
    // there is otherwise nothing to explain why its main() returned — which is
    // exactly the situation for a browser service that abandons its IPC
    // channel.
    if (std::getenv("VITREO_DUMP_ON_EXIT") != nullptr) {
        vitreo_hostcall_dump("guest-exit");
    }

    shut_down_guest_process(ctx, result, config, app_mapping);

    // Propagate chain-load request (if any) so the caller can re-invoke
    if (ctx.process_mgr.has_pending_launch()) {
        auto [path, args] = ctx.process_mgr.take_pending_launch();
        result.pending_launch_path = std::move(path);
        result.pending_launch_args = std::move(args);
        VITREO_TRACE("[vitreo] Chain-load pending: '%s'\n",
                     result.pending_launch_path.c_str());
    }

    result.process_elevated = process_elevated;
    result.elevation_credential = std::move(elevation_credential);
    result.token_context = config.token_context;
    return result;
}

} // namespace vitreo::cli
