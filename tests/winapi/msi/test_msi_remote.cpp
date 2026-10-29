// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_remote.cpp
/// @brief Windows Installer process architecture: custom-action servers and
/// the remote session API they relay (msi_remote.hpp), and the client /
/// service split (msi_service.hpp).
///
/// Sources: Learn "Windows Installer Service", "Custom Action Security",
/// "64-Bit Custom Actions", "Custom Action In-Script Execution Options",
/// "Obtaining Context Information for Deferred Execution Custom Actions",
/// "Custom Action Return Values", MsiSetExternalUIRecord, MsiProcessMessage;
/// the export table generated from msi.dll / Msi.lib (argument sizes);
/// MsiQuery.h prototypes (the relay signatures). The custom-action server
/// runs on a thread of this test with a recording host; its DLL "entry
/// points" call msi.dll exports through the fixture in the parameterized
/// bitness, so every relayed call crosses the real wire to an engine running
/// on the test thread.

#include "msi_fixture.hpp"

#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_remote.hpp"
#include "winapi/system/msi/msi_service.hpp"
#include "winapi/system/msi/msi_ui.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace vitreo::winapi::msi_test {
namespace {

using namespace vitreo::winapi::msi;

constexpr u16 kAmd64 = 0x8664;  // IMAGE_FILE_MACHINE_AMD64
constexpr u16 kI386 = 0x014C;   // IMAGE_FILE_MACHINE_I386
constexpr u16 kArm64 = 0xAA64;  // IMAGE_FILE_MACHINE_ARM64
constexpr u32 kInfo = 0x04000000;         // INSTALLMESSAGE_INFO
constexpr u32 kError = 0x01000000;        // INSTALLMESSAGE_ERROR
constexpr u32 kActionStart = 0x08000000;  // INSTALLMESSAGE_ACTIONSTART

std::vector<u8> pe_image(u16 machine) {
    std::vector<u8> b(0x200, 0);
    b[0] = 'M';
    b[1] = 'Z';
    b[0x3C] = 0x40;
    b[0x40] = 'P';
    b[0x41] = 'E';
    b[0x44] = static_cast<u8>(machine);
    b[0x45] = static_cast<u8>(machine >> 8);
    return b;
}

std::string utf8(const std::u16string& s) { return std::string(s.begin(), s.end()); }

/// The engine process's custom-action host: scratch memory of its own.
class EngineHost final : public CustomActionProcess {
public:
    static constexpr GuestAddr kBase = 0x01000000;
    static constexpr u32 kSize = 0x01000000;
    EngineHost() { (void)memory.map_region(kBase, kSize, MemoryProtection::ReadWrite); }
    [[nodiscard]] bool is_64bit() const override { return true; }
    u64 load_library_from_bytes(const std::vector<u8>&, std::u16string&) override { return 0; }
    u64 load_library(const std::u16string&) override { return 0; }
    u64 get_proc_address(u64, const std::string&) override { return 0; }
    void free_library(u64) override {}
    u32 call_dll_entry(u64, u32) override { return 0; }
    bool run_process(const std::u16string&, const std::u16string&, const std::u16string&, bool, u32&) override {
        return false;
    }
    bool write_temp_file(const std::vector<u8>&, const std::u16string&, std::u16string& path) override {
        path = u"C:\\Windows\\Installer\\MSI" + std::u16string(1, static_cast<char16_t>(u'A' + temp_files++)) + u".tmp";
        return true;
    }
    void delete_file(const std::u16string&) override { ++deleted; }
    std::optional<u16> image_machine(const std::u16string&) override { return std::nullopt; }
    u64 start_dll_entry(u64, u32) override { return 0; }
    u64 start_process(const std::u16string&, const std::u16string&, const std::u16string&) override { return 0; }
    u32 wait_and_close(u64) override { return 0; }
    void close_handle(u64) override {}
    GuestMemory& guest_memory() override { return memory; }
    GuestAddr alloc_scratch(u32 bytes) override {
        std::lock_guard lock(mutex);
        const u32 size = (bytes + 0xFFF) & ~0xFFFu;
        if (used + size > kSize) return 0;
        const GuestAddr at = kBase + used;
        used += size;
        blocks.push_back(at);
        return at;
    }
    void free_scratch(GuestAddr) override {}
    bool start_custom_action_server(bool, const std::u16string&) override { return false; }
    void pump_messages() override {}

