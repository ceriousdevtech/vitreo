#pragma once

/// @file seh.hpp
/// @brief Structured Exception Handling (SEH-001)

#include <vitreo/common.hpp>

#include <algorithm>
#include <functional>
#include <mutex>
#include <vector>

namespace vitreo::cpu {

// ============================================================================
// Exception codes
// ============================================================================
constexpr u32 EXCEPTION_ACCESS_VIOLATION     = 0xC0000005;
constexpr u32 EXCEPTION_INT_DIVIDE_BY_ZERO   = 0xC0000094;
constexpr u32 EXCEPTION_PRIV_INSTRUCTION     = 0xC0000096;
constexpr u32 EXCEPTION_STACK_OVERFLOW       = 0xC00000FD;
constexpr u32 EXCEPTION_CPP_MSVC             = 0xE06D7363;

// Exception handler return values
constexpr i32 EXCEPTION_CONTINUE_EXECUTION = -1;
constexpr i32 EXCEPTION_CONTINUE_SEARCH    =  0;

// UNW flags
constexpr u8 UNW_FLAG_NHANDLER  = 0;
constexpr u8 UNW_FLAG_EHANDLER  = 1;
constexpr u8 UNW_FLAG_UHANDLER  = 2;
constexpr u8 UNW_FLAG_CHAININFO = 4;

// ============================================================================
// RUNTIME_FUNCTION
// ============================================================================
struct RuntimeFunction {
    u32 begin_address;    // RVA of function start
    u32 end_address;      // RVA of function end
    u32 unwind_info_rva;  // RVA of UNWIND_INFO

    /// Check if a given RVA falls within this function
    [[nodiscard]] bool contains(u32 rva) const noexcept {
        return rva >= begin_address && rva < end_address;
    }
};

// ============================================================================
// EXCEPTION_RECORD (guest-layout)
// ============================================================================
struct ExceptionRecord {
    u32 exception_code;
    u32 exception_flags;
    u64 exception_record_next;  // Pointer to chained record (guest addr, usually 0)
    u64 exception_address;      // RIP where exception occurred
    u32 number_parameters;
    u64 exception_information[15]; // Exception information array
    
    ExceptionRecord() noexcept
        : exception_code(0), exception_flags(0), exception_record_next(0),
          exception_address(0), number_parameters(0), exception_information{} {}
};

// ============================================================================
// Function Table (registered RUNTIME_FUNCTION entries)
// ============================================================================

struct FunctionTableEntry {
    GuestAddr base_address;                      // Image base
    std::vector<RuntimeFunction> functions;       // Sorted by begin_address
};

// ============================================================================
// Top-level exception filter callback
// ============================================================================
using UnhandledExceptionFilter = std::function<i32(const ExceptionRecord&)>;

// ============================================================================
// SehManager
// ============================================================================
class SehManager {
public:
    SehManager() = default;
    
    /// RtlAddFunctionTable — register function table entries
    bool add_function_table(const RuntimeFunction* table, u32 entry_count,
                             GuestAddr base_address);
    
    /// RtlDeleteFunctionTable — remove entries by base address
    bool delete_function_table(GuestAddr base_address);
    
    /// RtlLookupFunctionEntry — find RUNTIME_FUNCTION for a given RIP
    /// @param control_pc The instruction pointer to look up
    /// @param image_base_out Output: the image base for the found entry
    /// @return Pointer to RuntimeFunction or nullptr if not found
    [[nodiscard]] const RuntimeFunction* lookup_function_entry(
        GuestAddr control_pc, GuestAddr& image_base_out) const;
    
    /// Dispatch an exception
    /// @return EXCEPTION_CONTINUE_EXECUTION or EXCEPTION_CONTINUE_SEARCH
    i32 dispatch_exception(const ExceptionRecord& record);
    
    /// RaiseException — build EXCEPTION_RECORD and dispatch
    void raise_exception(u32 code, u32 flags, u32 num_args, const u64* args,
                          GuestAddr rip);
    
    /// SetUnhandledExceptionFilter
    /// @return Previous filter (may be empty)
    UnhandledExceptionFilter set_unhandled_exception_filter(
        UnhandledExceptionFilter filter);
    
    /// Get total number of registered function entries
    [[nodiscard]] usize function_count() const;

private:
    mutable std::mutex m_mutex;
    std::vector<FunctionTableEntry> m_tables;
    UnhandledExceptionFilter m_unhandled_filter;
};

} // namespace vitreo::cpu
