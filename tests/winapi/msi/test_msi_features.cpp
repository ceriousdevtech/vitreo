// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_features.cpp
/// @brief Contract tests for the Windows Installer features area (costing,
/// directories and target paths, the feature/component state machine, the
/// feature-family exports), both bitnesses through msi_fixture.hpp.
///
/// Expected values are Windows 11's:
///  * tools/oracle_probe/msi_features_probe{32,64}.expected — the probe's
///    MsiOpenPackage sessions over tests/winapi/msi/fixtures/feat*.msi. The
///    scenario test replays every scenario of that file (its `set` / `op`
///    lines) through Vitreo's exports and compares the probe's own lines.
///  * tools/oracle_probe/msi_features_msiexec.expected — msiexec /l*v
///    Feature:/Component: lines for command-line requests.
/// Package and probe: tools/oracle_probe/msi_features_build_packages.ps1,
/// msi_features_probe.c, msi_features_run.sh.

#include "msi_fixture.hpp"

#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_features.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace vitreo::winapi::msi_test {
namespace {

namespace fs = std::filesystem;

const fs::path kRepo = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();

std::vector<std::string> read_lines(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

const char* kFeatures[] = {"Root", "Child", "High", "Cond", "Follow", "Follow2", "Src",
                           "Adv",  "NoAdv", "Zero", "Must", "Other",  "Empty"};
const char* kComponents[] = {"cRoot",  "cChild",  "cShared", "cOpt",     "cSrcOnly",  "cCond",      "cCondT",
                             "c64",    "cReg",    "cNever",  "cNoGuid",  "cPerm",     "cAdv",       "cNoAdv",
                             "cFollow", "cMust",  "cCondF",  "cUnlinked", "cIsoApp",  "cIsoShared", "cSrcDot",
                             "cNoKey", "cRegA",   "cRegB",   "cRegC"};
const char* kDirs[] = {"TARGETDIR", "ProgramFilesFolder", "APPDIR", "BINDIR",    "DOTDIR",   "SRCDOT",
                       "DATADIR",   "NODIR",              "PROPDIR", "PROPCHILD", "SourceDir"};
/// The probe's properties this area sets (or reads back unchanged) and
/// that do not depend on the oracle machine's free space or on costing
/// timing (CostingComplete: Windows costs in the background).
const std::set<std::string> kComparedProps = {
    "OutOfDiskSpace", "OutOfNoRbDiskSpace", "PrimaryVolumePath", "PRIMARYFOLDER", "Preselected", "ROOTDRIVE",
    "TARGETDIR", "SourceDir", "SOURCEDIR", "APPDIR", "BINDIR", "DOTDIR", "SRCDOT", "DATADIR", "NODIR", "PROPDIR",
    "PROPCHILD", "ProgramFilesFolder", "INSTALLLEVEL", "ADDLOCAL", "REMOVE", "SHORTFILENAMES", "ADDSOURCE",
    "ADDDEFAULT", "ADVERTISE", "REINSTALL", "PrimaryVolumeSpaceRequired"};

class MsiFeatures : public MsiTest {
protected:
    static constexpr GuestAddr NAME = DATA + 0x8000;
    static constexpr GuestAddr VAL = DATA + 0x9000;
    static constexpr GuestAddr OUTBUF = DATA + 0xA000;
    static constexpr GuestAddr PCCH = DATA + 0xB000;
    static constexpr GuestAddr O1 = DATA + 0xB100;
    static constexpr GuestAddr O2 = DATA + 0xB110;

    void SetUp() override {
        MsiTest::SetUp();
        root = fs::temp_directory_path() / ("vitreo_msi_feat_" + std::to_string(::getpid()) + (x64() ? "_64" : "_32"));
        fs::remove_all(root);
        // The oracle's layout: packages in C:\Users\Owner\msifeat, the
        // never-overwrite key file in C:\VitreoNoDir.
        fs::create_directories(root / "Users" / "Owner" / "msifeat");
        fs::create_directories(root / "VitreoNoDir");
        std::ofstream(root / "VitreoNoDir" / "never.txt").close();
        for (const char* p : {"feat32.msi", "feat64.msi", "featsn.msi"})
            fs::copy_file(kRepo / "tests" / "winapi" / "msi" / "fixtures" / p, root / "Users" / "Owner" / "msifeat" / p);
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_vfs(&vfs);
    }
    void TearDown() override {
        close();
        fs::remove_all(root);
    }

    u32 open(const std::string& package) {
        close();
        write_ansi(NAME, "C:\\Users\\Owner\\msifeat\\" + package);
        const u32 rc = static_cast<u32>(call("MsiOpenPackageA", {I(NAME), I(OUT)}));
        h = rc == 0 ? static_cast<u32>(read_int(OUT)) : 0;
        if (h != 0) {
            // The oracle's ProgramFilesFolder (a 32-bit view of 64-bit
            // Windows; the engine area computes Vitreo's own).
            set("ProgramFilesFolder", "C:\\Program Files (x86)\\");
        }
        return rc;
    }
    void close() {
        if (h != 0) (void)call("MsiCloseHandle", {I(h)});
        h = 0;
    }

    void write_ansi(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) ASSERT_TRUE(memory.write_u8(at + i, static_cast<u8>(s[i])).is_ok());
        ASSERT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
    }
    std::string read_ansi(GuestAddr at, usize max = 1024) {
        std::string out;
        for (usize i = 0; i < max; ++i) {
            const u8 c = memory.read_u8(at + i).value();
            if (c == 0) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    }
    u32 set(const std::string& name, const std::string& value) {
        write_ansi(NAME, name);
        write_ansi(VAL, value);
        return static_cast<u32>(call("MsiSetPropertyA", {I(h), I(NAME), I(VAL)}));
    }
    std::string prop(const std::string& name) {
        write_ansi(NAME, name);
        write_int(PCCH, 1024);
        if (call("MsiGetPropertyA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}) != 0) return "<err>";
        return read_ansi(OUTBUF);
    }
    u32 action(const std::string& a) {
        write_ansi(NAME, a);
        return static_cast<u32>(call("MsiDoActionA", {I(h), I(NAME)}));
    }

    /// One probe "op" (msi_features_probe.c op()).
    u32 op(const std::string& o) {
        if (o == "V") return static_cast<u32>(call("MsiVerifyDiskSpace", {I(h)}));
        const usize eq = o.find('=', 2);
        const std::string key = o.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
        const std::string val = eq == std::string::npos ? std::string{} : o.substr(eq + 1);
        write_ansi(NAME, key);
        write_ansi(VAL, val);
        switch (o[0]) {
        case 'F': return static_cast<u32>(call("MsiSetFeatureStateA", {I(h), I(NAME), I(static_cast<u32>(std::stoi(val)))}));
        case 'C': return static_cast<u32>(call("MsiSetComponentStateA", {I(h), I(NAME), I(static_cast<u32>(std::stoi(val)))}));
        case 'L': return static_cast<u32>(call("MsiSetInstallLevel", {I(h), I(static_cast<u32>(std::stoi(key)))}));
        case 'T': return static_cast<u32>(call("MsiSetTargetPathA", {I(h), I(NAME), I(VAL)}));
        case 'A': return static_cast<u32>(call("MsiSetFeatureAttributesA", {I(h), I(NAME), I(static_cast<u32>(std::stoi(val)))}));
        case 'D': return action(key);
        case 'P': return set(key, val);
        case 'V': return static_cast<u32>(call("MsiVerifyDiskSpace", {I(h)}));
        }
        return ~0u;
    }

    // The probe's dump lines, produced through Vitreo's A exports.
    std::vector<std::string> states() {
        std::vector<std::string> out;
        char line[512];
        for (const char* f : kFeatures) {
            write_ansi(NAME, f);
            write_ints(O1, {99, 99});
            const u32 rc = static_cast<u32>(call("MsiGetFeatureStateA", {I(h), I(NAME), I(O1), I(O1 + 4)}));
            write_int(O2, 0xFFFF);
            const u32 rv = static_cast<u32>(call("MsiGetFeatureValidStatesA", {I(h), I(NAME), I(O2)}));
            std::snprintf(line, sizeof(line), "  feature %s rc=%u installed=%d action=%d valid rc=%u 0x%x", f, rc,
                          read_int(O1), read_int(O1 + 4), rv, static_cast<u32>(read_int(O2)));
            out.push_back(line);
        }
        for (const char* c : kComponents) {
            write_ansi(NAME, c);
            write_ints(O1, {99, 99});
            const u32 rc = static_cast<u32>(call("MsiGetComponentStateA", {I(h), I(NAME), I(O1), I(O1 + 4)}));
            std::snprintf(line, sizeof(line), "  component %s rc=%u installed=%d action=%d", c, rc, read_int(O1),
                          read_int(O1 + 4));
            out.push_back(line);
        }
        return out;
    }
    std::vector<std::string> paths() {
        std::vector<std::string> out;
        for (const char* d : kDirs) {
            write_ansi(NAME, d);
            write_int(PCCH, 1024);
            const u32 rt = static_cast<u32>(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}));
            const std::string t = rt == 0 ? read_ansi(OUTBUF) : "";
            write_int(PCCH, 1024);
            const u32 rs = static_cast<u32>(call("MsiGetSourcePathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}));
            const std::string s = rs == 0 ? read_ansi(OUTBUF) : "";
            out.push_back("  dir " + std::string(d) + " target rc=" + std::to_string(rt) + " [" + t + "] source rc=" +
                          std::to_string(rs) + " [" + s + "]");
        }
        return out;
    }
    std::vector<std::string> costs() {
        std::vector<std::string> out;
        const i32 st[] = {-1, 3, 4, 2, 5};
        for (const char* f : kFeatures) {
            std::string line = std::string("  cost ") + f;
            write_ansi(NAME, f);
            for (i32 s : st) {
                for (int tree = 0; tree < 3; ++tree) {
                    write_int(O1, -12345);
                    const u32 rc = static_cast<u32>(call(
                        "MsiGetFeatureCostA", {I(h), I(NAME), I(static_cast<u32>(tree)), I(static_cast<u32>(s)), I(O1)}));
                    line += " " + std::to_string(s) + "/" + std::to_string(tree) + ":" + std::to_string(rc) + ":" +
                            std::to_string(read_int(O1));
                }
            }
            out.push_back(line);
        }
        for (usize i = 0; i <= std::size(kComponents); ++i) {
            const std::string c = i < std::size(kComponents) ? kComponents[i] : "";
            for (i32 s : st) {
                std::string line = "  ccost " + (c.empty() ? std::string("(null)") : c) + " state=" + std::to_string(s);
                write_ansi(NAME, c);
                for (u32 idx = 0; idx < 4; ++idx) {
                    write_int(PCCH, 64);
                    write_ints(O1, {-12345, -12345});
                    write_ansi(OUTBUF, "sentinel");
                    const u32 rc = static_cast<u32>(call("MsiEnumComponentCostsA",
                                                         {I(h), I(c.empty() ? 0 : NAME), I(idx), I(static_cast<u32>(s)),
                                                          I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}));
                    line += " [" + std::to_string(idx) + " rc=" + std::to_string(rc) + " " + read_ansi(OUTBUF) +
                            " n=" + std::to_string(read_int(PCCH)) + " cost=" + std::to_string(read_int(O1)) +
                            " temp=" + std::to_string(read_int(O1 + 4)) + "]";
                    if (rc != 0) break;
                }
                out.push_back(line);
            }
        }
        return out;
    }

    fs::path root;
    capsule::Vfs vfs;
    u32 h{0};
};

struct Scenario {
    std::string name, package;
    std::vector<std::string> props, before, after;
    /// Expected lines after CostFinalize (index 0) and after each after-op.
    std::vector<std::vector<std::string>> dumps;
    std::vector<u32> after_rc, before_rc;
    u32 finalize_rc{0};
};

std::vector<Scenario> parse_scenarios(const std::vector<std::string>& lines) {
    std::vector<Scenario> out;
    Scenario* cur = nullptr;
    bool finalized = false;
    for (const std::string& l : lines) {
        if (l.rfind("== scenario ", 0) == 0) {
            out.emplace_back();
            cur = &out.back();
            std::istringstream is(l.substr(12));
            is >> cur->name >> cur->package;
            finalized = false;
            continue;
        }
        if (l.rfind("== ", 0) == 0) {
            cur = nullptr;
            continue;
        }
        if (cur == nullptr) continue;
        auto rc_of = [](const std::string& s) { return static_cast<u32>(std::stoul(s.substr(s.rfind("rc=") + 3))); };
        if (l.rfind(" set ", 0) == 0) {
            cur->props.push_back(l.substr(5, l.rfind(" rc=") - 5));
        } else if (l.rfind(" op ", 0) == 0) {
            const std::string o = l.substr(4, l.rfind(" rc=") - 4);
            if (finalized) {
                cur->after.push_back(o);
                cur->after_rc.push_back(rc_of(l));
                cur->dumps.emplace_back();
            } else {
                cur->before.push_back(o);
                cur->before_rc.push_back(rc_of(l));
            }
        } else if (l.rfind(" CostFinalize rc=", 0) == 0) {
            cur->finalize_rc = rc_of(l);
            finalized = true;
            cur->dumps.emplace_back();
        } else if (l.rfind("  ", 0) == 0 && finalized) {
            cur->dumps.back().push_back(l);
        }
    }
    return out;
}

std::string line_key(const std::string& l) {
    // "  feature Root", "  component cRoot", "  dir APPDIR", "  cost Root",
    // "  ccost cRoot state=3".
    const usize a = l.find(' ', l.find_first_not_of(' '));
    const usize b = l.find(' ', a + 1);
    if (l.rfind("  ccost ", 0) == 0) return l.substr(0, l.find(' ', b + 1));
    return l.substr(0, b);
}

/// The lines of @p expected this area reproduces exactly (see the file
/// comment and .docs/contracts/msi/features.md for the exclusions).
void compare_dump(const Scenario& sc, usize index, const std::vector<std::string>& expected,
                  const std::vector<std::string>& actual, const std::map<std::string, std::string>& props) {
    std::map<std::string, std::string> act;
    for (const std::string& l : actual) act[line_key(l)] = l;
    // Costing is complete on Windows only after InstallValidate.
    const bool costed = index > 0 && sc.after[index - 1] == "D:InstallValidate";
    for (const std::string& e : expected) {
        if (sc.name == "shortfilenames_prop" && e.rfind("  dir ", 0) == 0) continue;
        if ((e.rfind("  cost ", 0) == 0 || e.rfind("  ccost ", 0) == 0) && !costed) continue;
        if (e.rfind("  prop ", 0) == 0) {
            const std::string name = e.substr(7, e.find(' ', 7) - 7);
            if (!kComparedProps.count(name)) continue;
            if ((name == "PrimaryVolumeSpaceRequired" || name == "PrimaryVolumePath") && !costed) continue;
            if (sc.name == "shortfilenames_prop" && name != "SHORTFILENAMES") continue;
            auto it = props.find(name);
            EXPECT_EQ(it == props.end() ? std::string("<missing>") : "[" + it->second + "]", e.substr(e.find('[')))
                << sc.name << " dump " << index << " prop " << name;
            continue;
        }
        auto it = act.find(line_key(e));
        EXPECT_EQ(it == act.end() ? std::string("<missing>") : it->second, e) << sc.name << " dump " << index;
    }
}

TEST_P(MsiFeatures, ProbeScenariosMatchWindows) {
    const fs::path file =
        kRepo / "tests" / "oracle_data" / (x64() ? "msi_features_probe64.expected" : "msi_features_probe32.expected");
    const auto scenarios = parse_scenarios(read_lines(file));
    ASSERT_GT(scenarios.size(), 40u) << file;
    for (const Scenario& sc : scenarios) {
        SCOPED_TRACE(sc.name);
        ASSERT_EQ(open(sc.package), 0u);
        for (const std::string& p : sc.props) {
            const usize eq = p.find('=');
            ASSERT_EQ(set(p.substr(0, eq), p.substr(eq + 1)), 0u);
        }
        EXPECT_EQ(action("CostInitialize"), 0u);
        EXPECT_EQ(action("FileCost"), 0u);
        for (usize i = 0; i < sc.before.size(); ++i) EXPECT_EQ(op(sc.before[i]), sc.before_rc[i]) << sc.before[i];
        EXPECT_EQ(action("CostFinalize"), sc.finalize_rc);
        auto dump = [&](usize index) {
            const auto& exp = sc.dumps[index];
            std::vector<std::string> lines = states();
            std::map<std::string, std::string> props;
            const bool full =
                std::any_of(exp.begin(), exp.end(), [](const std::string& l) { return l.rfind("  dir ", 0) == 0; });
            if (full) {
                for (auto& l : paths()) lines.push_back(l);
                for (auto& l : costs()) lines.push_back(l);
                for (const std::string& name : kComparedProps) props[name] = prop(name);
            }
            compare_dump(sc, index, exp, lines, props);
        };
        dump(0);
        for (usize i = 0; i < sc.after.size(); ++i) {
            // InstallValidate belongs to the engine area; the probe runs it
            // to complete Windows' background costing.
            const u32 rc = op(sc.after[i]);
            if (sc.after[i] != "D:InstallValidate") EXPECT_EQ(rc, sc.after_rc[i]) << sc.after[i];
            dump(i + 1);
        }
        close();
    }
}


// ---- explicit contracts (probe "phase", "handles", "arguments", "costed
// arguments" sections) ----------------------------------------------------------

struct PhaseExpect {
    u32 get_feature, get_component, valid, cost, enum_comp, enum_null, target, source, verify, set_attrs, set_feature,
        set_component, set_level, set_target;
};

TEST_P(MsiFeatures, ErrorContractPerCostingPhase) {
    ASSERT_EQ(open("feat32.msi"), 0u);
    auto check = [&](const PhaseExpect& e, const char* phase) {
        SCOPED_TRACE(phase);
        write_ansi(NAME, "Root");
        EXPECT_EQ(call("MsiGetFeatureStateA", {I(h), I(NAME), I(O1), I(O1 + 4)}), e.get_feature);
        write_ansi(NAME, "cRoot");
        EXPECT_EQ(call("MsiGetComponentStateA", {I(h), I(NAME), I(O1), I(O1 + 4)}), e.get_component);
        write_ansi(NAME, "Root");
        EXPECT_EQ(call("MsiGetFeatureValidStatesA", {I(h), I(NAME), I(O1)}), e.valid);
        EXPECT_EQ(call("MsiGetFeatureCostA", {I(h), I(NAME), I(0), I(3), I(O1)}), e.cost);
        write_ansi(NAME, "cRoot");
        write_int(PCCH, 256);
        EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O2)}),
                  e.enum_comp);
        write_int(PCCH, 256);
        EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(0), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O2)}),
                  e.enum_null);
        write_ansi(NAME, "APPDIR");
        write_int(PCCH, 256);
        EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), e.target);
        write_int(PCCH, 256);
        EXPECT_EQ(call("MsiGetSourcePathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), e.source);
        if (e.source == 0) EXPECT_EQ(read_ansi(OUTBUF), "C:\\Users\\Owner\\msifeat\\srcapp\\");
        EXPECT_EQ(call("MsiVerifyDiskSpace", {I(h)}), e.verify);
        write_ansi(NAME, "Root");
        EXPECT_EQ(call("MsiSetFeatureAttributesA", {I(h), I(NAME), I(0)}), e.set_attrs);
        write_ansi(NAME, "Child");
        EXPECT_EQ(call("MsiSetFeatureStateA", {I(h), I(NAME), I(3)}), e.set_feature);
        write_ansi(NAME, "cChild");
        EXPECT_EQ(call("MsiSetComponentStateA", {I(h), I(NAME), I(3)}), e.set_component);
        EXPECT_EQ(call("MsiSetInstallLevel", {I(h), I(3)}), e.set_level);
        write_ansi(NAME, "BINDIR");
        write_ansi(VAL, "C:\\PhaseBin\\");
        EXPECT_EQ(call("MsiSetTargetPathA", {I(h), I(NAME), I(VAL)}), e.set_target);
    };
    // probe "== phase open/costinitialize/filecost/costfinalize". After
    // CostFinalize Windows still answers MsiEnumComponentCosts 1626 until its
    // background costing ends; Vitreo costs synchronously (features.md).
    check({1606, 1607, 1609, 1609, 1609, 1626, 267, 267, 0, 1627, 1627, 1627, 1627, 267}, "open");
    ASSERT_EQ(action("CostInitialize"), 0u);
    check({0, 0, 0, 0, 1626, 1626, 267, 0, 0, 0, 1627, 1627, 1627, 267}, "costinitialize");
    write_ansi(NAME, "Root");
    write_ints(O1, {99, 99});
    ASSERT_EQ(call("MsiGetFeatureStateA", {I(h), I(NAME), I(O1), I(O1 + 4)}), 0u);
    EXPECT_EQ(read_int(O1), -1);
    EXPECT_EQ(read_int(O1 + 4), -1);
    EXPECT_EQ(call("MsiGetFeatureValidStatesA", {I(h), I(NAME), I(O1)}), 0u);
    EXPECT_EQ(read_int(O1), 0xe);
    EXPECT_EQ(prop("CostingComplete"), "0");
    ASSERT_EQ(action("FileCost"), 0u);
    ASSERT_EQ(action("CostFinalize"), 0u);
    check({0, 0, 0, 0, 0, 0, 0, 0, 0, 1627, 0, 0, 0, 0}, "costfinalize");
    EXPECT_EQ(prop("BINDIR"), "C:\\PhaseBin\\");
    write_ansi(NAME, "Root");
    ASSERT_EQ(call("MsiGetFeatureStateA", {I(h), I(NAME), I(O1), I(O1 + 4)}), 0u);
    EXPECT_EQ(read_int(O1), 2);
    EXPECT_EQ(read_int(O1 + 4), 3);
    write_ansi(NAME, "TARGETDIR");
    write_int(PCCH, 256);
    ASSERT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
    EXPECT_EQ(read_ansi(OUTBUF), "C:\\");
    EXPECT_EQ(read_int(PCCH), 3);
}

