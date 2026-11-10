/// @file test_profile_api.cpp
/// @brief kernel32 INI profile family (Get/WritePrivateProfile*, Get/WriteProfile*)
///        against Windows' own answers.
///
/// ProfileApiReplay runs tools/oracle_probe/profile_api_probe.cpp's exact call
/// sequence through the Vitreo exports (x86 stdcall and x64 fastcall argument
/// layouts), prints every line the probe prints in the probe's format, and
/// compares the whole output with tests/oracle_data/profile_api_probe.<arch>.expected
/// (Windows 11). Three probe rows are access violations on Windows; Vitreo
/// fails them with 0 / ERROR_INVALID_PARAMETER instead (documented in
/// .docs/contracts/kernel32.md § Profile (INI) files), and the replay checks
/// that answer before printing the native row.

#include "kernel32_cpuz_fixture.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <winapi/kernel32/error.hpp>

#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

namespace {

namespace fs = std::filesystem;

class ProfileApiTest : public ::testing::Test {
public:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kData, 0x20000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
        marshaler.attach_cpu(cpu);
        mgr.set_handle_table(&handles);
        mgr.set_sync_manager(&sync);
        mgr.set_thread_manager(&threads);
        root = fs::temp_directory_path() /
               ("vitreo_profile_api_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(fs::create_directories(root / "Windows"));
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string()))
                        .is_ok());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void set_mode(bool x64) {
        m_x64 = x64;
        if (x64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack + 0x400;
    }

    // ---- guest strings (bump arena, reset per call) ----
    GuestAddr alloc(usize bytes) {
        const GuestAddr at = m_next;
        m_next += static_cast<GuestAddr>((bytes + 15) & ~usize{15});
        return at;
    }
    GuestAddr bytes(const void* data, usize size) {
        const GuestAddr at = alloc(size);
        EXPECT_TRUE(memory.write_bytes(at, static_cast<const u8*>(data), size).is_ok());
        return at;
    }
    GuestAddr A(const char* s) { return s ? bytes(s, std::strlen(s) + 1) : 0; }
    GuestAddr A(const std::string& s) { return bytes(s.c_str(), s.size() + 1); }
    GuestAddr W(const char16_t* s) {
        if (!s) return 0;
        usize n = 0;
        while (s[n]) ++n;
        return bytes(s, (n + 1) * 2);
    }
    GuestAddr W(const std::u16string& s) { return bytes(s.c_str(), (s.size() + 1) * 2); }

    /// Call one export with the given arguments in the current ABI and
    /// return EAX.
    u32 call(void (Kernel32ExtManager::*fn)(Marshaler&), std::initializer_list<u64> args) {
        const u64 sp = cpu.gpr.rsp;
        usize i = 0;
        for (u64 a : args) {
            if (m_x64) {
                switch (i) {
                    case 0: cpu.gpr.rcx = a; break;
                    case 1: cpu.gpr.rdx = a; break;
                    case 2: cpu.gpr.r8 = a; break;
                    case 3: cpu.gpr.r9 = a; break;
                    default: EXPECT_TRUE(memory.write_u64(sp + 0x28 + (i - 4) * 8, a).is_ok());
                }
            } else {
                EXPECT_TRUE(memory.write_u32(sp + 4 + i * 4, static_cast<u32>(a)).is_ok());
            }
            ++i;
        }
        (mgr.*fn)(marshaler);
        m_next = kArena;
        return static_cast<u32>(cpu.gpr.rax);
    }

    static constexpr GuestAddr kData = 0x10000;
    static constexpr GuestAddr kBuf = kData;            // 0x1000 bytes
    static constexpr GuestAddr kArena = kData + 0x2000;
    static constexpr GuestAddr kStack = 0x80000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    capsule::Vfs vfs;
    HandleTable handles;
    SyncManager sync{handles};
    ThreadManager threads{handles};
    Kernel32ExtManager mgr{memory, vfs};
    fs::path root;
    bool m_x64 = false;
    GuestAddr m_next = kArena;
};

using K = Kernel32ExtManager;

// =============================================================================
// Replay of tools/oracle_probe/profile_api_probe.cpp
// =============================================================================

class ProfileProbeReplay {
public:
    ProfileProbeReplay(ProfileApiTest& t, bool x64)
        : t(t), bits(x64 ? "x64" : "x86"), dir(t.root / "probe" / (x64 ? "x64" : "x86")) {
        fs::create_directories(dir);
        g_dir = std::string("C:\\probe\\") + bits + "\\";
    }

    std::vector<std::string> run();

private:
    ProfileApiTest& t;
    const char* bits;
    fs::path dir;
    std::string g_dir;
    std::vector<std::string> out;

