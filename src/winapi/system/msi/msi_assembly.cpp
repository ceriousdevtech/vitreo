/// @file msi_assembly.cpp
/// @brief Windows Installer assemblies: the MsiAssembly / MsiAssemblyName
/// tables, the .NET global assembly cache layout, MsiPublishAssemblies /
/// MsiUnpublishAssemblies, and the InstallFiles / RemoveFiles part for global
/// assemblies (staged during the script, committed after it). See
/// msi_assembly.hpp for the measured contract and
/// .docs/contracts/msi/registration.md "Assemblies".

#include "winapi/system/msi/msi_assembly.hpp"

#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_features.hpp"
#include "winapi/system/msi/msi_files.hpp"
#include "winapi/system/msi/msi_product.hpp"
#include "winapi/system/msi/msi_register.hpp"
#include "winapi/system/msi/msi_register_internal.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <random>

namespace vitreo::winapi::msi {

using namespace reg_detail;

namespace {

constexpr i32 kIdCancel = 2;  // winuser.h IDCANCEL: the handler cancels the install

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string_view unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

std::vector<std::string_view> split_commas(std::string_view s) {
    std::vector<std::string_view> out;
    usize start = 0;
    for (;;) {
        const usize comma = s.find(',', start);
        out.push_back(s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string with_slash(std::string p) {
    if (!p.empty() && p.back() != '\\') p += '\\';
    return p;
}

std::string windows_dir_of(Session& s) {
    std::string w = s.property("WindowsFolder");
    while (!w.empty() && w.back() == '\\') w.pop_back();
    return w;
}

/// "1.2.3.4" -> up to four numeric parts; nullopt when malformed.
std::optional<std::vector<u32>> version_parts(std::string_view v) {
    std::vector<u32> parts;
    usize start = 0;
    for (;;) {
        const usize dot = v.find('.', start);
        const std::string_view part = v.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (part.empty() || part.size() > 5) return std::nullopt;
        u32 n = 0;
        for (char c : part) {
            if (c < '0' || c > '9') return std::nullopt;
            n = n * 10 + static_cast<u32>(c - '0');
        }
        if (n > 65535) return std::nullopt;
        parts.push_back(n);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (parts.size() > 4) return std::nullopt;
    return parts;
}

std::string culture_key(std::string_view c) { return iequals(c, "neutral") ? std::string{} : lower(std::string(c)); }
std::string token_key(std::string_view t) { return iequals(t, "null") ? std::string{} : lower(std::string(t)); }

/// The five fusion identity fields of a .NET name, normalized: a publication
/// of the same assembly however the package spelled it.
bool same_identity(const AssemblyName& a, const AssemblyName& b) {
    if (!iequals(a.name, b.name)) return false;
    auto field = [](const AssemblyName& n, std::string_view k) -> std::string {
        const std::string* v = n.find(k);
        if (v == nullptr) return "\x01";  // absent differs from every value
        if (k == "culture") return culture_key(*v);
        if (k == "publickeytoken") return token_key(*v);
        return lower(*v);
    };
    for (std::string_view k : {"version", "culture", "publickeytoken", "processorarchitecture"})
        if (field(a, k) != field(b, k)) return false;
    return true;
}

std::optional<std::vector<u8>> read_file(capsule::Vfs& vfs, const std::string& path) {
    auto h = vfs.open(path, capsule::AccessMode::Read, capsule::ShareMode::Read, capsule::CreationDisposition::OpenExisting);
    if (h.is_err()) return std::nullopt;
    std::vector<u8> out;
    std::vector<u8> chunk(1 << 16);
    for (;;) {
        auto n = vfs.read(h.value(), chunk);
        if (n.is_err()) {
            (void)vfs.close(h.value());
            return std::nullopt;
        }
        if (n.value() == 0) break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
    (void)vfs.close(h.value());
    return out;
}

bool exists(capsule::Vfs& vfs, const std::string& path) { return vfs.stat(path).is_ok(); }

void ensure_dirs(capsule::Vfs& vfs, const std::string& dir) {
    std::string d = dir;
    while (!d.empty() && d.back() == '\\') d.pop_back();
    if (d.size() <= 2 || exists(vfs, d)) return;
    const usize slash = d.rfind('\\');
    if (slash != std::string::npos) ensure_dirs(vfs, d.substr(0, slash));
    (void)vfs.create_dir(d);
}

/// Remove @p dir and everything in it (a cache directory: files only).
void remove_tree(capsule::Vfs& vfs, const std::string& dir) {
    std::string d = dir;
    while (!d.empty() && d.back() == '\\') d.pop_back();
    if (auto list = vfs.list_dir(d); list.is_ok()) {
        for (const auto& e : list.value()) {
            if (e.name == "." || e.name == "..") continue;
            const std::string child = d + "\\" + e.name;
            if (capsule::has_attribute(e.info.attributes, capsule::FileAttributes::Directory)) remove_tree(vfs, child);
            else (void)vfs.remove(child);
        }
    }
    (void)vfs.remove(d);
}

bool dir_empty(capsule::Vfs& vfs, const std::string& dir) {
    auto list = vfs.list_dir(dir);
    if (list.is_err()) return false;
    for (const auto& e : list.value())
        if (e.name != "." && e.name != "..") return false;
    return true;
}

/// The GAC_* directory of an architecture; empty for none.
std::string gac_name(std::string_view arch) {
    if (iequals(arch, "MSIL")) return "GAC_MSIL";
    if (iequals(arch, "x86")) return "GAC_32";
    if (iequals(arch, "AMD64") || iequals(arch, "IA64")) return "GAC_64";
    return {};
}

/// "<version>_<culture>_<token>" (culture empty when neutral, token lower case).
std::string version_dir(const AssemblyName& id) {
    const std::string* v = id.find("version");
    const std::string* c = id.find("culture");
    const std::string* t = id.find("publickeytoken");
    return (v ? *v : std::string{}) + "_" + (c ? culture_key(*c) : std::string{}) + "_" + (t ? token_key(*t) : std::string{});
}

/// Every cache directory @p id may live in, as Windows reports it:
/// .NET 4 layout first, then the .NET 2 / 1.x one.
std::vector<std::string> cache_candidates(const std::string& windows, const AssemblyName& id) {
    std::vector<std::string> gacs;
    if (const std::string* a = id.find("processorarchitecture")) {
        const std::string g = gac_name(*a);
        if (!g.empty()) gacs.push_back(g);
    } else {
        gacs = {"GAC_MSIL", "GAC_32", "GAC_64"};
    }
    std::vector<std::string> out;
    for (const std::string& g : gacs)
        out.push_back(windows + "\\Microsoft.Net\\assembly\\" + g + "\\" + id.name + "\\v4.0_" + version_dir(id) + "\\");
    for (const std::string& g : gacs)
        out.push_back(windows + "\\assembly\\" + g + "\\" + id.name + "\\" + version_dir(id) + "\\");
    if (id.find("processorarchitecture") == nullptr)
        out.push_back(windows + "\\assembly\\GAC\\" + id.name + "\\" + version_dir(id) + "\\");
    return out;
}

const std::string* cell_ptr(const std::vector<std::pair<std::string, std::string>>& v, std::string_view key) {
    for (const auto& [k, val] : v)
        if (k == key) return &val;
    return nullptr;
}

/// A text record for ActionData / errors.
Record text_record(std::initializer_list<std::string> fields) {
    Record r(static_cast<u32>(fields.size()));
    u32 i = 1;
    for (const std::string& f : fields) {
        r.fields[i].kind = Record::Kind::String;
        const std::vector<u16> u = utf8_to_utf16(f);
        r.fields[i].text.assign(u.begin(), u.end());
        ++i;
    }
    return r;
}

std::string random_staging_name() {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<usize> pick(0, sizeof(kChars) - 2);
    std::string s;
    for (int i = 0; i < 8; ++i) s += kChars[pick(gen)];
    return s;
}

std::string manifest_file_name(Session& s, const PackageAssembly& a) {
    const FileRow* f = find_file_row(s, a.manifest);
    return f ? target_file_name(s, *f) : std::string{};
}

}  // namespace

// ---- package side --------------------------------------------------------------------------

std::string PackageAssembly::identity() const {
    std::string out = name;
    for (const auto& [k, v] : attributes) out += "," + k + "=\"" + v + "\"";
    return out;
}

std::vector<PackageAssembly> package_assemblies(Session& session) {
    std::vector<PackageAssembly> out;
    Rows rows(session, "MsiAssembly");
    if (rows.size() == 0) return out;
    Rows names(session, "MsiAssemblyName");
    for (usize i = 0; i < rows.size(); ++i) {
        PackageAssembly a;
        a.component = rows.get(i, "Component_");
        a.feature = rows.get(i, "Feature_");
        a.manifest = rows.get(i, "File_Manifest");
        a.application = rows.get(i, "File_Application");
        // Learn (MsiAssembly.Attributes): 1 Win32, 0 or NULL .NET.
        a.win32 = rows.get(i, "Attributes") == "1";
        for (usize n = 0; n < names.size(); ++n) {
            if (names.get(n, "Component_") != a.component) continue;
            const std::string key = names.get(n, "Name"), value = names.get(n, "Value");
            if (iequals(key, "name")) a.name = value;
            else a.attributes.emplace_back(key, value);
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::optional<PackageAssembly> package_assembly(Session& session, const std::string& component) {
    for (PackageAssembly& a : package_assemblies(session))
        if (a.component == component) return std::move(a);
    return std::nullopt;
}

std::string assembly_key_path(Session& session, const PackageAssembly& assembly, const std::string& key_file) {
    std::string file;
    if (key_file != assembly.manifest)
        if (const FileRow* f = find_file_row(session, key_file)) file = target_file_name(session, *f);
    return std::string(assembly.win32 ? ">" : "<") + file + "\\" + assembly.identity();
}

bool is_assembly_key_path(std::string_view key_path) {
    return !key_path.empty() && (key_path.front() == '<' || key_path.front() == '>');
}

// ---- names ----------------------------------------------------------------------------------

const std::string* AssemblyName::find(std::string_view attribute) const { return cell_ptr(attributes, attribute); }

std::optional<AssemblyName> parse_net_assembly_name(std::string_view text) {
    const auto items = split_commas(text);
    AssemblyName out;
    out.name = std::string(unquote(trim(items.front())));
    if (out.name.empty()) return std::nullopt;
    for (usize i = 1; i < items.size(); ++i) {
        const std::string_view item = trim(items[i]);
        const usize eq = item.find('=');
        if (item.empty() || eq == std::string_view::npos) return std::nullopt;
        const std::string key = lower(std::string(trim(item.substr(0, eq))));
        const std::string value(unquote(trim(item.substr(eq + 1))));
        if (key.empty() || out.find(key) != nullptr) return std::nullopt;
        if (key == "version" && !version_parts(value)) return std::nullopt;
        out.attributes.emplace_back(key, value);
    }
    return out;
}

std::optional<AssemblyName> parse_win32_assembly_name(std::string_view text) {
    static constexpr std::string_view kKnown[] = {"type", "version", "processorArchitecture", "publicKeyToken", "language",
                                                  "versionScope"};
    const auto items = split_commas(text);
    AssemblyName out;
    out.name = std::string(items.front());
    if (out.name.empty()) return std::nullopt;
    for (usize i = 1; i < items.size(); ++i) {
        const std::string_view item = items[i];
        if (item.empty()) {
            if (i + 1 == items.size()) break;  // a trailing ',' is accepted
            return std::nullopt;
        }
        const usize eq = item.find('=');
        if (eq == std::string_view::npos) return std::nullopt;
        const std::string_view key = item.substr(0, eq), quoted = item.substr(eq + 1);
        if (std::find(std::begin(kKnown), std::end(kKnown), key) == std::end(kKnown)) return std::nullopt;
        if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"') return std::nullopt;
        if (out.find(key) != nullptr) return std::nullopt;
        out.attributes.emplace_back(std::string(key), std::string(quoted.substr(1, quoted.size() - 2)));
    }
    return out;
}

bool net_name_matches(const AssemblyName& request, const AssemblyName& registered) {
    if (!iequals(request.name, registered.name)) return false;
    for (const auto& [key, want] : request.attributes) {
        if (key != "version" && key != "culture" && key != "publickeytoken" && key != "processorarchitecture") continue;
        const std::string* have = registered.find(key);
        if (have == nullptr) return false;
        if (key == "version") {
            const auto w = version_parts(want), h = version_parts(*have);
            if (!w || !h) return false;
            for (usize k = 0; k < w->size(); ++k)
                if (k >= h->size() || (*w)[k] != (*h)[k]) return false;
        } else if (key == "culture") {
            if (culture_key(want) != culture_key(*have)) return false;
        } else if (key == "publickeytoken") {
            if (token_key(want) != token_key(*have)) return false;
        } else if (!iequals(want, *have)) {
            return false;
        }
    }
    return true;
}

bool win32_name_matches(const AssemblyName& request, const AssemblyName& registered) {
    if (!iequals(request.name, registered.name) || request.attributes.size() != registered.attributes.size()) return false;
    for (const auto& [key, want] : request.attributes) {
        const std::string* have = registered.find(key);
        if (have == nullptr || !iequals(want, *have)) return false;
    }
    return true;
}

// ---- the cache -------------------------------------------------------------------------------

std::optional<ClrImage> read_clr_image(std::span<const u8> f) {
    auto u16at = [&](usize o) -> std::optional<u32> {
        if (o + 2 > f.size()) return std::nullopt;
        return u32{f[o]} | (u32{f[o + 1]} << 8);
    };
    auto u32at = [&](usize o) -> std::optional<u32> {
        if (o + 4 > f.size()) return std::nullopt;
        return u32{f[o]} | (u32{f[o + 1]} << 8) | (u32{f[o + 2]} << 16) | (u32{f[o + 3]} << 24);
    };
    // winnt.h: IMAGE_DOS_HEADER.e_lfanew at 0x3C, "PE\0\0", IMAGE_FILE_HEADER,
    // IMAGE_OPTIONAL_HEADER32/64 (magic 0x10b / 0x20b), data directory 14
    // (IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR) at optional-header offset 96 / 112.
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return std::nullopt;
    const auto pe = u32at(0x3C);
    if (!pe || !u32at(*pe) || *u32at(*pe) != 0x00004550u) return std::nullopt;
    const auto machine = u16at(*pe + 4), sections = u16at(*pe + 6), opt_size = u16at(*pe + 20);
    const usize opt = *pe + 24;
    const auto magic = u16at(opt);
    if (!machine || !sections || !opt_size || !magic) return std::nullopt;
    const bool pe64 = *magic == 0x20B;
    if (!pe64 && *magic != 0x10B) return std::nullopt;
    const usize dirs = opt + (pe64 ? 112 : 96);
    const auto com_rva = u32at(dirs + 14 * 8);
    if (!com_rva || *com_rva == 0) return std::nullopt;
    const usize table = opt + *opt_size;
    auto to_offset = [&](u32 rva) -> std::optional<usize> {
        for (u32 i = 0; i < *sections; ++i) {
            const usize s = table + i * 40;
            const auto vsize = u32at(s + 8), va = u32at(s + 12), raw_size = u32at(s + 16), raw = u32at(s + 20);
            if (!vsize || !va || !raw_size || !raw) return std::nullopt;
            const u32 span = std::max(*vsize, *raw_size);
            if (rva >= *va && rva < *va + span) return usize{*raw} + (rva - *va);
        }
        return std::nullopt;
    };
    // IMAGE_COR20_HEADER: cb, runtime major/minor, MetaData {rva, size}, Flags.
    const auto cor = to_offset(*com_rva);
    if (!cor) return std::nullopt;
    const auto md_rva = u32at(*cor + 8), flags = u32at(*cor + 16);
    if (!md_rva || !flags) return std::nullopt;
    // ECMA-335 II.24.2.1: metadata root "BSJB", then the version string.
    const auto md = to_offset(*md_rva);
    if (!md || !u32at(*md) || *u32at(*md) != 0x424A5342u) return std::nullopt;
    const auto len = u32at(*md + 12);
    if (!len || *md + 16 + *len > f.size()) return std::nullopt;
    ClrImage out;
    for (u32 i = 0; i < *len && f[*md + 16 + i] != 0; ++i) out.runtime += static_cast<char>(f[*md + 16 + i]);
    // winnt.h COMIMAGE_FLAGS_ILONLY 0x1, _32BITREQUIRED 0x2, _32BITPREFERRED
    // 0x20000; IMAGE_FILE_MACHINE_I386 0x14c, AMD64 0x8664, IA64 0x200.
    constexpr u32 kIlOnly = 0x1, k32Required = 0x2, k32Preferred = 0x20000;
    if (pe64) {
        if (*machine == 0x8664) out.architecture = "AMD64";
        else if (*machine == 0x0200) out.architecture = "IA64";
        else return std::nullopt;
    } else {
        if (*machine != 0x014C) return std::nullopt;
        const bool x86 = (*flags & kIlOnly) == 0 || ((*flags & k32Required) != 0 && (*flags & k32Preferred) == 0);
        out.architecture = x86 ? "x86" : "MSIL";
    }
    if (out.runtime.size() < 2 || out.runtime[0] != 'v' || !std::isdigit(static_cast<unsigned char>(out.runtime[1])))
        return std::nullopt;
    return out;
}

std::string gac_directory(const std::string& windows_dir, const ClrImage& image, const AssemblyName& identity) {
    const int major = std::atoi(image.runtime.c_str() + 1);
    if (major >= 4)
        return windows_dir + "\\Microsoft.NET\\assembly\\" + gac_name(image.architecture) + "\\" + identity.name + "\\v4.0_" +
               version_dir(identity) + "\\";
    // .NET 1.x images carry no architecture: the CLR 2 cache keeps them in GAC
    // (measured: stdole 7.0.3300.0 in %WINDIR%\assembly\GAC).
    const std::string gac = major >= 2 ? gac_name(image.architecture) : std::string("GAC");
    return windows_dir + "\\assembly\\" + gac + "\\" + identity.name + "\\" + version_dir(identity) + "\\";
}

std::optional<std::string> find_global_assembly_file(capsule::Vfs& vfs, const std::string& windows_dir,
                                                     const AssemblyName& identity, const std::string& file_name) {
    std::vector<std::string> names;
    if (!file_name.empty()) names.push_back(file_name);
    else names = {identity.name + ".dll", identity.name + ".exe"};
    for (const std::string& dir : cache_candidates(windows_dir, identity))
        for (const std::string& n : names)
            if (exists(vfs, dir + n)) return dir + n;
    return std::nullopt;
}

std::optional<std::string> resolve_assembly_key_path(capsule::Vfs& vfs, const std::string& windows_dir,
                                                     std::string_view key_path, const char* api) {
    if (!is_assembly_key_path(key_path)) return std::nullopt;
    const usize slash = key_path.find('\\');
    if (slash == std::string_view::npos) return std::nullopt;
    if (key_path.front() == '>') {
        log_unimplemented(api, "a global Win32 assembly's WinSxS path (the component-store hash is undocumented)");
        return std::nullopt;
    }
    const auto id = parse_net_assembly_name(key_path.substr(slash + 1));
    if (!id) return std::nullopt;
    return find_global_assembly_file(vfs, windows_dir, *id, std::string(key_path.substr(1, slash - 1)));
}

// ---- InstallFiles / RemoveFiles -------------------------------------------------------------

AssemblyInstall plan_assembly_install(Session& session, capsule::Vfs* vfs, const std::string& component) {
    AssemblyInstall out;
    const auto a = package_assembly(session, component);
    if (!a || !a->global()) return out;
    if (a->win32) {
        log_unimplemented("InstallFiles", "global Win32 assembly (WinSxS: the component-store directory hash is "
                                          "undocumented and no Vitreo loader probes WinSxS)");
        out.kind = AssemblyInstall::Kind::Unsupported;
        return out;
    }
    const auto id = parse_net_assembly_name(a->identity());
    if (vfs != nullptr && id) {
        const std::string windows = windows_dir_of(session);
        if (const auto cached = find_global_assembly_file(*vfs, windows, *id, manifest_file_name(session, *a))) {
            // Measured (refcount-p2 ops): a product whose assembly is already
            // in the cache copies nothing. An in-place update by FileVersion
            // (Learn "Updating Assemblies") is not done.
            for (const auto& [k, v] : a->attributes) {
                if (!iequals(k, "FileVersion")) continue;
                const ExistingFile have = probe_existing_file(*vfs, *cached);
                if (have.info.version != parse_file_version(v)) {
                    log_unimplemented("InstallFiles", "in-place update of a global assembly by FileVersion");
                    out.kind = AssemblyInstall::Kind::Unsupported;
                    return out;
                }
            }
            out.kind = AssemblyInstall::Kind::AlreadyCached;
            return out;
        }
    }
    out.kind = AssemblyInstall::Kind::Stage;
    out.staging = windows_dir_of(session) + "\\assembly\\tmp\\" + random_staging_name() + "\\";
    return out;
}

void schedule_assembly_commit(ActionContext& ctx, const std::string& component, const std::string& staging) {
    Session& s = ctx.session();
    const auto a = package_assembly(s, component);
    if (!a) return;
    std::vector<std::string> files;
    for (const FileRow* f : file_rows_of_component(s, component)) files.push_back(target_file_name(s, *f));
    const std::string manifest = manifest_file_name(s, *a), windows = windows_dir_of(s), identity = a->identity();
    Services* services = &ctx.services();
    auto target = std::make_shared<std::string>();

    // First step (Learn "Rollback of Assemblies in the Global Assembly
    // Cache"): the staged assembly is checked while the script runs; a
    // failure rolls the install back with the staged copies.
    ScriptOp check;
    check.action = "InstallFiles";
    check.run = [=]() -> u32 {
        capsule::Vfs* vfs = services->vfs;
        if (vfs == nullptr) return kActionFailure;
        const auto bytes = read_file(*vfs, staging + manifest);
        const auto image = bytes ? read_clr_image(*bytes) : std::nullopt;
        const auto id = parse_net_assembly_name(identity);
        if (!image || !id) {
            std::fprintf(stderr, "[vitreo] msi: %s is not a .NET assembly the global assembly cache takes\n",
                         (staging + manifest).c_str());
            return kActionFailure;
        }
        const std::string* token = id->find("publickeytoken");
        if (token == nullptr || token_key(*token).empty()) {
            std::fprintf(stderr, "[vitreo] msi: global assembly %s has no PublicKeyToken (not strong-named)\n",
                         identity.c_str());
            return kActionFailure;
        }
        *target = gac_directory(windows, *image, *id);
        return kActionSuccess;
    };
    ctx.schedule(std::move(check));

    // Second step: after the script succeeded the staged files replace the
    // cache's (measured: a commit custom action already sees them).
    ScriptOp commit;
    commit.action = "InstallFiles";
    commit.commit_only = true;
    commit.run = [=]() -> u32 {
        capsule::Vfs* vfs = services->vfs;
        if (vfs == nullptr || target->empty()) return kActionSuccess;
        ensure_dirs(*vfs, *target);
        for (const std::string& f : files) {
            const std::string from = staging + f, to = *target + f;
            if (!exists(*vfs, from)) continue;
            if (exists(*vfs, to)) (void)vfs->remove(to);
            if (vfs->rename(from, to).is_err()) {
                if (const auto bytes = read_file(*vfs, from)) {
                    auto h = vfs->open(to, capsule::AccessMode::Write, capsule::ShareMode::None,
                                       capsule::CreationDisposition::CreateAlways);
                    if (h.is_ok()) {
                        (void)vfs->write(h.value(), *bytes);
                        (void)vfs->close(h.value());
                    }
                    (void)vfs->remove(from);
                }
            }
        }
        std::string dir = staging;
        while (!dir.empty() && dir.back() == '\\') dir.pop_back();
        if (dir_empty(*vfs, dir)) (void)vfs->remove(dir);
        return kActionSuccess;
    };
    ctx.schedule(std::move(commit));
}

bool schedule_assembly_removal(ActionContext& ctx, const std::string& component) {
    Session& s = ctx.session();
    const auto a = package_assembly(s, component);
    if (!a || !a->global()) return false;
    // A global Win32 assembly is never installed here (see InstallFiles):
    // nothing of it is in the component's directory or anywhere else.
    if (a->win32) return true;
    const auto id = parse_net_assembly_name(a->identity());
    if (!id) return true;
    capsule::Registry* registry = ctx.services().registry;
    Services* services = &ctx.services();
    const auto db = action_database(ctx);
    const std::string sid = db ? db->user_sid() : std::string{};
    const std::string windows = windows_dir_of(s), manifest = manifest_file_name(s, *a);
    const AssemblyName identity = *id;
    ScriptOp commit;
    commit.action = "RemoveFiles";
    commit.commit_only = true;
    commit.run = [=]() -> u32 {
        capsule::Vfs* vfs = services->vfs;
        if (vfs == nullptr) return kActionSuccess;
        // The cache copy stays while any product still publishes it (measured
        // refcount: removed with the last product). MsiUnpublishAssemblies ran
        // earlier in the script, so only other products' entries remain.
        if (registry != nullptr) {
            ProductDatabase d(*registry, sid);
            for (const u32 context : {kContextMachine, kContextUserUnmanaged, kContextUserManaged}) {
                const auto keys = d.keys(context, sid);
                auto k = registry->open_key(registry->view_root(keys.advertised_hive, capsule::RegView::Native64), keys.advertised_root + L"\\Assemblies\\Global", false);
                if (k.is_err()) continue;
                bool referenced = false;
                if (auto names = registry->enum_values(k.value()); names.is_ok()) {
                    for (const std::wstring& n : names.value()) {
                        const auto other = parse_net_assembly_name(narrow(n));
                        if (!other || !same_identity(*other, identity)) continue;
                        auto v = registry->query_value(k.value(), n);
                        if (v.is_ok())
                            if (const auto* list = std::get_if<std::vector<std::wstring>>(&v.value().data))
                                referenced = std::any_of(list->begin(), list->end(), [](const std::wstring& e) { return !e.empty(); });
                        if (referenced) break;
                    }
                }
                registry->close_key(k.value());
                if (referenced) return kActionSuccess;
            }
        }
        for (const std::string& dir : cache_candidates(windows, identity)) {
            const std::string probe = dir + (manifest.empty() ? identity.name + ".dll" : manifest);
            if (!exists(*vfs, probe)) continue;
            remove_tree(*vfs, dir);
            std::string parent = dir;
            while (!parent.empty() && parent.back() == '\\') parent.pop_back();
            parent = parent.substr(0, parent.rfind('\\'));
            if (dir_empty(*vfs, parent)) (void)vfs->remove(parent);
            break;
        }
        return kActionSuccess;
    };
    ctx.schedule(std::move(commit));
    return true;
}

// ---- MsiPublishAssemblies / MsiUnpublishAssemblies -----------------------------------------

namespace {

std::vector<std::wstring> read_list(capsule::Registry& r, u64 hive, const std::wstring& key, const std::wstring& name) {
    auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), key, false);
    if (k.is_err()) return {};
    std::vector<std::wstring> out;
    if (auto v = r.query_value(k.value(), name); v.is_ok())
        if (const auto* list = std::get_if<std::vector<std::wstring>>(&v.value().data)) out = *list;
    r.close_key(k.value());
    return out;
}

/// Write @p list as the value (deleting it when empty) and remove the keys
/// that became empty, from @p key up to and including @p stop.
void write_list(capsule::Registry& r, u64 hive, const std::wstring& key, const std::wstring& name,
                const std::vector<std::wstring>& list, const std::wstring& stop) {
    if (!list.empty()) {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), key, true);
        if (k.is_err()) return;
        (void)r.set_value(k.value(), name, capsule::RegValue{capsule::RegType::MultiSz, list});
        r.close_key(k.value());
        return;
    }
    auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), key, false);
    if (k.is_err()) return;
    (void)r.delete_value(k.value(), name);
    r.close_key(k.value());
    // Measured: Win32Assemblies (created by the install) is gone after the
    // uninstall; Assemblies stays only while another value keeps it.
    std::wstring cur = key;
    while (cur.size() >= stop.size()) {
        auto h = r.open_key(r.view_root(hive, capsule::RegView::Native64), cur, false);
        if (h.is_err()) break;
        const auto values = r.enum_values(h.value());
        const auto keys = r.enum_keys(h.value());
        r.close_key(h.value());
        if (values.is_err() || keys.is_err() || !values.value().empty() || !keys.value().empty()) break;
        const usize slash = cur.rfind(L'\\');
        if (slash == std::wstring::npos) break;
        auto parent = r.open_key(r.view_root(hive, capsule::RegView::Native64), cur.substr(0, slash), false);
        if (parent.is_err()) break;
        (void)r.delete_key(parent.value(), cur.substr(slash + 1));
        r.close_key(parent.value());
        if (cur == stop) break;
        cur = cur.substr(0, slash);
    }
}

u32 assembly_publication(ActionContext& ctx, bool publish) {
    const char* action = publish ? "MsiPublishAssemblies" : "MsiUnpublishAssemblies";
    Session& s = ctx.session();
    std::vector<PackageAssembly> wanted;
    for (PackageAssembly& a : package_assemblies(s)) {
        // Learn: global assemblies whose feature is installed or advertised,
        // private ones whose parent component is installed.
        const bool on = publish ? (component_installing(s, a.component) ||
                                   (a.global() && feature_final_state(s, a.feature) == install_state::kAdvertised))
                                : component_removing(s, a.component);
        if (on) wanted.push_back(std::move(a));
    }
    if (wanted.empty()) return kActionSuccess;
    if (!require_script(ctx)) return kActionFailure;
    auto db = action_database(ctx);
    if (!db) {
        log_unimplemented(action, "no capsule registry to publish assemblies in");
        return kActionFailure;
    }
    const ProductInstance inst = session_instance(ctx);
    const auto keys = db->keys(inst.context, inst.sid);
    capsule::Registry* registry = ctx.services().registry;
    const std::string product = upper(s.property("ProductCode"));
    Session* session = &s;
    Services* services = &ctx.services();
    for (const PackageAssembly& a : wanted) {
        if (publish && a.win32 && a.global()) {
            log_unimplemented(action, "global Win32 assembly (WinSxS store)");
            return kActionFailure;
        }
        std::string context = "Global";
        if (!a.global()) {
            context = file_target_path(s, a.application);
            if (context.empty()) {
                log_unimplemented(action, "private assembly whose File_Application path is unresolved");
                return kActionFailure;
            }
            std::replace(context.begin(), context.end(), '\\', '|');
        }
        const std::wstring stop = keys.advertised_root + (a.win32 ? L"\\Win32Assemblies" : L"\\Assemblies");
        const std::wstring key = stop + L"\\" + widen(context);
        const std::wstring name = widen(a.identity());
        const std::wstring desc = widen(session_descriptor(s, a.feature, a.component));
        const u64 hive = keys.advertised_hive;
        const std::string feature = a.feature;
        const std::string app_context = a.global() ? std::string{} : file_target_path(s, a.application);
        const std::string shown = a.identity();
        auto before = std::make_shared<std::vector<std::wstring>>();
        ScriptOp op;
        op.action = action;
        op.run = [=]() -> u32 {
            if (Engine* e = session->engine()) e->action_start(action);
            ActionContext msg(*session, *services);
            if (msg.message(kMsgActionData, text_record({app_context, shown})) == kIdCancel) return kActionUserExit;
            *before = read_list(*registry, hive, key, name);
            std::vector<std::wstring> list;
            for (const std::wstring& e : *before) {
                if (publish) {
                    if (e != desc) list.push_back(e);
                    continue;
                }
                Descriptor d;
                const std::string e8 = narrow(e);
                const std::vector<u16> u = utf8_to_utf16(e8);
                const bool ours = e == desc ||
                                  (decompose_descriptor(std::u16string(u.begin(), u.end()), d) == 0 &&
                                   upper(d.product) == product && d.feature == feature);
                if (!ours) list.push_back(e);
            }
            // Measured (refcount-both): the newest product's descriptor first.
            if (publish) list.insert(list.begin(), desc);
            write_list(*registry, hive, key, name, list, stop);
            return kActionSuccess;
        };
        op.rollback = [=]() { write_list(*registry, hive, key, name, *before, stop); };
        ctx.schedule(std::move(op));
    }
    return kActionSuccess;
}

}  // namespace

void register_assembly_actions(ActionRegistry& r) {
    r.add("MsiPublishAssemblies", [](ActionContext& c) { return assembly_publication(c, true); });
    r.add("MsiUnpublishAssemblies", [](ActionContext& c) { return assembly_publication(c, false); });
}

}  // namespace vitreo::winapi::msi
