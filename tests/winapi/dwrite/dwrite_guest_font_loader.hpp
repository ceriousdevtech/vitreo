// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dwrite_guest_font_loader.hpp
/// @brief Guest-implemented font collection/file loader harness for the
///        custom-collection DirectWrite tests.

#include "dwrite_manager_harness.hpp"

#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi::dwrite_test {

inline std::vector<u8> system_font_bytes(const std::u16string& family) {
    auto font = dwrite::match_system_font(family, 400, 5, 0);
    if (!font) return {};
    return dwrite::font_file_data(*font).value_or(std::vector<u8>{});
}

// Guest-implemented IDWriteFontCollectionLoader, IDWriteFontFileEnumerator,
// IDWriteFontFileLoader and IDWriteFontFileStream (slot order from dwrite.h),
// shaped like Chromium's font proxy: every file key is a UINT32 index into
// files the guest has memory-mapped. The test can replace a file's bytes
// between CreateCustomFontCollection calls while its key stays the same.
class GuestFontLoader {
public:
    static constexpr GuestAddr BASE = 0x40000000;
    static constexpr usize REGION_SIZE = 0x01000000;
    static constexpr GuestAddr COLLECTION_LOADER = BASE;
    static constexpr GuestAddr FILE_LOADER = BASE + 0x100;
    static constexpr GuestAddr ENUMERATOR = BASE + 0x200;
    static constexpr GuestAddr STREAMS = BASE + 0x300;   // 0x20 per file
    static constexpr GuestAddr COLLECTION_KEY = BASE + 0x800;
    static constexpr GuestAddr FILE_KEYS = BASE + 0x810;  // UINT32 per file
    static constexpr GuestAddr NAME = BASE + 0x900;
    static constexpr GuestAddr OUT = BASE + 0xA00;
    static constexpr GuestAddr DATA = BASE + 0x100000;
    static constexpr usize SLOT = 0x200000;
    static constexpr GuestAddr ADDREF = 0x72000010;
    static constexpr GuestAddr RELEASE = 0x72000020;
    static constexpr GuestAddr CREATE_ENUMERATOR = 0x72000030;
    static constexpr GuestAddr MOVE_NEXT = 0x72000040;
    static constexpr GuestAddr GET_CURRENT_FILE = 0x72000050;
    static constexpr GuestAddr CREATE_STREAM = 0x72000060;
    static constexpr GuestAddr READ_FRAGMENT = 0x72000070;
    static constexpr GuestAddr RELEASE_FRAGMENT = 0x72000080;
    static constexpr GuestAddr GET_FILE_SIZE = 0x72000090;
    static constexpr GuestAddr GET_LAST_WRITE_TIME = 0x720000A0;

    struct File {
        std::vector<u8> bytes;
        u64 last_write_time{0};
        GuestAddr object{0};
    };

    GuestFontLoader(GuestMemory& memory, cpu::CpuState& cpu,
                    DWriteManager& manager, bool is_64bit)
        : m_memory(memory), m_cpu(cpu), m_manager(manager), m_64(is_64bit) {}

