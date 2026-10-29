#include "msi.hpp"
#include "winapi/system/msi/msi_database.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_register.hpp"
#include "winapi/system/msi/msi_install.hpp"
#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/system/msi/msi_service.hpp"
#include "core/compression/cabinet.hpp"
#include "capsule/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "capsule/vfs/vfs.hpp"

#include <array>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <set>
#include <vector>

namespace vitreo::winapi {

namespace {

/// Decode UTF-8 to UTF-16LE code units. MSI buffer sizes are counted in
/// characters (UTF-16 code units), so the copy-out path needs the real code
/// unit count, not the UTF-8 byte length.
std::vector<u16> to_utf16(const std::string& str) {
    std::vector<u16> out;
    out.reserve(str.size());

    usize i = 0;
    while (i < str.size()) {
        const u8 b0 = static_cast<u8>(str[i]);
        u32 cp = 0;

        if ((b0 & 0x80) == 0) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= str.size()) break;
            cp = ((b0 & 0x1Fu) << 6) | (static_cast<u8>(str[i + 1]) & 0x3Fu);
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= str.size()) break;
            cp = ((b0 & 0x0Fu) << 12) |
                 ((static_cast<u8>(str[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<u8>(str[i + 2]) & 0x3Fu);
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            if (i + 3 >= str.size()) break;
            cp = ((b0 & 0x07u) << 18) |
                 ((static_cast<u8>(str[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<u8>(str[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<u8>(str[i + 3]) & 0x3Fu);
            i += 4;
        } else {
            break;
        }

        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            out.push_back(static_cast<u16>(0xD800u + (cp >> 10)));
            out.push_back(static_cast<u16>(0xDC00u + (cp & 0x3FFu)));
        } else {
            out.push_back(static_cast<u16>(cp));
        }
    }
    return out;
}

/// A product code must be a registry-format GUID: {8-4-4-4-12} with braces
/// and hyphens, 38 characters. Windows rejects anything else with an argument
/// error rather than reporting the product as merely absent, and callers rely
/// on that distinction.
bool is_product_code(const std::string& s) {
    if (s.size() != 38) return false;
    if (s.front() != '{' || s.back() != '}') return false;

    static constexpr int kHyphens[] = {9, 14, 19, 24};
    for (usize i = 1; i + 1 < s.size(); ++i) {
        const bool want_hyphen =
            (i == kHyphens[0] || i == kHyphens[1] ||
             i == kHyphens[2] || i == kHyphens[3]);
        const char c = s[i];
        if (want_hyphen) {
            if (c != '-') return false;
        } else {
            const bool hex = (c >= '0' && c <= '9') ||
                             (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
    }
    return true;
}

} // namespace

std::optional<std::vector<u8>> MsiManager::read_guest_file(
    const std::string& path) const {
    if (m_vfs == nullptr) return std::nullopt;

    auto opened = m_vfs->open(path, capsule::AccessMode::Read,
                              capsule::ShareMode::Read,
                              capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return std::nullopt;

    std::vector<u8> bytes;
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto read = m_vfs->read(opened.value(), chunk);
        if (read.is_err() || read.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    (void)m_vfs->close(opened.value());

    return bytes;
}

bool MsiManager::ensure_parent_directories(const std::string& path) const {
    if (m_vfs == nullptr) return false;

    // Walk forward creating each level. create_dir on an existing directory is
    // not an error worth stopping for — an install re-runs over a tree it
    // partly created all the time.
    const usize last = path.find_last_of('\\');
    if (last == std::string::npos) return true;

    for (usize i = 0; i <= last; ++i) {
        if (path[i] != '\\' || i == 0) continue;
        const std::string level = path.substr(0, i);
        // "C:" is a drive, not a directory, and creating it is meaningless.
        if (level.size() <= 2) continue;
        (void)m_vfs->create_dir(level);
    }
    const std::string parent = path.substr(0, last);
    if (parent.size() <= 2) return true;
    (void)m_vfs->create_dir(parent);
    return true;
}

bool MsiManager::write_guest_file(const std::string& path,
                                  std::span<const u8> bytes) const {
    if (m_vfs == nullptr) return false;
    if (!ensure_parent_directories(path)) return false;

    auto opened = m_vfs->open(path, capsule::AccessMode::Write,
                              capsule::ShareMode::None,
                              capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return false;

    bool ok = true;
    usize written = 0;
    while (written < bytes.size()) {
        auto n = m_vfs->write(opened.value(), bytes.subspan(written));
        if (n.is_err() || n.value() == 0) { ok = false; break; }
        written += n.value();
    }
    (void)m_vfs->close(opened.value());
    return ok;
}

u32 MsiManager::write_sized_wide_string(Marshaler& m,
                                        GuestAddr buffer,
                                        GuestAddr pcch,
                                        const std::string& value) {
    // pcchValueBuf is mandatory whenever a buffer is supplied; without it the
    // callee cannot know the capacity and Windows rejects the call.
    if (pcch == 0) {
        return (buffer == 0) ? MSI_ERROR_SUCCESS : MSI_ERROR_INVALID_PARAMETER;
    }

    const std::vector<u16> units = to_utf16(value);
    const u32 required = static_cast<u32>(units.size()); // excludes terminator

    const auto cap_res = m.read_u32(pcch);
    if (cap_res.is_err()) return MSI_ERROR_INVALID_PARAMETER;
    const u32 capacity = cap_res.value(); // includes room for the terminator

    // Null buffer is the documented size query: report the required length
    // and copy nothing.
    if (buffer == 0) {
        (void)m.write_u32(pcch, required);
        return MSI_ERROR_SUCCESS;
    }

    if (capacity == 0 || required + 1 > capacity) {
        // MSI truncates into whatever room exists, still null-terminating,
        // and reports the size needed for the whole value.
        if (capacity > 0) {
            const u32 copy_units = capacity - 1;
            for (u32 k = 0; k < copy_units; ++k) {
                (void)m.write_u16(buffer + k * 2, units[k]);
            }
            (void)m.write_u16(buffer + copy_units * 2, 0);
        }
        (void)m.write_u32(pcch, required);
        return MSI_ERROR_MORE_DATA;
    }

    for (u32 k = 0; k < required; ++k) {
        (void)m.write_u16(buffer + k * 2, units[k]);
    }
    (void)m.write_u16(buffer + required * 2, 0);
    (void)m.write_u32(pcch, required);
    return MSI_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Record formatting (records themselves: msi_database.cpp)
// ---------------------------------------------------------------------------

void MsiManager::impl_MsiFormatRecordW(Marshaler& m) {
    // UINT MsiFormatRecordW(MSIHANDLE hInstall, MSIHANDLE hRecord,
    //                       LPWSTR szResultBuf, LPDWORD pcchResultBuf)
    //
    // Field 0 holds the template and the remaining fields substitute into it.
    // MS Learn documents three markers: [N] takes record field N, [[N]] is a
    // literal "[N]", and {...} is a block that disappears entirely when any
    // [N] inside it is null or missing — which is how MSI keeps optional
    // clauses from leaving stray punctuation behind.
    const u32       record_handle = static_cast<u32>(m.arg(1));
    const GuestAddr buffer        = m.arg_ptr(2);
    const GuestAddr pcch          = m.arg_ptr(3);

    const std::shared_ptr<msi::Record> rec = msi::record_of(*this, record_handle);
    if (rec == nullptr) {
        VITREO_TRACE("[msi] MsiFormatRecordW: invalid handle %u\n",
                     record_handle);
        m.set_return(MSI_ERROR_INVALID_HANDLE);
        return;
    }

    const std::string tmpl = msi::record_string(*rec, 0);

    // Resolves [N]; reports whether the field existed and was non-null so an
    // enclosing {} block knows to drop itself.
    const auto field_value = [&](const std::string& digits,
                                 bool& resolved) -> std::string {
        resolved = false;
        if (digits.empty()) return {};
        for (const char c : digits) {
            if (c < '0' || c > '9') return {};
        }
        const auto index = static_cast<usize>(std::stoul(digits));
        if (index == 0 || index > rec->count()) return {};
        if (msi::record_is_null(*rec, static_cast<u32>(index))) return {};
        resolved = true;
        return msi::record_string(*rec, static_cast<u32>(index));
    };

    // Expands one run of text, reporting whether every [N] in it resolved.
    const auto expand = [&](const std::string& text, bool& all_resolved) {
        std::string out;
        all_resolved = true;
        for (usize i = 0; i < text.size();) {
            if (text[i] != '[') { out.push_back(text[i++]); continue; }
            // "[[" introduces a literal bracket run: [[N]] renders as [N].
            if (i + 1 < text.size() && text[i + 1] == '[') {
                const auto close = text.find("]]", i + 2);
                if (close == std::string::npos) { out.push_back(text[i++]); continue; }
                out.push_back('[');
                out.append(text, i + 2, close - (i + 2));
                out.push_back(']');
                i = close + 2;
                continue;
            }
            const auto close = text.find(']', i + 1);
            if (close == std::string::npos) { out.push_back(text[i++]); continue; }
            const std::string token = text.substr(i + 1, close - i - 1);
            bool resolved = false;
            const std::string value = field_value(token, resolved);
            if (resolved) {
                out += value;
            } else {
                // A property or unknown field: Windows leaves the marker in
                // place rather than deleting text it cannot resolve.
                all_resolved = false;
                out.push_back('[');
                out += token;
                out.push_back(']');
            }
            i = close + 1;
        }
        return out;
    };

    std::string result;
    for (usize i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const auto close = tmpl.find('}', i + 1);
            if (close == std::string::npos) { result.push_back(tmpl[i++]); continue; }
            bool all_resolved = false;
            const std::string inner =
                expand(tmpl.substr(i + 1, close - i - 1), all_resolved);
            // The whole block vanishes unless every substitution inside it
            // resolved.
            if (all_resolved) result += inner;
            i = close + 1;
            continue;
        }
        // Expand the run up to the next block boundary in one go.
        const auto next = tmpl.find('{', i);
        const auto run_end = (next == std::string::npos) ? tmpl.size() : next;
        bool ignored = false;
        result += expand(tmpl.substr(i, run_end - i), ignored);
        i = run_end;
    }

    VITREO_TRACE("[msi] MsiFormatRecordW(handle %u) -> \"%s\"\n", record_handle,
                 result.c_str());
    m.set_return(write_sized_wide_string(m, buffer, pcch, result));
}

// ---------------------------------------------------------------------------
// Product query API
// ---------------------------------------------------------------------------

void MsiManager::impl_MsiQueryProductStateW(Marshaler& m) {
    const GuestAddr product_ptr = m.arg_ptr(0);

    if (product_ptr == 0) {
        m.set_return(static_cast<u32>(INSTALLSTATE_INVALIDARG));
        return;
    }

    auto product = m.read_wide_string(product_ptr);
    if (product.is_err() || !is_product_code(product.value())) {
        m.set_return(static_cast<u32>(INSTALLSTATE_INVALIDARG));
        return;
    }

    const i32 state = product_state(product.value());
    set_last_error(0);
    m.set_return(static_cast<u32>(state));
}

// ---------------------------------------------------------------------------
// Installed-product registration
//
// Windows Installer keeps what it has installed in the registry, which is what
// makes a product visible to a *different* process — and that matters here,
// because the install runs in an elevated child while the query often comes
// from its parent. Vitreo stores the same facts under the documented
// per-machine location so the state survives the process that created it.
//
// Use Windows' packed GUID keys and preserve registration context. Guest
// registry consumers and MSI APIs must observe the same product ownership.
// ---------------------------------------------------------------------------

namespace {

// Packed GUID involution measured by msi_product_state_probe.c against real
// MSI enumeration. The first three fields reverse nibbles; the remaining
// bytes swap nibbles. Both registration and lookup use this representation.
std::wstring packed_product(const std::string& code) {
    std::wstring packed;
    for (unsigned char c : code) if (std::isxdigit(c))
        packed.push_back(static_cast<wchar_t>(std::toupper(c)));
    if (packed.size()!=32) return {};
    std::reverse(packed.begin(),packed.begin()+8);
    std::reverse(packed.begin()+8,packed.begin()+12);
    std::reverse(packed.begin()+12,packed.begin()+16);
    for(size_t i=16;i<32;i+=2) std::swap(packed[i],packed[i+1]);
    return packed;
}

std::string unpacked_product(const std::wstring& packed) {
    if(packed.size()!=32) return {};
    std::string text;
    for(wchar_t c:packed) {
        if(c>127 || !std::isxdigit(static_cast<unsigned char>(c))) return {};
        text.push_back(static_cast<char>(c));
    }
    const std::wstring reversed=packed_product(text);
    std::string result="{";
    for(size_t i=0;i<32;++i) {
        if(i==8 || i==12 || i==16 || i==20) result.push_back('-');
        result.push_back(static_cast<char>(reversed[i]));
    }
    return result+"}";
}

[[nodiscard]] std::wstring widen_ascii(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const unsigned char c : text) out.push_back(static_cast<wchar_t>(c));
    return out;
}

[[nodiscard]] std::string narrow_ascii(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) out.push_back(static_cast<char>(c & 0xFF));
    return out;
}

} // namespace

// The installed-products database lives in the registration area
// (msi_register.hpp, measured layout); these members answer through it.

bool MsiManager::is_product_registered(const std::string& product_code) const {
    return product_state(product_code) == INSTALLSTATE_DEFAULT;
}

i32 MsiManager::product_state(const std::string& code) const {
    if (m_registry == nullptr) return INSTALLSTATE_UNKNOWN;
    return msi::ProductDatabase(*m_registry, narrow_ascii(m_user_sid)).product_state(code);
}

std::wstring MsiManager::install_properties_path(const std::string& code) const {
    if (m_registry == nullptr) return {};
    msi::ProductDatabase db(*m_registry, narrow_ascii(m_user_sid));
    auto inst = db.installed(code);
    if (!inst) return {};
    return db.keys(inst->context, inst->sid).user_data + L"\\Products\\" + widen_ascii(msi::pack_guid(code)) +
           L"\\InstallProperties";
}

std::string MsiManager::product_property(const std::string& product_code, const std::string& property) const {
    if (m_registry == nullptr) return {};
    msi::ProductDatabase db(*m_registry, narrow_ascii(m_user_sid));
    auto inst = db.installed(product_code);
    return inst ? db.install_property(*inst, property).value_or(std::string{}) : std::string{};
}

void MsiManager::set_product_property(const std::string& product_code, const std::string& property,
                                      const std::string& value) {
    if (m_registry == nullptr) return;
    const std::wstring path = install_properties_path(product_code);
    if (path.empty()) return;
    auto key = m_registry->open_key(
        m_registry->view_root(capsule::HKEY_LOCAL_MACHINE, capsule::RegView::Native64), path, true);
    if (key.is_err()) return;
    (void)m_registry->set_value(key.value(), widen_ascii(property),
                                capsule::RegValue{capsule::RegType::Sz, widen_ascii(value)});
    m_registry->close_key(key.value());
}

std::vector<std::string> MsiManager::registered_products() const {
    return advertised_products().value_or(std::vector<std::string>{});
}

std::optional<std::vector<std::string>> MsiManager::advertised_products(bool machine_only) const {
    if (m_registry == nullptr) return std::vector<std::string>{};
    std::set<std::string> codes;
    for (const auto& inst : msi::ProductDatabase(*m_registry, narrow_ascii(m_user_sid))
                                .instances(machine_only ? msi::kContextMachine : msi::kContextAll))
        codes.insert(inst.product_code);
    return std::vector<std::string>(codes.begin(), codes.end());
}

void MsiManager::impl_MsiEnumProductsW(Marshaler& m) {
    if (m.arg_ptr(1) == 0) { m.set_return(MSI_ERROR_INVALID_PARAMETER); return; }
    auto products = advertised_products();
    const u32 index = m.arg32(0);
    if (!products || index >= products->size()) { m.set_return(MSI_ERROR_NO_MORE_ITEMS); return; }
    m.set_return(m.write_wide_string(m.arg_ptr(1), (*products)[index]).is_ok() ? MSI_ERROR_SUCCESS
                                                                               : MSI_ERROR_INVALID_PARAMETER);
}

void MsiManager::impl_MsiEnumProductsA(Marshaler& m) {
    if (m.arg_ptr(1) == 0) { m.set_return(MSI_ERROR_INVALID_PARAMETER); return; }
    auto products = advertised_products();
    const u32 index = m.arg32(0);
    if (!products || index >= products->size()) { m.set_return(MSI_ERROR_NO_MORE_ITEMS); return; }
    m.set_return(m.write_ansi_string(m.arg_ptr(1), (*products)[index]).is_ok() ? MSI_ERROR_SUCCESS
                                                                               : MSI_ERROR_INVALID_PARAMETER);
}

// RegisterProduct / PublishProduct / PublishFeatures / ProcessComponents are
// standard actions now (msi_register.cpp); the pre-sequencer path that
// called this cannot register a product correctly without them.
void MsiManager::register_product(msi::Session& session, const std::string& package_path) {
    (void)session;
    (void)package_path;
    msi::log_unimplemented("RegisterProduct", "legacy register_product: run the sequenced registration actions");
}

void MsiManager::impl_MsiSourceListAddSourceExW(Marshaler& m) {
    // UINT MsiSourceListAddSourceExW(LPCWSTR szProductCodeOrPatchCode,
    //                                LPCWSTR szUserSid,
    //                                MSIINSTALLCONTEXT dwContext,
    //                                DWORD dwOptions, LPCWSTR szSource,
    //                                DWORD dwIndex)
    //
    // Records where a product's package can be found again (for repair and
    // on-demand install). Callers invoke it right after installing, and a
    // missing export here is fatal: the delay-load stub calls through NULL and
    // takes the whole elevated worker with it.
    const GuestAddr product_ptr = m.arg_ptr(0);
    const u32       context     = m.arg32(2);
    const GuestAddr source_ptr  = m.arg_ptr(4);

    if (product_ptr == 0 || source_ptr == 0) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }
    auto product = m.read_wide_string(product_ptr);
    if (product.is_err() || !is_product_code(product.value())) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }
    std::string source;
    if (auto s2 = m.read_wide_string(source_ptr); s2.is_ok()) source = s2.value();

    // The source list belongs to a registered product. Vitreo registers what
    // it installs, so an unknown product code is genuinely unknown rather than
    // merely unsupported.
    if (!is_product_registered(product.value())) {
        VITREO_TRACE("[msi] MsiSourceListAddSourceExW(%s) -> "
                     "ERROR_UNKNOWN_PRODUCT\n", product.value().c_str());
        m.set_return(MSI_ERROR_UNKNOWN_PRODUCT);
        return;
    }
    set_product_property(product.value(), "InstallSource", source);
    VITREO_TRACE("[msi] MsiSourceListAddSourceExW(%s, ctx=%u) <- '%s'\n",
                 product.value().c_str(), context, source.c_str());
    m.set_return(MSI_ERROR_SUCCESS);
}

void MsiManager::impl_MsiGetProductInfoExW(Marshaler& m) {
    // UINT MsiGetProductInfoExW(LPCWSTR szProductCode, LPCWSTR szUserSid,
    //                           MSIINSTALLCONTEXT dwContext,
    //                           LPCWSTR szProperty, LPWSTR szValue,
    //                           LPDWORD pcchValue)
    const GuestAddr product_ptr  = m.arg_ptr(0);
    const GuestAddr property_ptr = m.arg_ptr(3);
    const GuestAddr buffer       = m.arg_ptr(4);
    const GuestAddr pcch         = m.arg_ptr(5);

    if (product_ptr == 0 || property_ptr == 0) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }
    auto product = m.read_wide_string(product_ptr);
    auto property = m.read_wide_string(property_ptr);
    if (product.is_err() || property.is_err() ||
        !is_product_code(product.value())) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }
    const u32 context=m.arg32(2);
    if ((context!=1 && context!=2 && context!=4) ||
        (context==4 && m.arg_ptr(1)!=0) || (buffer!=0 && pcch==0)) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);return;
    }
    // The legacy implementation ignored SID/context and could disclose a
    // different registration instance. Property mapping and cross-user access
    // must be implemented together before this API can answer safely.
    static std::once_flag warning;
    std::call_once(warning,[]{std::fprintf(stderr,
        "[vitreo] UNIMPLEMENTED msi!MsiGetProductInfoExW: context-specific property mapping\n");});
    m.set_return(MSI_ERROR_FUNCTION_FAILED);
}

void MsiManager::impl_MsiEnumProductsExW(Marshaler& m) {
    // UINT MsiEnumProductsExW(LPCWSTR szProductCode, LPCWSTR szUserSid,
    //                         DWORD dwContext, DWORD dwIndex,
    //                         WCHAR szInstalledProductCode[39],
    //                         MSIINSTALLCONTEXT *pdwInstalledContext,
    //                         LPWSTR szSid, LPDWORD pcchSid)
    constexpr u32 kContextMachine = 4; // MSIINSTALLCONTEXT_MACHINE
    const GuestAddr filter_ptr  = m.arg_ptr(0);
    const u32       index       = m.arg32(3);
    const GuestAddr product_out = m.arg_ptr(4);
    const GuestAddr context_out = m.arg_ptr(5);
    const GuestAddr sid_out     = m.arg_ptr(6);
    const GuestAddr sid_len_ptr = m.arg_ptr(7);

    const u32 context=m.arg32(2);
    if(context==0 || (context & ~7u)!=0 ||
       (context==kContextMachine && m.arg_ptr(1)!=0) ||
       (sid_out!=0 && sid_len_ptr==0)) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);return;
    }
    std::string filter;
    if(filter_ptr!=0) {
        auto f=m.read_wide_string(filter_ptr);
        if(f.is_err() || !is_product_code(f.value())) {
            m.set_return(MSI_ERROR_INVALID_PARAMETER);return;
        }
        filter=unpacked_product(packed_product(f.value()));
    }
    if(context!=kContextMachine) {
        static std::once_flag warning;
        std::call_once(warning,[]{std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED msi!MsiEnumProductsExW: user contexts and SID access checks\n");});
        m.set_return(MSI_ERROR_FUNCTION_FAILED);return;
    }
    auto available=advertised_products(true);
    if(!available) {m.set_return(1610u /* ERROR_BAD_CONFIGURATION */);return;}
    const auto& products=*available;
    std::vector<std::string> matches;
    for (const auto& code : products) {
        if (filter.empty() || code == filter) matches.push_back(code);
    }
    if (index >= matches.size()) {
        m.set_return(MSI_ERROR_NO_MORE_ITEMS);
        return;
    }
    if(sid_out!=0) {
        auto capacity=m.read_u32(sid_len_ptr);
        if(capacity.is_err()) {m.set_return(MSI_ERROR_INVALID_PARAMETER);return;}
        if(capacity.value()==0) {
            (void)m.write_u32(sid_len_ptr,0);
            m.set_return(MSI_ERROR_MORE_DATA);return;
        }
    }
    const std::string& code = matches[index];
    if (product_out != 0) {
        // The buffer is a fixed 39 characters; no length is negotiated.
        for (usize i = 0; i < code.size(); ++i) {
            (void)m.write_u16(product_out + i * 2,
                              static_cast<u16>(code[i]));
        }
        (void)m.write_u16(product_out + code.size() * 2, 0);
    }
    if (context_out != 0) (void)m.write_u32(context_out, kContextMachine);
    // Per-machine installs have no user SID; Windows returns an empty string.
    if (sid_len_ptr != 0) {
        if (sid_out != 0) (void)m.write_u16(sid_out, 0);
        (void)m.write_u32(sid_len_ptr, 0);
    }
    VITREO_TRACE("[msi] MsiEnumProductsExW(index=%u) -> %s\n", index,
                 code.c_str());
    m.set_return(MSI_ERROR_SUCCESS);
}

void MsiManager::impl_MsiGetPatchInfoExW(Marshaler& m) {
    // UINT MsiGetPatchInfoExW(LPCWSTR szPatchCode, LPCWSTR szProductCode, ...)
    // Vitreo's engine applies no patches, so no patch is ever registered
    // against a product. ERROR_UNKNOWN_PATCH is the documented answer.
    constexpr u32 kUnknownPatch = 1646u;
    VITREO_TRACE("[msi] MsiGetPatchInfoExW -> ERROR_UNKNOWN_PATCH\n");
    m.set_return(kUnknownPatch);
}

void MsiManager::impl_MsiDetermineApplicablePatchesW(Marshaler& m) {
    // UINT MsiDetermineApplicablePatchesW(LPCWSTR szProductPackagePath,
    //                                     DWORD cPatchInfo,
    //                                     PMSIPATCHSEQUENCEINFOW pPatchInfo)
    //
    // Each MSIPATCHSEQUENCEINFOW carries an out `uStatus` and `dwOrder`. With
    // no patch support the honest result is that none apply, which must be
    // written into every element rather than left as the caller's stack
    // garbage: a caller reading uninitialised uStatus would apply a patch
    // Vitreo cannot sequence.
    const u32       count = m.arg32(1);
    const GuestAddr info  = m.arg_ptr(2);
    if (info != 0 && count != 0) {
        // MSIPATCHSEQUENCEINFOW: szPatchData (ptr), ePatchDataType (DWORD),
        // dwOrder (DWORD), uStatus (UINT). The leading pointer widens on x64.
        const u32 ptr_size = m.cpu_is_64bit() ? 8u : 4u;
        const u32 stride   = m.cpu_is_64bit() ? 24u : 16u;
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr entry = info + static_cast<GuestAddr>(i) * stride;
            (void)m.write_u32(entry + ptr_size + 4, 0xFFFFFFFFu); // dwOrder
            (void)m.write_u32(entry + ptr_size + 8,
                              MSI_ERROR_UNKNOWN_PRODUCT);        // uStatus
        }
    }
    VITREO_TRACE("[msi] MsiDetermineApplicablePatchesW(%u) -> none apply\n",
                 count);
    m.set_return(MSI_ERROR_SUCCESS);
}

void MsiManager::impl_MsiSetExternalUIW(Marshaler& m) {
    // INSTALLUI_HANDLERW MsiSetExternalUIW(INSTALLUI_HANDLERW puiHandler,
    //                                      DWORD dwMessageFilter,
    //                                      LPVOID pvContext)
    // Returns the previously registered handler so the caller can chain or
    // restore it, which is the whole reason callers keep the return value.
    const GuestAddr handler = m.arg_ptr(0);
    const u32       filter  = m.arg32(1);
    const GuestAddr context = m.arg_ptr(2);

    const GuestAddr previous = m_external_ui_handler;
    m_external_ui_handler = handler;
    m_external_ui_filter  = filter;
    m_external_ui_context = context;

    // Registration is real; what Vitreo does not yet do is *call* the handler
    // with progress records during an install, so a caller driving a progress
    // bar from these messages will see it stay put. The install itself still
    // reports its outcome through the MsiInstallProduct return value.
    VITREO_TRACE("[msi] MsiSetExternalUIW(handler=0x%llx filter=0x%x) -> "
                 "previous 0x%llx\n",
                 static_cast<unsigned long long>(handler), filter,
                 static_cast<unsigned long long>(previous));
    m.set_return(static_cast<u64>(previous));
}

void MsiManager::impl_MsiSetExternalUIRecord(Marshaler& m) {
    // UINT MsiSetExternalUIRecord(INSTALLUI_HANDLER_RECORD puiHandler,
    //                             DWORD dwMessageFilter, LPVOID pvContext,
    //                             PINSTALLUI_HANDLER_RECORD ppuiPrevHandler)
    // Unlike MsiSetExternalUIW this returns a status and hands the previous
    // handler back through an out-parameter.
    const GuestAddr handler  = m.arg_ptr(0);
    const u32       filter   = m.arg32(1);
    const GuestAddr context  = m.arg_ptr(2);
    const GuestAddr prev_out = m.arg_ptr(3);

    const GuestAddr previous = m_external_ui_record_handler;
    m_external_ui_record_handler = handler;
    m_external_ui_filter         = filter;
    m_external_ui_context        = context;

    if (prev_out != 0) {
        (void)m.write_ptr(prev_out, previous);
    }
    VITREO_TRACE("[msi] MsiSetExternalUIRecord(handler=0x%llx filter=0x%x)\n",
                 static_cast<unsigned long long>(handler), filter);
    m.set_return(MSI_ERROR_SUCCESS);
}

void MsiManager::impl_MsiEnableLogW(Marshaler& m) {
    // UINT MsiEnableLogW(DWORD dwLogMode, LPCWSTR szLogFile,
    //                    DWORD dwLogAttributes)
    // A NULL szLogFile turns logging off. Vitreo's engine traces through
    // VITREO_TRACE rather than writing an MSI log file, so record the request
    // and report success — the caller's own logging is unaffected either way.
    const u32       mode     = m.arg32(0);
    const GuestAddr file_ptr = m.arg_ptr(1);
    std::string path;
    if (file_ptr != 0) {
        if (auto p = m.read_wide_string(file_ptr); p.is_ok()) path = p.value();
    }
    VITREO_TRACE("[msi] MsiEnableLogW(mode=0x%x, \"%s\")\n", mode,
                 path.c_str());
    m.set_return(MSI_ERROR_SUCCESS);
}

void MsiManager::impl_MsiInstallProductW(Marshaler& m) {
    // UINT MsiInstallProductW(LPCWSTR szPackagePath, LPCWSTR szCommandLine)
    const GuestAddr package_ptr = m.arg_ptr(0);
    const GuestAddr cmdline_ptr = m.arg_ptr(1);
    if (package_ptr == 0) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }
    auto package = m.read_wide_string(package_ptr);
    if (package.is_err() || package.value().empty()) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }
    std::string properties;
    if (cmdline_ptr != 0) {
        if (auto c = m.read_wide_string(cmdline_ptr); c.is_ok()) {
            properties = c.value();
        }
    }

    // Reuse the msiexec engine rather than duplicating the install: it already
    // opens the package, resolves directories, honours LaunchConditions and
    // runs the file-copy half of InstallExecuteSequence out of the package's
    // cabinets. Quote the path so a package under "Program Files" parses as
    // one token.
    std::string command_line = "/i \"" + package.value() + "\"";
    if (!properties.empty()) {
        command_line += " ";
        command_line += properties;
    }
    VITREO_TRACE("[msi] MsiInstallProductW(\"%s\")\n", package.value().c_str());
    m.set_return(run_msiexec(command_line));
}

