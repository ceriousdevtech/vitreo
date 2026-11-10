// SPDX-License-Identifier: LGPL-2.1-only
/// @file sxs_context.cpp
/// @brief Side-by-side store and activation-context generation (see header).

#include "winapi/common/sxs/sxs_context.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/pe/os_version.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"

#include <array>
#include <cstdio>

namespace vitreo::winapi::sxs {

namespace {

constexpr u32 kErrorFileNotFound = 2;
constexpr u32 kErrorAccessDenied = 5;
constexpr usize kMaxDependencyDepth = 64;

std::optional<std::vector<u8>> read_file(capsule::Vfs& vfs, const std::string& path) {
    auto opened = vfs.open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                           capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return std::nullopt;
    std::vector<u8> bytes;
    std::array<u8, 64 * 1024> chunk{};
    bool ok = true;
    for (;;) {
        auto n = vfs.read(opened.value(), chunk);
        if (n.is_err()) { ok = false; break; }
        if (n.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
    (void)vfs.close(opened.value());
    if (!ok) return std::nullopt;
    return bytes;
}

bool write_file(capsule::Vfs& vfs, const std::string& path, std::span<const u8> bytes) {
    auto h = vfs.open(path, capsule::AccessMode::Write, capsule::ShareMode::None,
                      capsule::CreationDisposition::CreateAlways);
    if (h.is_err()) return false;
    const bool ok = bytes.empty() || vfs.write(h.value(), bytes).is_ok();
    (void)vfs.close(h.value());
    return ok;
}

bool exists(capsule::Vfs& vfs, const std::string& path) { return vfs.stat(path).is_ok(); }

void ensure_dir(capsule::Vfs& vfs, const std::string& path) {
    std::string partial;
    usize start = 0;
    while (start <= path.size()) {
        const usize sep = path.find('\\', start);
        partial = path.substr(0, sep);
        if (partial.size() > 2 && !exists(vfs, partial)) (void)vfs.create_dir(partial);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
}

std::string join(const std::string& dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    return dir.back() == '\\' ? dir + std::string(name) : dir + "\\" + std::string(name);
}

std::string parent(const std::string& path) {
    const usize sep = path.find_last_of("\\/");
    return sep == std::string::npos ? std::string{} : path.substr(0, sep);
}

bool neutral_language(std::string_view lang) {
    return lang.empty() || lang == "*" || iequal(lang, "neutral") || iequal(lang, "none");
}

std::string store_root(const std::string& windows_dir) { return join(windows_dir, "WinSxS"); }

// 64-bit FNV-1a over the lower-cased identity: the store directory's last
// component. Vitreo's own (the Windows hash is undocumented).
std::string identity_hash(const Identity& id) {
    const std::string text = to_lower(id.type) + "|" + to_lower(id.name) + "|" +
                             (id.version ? id.version->str() : std::string{}) + "|" + to_lower(id.architecture) + "|" +
                             to_lower(id.public_key_token) + "|" +
                             (neutral_language(id.language) ? std::string("none") : to_lower(id.language));
    u64 h = 0xcbf29ce484222325ull;
    for (const char c : text) {
        h ^= static_cast<u8>(c);
        h *= 0x100000001b3ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(h));
    return buffer;
}

// WinSxS\Fusion\<arch>_<name>_<token>_<language|none>_<hash>\<major.minor>\<version>\
// (measured for an MSI-installed global Win32 assembly on Windows 11:
// .docs/contracts/msi/registration.md "Assemblies").
std::string fusion_directory(const std::string& windows_dir, const Identity& id) {
    Identity versionless = id;
    versionless.version.reset();
    const std::string language = neutral_language(id.language) ? std::string("none") : to_lower(id.language);
    char minor[16];
    std::snprintf(minor, sizeof(minor), "%u.%u", id.version->parts[0], id.version->parts[1]);
    return join(join(join(join(store_root(windows_dir), "Fusion"),
                          to_lower(id.architecture) + "_" + to_lower(id.name) + "_" +
                              (id.public_key_token.empty() ? std::string("none") : to_lower(id.public_key_token)) +
                              "_" + language + "_" + identity_hash(versionless)),
                     minor),
                id.version->str());
}

std::string key_prefix(const Identity& id) {
    return to_lower(id.architecture) + "_" + to_lower(id.name) + "_" +
           (id.public_key_token.empty() ? std::string("none") : to_lower(id.public_key_token)) + "_";
}

// The assemblies Windows itself ships in the store, as measured on Windows
// 11 (tests/oracle_data/actctx_os_assemblies.*.expected): a request binds when
// its major.minor is one the OS carries and its version is not above the
// OS's, and binds to the OS's version. Common-Controls brings its en-us
// Resources assembly (comctl32.dll.mui). Vitreo versions them with the build
// it reports (core/pe/os_version.hpp) and provides the files as its own
// built-in modules: nothing of Windows is copied.
struct OsAssembly {
    const char* name;
    u16 major, minor;
    const char* file;
    const char* resources_file;   // the language-specific companion, or nullptr
};
constexpr OsAssembly kOsAssemblies[] = {
    {"Microsoft.Windows.Common-Controls", 6, 0, "comctl32.dll", "comctl32.dll.mui"},
    {"Microsoft.Windows.Common-Controls", 5, 82, "comctl32.dll", "comctl32.dll.mui"},
    {"Microsoft.Windows.GdiPlus", 1, 0, "GdiPlus.dll", nullptr},
    {"Microsoft.Windows.GdiPlus", 1, 1, "GdiPlus.dll", nullptr},
};
constexpr const char* kOsUiLanguage = "en-us";

Version os_assembly_version(const OsAssembly& a) {
    Version v;
    v.parts = {a.major, a.minor, static_cast<u16>(pe::kOsBuildNumber), 0};
    return v;
}

// A reference matches a definition: name, type when the reference names
// one, architecture, token when the reference names one, exact version,
// language.
bool satisfies(const Identity& def, const Identity& ref, const Version& version, const std::string& arch) {
    if (!iequal(def.name, ref.name)) return false;
    if (!ref.type.empty() && !iequal(def.type, ref.type)) return false;
    if (!iequal(def.architecture, arch)) return false;
    if (!ref.public_key_token.empty() && !iequal(def.public_key_token, ref.public_key_token)) return false;
    if (!def.version || *def.version != version) return false;
    const bool ref_neutral = neutral_language(ref.language);
    if (ref_neutral != neutral_language(def.language)) return false;
    if (!ref_neutral && !iequal(def.language, ref.language)) return false;
    return true;
}

std::optional<Version> apply_redirects(const std::vector<BindingRedirect>& redirects, const Version& v) {
    for (const BindingRedirect& r : redirects)
        if (v >= r.old_low && v <= r.old_high) return r.new_version;
    return std::nullopt;
}

// A manifest from a .manifest file or from a PE's RT_MANIFEST resource 1
// (Learn "Assembly Searching Sequence": <name>.DLL holds the assembly
// manifest as a resource).
std::optional<Manifest> load_probe_candidate(capsule::Vfs& vfs, const std::string& path) {
    const auto bytes = read_file(vfs, path);
    if (!bytes) return std::nullopt;
    if (path.size() > 4 && iequal(std::string_view(path).substr(path.size() - 4), ".dll")) {
        const auto pe = pe::parse_pe(*bytes);
        if (pe.is_err()) return std::nullopt;
        const auto resource = pe::pe_manifest_resource(*bytes, pe.value(), 1);
        if (resource.empty()) return std::nullopt;
        return parse_manifest(resource);
    }
    return parse_manifest(*bytes);
}

class Generator {
public:
    Generator(capsule::Vfs& vfs, const ContextRequest& request) : m_vfs(vfs), m_request(request) {}

    ContextResult run() {
        ContextResult result;
        const auto root = parse_manifest(m_request.manifest);
        if (!root) {
            result.error = kErrorSxsManifestParseError;
            result.detail = m_request.manifest_path + ": manifest is not well-formed";
            return result;
        }
        report_unbuilt(*root, m_request.manifest_path);
        ResolvedAssembly root_assembly;
        root_assembly.identity = root->identity;
        root_assembly.manifest_path = m_request.manifest_path;
        root_assembly.directory = m_request.root_directory;
        root_assembly.files = root->files;
        m_data.assemblies.push_back(std::move(root_assembly));
        if (!resolve_dependencies(*root, 0)) {
            result.error = m_error;
            result.detail = m_detail;
            return result;
        }
        std::map<std::string, usize> owners;
        for (usize index = 0; index < m_data.assemblies.size(); ++index) {
            const ResolvedAssembly& a = m_data.assemblies[index];
            for (const std::string& file : a.files) {
                const std::string key = to_lower(file);
                auto [owner, first] = owners.try_emplace(key, index);
                if (!first && owner->second != index) {
                    result.error = kErrorSxsDuplicateDllName;
                    result.detail = "file " + file + " is listed by two assemblies";
                    return result;
                }
                if (a.builtin) continue;
                auto [it, inserted] = m_data.dll_redirection.try_emplace(key, join(a.directory, file), index);
                if (!inserted && it->second.second != index) {
                    result.error = kErrorSxsDuplicateDllName;
                    result.detail = "file " + file + " is listed by two assemblies";
                    return result;
                }
            }
        }
        result.data = std::move(m_data);
        return result;
    }

private:
    bool fail(u32 error, std::string detail) {
        m_error = error;
        m_detail = std::move(detail);
        return false;
    }

    bool resolve_dependencies(const Manifest& manifest, usize depth) {
        if (depth > kMaxDependencyDepth) return fail(kErrorSxsCantGenActCtx, "dependency chain too deep");
        for (const Dependency& dependency : manifest.dependencies) {
            const auto found = resolve(dependency);
            if (!found) {
                if (m_error != 0) return false;
                if (dependency.optional) continue;
                return fail(kErrorSxsCantGenActCtx,
                            "dependent assembly " + dependency.identity.name + "," +
                                (dependency.identity.version ? dependency.identity.version->str() : std::string("?")) +
                                " could not be found");
            }
            if (!found->second) continue;  // already in the roster
            if (!resolve_dependencies(found->first, depth + 1)) return false;
        }
        return true;
    }

    // {manifest, newly added}
    std::optional<std::pair<Manifest, bool>> resolve(const Dependency& dependency) {
        const Identity& ref = dependency.identity;
        if (!ref.version) {
            fail(kErrorSxsManifestFormatError, "dependent assembly " + ref.name + " has no version");
            return std::nullopt;
        }
        const std::string arch = ref.architecture.empty() || ref.architecture == "*"
                                     ? m_request.architecture : ref.architecture;
        Version version = *ref.version;

        // Application configuration redirects, then publisher policy.
        bool policy = true;
        if (m_request.config) {
            policy = !m_request.config->publisher_policy_disabled;
            for (const Dependency& entry : m_request.config->dependencies) {
                if (!iequal(entry.identity.name, ref.name)) continue;
                if (!entry.identity.public_key_token.empty() &&
                    !iequal(entry.identity.public_key_token, ref.public_key_token)) continue;
                if (entry.publisher_policy_disabled) policy = false;
                if (auto v = apply_redirects(entry.redirects, version)) version = *v;
            }
        }
        Identity resolved_ref = ref;
        resolved_ref.architecture = arch;
        resolved_ref.version = version;
        if (policy && !ref.public_key_token.empty()) {
            if (auto v = store_publisher_policy(m_vfs, m_request.windows_dir, resolved_ref)) version = *v;
        }
        resolved_ref.version = version;

        for (const ResolvedAssembly& a : m_data.assemblies) {
            if (a.identity && satisfies(*a.identity, ref, version, arch)) {
                Manifest m;
                return std::make_pair(m, false);
            }
        }

        // Operating-system assemblies (Windows' own publicKeyToken).
        if (iequal(ref.public_key_token, kWindowsPublicKeyToken)) {
            for (const OsAssembly& os : kOsAssemblies) {
                if (!iequal(ref.name, os.name) || version.parts[0] != os.major || version.parts[1] != os.minor) continue;
                const Version provided = os_assembly_version(os);
                if (version > provided) break;  // newer than this OS ships
                for (const ResolvedAssembly& a : m_data.assemblies)
                    if (a.builtin && a.identity && iequal(a.identity->name, os.name) &&
                        iequal(a.identity->architecture, arch) && a.identity->version == provided) {
                        return std::make_pair(Manifest{}, false);
                    }
                Manifest m;
                m.identity = Identity{"win32", os.name, provided, arch, std::string(kWindowsPublicKeyToken), {}};
                m.files = {os.file};
                add(m, {}, {}, true);
                m_data.assemblies.back().builtin = true;
                if (os.resources_file != nullptr) {
                    Manifest r;
                    r.identity = Identity{"win32", std::string(os.name) + ".Resources", provided, arch,
                                          std::string(kWindowsPublicKeyToken), kOsUiLanguage};
                    r.files = {os.resources_file};
                    add(r, {}, {}, true);
                    m_data.assemblies.back().builtin = true;
                }
                return std::make_pair(m, true);
            }
        }

        // Shared assemblies: the store, by exact identity.
        if (!ref.public_key_token.empty()) {
            Identity exact = resolved_ref;
            if (exact.type.empty()) exact.type = "win32";
            if (auto entry = store_find(m_vfs, m_request.windows_dir, exact)) {
                add(entry->manifest, entry->manifest_path, entry->directory, true);
                return std::make_pair(entry->manifest, true);
            }
        }

        // Private probing in the application directory: culture first.
        const std::string& base = m_request.application_dir;
        std::vector<std::string> roots;
        if (!neutral_language(ref.language)) roots.push_back(join(base, ref.language));
        roots.push_back(base);
        for (const std::string& root : roots) {
            const std::array<std::string, 4> candidates{
                join(root, ref.name + ".dll"),
                join(root, ref.name + ".manifest"),
                join(join(root, ref.name), ref.name + ".dll"),
                join(join(root, ref.name), ref.name + ".manifest"),
            };
            for (const std::string& candidate : candidates) {
                if (!exists(m_vfs, candidate)) continue;
                auto manifest = load_probe_candidate(m_vfs, candidate);
                if (!manifest || !manifest->identity) continue;
                if (!satisfies(*manifest->identity, ref, version, arch)) continue;
                add(*manifest, candidate, parent(candidate), false);
                return std::make_pair(std::move(*manifest), true);
            }
        }
        return std::nullopt;
    }

    static void report_unbuilt(const Manifest& manifest, const std::string& path) {
        for (const std::string& kind : manifest.unbuilt_sections)
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED sxs: %s declares <%s>; the activation context carries "
                                 "only DLL redirection, so lookups for it report not found\n",
                         path.c_str(), kind.c_str());
    }

    void add(const Manifest& manifest, const std::string& path, const std::string& dir, bool shared) {
        report_unbuilt(manifest, path);
        ResolvedAssembly a;
        a.identity = manifest.identity;
        a.manifest_path = path;
        a.directory = dir;
        a.files = manifest.files;
        a.shared = shared;
        m_data.assemblies.push_back(std::move(a));
    }

    capsule::Vfs& m_vfs;
    const ContextRequest& m_request;
    ContextData m_data;
    u32 m_error = 0;
    std::string m_detail;
};

}  // namespace

std::string store_key(const Identity& id) {
    const std::string language = neutral_language(id.language) ? std::string("none") : to_lower(id.language);
    return key_prefix(id) + (id.version ? id.version->str() : std::string("0.0.0.0")) + "_" + language + "_" +
           identity_hash(id);
}

std::optional<StoreEntry> store_find(capsule::Vfs& vfs, const std::string& windows_dir, const Identity& identity) {
    if (!identity.version || identity.architecture.empty() || identity.type.empty()) return std::nullopt;
    const std::string directory = fusion_directory(windows_dir, identity);
    const std::string manifest_path = join(directory, store_key(identity) + ".manifest");
    const auto bytes = read_file(vfs, manifest_path);
    if (!bytes) return std::nullopt;
    auto manifest = parse_manifest(*bytes);
    if (!manifest || !manifest->identity) return std::nullopt;
    StoreEntry entry;
    entry.identity = *manifest->identity;
    entry.manifest_path = manifest_path;
    entry.directory = directory;
    entry.manifest = std::move(*manifest);
    return entry;
}

std::optional<Version> store_publisher_policy(capsule::Vfs& vfs, const std::string& windows_dir,
                                              const Identity& reference) {
    if (!reference.version || reference.public_key_token.empty()) return std::nullopt;
    Identity policy;
    policy.type = "win32-policy";
    char name[64];
    std::snprintf(name, sizeof(name), "policy.%u.%u.", reference.version->parts[0], reference.version->parts[1]);
    policy.name = name + reference.name;
    policy.architecture = reference.architecture;
    policy.public_key_token = reference.public_key_token;
    const std::string prefix = key_prefix(policy);
    const std::string fusion = join(store_root(windows_dir), "Fusion");
    auto listing = vfs.list_dir(fusion);
    if (listing.is_err()) return std::nullopt;

    // The highest installed policy version governs (Learn: a newer publisher
    // configuration file supersedes the older ones). Every installed version
    // sits in <family>\<major.minor>\<version>\.
    std::optional<std::pair<Version, Manifest>> best;
    const auto names = [&](const std::string& dir) {
        std::vector<std::string> out;
        if (auto l = vfs.list_dir(dir); l.is_ok())
            for (const auto& e : l.value()) if (e.name != "." && e.name != "..") out.push_back(e.name);
        return out;
    };
    for (const auto& family : listing.value()) {
        if (to_lower(family.name).rfind(prefix, 0) != 0) continue;
        const std::string family_dir = join(fusion, family.name);
        for (const std::string& minor : names(family_dir)) {
            for (const std::string& version : names(join(family_dir, minor))) {
                const std::string version_dir = join(join(family_dir, minor), version);
                for (const std::string& file : names(version_dir)) {
                    const std::string lower = to_lower(file);
                    if (lower.size() < 9 || lower.compare(lower.size() - 9, 9, ".manifest") != 0) continue;
                    const auto bytes = read_file(vfs, join(version_dir, file));
                    if (!bytes) continue;
                    auto manifest = parse_manifest(*bytes);
                    if (!manifest || !manifest->identity || !manifest->identity->version) continue;
                    if (!iequal(manifest->identity->type, "win32-policy") ||
                        !iequal(manifest->identity->name, policy.name)) {
                        continue;
                    }
                    if (!best || *manifest->identity->version > best->first)
                        best = std::make_pair(*manifest->identity->version, std::move(*manifest));
                }
            }
        }
    }
    if (!best) return std::nullopt;
    for (const Dependency& d : best->second.dependencies) {
        if (!iequal(d.identity.name, reference.name)) continue;
        if (!d.identity.public_key_token.empty() && !iequal(d.identity.public_key_token, reference.public_key_token))
            continue;
        if (!d.identity.architecture.empty() && !iequal(d.identity.architecture, reference.architecture)) continue;
        if (auto v = apply_redirects(d.redirects, *reference.version)) return v;
    }
    return std::nullopt;
}

u32 store_install(capsule::Vfs& vfs, const std::string& windows_dir, std::span<const u8> manifest_bytes,
                  const std::map<std::string, std::string>& files, const std::string& catalog) {
    const auto manifest = parse_manifest(manifest_bytes);
    if (!manifest) return kErrorSxsManifestParseError;
    if (!manifest->identity || !manifest->identity->version || manifest->identity->architecture.empty() ||
        manifest->identity->type.empty() || manifest->identity->public_key_token.empty()) {
        return kErrorSxsManifestFormatError;
    }
    const std::string directory = fusion_directory(windows_dir, *manifest->identity);
    ensure_dir(vfs, directory);
    for (const std::string& file : manifest->files) {
        std::optional<std::string> source;
        for (const auto& [name, path] : files)
            if (iequal(name, file)) source = path;
        if (!source) return kErrorFileNotFound;
        const auto bytes = read_file(vfs, *source);
        if (!bytes) return kErrorFileNotFound;
        if (!write_file(vfs, join(directory, file), *bytes)) return kErrorAccessDenied;
    }
    if (!catalog.empty()) {
        const auto bytes = read_file(vfs, catalog);
        if (!bytes) return kErrorFileNotFound;
        if (!write_file(vfs, join(directory, store_key(*manifest->identity) + ".cat"), *bytes)) return kErrorAccessDenied;
    }
    if (!write_file(vfs, join(directory, store_key(*manifest->identity) + ".manifest"), manifest_bytes)) {
        return kErrorAccessDenied;
    }
    return 0;
}

bool store_remove(capsule::Vfs& vfs, const std::string& windows_dir, const Identity& identity) {
    const auto entry = store_find(vfs, windows_dir, identity);
    if (!entry) return false;
    for (const std::string& file : entry->manifest.files) (void)vfs.remove(join(entry->directory, file));
    (void)vfs.remove(join(entry->directory, store_key(entry->identity) + ".cat"));
    (void)vfs.remove(entry->manifest_path);
    // <family>\<major.minor>\<version>: drop each level once it is empty.
    std::string dir = entry->directory;
    for (int level = 0; level < 3; ++level) {
        auto listing = vfs.list_dir(dir);
        bool empty = listing.is_ok();
        if (empty) for (const auto& e : listing.value()) if (e.name != "." && e.name != "..") empty = false;
        if (!empty || vfs.remove(dir).is_err()) break;
        dir = parent(dir);
    }
    return true;
}

std::optional<std::string> ContextData::redirect_dll(std::string_view base_name) const {
    auto it = dll_redirection.find(to_lower(base_name));
    if (it == dll_redirection.end()) return std::nullopt;
    return it->second.first;
}

ContextResult build_context(capsule::Vfs& vfs, const ContextRequest& request) {
    return Generator(vfs, request).run();
}

}  // namespace vitreo::winapi::sxs
