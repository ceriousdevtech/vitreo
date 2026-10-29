#include "winapi/system/msi/msi_install.hpp"
#include "winapi/system/msi/msi_files.hpp"

#include <algorithm>
#include <map>

namespace vitreo::winapi::msi {

namespace {

u32 to_u32(const std::string& text) {
    u32 value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') break;
        value = value * 10 + static_cast<u32>(c - '0');
    }
    return value;
}

void append_segment(std::string& path, const std::string& segment) {
    if (segment.empty()) return;
    if (!path.empty() && path.back() != '\\') path.push_back('\\');
    path += segment;
}

/// One media row, reduced to what placing a file needs.
struct MediaRange {
    u32 last_sequence = 0;
    std::string cabinet;
};

std::vector<MediaRange> read_media(Session& session) {
    std::vector<MediaRange> media;
    const Table* table = session.package()->table("Media");
    if (table == nullptr) return media;

    const auto last_col = table->column_index("LastSequence");
    const auto cab_col = table->column_index("Cabinet");
    if (!last_col.has_value()) return media;

    for (const auto& row : table->rows) {
        if (*last_col >= row.size()) continue;
        MediaRange range;
        range.last_sequence = to_u32(row[*last_col]);
        if (cab_col.has_value() && *cab_col < row.size()) range.cabinet = row[*cab_col];
        media.push_back(std::move(range));
    }

    // The table is keyed by DiskId, not sorted by sequence, and the lookup is
    // a range search — so sort rather than trusting the row order.
    std::sort(media.begin(), media.end(),
              [](const MediaRange& a, const MediaRange& b) {
                  return a.last_sequence < b.last_sequence;
              });
    return media;
}

/// The media row that owns @p sequence: the first whose LastSequence reaches
/// it. Returns nullptr when the package has no row covering the sequence,
/// which means the package is inconsistent, not that the file is uncompressed.
const MediaRange* media_for_sequence(const std::vector<MediaRange>& media, u32 sequence) {
    auto it = std::lower_bound(media.begin(), media.end(), sequence,
                               [](const MediaRange& m, u32 value) {
                                   return m.last_sequence < value;
                               });
    if (it == media.end()) return nullptr;
    return &*it;
}

/// Directory table rows, as (parent, DefaultDir), for the source-side walk.
/// The target side is already resolved by the session.
std::map<std::string, std::pair<std::string, std::string>> read_directory_rows(
    Session& session) {
    std::map<std::string, std::pair<std::string, std::string>> rows;
    const Table* table = session.package()->table("Directory");
    if (table == nullptr) return rows;

    const auto key_col = table->column_index("Directory");
    const auto parent_col = table->column_index("Directory_Parent");
    const auto default_col = table->column_index("DefaultDir");
    if (!key_col.has_value() || !parent_col.has_value() || !default_col.has_value()) {
        return rows;
    }

    for (const auto& row : table->rows) {
        if (*key_col >= row.size() || *parent_col >= row.size() ||
            *default_col >= row.size()) {
            continue;
        }
        rows.emplace(row[*key_col], std::make_pair(row[*parent_col], row[*default_col]));
    }
    return rows;
}

/// Resolve a directory key to its path in the *source* tree, rooted at
/// @p source_dir. Memoized into @p cache.
std::string resolve_source_directory(
    const std::string& key,
    const std::map<std::string, std::pair<std::string, std::string>>& rows,
    const std::string& source_dir,
    std::map<std::string, std::string>& cache,
    int depth) {
    if (key.empty()) return source_dir;
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;
    if (depth > 64) return std::string{};  // self-referential package

    auto row = rows.find(key);
    if (row == rows.end()) return source_dir;

    const std::string& parent = row->second.first;
    // A root row (no parent) is the source root itself; its DefaultDir names
    // the *target* anchor and contributes nothing on the source side.
    std::string path = parent.empty()
        ? source_dir
        : resolve_source_directory(parent, rows, source_dir, cache, depth + 1);
    if (!parent.empty()) append_segment(path, default_dir_source(row->second.second));

    cache[key] = path;
    return path;
}

} // namespace

std::string default_dir_source(const std::string& default_dir) {
    // `target:source`. Without a colon the one name serves as both; with one,
    // the source half is what the payload sits under on the media.
    std::string source = default_dir;
    const usize colon = source.find(':');
    if (colon != std::string::npos) source = source.substr(colon + 1);

    source = long_file_name(source);
    if (source == ".") return std::string{};
    return source;
}

std::string short_file_name(const std::string& file_name) {
    const usize pipe = file_name.find('|');
    if (pipe == std::string::npos) return file_name;
    return file_name.substr(0, pipe);
}

std::string long_file_name(const std::string& file_name) {
    const usize pipe = file_name.find('|');
    if (pipe == std::string::npos) return file_name;
    return file_name.substr(pipe + 1);
}