void MsiManager::impl_MsiSetInternalUI(Marshaler& m) {
    // INSTALLUILEVEL MsiSetInternalUI(INSTALLUILEVEL dwUILevel, HWND *phWnd)
    //
    // Selects how much UI the installer engine shows, and optionally the owner
    // window. Both the level and the owner are *exchanged*: the call returns
    // the previous level, and when phWnd is supplied the previous owner is
    // written back through it. Callers rely on that to restore the engine's
    // state afterwards, so neither can be dropped.
    constexpr u32 kInstallUiLevelNoChange = 0;
    const u32       level    = m.arg32(0);
    const GuestAddr hwnd_ptr = m.arg_ptr(1);

    const u32 previous_level = m_internal_ui_level;
    // INSTALLUILEVEL_NOCHANGE queries without modifying.
    if (level != kInstallUiLevelNoChange) {
        m_internal_ui_level = level;
    }

    if (hwnd_ptr != 0) {
        const GuestAddr previous_owner = m_internal_ui_owner;
        if (auto requested = m.read_ptr(hwnd_ptr); requested.is_ok()) {
            m_internal_ui_owner = static_cast<GuestAddr>(requested.value());
        }
        (void)m.write_ptr(hwnd_ptr, previous_owner);
    }

    VITREO_TRACE("[msi] MsiSetInternalUI(level=%u) -> previous %u\n", level,
                 previous_level);
    m.set_return(previous_level);
}

