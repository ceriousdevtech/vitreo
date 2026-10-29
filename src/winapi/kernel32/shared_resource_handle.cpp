// SPDX-License-Identifier: LGPL-2.1-only
/// @file shared_resource_handle.cpp
/// @brief NT handles to DXGI shared resources and their names.

#include "winapi/kernel32/shared_resource_handle.hpp"

#include "ipc/shared_memory.hpp"
#include "winapi/kernel32/named_object_directory.hpp"

#include <cctype>
#include <charconv>
#include <cstring>
#include <mutex>
#include <vector>

namespace vitreo::winapi {
namespace {

std::mutex g_retainer_mutex;
SharedResourceRetainer g_retainer;

/// What a name's registry entry holds (POSIX shm, one page).
struct NameRecord {
    static constexpr u32 kMagic = 0x4d4e5244u;  // 'DRNM'
    u32 magic{0};
    u32 surface_id{0};
    u64 nonce{0};
};

/// One process's hold on a name: the registry entry, and the in-process
/// kernel-object namespace entry when this process created it.
struct NameRegistration {
    std::shared_ptr<ipc::SharedMemory> entry;
    std::vector<std::string> directory_names;
    ~NameRegistration() {
        for (const auto& name : directory_names) {
            named_object_unregister(name, NamedObjectKind::SharedResource);
        }
    }
};

constexpr u32 HANDLE_FLAG_INHERIT = 0x1;

std::string registry_name(const std::string& key) {
    return "dxgi-shared-resource:" + key;
}

/// Learn Kernel Object Namespaces: "Local\" is the session namespace a bare
/// name already lives in. Measured: the comparison ignores case.
std::string name_key(std::string_view name) {
    std::string key(name);
    for (char& c : key) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    constexpr std::string_view kLocal = "local\\";
    if (key.starts_with(kLocal)) key.erase(0, kLocal.size());
    return key;
}

std::shared_ptr<void> attach_name(const std::string& key) {
    auto entry = ipc::SharedMemory::create_or_open(registry_name(key),
                                                   sizeof(NameRecord), false);
    if (entry == nullptr) return nullptr;
    auto registration = std::make_shared<NameRegistration>();
    registration->entry = std::move(entry);
    return registration;
}

std::string to_hex(std::string_view text) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() * 2);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 15]);
    }
    return out;
}

std::optional<std::string> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::string out;
    out.reserve(hex.size() / 2);
    for (usize i = 0; i < hex.size(); i += 2) {
        unsigned value = 0;
        const auto result =
            std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != hex.data() + i + 2) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(value));
    }
    return out;
}

}  // namespace

void set_shared_resource_retainer(SharedResourceRetainer retainer) {
    std::lock_guard lock(g_retainer_mutex);
    g_retainer = std::move(retainer);
}

Handle allocate_shared_resource_handle(
    HandleTable& table, std::shared_ptr<SharedResourceObject> object,
    u32 access, bool inherit) {
    if (object == nullptr) return NULL_HANDLE;
    auto* holder = new std::shared_ptr<SharedResourceObject>(std::move(object));
    const Handle handle =
        table.allocate(HandleType::SharedResource, holder,
                       inherit ? HANDLE_FLAG_INHERIT : 0u, access);
    if (handle == NULL_HANDLE) delete holder;
    return handle;
}

std::shared_ptr<SharedResourceObject> shared_resource_object(
    const HandleTable& table, Handle handle, u32* access_out) {
    auto object =
        table.get_shared<SharedResourceObject>(handle,
                                               HandleType::SharedResource);
    if (object != nullptr && access_out != nullptr) {
        *access_out = table.get_access(handle);
    }
    return object;
}

void release_shared_resource_handle_object(void* object) noexcept {
    delete static_cast<std::shared_ptr<SharedResourceObject>*>(object);
}

Handle duplicate_shared_resource_handle(HandleTable& table, Handle source,
                                        u32 access, bool inherit) {
    return allocate_shared_resource_handle(
        table, shared_resource_object(table, source), access, inherit);
}

std::optional<std::string> prepare_shared_resource_transfer(
    const HandleTable& table, Handle source, u32 access, bool inherit) {
    const auto object = shared_resource_object(table, source);
    if (object == nullptr) return std::nullopt;
    return "D," + std::to_string(access) + "," +
           std::to_string(inherit ? HANDLE_FLAG_INHERIT : 0u) + "," +
           std::to_string(object->surface_id) + "," +
           std::to_string(object->nonce) + "," +
           (object->name_key.empty() ? std::string("-")
                                     : to_hex(object->name_key));
}

