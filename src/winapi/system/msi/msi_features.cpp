// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_features.cpp
/// @brief Windows Installer engine area: costing, directories and target
/// paths, feature/component selection and state (msi_features.hpp).
///
/// Behaviour the Learn pages leave open is measured by
/// tools/oracle_probe/msi_features_probe.c and msi_features_msiexec.ps1 on
/// the msi_features_build_packages.ps1 packages (expected output in
/// tools/oracle_probe/msi_features_*.expected); the rules below cite the
/// probe scenario that fixed each one. Contract notes and gaps:
/// .docs/contracts/msi/features.md.

#include "winapi/system/msi/msi_features.hpp"
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_database.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_files.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "capsule/vfs/vfs.hpp"
#include "platform/host_volume_space.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

namespace vitreo::winapi::msi {

namespace {

using namespace install_state;

// Windows error codes these functions return (winerror.h).
constexpr u32 kErrSuccess = 0;
constexpr u32 kErrInvalidHandle = 6;
constexpr u32 kErrInvalidParameter = 87;
constexpr u32 kErrDiskFull = 112;
constexpr u32 kErrMoreData = 234;
constexpr u32 kErrNoMoreItems = 259;
constexpr u32 kErrDirectory = 267;
constexpr u32 kErrInstallFailure = 1603;
constexpr u32 kErrUnknownFeature = 1606;
constexpr u32 kErrUnknownComponent = 1607;
constexpr u32 kErrInvalidHandleState = 1609;
constexpr u32 kErrFunctionNotCalled = 1626;
constexpr u32 kErrFunctionFailed = 1627;

// msidbFileAttributes (MsiDefs.h).
constexpr u32 kFileNoncompressed = 0x2000;
constexpr u32 kFileCompressed = 0x4000;
constexpr u32 kFilePatchAdded = 0x1000;

// The fixed costs measured on Windows 11 (probe "costed" / "primaryfolder*"
// scenarios, tools/oracle_probe/msi_features_probe*.expected):
//  * a component that writes Registry-table rows costs 1024 bytes, local or
//    source, whatever the number or size of the rows (cReg/cRegA/B/C: 2
//    units each);
//  * the installer's own temporary cost on the system volume is twice the
//    package size rounded to the cluster (24 KiB package: 96 units; lo.msi
//    374870016 bytes: 1464336 units; a 1000000-byte package: 3920 units);
//  * PrimaryVolumeSpaceRequired adds 3 clusters over the component costs and
//    that temporary cost (every primaryfolder* scenario: +24 units at 4 KiB).
constexpr i64 kRegistryCost = 1024;
constexpr i64 kFixedOverheadClusters = 3;

bool installed_kind(i32 s) { return s == kLocal || s == kSource || s == kAdvertised; }
bool present(i32 s) { return s == kLocal || s == kSource; }

i64 round_up(i64 bytes, i64 unit) { return unit <= 0 ? bytes : ((bytes + unit - 1) / unit) * unit; }

std::vector<std::string> split_list(const std::string& value) {
    // Learn ADDLOCAL..: comma-delimited, case-sensitive; no trimming (probe
    // addlocal_spaces: "High, Other" names " Other" and fails CostFinalize).
    std::vector<std::string> out;
    if (value.empty()) return out;
    usize start = 0;
    while (true) {
        const usize comma = value.find(',', start);
        out.push_back(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string with_slash(std::string p) {
    if (!p.empty() && p.back() != '\\') p.push_back('\\');
    return p;
}

/// Column access by name for one table.
struct Cols {
    const Table* t{nullptr};
    std::vector<std::optional<usize>> idx;
    Cols(const Table* table, std::initializer_list<const char*> names) : t(table) {
        for (const char* n : names) idx.push_back(t ? t->column_index(n) : std::nullopt);
    }
    [[nodiscard]] std::string cell(const std::vector<std::string>& row, usize i) const {
        if (i >= idx.size() || !idx[i] || *idx[i] >= row.size()) return {};
        return row[*idx[i]];
    }
    [[nodiscard]] i32 integer(const std::vector<std::string>& row, usize i, i32 fallback = 0) const {
        const auto v = cell_int(cell(row, i));
        return v ? *v : fallback;
    }
};

/// One half of a DefaultDir / FileName cell: `short|long` or one name.
struct NamePair {
    std::string short_name, long_name;
};
NamePair name_pair(const std::string& text) {
    const usize bar = text.find('|');
    if (bar == std::string::npos) return {text, text};
    return {text.substr(0, bar), text.substr(bar + 1)};
}

// ---- area-private session state -----------------------------------------------------

struct VolumeGeometry {
    i64 cluster{4096};
    std::optional<i64> available;  ///< bytes the caller may write
};

struct Internal {
    capsule::Vfs* vfs{nullptr};
    capsule::Registry* registry{nullptr};
    bool word_count_read{false};
    u32 word_count{0};
    bool sources_resolved{false};
    bool costing_complete{false};
    std::set<std::string> anchored;   ///< directories whose property defined them at CostFinalize
    std::map<std::string, i32> component_override;  ///< MsiSetComponentState requests
    std::set<std::string> reinstall;  ///< features listed by REINSTALL
    std::map<std::string, NamePair> target_names, source_names;  ///< per directory
    std::map<std::string, std::vector<std::pair<std::string, u32>>> component_files;  ///< file key + attributes
    std::map<std::string, std::vector<i64>> component_file_sizes;
    /// Cluster size per volume: fixed for the life of a session, and asked
    /// for once per component on every selection change (thousands of
    /// components), so it is read from the volume once.
    std::map<std::string, i64> cluster_by_volume;
    std::set<std::string> components_with_registry;
    std::map<std::string, bool> never_overwrite_hit;
    std::map<std::string, bool> key_file_superseded;  ///< files area's versioning verdict, per component
    std::map<std::string, NamePair> file_names;  ///< file key -> FileName
    std::string root_source;
    std::string root_source_name;  ///< the root row's DefaultDir (MsiGetSourcePath alias)
};

Internal& internal(Session& s) { return s.state<Internal>(); }

std::mutex& provider_mutex() {
    static std::mutex m;
    return m;
}
InstalledStateProvider& provider() {
    static InstalledStateProvider p;
    return p;
}

u32 word_count(Session& s) {
    Internal& in = internal(s);
    if (in.word_count_read) return in.word_count;
    in.word_count_read = true;
    if (Package* pkg = s.package()) {
        if (const auto* bytes = pkg->stream("\005SummaryInformation")) {
            const auto props = parse_summary_information(std::span<const u8>(bytes->data(), bytes->size()));
            if (auto it = props.find(15); it != props.end()) in.word_count = static_cast<u32>(it->second.integer);
        }
    }
    return in.word_count;
}

std::string volume_of(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':') return std::string(1, static_cast<char>(std::toupper(path[0]))) + ":";
    if (path.rfind("\\\\", 0) == 0) {
        const usize a = path.find('\\', 2);
        const usize b = a == std::string::npos ? a : path.find('\\', a + 1);
        return path.substr(0, b);
    }
    return {};
}

VolumeGeometry geometry(Session& s, const std::string& volume) {
    VolumeGeometry g;
    Internal& in = internal(s);
    if (in.vfs != nullptr && volume.size() == 2) {
        auto resolved = in.vfs->resolve_components(volume + "\\");
        capsule::VolumeInfo info;
        if (resolved.is_ok() && resolved.value().first != nullptr && resolved.value().first->volume_info(info) &&
            info.sectors_per_cluster > 0 && info.bytes_per_sector > 0) {
            g.cluster = static_cast<i64>(info.sectors_per_cluster) * info.bytes_per_sector;
            g.available = static_cast<i64>(info.available_bytes ? info.available_bytes
                                                               : static_cast<u64>(info.free_clusters) * g.cluster);
            return g;
        }
        // The volume reports no geometry: GetDiskFreeSpace's answer (8 x 512
        // clusters, the host volume's space; kernel32 directory_disk.cpp).
        platform::HostVolumeSpace space;
        if (platform::host_volume_space("/", space)) g.available = static_cast<i64>(space.available_bytes);
    }
    return g;
}

/// The volume's cluster size (geometry().cluster), read once per session.
i64 cluster_size(Session& s, const std::string& volume) {
    Internal& in = internal(s);
    if (auto it = in.cluster_by_volume.find(volume); it != in.cluster_by_volume.end()) return it->second;
    const i64 cluster = geometry(s, volume).cluster;
    in.cluster_by_volume.emplace(volume, cluster);
    return cluster;
}

// ---- directories -----------------------------------------------------------------------

/// Learn "Directory Table": the target name per SHORTFILENAMES (msiexec
/// shortfilenames scenario), the source name per Summary Word Count bit 0
/// (featsn.msi: DATADIR source SRCDAT~1).
std::string segment(const NamePair& n, bool use_short) {
    const std::string& v = use_short ? n.short_name : n.long_name;
    return v == "." ? std::string{} : v;
}

std::string join(const std::string& base, const std::string& seg) {
    if (seg.empty()) return with_slash(base);
    return with_slash(with_slash(base) + seg);
}

bool is_root(const DirectoryInfo& d) { return d.parent.empty() || d.parent == d.key; }

/// Directories in parent-before-child order.
std::vector<std::string> topo_order(FeatureModel& fm) {
    std::vector<std::string> order;
    std::set<std::string> done;
    std::function<void(const std::string&, int)> visit = [&](const std::string& key, int depth) {
        if (done.count(key) || depth > 128) return;
        auto it = fm.directories.find(key);
        if (it == fm.directories.end()) return;
        if (!is_root(it->second)) visit(it->second.parent, depth + 1);
        if (done.insert(key).second) order.push_back(key);
    };
    for (const std::string& k : fm.directory_order) visit(k, 0);
    return order;
}

void load_directories(Session& s) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    fm.directories.clear();
    fm.directory_order.clear();
    in.target_names.clear();
    in.source_names.clear();
    Package* pkg = s.package();
    const Table* t = pkg ? pkg->table("Directory") : nullptr;
    if (t == nullptr) return;
    Cols c(t, {"Directory", "Directory_Parent", "DefaultDir"});
    for (const auto& row : t->rows) {
        DirectoryInfo d;
        d.key = c.cell(row, 0);
        if (d.key.empty()) continue;
        d.parent = c.cell(row, 1);
        d.default_dir = c.cell(row, 2);
        const usize colon = d.default_dir.find(':');
        const std::string target = colon == std::string::npos ? d.default_dir : d.default_dir.substr(0, colon);
        const std::string source = colon == std::string::npos ? target : d.default_dir.substr(colon + 1);
        in.target_names[d.key] = name_pair(target);
        in.source_names[d.key] = name_pair(source);
        if (is_root(d) && fm.target_root.empty()) fm.target_root = d.key;
        fm.directory_order.push_back(d.key);
        fm.directories[d.key] = std::move(d);
    }
}

/// SourceDir: the property ResolveSource sets, else the package's folder.
std::string source_root(Session& s) {
    for (const char* p : {"SourceDir", "SOURCEDIR"}) {
        const std::string v = s.property(p);
        if (!v.empty()) return with_slash(v);
    }
    for (const char* p : {"OriginalDatabase", "DATABASE"}) {
        const std::string v = s.property(p);
        const usize slash = v.find_last_of('\\');
        if (slash != std::string::npos) return v.substr(0, slash + 1);
    }
    return {};
}

/// Source paths are available from CostInitialize on (probe phase lines:
/// MsiGetSourcePath succeeds after CostInitialize, MsiGetTargetPath only
/// after CostFinalize). A compressed package (Word Count bit 1) keeps every
/// source file in cabinets at SourceDir (feat64.msi: every source = SourceDir).
void resolve_sources(Session& s) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    in.root_source = source_root(s);
    if (!in.root_source.empty()) {
        if (s.property("SourceDir").empty()) s.set_property("SourceDir", in.root_source);
        if (s.property("SOURCEDIR").empty()) s.set_property("SOURCEDIR", in.root_source);
    }
    const u32 wc = word_count(s);
    const bool compressed = (wc & 2) != 0;
    const bool short_source = (wc & 1) != 0;
    for (const std::string& key : topo_order(fm)) {
        DirectoryInfo& d = fm.directories[key];
        if (is_root(d)) {
            d.source = in.root_source;
            if (key == fm.target_root) in.root_source_name = d.default_dir;
            continue;
        }
        const std::string parent = fm.directories[d.parent].source;
        d.source = compressed ? in.root_source : join(parent, segment(in.source_names[key], short_source));
    }
    in.sources_resolved = true;
}

/// Re-derive every target path that is not pinned (a root, a directory its
/// property defined at CostFinalize, or one MsiSetTargetPath set) from its
/// parent; set each directory's property (Learn: "the keys in the Directory
/// table become properties set to directory paths"). Returns the keys whose
/// path changed.
std::vector<std::string> derive_targets(Session& s) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    const bool short_target = !s.property("SHORTFILENAMES").empty();
    std::vector<std::string> changed;
    for (const std::string& key : topo_order(fm)) {
        DirectoryInfo& d = fm.directories[key];
        std::string target = d.target;
        if (!is_root(d) && !d.target_overridden && !in.anchored.count(key)) {
            target = join(fm.directories[d.parent].target, segment(in.target_names[key], short_target));
        }
        if (target != d.target || s.property(key) != target) changed.push_back(key);
        d.target = target;
        s.set_property(key, target);
    }
    return changed;
}

/// CostFinalize's directory resolution (Learn "Directory Table" / TARGETDIR /
/// ROOTDRIVE): a root resolves to its property, else ROOTDRIVE; a non-root
/// whose key is a defined property resolves to that value (probe targetdir:
/// APPDIR=C:\VitreoApp -> "C:\VitreoApp\"); every other directory is its
/// parent plus its DefaultDir target name.
void resolve_targets(Session& s) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    std::string rootdrive = s.property("ROOTDRIVE");
    if (rootdrive.empty()) {
        // Learn ROOTDRIVE: the installer sets it to the writable local drive
        // with the most free space; Vitreo's capsule has one writable fixed
        // drive, the VFS default drive.
        const char letter = in.vfs ? in.vfs->default_drive() : 'C';
        rootdrive = std::string(1, letter) + ":\\";
        s.set_property("ROOTDRIVE", rootdrive);
    }
    in.anchored.clear();
    for (const std::string& key : topo_order(fm)) {
        DirectoryInfo& d = fm.directories[key];
        if (d.target_overridden) continue;
        const std::string prop = s.property(key);
        if (is_root(d)) {
            d.target = with_slash(prop.empty() ? rootdrive : prop);
        } else if (!prop.empty()) {
            d.target = with_slash(prop);
            in.anchored.insert(key);
        }
    }
    (void)derive_targets(s);
    fm.directories_resolved = true;
}

