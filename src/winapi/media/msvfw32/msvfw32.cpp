// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo — msvfw32.dll (Video for Windows ICM)
//
// ICInfo / ICOpen / ICClose / ICSendMessage:
// https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-icinfo
// https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-icopen
// https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-icclose
// https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-icsendmessage

#include "msvfw32.hpp"
#include "capsule/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace vitreo::winapi {

namespace {

constexpr i32 kIcerrOk = 0;
constexpr i32 kIcerrUnsupported = -1;
constexpr i32 kIcerrBadHandle = -8;
constexpr u32 kIcversion = 0x0104;
constexpr u32 kIcinfoSize = 568; // ICINFO: 6*DWORD + WCHAR[16+128+128]
constexpr u32 kDrvLoad = 1;
constexpr u32 kDrvEnable = 2;
constexpr u32 kDrvOpen = 3;
constexpr u32 kDrvClose = 4;
constexpr u32 kDrvDisable = 5;
constexpr u32 kDrvFree = 6;
constexpr GuestAddr kSentinelDriverProc = 0xBEEF0001;
constexpr u32 kIcopenSize32 = 36;
constexpr u32 kIcopenSize64 = 56;

bool write_icopen(Marshaler& m, GuestAddr dest, u32 fcc_type, u32 fcc_handler,
                  u32 mode) {
    const bool is64 = m.cpu_is_64bit();
    const u32 size = is64 ? kIcopenSize64 : kIcopenSize32;
    if (!m.write_u32(dest + 0, size).is_ok()) {
        return false;
    }
    if (!m.write_u32(dest + 4, fcc_type).is_ok()) {
        return false;
    }
    if (!m.write_u32(dest + 8, fcc_handler).is_ok()) {
        return false;
    }
    if (!m.write_u32(dest + 12, kIcversion).is_ok()) {
        return false;
    }
    if (!m.write_u32(dest + 16, mode).is_ok()) {
        return false;
    }
    if (is64) {
        if (!m.write_u32(dest + 20, 0).is_ok()) {
            return false;
        }
        if (!m.write_u64(dest + 24, 0).is_ok()) {
            return false;
        }
        if (!m.write_ptr(dest + 32, 0).is_ok()) {
            return false;
        }
        if (!m.write_ptr(dest + 40, 0).is_ok()) {
            return false;
        }
        if (!m.write_u32(dest + 48, 0).is_ok()) {
            return false;
        }
    } else {
        if (!m.write_u32(dest + 20, 0).is_ok()) {
            return false;
        }
        if (!m.write_ptr(dest + 24, 0).is_ok()) {
            return false;
        }
        if (!m.write_ptr(dest + 28, 0).is_ok()) {
            return false;
        }
        if (!m.write_u32(dest + 32, 0).is_ok()) {
            return false;
        }
    }
    return true;
}

std::string fourcc_ascii(u32 fcc) {
    char buf[5] = {
        static_cast<char>(fcc & 0xFF),
        static_cast<char>((fcc >> 8) & 0xFF),
        static_cast<char>((fcc >> 16) & 0xFF),
        static_cast<char>((fcc >> 24) & 0xFF),
        0,
    };
    return std::string(buf);
}

std::string ascii_from_wide(std::wstring_view ws) {
    std::string out;
    out.reserve(ws.size());
    for (wchar_t c : ws) {
        if (c == 0) {
            break;
        }
        out.push_back(static_cast<char>(c & 0xFF));
    }
    return out;
}

void ascii_to_utf16(u16* dest, usize count, std::string_view src) {
    std::memset(dest, 0, count * sizeof(u16));
    const usize n = std::min(count > 0 ? count - 1 : 0, src.size());
    for (usize i = 0; i < n; ++i) {
        dest[i] = static_cast<u16>(static_cast<unsigned char>(src[i]));
    }
}

bool name_matches_type(std::string_view value_name, u32 fcc_type) {
    if (value_name.size() < 6 || value_name[4] != '.') {
        return false;
    }
    if (fcc_type == 0) {
        return true;
    }
    const std::string type = fourcc_ascii(fcc_type);
    if (value_name.size() < type.size() + 1) {
        return false;
    }
    for (usize i = 0; i < type.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value_name[i])) !=
            std::tolower(static_cast<unsigned char>(type[i]))) {
            return false;
        }
    }
    return value_name[type.size()] == '.';
}