    bool set_up(GuestAddr iid_address) {
        if (m_memory.map_region(BASE, REGION_SIZE, MemoryProtection::ReadWrite).is_err())
            return false;
        const usize ptr = m_64 ? 8 : 4;
        const auto object = [&](GuestAddr self, std::initializer_list<GuestAddr> slots) {
            const GuestAddr vtable = self + 0x40;
            (void)write_ptr(self, vtable);
            (void)write_ptr(vtable + 1 * ptr, ADDREF);
            (void)write_ptr(vtable + 2 * ptr, RELEASE);
            usize slot = 3;
            for (const GuestAddr fn : slots) (void)write_ptr(vtable + slot++ * ptr, fn);
        };
        object(COLLECTION_LOADER, {CREATE_ENUMERATOR});
        object(FILE_LOADER, {CREATE_STREAM});
        object(ENUMERATOR, {MOVE_NEXT, GET_CURRENT_FILE});
        const GuestAddr stream_vtable = STREAMS + 0x400;
        (void)write_ptr(stream_vtable + 1 * ptr, ADDREF);
        (void)write_ptr(stream_vtable + 2 * ptr, RELEASE);
        (void)write_ptr(stream_vtable + 3 * ptr, READ_FRAGMENT);
        (void)write_ptr(stream_vtable + 4 * ptr, RELEASE_FRAGMENT);
        (void)write_ptr(stream_vtable + 5 * ptr, GET_FILE_SIZE);
        (void)write_ptr(stream_vtable + 6 * ptr, GET_LAST_WRITE_TIME);
        for (u32 index = 0; index < 16; ++index)
            (void)write_ptr(STREAMS + index * 0x20, stream_vtable);
        m_manager.set_guest_call([this](GuestAddr fn, const u64* args, usize count, bool) {
            return guest_call(fn, args, count);
        });
        if (invoke(&DWriteManager::impl_DWriteCreateFactory, {0, iid_address, OUT}) != 0)
            return false;
        factory = read_ptr(OUT);
        return factory != 0 &&
            invoke(&DWriteManager::impl_RegisterFontCollectionLoader,
                   {factory, COLLECTION_LOADER}) == 0 &&
            invoke(&DWriteManager::impl_RegisterFontFileLoader, {factory, FILE_LOADER}) == 0;
    }

    bool add_file(std::vector<u8> bytes, u64 last_write_time) {
        const u32 index = static_cast<u32>(files.size());
        files.push_back({});
        if (!set_file(index, std::move(bytes), last_write_time)) return false;
        const GuestAddr key = FILE_KEYS + index * 4;
        (void)m_memory.write_u32(key, index);
        if (invoke(&DWriteManager::impl_CreateCustomFontFileReference,
                   {factory, key, 4, FILE_LOADER, OUT}) != 0) {
            return false;
        }
        files[index].object = read_ptr(OUT);
        return files[index].object != 0;
    }

    bool set_file(u32 index, std::vector<u8> bytes, u64 last_write_time) {
        if (bytes.size() > SLOT) return false;
        files[index].bytes = std::move(bytes);
        files[index].last_write_time = last_write_time;
        return files[index].bytes.empty() ||
            m_memory.write_bytes(DATA + index * SLOT, files[index].bytes.data(),
                                 files[index].bytes.size()).is_ok();
    }

    std::pair<u32, GuestAddr> create_collection(u32 key = 1) {
        log.clear();
        m_collection_key = key;
        (void)m_memory.write_u32(COLLECTION_KEY, key);
        const u32 hr = invoke(&DWriteManager::impl_CreateCustomFontCollection,
                              {factory, COLLECTION_LOADER, COLLECTION_KEY, 4, OUT});
        return {hr, hr == 0 ? read_ptr(OUT) : 0};
    }

    u32 family_count(GuestAddr collection) {
        return invoke(&DWriteManager::impl_GetFontFamilyCount, {collection});
    }

    bool reregister_collection_loader() {
        return invoke(&DWriteManager::impl_UnregisterFontCollectionLoader,
                      {factory, COLLECTION_LOADER}) == 0 &&
               invoke(&DWriteManager::impl_RegisterFontCollectionLoader,
                      {factory, COLLECTION_LOADER}) == 0;
    }
    bool reregister_file_loader() {
        return invoke(&DWriteManager::impl_UnregisterFontFileLoader, {factory, FILE_LOADER}) == 0 &&
               invoke(&DWriteManager::impl_RegisterFontFileLoader, {factory, FILE_LOADER}) == 0;
    }