// ---- tables -----------------------------------------------------------------------------

void load_tables(Session& s) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    fm.features.clear();
    fm.components.clear();
    fm.feature_order.clear();
    fm.component_order.clear();
    fm.target_root.clear();
    fm.finalized = false;
    fm.file_costed = false;
    fm.directories_resolved = false;
    in.component_override.clear();
    in.reinstall.clear();
    in.costing_complete = false;
    in.sources_resolved = false;
    in.component_files.clear();
    in.component_file_sizes.clear();
    in.components_with_registry.clear();
    in.file_names.clear();
    in.never_overwrite_hit.clear();
    in.key_file_superseded.clear();
    Package* pkg = s.package();
    if (pkg == nullptr) return;

    if (const Table* t = pkg->table("Feature")) {
        Cols c(t, {"Feature", "Feature_Parent", "Title", "Description", "Display", "Level", "Directory_", "Attributes"});
        for (const auto& row : t->rows) {
            FeatureInfo f;
            f.name = c.cell(row, 0);
            if (f.name.empty()) continue;
            f.parent = c.cell(row, 1);
            if (f.parent == f.name) f.parent.clear();
            f.title = c.cell(row, 2);
            f.description = c.cell(row, 3);
            f.display = c.integer(row, 4);
            f.authored_level = f.level = c.integer(row, 5);
            f.directory = c.cell(row, 6);
            f.attributes = static_cast<u32>(c.integer(row, 7));
            fm.feature_order.push_back(f.name);
            fm.features[f.name] = std::move(f);
        }
        for (const std::string& name : fm.feature_order) {
            const std::string& parent = fm.features[name].parent;
            if (!parent.empty() && fm.features.count(parent)) fm.features[parent].children.push_back(name);
        }
    }
    if (const Table* t = pkg->table("Component")) {
        Cols c(t, {"Component", "ComponentId", "Directory_", "Attributes", "Condition", "KeyPath"});
        for (const auto& row : t->rows) {
            ComponentInfo ci;
            ci.name = c.cell(row, 0);
            if (ci.name.empty()) continue;
            ci.component_id = c.cell(row, 1);
            ci.directory = c.cell(row, 2);
            ci.attributes = static_cast<u32>(c.integer(row, 3));
            ci.condition = c.cell(row, 4);
            ci.key_path = c.cell(row, 5);
            fm.component_order.push_back(ci.name);
            fm.components[ci.name] = std::move(ci);
        }
    }
    if (const Table* t = pkg->table("FeatureComponents")) {
        Cols c(t, {"Feature_", "Component_"});
        for (const auto& row : t->rows) {
            const std::string f = c.cell(row, 0), comp = c.cell(row, 1);
            auto fi = fm.features.find(f);
            auto ci = fm.components.find(comp);
            if (fi == fm.features.end() || ci == fm.components.end()) continue;
            fi->second.components.push_back(comp);
            ci->second.features.push_back(f);
        }
    }
    if (const Table* t = pkg->table("File")) {
        Cols c(t, {"File", "Component_", "FileName", "FileSize", "Attributes"});
        for (const auto& row : t->rows) {
            const std::string key = c.cell(row, 0), comp = c.cell(row, 1);
            in.file_names[key] = name_pair(c.cell(row, 2));
            in.component_files[comp].push_back({key, static_cast<u32>(c.integer(row, 4))});
            in.component_file_sizes[comp].push_back(c.integer(row, 3));
        }
    }
    if (const Table* t = pkg->table("Registry")) {
        Cols c(t, {"Component_"});
        for (const auto& row : t->rows) in.components_with_registry.insert(c.cell(row, 0));
    }
    load_directories(s);
    fm.loaded = true;
}

// ---- feature rules ------------------------------------------------------------------------

/// Learn "Component Table" / "MsiGetFeatureValidStates": a component can run
/// from source unless one of its files is compressed (per-file attribute,
/// else Summary Word Count bit 1) or patch-added.
bool source_capable(Session& s, const ComponentInfo& c) {
    const bool package_compressed = (word_count(s) & 2) != 0;
    const auto it = internal(s).component_files.find(c.name);
    if (it == internal(s).component_files.end()) return true;
    for (const auto& [key, attrs] : it->second) {
        const bool compressed = (attrs & kFileCompressed) || (package_compressed && !(attrs & kFileNoncompressed));
        if (compressed || (attrs & kFilePatchAdded)) return false;
    }
    return true;
}

