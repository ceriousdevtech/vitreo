// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_setupapi_inf.cpp
/// @brief SetupAPI's INF engine against real Windows: replays
///        oracle/setupapi_inf_probe.c call for call through Vitreo's
///        exports (both ABIs) and compares every printed line with
///        oracle/setupapi_inf_probe{32,64}.expected (winoracle, Windows 11
///        26200). The probe's INF text is read out of the probe source, the
///        cabinets and versioned DLLs are the committed fixtures the probe
///        ran against.
///
/// Normalisation, both sides: the INF last-write FILETIME in the
/// SP_INF_INFORMATION dump and heap pointers in TARGET*/FILEOPDELAYED
/// notifications. Expected side only: Windows spells the Windows directory
/// C:\WINDOWS where Vitreo's GetWindowsDirectory gives C:\Windows, and the
/// x86 SetupIterateCabinet leaves FILEPATHS.Flags / FullTargetName
/// uninitialised. Recorded gaps (kGaps) are measured behaviour Vitreo does
/// not implement and fails loudly instead.

#include <gtest/gtest.h>

#include "capsule/registry/registry.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/system/setupapi/setupapi.hpp"
#include "winapi/system/version/version.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

const std::filesystem::path kHere = std::filesystem::path(__FILE__).parent_path();

constexpr GuestAddr kArena = 0x100000;
constexpr u32 kArenaSize = 0x80000;
constexpr GuestAddr kStack1 = 0x1F0000;
constexpr GuestAddr kStack2 = 0x1F8000;
constexpr GuestAddr kScratch = 0x200000;
constexpr GuestAddr kQueueCb = 0x7001, kNullCtxCb = 0x7002, kCabCb = 0x7003;
constexpr u64 kCtx = 0x5150, kCabCtx = 0x7070;

// SetupAPI.h values the probe passes.
constexpr u32 INF_STYLE_OLDNT = 1, INF_STYLE_WIN4 = 2;
constexpr u32 SPINST_REGISTRY = 0x4, SPINST_FILES = 0x10, SPINST_ALL = 0x3ff;

/// The probe's kProbeInf, unescaped from its C string literals.
std::string probe_inf_text() {
    std::ifstream in(kHere / "oracle" / "setupapi_inf_probe.c");
    std::string line, out;
    bool inside = false;
    while (std::getline(in, line)) {
        if (!inside) {
            inside = line.find("static const char kProbeInf[] =") != std::string::npos;
            continue;
        }
        const auto first = line.find('"');
        if (first == std::string::npos) continue;
        const auto last = line.rfind('"');
        for (usize i = first + 1; i < last; ++i) {
            if (line[i] != '\\') { out.push_back(line[i]); continue; }
            const char e = line[++i];
            out.push_back(e == 'r' ? '\r' : e == 'n' ? '\n' : e);
        }
        if (line.find(';', last) != std::string::npos) break;
    }
    return out;
}

std::string format(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

class Replay {
public:
    explicit Replay(bool is64) : x64_(is64), bits_(is64 ? 64 : 32) {
        root_ = std::filesystem::temp_directory_path() /
                ("vitreo-setupinf-" + std::to_string(::getpid()) + (is64 ? "-64" : "-32"));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_ / "vprobe" / std::to_string(bits_) / "src");
        std::filesystem::create_directories(root_ / "Windows" / "Temp");
        std::filesystem::create_directories(root_ / "Windows" / "INF");
        EXPECT_TRUE(vfs_.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root_.string())).is_ok());
        for (const char* f : {"probe.cab", "flag.cab", "ver1.dll", "ver2.dll"}) {
            std::filesystem::copy_file(kHere / "fixtures" / f, host("src\\" + std::string(f)));
        }
        EXPECT_TRUE(memory_.map_region(kArena, 0x100000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory_.map_region(kScratch, 0x10000, MemoryProtection::ReadWrite).is_ok());
        for (auto* cpu : {&cpu1_, &cpu2_}) {
            if (x64_) cpu->init_long_mode(); else cpu->init_protected_mode();
        }
        m1_.attach_cpu(cpu1_);
        m2_.attach_cpu(cpu2_);
        services_.reserve = [](u32) -> GuestAddr { return kScratch; };
        services_.call = [this](GuestAddr fn, const u64* args, usize count, bool) -> u64 {
            EXPECT_EQ(count, 4u);
            return callback(fn, args[0], static_cast<u32>(args[1]), args[2], args[3]);
        };
        exports_ = create_setupapi_thunks([] { return std::string{}; }, &vfs_, &registry_, nullptr, &services_);
        w_ = "C:\\vprobe\\" + std::to_string(bits_);
    }

    ~Replay() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::vector<std::string> run();

