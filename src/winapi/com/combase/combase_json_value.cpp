// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_json_value.cpp
/// @brief Windows.Data.Json.JsonValue (IJsonValueStatics and IJsonValue)

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::winapi {

// ============================================================================
// Windows.Data.Json.JsonValue
//
// Windows.Data.Json.idl: IJsonValueStatics exposes Parse/TryParse and the four
// Create*Value factories; IJsonValue exposes ValueType, Stringify and the
// typed getters. A getter called on the wrong value type returns
// E_ILLEGAL_METHOD_CALL, which is what WinRT does rather than coercing.
// ============================================================================

namespace {

/// Minimal JSON scalar reader. Composite values need JsonObject/JsonArray
/// state Vitreo does not model, so they are reported as unsupported rather
/// than silently mis-parsed.
enum class ScalarParse { Null, Boolean, Number, String, Composite, Invalid };

ScalarParse classify_json_scalar(const std::u16string& text, double* number,
                                 bool* boolean, std::u16string* str) {
    usize i = 0;
    const usize n = text.size();
    auto skip_ws = [&] {
        while (i < n && (text[i] == u' ' || text[i] == u'\t' ||
                         text[i] == u'\n' || text[i] == u'\r')) {
            ++i;
        }
    };
    skip_ws();
    if (i >= n) return ScalarParse::Invalid;
    const char16_t c = text[i];
    if (c == u'{' || c == u'[') return ScalarParse::Composite;
    if (text.compare(i, 4, u"null") == 0) {
        i += 4; skip_ws();
        return i == n ? ScalarParse::Null : ScalarParse::Invalid;
    }
    if (text.compare(i, 4, u"true") == 0) {
        i += 4; skip_ws();
        if (i != n) return ScalarParse::Invalid;
        *boolean = true;
        return ScalarParse::Boolean;
    }
    if (text.compare(i, 5, u"false") == 0) {
        i += 5; skip_ws();
        if (i != n) return ScalarParse::Invalid;
        *boolean = false;
        return ScalarParse::Boolean;
    }
    if (c == u'"') {
        std::u16string out;
        ++i;
        while (i < n && text[i] != u'"') {
            if (text[i] == u'\\' && i + 1 < n) {
                ++i;
                switch (text[i]) {
                case u'n': out.push_back(u'\n'); break;
                case u't': out.push_back(u'\t'); break;
                case u'r': out.push_back(u'\r'); break;
                case u'b': out.push_back(u'\b'); break;
                case u'f': out.push_back(u'\f'); break;
                case u'u': {
                    if (i + 4 >= n) return ScalarParse::Invalid;
                    u32 code = 0;
                    for (int k = 1; k <= 4; ++k) {
                        const char16_t h = text[i + static_cast<usize>(k)];
                        code <<= 4;
                        if (h >= u'0' && h <= u'9') code |= static_cast<u32>(h - u'0');
                        else if (h >= u'a' && h <= u'f') code |= static_cast<u32>(h - u'a' + 10);
                        else if (h >= u'A' && h <= u'F') code |= static_cast<u32>(h - u'A' + 10);
                        else return ScalarParse::Invalid;
                    }
                    i += 4;
                    out.push_back(static_cast<char16_t>(code));
                    break;
                }
                default: out.push_back(text[i]); break;
                }
            } else {
                out.push_back(text[i]);
            }
            ++i;
        }
        if (i >= n) return ScalarParse::Invalid;
        ++i;
        skip_ws();
        if (i != n) return ScalarParse::Invalid;
        *str = std::move(out);
        return ScalarParse::String;
    }
    // Number.
    std::string ascii;
    while (i < n) {
        const char16_t d = text[i];
        if (d > 127) break;
        const char a = static_cast<char>(d);
        if ((a >= '0' && a <= '9') || a == '-' || a == '+' || a == '.' ||
            a == 'e' || a == 'E') {
            ascii.push_back(a);
            ++i;
            continue;
        }
        break;
    }
    if (ascii.empty()) return ScalarParse::Invalid;
    skip_ws();
    if (i != n) return ScalarParse::Invalid;
    char* end = nullptr;
    const double parsed = std::strtod(ascii.c_str(), &end);
    if (end == nullptr || *end != '\0') return ScalarParse::Invalid;
    *number = parsed;
    return ScalarParse::Number;
}

/// JSON number formatting: integral values print without a fractional part,
/// matching JsonValue::Stringify.
std::u16string json_number_to_string(double value) {
    char buffer[40];
    if (value == static_cast<double>(static_cast<long long>(value)) &&
        std::abs(value) < 1e15) {
        std::snprintf(buffer, sizeof(buffer), "%lld",
                      static_cast<long long>(value));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    }
    std::u16string out;
    for (const char* p = buffer; *p != '\0'; ++p) {
        out.push_back(static_cast<char16_t>(*p));
    }
    return out;
}

/// IJsonValueStatics::CreateNumberValue takes a DOUBLE. The Windows x64 ABI
/// puts it in XMM1 (its register slot, so the integer out parameter lands in
/// R8); the 32-bit ABI pushes it as two stack slots ahead of the out pointer.
double json_double_arg(Marshaler& m) {
    const auto* cpu = m.cpu();
    if (cpu == nullptr) return 0.0;
    if (m.cpu_is_64bit()) {
        double value = 0.0;
        const u64 bits = cpu->sse.xmm[1].u64_[0];
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    const u64 lo = m.arg(1);
    const u64 hi = m.arg(2);
    const u64 bits = (lo & 0xFFFFFFFFull) | (hi << 32);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::u16string json_quote(const std::u16string& value) {
    std::u16string out;
    out.push_back(u'"');
    for (const char16_t c : value) {
        switch (c) {
        case u'"': out += u"\\\""; break;
        case u'\\': out += u"\\\\"; break;
        case u'\n': out += u"\\n"; break;
        case u'\r': out += u"\\r"; break;
        case u'\t': out += u"\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back(u'"');
    return out;
}

} // namespace

GuestAddr CombaseManager::create_json_value(
    JsonType type, double number, bool boolean, std::u16string text,
    bool is_64bit) {
    Object object{};
    object.type = ObjectType::JsonValue;
    object.ref_count = 1;
    object.json_type = type;
    object.json_number = number;
    object.json_boolean = boolean;
    object.values.push_back(std::move(text));
    return allocate_object(std::move(object), is_64bit);
}

CombaseManager::Object* CombaseManager::json_value_arg(
    Marshaler& m, GuestAddr* out_param) {
    const GuestAddr self = m.arg_ptr(0);
    if (out_param != nullptr) {
        *out_param = m.arg_ptr(1);
        if (*out_param == 0) return nullptr;
    }
    auto it = m_objects.find(self);
    if (it == m_objects.end() ||
        it->second.type != ObjectType::JsonValue) {
        return nullptr;
    }
    return &it->second;
}

void CombaseManager::impl_JsonValueCreateNull(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const GuestAddr value =
        create_json_value(JsonType::Null, 0.0, false, u"null", m.cpu_is_64bit());
    if (value == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, value).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueCreateBoolean(Marshaler& m) {
    const bool input = (m.arg(1) & 0xFF) != 0;
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const GuestAddr value = create_json_value(
        JsonType::Boolean, 0.0, input, input ? u"true" : u"false",
        m.cpu_is_64bit());
    if (value == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, value).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueCreateNumber(Marshaler& m) {
    const double input = json_double_arg(m);
    const GuestAddr out = m.cpu_is_64bit() ? m.arg_ptr(2) : m.arg_ptr(3);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const GuestAddr value = create_json_value(
        JsonType::Number, input, false, json_number_to_string(input),
        m.cpu_is_64bit());
    if (value == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, value).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueCreateString(Marshaler& m) {
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr input = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    std::u16string text;
    if (input != 0) {
        auto read = read_hstring(input, is_64bit);
        if (!read.has_value()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        text = std::move(*read);
    }
    const GuestAddr value =
        create_json_value(JsonType::String, 0.0, false, text, is_64bit);
    if (value == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, value).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueParse(Marshaler& m) {
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr input = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    std::u16string text;
    if (input != 0) {
        auto read = read_hstring(input, is_64bit);
        if (read.has_value()) text = std::move(*read);
    }
    double number = 0.0;
    bool boolean = false;
    std::u16string str;
    const ScalarParse kind =
        classify_json_scalar(text, &number, &boolean, &str);
    if (kind == ScalarParse::Composite) {
        VITREO_TRACE(
            "[combase] JsonValue.Parse refused: object/array documents need "
            "JsonObject/JsonArray state that is not modelled\n");
        m.set_return(E_NOTIMPL);
        return;
    }
    if (kind == ScalarParse::Invalid) {
        // WinRT reports malformed JSON as E_INVALIDARG from Parse.
        m.set_return(E_INVALIDARG);
        return;
    }
    GuestAddr value = 0;
    switch (kind) {
    case ScalarParse::Null:
        value = create_json_value(JsonType::Null, 0.0, false, u"null", is_64bit);
        break;
    case ScalarParse::Boolean:
        value = create_json_value(JsonType::Boolean, 0.0, boolean,
                                  boolean ? u"true" : u"false", is_64bit);
        break;
    case ScalarParse::Number:
        value = create_json_value(JsonType::Number, number, false,
                                  json_number_to_string(number), is_64bit);
        break;
    case ScalarParse::String:
        value = create_json_value(JsonType::String, 0.0, false, str, is_64bit);
        break;
    default:
        break;
    }
    if (value == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, value).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueTryParse(Marshaler& m) {
    const GuestAddr result_out = m.arg_ptr(2);
    const GuestAddr succeeded_out = m.arg_ptr(3);
    if (succeeded_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_u8(succeeded_out, 0);
    if (result_out != 0) (void)m.write_ptr(result_out, 0);

    // Reuse Parse by shifting the out parameter into place.
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr input = m.arg_ptr(1);
    std::u16string text;
    if (input != 0) {
        auto read = read_hstring(input, is_64bit);
        if (read.has_value()) text = std::move(*read);
    }
    double number = 0.0;
    bool boolean = false;
    std::u16string str;
    const ScalarParse kind =
        classify_json_scalar(text, &number, &boolean, &str);
    if (kind == ScalarParse::Composite) {
        VITREO_TRACE(
            "[combase] JsonValue.TryParse refused: object/array documents need "
            "JsonObject/JsonArray state that is not modelled\n");
        m.set_return(E_NOTIMPL);
        return;
    }
    if (kind == ScalarParse::Invalid) {
        // Malformed input is not an error for TryParse: succeeded stays false.
        m.set_return(S_OK);
        return;
    }
    GuestAddr value = 0;
    switch (kind) {
    case ScalarParse::Null:
        value = create_json_value(JsonType::Null, 0.0, false, u"null", is_64bit);
        break;
    case ScalarParse::Boolean:
        value = create_json_value(JsonType::Boolean, 0.0, boolean,
                                  boolean ? u"true" : u"false", is_64bit);
        break;
    case ScalarParse::Number:
        value = create_json_value(JsonType::Number, number, false,
                                  json_number_to_string(number), is_64bit);
        break;
    case ScalarParse::String:
        value = create_json_value(JsonType::String, 0.0, false, str, is_64bit);
        break;
    default:
        break;
    }
    if (value == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (result_out != 0 && m.write_ptr(result_out, value).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_u8(succeeded_out, 1);
    m.set_return(S_OK);
}

void CombaseManager::impl_JsonValueGetValueType(Marshaler& m) {
    GuestAddr out = 0;
    Object* value = json_value_arg(m, &out);
    if (value == nullptr) {
        m.set_return(out == 0 ? E_POINTER : E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u32(out, static_cast<u32>(value->json_type)).is_ok()
                     ? S_OK
                     : E_POINTER);
}

void CombaseManager::impl_JsonValueStringify(Marshaler& m) {
    GuestAddr out = 0;
    Object* value = json_value_arg(m, &out);
    if (value == nullptr) {
        m.set_return(out == 0 ? E_POINTER : E_INVALIDARG);
        return;
    }
    const std::u16string raw =
        value->values.empty() ? std::u16string() : value->values[0];
    const std::u16string text =
        value->json_type == JsonType::String ? json_quote(raw) : raw;
    const GuestAddr hstring = create_hstring(text, m.cpu_is_64bit());
    m.set_return(m.write_ptr(out, hstring).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueGetString(Marshaler& m) {
    GuestAddr out = 0;
    Object* value = json_value_arg(m, &out);
    if (value == nullptr) {
        m.set_return(out == 0 ? E_POINTER : E_INVALIDARG);
        return;
    }
    if (value->json_type != JsonType::String) {
        m.set_return(E_ILLEGAL_METHOD_CALL);
        return;
    }
    const GuestAddr hstring = create_hstring(
        value->values.empty() ? std::u16string() : value->values[0],
        m.cpu_is_64bit());
    m.set_return(m.write_ptr(out, hstring).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueGetNumber(Marshaler& m) {
    GuestAddr out = 0;
    Object* value = json_value_arg(m, &out);
    if (value == nullptr) {
        m.set_return(out == 0 ? E_POINTER : E_INVALIDARG);
        return;
    }
    if (value->json_type != JsonType::Number) {
        m.set_return(E_ILLEGAL_METHOD_CALL);
        return;
    }
    u64 bits = 0;
    std::memcpy(&bits, &value->json_number, sizeof(bits));
    m.set_return(m.write_u64(out, bits).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_JsonValueGetBoolean(Marshaler& m) {
    GuestAddr out = 0;
    Object* value = json_value_arg(m, &out);
    if (value == nullptr) {
        m.set_return(out == 0 ? E_POINTER : E_INVALIDARG);
        return;
    }
    if (value->json_type != JsonType::Boolean) {
        m.set_return(E_ILLEGAL_METHOD_CALL);
        return;
    }
    m.set_return(
        m.write_u8(out, value->json_boolean ? 1u : 0u).is_ok() ? S_OK
                                                               : E_POINTER);
}

void CombaseManager::impl_JsonValueGetArray(Marshaler& m) {
    GuestAddr out = 0;
    (void)json_value_arg(m, &out);
    if (out != 0) (void)m.write_ptr(out, 0);
    // A scalar JsonValue is never an array, and composite values are refused
    // at Parse time, so this can only ever be the illegal-call case.
    m.set_return(out == 0 ? E_POINTER : E_ILLEGAL_METHOD_CALL);
}

void CombaseManager::impl_JsonValueGetObject(Marshaler& m) {
    GuestAddr out = 0;
    (void)json_value_arg(m, &out);
    if (out != 0) (void)m.write_ptr(out, 0);
    m.set_return(out == 0 ? E_POINTER : E_ILLEGAL_METHOD_CALL);
}

} // namespace vitreo::winapi