    void line(const std::string& key, const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, args);
        va_end(args);
        out.push_back(key + "=" + buf);
    }
    std::string P(const char* name) const { return g_dir + name; }
    std::u16string PW(const char* name) const {
        std::u16string w;
        for (char c : P(name)) w += static_cast<char16_t>(static_cast<unsigned char>(c));
        return w;
    }
    GuestAddr fileA(const char* name) { return name ? t.A(P(name)) : 0; }
    GuestAddr fileW(const char* name) { return name ? t.W(PW(name)) : 0; }

    void write_raw(const char* name, const void* data, usize size) {
        std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    void write_text(const char* name, const char* text) { write_raw(name, text, std::strlen(text)); }
    void remove(const char* name) {
        std::error_code ec;
        fs::remove(dir / name, ec);
    }

    void hexdump(const std::string& key, const unsigned char* b, usize size) {
        if (size == 0) { line(key, "(empty)"); return; }
        for (usize row = 0; row < size; row += 16) {
            std::string s;
            char tmp[8];
            for (usize i = row; i < row + 16 && i < size; ++i) {
                std::snprintf(tmp, sizeof tmp, i == row ? "%02x" : " %02x", b[i]);
                s += tmp;
            }
            char k[16];
            std::snprintf(k, sizeof k, ".%04zx", row);
            out.push_back(key + k + "=" + s);
        }
    }
    void dump_file(const std::string& key, const char* name) {
        std::ifstream f(dir / name, std::ios::binary);
        if (!f) { line(key + ".file", "(absent) error=2"); return; }
        std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
        if (b.size() > 4096) b.resize(4096);
        line(key + ".file_size", "%zu", b.size());
        hexdump(key + ".file", b.data(), b.size());
    }

    static std::string escA(const u8* s, usize n) {
        std::string o;
        for (usize i = 0; i < n; ++i) {
            const unsigned c = s[i];
            if (c >= 0x20 && c < 0x7f && c != '\\') { o += static_cast<char>(c); continue; }
            char tmp[8];
            std::snprintf(tmp, sizeof tmp, "\\x%02x", c);
            o += tmp;
        }
        return o;
    }
    static std::string escW(const u16* s, usize n) {
        std::string o;
        for (usize i = 0; i < n; ++i) {
            const unsigned c = s[i];
            if (c >= 0x20 && c < 0x7f && c != '\\') { o += static_cast<char>(c); continue; }
            char tmp[10];
            std::snprintf(tmp, sizeof tmp, "\\u%04x", c);
            o += tmp;
        }
        return o;
    }

    void prefill(bool wide) {
        std::vector<u8> cc(wide ? 1024 : 512, 0xcc);
        (void)t.memory.write_bytes(ProfileApiTest::kBuf, cc.data(), cc.size());
    }
    void reportA(const std::string& key, u32 ret, u32 err, u32 size) {
        line(key + ".ret", "%u", ret);
        line(key + ".err", "0x%x", err);
        u8 buf[512];
        (void)t.memory.read_bytes(ProfileApiTest::kBuf, buf, sizeof buf);
        const u32 n = ret < size ? ret : size;
        line(key + ".str", "\"%s\"", escA(buf, n).c_str());
        u32 show = ret + 2 < size ? ret + 2 : size;
        if (show > 96) show = 96;
        line(key + ".raw", "\"%s\"", escA(buf, show).c_str());
    }
    void reportW(const std::string& key, u32 ret, u32 err, u32 size) {
        line(key + ".ret", "%u", ret);
        line(key + ".err", "0x%x", err);
        u16 buf[512];
        for (u32 i = 0; i < 512; ++i) buf[i] = t.memory.read_u16(ProfileApiTest::kBuf + i * 2).value();
        const u32 n = ret < size ? ret : size;
        line(key + ".str", "\"%s\"", escW(buf, n).c_str());
        u32 show = ret + 2 < size ? ret + 2 : size;
        if (show > 96) show = 96;
        line(key + ".raw", "\"%s\"", escW(buf, show).c_str());
    }

    void getA(const std::string& key, const char* sec, const char* k, const char* def,
              const char* file, u32 size = 128) {
        prefill(false);
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_GetPrivateProfileStringA,
                             {t.A(sec), t.A(k), t.A(def), ProfileApiTest::kBuf, size, fileA(file)});
        reportA(key, r, get_last_error(), size);
    }
    void getW(const std::string& key, const char16_t* sec, const char16_t* k, const char16_t* def,
              const char* file, u32 size = 128) {
        prefill(true);
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_GetPrivateProfileStringW,
                             {t.W(sec), t.W(k), t.W(def), ProfileApiTest::kBuf, size, fileW(file)});
        reportW(key, r, get_last_error(), size);
    }
    void intA(const std::string& key, const char* sec, const char* k, i32 def, const char* file) {
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_GetPrivateProfileIntA,
                             {t.A(sec), t.A(k), static_cast<u32>(def), fileA(file)});
        const u32 e = get_last_error();
        line(key + ".ret", "%u (signed %d)", r, static_cast<int>(r));
        line(key + ".err", "0x%x", e);
    }
    void sectionA(const std::string& key, const char* sec, const char* file, u32 size) {
        prefill(false);
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_GetPrivateProfileSectionA,
                             {t.A(sec), ProfileApiTest::kBuf, size, fileA(file)});
        reportA(key, r, get_last_error(), size);
    }
    void namesA(const std::string& key, const char* file, u32 size) {
        prefill(false);
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_GetPrivateProfileSectionNamesA,
                             {ProfileApiTest::kBuf, size, fileA(file)});
        reportA(key, r, get_last_error(), size);
    }
    void writeA(const std::string& key, const char* sec, const char* k, const char* v,
                const char* file) {
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_WritePrivateProfileStringA,
                             {t.A(sec), t.A(k), t.A(v), fileA(file)});
        const u32 e = get_last_error();
        line(key + ".ret", "%d", static_cast<int>(r));
        line(key + ".err", "0x%x", e);
    }
    void writeW(const std::string& key, const char16_t* sec, const char16_t* k,
                const char16_t* v, const char* file) {
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_WritePrivateProfileStringW,
                             {t.W(sec), t.W(k), t.W(v), fileW(file)});
        const u32 e = get_last_error();
        line(key + ".ret", "%d", static_cast<int>(r));
        line(key + ".err", "0x%x", e);
    }
    void child(const char* mode);
    void struct_read(const std::string& key, const char* k, u32 size, bool dump = true) {
        std::vector<u8> cc(8, 0xcc);
        (void)t.memory.write_bytes(ProfileApiTest::kBuf, cc.data(), cc.size());
        set_last_error(0xdeadbeef);
        const u32 ok = t.call(&K::impl_GetPrivateProfileStructA,
                              {t.A("S"), t.A(k), ProfileApiTest::kBuf, size, fileA("st.ini")});
        const u32 err = get_last_error();
        line(key + ".ret", "%d", static_cast<int>(ok));
        line(key + ".err", "0x%x", err);
        if (!dump) return;
        u8 o[8];
        (void)t.memory.read_bytes(ProfileApiTest::kBuf, o, 8);
        hexdump(key + ".out", o, 8);
    }
};