private:
    // ---- plumbing ----
    [[nodiscard]] std::filesystem::path host(const std::string& relative_to_w) const {
        std::string rel = relative_to_w;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        return root_ / "vprobe" / std::to_string(bits_) / rel;
    }
    [[nodiscard]] std::filesystem::path host_abs(const std::string& win) const {
        std::string rel = win.substr(3);
        std::replace(rel.begin(), rel.end(), '\\', '/');
        return root_ / rel;
    }
    GuestAddr alloc(u32 bytes) {
        const GuestAddr at = kArena + cursor_;
        cursor_ += (bytes + 15u) & ~15u;
        EXPECT_LT(cursor_, kArenaSize);
        for (u32 i = 0; i < bytes; ++i) (void)memory_.write_u8(at + i, 0);
        return at;
    }
    GuestAddr str(const std::string& s) {
        const GuestAddr at = alloc(static_cast<u32>(s.size()) + 1);
        (void)m1_.write_ansi_string(at, s);
        return at;
    }
    std::string read_str(GuestAddr at) {
        if (at == 0) return "(null)";
        auto v = m1_.read_ansi_string(at);
        return v.is_ok() ? v.value() : "<bad>";
    }
    u64 ptr(GuestAddr at) {
        return x64_ ? memory_.read_u64(at).value() : memory_.read_u32(at).value();
    }
    u32 u32_at(GuestAddr at) { return memory_.read_u32(at).value(); }
    u16 u16_at(GuestAddr at) { return memory_.read_u16(at).value(); }

    u64 call_with(cpu::CpuState& cpu, Marshaler& m, GuestAddr stack, const char* name,
                  std::initializer_list<u64> args) {
        cpu.set_sp(stack);
        usize i = 0;
        for (u64 a : args) {
            if (x64_) {
                switch (i) {
                    case 0: cpu.gpr.rcx = a; break;
                    case 1: cpu.gpr.rdx = a; break;
                    case 2: cpu.gpr.r8 = a; break;
                    case 3: cpu.gpr.r9 = a; break;
                    default: (void)memory_.write_u64(stack + 8 * (i + 1), a);
                }
            } else {
                (void)memory_.write_u32(stack + 4 + 4 * i, static_cast<u32>(a));
            }
            ++i;
        }
        const auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
                                     [name](const ExportEntry& e) { return e.name == name; });
        if (it == exports_.entries.end()) {
            ADD_FAILURE() << "missing export " << name;
            return 0;
        }
        it->handler(m);
        return x64_ ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }
    u64 call(const char* name, std::initializer_list<u64> args) {
        return call_with(cpu1_, m1_, kStack1, name, args);
    }
    u64 nested(const char* name, std::initializer_list<u64> args) {
        return call_with(cpu2_, m2_, kStack2, name, args);
    }
    [[nodiscard]] u64 invalid_handle() const { return x64_ ? ~0ull : 0xFFFFFFFFull; }
    void print(const std::string& s) { out_.push_back(s); }

    std::string mask(const std::string& s) {
        std::string out;
        for (usize i = 0; i < s.size();) {
            const auto starts = [&](const std::string& p) {
                return s.size() - i >= p.size() &&
                       std::equal(p.begin(), p.end(), s.begin() + static_cast<std::ptrdiff_t>(i),
                                  [](char a, char b) { return std::tolower(a) == std::tolower(b); });
            };
            if (starts(w_)) { out += "<W>"; i += w_.size(); continue; }
            if (starts(win_)) { out += "<WIN>"; i += win_.size(); continue; }
            out.push_back(s[i++]);
        }
        return out;
    }
    std::string mask_ptr(GuestAddr at) { return at == 0 ? "(null)" : mask(read_str(at)); }

    void write_text(const std::string& rel, const std::string& text) {
        std::ofstream(host(rel), std::ios::binary) << text;
    }
    void list_dir(const std::string& label, const std::string& dir) {
        std::vector<std::pair<std::string, std::string>> names;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(host_abs(dir), ec)) {
            const std::string n = e.path().filename().string();
            names.emplace_back(n, e.is_directory() ? n + "/" : n + "=" + std::to_string(e.file_size()));
        }
        std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) {
            std::string x = a.first, y = b.first;
            for (auto& c : x) c = static_cast<char>(std::tolower(c));
            for (auto& c : y) c = static_cast<char>(std::tolower(c));
            return x < y;
        });
        std::string line = "  dir " + label + ":";
        for (const auto& n : names) line += " " + n.second;
        print(line);
    }
    void clear_dir(const std::string& dir) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(host_abs(dir), ec)) {
            std::filesystem::remove_all(e.path(), ec);
        }
    }

    // ---- the probe's callbacks ----
    u64 callback(GuestAddr fn, u64 ctx, u32 note, u64 p1, u64 p2);
    u32 queue_cb(u64 ctx, u32 note, u64 p1, u64 p2);
    u32 cab_cb(u64 ctx, u32 note, u64 p1, u64 p2);

    void install(u64 inf, const char* section, u32 flags, const std::string* root, u32 copy);
    void open_case(const char* label, const std::string& rel, const char* cls, u32 style);
    void print_info_buffer(const char* label, GuestAddr info, u32 size);
    void print_line(GuestAddr ctx);
    void find_case(u64 inf, const char* section, const char* key);
    void iterate(const char* label, const std::string& cab);
    void print_version(const char* label, const std::string& path);

    bool x64_;
    int bits_;
    std::filesystem::path root_;
    std::string w_, win_ = "C:\\Windows";
    capsule::Vfs vfs_;
    capsule::Registry reg_backing_;
    HandleTable handles_;
    RegistryThunks registry_{reg_backing_, handles_};
    GuestMemory memory_;
    cpu::CpuState cpu1_, cpu2_;
    Marshaler m1_{memory_}, m2_{memory_};
    SetupapiGuestServices services_;
    DllExports exports_;
    u32 cursor_ = 0;
    std::vector<std::string> out_;
    u64 default_ctx_ = 0;
    bool skip_copy_ = false, needmedia_doit_ = false;
    int cab_mode_ = 0, cab_files_ = 0;
};