u32 valid_states_of(Session& s, const FeatureInfo& f) {
    FeatureModel& fm = feature_model(s);
    u32 mask = 0;
    if (!(f.attributes & kFeatureDisallowAdvertise)) mask |= 1u << kAdvertised;
    if (!(f.attributes & kFeatureUIDisallowAbsent)) mask |= 1u << kAbsent;
    if (f.components.empty()) return mask | (1u << kLocal) | (1u << kSource);
    for (const std::string& name : f.components) {
        const ComponentInfo& c = fm.components[name];
        const u32 run = c.attributes & 3;
        if (run == 0 || run == 2 || run == 3) mask |= 1u << kLocal;
        if ((run == 1 || run == 2 || run == 3) && source_capable(s, c)) mask |= 1u << kSource;
    }
    return mask;
}

/// The state a request for @p want yields for @p f (probe addsource_all:
/// a feature that cannot run from source installs locally; advertise_all:
/// a DisallowAdvertise feature asked to advertise is absent).
i32 adjust(Session& s, const FeatureInfo& f, i32 want) {
    const u32 valid = valid_states_of(s, f);
    switch (want) {
    case kLocal: return (valid & (1u << kLocal)) ? kLocal : (valid & (1u << kSource)) ? kSource : kLocal;
    case kSource: return (valid & (1u << kSource)) ? kSource : kLocal;
    case kAdvertised: return (valid & (1u << kAdvertised)) ? kAdvertised : kAbsent;
    default: return want;
    }
}

/// Learn Feature.Attributes: FavorAdvertise (ignored for ADDDEFAULT),
/// FavorSource, else FavorLocal.
i32 default_state(Session& s, const FeatureInfo& f, bool honor_advertise) {
    if (honor_advertise && (f.attributes & kFeatureFavorAdvertise)) return adjust(s, f, kAdvertised);
    if (f.attributes & kFeatureFavorSource) return adjust(s, f, kSource);
    return adjust(s, f, kLocal);
}

i32 effective(const FeatureInfo& f) {
    if (f.request != kUnknown) return f.request;
    return installed_kind(f.installed) ? f.installed : kUnknown;
}

void remove_subtree(FeatureModel& fm, const std::string& name, std::set<std::string>& changed) {
    for (const std::string& child : fm.features[name].children) {
        FeatureInfo& c = fm.features[child];
        const i32 r = installed_kind(c.installed) ? kAbsent : kUnknown;
        if (c.request != r) changed.insert(child);
        c.request = r;
        remove_subtree(fm, child, changed);
    }
}

/// One request on one feature with the tree rules measured by the probe
/// (setstate_tree, addlocal_high, addlocal_all_remove_root):
///  * installing a feature installs every ancestor that is not installed,
///    in the requested state as each ancestor allows (Root becomes Local
///    when Child is set Local, Advertised when Child is advertised);
///  * removing a feature removes its whole subtree;
///  * a level-0 feature never changes; a UIDisallowAbsent feature ignores
///    an Absent request.
void request_feature(Session& s, const std::string& name, i32 want, std::set<std::string>& changed) {
    FeatureModel& fm = feature_model(s);
    FeatureInfo& f = fm.features[name];
    if (f.level <= 0) return;
    if (want == kAbsent || want == kUnknown) {
        if (want == kAbsent && (f.attributes & kFeatureUIDisallowAbsent)) return;
        if (f.request != want) changed.insert(name);
        f.request = want;
        remove_subtree(fm, name, changed);
        return;
    }
    const i32 state = adjust(s, f, want);
    if (f.request != state) changed.insert(name);
    f.request = state;
    if (!installed_kind(state)) {
        remove_subtree(fm, name, changed);
        return;
    }
    std::string up = f.parent;
    for (int depth = 0; !up.empty() && depth < 64; ++depth) {
        auto it = fm.features.find(up);
        if (it == fm.features.end()) break;
        FeatureInfo& p = it->second;
        if (installed_kind(effective(p))) break;
        if (p.level <= 0) break;
        const i32 ps = adjust(s, p, state);
        if (p.request != ps) changed.insert(up);
        p.request = ps;
        up = p.parent;
    }
}

/// The parent-to-child rules applied after any selection change (probe
/// default / addlocal_high / advertise_child / setattrs):
///  * a feature under a parent that is not installed is not installed
///    (Follow, level 1, stays null under High, level 5);
///  * FollowParent + UIDisallowAbsent: the parent's state; FollowParent
///    alone: the parent's state unless the feature is not selected (Learn
///    Feature table);
///  * UIDisallowAbsent: installed whenever the parent is, in the parent's
///    state (Must, level 5, Local under Root Local, Advertised under Root
///    Advertised);
///  * under an advertised parent an installed child is advertised (Empty
///    under Other set FavorAdvertise).
/// A level-0 feature is never selected; a root UIDisallowAbsent feature is
/// installed in its default state.
void apply_tree(Session& s, std::set<std::string>& changed) {
    FeatureModel& fm = feature_model(s);
    std::vector<std::string> order;
    std::set<std::string> seen;
    std::function<void(const std::string&, int)> visit = [&](const std::string& n, int depth) {
        if (seen.count(n) || depth > 64) return;
        const std::string& parent = fm.features[n].parent;
        if (!parent.empty() && fm.features.count(parent)) visit(parent, depth + 1);
        if (seen.insert(n).second) order.push_back(n);
    };
    for (const std::string& n : fm.feature_order) visit(n, 0);
    for (const std::string& n : order) {
        FeatureInfo& f = fm.features[n];
        i32 r = f.request;
        if (f.level <= 0) {
            r = kUnknown;
        } else if (f.parent.empty() || !fm.features.count(f.parent)) {
            if ((f.attributes & kFeatureUIDisallowAbsent) && !installed_kind(effective(f)))
                r = default_state(s, f, true);
        } else {
            const i32 p = effective(fm.features[f.parent]);
            const i32 mine = effective(f);
            if (!installed_kind(p)) {
                if (installed_kind(mine)) r = installed_kind(f.installed) ? kAbsent : kUnknown;
            } else {
                const bool follow = (f.attributes & kFeatureFollowParent) != 0;
                const bool must = (f.attributes & kFeatureUIDisallowAbsent) != 0;
                if ((follow && must) || (must && !installed_kind(mine)) || (follow && installed_kind(mine))) {
                    r = adjust(s, f, p);
                } else if (p == kAdvertised && present(mine)) {
                    r = adjust(s, f, kAdvertised);
                }
            }
        }
        if (r != f.request) {
            changed.insert(n);
            f.request = r;
        }
    }
}

/// Level-based selection of a first install (Learn Feature.Level /
/// INSTALLLEVEL; probe default, level250, level0): Level 1..INSTALLLEVEL in
/// the default state, everything else unselected.
void select_by_level(Session& s, std::set<std::string>& changed) {
    FeatureModel& fm = feature_model(s);
    const bool maintenance = !s.property("Installed").empty();
    for (const std::string& n : fm.feature_order) {
        FeatureInfo& f = fm.features[n];
        i32 r = kUnknown;
        if (!maintenance && f.level > 0 && f.level <= fm.install_level) r = default_state(s, f, true);
        if (r != f.request) changed.insert(n);
        f.request = r;
    }
}

/// The feature among @p features that costs least to install (Learn
/// COMPADDLOCAL: "installs the feature that requires the least amount of
/// disk space"; probe compaddlocal picks High over Child for cShared).
std::string cheapest_feature(Session& s, const std::vector<std::string>& features);

/// Learn ADDLOCAL...FILEADDDEFAULT, in Learn's evaluation order. Returns
/// false when a list names a feature, component or file the package does not
/// have (probe addlocal_unknown / addlocal_case: CostFinalize 1603).
bool apply_request_properties(Session& s, std::set<std::string>& changed) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    auto features_of = [&](const std::string& prop, std::vector<std::string>& out) -> bool {
        for (const std::string& n : split_list(s.property(prop))) {
            if (n == "ALL") {
                for (const std::string& f : fm.feature_order) out.push_back(f);
            } else if (fm.features.count(n)) {
                out.push_back(n);
            } else {
                return false;
            }
        }
        return true;
    };
    struct FeatureList { const char* prop; int kind; };
    // kind: 0 local, 1 remove, 2 source, 3 default, 4 reinstall, 5 advertise
    const FeatureList lists[] = {{"ADDLOCAL", 0}, {"REMOVE", 1}, {"ADDSOURCE", 2}, {"ADDDEFAULT", 3},
                                 {"REINSTALL", 4}, {"ADVERTISE", 5}};
    for (const FeatureList& l : lists) {
        std::vector<std::string> names;
        if (!features_of(l.prop, names)) return false;
        for (const std::string& n : names) {
            FeatureInfo& f = fm.features[n];
            switch (l.kind) {
            case 0: request_feature(s, n, kLocal, changed); break;
            case 1:
                // REMOVE of a feature that is not installed requests nothing
                // (probe addlocal_all_remove_child: Child null, not Absent).
                request_feature(s, n, installed_kind(f.installed) ? kAbsent : kUnknown, changed);
                break;
            case 2: request_feature(s, n, kSource, changed); break;
            case 3: request_feature(s, n, default_state(s, f, false), changed); break;
            case 4:
                // Learn REINSTALL: only features already installed are
                // reinstalled (probe reinstall_all on a first install: nothing).
                if (installed_kind(f.installed)) {
                    in.reinstall.insert(n);
                    request_feature(s, n, f.installed, changed);
                }
                break;
            case 5: request_feature(s, n, kAdvertised, changed); break;
            }
        }
    }
    struct ItemList { const char* prop; bool files; int kind; };
    const ItemList items[] = {{"COMPADDLOCAL", false, 0}, {"COMPADDSOURCE", false, 2}, {"COMPADDDEFAULT", false, 3},
                              {"FILEADDLOCAL", true, 0},  {"FILEADDSOURCE", true, 2},  {"FILEADDDEFAULT", true, 3}};
    for (const ItemList& l : items) {
        for (const std::string& item : split_list(s.property(l.prop))) {
            const ComponentInfo* comp = nullptr;
            if (l.files) {
                for (const auto& [cname, files] : in.component_files) {
                    for (const auto& file : files)
                        if (file.first == item) comp = &fm.components[cname];
                }
            } else {
                for (const auto& [cname, c] : fm.components)
                    if (!c.component_id.empty() && c.component_id == item) comp = &c;
            }
            if (comp == nullptr) return false;
            const std::string f = cheapest_feature(s, comp->features);
            if (f.empty()) continue;
            const i32 want = l.kind == 0 ? kLocal : l.kind == 2 ? kSource : default_state(s, fm.features[f], false);
            request_feature(s, f, want, changed);
        }
    }
    return true;
}

