/// @file marshaler.cpp
/// @brief Marshaler implementation

#include "winapi/core/marshaler.hpp"

#include <cstring>
#include <limits>

namespace vitreo::winapi {

// =============================================================================
// Constructor
// =============================================================================

Marshaler::Marshaler(GuestMemory& memory)
    : memory_(memory)
{
}

// =============================================================================
// String Reading
// =============================================================================

VResult<std::string> Marshaler::read_ansi_string(
    GuestAddr guest_ptr,
    usize max_length
) const {
    if (guest_ptr == 0) {
        return VResult<std::string>::err(Error::InvalidAddress);
    }
    
    // max_length == 0 means unbounded (header contract); never silently cap.
    usize limit = (max_length > 0) ? max_length
                                   : std::numeric_limits<usize>::max();
    std::string result;
    result.reserve(64);  // Reasonable starting size
    
    for (usize i = 0; i < limit; ++i) {
        auto byte_result = memory_.read_u8(guest_ptr + i);
        if (byte_result.is_err()) {
            return VResult<std::string>::err(byte_result.error());
        }
        
        u8 byte = byte_result.value();
        if (byte == 0) {
            // Found null terminator
            return VResult<std::string>::ok(std::move(result));
        }
        
        result.push_back(static_cast<char>(byte));
    }
    
    // Hit length limit without finding null terminator
    return VResult<std::string>::ok(std::move(result));
}

VResult<std::string> Marshaler::read_wide_string(
    GuestAddr guest_ptr,
    usize max_length
) const {
    if (guest_ptr == 0) {
        return VResult<std::string>::err(Error::InvalidAddress);
    }
    
    // max_length == 0 means unbounded (header contract); never silently cap.
    usize limit = (max_length > 0) ? max_length
                                   : std::numeric_limits<usize>::max();
    std::string result;
    result.reserve(64);
    
    // UTF-16LE -> UTF-8. A high surrogate followed by a low surrogate is one
    // supplementary-plane code point (4-byte UTF-8); W APIs accept any
    // Unicode name (Learn: Naming Files), so a path returned by
    // FindFirstFileW/GetFullPathNameW must round-trip. An unpaired surrogate
    // has no UTF-8 form and cannot name a host (APFS) file, so it becomes
    // U+FFFD rather than an invalid byte sequence that would make the whole
    // string look like ANSI to guest_path_to_host_utf8.
    auto append = [&result](u32 cp) {
        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (usize i = 0; i < limit; ++i) {
        auto word_result = memory_.read_u16(guest_ptr + (i * 2));
        if (word_result.is_err()) {
            return VResult<std::string>::err(word_result.error());
        }
        const u16 code_unit = word_result.value();
        if (code_unit == 0) {
            return VResult<std::string>::ok(std::move(result));
        }
        if (code_unit >= 0xD800 && code_unit <= 0xDBFF && i + 1 < limit) {
            auto next = memory_.read_u16(guest_ptr + ((i + 1) * 2));
            if (next.is_ok() && next.value() >= 0xDC00 && next.value() <= 0xDFFF) {
                append(0x10000u + ((static_cast<u32>(code_unit) - 0xD800u) << 10) +
                       (static_cast<u32>(next.value()) - 0xDC00u));
                ++i;
                continue;
            }
        }
        if (code_unit >= 0xD800 && code_unit <= 0xDFFF) {
            append(0xFFFD);
            continue;
        }
        append(code_unit);
    }
    
    // Hit length limit without finding null terminator
    return VResult<std::string>::ok(std::move(result));
}

// =============================================================================
// String Writing
// =============================================================================

VoidResult Marshaler::write_ansi_string(GuestAddr guest_ptr, std::string_view str) {
    if (guest_ptr == 0) {
        return VoidResult::err(Error::InvalidAddress);
    }
    
    // Write string bytes
    for (usize i = 0; i < str.size(); ++i) {
        auto result = memory_.write_u8(guest_ptr + i, static_cast<u8>(str[i]));
        if (result.is_err()) {
            return result;
        }
    }
    
    // Write null terminator
    return memory_.write_u8(guest_ptr + str.size(), 0);
}

VoidResult Marshaler::write_wide_string(GuestAddr guest_ptr, std::string_view str) {
    if (guest_ptr == 0) {
        return VoidResult::err(Error::InvalidAddress);
    }
    
    usize out_index = 0;
    usize i = 0;
    
    while (i < str.size()) {
        u32 codepoint = 0;
        u8 first_byte = static_cast<u8>(str[i]);
        
        // Decode UTF-8
        if ((first_byte & 0x80) == 0) {
            // Single byte (ASCII)
            codepoint = first_byte;
            i += 1;
        } else if ((first_byte & 0xE0) == 0xC0) {
            // 2-byte sequence
            if (i + 1 >= str.size()) break;
            codepoint = ((first_byte & 0x1F) << 6) |
                        (static_cast<u8>(str[i + 1]) & 0x3F);
            i += 2;
        } else if ((first_byte & 0xF0) == 0xE0) {
            // 3-byte sequence
            if (i + 2 >= str.size()) break;
            codepoint = ((first_byte & 0x0F) << 12) |
                        ((static_cast<u8>(str[i + 1]) & 0x3F) << 6) |
                        (static_cast<u8>(str[i + 2]) & 0x3F);
            i += 3;
        } else if ((first_byte & 0xF8) == 0xF0) {
            // 4-byte sequence (surrogate pair needed)
            if (i + 3 >= str.size()) break;
            codepoint = ((first_byte & 0x07) << 18) |
                        ((static_cast<u8>(str[i + 1]) & 0x3F) << 12) |
                        ((static_cast<u8>(str[i + 2]) & 0x3F) << 6) |
                        (static_cast<u8>(str[i + 3]) & 0x3F);
            i += 4;
        } else {
            // Invalid UTF-8, skip byte
            i += 1;
            continue;
        }
        
        // Encode to UTF-16LE
        if (codepoint < 0x10000) {
            // BMP character - single code unit
            auto result = memory_.write_u16(guest_ptr + (out_index * 2), 
                static_cast<u16>(codepoint));
            if (result.is_err()) {
                return result;
            }
            out_index += 1;
        } else {
            // Supplementary character - surrogate pair
            codepoint -= 0x10000;
            u16 high_surrogate = static_cast<u16>(0xD800 + (codepoint >> 10));
            u16 low_surrogate = static_cast<u16>(0xDC00 + (codepoint & 0x3FF));
            
            auto result = memory_.write_u16(guest_ptr + (out_index * 2), high_surrogate);
            if (result.is_err()) {
                return result;
            }
            out_index += 1;
            
            result = memory_.write_u16(guest_ptr + (out_index * 2), low_surrogate);
            if (result.is_err()) {
                return result;
            }
            out_index += 1;
        }
    }
    
    // Write null terminator (2 bytes)
    return memory_.write_u16(guest_ptr + (out_index * 2), 0);
}

// =============================================================================
// Primitive Reading
// =============================================================================

VResult<u8> Marshaler::read_u8(GuestAddr addr) const {
    return memory_.read_u8(addr);
}

VResult<u16> Marshaler::read_u16(GuestAddr addr) const {
    return memory_.read_u16(addr);
}

VResult<u32> Marshaler::read_u32(GuestAddr addr) const {
    return memory_.read_u32(addr);
}

VResult<u64> Marshaler::read_u64(GuestAddr addr) const {
    return memory_.read_u64(addr);
}

// =============================================================================
// Primitive Writing
// =============================================================================

VoidResult Marshaler::write_u8(GuestAddr addr, u8 value) {
    return memory_.write_u8(addr, value);
}

VoidResult Marshaler::write_u16(GuestAddr addr, u16 value) {
    return memory_.write_u16(addr, value);
}

VoidResult Marshaler::write_u32(GuestAddr addr, u32 value) {
    return memory_.write_u32(addr, value);
}

VoidResult Marshaler::write_u64(GuestAddr addr, u64 value) {
    return memory_.write_u64(addr, value);
}

// =============================================================================
// Fastcall Call Context
// =============================================================================

float Marshaler::arg_f32(usize index) const {
    u32 bits = 0;
    if (cpu_ != nullptr && cpu_->is_64bit() && index < 4) {
        bits = cpu_->sse.xmm[index].u32_[0];
    } else {
        bits = static_cast<u32>(arg(index));
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double Marshaler::arg_f64(usize index) const {
    u64 bits = 0;
    if (cpu_ != nullptr && cpu_->is_64bit() && index < 4) {
        bits = cpu_->sse.xmm[index].u64_[0];
    } else if (cpu_ != nullptr && cpu_->is_64bit()) {
        bits = arg(index);
    } else {
        bits = (arg(index) & 0xFFFFFFFFull) | (arg(index + 1) << 32);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void Marshaler::set_return_f32(float value) {
    if (cpu_ == nullptr) return;
    if (cpu_->is_64bit()) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        cpu_->sse.xmm[0].u64_[0] = bits;
        cpu_->sse.xmm[0].u64_[1] = 0;
        return;
    }
    cpu_->fpu.push(cpu::Float80(static_cast<double>(value)));
}

bool Marshaler::read_stack_window(u64 offset, usize size,
                                  u64& out) const noexcept {
    const StackWindow& window = stack_window_;
    if (window.bytes == nullptr) return false;
    const GuestAddr sp = cpu_->is_64bit()
                             ? static_cast<GuestAddr>(cpu_->gpr.rsp)
                             : static_cast<GuestAddr>(cpu_->sp());
    if (sp != window.base || offset > window.size ||
        size > window.size - offset) {
        return false;
    }
    // Fixed sizes, so each copy is one load rather than a library call.
    if (size == sizeof(u64)) {
        std::memcpy(&out, window.bytes + offset, sizeof(u64));
    } else {
        u32 value = 0;
        std::memcpy(&value, window.bytes + offset, sizeof(u32));
        out = value;
    }
    return true;
}

u64 Marshaler::stack_arg64(usize index) const {
    // On callee entry RSP points to the return address. The caller-provided
    // shadow space follows it, then stack args.
    //   [RSP + 0x00]       = return address
    //   [RSP + 0x08..0x27] = 32-byte shadow space
    //   [RSP + 0x28]       = 5th argument (index 4)
    //   [RSP + 0x30]       = 6th argument (index 5)
    //   ...
    const u64 offset = 0x28 + (static_cast<u64>(index) - 4) * 8;
    u64 value = 0;
    if (read_stack_window(offset, sizeof(u64), value)) return value;
    GuestAddr stack_addr = cpu_->gpr.rsp + offset;
    auto result = memory_.read_u64(stack_addr);
    return result.is_ok() ? result.value() : 0;
}

u64 Marshaler::stack_arg32(usize index) const {
    // x86 stdcall: ALL args on stack, right-to-left, above return address
    // ESP points to return address; args start at ESP + 4
    const u64 offset = 4 + static_cast<u64>(index) * 4;
    u64 value = 0;
    if (read_stack_window(offset, sizeof(u32), value)) return value;
    GuestAddr stack_addr = cpu_->sp() + offset;
    auto result = memory_.read_u32(stack_addr);
    return result.is_ok() ? static_cast<u64>(result.value()) : 0;
}

u64 Marshaler::guest_return_address() const {
    if (!cpu_) { return 0; }

    u64 window_value = 0;
    if (read_stack_window(0, cpu_->is_64bit() ? sizeof(u64) : sizeof(u32),
                          window_value)) {
        return window_value;
    }
    if (cpu_->is_64bit()) {
        auto result = memory_.read_u64(cpu_->sp());
        return result.is_ok() ? result.value() : 0;
    }
    auto result = memory_.read_u32(cpu_->sp());
    return result.is_ok() ? static_cast<u64>(result.value()) : 0;
}

void Marshaler::set_return(u64 value) noexcept {
    if (cpu_) {
        if (cpu_->is_64bit()) {
            cpu_->gpr.rax = value;
        } else {
            cpu_->gpr.rax = value & 0xFFFFFFFF;
        }
    }
}

u64 Marshaler::return_value() const noexcept {
    if (!cpu_) return 0;
    return cpu_->is_64bit() ? cpu_->gpr.rax : (cpu_->gpr.rax & 0xFFFFFFFFull);
}

void Marshaler::set_return_i64(u64 value) noexcept {
    if (!cpu_) return;
    if (cpu_->is_64bit()) {
        cpu_->gpr.rax = value;
    } else {
        // EDX:EAX pair. Zero the high halves so a later 64-bit read of RAX/RDX
        // (should the guest be mid-transition) cannot resurrect stale bits.
        cpu_->gpr.rax = value & 0xFFFFFFFFULL;
        cpu_->gpr.rdx = (value >> 32) & 0xFFFFFFFFULL;
    }
}

} // namespace vitreo::winapi