u64 Replay::callback(GuestAddr fn, u64 ctx, u32 note, u64 p1, u64 p2) {
    if (fn == kQueueCb) return queue_cb(ctx, note, p1, p2);
    if (fn == kCabCb) return cab_cb(ctx, note, p1, p2);
    if (fn == kNullCtxCb) {
        const u32 r = static_cast<u32>(nested("SetupDefaultQueueCallbackA", {0, note, p1, p2}));
        print(format("  nullctx note=%#x -> %u", note, r));
        return r;
    }
    ADD_FAILURE() << "unexpected callback " << fn;
    return 0;
}

u32 Replay::queue_cb(u64 ctx, u32 note, u64 p1, u64 p2) {
    const u32 p = x64_ ? 8 : 4;
    std::string line = format("  cb ctx=%s note=%#x", ctx == kCtx ? "ok" : "BAD", note);
    if (note <= 4) {
        line += format(" p1=%#llx p2=%#llx", static_cast<unsigned long long>(p1), static_cast<unsigned long long>(p2));
    } else if (note >= 5 && note <= 0xd) {
        const GuestAddr fp = static_cast<GuestAddr>(p1);
        line += format(" target=%s source=%s err=%u flags=%#x p2=%#llx", mask_ptr(ptr(fp)).c_str(),
                       note >= 0xb ? mask_ptr(ptr(fp + p)).c_str() : "-", u32_at(fp + 2 * p),
                       u32_at(fp + 2 * p + 4), static_cast<unsigned long long>(p2));
    } else if (note == 0xe) {
        const GuestAddr sm = static_cast<GuestAddr>(p1);
        line += format(" tag=%s desc=%s path=%s file=%s flags=%#x", mask_ptr(ptr(sm + p)).c_str(),
                       mask_ptr(ptr(sm + 2 * p)).c_str(), mask_ptr(ptr(sm + 3 * p)).c_str(),
                       mask_ptr(ptr(sm + 4 * p)).c_str(), u32_at(sm + 5 * p));
    } else {
        line += format(" p1=%#llx p2=%#llx", static_cast<unsigned long long>(p1), static_cast<unsigned long long>(p2));
    }
    u32 r;
    if (note == 0xb && skip_copy_) r = 2;
    else r = static_cast<u32>(nested("SetupDefaultQueueCallbackA", {default_ctx_, note, p1, p2}));
    if (note == 0xe && needmedia_doit_) {
        line += format(" (default %u)", r);
        r = 1;
    }
    print(line + format(" -> %u", r));
    return r;
}

u32 Replay::cab_cb(u64 ctx, u32 note, u64 p1, u64 p2) {
    const u32 p = x64_ ? 8 : 4;
    std::string line = format("  cab ctx=%s note=%#x", ctx == kCabCtx ? "ok" : "BAD", note);
    u32 r = 0;
    if (note == 0x10 || note == 0x12) {
        const GuestAddr ci = static_cast<GuestAddr>(p1);
        line += format(" path=%s file=%s disk=%s set=%u num=%u p2=%#llx", mask_ptr(ptr(ci)).c_str(),
                       mask_ptr(ptr(ci + p)).c_str(), mask_ptr(ptr(ci + 2 * p)).c_str(), u16_at(ci + 3 * p),
                       u16_at(ci + 3 * p + 2), static_cast<unsigned long long>(p2));
        if (cab_mode_ == 2) r = 0xdead;
    } else if (note == 0x11) {
        const GuestAddr fi = static_cast<GuestAddr>(p1);
        const std::string name = read_str(static_cast<GuestAddr>(ptr(fi)));
        line += format(" name=%s size=%u err=%u date=%#x time=%#x attr=%#x full=[%s] p2=%s", name.c_str(),
                       u32_at(fi + p), u32_at(fi + p + 4), u16_at(fi + p + 8), u16_at(fi + p + 10),
                       u16_at(fi + p + 12), mask(read_str(fi + p + 14)).c_str(),
                       mask_ptr(static_cast<GuestAddr>(p2)).c_str());
        if (cab_mode_ == 1) {
            set_last_error(0x1234);
            r = 0;
        } else if (cab_files_++ % 2 == 0) {
            (void)m1_.write_ansi_string(fi + p + 14, w_ + "\\it\\" + name);
            r = 1;
        } else {
            r = 2;
        }
    } else if (note == 0x13) {
        const GuestAddr fp = static_cast<GuestAddr>(p1);
        line += format(" target=%s source=%s err=%u flags=%#x p2=%#llx", mask_ptr(ptr(fp)).c_str(),
                       mask_ptr(ptr(fp + p)).c_str(), u32_at(fp + 2 * p), u32_at(fp + 2 * p + 4),
                       static_cast<unsigned long long>(p2));
    }
    print(line + format(" -> %#x", r));
    return r;
}