u32 fourcc_from_suffix(std::string_view value_name) {
    const auto dot = value_name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= value_name.size()) {
        return 0;
    }
    u32 fcc = 0;
    const usize n = std::min<usize>(4, value_name.size() - (dot + 1));
    for (usize i = 0; i < n; ++i) {
        fcc |= static_cast<u32>(static_cast<unsigned char>(value_name[dot + 1 + i]))
               << (8 * i);
    }
    return fcc;
}

} // namespace

void Msvfw32Manager::add_driver(u32 fcc_type, u32 fcc_handler, std::string dll_name) {
    std::lock_guard lock(mutex_);
    for (auto& d : seeded_) {
        if (d.fcc_type == fcc_type && d.fcc_handler == fcc_handler) {
            d.dll_name = std::move(dll_name);
            return;
        }
    }
    seeded_.push_back(VfwDriver{fcc_type, fcc_handler, std::move(dll_name)});
}

std::vector<VfwDriver> Msvfw32Manager::collect_drivers(u32 fcc_type) {
    std::vector<VfwDriver> out;
    {
        std::lock_guard lock(mutex_);
        for (const auto& d : seeded_) {
            if (fcc_type == 0 || d.fcc_type == fcc_type) {
                out.push_back(d);
            }
        }
    }
    if (registry_ == nullptr) {
        return out;
    }
    auto key = registry_->open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", false);
    if (key.is_err()) {
        return out;
    }
    auto names = registry_->enum_values(key.value());
    if (names.is_ok()) {
        for (const auto& wname : names.value()) {
            const std::string name = ascii_from_wide(wname);
            if (!name_matches_type(name, fcc_type)) {
                continue;
            }
            auto val = registry_->query_value(key.value(), wname);
            if (val.is_err() ||
                (val.value().type != capsule::RegType::Sz &&
                 val.value().type != capsule::RegType::ExpandSz)) {
                continue;
            }
            const auto* ws = std::get_if<std::wstring>(&val.value().data);
            if (ws == nullptr || ws->empty()) {
                continue;
            }
            const u32 handler = fourcc_from_suffix(name);
            const u32 type = fcc_type != 0
                                 ? fcc_type
                                 : (static_cast<u32>(static_cast<unsigned char>(name[0])) |
                                    (static_cast<u32>(static_cast<unsigned char>(name[1])) << 8) |
                                    (static_cast<u32>(static_cast<unsigned char>(name[2])) << 16) |
                                    (static_cast<u32>(static_cast<unsigned char>(name[3])) << 24));
            VfwDriver d{type, handler, ascii_from_wide(*ws)};
            const bool dup = std::any_of(out.begin(), out.end(), [&](const VfwDriver& e) {
                return e.fcc_type == d.fcc_type && e.fcc_handler == d.fcc_handler;
            });
            if (!dup) {
                out.push_back(std::move(d));
            }
        }
    }
    registry_->close_key(key.value());
    return out;
}

