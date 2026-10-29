#pragma once
/// @file msi_remote.hpp
/// @brief Windows Installer's process architecture: the wire the installer
/// processes talk over, and the remote session API a custom-action server
/// relays to the engine.
///
/// Processes (Learn "Windows Installer Service", "Custom Action Security",
/// "64-Bit Custom Actions"; .docs/contracts/msi/process.md):
///   * the client: any process that calls MsiInstallProduct & co., and
///     msiexec.exe /i. It keeps the external UI handlers, MsiSetInternalUI's
///     level and MsiEnableLog's request, runs the UI sequence when the UI
///     level shows wizard dialogs, and receives every message the install
///     posts (msi_service.hpp);
///   * the installer service: msiexec.exe /V, the "msiserver" service the
///     capsule SCM starts, running the execute sequence (or the whole install
///     when no wizard UI runs) in the system context;
///   * custom-action servers: msiexec.exe -Embedding of the DLL's bitness
///     (System32 for an x64 DLL, SysWOW64 for an x86 one), started by the
///     process running the engine for elevated (deferred no-impersonate)
///     actions and by the client for impersonated ones. A DLL custom action
///     runs there; the MSIHANDLEs it holds are proxies whose calls this file
///     relays to the engine.
///
/// Transport: ipc::UnixPipe (Windows named pipes over AF_UNIX, capsule-
/// scoped), framed messages. Every request is answered on the same
/// connection; the side that waits for an answer serves the other side's
/// requests meanwhile, so nested calls (a custom action's MsiDoAction that
/// runs another custom action, a message the engine relays to the client
/// while a custom action waits) never deadlock.

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_core.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo { class GuestMemory; }
namespace vitreo::ipc { class UnixPipe; }

namespace vitreo::winapi::msi {

class CustomActionProcess;

// ---- Wire codec -----------------------------------------------------------------

/// Little-endian, length-prefixed encoding of the values the installer
/// processes exchange.
class WireWriter {
public:
    void u8_(u8 v) { m_bytes.push_back(v); }
    void u32_(u32 v);
    void u64_(u64 v);
    void bytes(std::span<const u8> data);
    void str(const std::string& text);
    void u16str(const std::u16string& text);
    void record(const Record& record);
    void string_map(const std::map<std::string, std::string>& map);
    [[nodiscard]] std::vector<u8>& data() { return m_bytes; }

private:
    std::vector<u8> m_bytes;
};

class WireReader {
public:
    explicit WireReader(std::span<const u8> data) : m_data(data) {}
    [[nodiscard]] bool ok() const { return m_ok; }
    /// True when every byte was consumed and nothing failed.
    [[nodiscard]] bool done() const { return m_ok && m_pos == m_data.size(); }
    u8 u8_();
    u32 u32_();
    u64 u64_();
    std::vector<u8> bytes();
    std::string str();
    std::u16string u16str();
    std::optional<Record> record();
    std::map<std::string, std::string> string_map();

private:
    bool need(usize n);
    std::span<const u8> m_data;
    usize m_pos{0};
    bool m_ok{true};
};

/// Message kinds. Custom-action server <-> engine, then client <-> service.
enum class Wire : u32 {
    Hello = 1,        ///< CA server -> engine: pid, bitness
    RunDll = 2,       ///< engine -> CA server: run one DLL custom action
    Call = 3,         ///< CA server -> engine: one relayed msi.dll call
    CallReply = 4,
    CloseHandle = 5,  ///< CA server -> engine: MsiCloseHandle of a remote handle
    CloseReply = 6,
    Done = 7,         ///< CA server -> engine: the action's outcome
    Shutdown = 8,     ///< engine -> CA server: exit
    InstallRequest = 20,  ///< client -> service
    UiMessage = 21,       ///< service -> client: one INSTALLMESSAGE
    UiReply = 22,
    LaunchServer = 23,    ///< service -> client: start an impersonated CA server
    LaunchReply = 24,
    InstallDone = 25,     ///< service -> client: the install's result
};

/// A received message.
struct Frame {
    Wire kind{Wire::Hello};
    std::vector<u8> body;
};

/// One connected installer pipe endpoint. send() and the request/answer
/// pairing are serialized: several custom-action threads of one server share
/// its connection.
class RemoteChannel {
public:
    explicit RemoteChannel(std::shared_ptr<ipc::UnixPipe> pipe);
    ~RemoteChannel();
    RemoteChannel(const RemoteChannel&) = delete;
    RemoteChannel& operator=(const RemoteChannel&) = delete;

    /// A connected pair (tests, in-process links).
    static std::pair<std::shared_ptr<RemoteChannel>, std::shared_ptr<RemoteChannel>> pair();