TEST_P(MsiFeatures, InvalidHandles) {
    // probe "== handles": every export is ERROR_INVALID_HANDLE for 0, an
    // unknown handle and a record handle.
    const u32 rec = static_cast<u32>(call("MsiCreateRecord", {I(1)}));
    for (u32 bad : {0u, 0xDEADu, rec}) {
        write_ansi(NAME, "Root");
        write_int(PCCH, 256);
        EXPECT_EQ(call("MsiGetFeatureStateA", {I(bad), I(NAME), I(O1), I(O1 + 4)}), 6u);
        EXPECT_EQ(call("MsiGetComponentStateW", {I(bad), I(NAME), I(O1), I(O1 + 4)}), 6u);
        EXPECT_EQ(call("MsiSetFeatureStateA", {I(bad), I(NAME), I(3)}), 6u);
        EXPECT_EQ(call("MsiSetComponentStateA", {I(bad), I(NAME), I(3)}), 6u);
        EXPECT_EQ(call("MsiGetFeatureValidStatesA", {I(bad), I(NAME), I(O1)}), 6u);
        EXPECT_EQ(call("MsiGetFeatureCostA", {I(bad), I(NAME), I(0), I(3), I(O1)}), 6u);
        EXPECT_EQ(call("MsiEnumComponentCostsA", {I(bad), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O2)}), 6u);
        EXPECT_EQ(call("MsiGetTargetPathA", {I(bad), I(NAME), I(OUTBUF), I(PCCH)}), 6u);
        EXPECT_EQ(call("MsiGetSourcePathW", {I(bad), I(NAME), I(OUTBUF), I(PCCH)}), 6u);
        EXPECT_EQ(call("MsiSetTargetPathA", {I(bad), I(NAME), I(NAME)}), 6u);
        EXPECT_EQ(call("MsiSetInstallLevel", {I(bad), I(3)}), 6u);
        EXPECT_EQ(call("MsiSetFeatureAttributesA", {I(bad), I(NAME), I(0)}), 6u);
        EXPECT_EQ(call("MsiVerifyDiskSpace", {I(bad)}), 6u);
    }
}