bool Msvfw32Manager::write_icinfo(Marshaler& m, GuestAddr dest, const VfwDriver& driver) {
    if (dest == 0) {
        return false;
    }
    auto size_r = m.read_u32(dest);
    const u32 size = size_r.is_ok() && size_r.value() != 0 ? size_r.value() : kIcinfoSize;
    if (size < 24) {
        return false;
    }
    (void)m.write_u32(dest + 0, kIcinfoSize);
    (void)m.write_u32(dest + 4, driver.fcc_type);
    (void)m.write_u32(dest + 8, driver.fcc_handler);
    (void)m.write_u32(dest + 12, 0);
    (void)m.write_u32(dest + 16, 0);
    (void)m.write_u32(dest + 20, kIcversion);
    if (size < kIcinfoSize) {
        return true;
    }
    u16 name[16]{};
    u16 desc[128]{};
    u16 dll[128]{};
    const std::string handler = fourcc_ascii(driver.fcc_handler);
    ascii_to_utf16(name, 16, handler);
    ascii_to_utf16(desc, 128, driver.dll_name);
    ascii_to_utf16(dll, 128, driver.dll_name);
    (void)m.memory().write_bytes(dest + 24, reinterpret_cast<const u8*>(name),
                                 sizeof(name));
    (void)m.memory().write_bytes(dest + 24 + 32, reinterpret_cast<const u8*>(desc),
                                 sizeof(desc));
    (void)m.memory().write_bytes(dest + 24 + 32 + 256,
                                 reinterpret_cast<const u8*>(dll), sizeof(dll));
    return true;
}

u64 Msvfw32Manager::call_driver_proc(const GuestCall& call, GuestAddr driver_proc,
                                     u64 dw_driver_id, u32 hdrvr, u32 msg,
                                     u64 lparam1, u64 lparam2,
                                     bool is_64bit) const {
    const u64 args[5] = {dw_driver_id, hdrvr, msg, lparam1, lparam2};
    return call(driver_proc, args, 5, is_64bit);
}

void Msvfw32Manager::abandon_open(u32 hic, const std::string& dll_name) {
    std::lock_guard lock(mutex_);
    open_.erase(hic);
    auto it = dll_opens_.find(dll_name);
    if (it == dll_opens_.end()) {
        return;
    }
    if (it->second <= 1) {
        dll_opens_.erase(it);
    } else {
        --it->second;
    }
}

u32 Msvfw32Manager::open_hic(Marshaler& m, u32 fcc_type, u32 fcc_handler,
                             u32 mode) {
    auto drivers = collect_drivers(fcc_type);
    const VfwDriver* found = nullptr;
    for (const auto& d : drivers) {
        if (d.fcc_handler == fcc_handler &&
            (fcc_type == 0 || d.fcc_type == fcc_type)) {
            found = &d;
            break;
        }
    }
    if (found == nullptr) {
        return 0;
    }

    GuestAddr driver_proc = 0;
    if (modules_ != nullptr) {
        const GuestAddr base = modules_->load_library(found->dll_name);
        if (base == 0) {
            return 0;
        }
        driver_proc = modules_->get_proc_address(base, "DriverProc");
        if (driver_proc == 0) {
            return 0;
        }
    }

    GuestCall call;
    u32 hic = 0;
    bool first_open = false;
    OpenHic pending;
    pending.driver = *found;
    pending.dll_name = found->dll_name;
    {
        std::lock_guard lock(mutex_);
        call = guest_call_;
        if (!call) {
            return 0;
        }
        if (modules_ == nullptr) {
            driver_proc = kSentinelDriverProc;
        }
        pending.driver_proc = driver_proc;
        hic = next_hic_++;
        first_open = (dll_opens_[pending.dll_name] == 0);
        dll_opens_[pending.dll_name]++;
        open_.emplace(hic, pending);
    }

    if (!m.has_cpu() || m.cpu()->sp() < 0x80) {
        abandon_open(hic, pending.dll_name);
        return 0;
    }

    const GuestAddr icopen = static_cast<GuestAddr>(m.cpu()->sp() - 0x80);
    if (!write_icopen(m, icopen, fcc_type, fcc_handler, mode)) {
        abandon_open(hic, pending.dll_name);
        return 0;
    }

    const bool is64 = m.cpu_is_64bit();
    if (first_open) {
        // Driver Instances: DRV_LOAD returning 0 unloads immediately.
        if (call_driver_proc(call, driver_proc, 0, hic, kDrvLoad, 0, 0, is64) ==
            0) {
            abandon_open(hic, pending.dll_name);
            return 0;
        }
        (void)call_driver_proc(call, driver_proc, 0, hic, kDrvEnable, 0, 0, is64);
    }
    const u64 dw_id =
        call_driver_proc(call, driver_proc, 0, hic, kDrvOpen, 0, icopen, is64);
    if (dw_id == 0) {
        if (first_open) {
            (void)call_driver_proc(call, driver_proc, 0, hic, kDrvDisable, 0, 0,
                                   is64);
            (void)call_driver_proc(call, driver_proc, 0, hic, kDrvFree, 0, 0,
                                   is64);
        }
        abandon_open(hic, pending.dll_name);
        return 0;
    }
    {
        std::lock_guard lock(mutex_);
        auto it = open_.find(hic);
        if (it == open_.end()) {
            return 0;
        }
        it->second.dw_driver_id = dw_id;
    }
    return hic;
}

