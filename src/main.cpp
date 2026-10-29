// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI entry point

#include "app/cli/parser/arg_parser.hpp"
#include "app/cli/commands/command.hpp"
#include "app/cli/commands/capsule.hpp"
#include "app/cli/commands/mount.hpp"
#include "app/cli/commands/process.hpp"
#include "app/cli/commands/run.hpp"
#include "runtime/diagnostics/hang_dump.hpp"
#include "windowing/messages/input_latency.hpp"
#include "graphics/common/present_rate.hpp"
#include <core/cpu/jit/jit_compiler.hpp>
#include "runtime/diagnostics/host_resource_probe.hpp"
#include "runtime/diagnostics/memory_brake.hpp"
#include "core/cpu/jit/hot_block_dump.hpp"
#include "core/cpu/jit/jit_symbol_map.hpp"
#include "core/cpu/jit/ir/ir_frontend.hpp"
#include <vitreo/diagnostics.hpp>
#include "core/memory/early_low_reservation.hpp"
#include "platform/macos/tcc_responsibility.hpp"
#include <filesystem>
#include <cstdio>
#include <memory>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <csignal>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <execinfo.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include "core/cpu/jit/linear_fault.hpp"
#include <sys/ucontext.h>

#include "core/cpu/jit/jit_crash_breadcrumbs.hpp"