bool any_request_property(Session& s) {
    for (const char* p : {"ADDLOCAL", "REMOVE", "ADDSOURCE", "ADDDEFAULT", "REINSTALL", "ADVERTISE", "COMPADDLOCAL",
                          "COMPADDSOURCE", "COMPADDDEFAULT", "FILEADDLOCAL", "FILEADDSOURCE", "FILEADDDEFAULT"}) {
        if (!s.property(p).empty()) return true;
    }
    return false;
}

// ---- components ---------------------------------------------------------------------------

bool key_path_exists(Session& s, const ComponentInfo& c) {
    Internal& in = internal(s);
    if (c.registry_key_path()) {
        if (in.registry == nullptr) return false;
        const Table* t = s.package() ? s.package()->table("Registry") : nullptr;
        if (t == nullptr) return false;
        Cols col(t, {"Registry", "Root", "Key", "Name"});
        for (const auto& row : t->rows) {
            if (col.cell(row, 0) != c.key_path) continue;
            i32 root = col.integer(row, 1, -1);
            if (root == -1) root = s.property("ALLUSERS").empty() ? 1 : 2;
            static const capsule::RegHandle roots[] = {capsule::HKEY_CLASSES_ROOT, capsule::HKEY_CURRENT_USER,
                                                       capsule::HKEY_LOCAL_MACHINE, capsule::HKEY_USERS};
            if (root < 0 || root > 3) return false;
            const std::vector<u16> key16 = utf8_to_utf16(s.format(col.cell(row, 2)));
            const std::wstring key(key16.begin(), key16.end());
            // A registry key path is looked up in the component's view.
            auto h = in.registry->open_key(
                in.registry->view_root(roots[root], c.is_64bit() ? capsule::RegView::Native64
                                                                 : capsule::RegView::Wow32),
                key, false);
            if (h.is_err()) return false;
            const std::string name = s.format(col.cell(row, 3));
            bool exists = true;
            if (!name.empty() && name != "+" && name != "-" && name != "*") {
                const std::vector<u16> n16 = utf8_to_utf16(name);
                exists = in.registry->query_value(h.value(), std::wstring(n16.begin(), n16.end())).is_ok();
            }
            in.registry->close_key(h.value());
            return exists;
        }
        return false;
    }
    if (c.odbc_key_path() || c.key_path.empty() || in.vfs == nullptr) return false;
    auto f = in.file_names.find(c.key_path);
    if (f == in.file_names.end()) return false;
    const bool short_target = !s.property("SHORTFILENAMES").empty();
    const std::string path = with_slash(component_directory(s, c.name)) +
                             (short_target ? f->second.short_name : f->second.long_name);
    return in.vfs->stat(path).is_ok();
}

i32 component_state_for(Session& s, const ComponentInfo& c, i32 want) {
    const u32 run = c.attributes & 3;
    switch (want) {
    case kLocal: return (run == 1 && source_capable(s, c)) ? kSource : kLocal;
    case kSource: return (run == 0 || !source_capable(s, c)) ? kLocal : kSource;
    default: return want;
    }
}

/// Learn MsiSetFeatureState: a component linked to several features is Local
/// if any of them wants it local, else Source if any wants source, else
/// Absent if any removes it; then the component's own run-from attributes
/// (probe default: cSrcOnly Source under a Local feature; feat64.msi: Local
/// when the source is compressed). The action is the request, except: a
/// disabled component (false Condition), a never-overwrite component whose
/// key path exists, removal of what is not installed, and a request equal
/// to the installed state (unless reinstalled), take no action (probe
/// setstate_component, never_overwrite, component_condition).
void compute_components(Session& s) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    for (const std::string& name : fm.component_order) {
        ComponentInfo& c = fm.components[name];
        i32 want = kUnknown;
        bool reinstall = false;
        for (const std::string& fname : c.features) {
            const FeatureInfo& f = fm.features[fname];
            if (in.reinstall.count(fname)) reinstall = true;
            if (f.request == kLocal) want = kLocal;
            else if (f.request == kSource && want != kLocal) want = kSource;
            else if (f.request == kAbsent && want == kUnknown) want = kAbsent;
        }
        if (auto o = in.component_override.find(name); o != in.component_override.end()) want = o->second;
        // Learn "Using Transitive Components": an installed transitive
        // component whose condition turned false is removed.
        if ((c.attributes & kComponentTransitive) && present(c.installed) && !c.enabled) want = kAbsent;
        // Removing what is not installed requests nothing (msiexec
        // advertise_all: cNoAdv "Request: Null" under an Absent feature;
        // probe primaryfolder_regonly: an Absent override costs nothing).
        if (want == kAbsent && !present(c.installed)) want = kUnknown;
        c.request = component_state_for(s, c, want);
        i32 action = c.request;
        if (!c.enabled && !((c.attributes & kComponentTransitive) && present(c.installed))) action = kUnknown;
        if (action == kAbsent) {
            action = present(c.installed) && !(c.attributes & kComponentPermanent) ? kAbsent : kUnknown;
        } else if (present(action)) {
            if (c.attributes & kComponentNeverOverwrite) {
                auto hit = in.never_overwrite_hit.find(name);
                if (hit == in.never_overwrite_hit.end()) hit = in.never_overwrite_hit.emplace(name, key_path_exists(s, c)).first;
                if (hit->second) action = kUnknown;
            }
            // The key file already on disk beats the package's copy (file
            // versioning rules, files area; msi_files_probe C_vdown / C_uv:
            // "Action: Null").
            if (action == kLocal && in.vfs != nullptr && !c.registry_key_path() && !c.odbc_key_path() &&
                !c.key_path.empty()) {
                auto hit = in.key_file_superseded.find(name);
                if (hit == in.key_file_superseded.end())
                    hit = in.key_file_superseded.emplace(name, key_file_supersedes_package(s, *in.vfs, name)).first;
                if (hit->second) action = kUnknown;
            }
            if (action == c.installed && !reinstall) action = kUnknown;
        }
        c.action = action;
    }
    for (const std::string& name : fm.feature_order) {
        FeatureInfo& f = fm.features[name];
        f.action = (f.request == f.installed && installed_kind(f.installed) && !in.reinstall.count(name)) ? kUnknown
                                                                                                         : f.request;
    }
}

// ---- costing ----------------------------------------------------------------------------------

i64 local_bytes(Session& s, const ComponentInfo& c) {
    Internal& in = internal(s);
    const i64 cluster = cluster_size(s, volume_of(component_directory(s, c.name)));
    i64 total = 0;
    if (auto it = in.component_file_sizes.find(c.name); it != in.component_file_sizes.end())
        for (i64 size : it->second) total += round_up(size, cluster);
    if (in.components_with_registry.count(c.name)) total += kRegistryCost;
    return total;
}

/// Bytes a component costs in @p state (probe "costed": only a component
/// the selection requests and its Condition enables has a cost; Local =
/// cluster-rounded files + registry, Source = registry, Absent = minus what
/// is installed, Default = the cost of its action state, Unknown = 0).
i64 component_cost(Session& s, const ComponentInfo& c, i32 state) {
    if (!c.enabled || c.request == kUnknown) return 0;
    const i64 reg = internal(s).components_with_registry.count(c.name) ? kRegistryCost : 0;
    switch (state) {
    case kLocal: return local_bytes(s, c);
    case kSource: return reg;
    case kAbsent: return c.installed == kLocal ? -local_bytes(s, c) : c.installed == kSource ? -reg : 0;
    case kDefault: return present(c.action) || c.action == kAbsent ? component_cost(s, c, c.action) : 0;
    default: return 0;
    }
}