void Msvfw32Manager::impl_ICInfo(Marshaler& m) {
    // BOOL ICInfo(DWORD fccType, DWORD fccHandler, ICINFO *lpicinfo)
    const u32 fcc_type = static_cast<u32>(m.arg(0));
    const u32 fcc_handler = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    if (info == 0) {
        m.set_return(0);
        return;
    }
    auto drivers = collect_drivers(fcc_type);
    const VfwDriver* found = nullptr;
    if ((fcc_handler & 0xFFFF0000u) == 0) {
        if (fcc_handler < drivers.size()) {
            found = &drivers[fcc_handler];
        }
    } else {
        for (const auto& d : drivers) {
            if (d.fcc_handler == fcc_handler) {
                found = &d;
                break;
            }
        }
    }
    if (found == nullptr) {
        m.set_return(0);
        return;
    }
    m.set_return(write_icinfo(m, info, *found) ? 1 : 0);
}

void Msvfw32Manager::impl_ICOpen(Marshaler& m) {
    // HIC ICOpen(DWORD fccType, DWORD fccHandler, UINT wMode)
    const u32 fcc_type = static_cast<u32>(m.arg(0));
    const u32 fcc_handler = static_cast<u32>(m.arg(1));
    const u32 mode = static_cast<u32>(m.arg(2));
    m.set_return(open_hic(m, fcc_type, fcc_handler, mode));
}

void Msvfw32Manager::impl_ICClose(Marshaler& m) {
    // LRESULT ICClose(HIC hic) — ICERR_OK or error
    const u32 hic = static_cast<u32>(m.arg(0));
    GuestCall call;
    OpenHic handle;
    bool last = false;
    {
        std::lock_guard lock(mutex_);
        auto it = open_.find(hic);
        if (it == open_.end()) {
            m.set_return(static_cast<u32>(kIcerrBadHandle));
            return;
        }
        call = guest_call_;
        handle = it->second;
        auto count_it = dll_opens_.find(handle.dll_name);
        last = (count_it != dll_opens_.end() && count_it->second == 1);
    }
    if (call) {
        const bool is64 = m.cpu_is_64bit();
        (void)call_driver_proc(call, handle.driver_proc, handle.dw_driver_id,
                               hic, kDrvClose, 0, 0, is64);
        if (last) {
            (void)call_driver_proc(call, handle.driver_proc, 0, hic,
                                   kDrvDisable, 0, 0, is64);
            (void)call_driver_proc(call, handle.driver_proc, 0, hic, kDrvFree,
                                   0, 0, is64);
        }
    }
    {
        std::lock_guard lock(mutex_);
        open_.erase(hic);
        auto count_it = dll_opens_.find(handle.dll_name);
        if (count_it != dll_opens_.end()) {
            if (count_it->second <= 1) {
                dll_opens_.erase(count_it);
            } else {
                --count_it->second;
            }
        }
    }
    m.set_return(static_cast<u32>(kIcerrOk));
}

