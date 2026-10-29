#pragma once

/// @file error.hpp
/// @brief Thread-local error code management (GetLastError/SetLastError)
///
/// Implements Windows error code management using thread-local storage.
/// Each thread has its own last error value, matching Windows behavior.

#include <vitreo/common.hpp>

namespace vitreo::winapi {

// Forward declarations
class Marshaler;
struct DllExports;

// =============================================================================
// Windows Error Codes
// =============================================================================

/// Common Windows error codes (from winerror.h)
namespace error_codes {

constexpr u32 ERROR_SUCCESS             = 0;
constexpr u32 ERROR_INVALID_FUNCTION    = 1;
constexpr u32 ERROR_FILE_NOT_FOUND      = 2;
constexpr u32 ERROR_PATH_NOT_FOUND      = 3;
constexpr u32 ERROR_TOO_MANY_OPEN_FILES = 4;
constexpr u32 ERROR_ACCESS_DENIED       = 5;
constexpr u32 ERROR_INVALID_HANDLE      = 6;
constexpr u32 ERROR_NOT_SAME_OBJECT     = 1656;  // winerror.h; 657 is ERROR_PWD_TOO_LONG
constexpr u32 ERROR_ARENA_TRASHED       = 7;
constexpr u32 ERROR_NOT_ENOUGH_MEMORY   = 8;
constexpr u32 ERROR_INVALID_BLOCK       = 9;
constexpr u32 ERROR_BAD_ENVIRONMENT     = 10;
constexpr u32 ERROR_BAD_FORMAT          = 11;
constexpr u32 ERROR_INVALID_ACCESS      = 12;
constexpr u32 ERROR_INVALID_DATA        = 13;
constexpr u32 ERROR_OUTOFMEMORY         = 14;
constexpr u32 ERROR_INVALID_DRIVE       = 15;
constexpr u32 ERROR_CURRENT_DIRECTORY   = 16;
constexpr u32 ERROR_NOT_SAME_DEVICE     = 17;
constexpr u32 ERROR_NO_MORE_FILES       = 18;
constexpr u32 ERROR_WRITE_PROTECT       = 19;
constexpr u32 ERROR_BAD_UNIT            = 20;
constexpr u32 ERROR_NOT_READY           = 21;
constexpr u32 ERROR_BAD_COMMAND         = 22;
constexpr u32 ERROR_CRC                 = 23;
constexpr u32 ERROR_BAD_LENGTH          = 24;
constexpr u32 ERROR_NOT_ALL_ASSIGNED    = 1300;
constexpr u32 ERROR_SEEK                = 25;
constexpr u32 ERROR_NOT_DOS_DISK        = 26;
constexpr u32 ERROR_SECTOR_NOT_FOUND    = 27;
constexpr u32 ERROR_OUT_OF_PAPER        = 28;
constexpr u32 ERROR_WRITE_FAULT         = 29;
constexpr u32 ERROR_READ_FAULT          = 30;
constexpr u32 ERROR_GEN_FAILURE         = 31;
constexpr u32 ERROR_SHARING_VIOLATION   = 32;
constexpr u32 ERROR_LOCK_VIOLATION      = 33;
constexpr u32 ERROR_WRONG_DISK          = 34;
constexpr u32 ERROR_SHARING_BUFFER_EXCEEDED = 36;
constexpr u32 ERROR_HANDLE_EOF          = 38;
constexpr u32 ERROR_HANDLE_DISK_FULL    = 39;
constexpr u32 ERROR_NOT_SUPPORTED       = 50;
constexpr u32 ERROR_REM_NOT_LIST        = 51;
constexpr u32 ERROR_DUP_NAME            = 52;
constexpr u32 ERROR_BAD_NETPATH         = 53;
constexpr u32 ERROR_NETWORK_BUSY        = 54;
constexpr u32 ERROR_DEV_NOT_EXIST       = 55;
constexpr u32 ERROR_FILE_EXISTS         = 80;
constexpr u32 ERROR_CANNOT_MAKE         = 82;
constexpr u32 ERROR_FAIL_I24            = 83;
constexpr u32 ERROR_INVALID_PARAMETER   = 87;
/// SetPriorityClass PROCESS_MODE_BACKGROUND_* (winerror.h)
constexpr u32 ERROR_PROCESS_MODE_ALREADY_BACKGROUND = 402;
constexpr u32 ERROR_PROCESS_MODE_NOT_BACKGROUND     = 403;
constexpr u32 ERROR_MAPPED_ALIGNMENT    = 1132; // MapViewOfFile offset/base alignment
constexpr u32 ERROR_NO_PROC_SLOTS       = 89;
constexpr u32 ERROR_DRIVE_LOCKED        = 108;
constexpr u32 ERROR_BROKEN_PIPE         = 109;
constexpr u32 ERROR_OPEN_FAILED         = 110;
constexpr u32 ERROR_BUFFER_OVERFLOW     = 111;
constexpr u32 ERROR_DISK_FULL           = 112;
constexpr u32 ERROR_SIGNAL_REFUSED      = 156;
constexpr u32 ERROR_NO_MORE_SEARCH_HANDLES = 113;
constexpr u32 ERROR_INVALID_TARGET_HANDLE = 114;
constexpr u32 ERROR_INVALID_CATEGORY    = 117;
constexpr u32 ERROR_INVALID_VERIFY_SWITCH = 118;
constexpr u32 ERROR_BAD_DRIVER_LEVEL    = 119;
constexpr u32 ERROR_CALL_NOT_IMPLEMENTED = 120;
constexpr u32 ERROR_SEM_TIMEOUT         = 121;
constexpr u32 ERROR_INSUFFICIENT_BUFFER = 122;
constexpr u32 ERROR_INVALID_ADDRESS      = 487;
/// winerror.h: "The paging file is too small for this operation to complete."
constexpr u32 ERROR_COMMITMENT_LIMIT     = 1455;
constexpr u32 ERROR_INVALID_NAME        = 123;
constexpr u32 ERROR_INVALID_LEVEL       = 124;
constexpr u32 ERROR_NO_VOLUME_LABEL     = 125;
constexpr u32 ERROR_MOD_NOT_FOUND       = 126;
/// FormatMessage could not find a message for the id it was given.
constexpr u32 ERROR_MR_MID_NOT_FOUND    = 317;
constexpr u32 ERROR_FILE_INVALID        = 1006;
constexpr u32 ERROR_INVALID_ACL         = 1336;
constexpr u32 ERROR_INVALID_SID         = 1337;
constexpr u32 ERROR_INVALID_SECURITY_DESCR = 1338;
constexpr u32 ERROR_PROC_NOT_FOUND      = 127;
constexpr u32 ERROR_NEGATIVE_SEEK       = 131;
constexpr u32 ERROR_SEEK_ON_DEVICE      = 132;
constexpr u32 ERROR_DIR_NOT_ROOT        = 144;
constexpr u32 ERROR_NOT_LOCKED          = 158;   // UnlockFile: segment already unlocked
constexpr u32 ERROR_USER_MAPPED_FILE    = 1224;  // operation on a file with a user-mapped section open
constexpr u32 ERROR_INVALID_USER_BUFFER = 1784;  // supplied user buffer is not valid
constexpr u32 ERROR_UNABLE_TO_REMOVE_REPLACED   = 1175;  // ReplaceFile
constexpr u32 ERROR_UNABLE_TO_MOVE_REPLACEMENT  = 1176;  // ReplaceFile
constexpr u32 ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 = 1177; // ReplaceFile
constexpr u32 ERROR_NOT_A_REPARSE_POINT = 4390;
constexpr u32 ERROR_DIR_NOT_EMPTY       = 145;
constexpr u32 ERROR_ALREADY_EXISTS      = 183;
constexpr u32 ERROR_NOT_OWNER           = 288;   // ReleaseMutex / SignalObjectAndWait
constexpr u32 ERROR_TOO_MANY_POSTS      = 298;   // ReleaseSemaphore past lMaximumCount
constexpr u32 ERROR_BAD_EXE_FORMAT      = 193;
constexpr u32 ERROR_ENVVAR_NOT_FOUND    = 203;
constexpr u32 ERROR_FILE_TOO_LARGE      = 223;
constexpr u32 ERROR_PIPE_NOT_CONNECTED  = 233;
constexpr u32 ERROR_PIPE_BUSY           = 231;
constexpr u32 ERROR_PIPE_CONNECTED      = 535;
constexpr u32 ERROR_PIPE_LISTENING      = 536;
constexpr u32 ERROR_ELEVATION_REQUIRED  = 740;
constexpr u32 ERROR_NO_MORE_ITEMS       = 259;
constexpr u32 ERROR_MORE_DATA           = 234;
constexpr u32 ERROR_NO_DATA             = 232;
constexpr u32 ERROR_TIMEOUT             = 1460;
constexpr u32 ERROR_OPERATION_ABORTED   = 995;
constexpr u32 ERROR_IO_INCOMPLETE       = 996;
constexpr u32 ERROR_IO_PENDING          = 997;
constexpr u32 ERROR_NOACCESS            = 998;
constexpr u32 ERROR_SWAPERROR           = 999;
constexpr u32 ERROR_DLL_INIT_FAILED     = 1114;
constexpr u32 ERROR_CANCELLED           = 1223;
constexpr u32 ERROR_REQUEST_ABORTED     = 1235;  // winerror.h
constexpr u32 ERROR_REQUEST_PAUSED      = 3050;  // winerror.h

// user32-style errors
constexpr u32 ERROR_INVALID_WINDOW_HANDLE = 1400;
constexpr u32 ERROR_INVALID_MENU_HANDLE   = 1401;
constexpr u32 ERROR_INVALID_CURSOR_HANDLE = 1402;
constexpr u32 ERROR_INVALID_ACCEL_HANDLE  = 1403;
constexpr u32 ERROR_INVALID_HOOK_HANDLE   = 1404;
constexpr u32 ERROR_INVALID_DWP_HANDLE    = 1405;
constexpr u32 ERROR_CLASS_ALREADY_EXISTS  = 1410;
constexpr u32 ERROR_CLASS_DOES_NOT_EXIST  = 1411;
constexpr u32 ERROR_INVALID_THREAD_ID     = 1444;
constexpr u32 ERROR_CLIPBOARD_NOT_OPEN    = 1418;
constexpr u32 ERROR_HOTKEY_ALREADY_REGISTERED = 1409;
constexpr u32 ERROR_HOTKEY_NOT_REGISTERED = 1419;
constexpr u32 ERROR_INVALID_HOOK_FILTER   = 1426;
constexpr u32 ERROR_INVALID_FILTER_PROC   = 1427;
constexpr u32 ERROR_HOOK_NEEDS_HMOD       = 1428;
constexpr u32 ERROR_GLOBAL_ONLY_HOOK      = 1429;
constexpr u32 ERROR_MESSAGE_SYNC_ONLY     = 1159;
constexpr u32 ERROR_INVALID_MESSAGE       = 1002;
constexpr u32 ERROR_INVALID_MSGBOX_STYLE  = 1438;
constexpr u32 ERROR_INVALID_SPI_VALUE     = 1439;
constexpr u32 ERROR_NOT_FOUND             = 1168;
constexpr u32 ERROR_DEVICE_NOT_CONNECTED  = 1167;
constexpr u32 ERROR_BAD_ARGUMENTS         = 160;
constexpr u32 ERROR_OLD_WIN_VERSION       = 1150;  // winerror.h
constexpr u32 ERROR_BAD_PATHNAME          = 161;
constexpr u32 ERROR_INVALID_FLAGS         = 1004;
constexpr u32 ERROR_INVALID_GROUPNAME     = 1209;  // winerror.h; 1453 is ERROR_WORKING_SET_QUOTA
constexpr u32 ERROR_INVALID_MONITOR_HANDLE = 1461;
constexpr u32 ERROR_NO_SYSTEM_RESOURCES   = 1450;
constexpr u32 ERROR_INVALID_OWNER         = 1307;
constexpr u32 ERROR_INVALID_PRIMARY_GROUP = 1308;
constexpr u32 ERROR_PRIVILEGE_NOT_HELD     = 1314;
constexpr u32 ERROR_NONE_MAPPED           = 1332;
constexpr u32 ERROR_CANT_ACCESS_DOMAIN_INFO = 1351;
constexpr u32 ERROR_BAD_TOKEN_TYPE         = 1349;
/// The module has no resource directory at all.
constexpr u32 ERROR_RESOURCE_DATA_NOT_FOUND = 1812;
constexpr u32 ERROR_RESOURCE_TYPE_NOT_FOUND = 1813;
constexpr u32 ERROR_RESOURCE_NAME_NOT_FOUND = 1814;
constexpr u32 ERROR_RESOURCE_LANG_NOT_FOUND = 1815;
constexpr u32 ERROR_RESOURCE_ENUM_USER_STOP = 15106;
constexpr u32 ERROR_DIRECTORY             = 267;  // The directory name is invalid

constexpr u32 E_POINTER                   = 0x80004003u;

// Security / Service error codes
constexpr u32 ERROR_NO_TOKEN              = 1008;
constexpr u32 ERROR_INVALID_SERVICE_CONTROL           = 1052;
constexpr u32 ERROR_SERVICE_REQUEST_TIMEOUT           = 1053;
constexpr u32 ERROR_SERVICE_ALREADY_RUNNING           = 1056;
constexpr u32 ERROR_SERVICE_DISABLED                  = 1058;
constexpr u32 ERROR_SERVICE_DOES_NOT_EXIST = 1060;
constexpr u32 ERROR_SERVICE_NOT_ACTIVE                = 1062;
/// Returned by StartServiceCtrlDispatcher when the caller is not a service.
constexpr u32 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT = 1063;
constexpr u32 ERROR_SERVICE_MARKED_FOR_DELETE         = 1072;
constexpr u32 ERROR_SERVICE_EXISTS                    = 1073;
constexpr u32 ERROR_SERVICE_NO_THREAD                 = 1054;

// Printer error codes
constexpr u32 ERROR_INVALID_PRINTER_NAME  = 1801;

} // namespace error_codes

// =============================================================================
// Thread-Local Error Storage
// =============================================================================

/// Thread-local storage for the last error code
/// Initialized to ERROR_SUCCESS (0) for each thread
inline thread_local u32 g_last_error = 0;

// =============================================================================
// Error Management Functions
// =============================================================================

/// Get the calling thread's last-error code value
/// @return The last-error code
[[nodiscard]] u32 get_last_error() noexcept;

/// Set the calling thread's last-error code value
/// @param error_code The error code to set
void set_last_error(u32 error_code) noexcept;

// =============================================================================
// Win32 API Thunk Implementations
// =============================================================================

/// Implementation of GetLastError
/// DWORD WINAPI GetLastError(void);
/// 
/// Returns the error code in EAX via Marshaler.
void impl_GetLastError(Marshaler& m);

/// Implementation of SetLastError
/// VOID WINAPI SetLastError(DWORD dwErrCode);
/// 
/// Reads dwErrCode from the stack (first argument) via Marshaler.
void impl_SetLastError(Marshaler& m);

// =============================================================================
// Export Registration
// =============================================================================

/// Look up a Win32 error code and return its English message string,
/// or nullptr if the code is not in the built-in table.
const char* lookup_error_message(u32 code);

/// Build the DllExports table for error management APIs
/// @return DllExports with GetLastError/SetLastError entries
DllExports make_error_exports();

} // namespace vitreo::winapi
