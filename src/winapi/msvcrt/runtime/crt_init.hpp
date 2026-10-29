#pragma once
/// @file crt_init.hpp
/// @brief msvcrt/ucrtbase CRT initialization stubs (WIN32-067)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class ProcessManager;
class CrtDataBlock;
class ThreadManager;

/// CRT initialization manager
class CrtInitManager {
public:
    CrtInitManager();

    CrtInitManager(const CrtInitManager&) = delete;
    CrtInitManager& operator=(const CrtInitManager&) = delete;
    ~CrtInitManager() = default;

    // ---- _initterm ----
    /// Call each non-null void(*)(void) in [begin, end)
    /// Uses host-side function pointers for testing
    using VoidFunc = std::function<void()>;
    void initterm(const std::vector<VoidFunc>& funcs);

    // ---- _initterm_e ----
    /// Call each non-null int(*)(void) in [begin, end), stop on non-zero
    using IntFunc = std::function<int()>;
    [[nodiscard]] int initterm_e(const std::vector<IntFunc>& funcs);

    // ---- Guest _initterm callback ----
    /// Callback to run a guest void(*)(void) initializer.
    /// Must be set before _initterm is called from guest code.
    using GuestInitCallback = std::function<void(GuestAddr)>;
    void set_initterm_callback(GuestInitCallback cb) { m_initterm_callback = std::move(cb); }
    [[nodiscard]] bool has_initterm_callback() const noexcept { return !!m_initterm_callback; }
    void call_guest_init(GuestAddr func_addr) { if (m_initterm_callback) m_initterm_callback(func_addr); }

    /// Callback to run a guest int(*)(void) initializer and return its result.
    using GuestInitECallback = std::function<u64(GuestAddr)>;
    void set_initterm_e_callback(GuestInitECallback cb) { m_initterm_e_callback = std::move(cb); }
    [[nodiscard]] bool has_initterm_e_callback() const noexcept { return !!m_initterm_e_callback; }
    [[nodiscard]] u64 call_guest_init_e(GuestAddr func_addr) { return m_initterm_e_callback ? m_initterm_e_callback(func_addr) : 0; }

    // ---- __getmainargs / __wgetmainargs ----
    /// Set the program name and command line args
    void set_command_line(const std::string& program, const std::vector<std::string>& args);

    /// Get argc
    [[nodiscard]] int get_argc() const noexcept;
    /// Get argv (ANSI strings)
    [[nodiscard]] const std::vector<std::string>& get_argv() const noexcept;
    /// Get wide argv
    [[nodiscard]] const std::vector<std::wstring>& get_wargv() const;

    // ---- _amsg_exit ----
    /// Record fatal CRT error number; sets exit_requested flag
    void amsg_exit(int rterrnum);
    [[nodiscard]] bool exit_requested() const noexcept;
    [[nodiscard]] int last_error_num() const noexcept;

    // ---- _set_app_type ----
    void set_app_type(int app_type);
    [[nodiscard]] int get_app_type() const noexcept;

    // ---- _cexit / _c_exit ----
    void cexit();
    void c_exit();

    // ---- _configthreadlocale ----
    [[nodiscard]] int config_thread_locale(int type);

    // ---- _set_new_mode ----
    [[nodiscard]] int set_new_mode(int mode);

private:
    std::vector<std::string> m_argv;
    std::vector<std::wstring> m_wargv;
    bool m_exit_requested{false};
    int m_last_error_num{0};
    int m_app_type{0};         // 0=console, 1=GUI
    int m_thread_locale{0};    // 0=global
    int m_new_mode{0};
    bool m_wargv_dirty{true};
    GuestInitCallback m_initterm_callback;    // callback to run guest initializers
    GuestInitECallback m_initterm_e_callback; // callback to run guest initializers that return int
};

class CrtEnvironmentManager;

/// Register msvcrt init thunks
DllExports create_crt_init_thunks(
    CrtInitManager& mgr, CrtDataBlock& crt_data,
    CrtEnvironmentManager& env_mgr);

/// Create additional CRT thunks that need a ProcessManager reference
class CrtMemoryManager;

DllExports create_crt_process_thunks(ProcessManager& pm, CrtDataBlock& crt_data, ThreadManager& tm,
                                    CrtMemoryManager& memory_manager);

/// The floating-point environment exports (crt_fpenv.cpp): _controlfp,
/// _control87, _controlfp_s, __control87_2, _statusfp, _statusfp2, _clearfp,
/// _fpreset and __fpe_flt_rounds, over the calling thread's FCW and MXCSR.
void add_crt_fpenv_exports(DllExports& exports);

} // namespace vitreo::winapi