static void crash_handler(int sig, siginfo_t* info, void* ucontext_raw) {
    // The JIT's compile-time tripwires fprintf then abort. abort raises
    // SIGABRT without flushing stdio, so the reason was missing from every
    // crash report. The fprintf has already returned, so the stderr lock is
    // not held by this thread.
    std::fflush(stderr);
    const char msg[] = "\n==== CRASH (signal ";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    char num[40];
    int len = snprintf(num, sizeof(num), "%d, pid=%d", sig, getpid());
    write(STDERR_FILENO, num, static_cast<size_t>(len));
    const char msg2[] = ") ====\n";
    write(STDERR_FILENO, msg2, sizeof(msg2) - 1);
    if (info) {
        char buf[80];
        // si_code separates the fault kinds a bare address cannot: an
        // unmapped page (SEGV_MAPERR) from a permission fault (SEGV_ACCERR)
        // from an object error on a mapped file (BUS_OBJERR).
        int blen = snprintf(buf, sizeof(buf),
                            "Faulting address: 0x%llx (si_code=%d)\n",
                            (unsigned long long)(uintptr_t)info->si_addr,
                            info->si_code);
        write(STDERR_FILENO, buf, static_cast<size_t>(blen));

        // What the address actually is at fault time. A pre-flight probe can
        // say a range is mapped and the access still fault, so ask the kernel
        // here: mapped-but-refused and no-longer-mapped are different bugs
        // and si_code alone does not separate them from a stale probe.
        {
            mach_vm_address_t a =
                (mach_vm_address_t)(uintptr_t)info->si_addr;
            mach_vm_size_t rsize = 0;
            vm_region_basic_info_data_64_t vinfo{};
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            const mach_vm_address_t probe = a;
            char rbuf[160];
            int rlen;
            if (mach_vm_region(mach_task_self(), &a, &rsize,
                               VM_REGION_BASIC_INFO_64,
                               (vm_region_info_t)&vinfo, &cnt, &obj)
                    == KERN_SUCCESS && probe >= a && probe < a + rsize) {
                rlen = snprintf(rbuf, sizeof(rbuf),
                                "Fault region: 0x%llx+0x%llx prot=%d max=%d "
                                "shared=%d off=0x%llx\n",
                                (unsigned long long)a,
                                (unsigned long long)rsize,
                                vinfo.protection, vinfo.max_protection,
                                vinfo.shared ? 1 : 0,
                                (unsigned long long)vinfo.offset);
            } else {
                rlen = snprintf(rbuf, sizeof(rbuf),
                                "Fault region: UNMAPPED (next base 0x%llx)\n",
                                (unsigned long long)a);
            }
            write(STDERR_FILENO, rbuf, (size_t)rlen);
        }
    }
    // Extract the faulting PC from the host ucontext. The register file is
    // host-architecture specific; the symbolization below is not.
    if (ucontext_raw) {
        auto* uc = static_cast<ucontext_t*>(ucontext_raw);
        char buf[120];
        int blen;
#if defined(__aarch64__)
        // ARM64 keeps the caller's return address in LR, which is worth
        // symbolizing alongside PC: a JIT frame breaks the frame-pointer
        // chain, so LR is often the only surviving link to the caller.
        uintptr_t pc = uc->uc_mcontext->__ss.__pc;
        uintptr_t lr = uc->uc_mcontext->__ss.__lr;
        blen = snprintf(buf, sizeof(buf),
            "PC: 0x%llx  LR: 0x%llx\n",
            (unsigned long long)pc, (unsigned long long)lr);
#else
        // x86_64 has no link register; the return address lives on the stack
        // and is not safely readable from a signal handler. Report RSP as
        // data instead, and symbolize PC alone.
        uintptr_t pc = uc->uc_mcontext->__ss.__rip;
        uintptr_t lr = pc;
        blen = snprintf(buf, sizeof(buf),
            "PC: 0x%llx  RSP: 0x%llx\n",
            (unsigned long long)pc,
            (unsigned long long)uc->uc_mcontext->__ss.__rsp);
#endif
        write(STDERR_FILENO, buf, static_cast<size_t>(blen));
#if defined(__aarch64__)
        if (sig == SIGILL && info && reinterpret_cast<uintptr_t>(info->si_addr) == pc) {
            const unsigned int instruction =
                *reinterpret_cast<const volatile unsigned int*>(pc);
            blen = snprintf(buf, sizeof(buf), "Faulting ARM instruction: 0x%08x\n", instruction);
            write(STDERR_FILENO, buf, static_cast<size_t>(blen));
        }
#endif
        // Resolve PC/LR to image+offset. Without this the addresses are
        // meaningless under ASLR, and backtrace() usually yields nothing here
        // because a JIT frame breaks the frame-pointer chain — so these two
        // lines are often the only symbolizable evidence a crash leaves.
        // `atos -o <image> -l <base> <addr>` turns them into source lines.
#if defined(__aarch64__)
        constexpr int kSymbolizeCount = 2;   // PC and LR
#else
        constexpr int kSymbolizeCount = 1;   // PC only
#endif
        for (int which = 0; which < kSymbolizeCount; ++which) {
            uintptr_t addr = (which == 0) ? pc : lr;
            Dl_info dli;
            if (dladdr(reinterpret_cast<void*>(addr), &dli) != 0 &&
                dli.dli_fname != nullptr) {
                blen = snprintf(buf, sizeof(buf),
                    "%s in %s (base 0x%llx, +0x%llx)%s%s\n",
                    which == 0 ? "PC" : "LR", dli.dli_fname,
                    (unsigned long long)(uintptr_t)dli.dli_fbase,
                    (unsigned long long)(addr - (uintptr_t)dli.dli_fbase),
                    dli.dli_sname ? " sym=" : "",
                    dli.dli_sname ? dli.dli_sname : "");
                if (blen > 0) {
                    // snprintf reports the untruncated length. Image/symbol
                    // names routinely exceed this buffer; never leak the
                    // adjacent signal-handler stack into the crash report.
                    const size_t count = static_cast<size_t>(blen) < sizeof(buf)
                        ? static_cast<size_t>(blen) : sizeof(buf) - 1;
                    write(STDERR_FILENO, buf, count);
                }
            }
        }
        // Dump the host general-purpose registers for context.
#if defined(__aarch64__)
        for (int i = 0; i <= 20; i += 4) {
            char rbuf[256];
            int rlen;
            if (i + 3 <= 20) {
                rlen = snprintf(rbuf, sizeof(rbuf),
                    "X%d=0x%llx X%d=0x%llx X%d=0x%llx X%d=0x%llx\n",
                    i, (unsigned long long)uc->uc_mcontext->__ss.__x[i],
                    i+1, (unsigned long long)uc->uc_mcontext->__ss.__x[i+1],
                    i+2, (unsigned long long)uc->uc_mcontext->__ss.__x[i+2],
                    i+3, (unsigned long long)uc->uc_mcontext->__ss.__x[i+3]);
            } else {
                rlen = snprintf(rbuf, sizeof(rbuf),
                    "X%d=0x%llx\n",
                    i, (unsigned long long)uc->uc_mcontext->__ss.__x[i]);
            }
            write(STDERR_FILENO, rbuf, static_cast<size_t>(rlen));
        }
#else
        {
            const auto& ss = uc->uc_mcontext->__ss;
            const struct { const char* name; unsigned long long val; } regs[] = {
                {"RAX", ss.__rax}, {"RBX", ss.__rbx}, {"RCX", ss.__rcx},
                {"RDX", ss.__rdx}, {"RSI", ss.__rsi}, {"RDI", ss.__rdi},
                {"RBP", ss.__rbp}, {"RSP", ss.__rsp}, {"R8 ", ss.__r8},
                {"R9 ", ss.__r9},  {"R10", ss.__r10}, {"R11", ss.__r11},
                {"R12", ss.__r12}, {"R13", ss.__r13}, {"R14", ss.__r14},
                {"R15", ss.__r15},
            };
            constexpr size_t kRegCount = sizeof(regs) / sizeof(regs[0]);
            for (size_t i = 0; i < kRegCount; i += 4) {
                char rbuf[256];
                int off = 0;
                for (size_t j = i; j < i + 4 && j < kRegCount; ++j) {
                    off += snprintf(rbuf + off,
                                    sizeof(rbuf) - static_cast<size_t>(off),
                                    "%s=0x%llx ", regs[j].name, regs[j].val);
                }
                off += snprintf(rbuf + off,
                                sizeof(rbuf) - static_cast<size_t>(off), "\n");
                write(STDERR_FILENO, rbuf, static_cast<size_t>(off));
            }
            // GS carries the guest TEB once the native backend is running;
            // a fault with an unexpected GS base means the boundary swap was
            // missed, so it is worth recording even before that lands.
            char sbuf[96];
            int slen = snprintf(sbuf, sizeof(sbuf),
                "RFLAGS=0x%llx CS=0x%llx FS=0x%llx GS=0x%llx\n",
                (unsigned long long)ss.__rflags, (unsigned long long)ss.__cs,
                (unsigned long long)ss.__fs, (unsigned long long)ss.__gs);
            write(STDERR_FILENO, sbuf, static_cast<size_t>(slen));
        }
#endif
    }
    {
        char buf[256];
        const auto& crash = vitreo::cpu::jit::jit_crash_breadcrumbs;
        int blen = snprintf(buf, sizeof(buf),
            "Last JIT block: 0x%llx  Guest ESP: 0x%08x\n",
            static_cast<unsigned long long>(crash.last_block_addr),
            crash.guest_esp);
        write(STDERR_FILENO, buf, static_cast<size_t>(blen));

        // When the fault PC is inside a published JIT block, say WHICH guest
        // instruction and WHICH ARM word. Without this a host fault in
        // generated code is only "somewhere in the arena", and the difference
        // between that and a named instruction is the difference between
        // guessing at a lowering and reading it.
        //
        // Both lookups are async-signal-safe: the side table is lock-free
        // immutable metadata, and the instruction word is a plain load from a
        // page we are already executing.
#if defined(__aarch64__)
        {
            auto* uctx = static_cast<ucontext_t*>(ucontext_raw);
            const uintptr_t pc =
                uctx ? static_cast<uintptr_t>(uctx->uc_mcontext->__ss.__pc)
                     : 0;
            vitreo::GuestAddr grip = 0;
            if (pc != 0 &&
                vitreo::cpu::jit::jit_guest_rip_for_pc(pc, grip)) {
                const vitreo::u32 word =
                    *reinterpret_cast<const vitreo::u32*>(pc);
                blen = snprintf(buf, sizeof(buf),
                    "JIT fault: guest RIP 0x%08llx  ARM 0x%08x @ pc+0\n",
                    (unsigned long long)grip, word);
                write(STDERR_FILENO, buf, static_cast<size_t>(blen));
                // Three words of context either side, so the addressing mode
                // that produced the bad base is visible rather than inferred.
                for (int k = -3; k <= 3; ++k) {
                    const vitreo::u32 w =
                        *reinterpret_cast<const vitreo::u32*>(
                            pc + static_cast<intptr_t>(k) * 4);
                    blen = snprintf(buf, sizeof(buf),
                        "   %s%+d: 0x%08x\n", k == 0 ? "->" : "  ", k, w);
                    write(STDERR_FILENO, buf, static_cast<size_t>(blen));
                }
            }
        }
#endif

        blen = snprintf(buf, sizeof(buf),
            "Region1: 0x%08x-0x%08x host=0x%llx\n",
            crash.region1_base, crash.region1_end,
            (unsigned long long)crash.region1_host);
        write(STDERR_FILENO, buf, static_cast<size_t>(blen));

        blen = snprintf(buf, sizeof(buf),
            "Region2: 0x%08x-0x%08x host=0x%llx\n",
            crash.region2_base, crash.region2_end,
            (unsigned long long)crash.region2_host);
        write(STDERR_FILENO, buf, static_cast<size_t>(blen));
    }
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    // _exit skips atexit; still write a hot-block dump if one was requested.
    // dump() is one-shot and try_lock-based: no-op if atexit already ran or
    // g_mu is held/unusable. Must not throw here.
    vitreo::cpu::jit::hot_dump::dump();
    vitreo::diagnostics::perf_window_dump("crash");
    _exit(128 + sig);
}