    GuestMemory memory;
    std::mutex mutex;
    std::vector<GuestAddr> blocks;
    u32 used{0};
    int temp_files{0};
    int deleted{0};
};

using Entry = std::function<u32(u32 hInstall)>;

/// A custom-action server's host: "DLLs" whose entry points are test lambdas
/// run on their own thread, as the guest's CreateThread would.
class ServerHost final : public CustomActionProcess {
public:
    ServerHost(bool x64, std::map<std::string, Entry>* entries) : x64_(x64), entries_(entries) {}
    [[nodiscard]] bool is_64bit() const override { return x64_; }
    u64 load_library_from_bytes(const std::vector<u8>&, std::u16string&) override { return 0; }
    u64 load_library(const std::u16string& path) override { loaded.push_back(path); return 0x1000; }
    u64 get_proc_address(u64, const std::string& name) override {
        const auto it = entries_->find(name);
        if (it == entries_->end()) return 0;
        return 0x2000 + static_cast<u64>(std::distance(entries_->begin(), it));
    }
    void free_library(u64) override { ++freed; }
    u32 call_dll_entry(u64, u32) override { return 0; }
    bool run_process(const std::u16string&, const std::u16string&, const std::u16string&, bool, u32&) override {
        return false;
    }
    bool write_temp_file(const std::vector<u8>&, const std::u16string&, std::u16string&) override { return false; }
    void delete_file(const std::u16string&) override {}
    std::optional<u16> image_machine(const std::u16string&) override { return std::nullopt; }
    u64 start_dll_entry(u64 fn, u32 install) override {
        auto it = entries_->begin();
        std::advance(it, static_cast<std::ptrdiff_t>(fn - 0x2000));
        Entry entry = it->second;
        const u64 id = ++next_;
        threads_[id] = std::thread([this, id, entry, install] {
            const u32 rc = entry(install);
            std::lock_guard lock(mutex_);
            results_[id] = rc;
        });
        return id;
    }
    u64 start_process(const std::u16string&, const std::u16string&, const std::u16string&) override { return 0; }
    u32 wait_and_close(u64 handle) override {
        threads_[handle].join();
        threads_.erase(handle);
        std::lock_guard lock(mutex_);
        return results_[handle];
    }
    void close_handle(u64) override {}
    GuestMemory& guest_memory() override { return memory_; }
    GuestAddr alloc_scratch(u32) override { return 0; }
    void free_scratch(GuestAddr) override {}
    bool start_custom_action_server(bool, const std::u16string&) override { return false; }
    void pump_messages() override {}

    std::vector<std::u16string> loaded;
    int freed{0};

private:
    bool x64_;
    std::map<std::string, Entry>* entries_;
    GuestMemory memory_;
    std::mutex mutex_;
    std::map<u64, std::thread> threads_;
    std::map<u64, u32> results_;
    u64 next_{0};
};

class MsiRemote : public MsiTest {
protected:
    struct Launch {
        bool x64;
        bool elevated;
    };

    void SetUp() override {
        MsiTest::SetUp();
        pkg = std::make_shared<Package>(Package::create());
        ASSERT_EQ(pkg->create_table("Property", {make_column("Property", 's', 72, false, true, false, false),
                                                 make_column("Value", 's', 0, false, false, false, false)},
                                    false),
                  0u);
        Table* t = pkg->find_table("Property");
        for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{
                 {"ProductCode", "{11111111-2222-3333-4444-555555555555}"},
                 {"ProductName", "Remote Test"},
                 {"ProductLanguage", "1033"},
                 {"ProductVersion", "1.0.0"}}) {
            ASSERT_EQ(pkg->insert_row(*t, {k, v}, false), 0u);
        }
        pkg->set_stream("Binary.Ca64", pe_image(kAmd64));
        pkg->set_stream("Binary.Ca86", pe_image(kI386));
        pkg->set_stream("Binary.CaArm", pe_image(kArm64));
        set_process_custom_action_host(&engine_host);
        Services services;
        services.manager = &emgr;
        services.custom_actions = &engine_host;
        services.launch_custom_action_server = [this](bool x64, bool elevated, const std::u16string& token) {
            launches.push_back(Launch{x64, elevated});
            auto host = std::make_shared<ServerHost>(x64, &entries);
            hosts.push_back(host);
            servers.emplace_back([this, host, token] { (void)run_custom_action_server(mgr, utf8(token), *host); });
            return true;
        };
        Engine::Options o;
        o.package_path = "C:\\remote.msi";
        o.allow_system_changes = true;
        o.server = true;
        u32 error = 0;
        engine = Engine::open(emgr, pkg, o, services, error);
        ASSERT_TRUE(engine);
        set_client_ui_relay(emgr, [this](u32 type, const Record* record, const std::string& text) -> i32 {
            std::lock_guard lock(message_mutex);
            messages.push_back({type, text, record ? std::optional<Record>(*record) : std::nullopt});
            return message_answer;
        });
    }

