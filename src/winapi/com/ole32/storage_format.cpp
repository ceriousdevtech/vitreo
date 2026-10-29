// SPDX-License-Identifier: LGPL-2.1-only
/// @file storage_format.cpp
/// @brief Compound File Binary Format reader/writer.
///
/// Layout of a version-3 compound file, which is what every consumer of these
/// files on 32-bit Windows expects:
///
///   offset 0      512-byte header (signature, sector shifts, FAT bookkeeping,
///                 and the first 109 FAT sector numbers)
///   offset 512    sector 0; sector N lives at 512 + N * 512
///
/// Three chained structures live in those sectors:
///
///   * the **FAT** — one u32 per sector, giving the next sector in that
///     sector's chain (or ENDOFCHAIN)
///   * the **directory** — a stream of 128-byte entries forming a tree; each
///     entry names one storage or stream and points at its first sector
///   * the **mini stream** — streams smaller than 4096 bytes are packed into
///     64-byte mini sectors inside one ordinary stream owned by the root
///     entry, chained through a parallel **mini FAT**
///
/// The size rule is not advisory: a reader picks the mini stream or the normal
/// FAT purely from the recorded stream size, so a small stream written into
/// normal sectors would be read back from the wrong place.

#include "storage.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

namespace {

constexpr u32 SECTOR_SIZE       = 512;
constexpr u32 MINI_SECTOR_SIZE  = 64;
constexpr u32 MINI_CUTOFF       = 4096;
constexpr u32 DIR_ENTRY_SIZE    = 128;
constexpr u32 ENTRIES_PER_SECTOR = SECTOR_SIZE / DIR_ENTRY_SIZE; // 4
constexpr u32 FAT_ENTRIES_PER_SECTOR = SECTOR_SIZE / 4;          // 128
constexpr u32 DIFAT_IN_HEADER   = 109;

constexpr u32 FREESECT   = 0xFFFFFFFFu;
constexpr u32 ENDOFCHAIN = 0xFFFFFFFEu;
constexpr u32 FATSECT    = 0xFFFFFFFDu;
constexpr u32 DIFSECT    = 0xFFFFFFFCu;
constexpr u32 NOSTREAM   = 0xFFFFFFFFu;

constexpr u8 TYPE_UNALLOCATED = 0;
constexpr u8 TYPE_STORAGE     = 1;
constexpr u8 TYPE_STREAM      = 2;
constexpr u8 TYPE_ROOT        = 5;
constexpr u8 COLOR_BLACK      = 1;

constexpr std::array<u8, 8> SIGNATURE{
    0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

void put_u16(std::vector<u8>& out, usize offset, u16 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}
void put_u32(std::vector<u8>& out, usize offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}
void put_u64(std::vector<u8>& out, usize offset, u64 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}
[[nodiscard]] u16 get_u16(std::span<const u8> in, usize offset) {
    u16 v = 0; std::memcpy(&v, in.data() + offset, sizeof(v)); return v;
}
[[nodiscard]] u32 get_u32(std::span<const u8> in, usize offset) {
    u32 v = 0; std::memcpy(&v, in.data() + offset, sizeof(v)); return v;
}
[[nodiscard]] u64 get_u64(std::span<const u8> in, usize offset) {
    u64 v = 0; std::memcpy(&v, in.data() + offset, sizeof(v)); return v;
}

[[nodiscard]] char upper_ascii(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// Append one Unicode code point to a UTF-8 string.
void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

/// Entry names are UTF-16LE on disk and UTF-8 in memory. Narrowing a code
/// unit to its low byte instead of converting destroys any name outside
/// Latin-1 — installer packages name their table streams in a private
/// Unicode range, so truncation silently makes every table unreachable.
[[nodiscard]] std::vector<u16> utf16_from_utf8(const std::string& s) {
    std::vector<u16> units;
    units.reserve(s.size());

    usize i = 0;
    while (i < s.size()) {
        const u8 b0 = static_cast<u8>(s[i]);
        u32 cp;
        if ((b0 & 0x80) == 0) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((b0 & 0x1Fu) << 6) | (static_cast<u8>(s[i + 1]) & 0x3Fu);
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((b0 & 0x0Fu) << 12) |
                 ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<u8>(s[i + 2]) & 0x3Fu);
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((b0 & 0x07u) << 18) |
                 ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<u8>(s[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<u8>(s[i + 3]) & 0x3Fu);
            i += 4;
        } else {
            cp = b0;
            i += 1;
        }

        if (cp >= 0x10000u) {
            const u32 v = cp - 0x10000u;
            units.push_back(static_cast<u16>(0xD800u + (v >> 10)));
            units.push_back(static_cast<u16>(0xDC00u + (v & 0x3FFu)));
        } else {
            units.push_back(static_cast<u16>(cp));
        }
    }

    return units;
}

/// Directory entries are ordered by name length first, then by the
/// case-folded name. Both are measured in UTF-16 code units, which is what
/// the on-disk name is, not in UTF-8 bytes.
[[nodiscard]] bool directory_order_less(const std::string& a,
                                        const std::string& b) {
    const std::vector<u16> ua = utf16_from_utf8(a);
    const std::vector<u16> ub = utf16_from_utf8(b);
    if (ua.size() != ub.size()) return ua.size() < ub.size();
    for (usize i = 0; i < ua.size(); ++i) {
        const u16 ca = (ua[i] >= 'a' && ua[i] <= 'z')
            ? static_cast<u16>(ua[i] - 'a' + 'A') : ua[i];
        const u16 cb = (ub[i] >= 'a' && ub[i] <= 'z')
            ? static_cast<u16>(ub[i] - 'a' + 'A') : ub[i];
        if (ca != cb) return ca < cb;
    }
    return false;
}

/// One directory entry being assembled.
struct DirEntry {
    std::string name;
    u8  type{TYPE_UNALLOCATED};
    u32 left{NOSTREAM};
    u32 right{NOSTREAM};
    u32 child{NOSTREAM};
    std::array<u8, 16> clsid{};
    u32 state_bits{0};
    u64 creation_time{0};
    u64 modified_time{0};
    u32 start_sector{ENDOFCHAIN};
    u64 size{0};
};

/// Build a balanced binary search tree over `ids` (already in directory
/// order) and return the id of its root, wiring left/right as it goes.
u32 build_sibling_tree(std::vector<DirEntry>& entries,
                       const std::vector<u32>& ids, usize lo, usize hi) {
    if (lo >= hi) return NOSTREAM;
    const usize mid = lo + (hi - lo) / 2;
    const u32 id = ids[mid];
    entries[id].left  = build_sibling_tree(entries, ids, lo, mid);
    entries[id].right = build_sibling_tree(entries, ids, mid + 1, hi);
    return id;
}

/// Depth-first walk assigning a directory id to every node and recording the
/// stream bytes that will need sectors.
u32 flatten(const StorageNode& node, std::vector<DirEntry>& entries,
            std::vector<const StorageNode*>& nodes, bool is_root) {
    const u32 id = static_cast<u32>(entries.size());
    entries.emplace_back();
    nodes.push_back(&node);

    DirEntry entry;
    entry.name  = is_root ? std::string("Root Entry") : node.name;
    entry.type  = is_root ? TYPE_ROOT
                          : (node.is_storage ? TYPE_STORAGE : TYPE_STREAM);
    entry.clsid = node.clsid;
    entry.state_bits = node.state_bits;
    // [MS-CFB] 2.6.3: a stream object's times are zero; the root's
    // creation time is zero.
    if (node.is_storage || is_root) {
        entry.creation_time = is_root ? 0 : node.creation_time;
        entry.modified_time = node.modified_time;
    }
    if (!node.is_storage && !is_root) entry.size = node.data.size();
    entries[id] = std::move(entry);

    if (node.is_storage || is_root) {
        std::vector<u32> child_ids;
        child_ids.reserve(node.children.size());
        // `children` is already keyed case-insensitively, but the directory
        // order (length first) is different, so sort explicitly.
        std::vector<const StorageNode*> ordered;
        ordered.reserve(node.children.size());
        for (const auto& [name, child] : node.children) ordered.push_back(child.get());
        std::sort(ordered.begin(), ordered.end(),
                  [](const StorageNode* a, const StorageNode* b) {
                      return directory_order_less(a->name, b->name);
                  });
        for (const StorageNode* child : ordered) {
            child_ids.push_back(flatten(*child, entries, nodes, false));
        }
        entries[id].child =
            build_sibling_tree(entries, child_ids, 0, child_ids.size());
    }
    return id;
}

/// Append `data` to `sectors`, padded to a whole number of sectors, and chain
/// the sectors in `fat`. Returns the first sector, or ENDOFCHAIN if empty.
u32 write_chain(std::vector<u8>& sectors, std::vector<u32>& fat,
                std::span<const u8> data, u32 unit) {
    if (data.empty()) return ENDOFCHAIN;
    const u32 count = static_cast<u32>((data.size() + unit - 1) / unit);
    const u32 first = static_cast<u32>(sectors.size() / unit);
    sectors.insert(sectors.end(), data.begin(), data.end());
    sectors.resize(static_cast<usize>(first + count) * unit, 0);
    for (u32 i = 0; i < count; ++i) {
        fat.push_back(i + 1 == count ? ENDOFCHAIN : first + i + 1);
    }
    return first;
}

} // namespace

bool StorageNode::NameLess::operator()(const std::string& a,
                                       const std::string& b) const noexcept {
    for (usize i = 0; i < a.size() && i < b.size(); ++i) {
        const char ca = upper_ascii(a[i]);
        const char cb = upper_ascii(b[i]);
        if (ca != cb) return static_cast<unsigned char>(ca) <
                             static_cast<unsigned char>(cb);
    }
    return a.size() < b.size();
}

std::vector<u8> serialize_compound_file(const StorageNode& root) {
    std::vector<DirEntry> entries;
    std::vector<const StorageNode*> nodes;
    (void)flatten(root, entries, nodes, /*is_root=*/true);

    // --- Pack the small streams into the mini stream ---------------------
    std::vector<u8>  mini_stream;
    std::vector<u32> mini_fat;
    for (usize i = 0; i < entries.size(); ++i) {
        if (entries[i].type != TYPE_STREAM) continue;
        const auto& data = nodes[i]->data;
        if (data.empty() || data.size() >= MINI_CUTOFF) continue;
        entries[i].start_sector =
            write_chain(mini_stream, mini_fat,
                        std::span<const u8>(data.data(), data.size()),
                        MINI_SECTOR_SIZE);
    }

    // --- Lay out the regular sector area ---------------------------------
    // Order: large stream data, then the mini stream, then the mini FAT, then
    // the directory. The FAT itself is interleaved at the end once its size
    // is known.
    std::vector<u8>  sectors;
    std::vector<u32> fat;

    for (usize i = 0; i < entries.size(); ++i) {
        if (entries[i].type != TYPE_STREAM) continue;
        const auto& data = nodes[i]->data;
        if (data.size() < MINI_CUTOFF) continue;
        entries[i].start_sector =
            write_chain(sectors, fat,
                        std::span<const u8>(data.data(), data.size()),
                        SECTOR_SIZE);
    }

    // The root entry owns the mini stream.
    entries[0].start_sector =
        write_chain(sectors, fat,
                    std::span<const u8>(mini_stream.data(), mini_stream.size()),
                    SECTOR_SIZE);
    entries[0].size = mini_stream.size();

    std::vector<u8> mini_fat_bytes(mini_fat.size() * 4);
    for (usize i = 0; i < mini_fat.size(); ++i) {
        std::memcpy(mini_fat_bytes.data() + i * 4, &mini_fat[i], 4);
    }
    const u32 mini_fat_start =
        write_chain(sectors, fat,
                    std::span<const u8>(mini_fat_bytes.data(),
                                        mini_fat_bytes.size()),
                    SECTOR_SIZE);
    const u32 mini_fat_count = static_cast<u32>(
        (mini_fat_bytes.size() + SECTOR_SIZE - 1) / SECTOR_SIZE);

    // Directory entries, padded to a whole sector.
    std::vector<u8> dir(((entries.size() + ENTRIES_PER_SECTOR - 1) /
                         ENTRIES_PER_SECTOR) * ENTRIES_PER_SECTOR *
                        DIR_ENTRY_SIZE, 0);
    for (usize i = 0; i < dir.size() / DIR_ENTRY_SIZE; ++i) {
        const usize base = i * DIR_ENTRY_SIZE;
        if (i >= entries.size()) {
            put_u32(dir, base + 0x44, NOSTREAM);
            put_u32(dir, base + 0x48, NOSTREAM);
            put_u32(dir, base + 0x4C, NOSTREAM);
            continue;
        }
        const DirEntry& e = entries[i];
        // Names are UTF-16LE and capped at 31 code units plus the terminator.
        const std::vector<u16> units = utf16_from_utf8(e.name);
        const usize chars = std::min<usize>(units.size(), 31);
        for (usize c = 0; c < chars; ++c) {
            put_u16(dir, base + c * 2, units[c]);
        }
        put_u16(dir, base + chars * 2, 0);
        put_u16(dir, base + 0x40, static_cast<u16>((chars + 1) * 2));
        dir[base + 0x42] = e.type;
        dir[base + 0x43] = COLOR_BLACK;
        put_u32(dir, base + 0x44, e.left);
        put_u32(dir, base + 0x48, e.right);
        put_u32(dir, base + 0x4C, e.child);
        std::memcpy(dir.data() + base + 0x50, e.clsid.data(), e.clsid.size());
        put_u32(dir, base + 0x60, e.state_bits);
        put_u64(dir, base + 0x64, e.creation_time);
        put_u64(dir, base + 0x6C, e.modified_time);
        put_u32(dir, base + 0x74, e.start_sector);
        put_u64(dir, base + 0x78, e.size);
    }
    const u32 dir_start =
        write_chain(sectors, fat, std::span<const u8>(dir.data(), dir.size()),
                    SECTOR_SIZE);

    // --- Reserve the FAT's own sectors -----------------------------------
    // Each FAT sector describes 128 sectors including the FAT and DIFAT
    // sectors themselves, so solve for counts that cover the total. The
    // header lists the first 109 FAT sectors; past that ([MS-CFB] 2.5) a
    // chain of DIFAT sectors lists 127 more each, the last slot of each
    // naming the next DIFAT sector.
    constexpr u32 DIFAT_PER_SECTOR = FAT_ENTRIES_PER_SECTOR - 1;
    u32 fat_sector_count = 1;
    u32 difat_sector_count = 0;
    for (;;) {
        difat_sector_count = fat_sector_count > DIFAT_IN_HEADER
            ? (fat_sector_count - DIFAT_IN_HEADER + DIFAT_PER_SECTOR - 1) / DIFAT_PER_SECTOR
            : 0;
        const u32 total = static_cast<u32>(fat.size()) + fat_sector_count + difat_sector_count;
        const u32 needed =
            (total + FAT_ENTRIES_PER_SECTOR - 1) / FAT_ENTRIES_PER_SECTOR;
        if (needed <= fat_sector_count) break;
        fat_sector_count = needed;
    }
    const u32 fat_start = static_cast<u32>(fat.size());
    for (u32 i = 0; i < fat_sector_count; ++i) fat.push_back(FATSECT);
    const u32 difat_start = static_cast<u32>(fat.size());
    for (u32 i = 0; i < difat_sector_count; ++i) fat.push_back(DIFSECT);

    std::vector<u8> fat_bytes(
        static_cast<usize>(fat_sector_count) * SECTOR_SIZE, 0xFF);
    for (usize i = 0; i < fat.size(); ++i) {
        std::memcpy(fat_bytes.data() + i * 4, &fat[i], 4);
    }
    std::vector<u8> difat_bytes(
        static_cast<usize>(difat_sector_count) * SECTOR_SIZE, 0xFF);
    for (u32 d = 0; d < difat_sector_count; ++d) {
        for (u32 j = 0; j < DIFAT_PER_SECTOR; ++j) {
            const u32 index = DIFAT_IN_HEADER + d * DIFAT_PER_SECTOR + j;
            const u32 value = index < fat_sector_count ? fat_start + index : FREESECT;
            std::memcpy(difat_bytes.data() + (d * FAT_ENTRIES_PER_SECTOR + j) * 4, &value, 4);
        }
        const u32 next = d + 1 < difat_sector_count ? difat_start + d + 1 : ENDOFCHAIN;
        std::memcpy(difat_bytes.data() + (d * FAT_ENTRIES_PER_SECTOR + DIFAT_PER_SECTOR) * 4, &next, 4);
    }
    sectors.resize(static_cast<usize>(fat_start) * SECTOR_SIZE);
    sectors.insert(sectors.end(), fat_bytes.begin(), fat_bytes.end());
    sectors.insert(sectors.end(), difat_bytes.begin(), difat_bytes.end());

    // --- Header ----------------------------------------------------------
    std::vector<u8> out(SECTOR_SIZE, 0);
    std::memcpy(out.data(), SIGNATURE.data(), SIGNATURE.size());
    put_u16(out, 0x18, 0x003E);   // minor version
    put_u16(out, 0x1A, 0x0003);   // major version 3
    put_u16(out, 0x1C, 0xFFFE);   // little-endian
    put_u16(out, 0x1E, 9);        // 512-byte sectors
    put_u16(out, 0x20, 6);        // 64-byte mini sectors
    put_u32(out, 0x2C, fat_sector_count);
    put_u32(out, 0x30, dir_start);
    put_u32(out, 0x38, MINI_CUTOFF);
    put_u32(out, 0x3C, mini_fat_start);
    put_u32(out, 0x40, mini_fat_count);
    put_u32(out, 0x44, difat_sector_count ? difat_start : ENDOFCHAIN);
    put_u32(out, 0x48, difat_sector_count);
    for (u32 i = 0; i < DIFAT_IN_HEADER; ++i) {
        put_u32(out, 0x4C + i * 4,
                i < fat_sector_count ? fat_start + i : FREESECT);
    }

    out.insert(out.end(), sectors.begin(), sectors.end());
    return out;
}

namespace {

/// Follow a sector chain and gather `size` bytes (or the whole chain when
/// `size` is 0). Returns false on a malformed or looping chain.
[[nodiscard]] bool read_chain(std::span<const u8> image,
                              const std::vector<u32>& fat, u32 first,
                              u64 size, u32 unit, u64 data_origin,
                              std::vector<u8>& out) {
    out.clear();
    u32 sector = first;
    u64 guard = 0;
    const u64 max_steps = fat.size() + 1;
    while (sector != ENDOFCHAIN && sector != FREESECT) {
        if (sector >= fat.size() || ++guard > max_steps) return false;
        const u64 offset = data_origin + static_cast<u64>(sector) * unit;
        if (offset + unit > image.size()) return false;
        out.insert(out.end(), image.begin() + static_cast<std::ptrdiff_t>(offset),
                   image.begin() + static_cast<std::ptrdiff_t>(offset + unit));
        sector = fat[sector];
    }
    if (size != 0 && out.size() > size) out.resize(size);
    return true;
}

} // namespace

std::shared_ptr<StorageNode> parse_compound_file(std::span<const u8> image) {
    if (image.size() < SECTOR_SIZE) return nullptr;
    if (std::memcmp(image.data(), SIGNATURE.data(), SIGNATURE.size()) != 0) {
        return nullptr;
    }
    const u16 sector_shift = get_u16(image, 0x1E);
    const u16 mini_shift   = get_u16(image, 0x20);
    // [MS-CFB] defines two sector sizes: version 3 uses 512-byte sectors
    // (shift 9) and version 4 uses 4096 (shift 12). Rejecting v4 turns away
    // ordinary files — every MSI produced by recent WiX toolsets is v4 — so
    // the reader follows the header instead of assuming v3. The header itself
    // is always 512 bytes; in v4 the remainder of sector 0 is padding, which
    // is why the data origin is one *sector*, not one header, from the start.
    if ((sector_shift != 9 && sector_shift != 12) || mini_shift != 6) {
        return nullptr;
    }
    const u32 sector_size = 1u << sector_shift;
    const u32 fat_entries_per_sector = sector_size / 4;
    const u64 data_origin = sector_size;
    if (image.size() < sector_size) return nullptr;
    const u32 fat_sector_count = get_u32(image, 0x2C);
    const u32 dir_start        = get_u32(image, 0x30);
    const u32 mini_cutoff      = get_u32(image, 0x38);
    const u32 mini_fat_start   = get_u32(image, 0x3C);

    // Assemble the list of FAT sectors. The header carries the first 109 of
    // them inline; a file needing more (past roughly 7 MB) continues the list
    // in a chain of DIFAT sectors, each holding 127 further FAT sector
    // numbers followed by the number of the next DIFAT sector.
    std::vector<u32> fat_sectors;
    for (u32 i = 0; i < DIFAT_IN_HEADER; ++i) {
        const u32 sector = get_u32(image, 0x4C + i * 4);
        if (sector == FREESECT || sector == ENDOFCHAIN) continue;
        fat_sectors.push_back(sector);
    }

    u32 difat_sector = get_u32(image, 0x44);
    const u32 difat_count = get_u32(image, 0x48);
    // Bounded by the recorded DIFAT count so a corrupt self-referential chain
    // terminates instead of spinning.
    for (u32 n = 0; n < difat_count; ++n) {
        if (difat_sector == ENDOFCHAIN || difat_sector == FREESECT) break;
        const u64 offset =
            data_origin + static_cast<u64>(difat_sector) * sector_size;
        if (offset + sector_size > image.size()) return nullptr;
        // The final slot is the chain pointer, not a FAT sector number.
        for (u32 j = 0; j + 1 < fat_entries_per_sector; ++j) {
            const u32 sector = get_u32(image, offset + j * 4);
            if (sector == FREESECT || sector == ENDOFCHAIN) continue;
            fat_sectors.push_back(sector);
        }
        difat_sector =
            get_u32(image, offset + (fat_entries_per_sector - 1) * 4);
    }

    // The header's count is authoritative; the inline array is padded with
    // free entries beyond it.
    if (fat_sectors.size() > fat_sector_count) fat_sectors.resize(fat_sector_count);

    std::vector<u32> fat;
    fat.reserve(fat_sectors.size() * fat_entries_per_sector);
    for (const u32 sector : fat_sectors) {
        const u64 offset =
            data_origin + static_cast<u64>(sector) * sector_size;
        if (offset + sector_size > image.size()) return nullptr;
        for (u32 j = 0; j < fat_entries_per_sector; ++j) {
            fat.push_back(get_u32(image, offset + j * 4));
        }
    }
    if (fat.empty()) return nullptr;

    std::vector<u8> dir;
    if (!read_chain(image, fat, dir_start, 0, sector_size, data_origin, dir)) {
        return nullptr;
    }
    const usize entry_count = dir.size() / DIR_ENTRY_SIZE;
    if (entry_count == 0) return nullptr;

    const auto entry_at = [&](usize i, usize field) {
        return std::span<const u8>(dir).subspan(i * DIR_ENTRY_SIZE + field);
    };
    const auto entry_u32 = [&](usize i, usize field) {
        return get_u32(std::span<const u8>(dir), i * DIR_ENTRY_SIZE + field);
    };

    // The root entry owns the mini stream.
    std::vector<u8> mini_stream;
    (void)read_chain(image, fat, entry_u32(0, 0x74),
                     get_u64(std::span<const u8>(dir), 0x78), sector_size,
                     data_origin, mini_stream);
    std::vector<u8> mini_fat_bytes;
    std::vector<u32> mini_fat;
    if (read_chain(image, fat, mini_fat_start, 0, sector_size, data_origin,
                   mini_fat_bytes)) {
        mini_fat.resize(mini_fat_bytes.size() / 4);
        for (usize i = 0; i < mini_fat.size(); ++i) {
            std::memcpy(&mini_fat[i], mini_fat_bytes.data() + i * 4, 4);
        }
    }

    // Rebuild the tree. `visited` stops a corrupt file with a cyclic sibling
    // or child link from recursing forever.
    std::vector<bool> visited(entry_count, false);
    std::function<std::shared_ptr<StorageNode>(u32)> build_node;
    std::function<void(u32, StorageNode&)> add_siblings;

    build_node = [&](u32 id) -> std::shared_ptr<StorageNode> {
        if (id >= entry_count || visited[id]) return nullptr;
        visited[id] = true;

        const u8 type = dir[id * DIR_ENTRY_SIZE + 0x42];
        if (type == TYPE_UNALLOCATED) return nullptr;

        auto node = std::make_shared<StorageNode>();
        const u16 name_bytes = get_u16(std::span<const u8>(dir),
                                       id * DIR_ENTRY_SIZE + 0x40);
        const usize chars = name_bytes >= 2 ? (name_bytes / 2) - 1 : 0;
        std::vector<u16> name_units;
        name_units.reserve(chars);
        for (usize c = 0; c < chars && c < 31; ++c) {
            name_units.push_back(get_u16(std::span<const u8>(dir),
                                         id * DIR_ENTRY_SIZE + c * 2));
        }
        // Convert rather than narrow: a name outside Latin-1 must survive.
        node->name_utf16.assign(name_units.begin(), name_units.end());
        for (usize c = 0; c < name_units.size(); ++c) {
            u32 cp = name_units[c];
            if (cp >= 0xD800 && cp < 0xDC00 && c + 1 < name_units.size() &&
                name_units[c + 1] >= 0xDC00 && name_units[c + 1] < 0xE000) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) +
                     (name_units[c + 1] - 0xDC00u);
                ++c;
            }
            append_utf8(node->name, cp);
        }
        node->is_storage = (type == TYPE_STORAGE || type == TYPE_ROOT);
        std::memcpy(node->clsid.data(), entry_at(id, 0x50).data(),
                    node->clsid.size());
        node->state_bits = entry_u32(id, 0x60);
        node->creation_time = get_u64(std::span<const u8>(dir), id * DIR_ENTRY_SIZE + 0x64);
        node->modified_time = get_u64(std::span<const u8>(dir), id * DIR_ENTRY_SIZE + 0x6C);

