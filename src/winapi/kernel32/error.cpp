/// @file error.cpp
/// @brief GetLastError/SetLastError and the system message table

#include "error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace vitreo::winapi {

// =============================================================================
// Win32 Error Message Lookup
// =============================================================================

static const std::unordered_map<u32, const char*> k_error_messages = {
#include "crypto_error_messages.inc"
    {0,     "The operation completed successfully."},
    {1,     "Incorrect function."},
    {2,     "The system cannot find the file specified."},
    {3,     "The system cannot find the path specified."},
    {4,     "The system cannot open the file."},
    {5,     "Access is denied."},
    {6,     "The handle is invalid."},
    {8,     "Not enough memory resources are available to process this command."},
    {14,    "Not enough storage is available to complete this operation."},
    {15,    "The system cannot find the drive specified."},
    {17,    "The system cannot move the file to a different disk drive."},
    {18,    "There are no more files."},
    {21,    "The device is not ready."},
    {29,    "The system cannot write to the specified device."},
    {30,    "The system cannot read from the specified device."},
    {32,    "The process cannot access the file because it is being used by another process."},
    {33,    "The process cannot access the file because another process has locked a portion of the file."},
    {36,    "Too many files opened for sharing."},
    {50,    "The request is not supported."},
    {80,    "The file exists."},
    {87,    "The parameter is incorrect."},
    {111,   "The file name is too long."},
    {120,   "This function is not supported on this system."},
    {122,   "The data area passed to a system call is too small."},
    {123,   "The filename, directory name, or volume label syntax is incorrect."},
    {126,   "The specified module could not be found."},
    {127,   "The specified procedure could not be found."},
    {170,   "The requested resource is in use."},
    {183,   "Cannot create a file when that file already exists."},
    {206,   "The filename or extension is too long."},
    {232,   "The pipe is being closed."},
    {234,   "More data is available."},
    {267,   "The directory name is invalid."},
    {534,   "Arithmetic result exceeded 32 bits."},
    {1008,  "An attempt was made to reference a token that does not exist."},
    {1150,  "The specified program is not a Windows or MS-DOS program."},
    {1223,  "The operation was canceled by the user."},
    {1314,  "A required privilege is not held by the client."},
    {10054, "An existing connection was forcibly closed by the remote host."},
    {10060, "A connection attempt failed because the connected party did not properly respond."},
    {10061, "No connection could be made because the target machine actively refused it."},
};

const char* lookup_error_message(u32 code) {
    auto it = k_error_messages.find(code);
    if (it != k_error_messages.end()) return it->second;
    return nullptr;
}

// =============================================================================
// Error Management Functions
// =============================================================================

u32 get_last_error() noexcept {
    return g_last_error;
}

void set_last_error(u32 error_code) noexcept {
    g_last_error = error_code;
}

// =============================================================================
// Win32 API Thunk Implementations
// =============================================================================

void impl_GetLastError(Marshaler& m) {
    // GetLastError takes no parameters and returns DWORD in EAX
    u64 caller_ret = 0;
    if (auto* cpu = m.cpu()) {
        if (cpu->is_64bit()) {
            auto ret_r = m.memory().read_u64(cpu->sp());
            if (ret_r.is_ok()) caller_ret = ret_r.value();
        } else {
            auto ret_r = m.memory().read_u32(cpu->sp());
            if (ret_r.is_ok()) caller_ret = static_cast<u64>(ret_r.value());
        }
    }
    VITREO_TRACE( "[error] GetLastError(caller_ret=0x%llx) -> %u\n",
                  static_cast<unsigned long long>(caller_ret), g_last_error);
    m.set_return(static_cast<u64>(g_last_error));
}

void impl_SetLastError(Marshaler& m) {
    // SetLastError takes one DWORD parameter (dwErrCode) in RCX
    u32 err_code = static_cast<u32>(m.arg(0));
    u64 caller_ret = 0;
    if (auto* cpu = m.cpu()) {
        if (cpu->is_64bit()) {
            auto ret_r = m.memory().read_u64(cpu->sp());
            if (ret_r.is_ok()) caller_ret = ret_r.value();
        } else {
            auto ret_r = m.memory().read_u32(cpu->sp());
            if (ret_r.is_ok()) caller_ret = static_cast<u64>(ret_r.value());
        }
    }
    VITREO_TRACE( "[error] SetLastError(%u, caller_ret=0x%llx)\n", err_code,
                  static_cast<unsigned long long>(caller_ret));
    set_last_error(err_code);
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_error_exports() {
    DllExports exports;
    exports.entries.reserve(4);

    // GetLastError (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetLastError", 0,
        [](Marshaler& m) { impl_GetLastError(m); },
        0
    });

    // SetLastError (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "SetLastError", 0,
        [](Marshaler& m) { impl_SetLastError(m); },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