    void TearDown() override {
        if (engine) (void)finish_async_custom_actions(engine->session());
        set_client_ui_relay(emgr, nullptr);
        engine.reset();
        for (auto& t : servers) t.join();
        set_process_custom_action_host(nullptr);
        set_custom_action_server_process(false);
    }

    u32 run(const std::string& action, u32 type, const std::string& source, const std::string& target) {
        ActionContext ctx(engine->session(), engine->services());
        return run_custom_action(ctx, action, type, source, target);
    }

    // Calls from a custom action's thread (the CA server's process).
    static constexpr GuestAddr NAME = DATA + 0x8000;
    static constexpr GuestAddr VAL = DATA + 0x9000;
    static constexpr GuestAddr OUTBUF = DATA + 0xA000;
    static constexpr GuestAddr PCCH = DATA + 0xB000;

    std::u16string read_wide(GuestAddr at) {
        std::u16string out;
        for (u32 i = 0; i < 4096; ++i) {
            const u16 c = memory.read_u16(at + i * 2).value();
            if (c == 0) break;
            out.push_back(static_cast<char16_t>(c));
        }
        return out;
    }
    std::u16string get_property(u32 h, const std::u16string& name) {
        write_wide(NAME, name);
        write_int(PCCH, 1024);
        EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
        return read_wide(OUTBUF);
    }

    struct Message {
        u32 type;
        std::string text;
        std::optional<Record> record;
    };

    MsiManager emgr;  ///< the engine process's msi.dll
    EngineHost engine_host;
    std::shared_ptr<Package> pkg;
    std::shared_ptr<Engine> engine;
    std::map<std::string, Entry> entries;
    std::vector<Launch> launches;
    std::vector<std::shared_ptr<ServerHost>> hosts;
    std::vector<std::thread> servers;
    std::mutex message_mutex;
    std::vector<Message> messages;
    i32 message_answer{0};
};

// ---- Relay table ------------------------------------------------------------------

// Every relayed export exists in msi.dll, and its signature has one entry per
// argument: x86 stdcall cleanup bytes (from Msi.lib) / 4, every argument of
// these prototypes being 4 bytes on x86.
TEST_P(MsiRemote, RelaySignaturesMatchTheExportTable) {
    std::map<std::string, u32> bytes;
#define MSI_EXPORT(name, ordinal, cleanup) bytes[#name] = cleanup;
#include "winapi/system/msi/msi_export_table.inc"
#undef MSI_EXPORT
    for (const RelaySignature& sig : relay_signatures()) {
        ASSERT_TRUE(bytes.contains(sig.name)) << sig.name;
        EXPECT_EQ(sig.args.size() * 4, bytes[sig.name]) << sig.name;
        EXPECT_EQ(sig.args[0], 'h') << sig.name;
        EXPECT_EQ(sig.wide, sig.name.back() == 'W') << sig.name;
    }
}

TEST_P(MsiRemote, WireRecordRoundTrip) {
    Record r(3);
    r.fields[0].kind = Record::Kind::String;
    r.fields[0].text = u"[1] and [2]";
    r.fields[1].kind = Record::Kind::Integer;
    r.fields[1].integer = -5;
    r.fields[2].kind = Record::Kind::String;
    r.fields[2].text = u"\u00e9t\u00e9";
    r.fields[3].kind = Record::Kind::Stream;
    r.fields[3].stream = {1, 2, 3};
    WireWriter w;
    w.record(r);
    WireReader rd(w.data());
    auto back = rd.record();
    ASSERT_TRUE(back);
    EXPECT_TRUE(rd.done());
    ASSERT_EQ(back->count(), 3u);
    EXPECT_EQ(back->fields[0].text, r.fields[0].text);
    EXPECT_EQ(back->fields[1].integer, -5);
    EXPECT_EQ(back->fields[2].text, r.fields[2].text);
    EXPECT_EQ(back->fields[3].stream, r.fields[3].stream);
    // A truncated body is rejected, not half-read.
    std::vector<u8> cut(w.data().begin(), w.data().end() - 2);
    WireReader bad(cut);
    EXPECT_FALSE(bad.record().has_value());
}