void Replay::install(u64 inf, const char* section, u32 flags, const std::string* root, u32 copy) {
    set_last_error(0xdead);
    const u64 ok = call("SetupInstallFromInfSectionA",
                        {0, inf, str(section), flags, 0, root ? str(*root) : 0, copy, kQueueCb, kCtx, 0, 0});
    print(format("install %s flags=%#x root=%s copy=%#x -> %d lasterr=%#x", section, flags,
                 root ? mask(*root).c_str() : "(null)", copy, static_cast<int>(ok), get_last_error()));
}

void Replay::open_case(const char* label, const std::string& rel, const char* cls, u32 style) {
    const std::string path = rel[0] == '!' ? rel.substr(1) : w_ + "\\src\\" + rel;
    const GuestAddr line = alloc(4);
    (void)memory_.write_u32(line, 0xbeef);
    set_last_error(0xdead);
    const u64 h = call("SetupOpenInfFileA", {str(path), cls ? str(cls) : 0, style, line});
    const u32 le = get_last_error();
    print(format("open %s class=%s style=%#x -> %s lasterr=%#x line=%#x", label, cls ? cls : "(null)", style,
                 h == invalid_handle() ? "INVALID" : "ok", le, u32_at(line)));
    if (h != invalid_handle()) call("SetupCloseInfFile", {h});
}

void Replay::print_info_buffer(const char* label, GuestAddr info, u32 size) {
    std::string line = format("%s style=%u count=%u bytes=%u data:", label, u32_at(info), u32_at(info + 4), size);
    for (u32 i = 8; i < size; ++i) {
        const u8 c = memory_.read_u8(info + i).value();
        if (c >= 0x20 && c < 0x7f && c != '\\') line.push_back(static_cast<char>(c));
        else line += format("\\x%02x", c);
    }
    print(line);
}

void Replay::print_line(GuestAddr ctx) {
    const GuestAddr text = alloc(512), req = alloc(4);
    set_last_error(0xdead);
    const u64 ok = call("SetupGetLineTextA", {ctx, 0, 0, 0, text, 512, req});
    const u32 le = get_last_error();
    const u32 n = static_cast<u32>(call("SetupGetFieldCount", {ctx}));
    std::string line = format("     line=%u text-> %d lasterr=%#x req=%u [%s] fields=%u:", u32_at(ctx + (x64_ ? 20 : 12)),
                              static_cast<int>(ok), le, u32_at(req), ok ? read_str(text).c_str() : "", n);
    for (u32 f = 0; f <= n + 1; ++f) {
        const GuestAddr field = alloc(256), fr = alloc(4), iv = alloc(4);
        set_last_error(0xdead);
        if (call("SetupGetStringFieldA", {ctx, f, field, 256, fr}) != 0) {
            line += format(" %u=[%s]/%u", f, read_str(field).c_str(), u32_at(fr));
        } else {
            line += format(" %u=!%#x", f, get_last_error());
        }
        (void)memory_.write_u32(iv, 0x5555);
        set_last_error(0xdead);
        if (call("SetupGetIntField", {ctx, f, iv}) != 0) line += format("(int %d)", static_cast<i32>(u32_at(iv)));
        else line += format("(int !%#x)", get_last_error());
    }
    print(line);
}

void Replay::find_case(u64 inf, const char* section, const char* key) {
    const GuestAddr ctx = alloc(24);
    set_last_error(0xdead);
    const u64 ok = call("SetupFindFirstLineA", {inf, section ? str(section) : 0, key ? str(key) : 0, ctx});
    print(format("find %s/%s -> %d lasterr=%#x", section ? section : "(null)", key ? key : "(null)",
                 static_cast<int>(ok), get_last_error()));
    if (ok == 0) return;
    int guard = 0;
    do {
        print_line(ctx);
        set_last_error(0xdead);
    } while (call("SetupFindNextLine", {ctx, ctx}) != 0 && ++guard < 20);
    print(format("  next-end lasterr=%#x", get_last_error()));
}

void Replay::iterate(const char* label, const std::string& cab) {
    set_last_error(0xdead);
    const u64 ok = call("SetupIterateCabinetA", {str(cab), 0, kCabCb, kCabCtx});
    print(format("iterate %s mode=%d -> %d lasterr=%#x", label, cab_mode_, static_cast<int>(ok), get_last_error()));
}