constexpr char kRead[] =
    "[Sec]\r\n"
    "plain=value\r\n"
    "spaced =   padded value   \r\n"
    "squote='single'\r\n"
    "dquote=\"double\"\r\n"
    "mixquote=\"half\r\n"
    "inner=a\"b\"c\r\n"
    "sq2=' spaced in quotes '\r\n"
    "eq=a=b=c\r\n"
    "semi=val ; comment\r\n"
    "hash=val # comment\r\n"
    ";commented=1\r\n"
    "#hashkey=2\r\n"
    "empty=\r\n"
    "noequals\r\n"
    "  lead key  =x\r\n"
    "\tTabKey\t=\ttabbed\t\r\n"
    "DUP=first\r\n"
    "dup=second\r\n"
    "\r\n"
    "[ Spaced Sec ]\r\n"
    "k=v\r\n"
    "[sec]\r\n"
    "k2=fromsecondsec\r\n"
    "plain=second-plain\r\n"
    "[Ints]\r\n"
    "dec=42\r\n"
    "lead=   17\r\n"
    "neg=-5\r\n"
    "hex=0x10\r\n"
    "garbage=12abc\r\n"
    "emptyint=\r\n"
    "big=4294967295\r\n"
    "over=99999999999\r\n"
    "plus=+7\r\n"
    "space_in=1 2\r\n"
    "[Long]\r\n"
    "v=0123456789ABCDEFGHIJ\r\n"
    "  [Indented]\r\n"
    "ik=iv\r\n"
    "[Unterminated\r\n"
    "uk=uv\r\n"
    "[Trail]junk\r\n"
    "tk=tv\r\n"
    "[]\r\n"
    "ek=ev\r\n";