class MsiFeaturesFinalized : public MsiFeatures {
protected:
    void SetUp() override {
        MsiFeatures::SetUp();
        ASSERT_EQ(open("feat32.msi"), 0u);
        ASSERT_EQ(action("CostInitialize"), 0u);
        ASSERT_EQ(action("FileCost"), 0u);
        ASSERT_EQ(action("CostFinalize"), 0u);
    }
    u32 get_state(const char* api, const char* name, GuestAddr installed, GuestAddr act) {
        if (name) write_ansi(NAME, name);
        return static_cast<u32>(call(api, {I(h), I(name ? NAME : 0), I(installed), I(act)}));
    }
};

TEST_P(MsiFeaturesFinalized, StateArguments) {
    // probe "== arguments".
    write_ints(O1, {99, 99});
    EXPECT_EQ(get_state("MsiGetFeatureStateA", "Nope", O1, O1 + 4), 1606u);
    EXPECT_EQ(read_int(O1), 99);
    EXPECT_EQ(get_state("MsiGetFeatureStateA", "", O1, O1 + 4), 1606u);
    EXPECT_EQ(get_state("MsiGetFeatureStateA", nullptr, O1, O1 + 4), 1606u);
    EXPECT_EQ(get_state("MsiGetFeatureStateA", "root", O1, O1 + 4), 1606u);
    EXPECT_EQ(get_state("MsiGetFeatureStateA", "Root", 0, O1 + 4), 0u);
    EXPECT_EQ(get_state("MsiGetFeatureStateA", "Root", O1, 0), 0u);
    EXPECT_EQ(get_state("MsiGetComponentStateA", "Nope", O1, O1 + 4), 1607u);
    EXPECT_EQ(get_state("MsiGetComponentStateA", nullptr, O1, O1 + 4), 1607u);
    EXPECT_EQ(get_state("MsiGetComponentStateA", "cRoot", 0, O1 + 4), 0u);
    EXPECT_EQ(get_state("MsiGetComponentStateA", "cRoot", O1, 0), 0u);
    write_ansi(NAME, "Src");
    for (i32 st = -7; st <= 7; ++st) {
        const u32 want = (st >= 1 && st <= 4) ? 0u : 87u;
        EXPECT_EQ(call("MsiSetFeatureStateA", {I(h), I(NAME), I(static_cast<u32>(st))}), want) << st;
    }
    write_ansi(NAME, "Nope");
    EXPECT_EQ(call("MsiSetFeatureStateA", {I(h), I(NAME), I(3)}), 1606u);
    EXPECT_EQ(call("MsiSetFeatureStateA", {I(h), I(0), I(3)}), 1606u);
    write_ansi(NAME, "cOpt");
    for (i32 st = -7; st <= 7; ++st) {
        const u32 want = (st >= 2 && st <= 4) ? 0u : 87u;
        EXPECT_EQ(call("MsiSetComponentStateA", {I(h), I(NAME), I(static_cast<u32>(st))}), want) << st;
    }
    write_ansi(NAME, "Nope");
    EXPECT_EQ(call("MsiSetComponentStateA", {I(h), I(NAME), I(3)}), 1607u);
    EXPECT_EQ(call("MsiGetFeatureValidStatesA", {I(h), I(NAME), I(O1)}), 1606u);
    write_ansi(NAME, "Root");
    EXPECT_EQ(call("MsiGetFeatureValidStatesA", {I(h), I(NAME), I(0)}), 87u);
    EXPECT_EQ(call("MsiGetFeatureValidStatesA", {I(h), I(0), I(O1)}), 87u);
    write_ansi(NAME, "Child");
    for (i32 tree = -1; tree <= 3; ++tree)
        EXPECT_EQ(call("MsiGetFeatureCostA", {I(h), I(NAME), I(static_cast<u32>(tree)), I(3), I(O1)}),
                  (tree >= 0 && tree <= 2) ? 0u : 87u)
            << tree;
    for (i32 st = -7; st <= 7; ++st) {
        const bool ok = st == -1 || (st >= 2 && st <= 5);
        EXPECT_EQ(call("MsiGetFeatureCostA", {I(h), I(NAME), I(0), I(static_cast<u32>(st)), I(O1)}), ok ? 0u : 87u)
            << st;
    }
    EXPECT_EQ(call("MsiGetFeatureCostA", {I(h), I(NAME), I(0), I(3), I(0)}), 87u);
    write_ansi(NAME, "Nope");
    EXPECT_EQ(call("MsiGetFeatureCostA", {I(h), I(NAME), I(0), I(3), I(O1)}), 1606u);
}