    // The log without fragment traffic, whose offsets and sizes are not part
    // of any contract.
    std::vector<std::string> skeleton() const {
        std::vector<std::string> result;
        for (const auto& entry : log) {
            if (entry.rfind("ReadFileFragment", 0) != 0 &&
                entry.rfind("ReleaseFileFragment", 0) != 0) {
                result.push_back(entry);
            }
        }
        return result;
    }

    // Indices of the files the enumerator yields; all files when empty.
    std::vector<u32> active;

    bool has_family(GuestAddr collection, const std::u16string& name) {
        (void)m_memory.write_bytes(NAME, reinterpret_cast<const Byte*>(name.c_str()),
                                   (name.size() + 1) * sizeof(char16_t));
        (void)m_memory.write_u32(OUT + 0x10, 0xCCCCCCCCu);
        if (invoke(&DWriteManager::impl_FindFamilyName,
                   {collection, NAME, OUT + 0x08, OUT + 0x10}) != 0) {
            return false;
        }
        return m_memory.read_u32(OUT + 0x10).value_or(0) != 0;
    }

    u32 invoke(void (DWriteManager::*impl)(Marshaler&), std::initializer_list<u64> args) {
        const GuestAddr sp = m_cpu.gpr.rsp;
        usize index = 0;
        for (const u64 arg : args) {
            if (m_64) {
                if (index == 0) m_cpu.gpr.rcx = arg;
                else if (index == 1) m_cpu.gpr.rdx = arg;
                else if (index == 2) m_cpu.gpr.r8 = arg;
                else if (index == 3) m_cpu.gpr.r9 = arg;
                else (void)m_memory.write_u64(sp + 0x28 + (index - 4) * 8, arg);
            } else {
                (void)m_memory.write_u32(sp + 4 + index * 4, static_cast<u32>(arg));
            }
            ++index;
        }
        Marshaler marshaler(m_memory);
        marshaler.attach_cpu(m_cpu);
        (m_manager.*impl)(marshaler);
        return static_cast<u32>(m_cpu.gpr.rax);
    }

    GuestAddr read_ptr(GuestAddr address) const {
        return m_64 ? m_memory.read_u64(address).value_or(0)
                    : m_memory.read_u32(address).value_or(0);
    }
    bool write_ptr(GuestAddr address, GuestAddr value) {
        return m_64 ? m_memory.write_u64(address, value).is_ok()
                    : m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
    }