void MsiManager::impl_MsiEnumRelatedProductsW(Marshaler& m) {
    // UINT MsiEnumRelatedProductsW(LPCWSTR lpUpgradeCode, DWORD dwReserved,
    //                              DWORD iProductIndex, LPWSTR lpProductBuf)
    //
    // Enumerates the installed products that carry a given UpgradeCode, which
    // is how an installer finds an older version of itself to upgrade. The
    // caller's buffer must be at least 39 characters (a GUID plus its
    // terminator); MS Learn documents no way to ask for the required size,
    // because the size is fixed.
    const GuestAddr upgrade_ptr = m.arg_ptr(0);
    const u32       reserved    = m.arg32(1);
    const u32       index       = m.arg32(2);
    const GuestAddr buffer      = m.arg_ptr(3);

    if (upgrade_ptr == 0 || buffer == 0 || reserved != 0) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }
    auto upgrade = m.read_wide_string(upgrade_ptr);
    if (upgrade.is_err() || !is_product_code(upgrade.value())) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }

    // Nothing has been installed through the Windows Installer engine in this
    // capsule, so no product carries any upgrade code. ERROR_NO_MORE_ITEMS at
    // index 0 is the real Windows answer for an upgrade code with no installed
    // products — the same position MsiQueryProductStateW takes. Returning
    // ERROR_SUCCESS with an invented product code would tell an installer that
    // an older version is present and send it down an upgrade path.
    VITREO_TRACE("[msi] MsiEnumRelatedProductsW(%s, index=%u) -> "
                 "ERROR_NO_MORE_ITEMS\n", upgrade.value().c_str(), index);
    m.set_return(MSI_ERROR_NO_MORE_ITEMS);
}