void ProfileProbeReplay::child(const char* mode) {
    const std::string m = mode;
    const auto file = [&] { return fileA("r.ini"); };
    const auto wfile = [&] { return fileW("r.ini"); };
    // Windows answers these three with an access violation; Vitreo returns
    // 0 / ERROR_INVALID_PARAMETER (documented divergence).
    const bool faults = m == "emptysecA" || m == "nullbufA10" || m == "nullbufW10";
    prefill(m == "emptysecW" || m == "size0W");
    set_last_error(0xdeadbeef);
    u32 r = 0;
    if (m == "nullbufA0") r = t.call(&K::impl_GetPrivateProfileStringA, {t.A("Sec"), t.A("plain"), t.A("d"), 0, 0, file()});
    else if (m == "nullbufA10") r = t.call(&K::impl_GetPrivateProfileStringA, {t.A("Sec"), t.A("plain"), t.A("d"), 0, 10, file()});
    else if (m == "nullbufW0") r = t.call(&K::impl_GetPrivateProfileStringW, {t.W(u"Sec"), t.W(u"plain"), t.W(u"d"), 0, 0, wfile()});
    else if (m == "nullbufW10") r = t.call(&K::impl_GetPrivateProfileStringW, {t.W(u"Sec"), t.W(u"plain"), t.W(u"d"), 0, 10, wfile()});
    else if (m == "nullsecbufA") r = t.call(&K::impl_GetPrivateProfileSectionA, {t.A("Sec"), 0, 0, file()});
    else if (m == "nullnamesbufA") r = t.call(&K::impl_GetPrivateProfileSectionNamesA, {0, 0, file()});
    else if (m == "emptysecA") r = t.call(&K::impl_GetPrivateProfileStringA, {t.A(""), t.A("ek"), t.A("def"), ProfileApiTest::kBuf, 16, file()});
    else if (m == "emptysecW") {
        r = t.call(&K::impl_GetPrivateProfileStringW, {t.W(u""), t.W(u"ek"), t.W(u"def"), ProfileApiTest::kBuf, 16, wfile()});
        u16 b[16];
        for (u32 i = 0; i < 16; ++i) b[i] = t.memory.read_u16(ProfileApiTest::kBuf + i * 2).value();
        out.push_back("child." + m + ".str=\"" + escW(b, r < 16 ? r : 16) + "\"");
    } else if (m == "size0W") {
        r = t.call(&K::impl_GetPrivateProfileStringW, {t.W(u"Sec"), t.W(u"plain"), t.W(u"d"), ProfileApiTest::kBuf, 0, wfile()});
    }
    const u32 e = get_last_error();
    if (faults) {
        EXPECT_EQ(r, 0u) << mode;
        EXPECT_EQ(e, error_codes::ERROR_INVALID_PARAMETER) << mode;
        line("child." + m + ".exit", "0x%x", (r == 0 && e == 87) ? 0xc0000005u : 0u);
        return;
    }
    line("child." + m + ".ret", "%u", r);
    line("child." + m + ".err", "0x%x", e);
    line("child." + m + ".exit", "0x0");
}