TEST_P(MsiRemote, InstallRequestRoundTrip) {
    InstallRequest q;
    q.kind = InstallRequest::Kind::ExecuteSequence;
    q.package_path = "C:\\a b\\p.msi";
    q.action = "INSTALL";
    q.table = "InstallExecuteSequence";
    q.properties = {{"ADDLOCAL", "ALL"}, {"ProductCode", "{X}"}};
    q.ui_level = 3 | 0x20;
    q.message_filter = 0x1F;
    q.log_file = "C:\\l.txt";
    q.log_mode = 0x1FFF;
    q.log_append = true;
    q.identity.user_sid = "S-1-5-21-1-2-3-1000";
    q.identity.elevated = true;
    q.identity.process_id = 42;
    const auto bytes = encode_install_request(q);
    auto back = decode_install_request(bytes);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->kind, q.kind);
    EXPECT_EQ(back->package_path, q.package_path);
    EXPECT_EQ(back->table, q.table);
    EXPECT_EQ(back->properties, q.properties);
    EXPECT_EQ(back->ui_level, q.ui_level);
    EXPECT_EQ(back->message_filter, q.message_filter);
    EXPECT_EQ(back->log_mode, q.log_mode);
    EXPECT_TRUE(back->log_append);
    EXPECT_EQ(back->identity.user_sid, q.identity.user_sid);
    EXPECT_TRUE(back->identity.elevated);
    EXPECT_EQ(back->identity.process_id, 42u);
    std::vector<u8> extra = bytes;
    extra.push_back(0);
    EXPECT_FALSE(decode_install_request(extra));  // trailing garbage
    std::vector<u8> cut(bytes.begin(), bytes.end() - 1);
    EXPECT_FALSE(decode_install_request(cut));
}

// ---- DLL custom actions in custom-action servers --------------------------------------

// Learn "64-Bit Custom Actions": a DLL runs in the server of its own bitness,
// whatever the package's; an immediate action is impersonated. Servers are
// reused by the session's later actions of the same kind.
TEST_P(MsiRemote, DllRunsInServerOfItsBitness) {
    entries["Read"] = [this](u32 h) -> u32 {
        EXPECT_EQ(utf8(get_property(h, u"ProductName")), "Remote Test");
        write_wide(NAME, u"FROMCA");
        write_wide(VAL, u"set by CA");
        EXPECT_EQ(call("MsiSetPropertyW", {I(h), I(NAME), I(VAL)}), 0u);
        return 0;
    };
    EXPECT_EQ(run("Ca1", 1, "Ca64", "Read"), 0u);
    EXPECT_EQ(engine->session().property("FROMCA"), "set by CA");
    EXPECT_EQ(run("Ca2", 1, "Ca86", "Read"), 0u);
    EXPECT_EQ(run("Ca3", 1, "Ca64", "Read"), 0u);
    ASSERT_EQ(launches.size(), 2u);
    EXPECT_TRUE(launches[0].x64);
    EXPECT_FALSE(launches[0].elevated);
    EXPECT_FALSE(launches[1].x64);
    // The Binary-table code is written to a temp file, loaded by path, freed,
    // and the temp file deleted.
    EXPECT_EQ(engine_host.deleted, 3);
    EXPECT_EQ(hosts[0]->loaded.size(), 2u);
    EXPECT_EQ(hosts[0]->freed, 2);
}

// MsiGetProperty's buffer convention survives the relay (Learn MsiGetProperty:
// null buffer -> length; short buffer -> ERROR_MORE_DATA, truncated and
// terminated, *pcch = length).
TEST_P(MsiRemote, BufferConventionsThroughRelay) {
    entries["Buffers"] = [this](u32 h) -> u32 {
        write_wide(NAME, u"ProductName");
        write_int(PCCH, 0);
        EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(0), I(PCCH)}), 0u);
        EXPECT_EQ(read_int(PCCH), 11);
        write_int(PCCH, 5);
        EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
        EXPECT_EQ(read_wide(OUTBUF), u"Remo");
        EXPECT_EQ(read_int(PCCH), 11);
        write_int(PCCH, 12);
        EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
        EXPECT_EQ(read_wide(OUTBUF), u"Remote Test");
        EXPECT_EQ(read_int(PCCH), 11);
        // ANSI: the count is in bytes.
        const char name[] = "ProductName";
        for (usize i = 0; i < sizeof(name); ++i) EXPECT_TRUE(memory.write_u8(NAME + i, static_cast<u8>(name[i])).is_ok());
        write_int(PCCH, 64);
        EXPECT_EQ(call("MsiGetPropertyA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
        EXPECT_EQ(read_int(PCCH), 11);
        EXPECT_EQ(memory.read_u8(OUTBUF + 11).value(), 0u);
        EXPECT_EQ(memory.read_u8(OUTBUF).value(), static_cast<u8>('R'));
        // A null pcch is ERROR_INVALID_PARAMETER on the engine's side.
        write_wide(NAME, u"ProductName");
        EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(0)}), 87u);
        EXPECT_EQ(call("MsiEvaluateConditionW", {I(h), I(NAME)}), 1u);  // ProductName is set
        EXPECT_EQ(call("MsiGetLanguage", {I(h)}), 1033u);
        return 0;
    };
    EXPECT_EQ(run("CaBuf", 1, "Ca64", "Buffers"), 0u);
}