std::string cheapest_feature(Session& s, const std::vector<std::string>& features) {
    FeatureModel& fm = feature_model(s);
    std::string best;
    i64 best_cost = 0;
    for (const std::string& n : features) {
        const FeatureInfo& f = fm.features[n];
        if (f.level <= 0) continue;
        i64 cost = 0;
        for (const std::string& comp : f.components) cost += local_bytes(s, fm.components[comp]);
        if (best.empty() || cost < best_cost) {
            best = n;
            best_cost = cost;
        }
    }
    return best;
}

std::string system_volume(Session& s) {
    Internal& in = internal(s);
    return std::string(1, in.vfs ? in.vfs->default_drive() : 'C') + ":";
}

i64 package_bytes(Session& s) {
    Internal& in = internal(s);
    if (in.vfs == nullptr) return 0;
    for (const char* p : {"OriginalDatabase", "DATABASE"}) {
        const std::string path = s.property(p);
        if (path.empty()) continue;
        if (auto st = in.vfs->stat(path); st.is_ok()) return static_cast<i64>(st.value().size);
    }
    return 0;
}

/// The installer's temporary cost on the system volume (the measured
/// 2 x cluster-rounded package size; Learn MsiEnumComponentCosts: "costs for
/// caching the database in the secure folder as well as the cost to create
/// the installation script").
i64 engine_temp_bytes(Session& s) {
    const i64 cluster = cluster_size(s, system_volume(s));
    return 2 * round_up(package_bytes(s), cluster);
}

std::map<std::string, i64> required_by_volume(Session& s) {
    FeatureModel& fm = feature_model(s);
    std::map<std::string, i64> need;
    for (const std::string& name : fm.component_order) {
        const ComponentInfo& c = fm.components[name];
        const i64 cost = component_cost(s, c, c.action);
        if (cost != 0) need[volume_of(component_directory(s, name))] += cost;
    }
    const std::string sys = system_volume(s);
    need[sys] += engine_temp_bytes(s) + kFixedOverheadClusters * cluster_size(s, sys);
    return need;
}

/// Learn CostingComplete / OutOfDiskSpace / OutOfNoRbDiskSpace /
/// PRIMARYFOLDER / PrimaryVolume*: Vitreo costs synchronously, so costing is
/// complete as soon as the selection is known (Windows completes it in the
/// background and sets CostingComplete to 1 when done; msiexec logs it 1).
void update_cost_properties(Session& s) {
    Internal& in = internal(s);
    in.costing_complete = true;
    bool out_of_space = false;
    const auto need = required_by_volume(s);
    for (const auto& [volume, bytes] : need) {
        const auto g = geometry(s, volume);
        if (g.available && bytes > *g.available) out_of_space = true;
    }
    s.set_property("CostingComplete", "1");
    s.set_property("OutOfDiskSpace", out_of_space ? "1" : "0");
    s.set_property("OutOfNoRbDiskSpace", out_of_space ? "1" : "0");
    const std::string primary = s.property("PRIMARYFOLDER");
    FeatureModel& fm = feature_model(s);
    if (!primary.empty() && fm.directories.count(primary)) {
        // probe primaryfolder: PrimaryVolumePath "C:", the rest in 512-byte units.
        const std::string volume = volume_of(fm.directories[primary].target);
        const auto g = geometry(s, volume);
        const i64 required = (need.count(volume) ? need.at(volume) : 0) / 512;
        const i64 available = g.available ? *g.available / 512 : 0;
        s.set_property("PrimaryVolumePath", volume);
        s.set_property("PrimaryVolumeSpaceRequired", std::to_string(required));
        s.set_property("PrimaryVolumeSpaceAvailable", std::to_string(available));
        s.set_property("PrimaryVolumeSpaceRemaining", std::to_string(available - required));
    } else {
        // msiexec plain: all three "0" when PRIMARYFOLDER is unset.
        s.set_property("PrimaryVolumeSpaceRequired", "0");
        s.set_property("PrimaryVolumeSpaceAvailable", "0");
        s.set_property("PrimaryVolumeSpaceRemaining", "0");
    }
}

void evaluate_components(Session& s) {
    FeatureModel& fm = feature_model(s);
    for (const std::string& name : fm.component_order) {
        ComponentInfo& c = fm.components[name];
        if (c.condition.empty()) {
            c.enabled = true;
            continue;
        }
        const auto v = s.evaluate_condition(c.condition);
        if (!v) log_unimplemented("CostFinalize", "Component.Condition outside the condition grammar (treated false)");
        c.enabled = v.value_or(false);
        // Learn Component.Condition: a non-transitive component stays enabled
        // once installed even if its condition turns false.
        if (!c.enabled && present(c.installed) && !(c.attributes & kComponentTransitive)) c.enabled = true;
    }
}

void load_installed_states(Session& s) {
    FeatureModel& fm = feature_model(s);
    InstalledStateProvider p;
    {
        std::lock_guard lock(provider_mutex());
        p = provider();
    }
    const std::string product = s.property("ProductCode");
    // A first install reports every feature and registered component Absent
    // and a component with no ComponentId Unknown (probe default).
    for (auto& [name, f] : fm.features) {
        i32 st = p.feature ? p.feature(s, product, name) : kUnknown;
        f.installed = st == kUnknown ? kAbsent : st;
    }
    for (auto& [name, c] : fm.components) {
        if (c.component_id.empty()) {
            c.installed = kUnknown;
            continue;
        }
        i32 st = p.component ? p.component(s, product, c.component_id) : kUnknown;
        c.installed = st == kUnknown ? kAbsent : st;
    }
}

void install_resolvers(Session& s) {
    Session* sp = &s;
    ItemStateResolvers& r = s.item_states();
    r.component_action = [sp](const std::string& n) { return component_action_state(*sp, n); };
    r.component_installed = [sp](const std::string& n) { return component_installed_state(*sp, n); };
    r.feature_action = [sp](const std::string& n) { return feature_action_state(*sp, n); };
    r.feature_installed = [sp](const std::string& n) { return feature_installed_state(*sp, n); };
    r.component_directory = [sp](const std::string& n) -> std::optional<std::string> {
        const ComponentInfo* c = find_component(*sp, n);
        if (c == nullptr) return std::nullopt;
        return component_directory(*sp, n);
    };
    r.directory_path = [sp](const std::string& key) -> std::optional<std::string> {
        FeatureModel& fm = feature_model(*sp);
        if (!fm.directories_resolved) return std::nullopt;
        auto it = fm.directories.find(key);
        if (it == fm.directories.end()) return std::nullopt;
        return it->second.target;
    };
}

/// Clears MsiSetComponentState requests on the components of changed
/// features (probe setstate_component: setting Child again resets cChild).
void clear_overrides(Session& s, const std::set<std::string>& changed) {
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    for (const std::string& f : changed)
        for (const std::string& c : fm.features[f].components) in.component_override.erase(c);
}

void refresh(Session& s, const std::set<std::string>& changed) {
    clear_overrides(s, changed);
    compute_components(s);
    update_cost_properties(s);
}

}  // namespace

// ---- public interface --------------------------------------------------------------------

void set_installed_state_provider(InstalledStateProvider p) {
    std::lock_guard lock(provider_mutex());
    provider() = std::move(p);
}

FeatureModel& feature_model(Session& session) { return session.state<FeatureModel>(); }

const FeatureInfo* find_feature(Session& session, const std::string& name) {
    FeatureModel& fm = feature_model(session);
    auto it = fm.features.find(name);
    return it == fm.features.end() ? nullptr : &it->second;
}

const ComponentInfo* find_component(Session& session, const std::string& name) {
    FeatureModel& fm = feature_model(session);
    auto it = fm.components.find(name);
    return it == fm.components.end() ? nullptr : &it->second;
}

std::optional<i32> feature_installed_state(Session& session, const std::string& name) {
    const FeatureInfo* f = find_feature(session, name);
    if (f == nullptr) return std::nullopt;
    return f->installed;
}
std::optional<i32> feature_action_state(Session& session, const std::string& name) {
    const FeatureInfo* f = find_feature(session, name);
    if (f == nullptr) return std::nullopt;
    return f->action;
}
std::optional<i32> component_installed_state(Session& session, const std::string& name) {
    const ComponentInfo* c = find_component(session, name);
    if (c == nullptr) return std::nullopt;
    return c->installed;
}
std::optional<i32> component_action_state(Session& session, const std::string& name) {
    const ComponentInfo* c = find_component(session, name);
    if (c == nullptr) return std::nullopt;
    return c->action;
}

std::string target_path(Session& session, const std::string& directory) {
    FeatureModel& fm = feature_model(session);
    if (!fm.directories_resolved) return {};
    auto it = fm.directories.find(directory);
    return it == fm.directories.end() ? std::string{} : it->second.target;
}

std::string source_path(Session& session, const std::string& directory) {
    FeatureModel& fm = feature_model(session);
    Internal& in = internal(session);
    if (!in.sources_resolved) return {};
    auto it = fm.directories.find(directory);
    if (it != fm.directories.end()) return it->second.source;
    // Learn MsiGetSourcePath: a root may be named by its DefaultDir value
    // (probe: "SourceDir" resolves for the source path, not the target).
    if (!in.root_source_name.empty() && directory == in.root_source_name) return in.root_source;
    return {};
}

std::string component_directory(Session& session, const std::string& component) {
    const ComponentInfo* c = find_component(session, component);
    if (c == nullptr) return {};
    if (c->action == kSource || (c->action == kUnknown && c->installed == kSource))
        return source_path(session, c->directory);
    return target_path(session, c->directory);
}