std::vector<std::string> ProfileProbeReplay::run() {
    line("probe", "profile_api arch=%s acp=%u", bits, 1252u);
    write_text("r.ini", kRead);
    remove("absent.ini");

    struct { const char *sec, *key, *label; } reads[] = {
        {"Sec", "plain", "plain"}, {"SEC", "PLAIN", "case_both_upper"}, {"sec", "plain", "lower_sec"},
        {"Sec", "spaced", "spaced"}, {"Sec", "spaced ", "spaced_trailing_space_key"},
        {"Sec", " spaced", "spaced_leading_space_key"},
        {"Sec", "squote", "squote"}, {"Sec", "dquote", "dquote"}, {"Sec", "mixquote", "mixquote"},
        {"Sec", "inner", "inner_quotes"}, {"Sec", "sq2", "sq2"}, {"Sec", "eq", "eq"},
        {"Sec", "semi", "semi"}, {"Sec", "hash", "hash"}, {"Sec", ";commented", "semicolon_key"},
        {"Sec", "commented", "commented_key"}, {"Sec", "#hashkey", "hash_key"}, {"Sec", "empty", "empty"},
        {"Sec", "noequals", "noequals"}, {"Sec", "lead key", "lead_key"}, {"Sec", "  lead key  ", "lead_key_spaced"},
        {"Sec", "TabKey", "tab_key"}, {"Sec", "dup", "dup"}, {"Sec", "DUP", "DUP"}, {"Sec", "k2", "k2_from_dup_section"},
        {"Sec", "absent", "absent_key"}, {"Nope", "plain", "absent_section"},
        {"Spaced Sec", "k", "spaced_section"}, {" Spaced Sec ", "k", "spaced_section_padded_name"},
        {"Indented", "ik", "indented_section"}, {"Unterminated", "uk", "unterminated_section"},
        {"[Unterminated", "uk", "unterminated_section_bracket"}, {"Trail", "tk", "trailing_junk_section"},
    };
    for (auto& r : reads) getA(std::string("readA.") + r.label, r.sec, r.key, "def", "r.ini");
    getA("readA.absent_file", "Sec", "plain", "def", "absent.ini");
    getA("readA.default_trailing_spaces", "Sec", "absent", "def   ", "r.ini");
    getA("readA.default_leading_spaces", "Sec", "absent", "   def", "r.ini");
    getA("readA.default_quoted", "Sec", "absent", "\"def\"", "r.ini");
    getA("readA.default_null", "Sec", "absent", nullptr, "r.ini");
    getA("readA.default_null_absent_file", "Sec", "plain", nullptr, "absent.ini");
    getA("readA.default_empty", "Sec", "absent", "", "r.ini");

    for (u32 size : {21u, 20u, 10u, 2u, 1u})
        getA("truncA.value.size_" + std::to_string(size), "Long", "v", "def", "r.ini", size);
    for (u32 size : {10u, 3u, 2u, 1u})
        getA("truncA.default.size_" + std::to_string(size), "Long", "absent", "defaultvalue", "r.ini", size);
    for (u32 size : {512u, 20u, 9u, 3u, 2u, 1u}) {
        getA("enumA.section_names.size_" + std::to_string(size), nullptr, nullptr, "def", "r.ini", size);
        getA("enumA.key_names_Sec.size_" + std::to_string(size), "Sec", nullptr, "def", "r.ini", size);
    }
    getA("enumA.section_names.absent_file", nullptr, nullptr, "def", "absent.ini", 64);
    getA("enumA.key_names.absent_section", "Nope", nullptr, "def", "r.ini", 64);
    getA("enumA.section_names_with_key", nullptr, "plain", "def", "r.ini", 512);
    for (u32 size : {512u, 9u, 2u}) {
        getW("enumW.section_names.size_" + std::to_string(size), nullptr, nullptr, u"def", "r.ini", size);
        getW("enumW.key_names_Sec.size_" + std::to_string(size), u"Sec", nullptr, u"def", "r.ini", size);
    }
    for (u32 size : {21u, 20u, 10u, 2u, 1u})
        getW("truncW.value.size_" + std::to_string(size), u"Long", u"v", u"def", "r.ini", size);
    {
        prefill(false);
        set_last_error(0xdeadbeef);
        const u32 r = t.call(&K::impl_GetPrivateProfileStringA,
                             {t.A("Long"), t.A("v"), t.A("def"), ProfileApiTest::kBuf, 0, fileA("r.ini")});
        reportA("truncA.value.size_0", r, get_last_error(), 4);
    }
    for (const char* m : {"emptysecA", "emptysecW", "nullbufA0", "nullbufA10", "nullbufW0", "nullbufW10",
                          "size0W", "nullsecbufA", "nullnamesbufA"})
        child(m);

    for (const char* k : {"dec", "lead", "neg", "hex", "garbage", "emptyint", "big", "over", "plus",
                          "space_in", "absent"})
        intA(std::string("intA.") + k, "Ints", k, 99, "r.ini");
    intA("intA.absent_negative_default", "Ints", "absent", -7, "r.ini");
    intA("intA.absent_file", "Ints", "dec", 99, "absent.ini");
    intA("intA.string_value", "Sec", "plain", 99, "r.ini");
    {
        set_last_error(0xdeadbeef);
        u32 r = t.call(&K::impl_GetPrivateProfileIntW, {t.W(u"Ints"), t.W(u"hex"), 99, fileW("r.ini")});
        line("intW.hex.ret", "%u", r);
        r = t.call(&K::impl_GetPrivateProfileIntW, {t.W(u"Ints"), t.W(u"neg"), 99, fileW("r.ini")});
        line("intW.neg.ret", "%u (signed %d)", r, static_cast<int>(r));
    }

    for (u32 size : {512u, 40u, 20u, 3u, 2u, 1u})
        sectionA("sectionA.Sec.size_" + std::to_string(size), "Sec", "r.ini", size);
    sectionA("sectionA.sec_lower", "sec", "r.ini", 512);
    sectionA("sectionA.Spaced_Sec", "Spaced Sec", "r.ini", 512);
    sectionA("sectionA.absent_section", "Nope", "r.ini", 64);
    sectionA("sectionA.absent_file", "Sec", "absent.ini", 64);
    for (u32 size : {512u, 20u, 9u, 3u, 2u, 1u})
        namesA("namesA.size_" + std::to_string(size), "r.ini", size);
    namesA("namesA.absent_file", "absent.ini", 64);
    {
        prefill(true);
        set_last_error(0xdeadbeef);
        u32 r = t.call(&K::impl_GetPrivateProfileSectionW, {t.W(u"Sec"), ProfileApiTest::kBuf, 512, fileW("r.ini")});
        reportW("sectionW.Sec", r, get_last_error(), 512);
        prefill(true);
        set_last_error(0xdeadbeef);
        r = t.call(&K::impl_GetPrivateProfileSectionNamesW, {ProfileApiTest::kBuf, 9, fileW("r.ini")});
        reportW("namesW.size_9", r, get_last_error(), 9);
    }

    remove("w1.ini");
    writeA("writeA.new_file", "NewSec", "key", "value", "w1.ini");
    dump_file("writeA.new_file", "w1.ini");
    writeA("writeA.add_key", "NewSec", "key2", "value2", "w1.ini");
    writeA("writeA.add_section", "Other", "o", "1", "w1.ini");
    dump_file("writeA.add_key_and_section", "w1.ini");
    writeA("writeA.add_key_case_insensitive_section", "NEWSEC", "KEY", "replaced", "w1.ini");
    dump_file("writeA.replace_via_case", "w1.ini");
    writeA("writeA.spaced_value", "NewSec", "sp", "  sp  ", "w1.ini");
    writeA("writeA.spaced_key", "NewSec", "  spk  ", "v", "w1.ini");
    writeA("writeA.quoted_value", "NewSec", "q", "\"quoted\"", "w1.ini");
    writeA("writeA.empty_value", "NewSec", "e", "", "w1.ini");
    dump_file("writeA.spacing_cases", "w1.ini");
    getA("writeA.readback_spaced_value", "NewSec", "sp", "def", "w1.ini");
    getA("writeA.readback_spaced_key", "NewSec", "spk", "def", "w1.ini");
    getA("writeA.readback_quoted_value", "NewSec", "q", "def", "w1.ini");
    writeA("writeA.delete_key", "NewSec", "key2", nullptr, "w1.ini");
    dump_file("writeA.delete_key", "w1.ini");
    writeA("writeA.delete_absent_key", "NewSec", "nokey", nullptr, "w1.ini");
    writeA("writeA.delete_section", "Other", nullptr, nullptr, "w1.ini");
    dump_file("writeA.delete_section", "w1.ini");
    writeA("writeA.delete_absent_section", "NoSec", nullptr, nullptr, "w1.ini");
    writeA("writeA.flush_all_null", nullptr, nullptr, nullptr, nullptr);
    writeA("writeA.flush_null_sec_key_with_file", nullptr, nullptr, nullptr, "w1.ini");
    writeA("writeA.null_section_with_key", nullptr, "k", "v", "w1.ini");
    writeA("writeA.missing_directory", "S", "k", "v", "nodir\\x.ini");
    remove("w_absent.ini");
    writeA("writeA.delete_key_absent_file", "S", "k", nullptr, "w_absent.ini");
    dump_file("writeA.delete_key_absent_file", "w_absent.ini");

    write_text("w2.ini",
               "; header comment\r\n"
               "[S]\r\n"
               " a = 1 ; trailing\r\n"
               "b=2\r\n"
               "\r\n"
               "; between\r\n"
               "[T]\r\n"
               "t=1");
    writeA("writeA.replace_formatted", "S", "a", "X", "w2.ini");
    dump_file("writeA.replace_formatted", "w2.ini");
    writeA("writeA.append_to_section_without_trailing_newline", "T", "t2", "2", "w2.ini");
    dump_file("writeA.append_no_trailing_newline", "w2.ini");
    write_text("w3.ini", "[S]\nlf=1\nb=2\n");
    writeA("writeA.lf_file_add", "S", "c", "3", "w3.ini");
    writeA("writeA.lf_file_replace", "S", "lf", "9", "w3.ini");
    dump_file("writeA.lf_file", "w3.ini");

    {
        const char16_t content[] = u"\xfeff[S]\r\nk=v\r\n";
        write_raw("u16.ini", content, sizeof content - sizeof(char16_t));
        writeA("writeA.utf16_file", "S", "a", "ascii", "u16.ini");
        writeW("writeW.utf16_file_nonascii", u"S", u"w", u"\u00e9\u4e2d\u00ff", "u16.ini");
        writeW("writeW.utf16_file_new_section", u"N\u00e9", u"k\u00e9", u"v", "u16.ini");
        dump_file("write.utf16_file", "u16.ini");
        getA("readA.utf16_file_w", "S", "w", "def", "u16.ini");
        getW("readW.utf16_file_w", u"S", u"w", u"def", "u16.ini");
        getW("readW.utf16_file_k", u"S", u"k", u"def", "u16.ini");
        getW("readW.utf16_file_nonascii_section", u"N\u00e9", u"k\u00e9", u"def", "u16.ini");
        getA("readA.utf16_file_names", nullptr, nullptr, "def", "u16.ini", 64);
    }
    {
        const char16_t content[] = u"[S]\r\nk=v\r\n";
        write_raw("u16nobom.ini", content, sizeof content - sizeof(char16_t));
        getA("readA.utf16_nobom_k", "S", "k", "def", "u16nobom.ini");
        getW("readW.utf16_nobom_k", u"S", u"k", u"def", "u16nobom.ini");
        writeW("writeW.utf16_nobom_add", u"S", u"w", u"\u00e9", "u16nobom.ini");
        dump_file("write.utf16_nobom", "u16nobom.ini");
    }
    {
        write_text("u8.ini", "\xef\xbb\xbf[S]\r\nk=\xc3\xa9\r\n");
        getA("readA.utf8bom_k", "S", "k", "def", "u8.ini");
        getW("readW.utf8bom_k", u"S", u"k", u"def", "u8.ini");
        writeW("writeW.utf8bom_add", u"S", u"w", u"\u00e9\u4e2d", "u8.ini");
        dump_file("write.utf8bom", "u8.ini");
    }
    {
        remove("ansi.ini");
        writeW("writeW.ansi_new_file_nonascii", u"S", u"w", u"\u00e9\u4e2d\u00ff", "ansi.ini");
        dump_file("writeW.ansi_new_file", "ansi.ini");
        write_text("ansi2.ini", "[S]\r\nk=v\r\n");
        writeW("writeW.ansi_existing_nonascii", u"S", u"w", u"\u00e9\u4e2d", "ansi2.ini");
        dump_file("writeW.ansi_existing", "ansi2.ini");
    }

    {
        const char* base = "[S]\r\na=1\r\n; c\r\nb=2\r\n[T]\r\nt=1\r\n";
        write_text("ws.ini", base);
        static const char kList[] = "x=1\0 y = 2 \0novalue\0;c2\0";
        set_last_error(0xdeadbeef);
        u32 r = t.call(&K::impl_WritePrivateProfileSectionA,
                       {t.A("S"), t.bytes(kList, sizeof kList), fileA("ws.ini")});
        line("writeSectionA.replace.ret", "%d", static_cast<int>(r));
        line("writeSectionA.replace.err", "0x%x", get_last_error());
        dump_file("writeSectionA.replace", "ws.ini");
        set_last_error(0xdeadbeef);
        static const char kEmpty[] = "\0";
        r = t.call(&K::impl_WritePrivateProfileSectionA,
                   {t.A("S"), t.bytes(kEmpty, sizeof kEmpty), fileA("ws.ini")});
        line("writeSectionA.empty_list.ret", "%d", static_cast<int>(r));
        line("writeSectionA.empty_list.err", "0x%x", get_last_error());
        dump_file("writeSectionA.empty_list", "ws.ini");
        write_text("ws.ini", base);
        set_last_error(0xdeadbeef);
        r = t.call(&K::impl_WritePrivateProfileSectionA, {t.A("S"), 0, fileA("ws.ini")});
        line("writeSectionA.null.ret", "%d", static_cast<int>(r));
        line("writeSectionA.null.err", "0x%x", get_last_error());
        dump_file("writeSectionA.null", "ws.ini");
        set_last_error(0xdeadbeef);
        static const char kNew[] = "n=1\0";
        r = t.call(&K::impl_WritePrivateProfileSectionA,
                   {t.A("New"), t.bytes(kNew, sizeof kNew), fileA("ws.ini")});
        line("writeSectionA.new_section.ret", "%d", static_cast<int>(r));
        dump_file("writeSectionA.new_section", "ws.ini");
        remove("ws_new.ini");
        static const char16_t kWList[] = u"a=\u00e9\0b=2\0";
        r = t.call(&K::impl_WritePrivateProfileSectionW,
                   {t.W(u"W"), t.bytes(kWList, sizeof kWList), fileW("ws_new.ini")});
        line("writeSectionW.new_file.ret", "%d", static_cast<int>(r));
        dump_file("writeSectionW.new_file", "ws_new.ini");
    }

    {
        const u8 data[5] = {0x01, 0x02, 0x03, 0xab, 0xff};
        remove("st.ini");
        set_last_error(0xdeadbeef);
        u32 r = t.call(&K::impl_WritePrivateProfileStructA,
                       {t.A("S"), t.A("st"), t.bytes(data, 5), 5, fileA("st.ini")});
        line("structA.write.ret", "%d", static_cast<int>(r));
        line("structA.write.err", "0x%x", get_last_error());
        const u8 zero[1] = {0};
        (void)t.call(&K::impl_WritePrivateProfileStructA,
                     {t.A("S"), t.A("zero_len"), t.bytes(zero, 1), 0, fileA("st.ini")});
        (void)t.call(&K::impl_WritePrivateProfileStructW,
                     {t.W(u"S"), t.W(u"stw"), t.bytes(data, 5), 5, fileW("st.ini")});
        dump_file("structA.write", "st.ini");
        getA("structA.raw_value", "S", "st", "def", "st.ini");
        getA("structA.raw_value_zero_len", "S", "zero_len", "def", "st.ini");
        for (u32 size : {5u, 4u, 6u}) struct_read("structA.read.size_" + std::to_string(size), "st", size);
        struct { const char *label, *value; } bad[] = {
            {"bad_checksum", "010203ABFF00"}, {"lowercase_hex", "010203abffb1"},
            {"odd_length", "010203ABFFB"}, {"non_hex", "0102ZZABFFB1"},
        };
        for (auto& b : bad) {
            (void)t.call(&K::impl_WritePrivateProfileStringA,
                         {t.A("S"), t.A(b.label), t.A(b.value), fileA("st.ini")});
            struct_read(std::string("structA.read.") + b.label, b.label, 5);
        }
        struct_read("structA.read.absent_key", "absent", 5, false);
        set_last_error(0xdeadbeef);
        r = t.call(&K::impl_GetPrivateProfileStructW,
                   {t.W(u"S"), t.W(u"stw"), ProfileApiTest::kBuf, 5, fileW("st.ini")});
        line("structW.read.ret", "%d", static_cast<int>(r));
        line("structW.read.err", "0x%x", get_last_error());
        set_last_error(0xdeadbeef);
        r = t.call(&K::impl_WritePrivateProfileStructA, {t.A("S"), t.A("st"), 0, 0, fileA("st.ini")});
        line("structA.write_null.ret", "%d", static_cast<int>(r));
        line("structA.write_null.err", "0x%x", get_last_error());
        dump_file("structA.after_write_null", "st.ini");
    }

    {
        std::string hi = "[S]\r\nhi=";
        for (int c = 0x80; c <= 0xff; c += 0x11) hi += static_cast<char>(c);
        hi += "\r\n[\xe9sec]\r\n\xe9k=v\r\n";
        write_text("hi.ini", hi.c_str());
        getA("readA.ansi_high", "S", "hi", "def", "hi.ini");
        getW("readW.ansi_high", u"S", u"hi", u"def", "hi.ini");
        getA("readA.ansi_high_section", "\xe9sec", "\xe9k", "def", "hi.ini");
        getW("readW.ansi_high_section", u"\u00e9sec", u"\u00e9k", u"def", "hi.ini");
        getA("readA.ansi_high_section_upper", "\xc9SEC", "\xc9K", "def", "hi.ini");
        getW("readW.ansi_high_section_upper", u"\u00c9SEC", u"\u00c9K", u"def", "hi.ini");
        getW("readW.ansi_high_names", nullptr, nullptr, u"def", "hi.ini", 64);
    }
    getW("readW.plain", u"Sec", u"plain", u"def", "r.ini");
    getW("readW.spaced", u"Sec", u"spaced", u"def", "r.ini");
    getW("readW.squote", u"Sec", u"squote", u"def", "r.ini");
    getW("readW.dquote", u"Sec", u"dquote", u"def", "r.ini");
    getW("readW.mixquote", u"Sec", u"mixquote", u"def", "r.ini");
    getW("readW.default_trailing_spaces", u"Sec", u"absent", u"def   ", "r.ini");
    getW("readW.default_null", u"Sec", u"absent", nullptr, "r.ini");
    getW("readW.absent_file", u"Sec", u"plain", u"def", "absent.ini");
    return out;
}