void Replay::print_version(const char* label, const std::string& path) {
    std::ifstream in(host_abs(path), std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto v = fixed_file_version(bytes);
    if (!v) {
        print(format("  %s version: none", label));
        return;
    }
    const u32 ms = static_cast<u32>(*v >> 32);
    print(format("  %s version: %u.%u", label, ms >> 16, ms & 0xffff));
}

std::vector<std::string> Replay::run() {
    print(format("bits=%d", bits_));
    std::string inf = probe_inf_text();
    for (usize at; (at = inf.find("%BITS%")) != std::string::npos;) inf.replace(at, 6, std::to_string(bits_));
    write_text("src\\probe.inf", inf);
    write_text("src\\loose.txt", "loose file\r\n");
    write_text("src\\noversion.inf", "[Foo]\r\nx=1\r\n");
    write_text("src\\badsection.inf", "[Version]\r\nSignature=\"$Windows NT$\"\r\n[Broken\r\nx=1\r\n");
    write_text("src\\chicago.inf", "[Version]\r\nSignature=\"$CHICAGO$\"\r\nAdvancedINF=2.0\r\n");
    write_text("src\\empty.inf", "");
    write_text("src\\oldnt.inf", "[Identification]\r\nOptionType=NetAdapter\r\n");
    {
        std::string uni = "\xff\xfe";
        for (char c : std::string("[Version]\r\nSignature=\"$Windows NT$\"\r\nClass=VitreoProbe\r\n")) {
            uni.push_back(c);
            uni.push_back('\0');
        }
        write_text("src\\unicode.inf", uni);
    }

    open_case("probe", "probe.inf", nullptr, INF_STYLE_WIN4);
    open_case("probe-class", "probe.inf", "VitreoProbe", INF_STYLE_WIN4);
    open_case("probe-class-case", "probe.inf", "vitreoprobe", INF_STYLE_WIN4);
    open_case("probe-wrongclass", "probe.inf", "Other", INF_STYLE_WIN4);
    open_case("probe-oldnt", "probe.inf", nullptr, INF_STYLE_OLDNT);
    open_case("probe-both", "probe.inf", nullptr, INF_STYLE_OLDNT | INF_STYLE_WIN4);
    open_case("probe-style0", "probe.inf", nullptr, 0);
    open_case("missing", "nope.inf", nullptr, INF_STYLE_WIN4);
    open_case("relative", "!probe.inf", nullptr, INF_STYLE_WIN4);
    open_case("noversion", "noversion.inf", nullptr, INF_STYLE_WIN4);
    open_case("badsection", "badsection.inf", nullptr, INF_STYLE_WIN4);
    open_case("chicago", "chicago.inf", nullptr, INF_STYLE_WIN4);
    open_case("empty", "empty.inf", nullptr, INF_STYLE_WIN4);
    open_case("oldnt-file-win4", "oldnt.inf", nullptr, INF_STYLE_WIN4);
    open_case("oldnt-file-oldnt", "oldnt.inf", nullptr, INF_STYLE_OLDNT);
    open_case("unicode", "unicode.inf", "VitreoProbe", INF_STYLE_WIN4);

    std::string path = w_ + "\\src\\probe.inf";
    const u64 h = call("SetupOpenInfFileA", {str(path), 0, INF_STYLE_WIN4, 0});
    {
        const GuestAddr buf = alloc(4096), req = alloc(4);
        (void)memory_.write_u32(req, 0xbeef);
        set_last_error(0xdead);
        u64 ok = call("SetupGetInfInformationA", {h, 1, 0, 0, req});
        print(format("info hinf size-query -> %d lasterr=%#x req=%u", static_cast<int>(ok), get_last_error(), u32_at(req)));
        const u32 need = u32_at(req);
        set_last_error(0xdead);
        ok = call("SetupGetInfInformationA", {h, 1, buf, need - 1, req});
        print(format("info hinf short -> %d lasterr=%#x req=%u", static_cast<int>(ok), get_last_error(), u32_at(req)));
        set_last_error(0xdead);
        ok = call("SetupGetInfInformationA", {h, 1, buf, 4096, req});
        print(format("info hinf full -> %d lasterr=%#x req=%u", static_cast<int>(ok), get_last_error(), u32_at(req)));
        print_info_buffer("  hinf", buf, u32_at(req));
        {
            const GuestAddr ver = alloc(256), vr = alloc(4);
            set_last_error(0xdead);
            ok = call("SetupQueryInfVersionInformationA", {buf, 0, str("Provider"), ver, 256, vr});
            print(format("  query-version Provider -> %d lasterr=%#x req=%u value=[%s]", static_cast<int>(ok),
                         get_last_error(), u32_at(vr), ok ? read_str(ver).c_str() : ""));
            ok = call("SetupQueryInfFileInformationA", {buf, 0, ver, 256, vr});
            print(format("  query-file -> %d req=%u value=[%s]", static_cast<int>(ok), u32_at(vr),
                         ok ? mask(read_str(ver)).c_str() : ""));
        }
        set_last_error(0xdead);
        ok = call("SetupGetInfInformationA", {str(path), 2, buf, 4096, req});
        print(format("info name -> %d lasterr=%#x req=%u", static_cast<int>(ok), get_last_error(), u32_at(req)));
        if (ok) print_info_buffer("  name", buf, u32_at(req));
        path = w_ + "\\src\\chicago.inf";
        ok = call("SetupGetInfInformationA", {str(path), 2, buf, 4096, req});
        print(format("info chicago -> %d lasterr=%#x req=%u", static_cast<int>(ok), get_last_error(), u32_at(req)));
        if (ok) print_info_buffer("  chicago", buf, u32_at(req));
        path = w_ + "\\src\\nope.inf";
        set_last_error(0xdead);
        ok = call("SetupGetInfInformationA", {str(path), 2, buf, 4096, req});
        print(format("info missing -> %d lasterr=%#x", static_cast<int>(ok), get_last_error()));
        set_last_error(0xdead);
        ok = call("SetupGetInfInformationA", {h, 99, buf, 4096, req});
        print(format("info badsearch -> %d lasterr=%#x", static_cast<int>(ok), get_last_error()));
    }

    find_case(h, "Version", nullptr);
    find_case(h, "SourceDisksNames", "2");
    find_case(h, "Loose_Files", nullptr);
    find_case(h, "DxStyle", nullptr);
    find_case(h, "NoSuch", nullptr);
    find_case(h, "Empty_Install", nullptr);
    find_case(h, "Version", "NoKey");
    find_case(h, "version", "signature");
    {
        const GuestAddr ctx = alloc(24), next = alloc(24), text = alloc(64), req = alloc(4);
        set_last_error(0xdead);
        u64 ok = call("SetupGetLineTextA", {0, h, str("Strings"), str("Prov"), 0, 0, req});
        print(format("linetext by key size-query -> %d lasterr=%#x req=%u", static_cast<int>(ok), get_last_error(), u32_at(req)));
        set_last_error(0xdead);
        ok = call("SetupGetLineTextA", {0, h, str("Strings"), str("Prov"), text, 4, req});
        print(format("linetext short -> %d lasterr=%#x req=%u", static_cast<int>(ok), get_last_error(), u32_at(req)));
        set_last_error(0xdead);
        ok = call("SetupGetLineTextA", {0, h, str("Strings"), str("Nope"), text, 64, req});
        print(format("linetext missing key -> %d lasterr=%#x", static_cast<int>(ok), get_last_error()));
        call("SetupFindFirstLineA", {h, str("Version"), 0, ctx});
        set_last_error(0xdead);
        ok = call("SetupFindNextMatchLineA", {ctx, str("Provider"), next});
        print(format("nextmatch Provider -> %d lasterr=%#x line=%u", static_cast<int>(ok), get_last_error(),
                     u32_at(next + (x64_ ? 20 : 12))));
        set_last_error(0xdead);
        ok = call("SetupFindNextMatchLineA", {ctx, str("Nope"), next});
        print(format("nextmatch Nope -> %d lasterr=%#x", static_cast<int>(ok), get_last_error()));
        set_last_error(0xdead);
        i32 lc = static_cast<i32>(call("SetupGetLineCountA", {h, str("DxStyle")}));
        print(format("linecount DxStyle -> %d lasterr=%#x", lc, get_last_error()));
        set_last_error(0xdead);
        lc = static_cast<i32>(call("SetupGetLineCountA", {h, str("NoSuch")}));
        print(format("linecount NoSuch -> %d lasterr=%#x", lc, get_last_error()));
        set_last_error(0xdead);
        ok = call("SetupGetLineByIndexA", {h, str("DxStyle"), 2, ctx});
        print(format("byindex 2 -> %d lasterr=%#x line=%u", static_cast<int>(ok), get_last_error(),
                     u32_at(ctx + (x64_ ? 20 : 12))));
        set_last_error(0xdead);
        ok = call("SetupGetLineByIndexA", {h, str("DxStyle"), 99, ctx});
        print(format("byindex 99 -> %d lasterr=%#x", static_cast<int>(ok), get_last_error()));
        print(format("ctx-size %u", x64_ ? 24u : 16u));
    }

    const std::string dest = win_ + "\\Temp\\vprobe" + std::to_string(bits_);
    const std::string src = w_ + "\\src";
    std::filesystem::create_directories(host_abs(dest));
    default_ctx_ = call("SetupInitDefaultQueueCallbackEx", {0, invalid_handle(), 0, 0, 0});
    print(format("default-ctx %s", default_ctx_ ? "ok" : "NULL"));

    install(h, "Loose_Install", SPINST_FILES, &src, 0);
    list_dir("dest", dest);
    install(h, "Loose_Install", SPINST_FILES, nullptr, 0);
    list_dir("dest", dest);
    install(h, "NoOver_Install", SPINST_FILES, &src, 0);
    install(h, "Sub_Install", SPINST_FILES, &src, 0);
    list_dir("dest/sub", dest + "\\sub");
    install(h, "Both_Install", SPINST_FILES, &src, 0);
    clear_dir(dest);
    install(h, "Direct_Install", SPINST_FILES, &src, 0);
    list_dir("dest", dest);
    clear_dir(dest);
    install(h, "Cab_Install", SPINST_FILES, &src, 0);
    list_dir("dest", dest);
    list_dir("src", src);
    clear_dir(dest);
    skip_copy_ = true;
    install(h, "Loose_Install", SPINST_FILES, &src, 0);
    skip_copy_ = false;
    list_dir("dest", dest);
    install(h, "Loose_Install", SPINST_FILES, &src, 0);
    install(h, "Del_Install", SPINST_FILES, &src, 0);
    list_dir("dest", dest);
    install(h, "Loose_Install", SPINST_REGISTRY, &src, 0);
    list_dir("dest", dest);
    install(h, "Empty_Install", SPINST_ALL, &src, 0);
    install(h, "NoSuch_Install", SPINST_FILES, &src, 0);
    install(h, "NoSuch_Install", SPINST_REGISTRY, &src, 0);
    install(h, "Loose_Install", 0, &src, 0);
    install(h, "Reg_Install", SPINST_REGISTRY, &src, 0);
    {
        u64 key = 0;
        u32 r = registry_.reg_open_key_ex_a(0x80000001u, "Software\\VitreoSetupProbe", 0, 0x20019, &key);
        print(format("  reg open -> %d", static_cast<int>(r)));
        if (r == 0) {
            u8 v[64] = {};
            u32 type = 0, size = sizeof v;
            r = registry_.reg_query_value_ex_a(key, "Str", &type, v, &size);
            print(format("  reg Str -> %d type=%u size=%u [%s]", static_cast<int>(r), type, size, reinterpret_cast<char*>(v)));
            u32 d = 0;
            size = 4;
            r = registry_.reg_query_value_ex_a(key, "Dw", &type, reinterpret_cast<u8*>(&d), &size);
            print(format("  reg Dw -> %d type=%u size=%u %#x", static_cast<int>(r), type, size, d));
            (void)registry_.reg_close_key(key);
        }
    }
    install(h, "RegDel_Install", SPINST_REGISTRY, &src, 0);
    {
        u64 key = 0;
        const u32 r = registry_.reg_open_key_ex_a(0x80000001u, "Software\\VitreoSetupProbe", 0, 0x20019, &key);
        print(format("  reg open after DelReg -> %d", static_cast<int>(r)));
        if (r == 0) (void)registry_.reg_close_key(key);
    }
    skip_copy_ = true;
    install(h, "Sys_Install", SPINST_FILES, &src, 0);
    clear_dir(dest);
    install(h, "Cont_Install", SPINST_FILES, &src, 0);
    skip_copy_ = false;
    set_last_error(0xdead);
    {
        const u64 ok = call("SetupInstallFromInfSectionA",
                            {0, h, str("Loose_Install"), SPINST_FILES, 0, str(src), 0, kNullCtxCb, 0, 0, 0});
        print(format("install nullctx -> %d lasterr=%#x", static_cast<int>(ok), get_last_error()));
        list_dir("dest", dest);
    }
    {
        const std::string ver = dest + "\\ver.dll";
        static const u32 styles[] = {0x4, 0x144, 0x10000, 0x2000, 0x8, 0x40, 0x100, 0x1000};
        for (u32 style : styles) {
            std::error_code ec;
            std::filesystem::remove(host_abs(ver), ec);
            print(format("-- style %#x, target absent", style));
            install(h, "V1_Install", SPINST_FILES, &src, style);
            print_version("dest", ver);
            print(format("-- style %#x, target v2, source v1", style));
            install(h, "V2_Install", SPINST_FILES, &src, 0);
            install(h, "V1_Install", SPINST_FILES, &src, style);
            print_version("dest", ver);
            print(format("-- style %#x, target v1, source v1", style));
            std::filesystem::remove(host_abs(ver), ec);
            install(h, "V1_Install", SPINST_FILES, &src, 0);
            install(h, "V1_Install", SPINST_FILES, &src, style);
            print_version("dest", ver);
        }
        std::error_code ec;
        std::filesystem::remove(host_abs(ver), ec);
    }
    write_text("src\\flagloose.txt", "flag loose\r\n");
    write_text("src\\flag10loose.txt", "flag10 loose\r\n");
    clear_dir(dest);
    install(h, "FlagLoose_Install", SPINST_FILES, &src, 0);
    install(h, "FlagCab_Install", SPINST_FILES, &src, 0);
    install(h, "Flag10_Install", SPINST_FILES, &src, 0);
    install(h, "Flag10Loose_Install", SPINST_FILES, &src, 0);
    list_dir("dest", dest);
    needmedia_doit_ = true;
    install(h, "FlagLoose_Install", SPINST_FILES, &src, 0);
    install(h, "Flag10Loose_Install", SPINST_FILES, &src, 0);
    needmedia_doit_ = false;
    list_dir("dest", dest);
    install(h, "Missing_Install", SPINST_FILES, &src, 0);
    list_dir("dest", dest);
    call("SetupTermDefaultQueueCallback", {default_ctx_});
    call("SetupCloseInfFile", {h});

    const std::string cab = w_ + "\\src\\probe.cab", it = w_ + "\\it";
    std::filesystem::create_directories(host_abs(it));
    cab_mode_ = 0;
    iterate("probe", cab);
    list_dir("it", it);
    cab_mode_ = 1;
    iterate("probe", cab);
    cab_mode_ = 2;
    iterate("probe", cab);
    cab_mode_ = 0;
    iterate("text", w_ + "\\src\\loose.txt");
    iterate("missing", w_ + "\\src\\nope.cab");

    for (const char* name : {"SetupQueryRegisteredOsComponent", "SetupRegisterOsComponent",
                             "SetupUnRegisterOsComponent", "SetupQueryRegisteredOsComponentsOrder",
                             "SetupEnumInfSectionsA"}) {
        const bool present = std::any_of(exports_.entries.begin(), exports_.entries.end(),
                                         [name](const ExportEntry& e) { return e.name == name; });
        print(format("export %s %s", name, present ? "present" : "absent"));
    }
    return out_;
}

/// Zero the INF FILETIME (bytes 4..11 of VersionData) in a printed
/// SP_INF_INFORMATION dump, neutralise pointers in the notifications whose
/// Param1 is a record address, and the one last error Windows leaves
/// unspecified.
std::string normalise(std::string line) {
    const auto data = line.find(" data:");
    if (data != std::string::npos) {
        std::vector<std::string> units;
        for (usize i = data + 6; i < line.size();) {
            if (line[i] == '\\' && i + 3 < line.size() && line[i + 1] == 'x') {
                units.push_back(line.substr(i, 4));
                i += 4;
            } else {
                units.push_back(line.substr(i, 1));
                ++i;
            }
        }
        for (usize u = 4; u < 12 && u < units.size(); ++u) units[u] = "\\x00";
        std::string rebuilt = line.substr(0, data + 6);
        for (const auto& u : units) rebuilt += u;
        line = rebuilt;
    }
    // The by-name SetupGetInfInformation leaves whatever its internal file
    // probing last set (0 on one oracle run, 0xce on the next): no contract.
    if (line.rfind("info name -> 1 ", 0) == 0) {
        line = std::regex_replace(line, std::regex(R"(lasterr=0x[0-9a-f]+|lasterr=0)"), "lasterr=*");
    }
    static const std::regex pointer_note(R"(note=(0x14|0x20000|0x40000) p1=0x[0-9a-f]+ p2=0x[0-9a-f]+|note=(0x14|0x20000|0x40000) p1=0x[0-9a-f]+)");
    line = std::regex_replace(line, pointer_note, "note=$1$2 p1=PTR");
    return line;
}

std::string normalise_expected(std::string line, bool is64) {
    for (usize at; (at = line.find("C:\\WINDOWS")) != std::string::npos;) line.replace(at, 10, "C:\\Windows");
    if (!is64 && line.find("cab ctx=ok note=0x13") != std::string::npos) {
        line = std::regex_replace(line, std::regex(R"(flags=0x[0-9a-f]+)"), "flags=0");
    }
    if (!is64 && line.find("cab ctx=ok note=0x11") != std::string::npos) {
        line = std::regex_replace(line, std::regex(R"(full=\[[^\]]*\])"), "full=[]");
    }
    return normalise(line);
}

/// Measured behaviour Vitreo does not implement; each fails loudly
/// (UNIMPLEMENTED + ERROR_CALL_NOT_IMPLEMENTED) instead. A gap names the
/// install section whose whole block (its callback lines and result line)
/// is excluded, or an exact output line prefix.
struct Gap {
    const char* install;
    const char* line_prefix;
    const char* why;
};
constexpr Gap kGaps[] = {
    {"install NoOver_Install ", nullptr,
     "COPYFLG_FORCE_FILE_IN_USE (0x8): Windows queues the copy for reboot (FILEOPDELAYED)"},
    {nullptr, "export SetupEnumInfSectionsA",
     "SetupEnumInfSectionsA is not exported yet"},
};

std::vector<std::string> strip_gaps(std::vector<std::string> lines) {
    for (const auto& gap : kGaps) {
        if (gap.install != nullptr) {
            for (usize i = 0; i < lines.size(); ++i) {
                if (lines[i].rfind(gap.install, 0) != 0) continue;
                usize first = i;
                while (first > 0 && lines[first - 1].rfind("  cb ", 0) == 0) --first;
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(first),
                            lines.begin() + static_cast<std::ptrdiff_t>(i + 1));
                break;
            }
        } else {
            lines.erase(std::remove_if(lines.begin(), lines.end(),
                                       [&](const std::string& l) { return l.rfind(gap.line_prefix, 0) == 0; }),
                        lines.end());
        }
    }
    return lines;
}