u32 set_target_path(Session& session, const std::string& directory, const std::string& path) {
    FeatureModel& fm = feature_model(session);
    Internal& in = internal(session);
    if (!fm.directories_resolved) return kErrDirectory;
    auto it = fm.directories.find(directory);
    if (it == fm.directories.end()) return kErrDirectory;
    // Learn MsiSetTargetPath: a full path; probe arguments: relative paths,
    // "/" separators, "." / ".." segments, characters invalid in file names
    // and drives that do not exist (or are read-only, D: CD-ROM) are 267;
    // "C:" is "C:\"; an existing file name is accepted.
    if (path.size() < 2 || path[1] != ':' || !capsule::is_valid_drive_letter(path[0])) {
        if (path.rfind("\\\\", 0) == 0) log_unimplemented("MsiSetTargetPath", "UNC target paths (no network shares)");
        return kErrDirectory;
    }
    if (path.size() > 2 && path[2] != '\\') return kErrDirectory;
    for (char ch : path.substr(2)) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 32 || ch == '/' || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*' ||
            ch == ':')
            return kErrDirectory;
    }
    std::string normalized = with_slash(path.size() == 2 ? path + "\\" : path);
    {
        usize start = 3;
        while (start < normalized.size()) {
            const usize end = normalized.find('\\', start);
            const std::string seg = normalized.substr(start, end - start);
            if (seg.empty() || seg == "." || seg == "..") return kErrDirectory;
            start = end + 1;
        }
    }
    if (in.vfs != nullptr) {
        const char letter = capsule::normalize_drive_letter(normalized[0]);
        if (!in.vfs->is_drive_mounted(letter)) return kErrDirectory;
        auto fs = in.vfs->resolve_components(std::string(1, letter) + ":\\");
        if (fs.is_err() || fs.value().first == nullptr || fs.value().first->is_optical()) return kErrDirectory;
    }
    DirectoryInfo& d = it->second;
    d.target = normalized;
    d.target_overridden = true;
    in.anchored.erase(directory);
    (void)derive_targets(session);
    // The key path of a never-overwrite component may now be elsewhere.
    in.never_overwrite_hit.clear();
    in.key_file_superseded.clear();
    if (fm.finalized) refresh(session, {});
    return kErrSuccess;
}

u32 set_feature_state(Session& session, const std::string& feature, i32 state) {
    if (state != kAdvertised && state != kAbsent && state != kLocal && state != kSource) return kErrInvalidParameter;
    FeatureModel& fm = feature_model(session);
    if (!fm.finalized) return kErrFunctionFailed;
    if (!fm.features.count(feature)) return kErrUnknownFeature;
    std::set<std::string> changed;
    request_feature(session, feature, state, changed);
    apply_tree(session, changed);
    changed.insert(feature);
    refresh(session, changed);
    return kErrSuccess;
}

u32 set_component_state(Session& session, const std::string& component, i32 state) {
    if (state != kAbsent && state != kLocal && state != kSource) return kErrInvalidParameter;
    FeatureModel& fm = feature_model(session);
    if (!fm.finalized) return kErrFunctionFailed;
    if (!fm.components.count(component)) return kErrUnknownComponent;
    internal(session).component_override[component] = state;
    refresh(session, {});
    return kErrSuccess;
}

u32 set_install_level(Session& session, i32 level) {
    FeatureModel& fm = feature_model(session);
    if (!fm.finalized) return kErrFunctionFailed;
    // Learn: 0 or negative keeps the level but still re-selects; probe
    // arguments: any positive value is stored (32768, 65536).
    if (level > 0) {
        fm.install_level = level;
        session.set_property("INSTALLLEVEL", std::to_string(level));
    }
    std::set<std::string> changed;
    select_by_level(session, changed);
    apply_tree(session, changed);
    refresh(session, changed);
    return kErrSuccess;
}

u32 set_feature_attributes(Session& session, const std::string& feature, u32 attrs) {
    FeatureModel& fm = feature_model(session);
    // Learn: only between CostInitialize and CostFinalize, else
    // ERROR_FUNCTION_FAILED; probe: unknown bits are accepted.
    if (!fm.loaded || fm.finalized) return kErrFunctionFailed;
    auto it = fm.features.find(feature);
    if (it == fm.features.end()) return kErrUnknownFeature;
    u32 bits = it->second.attributes & kFeatureUIDisallowAbsent;
    if (attrs & 2) bits |= kFeatureFavorSource;            // INSTALLFEATUREATTRIBUTE_FAVORSOURCE
    if (attrs & 4) bits |= kFeatureFollowParent;           // _FOLLOWPARENT
    if (attrs & 8) bits |= kFeatureFavorAdvertise;         // _FAVORADVERTISE
    if (attrs & 16) bits |= kFeatureDisallowAdvertise;     // _DISALLOWADVERTISE
    if (attrs & 32) bits |= kFeatureNoUnsupportedAdvertise;  // _NOUNSUPPORTEDADVERTISE
    it->second.attributes = bits;  // _FAVORLOCAL (1) is the absence of the others
    return kErrSuccess;
}

void recompute_states(Session& session) {
    FeatureModel& fm = feature_model(session);
    if (!fm.finalized) return;
    std::set<std::string> changed;
    apply_tree(session, changed);
    refresh(session, changed);
}

u32 feature_valid_states(Session& session, const std::string& feature) {
    const FeatureInfo* f = find_feature(session, feature);
    return f ? valid_states_of(session, *f) : 0;
}

i64 feature_cost(Session& session, const std::string& feature, i32 tree, i32 state) {
    FeatureModel& fm = feature_model(session);
    if (!internal(session).costing_complete || !fm.features.count(feature)) return 0;
    // probe "costed": a feature's Default/Unknown cost is 0.
    if (state != kLocal && state != kSource && state != kAbsent) return 0;
    std::set<std::string> feats{feature};
    if (tree == 1) {
        std::vector<std::string> stack{feature};
        while (!stack.empty()) {
            const std::string n = stack.back();
            stack.pop_back();
            for (const std::string& c : fm.features[n].children)
                if (feats.insert(c).second) stack.push_back(c);
        }
    } else if (tree == 2) {
        std::string up = fm.features[feature].parent;
        for (int d = 0; !up.empty() && fm.features.count(up) && d < 64; ++d) {
            feats.insert(up);
            up = fm.features[up].parent;
        }
    }
    // A component shared by several features in the set counts once
    // (probe: Root CHILDREN 488, not the 632 a per-feature sum gives).
    std::set<std::string> comps;
    for (const std::string& f : feats)
        for (const std::string& c : fm.features[f].components) comps.insert(c);
    i64 total = 0;
    for (const std::string& c : comps) total += component_cost(session, fm.components[c], state);
    return total;
}

std::vector<VolumeCost> component_costs(Session& session, const std::string& component, i32 state) {
    FeatureModel& fm = feature_model(session);
    std::vector<VolumeCost> out;
    if (component.empty()) {
        // Learn: the whole install; piCost 0, piTempCost the installer's own
        // (probe "EnumCost empty-name": C: cost 0 temp 96).
        std::set<std::string> volumes{system_volume(session)};
        for (const auto& [name, c] : fm.components)
            if (c.request != kUnknown) volumes.insert(volume_of(component_directory(session, name)));
        volumes.erase("");
        for (const std::string& v : volumes)
            out.push_back({v, 0, v == system_volume(session) ? engine_temp_bytes(session) : 0});
        return out;
    }
    auto it = fm.components.find(component);
    if (it == fm.components.end()) return out;
    std::string volume = volume_of(target_path(session, it->second.directory));
    if (volume.empty()) volume = system_volume(session);
    out.push_back({volume, component_cost(session, it->second, state), 0});
    return out;
}

// ---- actions ---------------------------------------------------------------------------------

u32 action_cost_initialize(ActionContext& ctx) {
    Session& s = ctx.session();
    Internal& in = internal(s);
    in.vfs = ctx.services().vfs;
    in.registry = ctx.services().registry;
    if (in.vfs == nullptr && ctx.services().manager) in.vfs = ctx.services().manager->vfs();
    if (in.registry == nullptr && ctx.services().manager) in.registry = ctx.services().manager->registry();
    load_tables(s);
    install_resolvers(s);
    install_file_state_resolvers(s);  // [#file] / [!file] (files area)
    resolve_sources(s);
    // probe phase costinitialize: CostingComplete "0" until costing finishes.
    s.set_property("CostingComplete", "0");
    return kActionSuccess;
}

u32 action_file_cost(ActionContext& ctx) {
    FeatureModel& fm = feature_model(ctx.session());
    // probe "FileCost alone": 1603 without CostInitialize.
    if (!fm.loaded) return kActionFailure;
    fm.file_costed = true;
    return kActionSuccess;
}