void compare_with_expected(const std::vector<std::string>& got, const char* arch) {
    const std::string path =
        std::string(VITREO_ORACLE_PROBE_DIR) + "/profile_api_probe." + arch + ".expected";
    std::ifstream f(path);
    ASSERT_TRUE(f) << path;
    std::vector<std::string> want;
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        want.push_back(l);
    }
    std::ostringstream diff;
    int mismatches = 0;
    for (usize i = 0; i < std::max(want.size(), got.size()); ++i) {
        const std::string w = i < want.size() ? want[i] : "<missing>";
        const std::string g = i < got.size() ? got[i] : "<missing>";
        if (w != g && ++mismatches <= 60) {
            diff << "line " << (i + 1) << "\n  windows: " << w << "\n  vitreo:  " << g << "\n";
        }
    }
    EXPECT_EQ(mismatches, 0) << diff.str();
}

} // namespace

TEST_F(ProfileApiTest, ProfileProbeReplayX86) {
    set_mode(false);
    ProfileProbeReplay replay(*this, false);
    compare_with_expected(replay.run(), "x86");
}

TEST_F(ProfileApiTest, ProfileProbeReplayX64) {
    set_mode(true);
    ProfileProbeReplay replay(*this, true);
    compare_with_expected(replay.run(), "x64");
}

