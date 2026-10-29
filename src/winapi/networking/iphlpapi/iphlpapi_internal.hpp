#pragma once
/// @file iphlpapi_internal.hpp
/// @brief Private surface shared by the iphlpapi/*.cpp translation units.
///
/// iphlpapi.cpp was split by API family (TCP table, legacy interface tables,
/// adapter info, adapter addresses, notifications, MIB_IF_ROW2, interface
/// conversions, ICMP, dhcpcsvc). The include block, the contract constants and
/// the helpers more than one of those files needs were file-local in one
/// anonymous namespace before the split. They stay in an anonymous namespace
/// here, with the free functions `inline`, so every call site still inlines
/// them across the new translation-unit boundaries and no external symbol
/// appears.
///
/// Private to the IP Helper implementation; not part of iphlpapi.hpp's surface.

#include "iphlpapi.hpp"
#include "host_interfaces.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include <thread>
#include <condition_variable>
#include <poll.h>
#include <chrono>
#include <atomic>
#include <netinet/ip_icmp.h>
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <cstdlib>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __APPLE__
#include <net/if_dl.h>
#include <net/if_var.h>
#include <net/route.h>
#include <cerrno>
#include <libproc.h>
#include <sys/proc_info.h>
#endif

namespace vitreo::winapi {

namespace {

#include "ip_interface_contract_constants.inc"
#include "ip_compartment_contract.inc"
#include "icmp_contract_constants.inc"
#include "if_row2_contract.inc"
#include "adapter_contract.inc"

// Windows error codes
constexpr u32 NO_ERROR              = 0;
constexpr u32 ERROR_FILE_NOT_FOUND  = 2;
constexpr u32 ERROR_NOT_ENOUGH_MEMORY_CODE = 8;
constexpr u32 ERROR_BUFFER_OVERFLOW = 111;
constexpr u32 ERROR_INVALID_NAME_CODE = 123;
constexpr u32 ERROR_NO_DATA         = 232;
constexpr u32 ERROR_INVALID_PARAMETER = 87;
constexpr u32 ERROR_INSUFFICIENT_BUFFER = 122;
constexpr u32 ERROR_NOT_SUPPORTED = 50;
constexpr u32 ERROR_NOT_FOUND     = 1168;

// "\DEVICE\TCPIP_" + adapter GUID (iphlp_adapters_probe gii-relations).
constexpr std::string_view kTcpipDevicePrefix = "\\DEVICE\\TCPIP_";

[[nodiscard]] inline const AdapterAbi& adapter_abi(const Marshaler& m) {
    return m.cpu_is_64bit() ? kAdapterAbi64 : kAdapterAbi32;
}

[[nodiscard]] inline std::u16string utf8_to_utf16(std::string_view in) {
    std::u16string out;
    for (usize i = 0; i < in.size();) {
        const u8 c = static_cast<u8>(in[i]);
        u32 cp = 0xFFFD;
        usize len = 1;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 6 && i + 1 < in.size()) { cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(in[i + 1]) & 0x3Fu); len = 2; }
        else if ((c >> 4) == 14 && i + 2 < in.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<u8>(in[i + 1]) & 0x3Fu) << 6) | (static_cast<u8>(in[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c >> 3) == 30 && i + 3 < in.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<u8>(in[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<u8>(in[i + 2]) & 0x3Fu) << 6) | (static_cast<u8>(in[i + 3]) & 0x3Fu);
            len = 4;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

/// Adapter strings in the ANSI structures: ASCII survives, anything else
/// becomes '?' (the default-character substitution of WideCharToMultiByte).
[[nodiscard]] inline std::string ansi_text(std::string_view utf8) {
    std::string out;
    for (char16_t c : utf8_to_utf16(utf8)) {
        if (c >= 0xDC00 && c <= 0xDFFF) continue;
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

/// A guest structure image built in host memory. Pointers are recorded as
/// offsets and resolved against the guest base address when written out, so
/// the same image yields the required size before the caller's buffer exists.
class GuestBlob {
public:
    explicit GuestBlob(bool wide) : wide_(wide) {}
    [[nodiscard]] usize reserve(usize size, usize align = 8) {
        const usize off = (bytes_.size() + align - 1) & ~(align - 1);
        bytes_.resize(off + size, 0);
        return off;
    }
    template <typename T> void put(usize off, T value) { std::memcpy(bytes_.data() + off, &value, sizeof(T)); }
    void put_bytes(usize off, const void* data, usize n) { if (n) std::memcpy(bytes_.data() + off, data, n); }
    void put_ascii(usize off, std::string_view text, usize capacity) {
        const usize n = std::min(text.size(), capacity - 1);
        put_bytes(off, text.data(), n);
        bytes_[off + n] = 0;
    }
    /// Pointer at `off` to blob offset `target`.
    void pointer(usize off, usize target) { fixups_.push_back({off, target}); }
    [[nodiscard]] usize wide_string(std::string_view utf8) {
        const auto text = utf8_to_utf16(utf8);
        const usize off = reserve((text.size() + 1) * 2, 2);
        put_bytes(off, text.data(), text.size() * 2);
        return off;
    }
    [[nodiscard]] usize ascii_string(std::string_view text) {
        const usize off = reserve(text.size() + 1, 1);
        put_bytes(off, text.data(), text.size());
        return off;
    }
    /// A Windows SOCKADDR_IN / SOCKADDR_IN6 for `address`; returns its offset.
    [[nodiscard]] usize sockaddr(const iphlp::NetAddress& address) {
        const bool v4 = address.is_v4();
        const usize off = reserve(v4 ? 16 : 28, 4);
        put<u16>(off, address.family);
        if (v4) {
            put_bytes(off + 4, address.bytes.data(), 4);
        } else {
            put_bytes(off + 8, address.bytes.data(), 16);
            put<u32>(off + 24, address.scope_id);
        }
        return off;
    }
    [[nodiscard]] static u32 sockaddr_length(const iphlp::NetAddress& address) {
        return address.is_v4() ? 16u : 28u;
    }
    [[nodiscard]] usize size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::vector<u8> finish(GuestAddr base) const {
        std::vector<u8> out = bytes_;
        for (const auto& [off, target] : fixups_) {
            const u64 value = base + target;
            if (wide_) std::memcpy(out.data() + off, &value, 8);
            else { const u32 v32 = static_cast<u32>(value); std::memcpy(out.data() + off, &v32, 4); }
        }
        return out;
    }

private:
    bool wide_;
    std::vector<u8> bytes_;
    std::vector<std::pair<usize, usize>> fixups_;
};

[[nodiscard]] inline std::u16string tcpip_device_name(const iphlp::HostInterface& iface) {
    return utf8_to_utf16(std::string(kTcpipDevicePrefix) + iphlp::format_guid_braced(iface.guid));
}

[[nodiscard]] inline std::optional<u32> read_size(Marshaler& m, GuestAddr at) {
    if (at == 0) return std::nullopt;
    const auto value = m.read_u32(at);
    if (value.is_err()) return std::nullopt;
    return value.value();
}

[[nodiscard]] inline bool guest_writable(Marshaler& m, GuestAddr address, usize size) {
    return address != 0 && m.memory().guest_can_write(address, size);
}

[[nodiscard]] inline std::string read_guest_wide(Marshaler& m, GuestAddr at, usize max_chars) {
    std::string out;
    for (usize i = 0; i < max_chars; ++i) {
        const auto ch = m.read_u16(at + i * 2);
        if (ch.is_err() || ch.value() == 0) break;
        out.push_back(ch.value() < 0x80 ? static_cast<char>(ch.value()) : '\x7f');
    }
    return out;
}

} // namespace

} // namespace vitreo::winapi
