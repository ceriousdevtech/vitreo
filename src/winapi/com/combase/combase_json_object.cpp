// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_json_object.cpp
/// @brief Windows.Data.Json.JsonObject (IJsonObject)

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <cstring>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Windows.Data.Json.JsonObject — IJsonObject.
//
// Windows.Data.Json.idl: the typed getters raise when the name is absent
// (E_BOUNDS) or holds another type (E_ILLEGAL_METHOD_CALL); only the
// *WithDefaultValues variants substitute a fallback, and those are a separate
// interface.
// ---------------------------------------------------------------------------
CombaseManager::Object* CombaseManager::json_object_arg(
    Marshaler& m, std::u16string* name, GuestAddr* out_param) {
    if (out_param != nullptr) *out_param = m.arg_ptr(2);
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::JsonObject) {
        return nullptr;
    }
    if (name != nullptr) {
        const GuestAddr name_string = m.arg_ptr(1);
        if (name_string != 0) {
            auto read = read_hstring(name_string, m.cpu_is_64bit());
            if (read.has_value()) *name = std::move(*read);
        }
    }
    return &it->second;
}

CombaseManager::Object* CombaseManager::json_member(
    Object& owner, const std::u16string& name) {
    for (const auto& entry : owner.json_members) {
        if (entry.first != name) continue;
        auto it = m_objects.find(entry.second);
        return it == m_objects.end() ? nullptr : &it->second;
    }
    return nullptr;
}

void CombaseManager::impl_JsonObjectGetNamedValue(Marshaler& m) {
    std::u16string name;
    GuestAddr out = 0;
    Object* owner = json_object_arg(m, &name, &out);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    if (owner == nullptr) { m.set_return(E_INVALIDARG); return; }
    for (const auto& entry : owner->json_members) {
        if (entry.first != name) continue;
        auto it = m_objects.find(entry.second);
        if (it != m_objects.end()) ++it->second.ref_count;
        m.set_return(m.write_ptr(out, entry.second).is_ok() ? S_OK : E_POINTER);
        return;
    }
    m.set_return(E_BOUNDS);
}

void CombaseManager::impl_JsonObjectSetNamedValue(Marshaler& m) {
    std::u16string name;
    Object* owner = json_object_arg(m, &name, nullptr);
    if (owner == nullptr) { m.set_return(E_INVALIDARG); return; }
    const GuestAddr value = m.arg_ptr(2);
    for (auto& entry : owner->json_members) {
        if (entry.first != name) continue;
        entry.second = value;
        m.set_return(S_OK);
        return;
    }
    owner->json_members.emplace_back(name, value);
    m.set_return(S_OK);
}

void CombaseManager::impl_JsonObjectGetNamedObject(Marshaler& m) {
    std::u16string name;
    GuestAddr out = 0;
    Object* owner = json_object_arg(m, &name, &out);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    if (owner == nullptr) { m.set_return(E_INVALIDARG); return; }
    Object* member = json_member(*owner, name);
    if (member == nullptr) { m.set_return(E_BOUNDS); return; }
    if (member->type != ObjectType::JsonObject) {
        m.set_return(E_ILLEGAL_METHOD_CALL);
        return;
    }
    ++member->ref_count;
    for (const auto& entry : owner->json_members) {
        if (entry.first == name) {
            m.set_return(m.write_ptr(out, entry.second).is_ok() ? S_OK
                                                                : E_POINTER);
            return;
        }
    }
    m.set_return(E_BOUNDS);
}

void CombaseManager::impl_JsonObjectGetNamedArray(Marshaler& m) {
    std::u16string name;
    GuestAddr out = 0;
    Object* owner = json_object_arg(m, &name, &out);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    if (owner == nullptr) { m.set_return(E_INVALIDARG); return; }
    // JsonArray is not modelled, so a stored member can never be one.
    m.set_return(json_member(*owner, name) == nullptr ? E_BOUNDS
                                                      : E_ILLEGAL_METHOD_CALL);
}

void CombaseManager::impl_JsonObjectGetNamedString(Marshaler& m) {
    std::u16string name;
    GuestAddr out = 0;
    Object* owner = json_object_arg(m, &name, &out);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    if (owner == nullptr) { m.set_return(E_INVALIDARG); return; }
    Object* member = json_member(*owner, name);
    if (member == nullptr) { m.set_return(E_BOUNDS); return; }
    if (member->type != ObjectType::JsonValue ||
        member->json_type != JsonType::String) {
        m.set_return(E_ILLEGAL_METHOD_CALL);
        return;
    }
    const GuestAddr hstring = create_hstring(
        member->values.empty() ? std::u16string() : member->values[0],
        m.cpu_is_64bit());
    m.set_return(m.write_ptr(out, hstring).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonObjectGetNamedNumber(Marshaler& m) {
    std::u16string name;
    GuestAddr out = 0;
    Object* owner = json_object_arg(m, &name, &out);
    if (out == 0) { m.set_return(E_POINTER); return; }
    if (owner == nullptr) { m.set_return(E_INVALIDARG); return; }
    Object* member = json_member(*owner, name);
    if (member == nullptr) { m.set_return(E_BOUNDS); return; }
    if (member->type != ObjectType::JsonValue ||
        member->json_type != JsonType::Number) {
        m.set_return(E_ILLEGAL_METHOD_CALL);
        return;
    }
    u64 bits = 0;
    std::memcpy(&bits, &member->json_number, sizeof(bits));
    m.set_return(m.write_u64(out, bits).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonObjectGetNamedBoolean(Marshaler& m) {
    std::u16string name;
    GuestAddr out = 0;
    Object* owner = json_object_arg(m, &name, &out);
    if (out == 0) { m.set_return(E_POINTER); return; }
    if (owner == nullptr) { m.set_return(E_INVALIDARG); return; }
    Object* member = json_member(*owner, name);
    if (member == nullptr) { m.set_return(E_BOUNDS); return; }
    if (member->type != ObjectType::JsonValue ||
        member->json_type != JsonType::Boolean) {
        m.set_return(E_ILLEGAL_METHOD_CALL);
        return;
    }
    m.set_return(m.write_u8(out, member->json_boolean ? 1u : 0u).is_ok()
                     ? S_OK
                     : E_POINTER);
}

} // namespace vitreo::winapi
