// Generated from reg_open_sdk{32,64}.expected. Do not hand edit.
// Captured from actual Microsoft SDK through MSVC on winoracle.
namespace reg_open_sdk {
inline constexpr auto ERROR_SUCCESS = 0u;
inline constexpr auto ERROR_FILE_NOT_FOUND = 2u;
inline constexpr auto ERROR_ACCESS_DENIED = 5u;
inline constexpr auto ERROR_INVALID_HANDLE = 6u;
inline constexpr auto ERROR_INVALID_PARAMETER = 87u;
inline constexpr auto ERROR_NOACCESS = 998u;
inline constexpr auto REG_OPTION_OPEN_LINK = 8u;
inline constexpr auto REG_OPTION_NON_VOLATILE = 0u;
inline constexpr auto KEY_QUERY_VALUE = 1u;
inline constexpr auto KEY_SET_VALUE = 2u;
inline constexpr auto KEY_READ = 131097u;
inline constexpr auto KEY_WRITE = 131078u;
inline constexpr auto KEY_EXECUTE = 131097u;
inline constexpr auto KEY_ALL_ACCESS = 983103u;
inline constexpr auto MAXIMUM_ALLOWED = 33554432u;
inline constexpr auto KEY_WOW64_32KEY = 512u;
inline constexpr auto KEY_WOW64_64KEY = 256u;
inline constexpr auto KEY_WOW64_RES = 768u;
inline constexpr auto HKEY_CLASSES_ROOT_LOW = 2147483648u;
inline constexpr auto HKEY_CURRENT_USER_LOW = 2147483649u;
inline constexpr auto HKEY_LOCAL_MACHINE_LOW = 2147483650u;
inline constexpr auto HKEY_USERS_LOW = 2147483651u;
inline constexpr auto HKEY_CURRENT_CONFIG_LOW = 2147483653u;
inline constexpr auto HKEY_PERFORMANCE_DATA_LOW = 2147483652u;
inline constexpr auto HKEY_PERFORMANCE_TEXT_LOW = 2147483728u;
inline constexpr auto HKEY_PERFORMANCE_NLSTEXT_LOW = 2147483744u;
inline constexpr auto REGSAM_SIZE = 4u;
inline constexpr auto LSTATUS_SIZE = 4u;
namespace x86 {
inline constexpr auto HKEY_CLASSES_ROOT_NATIVE = 2147483648u;
inline constexpr auto HKEY_CURRENT_USER_NATIVE = 2147483649u;
inline constexpr auto HKEY_LOCAL_MACHINE_NATIVE = 2147483650u;
inline constexpr auto HKEY_USERS_NATIVE = 2147483651u;
inline constexpr auto HKEY_CURRENT_CONFIG_NATIVE = 2147483653u;
inline constexpr auto HKEY_PERFORMANCE_DATA_NATIVE = 2147483652u;
inline constexpr auto HKEY_PERFORMANCE_TEXT_NATIVE = 2147483728u;
inline constexpr auto HKEY_PERFORMANCE_NLSTEXT_NATIVE = 2147483744u;
inline constexpr auto HKEY_SIZE = 4u;
inline constexpr auto PHKEY_SIZE = 4u;
} // namespace x86
namespace x64 {
inline constexpr auto HKEY_CLASSES_ROOT_NATIVE = 18446744071562067968ull;
inline constexpr auto HKEY_CURRENT_USER_NATIVE = 18446744071562067969ull;
inline constexpr auto HKEY_LOCAL_MACHINE_NATIVE = 18446744071562067970ull;
inline constexpr auto HKEY_USERS_NATIVE = 18446744071562067971ull;
inline constexpr auto HKEY_CURRENT_CONFIG_NATIVE = 18446744071562067973ull;
inline constexpr auto HKEY_PERFORMANCE_DATA_NATIVE = 18446744071562067972ull;
inline constexpr auto HKEY_PERFORMANCE_TEXT_NATIVE = 18446744071562068048ull;
inline constexpr auto HKEY_PERFORMANCE_NLSTEXT_NATIVE = 18446744071562068064ull;
inline constexpr auto HKEY_SIZE = 8u;
inline constexpr auto PHKEY_SIZE = 8u;
} // namespace x64
} // namespace reg_open_sdk