void Msvfw32Manager::impl_ICSendMessage(Marshaler& m) {
    // LRESULT ICSendMessage(HIC, UINT msg, DWORD_PTR, DWORD_PTR)
    const u32 hic = static_cast<u32>(m.arg(0));
    const u32 msg = static_cast<u32>(m.arg(1));
    const u64 dw1 = m.arg(2);
    const u64 dw2 = m.arg(3);
    GuestCall call;
    GuestAddr driver_proc = 0;
    u64 dw_id = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = open_.find(hic);
        if (it == open_.end()) {
            m.set_return(static_cast<u32>(kIcerrBadHandle));
            return;
        }
        call = guest_call_;
        driver_proc = it->second.driver_proc;
        dw_id = it->second.dw_driver_id;
    }
    if (!call) {
        m.set_return(static_cast<u32>(kIcerrUnsupported));
        return;
    }
    m.set_return(call_driver_proc(call, driver_proc, dw_id, hic, msg, dw1, dw2,
                                  m.cpu_is_64bit()));
}

void Msvfw32Manager::impl_ICLocate(Marshaler& m) {
    // HIC ICLocate(DWORD fccType, DWORD fccHandler, LPBITMAPINFOHEADER lpbiIn,
    //              LPBITMAPINFOHEADER lpbiOut, WORD wFlags)
    const u32 fcc_type = static_cast<u32>(m.arg(0));
    u32 fcc_handler = static_cast<u32>(m.arg(1));
    const GuestAddr bi_in = m.arg_ptr(2);
    (void)m.arg_ptr(3);
    (void)m.arg(4);
    if (fcc_handler == 0 && bi_in != 0) {
        // BITMAPINFOHEADER.biCompression at offset 16.
        auto comp = m.read_u32(bi_in + 16);
        if (comp.is_ok()) {
            fcc_handler = comp.value();
        }
    }
    if (fcc_handler == 0) {
        auto drivers = collect_drivers(fcc_type);
        if (drivers.empty()) {
            m.set_return(0);
            return;
        }
        fcc_handler = drivers.front().fcc_handler;
    }
    m.set_return(open_hic(m, fcc_type, fcc_handler, 0));
}

void Msvfw32Manager::impl_DrawDibOpen(Marshaler& m) {
    // HDRAWDIB DrawDibOpen(void) — no DrawDib engine; fail honestly.
    m.set_return(0);
}

void Msvfw32Manager::impl_DrawDibClose(Marshaler& m) {
    (void)m.arg(0);
    m.set_return(0);
}

DllExports create_msvfw32_thunks(Msvfw32Manager& mgr) {
    DllExports exports;
    auto add = [&](const char* name, u16 stdcall_bytes, ApiHandler handler) {
        exports.entries.push_back(ExportEntry{
            std::string(name), 0, std::move(handler), stdcall_bytes,
        });
    };
    add("ICInfo", 12, [&mgr](Marshaler& m) { mgr.impl_ICInfo(m); });
    add("ICOpen", 12, [&mgr](Marshaler& m) { mgr.impl_ICOpen(m); });
    add("ICClose", 4, [&mgr](Marshaler& m) { mgr.impl_ICClose(m); });
    add("ICSendMessage", 16, [&mgr](Marshaler& m) { mgr.impl_ICSendMessage(m); });
    add("ICLocate", 20, [&mgr](Marshaler& m) { mgr.impl_ICLocate(m); });
    add("DrawDibOpen", 0, [&mgr](Marshaler& m) { mgr.impl_DrawDibOpen(m); });
    add("DrawDibClose", 4, [&mgr](Marshaler& m) { mgr.impl_DrawDibClose(m); });
    return exports;
}

} // namespace vitreo::winapi
