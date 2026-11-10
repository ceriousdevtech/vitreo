// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file translation_cache.hpp
/// @brief A persistent, cross-launch cache of shader translations (SPIR-V ->
///        MSL, DXIL -> DXBC), keyed on their exact inputs and on the
///        translator itself.
///
/// The key of an entry is the SHA-256 of: the entry kind, the translator
/// hash (VITREO_TRANSLATOR_HASH, generated at build time from the sources of
/// the shader translators and SPIRV-Cross -- not the binary's LC_UUID, so a
/// rebuild that does not touch a translator stays warm, and any change to
/// one starts a new cache), and every input the translation reads (the
/// caller's key material). The value is the translation's result.
///
/// Where: $VITREO_SHADER_CACHE_DIR when set (tests, the replay benchmark),
/// else the capsule's cache directory ($VITREO_IPC_SCOPE/cache/
/// shader-translations). Neither: no persistent cache (lookups miss, stores
/// are dropped). VITREO_SHADER_CACHE_DIR=off turns it off.
///
/// Layout: <root>/<translator hash>/<key[0..1]>/<key>.bin. Each file is
/// written to a temporary name in its directory and renamed over the final
/// one, so a reader in any process sees a whole entry or none; a file whose
/// header, length, key or payload checksum does not match (a torn write
/// after power loss, a foreign file) is a miss and is removed. Two
/// processes storing one key write the same bytes. The cache is bounded:
/// past kMaxBytes the oldest files (by modification time, every translator
/// hash's directory together) are removed down to three quarters of it, at
/// startup and after every kPruneEvery stores.

#include <vitreo/common.hpp>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// The entry kinds (part of every key).
enum class TranslationKind : u32 {
    SpirvToMsl = 1,
    DxilToDxbc = 2,
};

/// Builds an entry key: SHA-256 over the kind, the translator hash and the
/// added inputs, each length-prefixed so no two input lists collide.
class TranslationKey {
public:
    explicit TranslationKey(TranslationKind kind);
    TranslationKey& bytes(const void* data, usize size);
    TranslationKey& u32_value(u32 value) { return bytes(&value, sizeof value); }
    TranslationKey& text(const std::string& value) { return bytes(value.data(), value.size()); }
    [[nodiscard]] std::array<u8, 32> finish() const;

private:
    std::vector<u8> m_material;
};

/// Little-endian payload writer / reader for cache values.
class PayloadWriter {
public:
    void u32_value(u32 v) { raw(&v, 4); }
    void bytes(const void* data, usize size) {
        u32_value(static_cast<u32>(size));
        raw(data, size);
    }
    void raw(const void* data, usize size) {
        const auto* p = static_cast<const u8*>(data);
        out.insert(out.end(), p, p + size);
    }
    std::vector<u8> out;
};

class PayloadReader {
public:
    explicit PayloadReader(const std::vector<u8>& in) : m_in(in) {}
    bool u32_value(u32& v) { return raw(&v, 4); }
    bool bytes(std::vector<u8>& v) {
        u32 n = 0;
        if (!u32_value(n) || n > m_in.size() - m_at) return false;
        v.assign(m_in.begin() + static_cast<std::ptrdiff_t>(m_at),
                 m_in.begin() + static_cast<std::ptrdiff_t>(m_at + n));
        m_at += n;
        return true;
    }
    bool raw(void* data, usize size) {
        if (size > m_in.size() - m_at) return false;
        std::copy_n(m_in.begin() + static_cast<std::ptrdiff_t>(m_at), size, static_cast<u8*>(data));
        m_at += size;
        return true;
    }
    [[nodiscard]] bool done() const { return m_at == m_in.size(); }

private:
    const std::vector<u8>& m_in;
    usize m_at{0};
};

class TranslationCache {
public:
    static constexpr u64 kMaxBytes = 1ull << 30;  // 1 GiB per capsule
    static constexpr u32 kPruneEvery = 2048;

    static TranslationCache& instance();

    [[nodiscard]] bool enabled() const { return !m_dir.empty(); }
    /// The payload stored under @p key, or false.
    bool lookup(const std::array<u8, 32>& key, std::vector<u8>& payload);
    void store(const std::array<u8, 32>& key, const std::vector<u8>& payload);

    /// Tests: a cache rooted at @p root ("" = off), replacing the process
    /// one's directory.
    void reset_for_test(const std::string& root);
    /// This build's translator hash (hex).
    [[nodiscard]] static const char* translator_hash();
    /// The directory entries of this translator live in ("" when off).
    [[nodiscard]] std::string directory() const;

private:
    TranslationCache();
    void open(const std::string& root);
    void prune();
    [[nodiscard]] std::string path_of(const std::array<u8, 32>& key) const;

    mutable std::mutex m_mutex;
    std::string m_root;
    std::string m_dir;
    std::atomic<u32> m_stores{0};
};

}  // namespace vitreo::graphics