u32 action_cost_finalize(ActionContext& ctx) {
    Session& s = ctx.session();
    FeatureModel& fm = feature_model(s);
    Internal& in = internal(s);
    // probe "CostFinalize alone": 1603 without CostInitialize; it does not
    // need FileCost ("CostFinalize without FileCost" 0).
    if (!fm.loaded) return kActionFailure;
    if (in.vfs == nullptr) in.vfs = ctx.services().vfs;
    if (in.registry == nullptr) in.registry = ctx.services().registry;
    fm.finalized = false;
    in.component_override.clear();
    in.reinstall.clear();
    in.never_overwrite_hit.clear();
    in.key_file_superseded.clear();
    resolve_targets(s);

    // Learn Condition table: a true condition sets the feature's level
    // (rows in table order; probe condition_disable / condition_raise). The
    // probe shows it applies with request properties set too
    // (condition_with_addlocal, msiexec condition_disable_addlocal).
    for (auto& [name, f] : fm.features) f.level = f.authored_level;
    if (const Table* t = s.package() ? s.package()->table("Condition") : nullptr) {
        Cols c(t, {"Feature_", "Level", "Condition"});
        for (const auto& row : t->rows) {
            auto it = fm.features.find(c.cell(row, 0));
            if (it == fm.features.end()) continue;
            const std::string cond = c.cell(row, 2);
            const auto v = s.evaluate_condition(cond);
            if (!v) log_unimplemented("CostFinalize", "Condition-table expression outside the grammar (treated false)");
            if (v.value_or(false)) it->second.level = c.integer(row, 1);
        }
    }
    const std::string level = s.property("INSTALLLEVEL");
    fm.install_level = level.empty() ? 1 : cell_int(level).value_or(1);

    load_installed_states(s);
    evaluate_components(s);
    for (auto& [name, f] : fm.features) f.request = kUnknown;

    std::set<std::string> changed;
    if (s.property("ACTION") == "ADMIN") {
        // Learn CostFinalize: an administrative install selects every feature
        // whose Level is not 0.
        for (const std::string& n : fm.feature_order)
            if (fm.features[n].level > 0) fm.features[n].request = kLocal;
    } else if (any_request_property(s)) {
        if (!apply_request_properties(s, changed)) {
            for (auto& [name, f] : fm.features) f.request = kUnknown;
            return kActionFailure;
        }
    } else {
        select_by_level(s, changed);
    }
    apply_tree(s, changed);
    fm.finalized = true;
    compute_components(s);
    update_cost_properties(s);
    return kActionSuccess;
}

u32 action_isolate_components(ActionContext& ctx) {
    Session& s = ctx.session();
    FeatureModel& fm = feature_model(s);
    // probe "IsolateComponents alone": 1603 without CostInitialize.
    if (!fm.loaded) return kActionFailure;
    // Learn IsolatedComponent: the files of Component_Shared also go into
    // the directory of each Component_Application (the files area installs
    // them there; ComponentInfo::isolated_into names the applications).
    for (auto& [name, c] : fm.components) c.isolated_into.clear();
    if (const Table* t = s.package() ? s.package()->table("IsolatedComponent") : nullptr) {
        Cols c(t, {"Component_Shared", "Component_Application"});
        for (const auto& row : t->rows) {
            auto shared = fm.components.find(c.cell(row, 0));
            if (shared == fm.components.end() || !fm.components.count(c.cell(row, 1))) continue;
            shared->second.isolated_into.push_back(c.cell(row, 1));
        }
    }
    return kActionSuccess;
}

u32 action_migrate_feature_states(ActionContext& ctx) {
    Session& s = ctx.session();
    FeatureModel& fm = feature_model(s);
    // Learn MigrateFeatureStates: first installs only, after CostFinalize;
    // probe "MigrateFeatureStates alone" and the msiexec logs (no Upgrade
    // table: "Return value 0"): nothing to migrate is ERROR_FUNCTION_NOT_CALLED.
    if (!fm.finalized || !s.property("Installed").empty() || s.property("REMOVE") == "ALL") return kActionNotCalled;
    const Table* t = s.package() ? s.package()->table("Upgrade") : nullptr;
    if (t == nullptr) return kActionNotCalled;
    InstalledStateProvider p;
    {
        std::lock_guard lock(provider_mutex());
        p = provider();
    }
    // Learn: the Upgrade rows' ActionProperty lists the related products
    // FindRelatedProducts found; msidbUpgradeAttributesMigrateFeatures (1).
    Cols c(t, {"UpgradeCode", "Attributes", "ActionProperty"});
    std::map<std::string, i32> migrated;
    bool any = false;
    auto rank = [](i32 st) { return st == kLocal ? 4 : st == kSource ? 3 : st == kAdvertised ? 2 : st == kAbsent ? 1 : 0; };
    for (const auto& row : t->rows) {
        if (!(c.integer(row, 1) & 1)) continue;
        std::vector<std::string> products = split_list(s.property(c.cell(row, 2)));
        if (products.empty() && p.related_products) products = p.related_products(s, c.cell(row, 0));
        for (std::string product : products) {
            if (product.empty()) continue;
            if (!p.product_features) {
                log_unimplemented("MigrateFeatureStates", "no installed-products database registered");
                continue;
            }
            any = true;
            Record r(1);
            r.fields[1].kind = Record::Kind::String;
            const std::vector<u16> w = utf8_to_utf16(product);
            r.fields[1].text.assign(w.begin(), w.end());
            (void)ctx.message(kMsgActionData, r);
            // Learn: precedence local, source, advertised, absent.
            for (const auto& [feature, st] : p.product_features(s, product))
                if (rank(st) > rank(migrated[feature])) migrated[feature] = st;
        }
    }
    if (!any) return kActionNotCalled;
    std::set<std::string> changed;
    for (const auto& [feature, st] : migrated) {
        if (!fm.features.count(feature)) continue;
        request_feature(s, feature, st == kAbsent ? kUnknown : st, changed);
    }
    apply_tree(s, changed);
    refresh(s, changed);
    return kActionSuccess;
}

void register_feature_actions(ActionRegistry& r) {
    r.add("CostInitialize", action_cost_initialize);
    r.add("FileCost", action_file_cost);
    r.add("CostFinalize", action_cost_finalize);
    r.add("IsolateComponents", action_isolate_components);
    r.add("MigrateFeatureStates", action_migrate_feature_states);
    static std::once_flag hook_once;
    std::call_once(hook_once, [] {
        // The client UI hands its selection to the server session as the
        // request properties and directory properties Learn's selection
        // rules read (ADDLOCAL / ADDSOURCE / ADVERTISE / REMOVE).
        add_execute_handoff_hook([](Session& client, Session& server) {
            FeatureModel& fm = feature_model(client);
            if (!fm.finalized) return;
            std::string local, source, advertise, remove;
            auto add = [](std::string& list, const std::string& name) {
                if (!list.empty()) list += ',';
                list += name;
            };
            for (const std::string& n : fm.feature_order) {
                const FeatureInfo& f = fm.features[n];
                if (f.action == kLocal) add(local, n);
                else if (f.action == kSource) add(source, n);
                else if (f.action == kAdvertised) add(advertise, n);
                else if (f.action == kAbsent && installed_kind(f.installed)) add(remove, n);
            }
            for (const char* p : {"ADDLOCAL", "ADDSOURCE", "ADDDEFAULT", "ADVERTISE", "REMOVE", "COMPADDLOCAL",
                                  "COMPADDSOURCE", "COMPADDDEFAULT", "FILEADDLOCAL", "FILEADDSOURCE", "FILEADDDEFAULT"})
                server.set_property(p, "");
            if (!local.empty()) server.set_property("ADDLOCAL", local);
            if (!source.empty()) server.set_property("ADDSOURCE", source);
            if (!advertise.empty()) server.set_property("ADVERTISE", advertise);
            if (!remove.empty()) server.set_property("REMOVE", remove);
            server.set_property("INSTALLLEVEL", std::to_string(fm.install_level));
            for (const auto& [key, d] : fm.directories)
                if (Session::is_public_property(key)) server.set_property(key, d.target);
        });
    });
}

// ---- exports -------------------------------------------------------------------------------

namespace {

struct FeatureApi {
    MsiManager& mgr;

    Session* session(u32 h) const {
        auto ih = mgr.handles().get<InstallHandle>(h);
        if (!ih || !ih->engine) return nullptr;
        Session& s = ih->engine->session();
        Internal& in = internal(s);
        if (in.vfs == nullptr) in.vfs = ih->engine->services().vfs ? ih->engine->services().vfs : mgr.vfs();
        if (in.registry == nullptr)
            in.registry = ih->engine->services().registry ? ih->engine->services().registry : mgr.registry();
        return &s;
    }

    static std::optional<std::string> read_str(Marshaler& m, GuestAddr p, bool wide) {
        if (p == 0) return std::nullopt;
        if (wide) {
            auto s = m.read_wide_string(p);
            if (s.is_err()) return std::nullopt;
            return s.value();
        }
        auto s = m.read_ansi_string(p);
        if (s.is_err()) return std::nullopt;
        return capsule::ansi_to_utf8(s.value());
    }

    /// The buffer convention measured for MsiGetTargetPath / MsiGetSourcePath
    /// / MsiEnumComponentCosts (probe arguments): *pcch is the capacity with
    /// the terminator, on return the length without it; a null buffer
    /// reports the length (ERROR_SUCCESS); a short buffer gets what fits,
    /// terminated, and ERROR_MORE_DATA; a zero capacity writes nothing.
    static u32 write_out(Marshaler& m, GuestAddr buf, GuestAddr pcch, const std::string& value, bool wide) {
        auto cap = m.read_u32(pcch);
        if (cap.is_err()) return kErrInvalidParameter;
        const u32 c = cap.value();
        if (wide) {
            const std::vector<u16> units = utf8_to_utf16(value);
            const u32 need = static_cast<u32>(units.size());
            (void)m.write_u32(pcch, need);
            if (buf == 0) return kErrSuccess;
            const u32 n = std::min(need, c > 0 ? c - 1 : 0);
            if (c > 0) {
                for (u32 k = 0; k < n; ++k) (void)m.write_u16(buf + k * 2, units[k]);
                (void)m.write_u16(buf + n * 2, 0);
            }
            return need + 1 > c ? kErrMoreData : kErrSuccess;
        }
        const std::string bytes = utf8_to_acp(value);
        const u32 need = static_cast<u32>(bytes.size());
        (void)m.write_u32(pcch, need);
        if (buf == 0) return kErrSuccess;
        const u32 n = std::min(need, c > 0 ? c - 1 : 0);
        if (c > 0) {
            for (u32 k = 0; k < n; ++k) (void)m.write_u8(buf + k, static_cast<u8>(bytes[k]));
            (void)m.write_u8(buf + n, 0);
        }
        return need + 1 > c ? kErrMoreData : kErrSuccess;
    }