// The database a custom action opens is the engine's; views are proxies,
// fetched records come back by value as the server's own records.
TEST_P(MsiRemote, DatabaseAndViewsThroughRelay) {
    entries["Query"] = [this](u32 h) -> u32 {
        const u32 db = static_cast<u32>(call("MsiGetActiveDatabase", {I(h)}));
        EXPECT_NE(db, 0u);
        write_wide(VAL, u"SELECT `Value` FROM `Property` WHERE `Property` = 'ProductName'");
        EXPECT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(VAL), I(OUT)}), 0u);
        const u32 view = static_cast<u32>(read_int(OUT));
        EXPECT_NE(view, 0u);
        EXPECT_EQ(call("MsiViewExecute", {I(view), I(0)}), 0u);
        EXPECT_EQ(call("MsiViewFetch", {I(view), I(OUT2)}), 0u);
        const u32 rec = static_cast<u32>(read_int(OUT2));
        EXPECT_TRUE(mgr.handles().get<Record>(rec) != nullptr);  // a local record
        write_int(PCCH, 64);
        EXPECT_EQ(call("MsiRecordGetStringW", {I(rec), I(1), I(OUTBUF), I(PCCH)}), 0u);
        EXPECT_EQ(read_wide(OUTBUF), u"Remote Test");
        EXPECT_EQ(call("MsiCloseHandle", {I(rec)}), 0u);
        write_int(OUT2, 0x77);
        EXPECT_EQ(call("MsiViewFetch", {I(view), I(OUT2)}), 259u);  // ERROR_NO_MORE_ITEMS
        EXPECT_EQ(call("MsiViewClose", {I(view)}), 0u);
        EXPECT_EQ(call("MsiCloseHandle", {I(view)}), 0u);
        EXPECT_EQ(call("MsiCloseHandle", {I(view)}), 6u);  // closed: ERROR_INVALID_HANDLE
        EXPECT_EQ(call("MsiCloseHandle", {I(db)}), 0u);
        return 0;
    };
    EXPECT_EQ(run("CaDb", 1, "Ca64", "Query"), 0u);
}

// MsiProcessMessage from a custom action reaches the engine's message route
// (here the client relay) with the server's record, and returns its answer.
TEST_P(MsiRemote, ProcessMessageCarriesTheServersRecord) {
    message_answer = 1;  // IDOK
    entries["Msg"] = [this](u32 h) -> u32 {
        const u32 rec = static_cast<u32>(call("MsiCreateRecord", {I(1)}));
        write_wide(VAL, u"from the custom action");
        EXPECT_EQ(call("MsiRecordSetStringW", {I(rec), I(0), I(VAL)}), 0u);
        EXPECT_EQ(call("MsiProcessMessage", {I(h), I(kInfo), I(rec)}), 1u);
        EXPECT_EQ(call("MsiCloseHandle", {I(rec)}), 0u);
        return 0;
    };
    EXPECT_EQ(run("CaMsg", 1, "Ca64", "Msg"), 0u);
    bool seen = false;
    for (const Message& m : messages) seen |= m.type == kInfo && m.text == "from the custom action";
    EXPECT_TRUE(seen);
}