        if (type == TYPE_STREAM) {
            // [MS-CFB] 2.6.3: a version 3 file's stream size is at most
            // 0x80000000, and parsers ignore the high DWORD, which some
            // older writers left uninitialised.
            u64 size = get_u64(std::span<const u8>(dir),
                               id * DIR_ENTRY_SIZE + 0x78);
            if (sector_shift == 9) size &= 0xFFFFFFFFull;
            const u32 start = entry_u32(id, 0x74);
            if (size < mini_cutoff) {
                (void)read_chain(
                    std::span<const u8>(mini_stream), mini_fat, start, size,
                    MINI_SECTOR_SIZE, 0, node->data);
            } else {
                (void)read_chain(image, fat, start, size, sector_size,
                                 data_origin, node->data);
            }
        } else {
            const u32 child = entry_u32(id, 0x4C);
            if (child != NOSTREAM) add_siblings(child, *node);
        }
        return node;
    };

    add_siblings = [&](u32 id, StorageNode& parent) {
        if (id >= entry_count || visited[id]) return;
        const u32 left  = entry_u32(id, 0x44);
        const u32 right = entry_u32(id, 0x48);
        auto node = build_node(id);
        if (node != nullptr) parent.children[node->name] = node;
        if (left  != NOSTREAM) add_siblings(left,  parent);
        if (right != NOSTREAM) add_siblings(right, parent);
    };

    auto root = build_node(0);
    if (root != nullptr) {
        root->is_storage = true;
        root->data.clear();
    }
    return root;
}

} // namespace vitreo::winapi