TEST_P(MsiFeaturesFinalized, EnumComponentCostsArguments) {
    // probe "== costed arguments" (Windows answers these after its costing
    // completes; Vitreo's costing completes at CostFinalize).
    write_ansi(NAME, "cChild");
    for (i32 st = -7; st <= 7; ++st) {
        write_int(PCCH, 256);
        write_ints(O1, {-12345, -12345});
        const u32 rc = static_cast<u32>(
            call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(static_cast<u32>(st)), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}));
        const bool ok = st == -1 || (st >= 2 && st <= 5);
        EXPECT_EQ(rc, ok ? 0u : 87u) << st;
        if (!ok) continue;
        EXPECT_EQ(read_ansi(OUTBUF), "C:");
        EXPECT_EQ(read_int(PCCH), 2);
        EXPECT_EQ(read_int(O1), (st == 3 || st == 5) ? 16 : 0) << st;
        EXPECT_EQ(read_int(O1 + 4), 0);
    }
    // cap 0..4: 234 for 0..2 (costs still written), 0 from 3.
    for (u32 cap = 0; cap < 5; ++cap) {
        write_int(PCCH, static_cast<i32>(cap));
        write_ansi(OUTBUF, "sentinel");
        write_ints(O1, {-12345, -12345});
        const u32 rc = static_cast<u32>(
            call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}));
        EXPECT_EQ(rc, cap < 3 ? 234u : 0u) << cap;
        EXPECT_EQ(read_int(PCCH), 2);
        EXPECT_EQ(read_int(O1), 16);
        EXPECT_EQ(read_ansi(OUTBUF), cap == 0 ? "sentinel" : cap == 1 ? "" : cap == 2 ? "C" : "C:") << cap;
    }
    write_int(PCCH, 10);
    EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(0), I(PCCH), I(O1), I(O1 + 4)}), 87u);
    EXPECT_EQ(read_int(PCCH), 10);
    EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(0), I(O1), I(O1 + 4)}), 87u);
    write_int(PCCH, 256);
    EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(0), I(O1 + 4)}), 87u);
    EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(0)}), 87u);
    EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(1), I(3), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}), 259u);
    EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(7), I(3), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}), 259u);
    write_ansi(NAME, "Nope");
    EXPECT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}), 1607u);
    // The whole install: C: cost 0, temp 2 x the 24 KiB package = 96 units.
    write_ansi(NAME, "");
    write_int(PCCH, 256);
    ASSERT_EQ(call("MsiEnumComponentCostsA", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}), 0u);
    EXPECT_EQ(read_ansi(OUTBUF), "C:");
    EXPECT_EQ(read_int(O1), 0);
    EXPECT_EQ(read_int(O1 + 4), 96);
    // W: cap 2 is 234 with "C", cap 3 fits.
    write_wide(NAME, u"cChild");
    write_int(PCCH, 2);
    EXPECT_EQ(call("MsiEnumComponentCostsW", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}), 234u);
    EXPECT_EQ(read_int(PCCH), 2);
    EXPECT_EQ(memory.read_u16(OUTBUF).value(), u'C');
    EXPECT_EQ(memory.read_u16(OUTBUF + 2).value(), 0);
    write_int(PCCH, 3);
    EXPECT_EQ(call("MsiEnumComponentCostsW", {I(h), I(NAME), I(0), I(3), I(OUTBUF), I(PCCH), I(O1), I(O1 + 4)}), 0u);
    EXPECT_EQ(memory.read_u16(OUTBUF + 2).value(), u':');
}