// When VITREO_LOG_DIR is set, redirect this process's stdout and stderr to a
// per-pid file under it. A multi-process guest forks a fresh host for the
// client, each renderer, and each helper, and those children inherit the
// parent's fds, so without this every process's guest output interleaves
// into one stream. The environment variable is inherited across the relaunch
// exec, so every host in the tree gets its own log. No-op when unset, so the
// default single-stream behavior is unchanged.
static void maybe_redirect_per_pid_log() {
    const char* dir = std::getenv("VITREO_LOG_DIR");
    if (dir == nullptr || dir[0] == '\0') return;
    ::mkdir(dir, 0700);  // best-effort; ignore EEXIST
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/vitreo_%d.log", dir,
                  static_cast<int>(::getpid()));
    int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    (void)::dup2(fd, STDOUT_FILENO);
    (void)::dup2(fd, STDERR_FILENO);
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) ::close(fd);
}

static std::string xml_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

static uint64_t stable_name_hash(std::string_view name) {
    uint64_t hash = 1469598103934665603ull;
    for (const char raw_ch : name) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t stable_bundle_hash(
    std::string_view identity_key,
    std::string_view app_name,
    std::span<const vitreo::u8> icon_data) {
    uint64_t hash = stable_name_hash(identity_key);
    hash ^= 0;
    hash *= 1099511628211ull;
    for (const char raw_byte : app_name) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    hash ^= 0;
    hash *= 1099511628211ull;
    for (const vitreo::u8 byte : icon_data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

static std::string bundle_executable_name(std::string_view app_name) {
    std::string result;
    result.reserve(app_name.size());
    for (const char raw_ch : app_name) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        if (ch < 0x20 || ch == '/' || ch == ':') {
            result.push_back('_');
        } else {
            result.push_back(static_cast<char>(ch));
        }
    }
    if (result.empty() || result == "." || result == "..") {
        result = "VitreoGuest";
    }
    return result;
}

static std::filesystem::path invoked_executable_path() {
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }
    return buffer.data();
}

static std::filesystem::path current_executable_path() {
    const auto invoked_path = invoked_executable_path();
    if (invoked_path.empty()) return {};
    std::error_code ec;
    auto path = std::filesystem::canonical(invoked_path, ec);
    return ec ? invoked_path : path;
}

static std::filesystem::path prepare_guest_app_bundle(
    std::string_view app_name,
    std::string_view identity_key,
    std::span<const vitreo::u8> icon_data,
    bool accessory,
    std::string& error) {
    namespace fs = std::filesystem;

    const fs::path runtime_path = current_executable_path();
    if (runtime_path.empty()) {
        error = "unable to locate the Vitreo executable";
        return {};
    }

    std::ostringstream hash_text;
    hash_text << std::hex << std::setfill('0') << std::setw(16)
              << stable_bundle_hash(
                     accessory ? std::string(identity_key) + "\naccessory" :
                                 std::string(identity_key), app_name, icon_data);
    const std::string executable_name = bundle_executable_name(app_name);
    const fs::path root =
        fs::temp_directory_path() /
        ("vitreo-app-bundles-" + std::to_string(static_cast<unsigned>(::getuid())));
    // Keep the hash in the parent directory, not in the .app name.
    // LaunchServices may briefly fall back to the bundle directory name before
    // it consumes Info.plist, and that fallback is visible in the Dock and
    // Command-Tab switcher.
    const fs::path bundle =
        root / hash_text.str() / (executable_name + ".app");
    const fs::path contents = bundle / "Contents";
    const fs::path macos = contents / "MacOS";
    const fs::path resources = contents / "Resources";
    const fs::path wrapper_executable = macos / executable_name;

    std::error_code ec;
    fs::create_directories(macos, ec);
    if (ec) {
        error = "unable to create app bundle directory: " + ec.message();
        return {};
    }

    const fs::path icon_path = resources / "AppIcon.ico";
    if (!icon_data.empty()) {
        fs::create_directories(resources, ec);
        if (ec) {
            error = "unable to create app bundle resource directory: " +
                    ec.message();
            return {};
        }

        const fs::path temporary_icon =
            resources / ("AppIcon." +
                         std::to_string(static_cast<int>(::getpid())) +
                         ".ico");
        {
            std::ofstream icon(temporary_icon,
                               std::ios::binary | std::ios::trunc);
            if (!icon) {
                error = "unable to create app bundle icon";
                return {};
            }
            icon.write(
                reinterpret_cast<const char*>(icon_data.data()),
                static_cast<std::streamsize>(icon_data.size()));
            icon.close();
            if (!icon) {
                error = "unable to write app bundle icon";
                return {};
            }
        }
        fs::rename(temporary_icon, icon_path, ec);
        if (ec) {
            (void)fs::remove(temporary_icon);
            error = "unable to publish app bundle icon: " + ec.message();
            return {};
        }
    } else {
        (void)fs::remove(icon_path, ec);
        ec.clear();
    }

    const std::string escaped_name = xml_escape(app_name);
    const std::string escaped_executable = xml_escape(executable_name);
    const fs::path plist_path = contents / "Info.plist";
    const fs::path temporary_plist =
        contents / ("Info.plist." + std::to_string(static_cast<int>(::getpid())));
    {
        std::ofstream plist(temporary_plist, std::ios::binary | std::ios::trunc);
        if (!plist) {
            error = "unable to create app bundle Info.plist";
            return {};
        }
        plist
            << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            << "<plist version=\"1.0\"><dict>\n"
            << "<key>CFBundleDisplayName</key><string>" << escaped_name
            << "</string>\n"
            << "<key>CFBundleExecutable</key><string>" << escaped_executable
            << "</string>\n"
            << "<key>CFBundleIdentifier</key><string>dev.vitreo.guest.v2."
            << hash_text.str() << "</string>\n"
            << (icon_data.empty()
                    ? ""
                    : "<key>CFBundleIconFile</key><string>AppIcon.ico</string>\n")
            << "<key>CFBundleName</key><string>" << escaped_name
            << "</string>\n"
            // Windows.Devices.Bluetooth reports the host radio, and macOS
            // TCC terminates a process that reaches CoreBluetooth with no
            // declared purpose. Declaring it here is what lets a guest see
            // the real adapter instead of "no radio".
            << "<key>NSBluetoothAlwaysUsageDescription</key><string>"
            << escaped_name
            << " uses Bluetooth to reach devices the Windows application "
               "asks for.</string>\n"
            // Windows audio capture (waveIn, DirectSound capture, WASAPI
            // eCapture) records from the host microphone. macOS TCC asks
            // the user once and terminates a process that opens audio input
            // without a declared purpose.
            << "<key>NSMicrophoneUsageDescription</key><string>"
            << escaped_name
            << " records from the microphone when the Windows application "
               "asks for audio input.</string>\n"
            // Windows has no App Transport Security: WinHTTP, WinInet and
            // crypt32's root auto-update fetch plain http:// URLs, which
            // NSURLSession refuses (-1022) once a bundle declares no
            // exception (user decision 2026-10-27).
            << "<key>NSAppTransportSecurity</key><dict>"
               "<key>NSAllowsArbitraryLoads</key><true/></dict>\n"
            << "<key>CFBundlePackageType</key><string>APPL</string>\n"
            << "<key>CFBundleVersion</key><string>1</string>\n"
            << (accessory ? "<key>LSUIElement</key><true/>\n" : "")
            << "</dict></plist>\n";
        plist.close();
        if (!plist) {
            error = "unable to write app bundle Info.plist";
            return {};
        }
    }

    fs::rename(temporary_plist, plist_path, ec);
    if (ec) {
        (void)fs::remove(temporary_plist);
        error = "unable to publish app bundle Info.plist: " + ec.message();
        return {};
    }

    bool replace_link = true;
    if (fs::is_symlink(wrapper_executable, ec) && !ec) {
        const fs::path target = fs::read_symlink(wrapper_executable, ec);
        replace_link = ec || target != runtime_path;
    }
    if (replace_link) {
        ec.clear();
        (void)fs::remove(wrapper_executable, ec);
        ec.clear();
        fs::create_symlink(runtime_path, wrapper_executable, ec);
        if (ec) {
            error = "unable to link Vitreo into app bundle: " + ec.message();
            return {};
        }
    }

    return wrapper_executable;
}

static void maybe_reexec_with_guest_app_identity(
    int argc,
    char* argv[],
    const vitreo::cli::ParsedArgs& args) {
    if (args.command != "run" || args.positional_args.empty()) return;

    if (const char* active_wrapper = std::getenv("VITREO_MACOS_APP_WRAPPER");
        active_wrapper && active_wrapper[0] != '\0') {
        const auto invoked_path = invoked_executable_path();
        if (!invoked_path.empty() &&
            invoked_path.lexically_normal() ==
                std::filesystem::path(active_wrapper).lexically_normal()) {
            return;
        }
    }

    const std::string& identity_path =
        args.relaunched_as.has_value()
            ? args.relaunched_as.value()
            : args.positional_args.front();
    const std::string capsule_name =
        args.capsule_name.has_value() ? args.capsule_name.value() : "default";
    const auto metadata =
        vitreo::cli::extract_application_metadata_for_launch(
            args.positional_args.front(), capsule_name);
    const std::string_view explicit_name =
        args.app_name.has_value()
            ? std::string_view(args.app_name.value())
            : std::string_view{};
    const std::string app_name =
        vitreo::cli::select_application_display_name(
            explicit_name, metadata, identity_path);
    if (app_name.empty()) return;
    VITREO_TRACE(
        "[identity] executable=\"%s\" explicit=\"%.*s\" "
        "product=\"%s\" description=\"%s\" resolved=\"%s\" "
        "icon_bytes=%zu\n",
        identity_path.c_str(), static_cast<int>(explicit_name.size()),
        explicit_name.empty() ? "" : explicit_name.data(),
        metadata.product_name.c_str(),
        metadata.file_description.c_str(), app_name.c_str(),
        metadata.icon_data.size());

    // Keep the process/menu identity correct even when app-bundle wrapping is
    // explicitly disabled or bundle preparation fails.
    (void)::setenv("VITREO_MACOS_APP_NAME", app_name.c_str(), 1);

    // A descendant needs an app identity when it owns native windows. Agent
    // bundles provide that without a startup Dock tile; windowing promotes
    // only when the visible HWND's Windows taskbar signals request it.
    // Apple LSUIElement and NSApplicationActivationPolicyAccessory contracts.
    const char* role = std::getenv("VITREO_MACOS_PROCESS_ROLE");
    const bool accessory = role != nullptr && std::strcmp(role, "child") == 0;

    if (const char* disabled = std::getenv("VITREO_NO_APP_BUNDLE");
        disabled && disabled[0] != '\0' && std::strcmp(disabled, "0") != 0) {
        return;
    }

    std::string error;
    const std::filesystem::path wrapper =
        prepare_guest_app_bundle(
            app_name, identity_path, metadata.icon_data, accessory, error);
    if (wrapper.empty()) {
        std::fprintf(stderr,
                     "[vitreo] Unable to prepare macOS app identity '%s': %s\n",
                     app_name.c_str(), error.c_str());
        return;
    }

    std::vector<char*> exec_argv;
    std::string wrapper_text = wrapper.string();
    exec_argv.reserve(static_cast<size_t>(argc) + 1);
    exec_argv.push_back(wrapper_text.data());
    for (int i = 1; i < argc; ++i) exec_argv.push_back(argv[i]);
    exec_argv.push_back(nullptr);

    (void)::setenv("VITREO_MACOS_APP_WRAPPER", wrapper_text.c_str(), 1);
    // Become the bundle's image responsible for itself, so macOS privacy
    // consent (TCC) is asked for this application instead of inherited from
    // whatever launched it (a shell inside Terminal or another host app:
    // their recorded answers would otherwise decide for the guest, and a
    // "Don't Allow" there refuses without asking). POSIX_SPAWN_SETEXEC keeps
    // what execv keeps: pid, process group, parent and descriptors.
    // platform/macos/tcc_responsibility.hpp.
    const int disclaim_error = vitreo::platform::spawn_disclaiming_responsibility(
        nullptr, wrapper_text.c_str(), exec_argv.data(), *_NSGetEnviron(), true);
    std::fprintf(stderr,
                 "[vitreo] Unable to relaunch '%s' responsible for itself (%s); macOS "
                 "privacy consent stays with the launching app\n",
                 app_name.c_str(), std::strerror(disclaim_error));
    ::execv(wrapper_text.c_str(), exec_argv.data());

    const int exec_error = errno;
    (void)::unsetenv("VITREO_MACOS_APP_WRAPPER");
    std::fprintf(stderr,
                 "[vitreo] Unable to relaunch with macOS app identity '%s': %s\n",
                 app_name.c_str(), std::strerror(exec_error));
}

// The real entry point. On x86_64 a constructor has already claimed the low
// 4GB (see early_low_reservation.hpp); this call is idempotent.
static int vitreo_main(int argc, char* argv[]) {
    using namespace vitreo::cli;
    namespace fs = std::filesystem;

    // FIRST. On the native x86_64 backend the guest address is the host
    // address, so a 32-bit guest must live below 4GB -- and linking with
    // -pagezero_size (required to reach that space at all) also hands it to
    // the host as ordinary allocatable memory. A constructor(101) already
    // claimed it before C++ statics malloc'd; this call is idempotent and
    // still needed if that object file was not pulled into a smaller tool.
    (void)vitreo::memory::reserve_low_guest_space();

    maybe_redirect_per_pid_log();
    reexec_for_scoped_environment(argc, argv);  // VITREO_SCOPED_ENV: before any thread (run.hpp)

    // Set up alternate signal stack so handler works even on stack overflow
    static char alt_stack_mem[SIGSTKSZ * 4];
    stack_t ss;
    ss.ss_sp = alt_stack_mem;
    ss.ss_size = sizeof(alt_stack_mem);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    // Install crash handler with SA_ONSTACK | SA_SIGINFO
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    // SIGTRAP and SIGFPE were previously unhandled, so a guest or JIT-induced
    // trap terminated the process with no dump at all — exit code 133 (SIGTRAP)
    // and nothing else to work from. Route them through the same handler.
    sigaction(SIGTRAP, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);

    // SIGUSR1 / SIGINFO dump where every guest thread is, so a stalled run can
    // be placed without restarting it under an instruction-count probe.
    vitreo::diagnostics::install_hang_dump();
    vitreo::diagnostics::install_host_resource_probe();
    vitreo::diagnostics::install_memory_brake();

    // Tier 1 IR coverage, reachable WITHOUT the process exiting.
    //
    // The coverage report is otherwise armed on `std::atexit`, which a GUI
    // guest never reaches: it parks in its message loop, SIGTERM does not run
    // atexit handlers, and the guest re-execs as its own app bundle so a
    // launcher timeout never reaches it either. That made the accept rate
    // unobtainable for exactly the workloads worth measuring — a real
    // application rather than a console tool that exits on its own.
    //
    // Prints nothing unless `VITREO_JIT_IR_STATS` armed the counters, so this
    // costs one no-op call per hang dump in the default configuration.
    vitreo::diagnostics::register_hang_dump_section(
        [](std::FILE* out) { vitreo::cpu::jit::ir::stats::dump_to(out); });

    // Same reasoning for the input-latency table (VITREO_INPUT_LATENCY): a GUI
    // guest is killed rather than exiting, so the atexit report never runs.
    // `kill -USR1 <pid>` prints it mid-run instead. No-op unless armed.
    vitreo::diagnostics::register_hang_dump_section(
        [](std::FILE* out) { vitreo::windowing::input_latency::report_to(out); });

    // Frame rate. Registered in every process because the process that
    // presents is not the one that runs the message loop: in a multi-process
    // the GPU process owns the swapchain.
    vitreo::diagnostics::register_hang_dump_section(
        [](std::FILE* out) { vitreo::graphics::present_rate_report(out); });

    // Compile-memo reuse. Four browser processes translating the same startup
    // path is the largest remaining startup cost, and the memo's cross-process
    // hit rate has never been measured; its atexit report never runs because a
    // GUI guest is killed. No-op unless VITREO_JIT_MEMO_STATS armed it.
    vitreo::diagnostics::register_hang_dump_section(
        [](std::FILE* out) { vitreo::cpu::jit::memo_stats_dump_to(out); });
    vitreo::diagnostics::register_hang_dump_section(
        [](std::FILE* out) { vitreo::cpu::jit::symbol_map::flush_provenance(out); });

    // If invoked with no arguments, show usage
    if (argc < 2) {
        VITREO_TRACE(
            "Vitreo - Windows Runtime Engine for macOS ARM64\n"
            "Usage: vitreo run <pe-path> [--verbose] [--capsule <name>] "
            "[--app-name <name>] [-- <args...>]\n"
            "       vitreo capsule create <name>\n"
            "       vitreo capsule list\n"
            "       vitreo capsule info|validate|rename|clone ...\n"
            "       vitreo capsule destroy <name>\n"
            "       vitreo process list|stop|stop-all [--capsule <name>] [--json]\n"
            "       vitreo mount [image.iso|image.cue|image.bin] [--drive D] [--capsule <name>]\n"
            "       vitreo unmount <drive> [--capsule <name>]\n"
            "       vitreo version\n"
            "       vitreo help\n");
        return 1;
    }

    auto parsed = parse_args(argc, const_cast<const char* const*>(argv));
    if (!parsed.success) {
        // Always surface parse errors on stderr — VITREO_TRACE is compiled out
        // in release, which made bad invocations (e.g. an unknown flag) fail
        // silently with exit 1 and no message.
        std::fprintf(stderr, "Error: %s\n", parsed.error.c_str());
        return 1;
    }

    // LaunchServices derives the Dock label from the containing .app bundle,
    // not from NSProcessInfo. Re-exec once through a tiny generated bundle
    // before any AppKit initialization. exec preserves the PID and the
    // supervised process-tree relationship.
    maybe_reexec_with_guest_app_identity(argc, argv, parsed.args);

    CommandDispatcher dispatcher;

    // Capsule root: ~/.vitreo/capsules
    fs::path capsule_root;
    if (const char* home = std::getenv("HOME")) {
        capsule_root = fs::path(home) / ".vitreo" / "capsules";
    } else {
        capsule_root = fs::current_path() / ".vitreo" / "capsules";
    }
    // Ensure the root exists so capsule create/list work out of the box
    std::error_code ec;
    fs::create_directories(capsule_root, ec);

    static CapsuleManager capsule_manager(capsule_root);
    static MountManager mount_manager(capsule_root);

    auto run_cmd     = std::make_unique<RunCommand>();
    auto capsule_cmd = std::make_unique<CapsuleCommand>(capsule_manager);
    auto mount_cmd   = std::make_unique<MountCommand>(mount_manager);
    auto unmount_cmd = std::make_unique<UnmountCommand>(mount_manager);
    auto process_cmd = std::make_unique<ProcessCommand>(capsule_root);
    auto ver_cmd     = std::make_unique<VersionCommand>();
    auto help_cmd    = std::make_unique<HelpCommand>(dispatcher);

    dispatcher.register_command(std::move(run_cmd));
    dispatcher.register_command(std::move(capsule_cmd));
    dispatcher.register_command(std::move(mount_cmd));
    dispatcher.register_command(std::move(unmount_cmd));
    dispatcher.register_command(std::move(process_cmd));
    dispatcher.register_command(std::move(ver_cmd));
    dispatcher.register_command(std::move(help_cmd));

    auto result = dispatcher.dispatch(parsed.args);

    // A Unix process status preserves only the low eight bits of a normal
    // exit code.  Windows process exit codes are 32-bit values (including
    // NTSTATUS exception values such as 0xC0000005), so native supervisors
    // may request the complete result through this explicit side channel.
    // The file is written only after command execution has completed; a
    // supervisor reads it after observing process termination.
    if (const char* result_path = std::getenv("VITREO_RESULT_FILE");
        result_path != nullptr && result_path[0] != '\0') {
        std::ofstream output(result_path, std::ios::out | std::ios::trunc);
        if (output) {
            output << static_cast<uint32_t>(result.exit_code) << '\n';
        }
    }

    if (!result.message.empty()) {
        auto* stream = (result.exit_code == 0) ? stdout : stderr;
        std::fprintf(stream, "%s\n", result.message.c_str());
    }

    return result.exit_code;
}


int main(int argc, char* argv[]) {
    return vitreo_main(argc, argv);
}