// Learn "Custom Action Security": a deferred no-impersonate action runs in
// the elevated server; "Obtaining Context Information for Deferred Execution
// Custom Actions": it sees CustomActionData, ProductCode and UserSID only.
TEST_P(MsiRemote, DeferredNoImpersonateRunsElevatedWithCustomActionData) {
    entries["Deferred"] = [this](u32 h) -> u32 {
        EXPECT_EQ(utf8(get_property(h, u"CustomActionData")), "data123");
        EXPECT_EQ(utf8(get_property(h, u"ProductCode")), "{11111111-2222-3333-4444-555555555555}");
        EXPECT_EQ(utf8(get_property(h, u"ProductName")), "");
        EXPECT_EQ(call("MsiGetMode", {I(h), I(kRunScheduled)}), 1u);
        EXPECT_EQ(call("MsiGetActiveDatabase", {I(h)}), 0u);
        return 0;
    };
    entries["Impersonated"] = [this](u32 h) -> u32 {
        EXPECT_EQ(utf8(get_property(h, u"CustomActionData")), "other");
        return 0;
    };
    ASSERT_TRUE(engine->begin_script());
    engine->session().set_property("CaDef", "data123");
    engine->session().set_property("CaImp", "other");
    EXPECT_EQ(run("CaDef", 1 | kCaInScript | kCaNoImpersonate, "Ca64", "Deferred"), 0u);
    EXPECT_EQ(run("CaImp", 1 | kCaInScript, "Ca86", "Impersonated"), 0u);
    EXPECT_TRUE(launches.empty());  // scheduled, not run yet
    EXPECT_EQ(engine->execute_script(true), 0u);
    ASSERT_EQ(launches.size(), 2u);
    EXPECT_TRUE(launches[0].x64);
    EXPECT_TRUE(launches[0].elevated);
    EXPECT_FALSE(launches[1].x64);
    EXPECT_FALSE(launches[1].elevated);
}

// Learn "Custom Action Return Values" and the measured mapping
// (msi_customaction_*.expected); a missing entry point is error 1723.
TEST_P(MsiRemote, ReturnValuesAndMissingEntry) {
    entries["R1"] = [](u32) -> u32 { return 1; };
    entries["R259"] = [](u32) -> u32 { return 259; };
    entries["R1602"] = [](u32) -> u32 { return 1602; };
    entries["R1626"] = [](u32) -> u32 { return 1626; };
    EXPECT_EQ(run("A", 1, "Ca64", "R1"), 1603u);
    EXPECT_EQ(run("B", 1, "Ca64", "R259"), 259u);
    EXPECT_EQ(run("C", 1, "Ca64", "R1602"), 1602u);
    EXPECT_EQ(run("D", 1, "Ca64", "R1626"), 1626u);
    EXPECT_EQ(run("E", 1 | kCaContinue, "Ca64", "R1"), 0u);
    EXPECT_EQ(run("F", 1, "Ca64", "NoSuchEntry"), 1603u);
    EXPECT_EQ(run("G", 1 | kCaContinue, "Ca64", "NoSuchEntry"), 0u);
    bool saw_1723 = false;
    for (const Message& m : messages) {
        if ((m.type & 0xFF000000u) == kError && m.record && m.record->count() >= 1 &&
            m.record->fields[1].kind == Record::Kind::Integer && m.record->fields[1].integer == 1723) {
            saw_1723 = true;
        }
    }
    EXPECT_TRUE(saw_1723);
    EXPECT_EQ(launches.size(), 1u);  // one x64 server ran all of them
}

// A DLL that is not an x86 or x64 image runs in no custom-action server.
TEST_P(MsiRemote, NonX86ImageFailsWithoutAServer) {
    entries["Any"] = [](u32) -> u32 { return 0; };
    EXPECT_EQ(run("Arm", 1, "CaArm", "Any"), 1603u);
    EXPECT_EQ(run("ArmC", 1 | kCaContinue, "CaArm", "Any"), 0u);
    EXPECT_TRUE(launches.empty());
}

// msidbCustomActionTypeAsync: the action runs in its own server, the
// sequence goes on, and its result never changes the install's (measured).
TEST_P(MsiRemote, AsyncDllRunsInADedicatedServer) {
    entries["Sync"] = [](u32) -> u32 { return 0; };
    entries["Async"] = [](u32) -> u32 { return 1603; };
    EXPECT_EQ(run("S", 1, "Ca64", "Sync"), 0u);
    EXPECT_EQ(run("As", 1 | kCaAsync, "Ca64", "Async"), 0u);
    EXPECT_EQ(launches.size(), 2u);
    EXPECT_EQ(finish_async_custom_actions(engine->session()), 0u);
}