    static void write_state(Marshaler& m, GuestAddr p, i32 v) {
        if (p != 0) (void)m.write_u32(p, static_cast<u32>(v));
    }

    // UINT MsiGetFeatureState(hInstall, szFeature, *piInstalled, *piAction)
    void get_feature_state(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        // probe: null name and an unloaded Feature table are 1606; null
        // outputs are skipped.
        const FeatureInfo* f = name ? find_feature(*s, *name) : nullptr;
        if (!f) return m.set_return(kErrUnknownFeature);
        write_state(m, m.arg_ptr(2), f->installed);
        write_state(m, m.arg_ptr(3), f->action);
        m.set_return(kErrSuccess);
    }

    void get_component_state(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        const ComponentInfo* c = name ? find_component(*s, *name) : nullptr;
        if (!c) return m.set_return(kErrUnknownComponent);
        write_state(m, m.arg_ptr(2), c->installed);
        write_state(m, m.arg_ptr(3), c->action);
        m.set_return(kErrSuccess);
    }

    void set_feature_state_api(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const i32 state = static_cast<i32>(m.arg32(2));
        if (state != kAdvertised && state != kAbsent && state != kLocal && state != kSource)
            return m.set_return(kErrInvalidParameter);
        if (!feature_model(*s).finalized) return m.set_return(kErrFunctionFailed);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        if (!name) return m.set_return(kErrUnknownFeature);
        m.set_return(set_feature_state(*s, *name, state));
    }

    void set_component_state_api(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const i32 state = static_cast<i32>(m.arg32(2));
        if (state != kAbsent && state != kLocal && state != kSource) return m.set_return(kErrInvalidParameter);
        if (!feature_model(*s).finalized) return m.set_return(kErrFunctionFailed);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        if (!name) return m.set_return(kErrUnknownComponent);
        m.set_return(set_component_state(*s, *name, state));
    }

    void get_valid_states(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        const GuestAddr out = m.arg_ptr(2);
        if (!name || out == 0) return m.set_return(kErrInvalidParameter);
        if (!feature_model(*s).loaded) return m.set_return(kErrInvalidHandleState);
        const FeatureInfo* f = find_feature(*s, *name);
        if (!f) return m.set_return(kErrUnknownFeature);
        (void)m.write_u32(out, valid_states_of(*s, *f));
        m.set_return(kErrSuccess);
    }

    // UINT MsiGetFeatureCost(hInstall, szFeature, MSICOSTTREE, INSTALLSTATE, INT* piCost)
    void get_feature_cost(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const i32 tree = static_cast<i32>(m.arg32(2));
        const i32 state = static_cast<i32>(m.arg32(3));
        const GuestAddr out = m.arg_ptr(4);
        if (out == 0 || tree < 0 || tree > 2) return m.set_return(kErrInvalidParameter);
        if (state != kUnknown && state != kAbsent && state != kLocal && state != kSource && state != kDefault)
            return m.set_return(kErrInvalidParameter);
        if (!feature_model(*s).loaded) return m.set_return(kErrInvalidHandleState);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        if (!name || !find_feature(*s, *name)) return m.set_return(kErrUnknownFeature);
        (void)m.write_u32(out, static_cast<u32>(static_cast<i32>(feature_cost(*s, *name, tree, state) / 512)));
        m.set_return(kErrSuccess);
    }

    // UINT MsiEnumComponentCosts(hInstall, szComponent, dwIndex, iState,
    //                            szDriveBuf, pcchDriveBuf, piCost, piTempCost)
    void enum_component_costs(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        const u32 index = m.arg32(2);
        const i32 state = static_cast<i32>(m.arg32(3));
        const GuestAddr buf = m.arg_ptr(4), pcch = m.arg_ptr(5), cost = m.arg_ptr(6), temp = m.arg_ptr(7);
        if (buf == 0 || pcch == 0 || cost == 0 || temp == 0) return m.set_return(kErrInvalidParameter);
        if (state != kUnknown && state != kAbsent && state != kLocal && state != kSource && state != kDefault)
            return m.set_return(kErrInvalidParameter);
        const bool whole = !name || name->empty();
        FeatureModel& fm = feature_model(*s);
        if (!fm.loaded) return m.set_return(whole ? kErrFunctionNotCalled : kErrInvalidHandleState);
        if (!internal(*s).costing_complete) return m.set_return(kErrFunctionNotCalled);
        if (!whole && !find_component(*s, *name)) return m.set_return(kErrUnknownComponent);
        const auto costs = component_costs(*s, whole ? std::string{} : *name, state);
        if (index >= costs.size()) return m.set_return(kErrNoMoreItems);
        const VolumeCost& v = costs[index];
        // probe "EnumCost cap": the costs are written even on ERROR_MORE_DATA.
        (void)m.write_u32(cost, static_cast<u32>(static_cast<i32>(v.cost / 512)));
        (void)m.write_u32(temp, static_cast<u32>(static_cast<i32>(v.temp_cost / 512)));
        m.set_return(write_out(m, buf, pcch, v.volume, wide));
    }

    void get_path(Marshaler& m, bool wide, bool source) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        const GuestAddr buf = m.arg_ptr(2), pcch = m.arg_ptr(3);
        if (!name || pcch == 0) return m.set_return(kErrInvalidParameter);
        const std::string path = source ? source_path(*s, *name) : target_path(*s, *name);
        if (path.empty()) return m.set_return(kErrDirectory);
        m.set_return(write_out(m, buf, pcch, path, wide));
    }

    void set_target_path_api(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        const auto folder = read_str(m, m.arg_ptr(1), wide);
        const auto path = read_str(m, m.arg_ptr(2), wide);
        if (!folder || !path) return m.set_return(kErrInvalidParameter);
        m.set_return(set_target_path(*s, *folder, *path));
    }

    void set_install_level_api(Marshaler& m) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        m.set_return(set_install_level(*s, static_cast<i32>(m.arg32(1))));
    }

    void set_feature_attributes_api(Marshaler& m, bool wide) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        FeatureModel& fm = feature_model(*s);
        if (!fm.loaded || fm.finalized) return m.set_return(kErrFunctionFailed);
        const auto name = read_str(m, m.arg_ptr(1), wide);
        if (!name) return m.set_return(kErrUnknownFeature);
        m.set_return(set_feature_attributes(*s, *name, m.arg32(2)));
    }

    void verify_disk_space(Marshaler& m) {
        Session* s = session(m.arg32(0));
        if (!s) return m.set_return(kErrInvalidHandle);
        // probe: ERROR_SUCCESS before costing too.
        if (!internal(*s).costing_complete) return m.set_return(kErrSuccess);
        for (const auto& [volume, bytes] : required_by_volume(*s)) {
            const auto g = geometry(*s, volume);
            if (g.available && bytes > *g.available) return m.set_return(kErrDiskFull);
        }
        m.set_return(kErrSuccess);
    }
};

}  // namespace

void register_feature_api(MsiManager& mgr, HandlerMap& map) {
    auto api = std::make_shared<FeatureApi>(FeatureApi{mgr});
    for (const bool wide : {false, true}) {
        const std::string sfx = wide ? "W" : "A";
        map["MsiGetFeatureState" + sfx] = [api, wide](Marshaler& m) { api->get_feature_state(m, wide); };
        map["MsiGetComponentState" + sfx] = [api, wide](Marshaler& m) { api->get_component_state(m, wide); };
        map["MsiSetFeatureState" + sfx] = [api, wide](Marshaler& m) { api->set_feature_state_api(m, wide); };
        map["MsiSetComponentState" + sfx] = [api, wide](Marshaler& m) { api->set_component_state_api(m, wide); };
        map["MsiGetFeatureValidStates" + sfx] = [api, wide](Marshaler& m) { api->get_valid_states(m, wide); };
        map["MsiGetFeatureCost" + sfx] = [api, wide](Marshaler& m) { api->get_feature_cost(m, wide); };
        map["MsiEnumComponentCosts" + sfx] = [api, wide](Marshaler& m) { api->enum_component_costs(m, wide); };
        map["MsiGetTargetPath" + sfx] = [api, wide](Marshaler& m) { api->get_path(m, wide, false); };
        map["MsiGetSourcePath" + sfx] = [api, wide](Marshaler& m) { api->get_path(m, wide, true); };
        map["MsiSetTargetPath" + sfx] = [api, wide](Marshaler& m) { api->set_target_path_api(m, wide); };
        map["MsiSetFeatureAttributes" + sfx] = [api, wide](Marshaler& m) { api->set_feature_attributes_api(m, wide); };
    }
    map["MsiSetInstallLevel"] = [api](Marshaler& m) { api->set_install_level_api(m); };
    map["MsiVerifyDiskSpace"] = [api](Marshaler& m) { api->verify_disk_space(m); };
}

}  // namespace vitreo::winapi::msi
