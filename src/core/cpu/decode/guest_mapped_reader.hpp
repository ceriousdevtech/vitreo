#pragma once

/// @file guest_mapped_reader.hpp
/// @brief ByteReader over a mapped GuestMemory window.
///
/// Intel SDM Vol. 2: an x86 instruction is at most 15 bytes. Prefetch a
/// contiguous mapped window so a cold basic-block decode is one host_ptr,
/// not one lookup per instruction. Callers that decode a whole block should
/// reuse one reader; the window refills at page boundaries.

#include "core/cpu/decode/decoder.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>

namespace vitreo::cpu {

class GuestMappedByteReader final : public ByteReader {
public:
    static constexpr usize kDefaultWindow = 1024;
    static constexpr usize kMaxInsn = 15;

    GuestMappedByteReader(GuestMemory& memory, u64 address,
                          usize window = kDefaultWindow) noexcept
        : m_memory(memory), m_start(address),
          m_window(window < kMaxInsn ? kMaxInsn : window) {
        if (m_window > sizeof(m_buf)) m_window = sizeof(m_buf);
        fill_window(address);
    }

    [[nodiscard]] bool read_u8(u8& out) noexcept override {
        const u64 abs = m_start + m_pos;
        if (in_window(abs)) {
            out = m_buf[abs - m_buf_base];
            ++m_pos;
            return true;
        }
        fill_window(abs);
        if (in_window(abs)) {
            out = m_buf[abs - m_buf_base];
            ++m_pos;
            return true;
        }
        auto result = m_memory.fetch_u8(static_cast<GuestAddr>(abs));
        if (!result) return false;
        out = result.value();
        ++m_pos;
        return true;
    }

    [[nodiscard]] usize position() const noexcept override { return m_pos; }

private:
    [[nodiscard]] bool in_window(u64 abs) const noexcept {
        return m_len != 0 && abs >= m_buf_base && abs < m_buf_base + m_len;
    }

    void fill_window(u64 abs) noexcept {
        m_buf_base = abs;
        m_len = 0;
        usize page_left = 0x1000u - static_cast<usize>(abs & 0xFFFu);
        usize n = page_left < m_window ? page_left : m_window;
        if (n < kMaxInsn) n = kMaxInsn;
        const Byte* p = m_memory.fetch_ptr(static_cast<GuestAddr>(abs), n);
        if (p == nullptr && n > kMaxInsn) {
            n = kMaxInsn;
            p = m_memory.fetch_ptr(static_cast<GuestAddr>(abs), n);
        }
        if (p == nullptr) return;
        std::memcpy(m_buf, p, n);
        m_len = n;
    }

    GuestMemory& m_memory;
    u64 m_start;
    u64 m_buf_base{0};
    usize m_pos{0};
    usize m_len{0};
    usize m_window;
    u8 m_buf[kDefaultWindow];
};

}  // namespace vitreo::cpu