// MsiSetExternalUIRecord cannot be called from a custom action (Learn); in
// a custom-action server every thread runs custom-action code.
TEST_P(MsiRemote, ExternalUiRecordRefusedInCustomActionServer) {
    set_custom_action_server_process(true);
    EXPECT_EQ(call("MsiSetExternalUIRecord", {I(0), I(0), I(0), I(0)}), 120u);
    set_custom_action_server_process(false);
    EXPECT_EQ(call("MsiSetExternalUIRecord", {I(0), I(0), I(0), I(0)}), 0u);
}

// ---- Client / service ------------------------------------------------------------------

// The client answers the service's messages through its handlers, starts the
// impersonated servers it asks for, and returns the service's result.
TEST_P(MsiRemote, ClientServesTheServiceUntilDone) {
    auto [client_end, service_end] = RemoteChannel::pair();
    std::thread service([service_end = service_end] {
        auto request = service_end->receive();
        ASSERT_TRUE(request);
        ASSERT_EQ(request->kind, Wire::InstallRequest);
        auto q = decode_install_request(request->body);
        ASSERT_TRUE(q);
        EXPECT_EQ(q->package_path, "C:\\p.msi");
        Record r(1);
        r.fields[1].kind = Record::Kind::Integer;
        r.fields[1].integer = 1722;
        WireWriter w;
        w.u32_(kError);
        w.u8_(1);
        w.record(r);
        w.str("Error 1722.");
        ASSERT_TRUE(service_end->send(Wire::UiMessage, w.data()));
        auto reply = service_end->receive();
        ASSERT_TRUE(reply && reply->kind == Wire::UiReply);
        EXPECT_EQ(WireReader(reply->body).u32_(), 2u);  // IDCANCEL
        WireWriter l;
        l.u8_(0);
        l.u16str(u"TOKEN");
        ASSERT_TRUE(service_end->send(Wire::LaunchServer, l.data()));
        auto launched = service_end->receive();
        ASSERT_TRUE(launched && launched->kind == Wire::LaunchReply);
        EXPECT_EQ(launched->body.at(0), 1u);
        WireWriter d;
        d.u32_(3010);
        d.u8_(1);
        d.u8_(0);
        ASSERT_TRUE(service_end->send(Wire::InstallDone, d.data()));
    });
    InstallRequest q;
    q.package_path = "C:\\p.msi";
    InstallClient client;
    std::string text;
    bool launched_x64 = true;
    std::u16string token;
    client.message = [&](u32 type, const Record* record, const std::string& t) -> i32 {
        EXPECT_EQ(type, kError);
        EXPECT_TRUE(record != nullptr);
        text = t;
        return 2;
    };
    client.launch_server = [&](bool x64, const std::u16string& tk) {
        launched_x64 = x64;
        token = tk;
        return true;
    };
    const InstallResult result = run_in_service(*client_end, q, client);
    service.join();
    EXPECT_EQ(result.rc, 3010u);
    EXPECT_TRUE(result.reboot_at_end);
    EXPECT_EQ(text, "Error 1722.");
    EXPECT_FALSE(launched_x64);
    EXPECT_EQ(token, u"TOKEN");
}

// A service that goes away is ERROR_INSTALL_SERVICE_FAILURE.
TEST_P(MsiRemote, BrokenServiceConnectionIs1601) {
    auto [client_end, service_end] = RemoteChannel::pair();
    std::thread service([service_end = service_end] {
        (void)service_end->receive();
        service_end->close();
    });
    InstallRequest q;
    const InstallResult result = run_in_service(*client_end, q, InstallClient{});
    service.join();
    EXPECT_EQ(result.rc, 1601u);
}

