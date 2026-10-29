#pragma once

/// @file marshaler.hpp
/// @brief Marshaler for converting between guest and host memory
///
/// Handles reading strings, structs, and arguments from guest memory,
/// and writing results back. Accounts for differences in pointer sizes,
/// memory layouts, and string encodings.

#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <string>

namespace vitreo::winapi {

// =============================================================================
// Marshaler Class
// =============================================================================

/// Marshals data between guest and host memory
///
/// The Marshaler provides utilities for:
/// - Reading ANSI and wide strings from guest memory
/// - Writing strings to guest memory
/// - Reading arbitrary structures
/// - Reading function arguments from stack/registers
/// - Setting return values
class Marshaler {
public:
    /// Create a Marshaler with access to guest memory
    /// @param memory Reference to guest memory
    explicit Marshaler(GuestMemory& memory);
    
    /// No copy
    Marshaler(const Marshaler&) = delete;
    Marshaler& operator=(const Marshaler&) = delete;
    
    /// Move constructor allowed, but assignment deleted (reference member)
    Marshaler(Marshaler&&) noexcept = default;
    Marshaler& operator=(Marshaler&&) = delete;
    
    /// Destructor
    ~Marshaler() = default;
    
    // -------------------------------------------------------------------------
    // String Reading
    // -------------------------------------------------------------------------
    
    /// Read a null-terminated ANSI string from guest memory
    /// @param guest_ptr Guest pointer to string
    /// @param max_length Maximum characters to read (0 = no limit)
    /// @return String contents or error
    [[nodiscard]] VResult<std::string> read_ansi_string(
        GuestAddr guest_ptr,
        usize max_length = 0
    ) const;
    
    /// Read a null-terminated wide string (UTF-16LE) from guest memory
    /// @param guest_ptr Guest pointer to string
    /// @param max_length Maximum characters to read (0 = no limit)
    /// @return String contents (converted to UTF-8) or error
    [[nodiscard]] VResult<std::string> read_wide_string(
        GuestAddr guest_ptr,
        usize max_length = 0
    ) const;
    
    // -------------------------------------------------------------------------
    // String Writing
    // -------------------------------------------------------------------------
    
    /// Write an ANSI string to guest memory (including null terminator)
    /// @param guest_ptr Destination guest pointer
    /// @param str String to write
    /// @return Success or error
    [[nodiscard]] VoidResult write_ansi_string(GuestAddr guest_ptr, std::string_view str);
    
    /// Write a wide string (UTF-16LE) to guest memory (including null terminator)
    /// @param guest_ptr Destination guest pointer
    /// @param str String to write (UTF-8, will be converted)
    /// @return Success or error
    [[nodiscard]] VoidResult write_wide_string(GuestAddr guest_ptr, std::string_view str);
    
    // -------------------------------------------------------------------------
    // Structure Reading/Writing
    // -------------------------------------------------------------------------
    
    /// Read a trivially copyable structure from guest memory
    /// @tparam T Structure type (must be trivially copyable)
    /// @param guest_ptr Guest pointer to structure
    /// @return Structure value or error
    template<typename T>
    [[nodiscard]] VResult<T> read_struct(GuestAddr guest_ptr) const;
    
    /// Write a trivially copyable structure to guest memory
    /// @tparam T Structure type (must be trivially copyable)
    /// @param guest_ptr Destination guest pointer
    /// @param value Structure to write
    /// @return Success or error
    template<typename T>
    [[nodiscard]] VoidResult write_struct(GuestAddr guest_ptr, const T& value);
    
    // -------------------------------------------------------------------------
    // Primitive Reading/Writing
    // -------------------------------------------------------------------------
    
    /// Read a u8 from guest memory
    [[nodiscard]] VResult<u8> read_u8(GuestAddr addr) const;
    
    /// Read a u16 from guest memory (little-endian)
    [[nodiscard]] VResult<u16> read_u16(GuestAddr addr) const;
    
    /// Read a u32 from guest memory (little-endian)
    [[nodiscard]] VResult<u32> read_u32(GuestAddr addr) const;
    
    /// Read a u64 from guest memory (little-endian)
    [[nodiscard]] VResult<u64> read_u64(GuestAddr addr) const;
    
    /// Write a u8 to guest memory
    [[nodiscard]] VoidResult write_u8(GuestAddr addr, u8 value);
    
    /// Write a u16 to guest memory (little-endian)
    [[nodiscard]] VoidResult write_u16(GuestAddr addr, u16 value);
    
    /// Write a u32 to guest memory (little-endian)
    [[nodiscard]] VoidResult write_u32(GuestAddr addr, u32 value);
    
    /// Write a u64 to guest memory (little-endian)
    [[nodiscard]] VoidResult write_u64(GuestAddr addr, u64 value);

