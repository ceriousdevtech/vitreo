// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: mscoree.dll, the OS .NET runtime shim
// (winapi/system/mscoree/mscoree_shim.hpp).

#include "dll_registry_internal.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/system/mscoree/mscoree_shim.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::cli {

namespace {

using winapi::mscoree::ExportRole;
using winapi::mscoree::MscoreeShim;
using winapi::mscoree::ShimExport;

constexpr const char* kCleanupCallback = "VitreoShimImplCleanupCallback";
constexpr const char* kRuntimeStringCallback = "VitreoShimImplRuntimeStringCallback";

/// The shim's guest operations: calls of the kernel32 exports the in-box
/// mscoree.dll imports, made on the calling guest thread. One scratch page
/// per operation, taken lazily: a bound shim's forwards make no calls.
class GuestShimHost final : public winapi::mscoree::ShimHost {
public:
    GuestShimHost(RuntimeContext& ctx, GuestMemory& memory, bool is64) : ctx_(ctx), memory_(memory), is64_(is64) {}
    ~GuestShimHost() override {
        if (scratch_ != 0) (void)ctx_.vmm.virtual_free_internal(scratch_);
    }
    GuestShimHost(const GuestShimHost&) = delete;
    GuestShimHost& operator=(const GuestShimHost&) = delete;

    [[nodiscard]] bool is_64bit() const override { return is64_; }

    std::optional<std::u16string> environment_variable(std::u16string_view name) override {
        constexpr u32 kChars = 2048;
        Mark mark(*this);
        const GuestAddr n = wide(std::u16string(name));
        const GuestAddr buf = take(kChars * 2);
        if (n == 0 || buf == 0) return std::nullopt;
        const u64 len = call("kernel32.dll", "GetEnvironmentVariableW", {n, buf, kChars}) & 0xFFFFFFFFu;
        if (len == 0 || len >= kChars) return std::nullopt;
        return read_wide(buf);
    }
    std::u16string windows_directory() override {
        constexpr u32 kChars = 260;
        Mark mark(*this);
        const GuestAddr buf = take(kChars * 2);
        if (buf == 0) return {};
        const u64 len = call("kernel32.dll", "GetWindowsDirectoryW", {buf, kChars}) & 0xFFFFFFFFu;
        if (len == 0 || len >= kChars) return {};
        return read_wide(buf);
    }
    std::vector<std::u16string> policy_v4_builds() override {
        std::vector<std::u16string> out;
        auto key = ctx_.registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                          L"SOFTWARE\\Microsoft\\.NETFramework\\Policy\\v4.0", false);
        if (key.is_err()) return out;
        auto names = ctx_.registry.enum_values(key.value());
        if (names.is_ok()) {
            for (const auto& n : names.value()) out.emplace_back(n.begin(), n.end());
        }
        ctx_.registry.close_key(key.value());
        return out;
    }
    u64 load_library(const std::u16string& path) override {
        Mark mark(*this);
        const GuestAddr p = wide(path);
        return p == 0 ? 0 : call("kernel32.dll", "LoadLibraryW", {p});
    }
    void free_library(u64 module) override { (void)call("kernel32.dll", "FreeLibrary", {module}); }
    u64 get_proc_address(u64 module, const char* name) override {
        Mark mark(*this);
        const GuestAddr n = ansi(name);
        return n == 0 ? 0 : call("kernel32.dll", "GetProcAddress", {module, n});
    }
    u64 get_proc_address_ordinal(u64 module, u16 ordinal) override {
        return call("kernel32.dll", "GetProcAddress", {module, ordinal});
    }
    u64 call(u64 fn, const std::vector<u64>& args) override { return invoke(fn, args); }
    u64 shim_instance() override {
        Mark mark(*this);
        const GuestAddr n = wide(u"mscoree.dll");
        return n == 0 ? 0 : call("kernel32.dll", "GetModuleHandleW", {n});
    }

private:
    static constexpr u32 kScratch = 0x2000;

    class Mark {
    public:
        explicit Mark(GuestShimHost& host) : host_(host), cursor_(host.cursor_) {}
        ~Mark() { host_.cursor_ = cursor_; }
        Mark(const Mark&) = delete;
        Mark& operator=(const Mark&) = delete;

    private:
        GuestShimHost& host_;
        u32 cursor_;
    };

    u64 invoke(u64 fn, const std::vector<u64>& args) {
        if (!ctx_.system_program_call) return 0;
        const u64 r = ctx_.system_program_call(static_cast<GuestAddr>(fn), args.data(), args.size(), is64_);
        return is64_ ? r : (r & 0xFFFFFFFFull);
    }
    u64 call(const char* dll, const char* name, const std::vector<u64>& args) {
        auto fn = ctx_.thunk_registry.get_thunk_address(dll, name);
        return fn.is_err() ? 0 : invoke(fn.value(), args);
    }
    GuestAddr take(u32 bytes) {
        if (scratch_ == 0) scratch_ = static_cast<GuestAddr>(ctx_.vmm.virtual_alloc(0, kScratch, 0x3000, 0x04));
        const u32 aligned = (bytes + 15u) & ~15u;
        if (scratch_ == 0 || cursor_ + aligned > kScratch) return 0;
        const GuestAddr at = scratch_ + cursor_;
        cursor_ += aligned;
        return at;
    }
    GuestAddr wide(const std::u16string& text) {
        const GuestAddr at = take(static_cast<u32>((text.size() + 1) * 2));
        for (usize i = 0; at != 0 && i <= text.size(); ++i)
            (void)memory_.write_u16(at + i * 2, i < text.size() ? static_cast<u16>(text[i]) : 0);
        return at;
    }
    GuestAddr ansi(const char* text) {
        const std::string s(text);
        const GuestAddr at = take(static_cast<u32>(s.size() + 1));
        for (usize i = 0; at != 0 && i <= s.size(); ++i)
            (void)memory_.write_u8(at + i, i < s.size() ? static_cast<u8>(s[i]) : 0);
        return at;
    }
    std::u16string read_wide(GuestAddr at) const {
        std::u16string out;
        for (usize i = 0; i < 32768; ++i) {
            const u16 c = memory_.read_u16(at + i * 2).value_or(0);
            if (c == 0) break;
            out.push_back(static_cast<char16_t>(c));
        }
        return out;
    }

