#pragma once
/// @file msi_customaction.hpp
/// @brief Windows Installer custom actions: the CustomAction Type bits, the
/// context a custom action runs in, the install handle it receives, and the
/// custom-action server process (msiexec.exe -Embedding, of the DLL's bitness)
/// that loads and calls its code (process architecture: msi_remote.hpp).
///
/// Contract sources: MsiDefs.h `msidbCustomActionType` (SDK 10.0.26100);
/// Learn "Custom Action Types", "Custom Action Return Processing Options",
/// "Custom Action Execution Scheduling Options", "Custom Action In-Script
/// Execution Options", "Custom Action Hidden Target Option", "Custom Action
/// Patch Uninstall Option", "Custom Action Return Values", "Deferred
/// Execution Custom Actions", "Rollback Custom Actions", "Commit Custom
/// Actions", "Obtaining Context Information for Deferred Execution Custom
/// Actions", "64-Bit Custom Actions", and tools/oracle_probe/msi_customaction_*
/// for what Learn leaves open. Gaps: .docs/contracts/msi/customaction.md.

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_core.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo { class GuestMemory; }

namespace vitreo::winapi::msi {

/// msidbCustomActionType (MsiDefs.h). Checked against the header's values in
/// tests/winapi/msi/test_msi_customaction.cpp.
enum CustomActionTypeBits : u32 {
    kCaDll = 0x00000001,             ///< Target = entry point name
    kCaExe = 0x00000002,             ///< Target = command line args
    kCaTextData = 0x00000003,        ///< Target = text formatted into a property/directory/error
    kCaJScript = 0x00000005,         ///< Target = entry point name, null if none to call
    kCaVBScript = 0x00000006,        ///< Target = entry point name, null if none to call
    kCaInstall = 0x00000007,         ///< Target = property list for nested engine initialization
    kCaBinaryData = 0x00000000,      ///< Source = Binary.Name
    kCaSourceFile = 0x00000010,      ///< Source = File.File
    kCaDirectory = 0x00000020,       ///< Source = Directory.Directory
    kCaProperty = 0x00000030,        ///< Source = Property.Property
    kCaContinue = 0x00000040,        ///< ignore action return status, continue running
    kCaAsync = 0x00000080,           ///< run asynchronously
    kCaFirstSequence = 0x00000100,   ///< skip if UI sequence already run
    kCaOncePerProcess = 0x00000200,  ///< skip if UI sequence already run in same process
    kCaClientRepeat = 0x00000300,    ///< run on client only if UI already run on client
    kCaInScript = 0x00000400,        ///< queue for execution within script
    kCaRollback = 0x00000100,        ///< with InScript: queue in the rollback script
    kCaCommit = 0x00000200,          ///< with InScript: run from the script on success
    kCaNoImpersonate = 0x00000800,   ///< no impersonation, run in system context
    kCa64BitScript = 0x00001000,     ///< script runs in a 64-bit process
    kCaHideTarget = 0x00002000,      ///< do not log Target / CustomActionData
    kCaTSAware = 0x00004000,         ///< impersonate for per-machine installs on TS machines
    kCaPatchUninstall = 0x00008000,  ///< ExtendedType column: run only during a patch uninstall
};

/// The guest process side a custom action needs beyond CustomActionHost:
/// temporary files, threads and processes it can start and wait on. The
/// msiexec.exe system program implements it (dll_registry_system_programs).
class CustomActionProcess : public CustomActionHost {
public:
    /// GetTempFileNameW(@p directory, L"MSI", 0) (Windows names custom-action
    /// temp files MSIxxxx.tmp; an empty @p directory is GetTempPathW's), then
    /// write @p bytes to it. False when it could not be made.
    virtual bool write_temp_file(const std::vector<u8>& bytes, const std::u16string& directory,
                                 std::u16string& path) = 0;
    /// DeleteFileW.
    virtual void delete_file(const std::u16string& path) = 0;
    /// The machine of a PE file (IMAGE_FILE_MACHINE_*), or nullopt when the
    /// file cannot be read or is not a PE image.
    [[nodiscard]] virtual std::optional<u16> image_machine(const std::u16string& path) = 0;
    /// CreateThread running `UINT __stdcall entry(MSIHANDLE)` with
    /// @p install_handle; the thread handle, 0 on failure.
    virtual u64 start_dll_entry(u64 entry, u32 install_handle) = 0;
    /// CreateProcessW; the process handle (the thread handle is closed), 0 on
    /// failure.
    virtual u64 start_process(const std::u16string& application, const std::u16string& command_line,
                              const std::u16string& directory) = 0;
    /// Wait for a thread or process handle, return its exit code, close it.
    virtual u32 wait_and_close(u64 handle) = 0;
    virtual void close_handle(u64 handle) = 0;

    // -- the installer's process architecture (msi_remote.hpp) --

    /// This process's guest memory, and scratch blocks in it: the engine side
    /// of the remote session API stages a relayed call's arguments there.
    virtual GuestMemory& guest_memory() = 0;
    /// VirtualAlloc'd read/write block of at least @p bytes, 0 on failure.
    virtual GuestAddr alloc_scratch(u32 bytes) = 0;
    virtual void free_scratch(GuestAddr block) = 0;
    /// CreateProcessW of the custom-action server of @p x64's bitness,
    /// `"%WINDIR%\System32\msiexec.exe" -Embedding <token>` (SysWOW64 for
    /// x86; Sysnative from a 32-bit process). The process handle is not kept:
    /// the server's connection is its lifetime. False when it did not start.
    virtual bool start_custom_action_server(bool x64, const std::u16string& token) = 0;
    /// Dispatch the calling thread's pending window messages (the engine
    /// thread owns the internal UI's windows while it waits).
    virtual void pump_messages() = 0;
};

/// The custom-action host of this process (every guest process has one:
/// dll_registry_system_programs install_msi_process_host; null in a bare
/// unit-test manager).
/// run_custom_action uses it; the engine fills Services::custom_actions from
/// it for the other areas (SelfRegModules loads DLLs through it).
void set_process_custom_action_host(CustomActionProcess* host);
[[nodiscard]] CustomActionProcess* process_custom_action_host();

/// Wait for the asynchronous (msidbCustomActionTypeAsync) custom actions
/// still running and release what they hold. The engine calls it when the
/// install session ends (measured: Windows collects them after the top-level
/// action ends). Their return values never change the install's result
/// (measured, with and without Continue), so this returns kActionSuccess.
u32 finish_async_custom_actions(Session& session);

/// The custom-action server's side (msiexec.exe -Embedding <token>):
/// connect to the engine's pipe for @p token, then run each DLL custom action
/// the engine sends (LoadLibraryExW, GetProcAddress, the entry on its own
/// thread with a proxy hInstall, FreeLibrary) until told to stop or the
/// engine goes away. Returns the process exit code (0).
u32 run_custom_action_server(MsiManager& mgr, const std::string& token, CustomActionProcess& host);

/// The Windows pipe name a custom-action server with @p token connects to.
[[nodiscard]] std::string custom_action_server_pipe(const std::string& token);

/// Map the value a DLL custom action returned to the ActionCode the
/// sequencer acts on (Learn "Custom Action Return Values", measured
/// tools/oracle_probe/msi_customaction_*): exposed for tests.
[[nodiscard]] u32 dll_return_to_action_code(u32 returned);

}  // namespace vitreo::winapi::msi