    /// Write a pointer-sized value (HANDLE, HMODULE, PSID, SIZE_T, ...) at
    /// the guest's pointer width: 8 bytes in 64-bit mode, 4 bytes otherwise.
    /// Using write_u32 for these on x64 leaves the caller's high dword as
    /// garbage.
    [[nodiscard]] VoidResult write_ptr(GuestAddr addr, u64 value) {
        return cpu_is_64bit() ? write_u64(addr, value)
                              : write_u32(addr, static_cast<u32>(value));
    }

    /// Read a pointer-sized value at the guest's pointer width — the
    /// counterpart to write_ptr. Reading an out-parameter the guest owns
    /// (e.g. the RPC_CSTR* passed to RpcStringFree) with read_u32 on x64
    /// truncates a valid 64-bit pointer to its low dword.
    [[nodiscard]] VResult<u64> read_ptr(GuestAddr addr) const {
        if (cpu_is_64bit()) return read_u64(addr);
        auto r = read_u32(addr);
        if (!r.is_ok()) return VResult<u64>::err(r.error());
        return VResult<u64>::ok(static_cast<u64>(r.value()));
    }

    // -------------------------------------------------------------------------
    // CPU State / Fastcall Call Context
    // -------------------------------------------------------------------------
    
    /// Attach CPU state for register-based argument access
    /// @param cpu Reference to CpuState
    void attach_cpu(cpu::CpuState& cpu) noexcept { cpu_ = &cpu; }

    /// Check whether CPU state is attached
    [[nodiscard]] bool has_cpu() const noexcept { return cpu_ != nullptr; }

    /// Get CPU state (for FPU access in _CIacos, _ftol, etc.)
    [[nodiscard]] cpu::CpuState* cpu() noexcept { return cpu_; }

    /// The access violation Windows' own code would take touching guest
    /// memory: STATUS_ACCESS_VIOLATION, ExceptionInformation[0] = 1 for a
    /// write and 0 for a read, [1] = the address. It is raised in the guest
    /// when the thunk returns. False when no CPU is attached (nothing can be
    /// raised); the caller must then return its documented failure value.
    bool raise_access_violation(bool write, GuestAddr address) noexcept {
        if (cpu_ == nullptr) return false;
        cpu_->pending_raise.active = true;
        cpu_->pending_raise.from_helper = false;
        cpu_->pending_raise.code = 0xC0000005u;
        cpu_->pending_raise.flags = 0;
        cpu_->pending_raise.num_args = 2;
        cpu_->pending_raise.args[0] = write ? 1u : 0u;
        cpu_->pending_raise.args[1] = address;
        return true;
    }

    /// Check whether the attached CPU is in 64-bit mode
    [[nodiscard]] bool cpu_is_64bit() const noexcept { return cpu_ != nullptr && cpu_->is_64bit(); }
    
    /// Read a fastcall argument by index
    /// @param index Argument index (0 = RCX, 1 = RDX, 2 = R8, 3 = R9, >=4 stack)
    /// @return Argument value (0 if no CPU attached)
    ///
    /// Inline for the register arguments: every handler reads its arguments
    /// here, and a draw-heavy D3D11 frame makes ~18k crossings (WP-R1 C4).
    /// Stack slots stay out of line (stack_arg64 / stack_arg32).
    [[nodiscard]] u64 arg(usize index) const {
        // Caller must attach CPU state before calling arg()
        if (!cpu_) { return 0; }
        if (cpu_->is_64bit()) {
            // x64 fastcall: first 4 args in registers, rest on stack
            switch (index) {
                case 0: return cpu_->gpr.rcx;
                case 1: return cpu_->gpr.rdx;
                case 2: return cpu_->gpr.r8;
                case 3: return cpu_->gpr.r9;
                default: return stack_arg64(index);
            }
        }
        return stack_arg32(index);
    }

    /// The host bytes behind [base, base + size) of guest memory, located by
    /// the crossing that is running this handler (cli::run_direct_thunk),
    /// where `base` was the stack pointer at entry: the return address and
    /// the stack arguments of both ABIs, found with ONE guest-memory lookup
    /// instead of one per slot, and read in place.
    ///
    /// A window over stack arguments exists only when that single checked
    /// access would have succeeded for every byte (GuestMemory::host_ptr,
    /// which is resolve_for_read: a guard page, an unreadable page or a
    /// range split across mappings yields no window, and every slot is then
    /// read on its own exactly as before); see run_direct_thunk for the
    /// return-address-only case. It answers only while the stack pointer is
    /// still `base`, so a nested crossing on a deeper frame never sees it.
    struct StackWindow {
        GuestAddr base{0};
        u32 size{0};
        const u8* bytes{nullptr};
    };
    void set_stack_window(const StackWindow& window) noexcept {
        stack_window_ = window;
    }
    [[nodiscard]] const StackWindow& stack_window() const noexcept {
        return stack_window_;
    }

    /// A `float` argument by value. The Windows x64 convention passes a float
    /// in the first four positions in XMM0-3 (its GPR slot holds whatever the
    /// caller last left there); from the fifth on, and on x86, it occupies its
    /// stack slot like any 4-byte argument. `arg()` alone reads the GPR.
    [[nodiscard]] float arg_f32(usize index) const;