void MsiManager::impl_MsiGetProductInfoW(Marshaler& m) {
    const GuestAddr product_ptr = m.arg_ptr(0);
    const GuestAddr attribute_ptr = m.arg_ptr(1);
    const GuestAddr buffer = m.arg_ptr(2);
    const GuestAddr pcch = m.arg_ptr(3);
    (void)buffer;
    (void)pcch;

    if (product_ptr == 0 || attribute_ptr == 0) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }

    auto product = m.read_wide_string(product_ptr);
    if (product.is_err() || !is_product_code(product.value())) {
        m.set_return(MSI_ERROR_INVALID_PARAMETER);
        return;
    }

    auto attribute = m.read_wide_string(attribute_ptr);
    const std::string attr_name = attribute.is_ok() ? attribute.value() : std::string{};

    // Consistent with MsiQueryProductStateW above: the product is not
    // installed, so there is no product information to return.
    VITREO_TRACE("[msi] MsiGetProductInfoW(%s, %s) -> ERROR_UNKNOWN_PRODUCT\n",
                 product.value().c_str(), attr_name.c_str());
    m.set_return(MSI_ERROR_UNKNOWN_PRODUCT);
}


// ---------------------------------------------------------------------------
// msiexec
// ---------------------------------------------------------------------------

// The engine glue (msi_sequence.cpp owns the Engine).