    RuntimeContext& ctx_;
    GuestMemory& memory_;
    bool is64_;
    GuestAddr scratch_ = 0;
    u32 cursor_ = 0;
};

/// Continue at @p target exactly as if the caller had called it: the stack
/// (return address, arguments) and every register are left as the export
/// received them -- the in-box shim's tail jump into mscoreei.
void tail_transfer(winapi::Marshaler& m, u64 target) {
    cpu::CpuState* cpu = m.cpu();
    cpu->rip = target;
    cpu->thunk_control_transfer = true;
}

/// The shim object, created on first use so its callback addresses (internal
/// entries of this same DLL) are already published.
struct ShimSlot {
    std::once_flag once;
    std::unique_ptr<MscoreeShim> shim;
};

MscoreeShim& shim_of(RuntimeContext& ctx, ShimSlot& slot) {
    std::call_once(slot.once, [&] {
        auto cleanup = ctx.thunk_registry.get_thunk_address("mscoree.dll", kCleanupCallback, true);
        auto runtime_string = ctx.thunk_registry.get_thunk_address("mscoree.dll", kRuntimeStringCallback, true);
        slot.shim = std::make_unique<MscoreeShim>(cleanup.is_ok() ? cleanup.value() : 0,
                                                  runtime_string.is_ok() ? runtime_string.value() : 0);
    });
    return *slot.shim;
}

}  // namespace

void register_mscoree_dll(RuntimeContext& ctx, bool guest_is_64bit) {
    auto slot = std::make_shared<ShimSlot>();
    winapi::DllExports exports;
    for (const ShimExport& e : winapi::mscoree::shim_exports(guest_is_64bit)) {
        const u16 bytes = guest_is_64bit ? 0 : e.x86_stdcall_bytes;
        winapi::ApiHandler handler;
        switch (winapi::mscoree::export_role(e)) {
            case ExportRole::ImageUnloading:
                // VOID _CorImageUnloading(PVOID): the in-box shim's body is `ret`.
                handler = [](winapi::Marshaler&) {};
                break;
            case ExportRole::ValidateImage:
                // The loader-side work Learn describes (PE32 -> PE32+, entry
                // at _CorExeMain) is done by Vitreo's loader itself (see
                // core/pe/loader/cor_image.hpp); a guest calling the export
                // directly is not served.
                handler = [](winapi::Marshaler& m) {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED mscoree!_CorValidateImage: guest call\n");
                    });
                    m.set_return(winapi::mscoree::E_FAIL_HR);
                };
                break;
            case ExportRole::GetVersionExW:
                handler = [&ctx](winapi::Marshaler& m) {
                    auto target = ctx.thunk_registry.get_thunk_address("kernel32.dll", "GetVersionExW");
                    if (target.is_err()) {
                        m.set_return(0);
                        return;
                    }
                    tail_transfer(m, target.value());
                };
                break;
            case ExportRole::ForwardByName:
            case ExportRole::ForwardByOrdinal:
                handler = [&ctx, slot, e](winapi::Marshaler& m) {
                    MscoreeShim& shim = shim_of(ctx, *slot);
                    GuestShimHost host(ctx, m.memory(), m.cpu_is_64bit());
                    const u64 target = shim.forward_target(host, e);
                    if (target == 0) {
                        m.set_return(winapi::mscoree::CLR_E_SHIM_RUNTIMEEXPORT);
                        return;
                    }
                    tail_transfer(m, target);
                };
                break;
        }
        exports.entries.push_back(winapi::ExportEntry{e.name, e.ordinal, std::move(handler), bytes});
    }

    // The callbacks the shim registers with mscoreei (measured from the
    // in-box shim: void WINAPI(void) and void WINAPI(LPCWSTR)). Internal:
    // absent from the export table, reachable only by the addresses passed.
    winapi::ExportEntry cleanup{kCleanupCallback, 0, [&ctx, slot](winapi::Marshaler&) {
        shim_of(ctx, *slot).on_cleanup();
    }, 0};
    cleanup.internal = true;
    exports.entries.push_back(std::move(cleanup));
    winapi::ExportEntry runtime_string{kRuntimeStringCallback, 0, [&ctx, slot](winapi::Marshaler& m) {
        const GuestAddr at = m.arg_ptr(0);
        std::u16string value;
        for (usize i = 0; at != 0 && i < 32768; ++i) {
            const u16 c = m.memory().read_u16(at + i * 2).value_or(0);
            if (c == 0) break;
            value.push_back(static_cast<char16_t>(c));
        }
        shim_of(ctx, *slot).on_runtime_string(std::move(value));
    }, 4};
    runtime_string.internal = true;
    exports.entries.push_back(std::move(runtime_string));

    (void)ctx.thunk_registry.register_dll("mscoree.dll", std::move(exports));
}

}  // namespace vitreo::cli