// The service runs the install with its own engine and relays what the
// client's filter asks for: action starts and errors with the formatted
// text the string handler would get.
TEST_P(MsiRemote, ServiceRunsTheInstallAndRelaysMessages) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("vitreo_msi_remote_" + std::to_string(::getpid()) + (x64() ? "_64" : "_32"));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    MsiManager smgr;  // the service process's msi.dll
    smgr.set_vfs(&vfs);
    {
        Package p = Package::create();
        ASSERT_EQ(p.create_table("Property", {make_column("Property", 's', 72, false, true, false, false),
                                              make_column("Value", 's', 0, false, false, false, false)},
                                 false),
                  0u);
        Table* t = p.find_table("Property");
        ASSERT_EQ(p.insert_row(*t, {"ProductCode", "{11111111-2222-3333-4444-555555555555}"}, false), 0u);
        ASSERT_EQ(p.insert_row(*t, {"ProductName", "Svc"}, false), 0u);
        ASSERT_EQ(p.create_table("CustomAction", {make_column("Action", 's', 72, false, true, false, false),
                                                  make_column("Type", 'i', 2, false, false, false, false),
                                                  make_column("Source", 's', 72, true, false, false, false),
                                                  make_column("Target", 's', 255, true, false, false, false)},
                                 false),
                  0u);
        Table* ca = p.find_table("CustomAction");
        ASSERT_EQ(p.insert_row(*ca, {"CaSet", "51", "MYPROP", "hello"}, false), 0u);
        ASSERT_EQ(p.insert_row(*ca, {"CaErr", "19", "", "literal [MYPROP]"}, false), 0u);
        ASSERT_EQ(p.create_table("InstallExecuteSequence",
                                 {make_column("Action", 's', 72, false, true, false, false),
                                  make_column("Condition", 's', 255, true, false, false, false),
                                  make_column("Sequence", 'i', 2, true, false, false, false)},
                                 false),
                  0u);
        Table* seq = p.find_table("InstallExecuteSequence");
        ASSERT_EQ(p.insert_row(*seq, {"CaSet", "", "100"}, false), 0u);
        ASSERT_EQ(p.insert_row(*seq, {"CaErr", "", "200"}, false), 0u);
        const std::vector<u8> bytes = p.serialize();
        std::ofstream(root / "svc.msi", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                                static_cast<std::streamsize>(bytes.size()));
    }
    for (const bool want_errors : {true, false}) {
        auto [client_end, service_end] = RemoteChannel::pair();
        std::thread service([&smgr, service_end = service_end] {
            auto frame = service_end->receive();
            ASSERT_TRUE(frame && frame->kind == Wire::InstallRequest);
            auto q = decode_install_request(frame->body);
            ASSERT_TRUE(q);
            const InstallResult result = serve_install_request(smgr, *service_end, *q);
            WireWriter w;
            w.u32_(result.rc);
            w.u8_(0);
            w.u8_(0);
            (void)service_end->send(Wire::InstallDone, w.data());
        });
        InstallRequest q;
        q.package_path = "C:\\svc.msi";
        q.action = "INSTALL";
        q.ui_level = 2;  // INSTALLUILEVEL_NONE
        q.message_filter = log_mode_bit(kActionStart) | (want_errors ? log_mode_bit(kError) : 0u);
        q.identity.user_sid = "S-1-5-21-1-2-3-1001";
        std::vector<std::pair<u32, std::string>> seen;
        InstallClient client;
        client.message = [&](u32 type, const Record* record, const std::string& text) -> i32 {
            std::string action;
            if ((type & 0xFF000000u) == kActionStart && record && record->count() >= 1)
                action = utf8(record->fields[1].text);
            seen.emplace_back(type & 0xFF000000u, (type & 0xFF000000u) == kActionStart ? action : text);
            return 0;
        };
        const InstallResult result = run_in_service(*client_end, q, client);
        service.join();
        EXPECT_EQ(result.rc, 1603u);  // type 19 always fails
        bool set_started = false, error_seen = false, other = false;
        for (const auto& [type, text] : seen) {
            if (type == kActionStart && text == "CaSet") set_started = true;
            else if (type == kError && text == "literal hello") error_seen = true;
            else if (type != kActionStart && type != kError) other = true;
        }
        EXPECT_TRUE(set_started);
        EXPECT_EQ(error_seen, want_errors);
        EXPECT_FALSE(other);  // outside the filter: answered in the service
    }
    std::filesystem::remove_all(root);
}

// While the service's relay is set, route_message hands every message to it.
TEST_P(MsiRemote, RouteMessageUsesTheClientRelay) {
    int calls = 0;
    set_client_ui_relay(mgr, [&](u32 type, const Record*, const std::string& text) -> i32 {
        ++calls;
        EXPECT_EQ(type, kInfo);
        EXPECT_EQ(text, "formatted");
        return 6;  // IDYES
    });
    Record r(0);
    const RecordFormatter format = [](const Record&) { return std::string("formatted"); };
    EXPECT_EQ(route_message(mgr, nullptr, nullptr, kInfo, &r, format), 6);
    set_client_ui_relay(mgr, nullptr);
    EXPECT_EQ(route_message(mgr, nullptr, nullptr, kInfo, &r, format), 0);
    EXPECT_EQ(calls, 1);
}

INSTANTIATE_MSI_BITNESS(MsiRemote);

}  // namespace
}  // namespace vitreo::winapi::msi_test