    /// A `double` argument by value: XMM0-3 for the first four positions on
    /// x64 (the GPR slot is undefined), two consecutive 4-byte stack slots
    /// (low dword first) on x86, where it is `index` and `index + 1`.
    [[nodiscard]] double arg_f64(usize index) const;

    /// Return a `float`: XMM0 on x64, ST(0) on x86 (both conventions return
    /// floating point there, never in RAX/EAX).
    void set_return_f32(float value);

    /// Read the guest return address of the current export call.
    ///
    /// On callee entry the stack pointer points at the return address in both
    /// bitnesses -- the same fact `arg()` relies on to find stack arguments.
    /// This identifies the *call site* inside the guest, which is what makes a
    /// trace diffable against a reference run of the same binary.
    /// @return Guest return address, or 0 if no CPU is attached or the read fails
    [[nodiscard]] u64 guest_return_address() const;

    /// Read a 32-bit (DWORD/UINT/BOOL) argument.
    ///
    /// The x64 calling convention gives every argument an 8-byte home, but a
    /// 32-bit parameter only defines the **low 32 bits** of it: MSVC emits
    /// `mov dword ptr [rsp+N], reg` for a stack-passed DWORD and leaves the
    /// upper half holding whatever was there before. Reading such a slot with
    /// arg() therefore returns garbage in the high dword, and any check of the
    /// form `arg(i) != 0` then fails a perfectly valid call.
    /// (Observed: UnityPlayer.dll passing dwFlags=0 to CreateSemaphoreExW.)
    /// Use this for every parameter Windows declares 32 bits wide.
    [[nodiscard]] u32 arg32(usize index) const {
        return static_cast<u32>(arg(index));
    }

    /// Read a fastcall argument as a guest pointer
    /// @param index Argument index
    /// @return Guest address
    [[nodiscard]] GuestAddr arg_ptr(usize index) const { return static_cast<GuestAddr>(arg(index)); }
    
    /// Set the return value (RAX)
    /// @param value Value to place in RAX
    void set_return(u64 value) noexcept;

    /// The value last placed in RAX/EAX by set_return (0 without a CPU), for
    /// diagnostics that log a call's result after the method returned.
    [[nodiscard]] u64 return_value() const noexcept;

    /// Set a 64-bit return value observing the guest ABI.
    ///
    /// On x64 a 64-bit result is returned in RAX, identical to set_return. On
    /// x86 an __int64/unsigned long long is returned in the EDX:EAX pair (low
    /// dword in EAX, high dword in EDX) - the caller reads *both* registers.
    /// set_return alone writes only EAX and leaves EDX holding whatever the
    /// last instruction left there, so a guest that consumes the full 64-bit
    /// value (e.g. GetTickCount64 feeding a 64-bit divide) sees a garbage high
    /// dword. Use this for every export whose declared return type is 64-bit.
    void set_return_i64(u64 value) noexcept;
    
    // -------------------------------------------------------------------------
    // Memory Access
    // -------------------------------------------------------------------------
    
    /// Get reference to underlying guest memory
    [[nodiscard]] GuestMemory& memory() noexcept { return memory_; }
    [[nodiscard]] const GuestMemory& memory() const noexcept { return memory_; }
    
private:
    /// arg() past the fourth x64 argument, and every x86 argument: a stack
    /// slot, from the stack window when it covers the slot, else read
    /// through guest memory.
    [[nodiscard]] u64 stack_arg64(usize index) const;
    [[nodiscard]] u64 stack_arg32(usize index) const;
    /// True with @p out filled when the stack window covers @p size bytes at
    /// @p offset from the current stack pointer.
    [[nodiscard]] bool read_stack_window(u64 offset, usize size,
                                         u64& out) const noexcept;

    GuestMemory& memory_;
    cpu::CpuState* cpu_ = nullptr;
    StackWindow stack_window_{};

    /// Default maximum string length to prevent runaway reads
    static constexpr usize DEFAULT_MAX_STRING_LENGTH = 65536;
};

// =============================================================================
// Template Implementations
// =============================================================================

template<typename T>
VResult<T> Marshaler::read_struct(GuestAddr guest_ptr) const {
    static_assert(std::is_trivially_copyable_v<T>, 
        "read_struct requires trivially copyable type");
    
    T value{};
    auto result = memory_.read_bytes(guest_ptr, 
        reinterpret_cast<Byte*>(&value), sizeof(T));
    
    if (result.is_err()) {
        return VResult<T>::err(result.error());
    }
    
    return VResult<T>::ok(value);
}

template<typename T>
VoidResult Marshaler::write_struct(GuestAddr guest_ptr, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>,
        "write_struct requires trivially copyable type");
    
    return memory_.write_bytes(guest_ptr,
        reinterpret_cast<const Byte*>(&value), sizeof(T));
}

} // namespace vitreo::winapi