msi::MachineState MsiManager::machine_state() const {
    if (m_machine_state_set) return m_machine_state;
    return msi::default_machine_state(m_registry, m_user_sid);
}

msi::Services MsiManager::engine_services() {
    msi::Services s;
    s.vfs = m_vfs;
    s.registry = m_registry;
    s.manager = this;
    msi::CustomActionHost* host = msi::process_custom_action_host();
    s.custom_actions = host;
    s.ui = m_ui_host;
    s.guest_64bit = host != nullptr ? host->is_64bit() : true;
    return s;
}

std::optional<msi::Record> MsiManager::engine_legacy_record(u32 handle) {
    const std::shared_ptr<msi::Record> r = msi::record_of(*this, handle);
    if (r == nullptr) return std::nullopt;
    return *r;
}

std::shared_ptr<msi::Package> MsiManager::engine_database_package(u32 handle) {
    const std::shared_ptr<msi::DatabaseObject> db = msi::database_of(*this, handle);
    return db != nullptr ? db->package : nullptr;
}

u32 MsiManager::run_msiexec(const std::string& command_line_in) {
    VITREO_TRACE("[msiexec] %s\n", command_line_in.c_str());
    // A caller that passes only the arguments (no executable token).
    std::string command_line = command_line_in;
    if (!command_line.empty() && (command_line[0] == '/' || command_line[0] == '-')) {
        command_line = "msiexec.exe " + command_line;
    }

    // Learn "Error Codes": ERROR_INVALID_COMMAND_LINE for a line msiexec
    // cannot use.
    constexpr u32 kInvalidCommandLine = 1639;
    auto cmd = msi::parse_msiexec_command_line(command_line);
    if (!cmd.has_value() || !cmd->invalid.empty()) {
        VITREO_TRACE("[msiexec] invalid command line (%s) -> 1639\n", cmd ? cmd->invalid.c_str() : "");
        return kInvalidCommandLine;
    }

    std::map<std::string, std::string> properties = cmd->properties;
    std::string package_path = cmd->package;
    using Mode = msi::MsiexecCommand::Mode;
    switch (cmd->mode) {
        case Mode::Install: break;
        case Mode::Administrative: properties["ACTION"] = "ADMIN"; break;
        case Mode::Advertise: properties["ACTION"] = "ADVERTISE"; break;
        case Mode::Uninstall: properties["REMOVE"] = "ALL"; break;
        case Mode::Repair:
            properties["REINSTALL"] = "ALL";
            properties["REINSTALLMODE"] = cmd->repair_modes;
            break;
        case Mode::Patch:
            msi::log_unimplemented("msiexec", "/p /update: applying a patch package needs the patch engine");
            return MSI_ERROR_INSTALL_FAILURE;
        case Mode::RegisterServer:
        case Mode::UnregisterServer:
            msi::log_unimplemented("msiexec", "/y /z: DllRegisterServer of a module");
            return MSI_ERROR_INSTALL_FAILURE;
        case Mode::Help:
            msi::log_unimplemented("msiexec", "/? help dialog");
            return MSI_ERROR_INSTALL_FAILURE;
        case Mode::Unsupported:
            return kInvalidCommandLine;
    }
    if (package_path.empty()) return kInvalidCommandLine;
    if (!cmd->transforms.empty()) properties["TRANSFORMS"] = cmd->transforms;
    switch (cmd->restart) {
        case msi::MsiexecCommand::Restart::No: properties["REBOOT"] = "ReallySuppress"; break;
        case msi::MsiexecCommand::Restart::Force: properties["REBOOT"] = "Force"; break;
        case msi::MsiexecCommand::Restart::Prompt: properties["REBOOTPROMPT"] = ""; break;
        default: break;
    }

    // /x and /f name an installed product by its code: run its cached package.
    if ((cmd->mode == Mode::Uninstall || cmd->mode == Mode::Repair) && package_path.size() == 38 &&
        package_path.front() == '{') {
        if (!is_product_registered(package_path)) return MSI_ERROR_UNKNOWN_PRODUCT;
        const std::string local = product_property(package_path, "LocalPackage");
        if (local.empty()) return MSI_ERROR_UNKNOWN_PRODUCT;
        package_path = local;
    }

    // msiexec /i is a client like any MsiInstallProduct caller: the install
    // runs in the Windows Installer service (msi_service.hpp).
    msi::InstallInvocation inv;
    inv.package_path = msi::absolute_package_path(*this, package_path);
    auto bytes = read_guest_file(inv.package_path);
    if (!bytes.has_value()) return MSI_ERROR_INSTALL_PACKAGE_OPEN_FAILED;
    auto package = msi::Package::open(std::span<const u8>(bytes->data(), bytes->size()));
    if (!package.has_value()) return MSI_ERROR_INSTALL_PACKAGE_INVALID;
    inv.package = std::make_shared<msi::Package>(std::move(*package));
    inv.ui_level = cmd->ui_level;
    inv.properties = properties;
    const std::string action_property = properties.count("ACTION") ? properties["ACTION"] : std::string{};
    inv.action = action_property == "ADMIN" || action_property == "ADVERTISE" ? action_property : std::string("INSTALL");
    if (!cmd->log_file.empty()) {
        inv.log_file = msi::absolute_package_path(*this, cmd->log_file);
        // /l letters -> INSTALLLOGMODE bits (Msi.h; Learn "Command-Line Options").
        u32 mode = 0;
        for (const char c : cmd->log_modes) {
            switch (c) {
                case 'i': mode |= 1u << 4; break;   // INFO
                case 'w': mode |= 1u << 2; break;   // WARNING
                case 'e': mode |= 1u << 1; break;   // ERROR
                case 'a': mode |= 1u << 8; break;   // ACTIONSTART
                case 'r': mode |= 1u << 9; break;   // ACTIONDATA
                case 'u': mode |= 1u << 3; break;   // USER
                case 'c': mode |= 1u << 11; break;  // COMMONDATA
                case 'm': mode |= 1u << 0; break;   // FATALEXIT (out of memory)
                case 'o': mode |= 1u << 7; break;   // OUTOFDISKSPACE
                case 'p': mode |= 1u << 10; break;  // PROPERTYDUMP
                case 'v': mode |= 1u << 12; break;  // VERBOSE
                case 'x': mode |= 1u << 13; break;  // EXTRADEBUG
                case '*': mode |= (1u << 4) | (1u << 2) | (1u << 1) | (1u << 8) | (1u << 9) | (1u << 3) |
                                  (1u << 11) | (1u << 0) | (1u << 7) | (1u << 10) | (1u << 6); break;
                case '+': inv.log_append = true; break;
                case '!': inv.log_flush = true; break;
                default: break;
            }
        }
        // "/l file" with no letters logs "iwearmo" (Learn).
        if (mode == 0) mode = (1u << 4) | (1u << 2) | (1u << 1) | (1u << 8) | (1u << 9) | (1u << 0) | (1u << 7);
        inv.log_mode = mode;
    }

    u32 rc = msi::install_package(*this, inv);
    VITREO_TRACE("[msiexec] %s -> %u\n", inv.action.c_str(), rc);
    if (rc == MSI_ERROR_SUCCESS && cmd->restart == msi::MsiexecCommand::Restart::Force) {
        msi::log_unimplemented("msiexec", "/forcerestart: restarting the system");
        rc = 3010;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// InstallFiles
//
// The file-copy half of InstallExecuteSequence. The other standard actions —
// registry, shortcuts, custom actions — are separate and are not run here; a
// package that needs them is installed short, which install_files() reports
// rather than papering over.
// ---------------------------------------------------------------------------

u32 MsiManager::install_files(msi::Session& session, const std::string& package_path) {
    // SourceDir is the directory the package sits in. Uncompressed payload and
    // external cabinets are both resolved against it, so getting it wrong
    // makes every non-embedded file unreachable at once.
    std::string source_dir = package_path;
    const usize slash = source_dir.find_last_of('\\');
    source_dir = (slash == std::string::npos) ? std::string{} : source_dir.substr(0, slash);
    session.set_property("SourceDir", source_dir + "\\");
    session.set_property("OriginalDatabase", package_path);
    session.set_property("DATABASE", package_path);

    std::vector<std::string> unresolved;
    const std::vector<msi::PlannedFile> plan =
        msi::plan_files(session, source_dir, unresolved);

    VITREO_TRACE("[msiexec] plan: %zu file(s) to install, %zu unresolved, "
                 "source dir '%s'\n",
                 plan.size(), unresolved.size(), source_dir.c_str());
    for (const std::string& problem : unresolved) {
        VITREO_TRACE("[msiexec]   unresolved: %s\n", problem.c_str());
    }
    if (plan.empty()) {
        VITREO_TRACE("[msiexec] nothing to install -> 1603\n");
        return MSI_ERROR_INSTALL_FAILURE;
    }

    // Group compressed files by cabinet. A cabinet folder is the unit of
    // decompression — its blocks share one history — so extracting files one
    // at a time would re-decode the whole folder per file, which for this
    // scale of payload is the difference between minutes and hours.
    std::map<std::string, std::vector<const msi::PlannedFile*>> by_cabinet;
    usize copied = 0;
    usize failed = 0;

    for (const msi::PlannedFile& file : plan) {
        if (!file.cabinet.empty()) {
            by_cabinet[file.cabinet].push_back(&file);
            continue;
        }
        auto bytes = read_guest_file(file.source_path);
        if (!bytes.has_value()) {
            VITREO_TRACE("[msiexec] missing source '%s' for %s\n",
                         file.source_path.c_str(), file.key.c_str());
            ++failed;
            continue;
        }
        if (!write_guest_file(file.target_path,
                              std::span<const u8>(bytes->data(), bytes->size()))) {
            VITREO_TRACE("[msiexec] failed to write '%s'\n", file.target_path.c_str());
            ++failed;
            continue;
        }
        ++copied;
    }

    for (const auto& [cabinet_name, members] : by_cabinet) {
        // A leading '#' names a stream inside the package; anything else is a
        // file beside it.
        std::vector<u8> image;
        if (cabinet_name.front() == '#') {
            const std::vector<u8>* embedded =
                session.package()->stream(cabinet_name.substr(1));
            if (embedded == nullptr) {
                VITREO_TRACE("[msiexec] embedded cabinet '%s' absent from package\n",
                             cabinet_name.c_str());
                failed += members.size();
                continue;
            }
            image = *embedded;
        } else {
            auto bytes = read_guest_file(source_dir + "\\" + cabinet_name);
            if (!bytes.has_value()) {
                VITREO_TRACE("[msiexec] cabinet '%s' not found beside package\n",
                             cabinet_name.c_str());
                failed += members.size();
                continue;
            }
            image = std::move(*bytes);
        }

        auto parsed = compression::cabinet::Cabinet::parse(
            std::span<const u8>(image.data(), image.size()));
        if (parsed.is_err()) {
            VITREO_TRACE("[msiexec] cabinet '%s' is not a valid MSCF container\n",
                         cabinet_name.c_str());
            failed += members.size();
            continue;
        }
        const compression::cabinet::Cabinet& cab = parsed.value();

        // Decompress each folder once, then slice every wanted file out of it.
        std::map<usize, std::vector<const compression::cabinet::FileEntry*>> by_folder;
        std::map<std::string, const msi::PlannedFile*> wanted;
        for (const msi::PlannedFile* file : members) wanted.emplace(file->key, file);
        for (const compression::cabinet::FileEntry& entry : cab.files()) {
            if (wanted.count(entry.name) == 0) continue;
            by_folder[entry.folder_index].push_back(&entry);
        }

        for (const auto& [folder_index, entries] : by_folder) {
            auto folder = cab.extract_folder(folder_index);
            if (folder.is_err()) {
                VITREO_TRACE("[msiexec] cabinet '%s' folder %zu failed to "
                             "decompress\n", cabinet_name.c_str(), folder_index);
                failed += entries.size();
                continue;
            }
            const std::vector<u8>& data = folder.value();
            for (const compression::cabinet::FileEntry* entry : entries) {
                auto slice = compression::cabinet::Cabinet::slice_file(
                    *entry, std::span<const u8>(data.data(), data.size()));
                if (slice.is_err()) { ++failed; continue; }
                const msi::PlannedFile* file = wanted[entry->name];
                VITREO_TRACE("[msiexec]   -> '%s' (%zu bytes)\n",
                             file->target_path.c_str(), slice.value().size());
                if (!write_guest_file(file->target_path, slice.value())) {
                    VITREO_TRACE("[msiexec] failed to write '%s'\n",
                                 file->target_path.c_str());
                    ++failed;
                    continue;
                }
                ++copied;
            }
        }

        // An entry the cabinet simply does not carry is a package/media
        // mismatch, not a write failure, and is worth naming separately.
        for (const msi::PlannedFile* file : members) {
            if (cab.find_file(file->key) == nullptr) {
                VITREO_TRACE("[msiexec] '%s' not present in cabinet '%s'\n",
                             file->key.c_str(), cabinet_name.c_str());
                ++failed;
            }
        }
    }

    VITREO_TRACE("[msiexec] installed %zu file(s), %zu failed\n", copied, failed);
    if (failed != 0) return MSI_ERROR_INSTALL_FAILURE;
    return MSI_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Registration
//
// This installer imports msi.dll purely by ordinal, so each entry carries its
// real export ordinal. Ordinals are the historical .def order and are stable
// for this Windows Installer 1.0-era range; the stdcall byte counts are the
// documented prototypes, every argument being 4 bytes in 32-bit mode
// (MSIHANDLE is a ULONG and INSTALLSTATE is an enum, neither is 64-bit).
// ---------------------------------------------------------------------------

void msi::register_legacy(MsiManager& mgr, msi::HandlerMap& map) {
    // The pre-engine surface. Ordinals and x86 cleanup bytes now come from
    // the generated export table (msi_export_table.inc); an area that takes
    // over one of these names replaces it.
#define MSI_RECORD_READ_EXPORT(name, ordinal, bytes) \
    map[#name] = [&mgr](Marshaler& m) { mgr.impl_##name(m); };
#include "msi_product_exports.inc"
#undef MSI_RECORD_READ_EXPORT
    map["MsiGetProductInfoW"] = [&mgr](Marshaler& m) { mgr.impl_MsiGetProductInfoW(m); };
    map["MsiQueryProductStateW"] = [&mgr](Marshaler& m) { mgr.impl_MsiQueryProductStateW(m); };
    map["MsiFormatRecordW"] = [&mgr](Marshaler& m) { mgr.impl_MsiFormatRecordW(m); };
    map["MsiEnumRelatedProductsW"] = [&mgr](Marshaler& m) { mgr.impl_MsiEnumRelatedProductsW(m); };
    map["MsiSetInternalUI"] = [&mgr](Marshaler& m) { mgr.impl_MsiSetInternalUI(m); };
    map["MsiSetExternalUIW"] = [&mgr](Marshaler& m) { mgr.impl_MsiSetExternalUIW(m); };
    map["MsiSetExternalUIRecord"] = [&mgr](Marshaler& m) { mgr.impl_MsiSetExternalUIRecord(m); };
    map["MsiEnableLogW"] = [&mgr](Marshaler& m) { mgr.impl_MsiEnableLogW(m); };
    map["MsiInstallProductW"] = [&mgr](Marshaler& m) { mgr.impl_MsiInstallProductW(m); };
    map["MsiSourceListAddSourceExW"] = [&mgr](Marshaler& m) { mgr.impl_MsiSourceListAddSourceExW(m); };
    map["MsiGetProductInfoExW"] = [&mgr](Marshaler& m) { mgr.impl_MsiGetProductInfoExW(m); };
    map["MsiEnumProductsExW"] = [&mgr](Marshaler& m) { mgr.impl_MsiEnumProductsExW(m); };
    map["MsiGetPatchInfoExW"] = [&mgr](Marshaler& m) { mgr.impl_MsiGetPatchInfoExW(m); };
    map["MsiDetermineApplicablePatchesW"] = [&mgr](Marshaler& m) { mgr.impl_MsiDetermineApplicablePatchesW(m); };
}

} // namespace vitreo::winapi