Handle import_shared_resource_transfer(HandleTable& table,
                                       std::string_view descriptor) {
    if (!descriptor.starts_with("D,")) return NULL_HANDLE;
    std::string_view fields[5];
    std::string_view rest = descriptor.substr(2);
    for (usize i = 0; i < 5; ++i) {
        const usize comma = rest.find(',');
        if (i + 1 == 5) {
            if (comma != std::string_view::npos) return NULL_HANDLE;
            fields[i] = rest;
        } else {
            if (comma == std::string_view::npos) return NULL_HANDLE;
            fields[i] = rest.substr(0, comma);
            rest = rest.substr(comma + 1);
        }
    }
    u32 access = 0;
    u32 flags = 0;
    u32 surface = 0;
    u64 nonce = 0;
    const auto parse = [](std::string_view text, auto& value) {
        const auto result =
            std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} &&
               result.ptr == text.data() + text.size();
    };
    if (!parse(fields[0], access) || !parse(fields[1], flags) ||
        !parse(fields[2], surface) || !parse(fields[3], nonce) ||
        surface == 0) {
        return NULL_HANDLE;
    }
    auto object = std::make_shared<SharedResourceObject>();
    object->surface_id = surface;
    object->nonce = nonce;
    {
        std::lock_guard lock(g_retainer_mutex);
        if (g_retainer) object->keepalive = g_retainer(surface);
    }
    if (fields[4] != "-") {
        auto key = from_hex(fields[4]);
        if (!key) return NULL_HANDLE;
        // The name lives while any process holds a handle to its object.
        object->name = attach_name(*key);
        if (object->name != nullptr) object->name_key = std::move(*key);
    }
    return allocate_shared_resource_handle(table, std::move(object), access,
                                           (flags & HANDLE_FLAG_INHERIT) != 0);
}

SharedResourceNameResult register_shared_resource_name(
    std::string_view name, u32 surface_id, u64 nonce,
    std::shared_ptr<void>& registration_out, std::string& key_out) {
    registration_out.reset();
    // One namespace with this process's events, mutexes, sections, ...
    // which this process's directory holds as spelled (events) or without
    // "Local\" (sections): claim both spellings.
    std::vector<std::string> directory_names{std::string(name)};
    const std::string bare = normalize_named_object_name(std::string(name));
    if (bare.empty()) return SharedResourceNameResult::Failed;
    if (bare != directory_names.front()) directory_names.push_back(bare);
    for (const auto& spelling : directory_names) {
        if (named_object_kind_conflict(spelling,
                                       NamedObjectKind::SharedResource)) {
            return SharedResourceNameResult::AlreadyExists;
        }
    }
    const std::string key = name_key(name);
    auto entry = ipc::SharedMemory::create_or_open(registry_name(key),
                                                   sizeof(NameRecord), true);
    if (entry == nullptr) return SharedResourceNameResult::Failed;
    if (!entry->is_creator()) return SharedResourceNameResult::AlreadyExists;
    void* view = entry->map_view(0, sizeof(NameRecord));
    if (view == nullptr) return SharedResourceNameResult::Failed;
    NameRecord record{};
    record.magic = NameRecord::kMagic;
    record.surface_id = surface_id;
    record.nonce = nonce;
    std::memcpy(view, &record, sizeof(record));
    ipc::SharedMemory::unmap_view(view, sizeof(NameRecord));
    auto registration = std::make_shared<NameRegistration>();
    registration->entry = std::move(entry);
    for (const auto& spelling : directory_names) {
        if (!named_object_register(spelling,
                                   NamedObjectKind::SharedResource)) {
            return SharedResourceNameResult::AlreadyExists;
        }
        registration->directory_names.push_back(spelling);
    }
    registration_out = std::move(registration);
    key_out = key;
    return SharedResourceNameResult::Registered;
}

std::optional<SharedResourceNameTarget> lookup_shared_resource_name(
    std::string_view name) {
    const std::string key = name_key(name);
    if (key.empty()) return std::nullopt;
    auto entry = ipc::SharedMemory::create_or_open(registry_name(key),
                                                   sizeof(NameRecord), false);
    if (entry == nullptr) return std::nullopt;
    void* view = entry->map_view(0, sizeof(NameRecord));
    if (view == nullptr) return std::nullopt;
    NameRecord record{};
    std::memcpy(&record, view, sizeof(record));
    ipc::SharedMemory::unmap_view(view, sizeof(NameRecord));
    if (record.magic != NameRecord::kMagic || record.surface_id == 0) {
        return std::nullopt;
    }
    return SharedResourceNameTarget{record.surface_id, record.nonce};
}

}  // namespace vitreo::winapi