TEST_P(MsiFeaturesFinalized, PathBufferConvention) {
    // probe "TargetPath cap ..": APPDIR is 44 characters.
    const std::string appdir = "C:\\Program Files (x86)\\Vitreo Features Test\\";
    write_ansi(NAME, "APPDIR");
    for (u32 cap = 0; cap < 4; ++cap) {
        write_int(PCCH, static_cast<i32>(cap));
        write_ansi(OUTBUF, "sentinel");
        EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
        EXPECT_EQ(read_int(PCCH), 44);
        EXPECT_EQ(read_ansi(OUTBUF), cap == 0 ? std::string("sentinel") : appdir.substr(0, cap - 1));
    }
    write_int(PCCH, 44);
    EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
    EXPECT_EQ(read_ansi(OUTBUF), appdir.substr(0, 43));
    write_int(PCCH, 45);
    EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
    EXPECT_EQ(read_ansi(OUTBUF), appdir);
    EXPECT_EQ(read_int(PCCH), 44);
    write_int(PCCH, 5);
    EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(0), I(PCCH)}), 0u);
    EXPECT_EQ(read_int(PCCH), 44);
    EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(0)}), 87u);
    EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(0), I(OUTBUF), I(PCCH)}), 87u);
    write_int(PCCH, 256);
    for (const char* bad : {"Nope", "appdir", "SourceDir"}) {
        write_ansi(NAME, bad);
        EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 267u) << bad;
    }
    write_ansi(NAME, "Nope");
    EXPECT_EQ(call("MsiGetSourcePathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 267u);
    // A root's DefaultDir names its source path (probe dir SourceDir lines).
    write_ansi(NAME, "SourceDir");
    write_int(PCCH, 256);
    EXPECT_EQ(call("MsiGetSourcePathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
    EXPECT_EQ(read_ansi(OUTBUF), "C:\\Users\\Owner\\msifeat\\");
    write_ansi(NAME, "APPDIR");
    write_int(PCCH, 0);
    EXPECT_EQ(call("MsiGetSourcePathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
    EXPECT_EQ(read_int(PCCH), 30);
    write_wide(NAME, u"APPDIR");
    write_int(PCCH, 0);
    EXPECT_EQ(call("MsiGetTargetPathW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
    EXPECT_EQ(read_int(PCCH), 44);
    write_int(PCCH, 3);
    EXPECT_EQ(call("MsiGetTargetPathW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
    EXPECT_EQ(memory.read_u16(OUTBUF).value(), u'C');
    EXPECT_EQ(memory.read_u16(OUTBUF + 2).value(), u':');
    EXPECT_EQ(memory.read_u16(OUTBUF + 4).value(), 0);
}

TEST_P(MsiFeaturesFinalized, SetTargetPathValidationAndPropagation) {
    // probe "SetTargetPath(..)" lines: rc, then APPDIR / DOTDIR / APPDIR property.
    struct Case {
        const char* dir;
        const char* path;
        u32 rc;
        const char* appdir;
        const char* dotdir;
    };
    const std::string pf = "C:\\Program Files (x86)\\Vitreo Features Test\\";
    const Case cases[] = {
        {"Nope", "C:\\x\\", 267, nullptr, nullptr},
        {"APPDIR", "relative\\", 267, nullptr, nullptr},
        {"APPDIR", "C:", 0, "C:\\", "C:\\"},
        {"APPDIR", "C:\\", 0, "C:\\", "C:\\"},
        {"APPDIR", "Q:\\nodrive\\", 267, "C:\\", "C:\\"},
        {"APPDIR", "", 267, "C:\\", "C:\\"},
        {"APPDIR", "C:\\a\\..\\b\\", 267, "C:\\", "C:\\"},
        {"APPDIR", "C:/fwd/slash/", 267, "C:\\", "C:\\"},
        {"APPDIR", "\\\\server\\share\\dir\\", 267, "C:\\", "C:\\"},
        {"APPDIR", "C:\\bad<name\\", 267, "C:\\", "C:\\"},
        {"appdir", "C:\\case\\", 267, "C:\\", "C:\\"},
        {"SourceDir", "C:\\srcdir\\", 267, "C:\\", "C:\\"},
        {"DOTDIR", "C:\\dot\\", 0, "C:\\", "C:\\dot\\"},
        {"APPDIR", "C:\\Final Place\\", 0, "C:\\Final Place\\", "C:\\dot\\"},
    };
    for (const Case& c : cases) {
        write_ansi(NAME, c.dir);
        write_ansi(VAL, c.path);
        EXPECT_EQ(call("MsiSetTargetPathA", {I(h), I(NAME), I(VAL)}), c.rc) << c.dir << " " << c.path;
        write_ansi(NAME, "APPDIR");
        write_int(PCCH, 256);
        ASSERT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
        EXPECT_EQ(read_ansi(OUTBUF), c.appdir ? c.appdir : pf) << c.path;
        EXPECT_EQ(prop("APPDIR"), c.appdir ? c.appdir : pf);
        write_ansi(NAME, "DOTDIR");
        write_int(PCCH, 256);
        ASSERT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
        EXPECT_EQ(read_ansi(OUTBUF), c.dotdir ? c.dotdir : pf) << c.path;
    }
    write_ansi(NAME, "APPDIR");
    EXPECT_EQ(call("MsiSetTargetPathA", {I(h), I(NAME), I(0)}), 87u);
    EXPECT_EQ(call("MsiSetTargetPathA", {I(h), I(0), I(NAME)}), 87u);
}

TEST_P(MsiFeaturesFinalized, InstallLevelAndAttributes) {
    // probe "SetInstallLevel(..) .. INSTALLLEVEL=[..]".
    const std::pair<i32, const char*> levels[] = {{0, "3"},          {-1, "3"},          {1, "1"},
                                                  {32767, "32767"},  {32768, "32768"},   {40000, "40000"},
                                                  {65536, "65536"},  {3, "3"}};
    for (const auto& [level, prop_value] : levels) {
        EXPECT_EQ(call("MsiSetInstallLevel", {I(h), I(static_cast<u32>(level))}), 0u);
        EXPECT_EQ(prop("INSTALLLEVEL"), prop_value) << level;
    }
    write_ansi(NAME, "Root");
    EXPECT_EQ(call("MsiSetFeatureAttributesA", {I(h), I(NAME), I(1)}), 1627u);
    // Between CostInitialize and CostFinalize: unknown/null 1606, any bits 0.
    ASSERT_EQ(open("feat32.msi"), 0u);
    ASSERT_EQ(action("CostInitialize"), 0u);
    write_ansi(NAME, "Nope");
    EXPECT_EQ(call("MsiSetFeatureAttributesA", {I(h), I(NAME), I(1)}), 1606u);
    EXPECT_EQ(call("MsiSetFeatureAttributesA", {I(h), I(0), I(1)}), 1606u);
    write_ansi(NAME, "Root");
    for (u32 bits : {64u, 0xFFFFu, 63u}) EXPECT_EQ(call("MsiSetFeatureAttributesA", {I(h), I(NAME), I(bits)}), 0u);
}

TEST_P(MsiFeatures, ActionPreconditions) {
    // probe: "CostFinalize alone rc=1603" (every path then 267), "FileCost
    // alone rc=1603", "IsolateComponents alone rc=1603", "MigrateFeatureStates
    // alone rc=1626", "CostFinalize without FileCost rc=0".
    ASSERT_EQ(open("feat32.msi"), 0u);
    EXPECT_EQ(action("CostFinalize"), 1603u);
    write_ansi(NAME, "TARGETDIR");
    write_int(PCCH, 256);
    EXPECT_EQ(call("MsiGetTargetPathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 267u);
    EXPECT_EQ(call("MsiGetSourcePathA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 267u);
    ASSERT_EQ(open("feat32.msi"), 0u);
    EXPECT_EQ(action("FileCost"), 1603u);
    EXPECT_EQ(action("IsolateComponents"), 1603u);
    EXPECT_EQ(action("MigrateFeatureStates"), 1626u);
    ASSERT_EQ(open("feat32.msi"), 0u);
    EXPECT_EQ(action("CostInitialize"), 0u);
    EXPECT_EQ(action("CostFinalize"), 0u);
}

TEST_P(MsiFeaturesFinalized, IsolateComponentsRecordsTheApplicationFolder) {
    ASSERT_EQ(open("feat32.msi"), 0u);
    ASSERT_EQ(action("CostInitialize"), 0u);
    ASSERT_EQ(action("IsolateComponents"), 0u);
    ASSERT_EQ(action("CostFinalize"), 0u);
    auto ih = mgr.handles().get<msi::InstallHandle>(h);
    ASSERT_TRUE(ih && ih->engine);
    const msi::ComponentInfo* shared = msi::find_component(ih->engine->session(), "cIsoShared");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->isolated_into, std::vector<std::string>{"cIsoApp"});
    EXPECT_EQ(msi::component_directory(ih->engine->session(), "cIsoApp"), "C:\\VitreoFeatProp\\");
}

// ---- msiexec command lines (msi_features_msiexec.expected) -------------------------

const char* state_name(i32 s) {
    switch (s) {
    case 1: return "Advertise";
    case 2: return "Absent";
    case 3: return "Local";
    case 4: return "Source";
    default: return "Null";
    }
}

TEST_P(MsiFeatures, MsiexecCommandLineSelectionsMatchWindows) {
    const auto lines = read_lines(kRepo / "tests" / "oracle_data" / "msi_features_msiexec.expected");
    std::ifstream in(root / "Users" / "Owner" / "msifeat" / "feat32.msi", std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    struct Run {
        std::string name;
        std::map<std::string, std::string> props;
        std::vector<std::string> lines;
        u32 exit{0};
    };
    std::vector<Run> runs;
    for (const std::string& l : lines) {
        if (l.rfind("== ", 0) == 0) {
            runs.emplace_back();
            runs.back().name = l.substr(3, l.find(' ', 3) - 3);
            runs.back().exit = static_cast<u32>(std::stoul(l.substr(l.find("exit=") + 5)));
        } else if (!runs.empty()) {
            runs.back().lines.push_back(l);
        }
    }
    // The command lines (msi_features_msiexec.ps1).
    const std::map<std::string, std::map<std::string, std::string>> cmd = {
        {"plain", {}},
        {"addlocal_all", {{"ADDLOCAL", "ALL"}}},
        {"addlocal_high", {{"ADDLOCAL", "High"}}},
        {"remove_all", {{"REMOVE", "ALL"}}},
        {"addsource_all_addlocal_child", {{"ADDSOURCE", "ALL"}, {"ADDLOCAL", "Child"}}},
        {"advertise_all", {{"ADVERTISE", "ALL"}}},
        {"adddefault_all", {{"ADDDEFAULT", "ALL"}}},
        {"condition_disable_addlocal", {{"PROP_DISABLE", "1"}, {"ADDLOCAL", "Cond"}}},
        {"condition_disable", {{"PROP_DISABLE", "1"}}},
        {"installlevel", {{"INSTALLLEVEL", "250"}}},
        {"compaddlocal", {{"COMPADDLOCAL", "{0A000001-0000-4000-8000-000000000003}"}}},
        {"shortfilenames", {{"SHORTFILENAMES", "1"}}},
    };
    ASSERT_EQ(runs.size(), cmd.size());
    for (const Run& run : runs) {
        SCOPED_TRACE(run.name);
        if (run.exit != 0) continue;  // remove_all of a product not installed: 1605 before costing
        auto pkg = msi::Package::open(bytes);
        ASSERT_TRUE(pkg.has_value());
        msi::Engine::Options opt;
        opt.package_path = "C:\\Users\\Owner\\msifeat\\feat32.msi";
        opt.ui_level = 2;  // /qn
        opt.server = true;
        opt.command_line = cmd.at(run.name);
        msi::Services services;
        services.vfs = &vfs;
        services.manager = &mgr;
        u32 err = 0;
        auto engine = msi::Engine::open(mgr, std::make_shared<msi::Package>(std::move(*pkg)), opt, services, err);
        ASSERT_TRUE(engine) << err;
        msi::Session& s = engine->session();
        s.set_property("ProgramFilesFolder", "C:\\Program Files (x86)\\");
        msi::ActionContext ctx(s, engine->services());
        ASSERT_EQ(msi::action_cost_initialize(ctx), 0u);
        ASSERT_EQ(msi::action_file_cost(ctx), 0u);
        ASSERT_EQ(msi::action_isolate_components(ctx), 0u);
        ASSERT_EQ(msi::action_cost_finalize(ctx), 0u);
        msi::FeatureModel& fm = msi::feature_model(s);
        for (const std::string& l : run.lines) {
            const usize f = l.find("Feature: "), c = l.find("Component: ");
            if (f != std::string::npos || c != std::string::npos) {
                const bool is_feature = f != std::string::npos;
                const usize start = is_feature ? f + 9 : c + 11;
                const std::string name = l.substr(start, l.find(';', start) - start);
                if (name.rfind("__", 0) == 0) continue;  // Windows' own HKCU key-path helper component
                char got[256];
                if (is_feature) {
                    ASSERT_TRUE(fm.features.count(name)) << name;
                    const auto& x = fm.features[name];
                    std::snprintf(got, sizeof(got), " Feature: %s; Installed: %s; Request: %s; Action: %s", name.c_str(),
                                  state_name(x.installed), state_name(x.request), state_name(x.action));
                } else {
                    ASSERT_TRUE(fm.components.count(name)) << name;
                    const auto& x = fm.components[name];
                    std::snprintf(got, sizeof(got), " Component: %s; Installed: %s; Request: %s; Action: %s",
                                  name.c_str(), state_name(x.installed), state_name(x.request), state_name(x.action));
                }
                EXPECT_EQ(std::string(got), l.substr(l.find(is_feature ? " Feature:" : " Component:")));
            }
            // Directory properties CostFinalize sets (shortfilenames run).
            const usize pc = l.find("PROPERTY CHANGE: Adding ");
            if (pc != std::string::npos && (l.find(" APPDIR ") != std::string::npos ||
                                            l.find(" DATADIR ") != std::string::npos)) {
                const std::string name = l.substr(pc + 24, l.find(' ', pc + 24) - pc - 24);
                const usize q = l.find('\'');
                EXPECT_EQ(s.property(name), l.substr(q + 1, l.rfind('\'') - q - 1)) << name;
            }
        }
    }
}

}  // namespace

INSTANTIATE_MSI_BITNESS(MsiFeatures);
INSTANTIATE_MSI_BITNESS(MsiFeaturesFinalized);

}  // namespace vitreo::winapi::msi_test