    bool send(Wire kind, const std::vector<u8>& body);
    /// The next message; nullopt on end of stream or error, or when
    /// @p timeout_ms (-1: none) passed with nothing to read (then
    /// timed_out() is true).
    std::optional<Frame> receive(int timeout_ms = -1);
    [[nodiscard]] bool timed_out() const { return m_timed_out; }
    /// Send @p kind and wait for the @p reply kind, answering nothing else
    /// (the peer of a CA-server relay call never sends requests back on it).
    std::optional<std::vector<u8>> transact(Wire kind, const std::vector<u8>& body, Wire reply);
    [[nodiscard]] bool broken() const { return m_broken; }
    /// The endpoint's descriptor (poll), -1 when closed.
    [[nodiscard]] int fd() const;
    void close();

private:
    bool read_exact(void* out, usize n);
    std::shared_ptr<ipc::UnixPipe> m_pipe;
    std::mutex m_send_mutex;
    std::mutex m_transact_mutex;
    bool m_broken{false};
    bool m_timed_out{false};
};

// ---- The remote session API (custom-action server side) ------------------------------

/// The MSIHANDLE a custom-action server's code holds for an engine object
/// (install session, database, view, summary information): calls on it are
/// relayed to the engine (Learn "Accessing a Database or Session from Within
/// a Custom Action"; records are local to the server and travel by value).
struct RemoteHandle {
    std::shared_ptr<RemoteChannel> channel;
    u32 remote{0};
};

/// How one relayable export's arguments are relayed (msiquery.h prototypes):
///   h  the target MSIHANDLE (argument 0: a RemoteHandle relays the call)
///   i  a 32-bit value
///   s  an input string (A or W per `wide`), may be null
///   r  an input record handle (sent by value), may be 0
///   R  an in/out record handle (MsiViewModify refreshes it)
///   B  an output string buffer, followed by n
///   n  its DWORD* capacity / length in TCHARs
///   o  a DWORD/INT output (may be null)
///   f  a FILETIME output (may be null)
///   F  a FILETIME input (may be null)
///   H  an MSIHANDLE output (records come back by value, other handles as
///      proxies)
/// `ret` is 'u' for a 32-bit return value, 'H' for a returned MSIHANDLE.
struct RelaySignature {
    std::string name;
    std::string args;
    bool wide{false};
    char ret{'u'};
};
/// Every msi.dll export a custom action can call on an engine handle.
[[nodiscard]] const std::vector<RelaySignature>& relay_signatures();

/// The last registrar (msi_core.cpp msi_handlers): wraps every relayable
/// export, MsiCloseHandle and MsiCloseAllHandles so a RemoteHandle argument
/// relays the call; any other handle runs the local handler. Records the
/// unwrapped handlers for the engine side (local_handlers).
void register_remote_session_api(MsiManager& mgr, HandlerMap& map);

/// This process is a custom-action server: every guest thread runs custom
/// action code (MsiSetExternalUIRecord answers ERROR_CALL_NOT_IMPLEMENTED,
/// Learn MsiSetExternalUIRecord).
void set_custom_action_server_process(bool on);
[[nodiscard]] bool custom_action_server_process();

/// A local proxy for engine handle @p remote on @p channel.
u32 add_remote_handle(MsiManager& mgr, std::shared_ptr<RemoteChannel> channel, u32 remote);

// ---- The remote session API (engine side) ---------------------------------------------

/// Answers one custom action's relayed calls in the engine process, on the
/// engine thread (a guest thread). Calls run the same export handlers a
/// local caller reaches, through a Marshaler over scratch guest memory of
/// this process. Handles created for the action are closed when it ends.
class RelayExecutor {
public:
    RelayExecutor(MsiManager& mgr, CustomActionProcess& host);
    ~RelayExecutor();
    RelayExecutor(const RelayExecutor&) = delete;
    RelayExecutor& operator=(const RelayExecutor&) = delete;

    /// A Wire::Call body -> the Wire::CallReply body.
    std::vector<u8> call(std::span<const u8> body);
    /// A Wire::CloseHandle body -> the Wire::CloseReply body.
    std::vector<u8> close(std::span<const u8> body);
    /// Close every engine handle handed to the action.
    void close_all();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/// Serve @p channel's relayed calls until the server sends Wire::Done; its
/// body is returned. nullopt when the connection broke first (the server
/// died). Between messages, @p idle runs every @p idle_ms (the engine thread
/// pumps its UI there).
std::optional<std::vector<u8>> serve_custom_action(RemoteChannel& channel, RelayExecutor& executor,
                                                   const std::function<void()>& idle = {}, int idle_ms = 20);

/// Take / put this process's MsiGetLastErrorRecord record (msi_database.cpp).
[[nodiscard]] std::shared_ptr<Record> take_last_error_record();
void put_last_error_record(std::shared_ptr<Record> record);

}  // namespace vitreo::winapi::msi