void replay_matches_windows(bool is64) {
    std::vector<std::string> expected;
    {
        std::ifstream in(kHere / "oracle" / (is64 ? "setupapi_inf_probe64.expected" : "setupapi_inf_probe32.expected"));
        ASSERT_TRUE(in.good());
        for (std::string line; std::getline(in, line);) expected.push_back(normalise_expected(line, is64));
    }
    Replay replay(is64);
    std::vector<std::string> actual = replay.run();
    for (auto& line : actual) line = normalise(line);
    expected = strip_gaps(expected);
    actual = strip_gaps(actual);
    const usize n = std::max(expected.size(), actual.size());
    int mismatches = 0;
    for (usize i = 0; i < n && mismatches < 15; ++i) {
        const std::string e = i < expected.size() ? expected[i] : "<none>";
        const std::string a = i < actual.size() ? actual[i] : "<none>";
        if (e != a) {
            ADD_FAILURE() << "line " << i + 1 << "\n  windows: " << e << "\n  vitreo:  " << a;
            ++mismatches;
        }
    }
    EXPECT_EQ(expected.size(), actual.size());
}

}  // namespace

TEST(SetupapiInfOracle, ReplayMatchesWindowsX64) { replay_matches_windows(true); }
TEST(SetupapiInfOracle, ReplayMatchesWindowsX86) { replay_matches_windows(false); }