    GuestAddr factory{0};
    std::vector<File> files;
    std::vector<std::string> log;
    // Breaches of documented IDWriteFont* caller obligations.
    std::vector<std::string> violations;
    std::set<GuestAddr> outstanding_fragments;
    u32 open_streams{0};

private:
    u64 guest_call(GuestAddr fn, const u64* args, usize count) {
        constexpr u64 E_FAIL = 0x80004005u;
        constexpr u64 E_INVALIDARG = 0x80070057u;
        const auto stream_index = [&](u64 stream) -> std::optional<u32> {
            if (stream < STREAMS || (stream - STREAMS) % 0x20 != 0) return std::nullopt;
            const u32 index = static_cast<u32>((stream - STREAMS) / 0x20);
            return index < files.size() ? std::optional<u32>(index) : std::nullopt;
        };
        if (fn == DWriteManagerTest::RELEASE_THUNK) return 1;  // file stays owned here
        if (fn == ADDREF) return 2;
        if (fn == RELEASE) {
            if (args[0] == ENUMERATOR) {
                log.push_back("Release(enumerator)");
            } else if (auto index = stream_index(args[0])) {
                log.push_back("Release(stream " + std::to_string(*index) + ")");
                if (open_streams == 0) violations.push_back("stream over-released");
                else --open_streams;
            }
            return 1;
        }
        if (fn == CREATE_ENUMERATOR && count == 5) {
            // CreateEnumeratorFromKey(factory, collectionKey, collectionKeySize, enumerator)
            if (args[1] != factory) violations.push_back("enumerator factory");
            if (args[3] != 4 || m_memory.read_u32(args[2]).value_or(0) != m_collection_key)
                violations.push_back("collection key");
            log.push_back("CreateEnumeratorFromKey");
            m_position = 0;
            m_has_current = false;
            write_ptr(args[4], ENUMERATOR);
            return 0;
        }
        if (fn == MOVE_NEXT && count == 2) {
            m_has_current = m_position < enumerated().size();
            if (m_has_current) ++m_position;
            (void)m_memory.write_u32(args[1], m_has_current ? 1 : 0);
            log.push_back(std::string("MoveNext->") + (m_has_current ? "1" : "0"));
            return 0;
        }
        if (fn == GET_CURRENT_FILE && count == 2) {
            // Learn: E_FAIL when MoveNext has not positioned the enumerator.
            if (!m_has_current) {
                violations.push_back("GetCurrentFontFile without current file");
                return E_FAIL;
            }
            const u32 current = enumerated()[m_position - 1];
            log.push_back("GetCurrentFontFile " + std::to_string(current));
            write_ptr(args[1], files[current].object);
            return 0;
        }
        if (fn == CREATE_STREAM && count == 4) {
            const u32 index = m_memory.read_u32(args[1]).value_or(~0u);
            if (args[2] != 4 || index >= files.size()) {
                violations.push_back("stream key");
                return E_INVALIDARG;
            }
            log.push_back("CreateStreamFromKey " + std::to_string(index));
            ++open_streams;
            write_ptr(args[3], STREAMS + index * 0x20);
            return 0;
        }
        const auto index = count != 0 ? stream_index(args[0]) : std::nullopt;
        if (fn == GET_FILE_SIZE && count == 2 && index) {
            log.push_back("GetFileSize " + std::to_string(*index));
            (void)m_memory.write_u64(args[1], files[*index].bytes.size());
            return 0;
        }
        if (fn == GET_LAST_WRITE_TIME && count == 2 && index) {
            log.push_back("GetLastWriteTime " + std::to_string(*index));
            (void)m_memory.write_u64(args[1], files[*index].last_write_time);
            return 0;
        }
        if (fn == READ_FRAGMENT && index && count == (m_64 ? 5u : 7u)) {
            const u64 offset = m_64 ? args[2] : (args[2] | (args[3] << 32));
            const u64 size = m_64 ? args[3] : (args[4] | (args[5] << 32));
            const u64 context_out = m_64 ? args[4] : args[6];
            const u64 file_size = files[*index].bytes.size();
            log.push_back("ReadFileFragment " + std::to_string(*index) + " " +
                          std::to_string(offset) + "+" + std::to_string(size));
            // Learn: fragments outside the file bounds must fail.
            if (offset > file_size || size > file_size - offset) {
                violations.push_back("fragment out of bounds");
                return E_INVALIDARG;
            }
            const GuestAddr context = 0xC0DE0000u + static_cast<GuestAddr>(++m_contexts);
            outstanding_fragments.insert(context);
            write_ptr(args[1], DATA + *index * SLOT + offset);
            write_ptr(context_out, context);
            return 0;
        }
        if (fn == RELEASE_FRAGMENT && count == 2 && index) {
            log.push_back("ReleaseFileFragment " + std::to_string(*index));
            if (outstanding_fragments.erase(args[1]) == 0)
                violations.push_back("released unknown fragment context");
            return 0;
        }
        violations.push_back("unexpected guest call");
        return E_FAIL;
    }

    std::vector<u32> enumerated() const {
        if (!active.empty()) return active;
        std::vector<u32> all(files.size());
        for (u32 index = 0; index < all.size(); ++index) all[index] = index;
        return all;
    }

    GuestMemory& m_memory;
    cpu::CpuState& m_cpu;
    DWriteManager& m_manager;
    bool m_64;
    u32 m_collection_key{0};
    usize m_position{0};
    bool m_has_current{false};
    u32 m_contexts{0};
};

}  // namespace vitreo::winapi::dwrite_test