// Learn (GetPrivateProfileString, lpFileName): a name without a path is in the
// Windows directory; GetProfile*/WriteProfile* use win.ini there.
TEST_F(ProfileApiTest, BareNameAndWinIniResolveInWindowsDirectory) {
    set_mode(false);
    {
        std::ofstream ini(root / "Windows" / "app.ini", std::ios::binary);
        ini << "[S]\r\nk=bare\r\n";
    }
    std::vector<u8> cc(64, 0xcc);
    (void)memory.write_bytes(kBuf, cc.data(), cc.size());
    EXPECT_EQ(call(&K::impl_GetPrivateProfileStringA,
                   {A("S"), A("k"), A("d"), kBuf, 64, A("app.ini")}),
              4u);
    u8 got[5] = {};
    (void)memory.read_bytes(kBuf, got, 5);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(got)), "bare");

    // WriteProfileString creates win.ini in the Windows directory.
    EXPECT_EQ(call(&K::impl_WriteProfileStringW, {W(u"Desktop"), W(u"Wallpaper"), W(u"(None)")}), 1u);
    EXPECT_EQ(call(&K::impl_WriteProfileStringA, {A("Desktop"), A("TileWallpaper"), A("0")}), 1u);
    std::ifstream f(root / "Windows" / "win.ini", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(text, "[Desktop]\r\nWallpaper=(None)\r\nTileWallpaper=0\r\n");

    EXPECT_EQ(call(&K::impl_GetProfileIntA, {A("Desktop"), A("TileWallpaper"), 7}), 0u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(call(&K::impl_GetProfileIntW, {W(u"Desktop"), W(u"Missing"), 7}), 7u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);

    (void)memory.write_bytes(kBuf, cc.data(), cc.size());
    EXPECT_EQ(call(&K::impl_GetProfileSectionW, {W(u"desktop"), kBuf, 64}), 33u);
    std::u16string section;
    for (u32 i = 0; i < 34; ++i) section.push_back(memory.read_u16(kBuf + i * 2).value());
    EXPECT_EQ(section, std::u16string(u"Wallpaper=(None)\0TileWallpaper=0\0\0", 34));

    static const char kList[] = "a=1\0";
    EXPECT_EQ(call(&K::impl_WriteProfileSectionA, {A("Desktop"), bytes(kList, sizeof kList)}), 1u);
    std::ifstream g(root / "Windows" / "win.ini", std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(g)), {});
    EXPECT_EQ(after, "[Desktop]\r\na=1\r\n");
}