std::vector<PlannedFile> plan_files(Session& session,
                                    const std::string& source_dir,
                                    std::vector<std::string>& unresolved) {
    std::vector<PlannedFile> plan;
    if (session.package() == nullptr) return plan;

    const Table* files = session.package()->table("File");
    const Table* components = session.package()->table("Component");
    if (files == nullptr || components == nullptr) return plan;

    const auto component_key_col = components->column_index("Component");
    const auto component_dir_col = components->column_index("Directory_");
    const auto component_cond_col = components->column_index("Condition");
    if (!component_key_col.has_value() || !component_dir_col.has_value()) return plan;

    // Component -> (directory key, condition).
    std::map<std::string, std::pair<std::string, std::string>> component_rows;
    for (const auto& row : components->rows) {
        if (*component_key_col >= row.size() || *component_dir_col >= row.size()) continue;
        std::string condition;
        if (component_cond_col.has_value() && *component_cond_col < row.size()) {
            condition = row[*component_cond_col];
        }
        component_rows.emplace(row[*component_key_col],
                               std::make_pair(row[*component_dir_col], condition));
    }

    const auto file_key_col = files->column_index("File");
    const auto file_component_col = files->column_index("Component_");
    const auto file_name_col = files->column_index("FileName");
    const auto file_size_col = files->column_index("FileSize");
    const auto file_attr_col = files->column_index("Attributes");
    const auto file_seq_col = files->column_index("Sequence");
    if (!file_key_col.has_value() || !file_component_col.has_value() ||
        !file_name_col.has_value() || !file_seq_col.has_value()) {
        return plan;
    }

    const std::vector<MediaRange> media = read_media(session);
    const SourceMode mode = source_mode(session);
    const auto directory_rows = read_directory_rows(session);
    std::map<std::string, std::string> source_cache;

    for (const auto& row : files->rows) {
        if (*file_key_col >= row.size() || *file_component_col >= row.size() ||
            *file_name_col >= row.size() || *file_seq_col >= row.size()) {
            continue;
        }

        const std::string& key = row[*file_key_col];
        const std::string& component_key = row[*file_component_col];

        auto component = component_rows.find(component_key);
        if (component == component_rows.end()) {
            unresolved.push_back(key + ": no component '" + component_key + "'");
            continue;
        }

        // A false condition is the package deciding not to install this
        // component; an *unparseable* one is this engine failing to decide,
        // and installing anyway would place files the package excluded.
        const std::string& condition = component->second.second;
        if (!condition.empty()) {
            auto enabled = session.evaluate_condition(condition);
            if (!enabled.has_value()) {
                unresolved.push_back(key + ": condition not evaluatable: " + condition);
                continue;
            }
            if (!*enabled) continue;
        }

        const std::string& directory_key = component->second.first;
        std::string target_dir = session.directory_path(directory_key);
        if (target_dir.empty()) {
            unresolved.push_back(key + ": directory '" + directory_key + "' unresolved");
            continue;
        }

        PlannedFile file;
        file.key = key;
        file.sequence = to_u32(row[*file_seq_col]);
        if (file_size_col.has_value() && *file_size_col < row.size()) {
            file.size = to_u32(row[*file_size_col]);
        }

        const std::string name = long_file_name(row[*file_name_col]);
        file.target_path = target_dir;
        append_segment(file.target_path, name);

        const u32 attributes = (file_attr_col.has_value() && *file_attr_col < row.size())
            ? to_u32(row[*file_attr_col]) : 0u;

        const MediaRange* range = media_for_sequence(media, file.sequence);
        if (range == nullptr) {
            unresolved.push_back(key + ": sequence " + std::to_string(file.sequence) +
                                 " outside every media range");
            continue;
        }

        // Learn (File.Attributes, Word Count Summary): the package's Word
        // Count decides the source type; the file's Compressed /
        // Noncompressed bits override it.
        bool compressed = mode.compressed;
        if ((attributes & kFileNoncompressed) != 0) compressed = false;
        else if ((attributes & kFileCompressed) != 0) compressed = true;

        if (compressed) {
            if (range->cabinet.empty()) {
                unresolved.push_back(key + ": marked compressed but its media row "
                                           "names no cabinet");
                continue;
            }
            file.cabinet = range->cabinet;
        } else {
            // A compressed package installs uncompressed files only from the
            // source root (Learn: Word Count Summary).
            file.source_path = mode.compressed
                ? source_dir
                : resolve_source_directory(directory_key, directory_rows, source_dir, source_cache, 0);
            append_segment(file.source_path, mode.short_names ? short_file_name(row[*file_name_col]) : name);
        }

        plan.push_back(std::move(file));
    }

    return plan;
}

} // namespace vitreo::winapi::msi
