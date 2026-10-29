/// @file test_winhttp.cpp
/// @brief Focused WinHTTP callback contract tests.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/networking/winhttp/winhttp.hpp"
#include "winapi/networking/winhttp/url_components.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <array>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

namespace vitreo::winapi {

class WinHttpContractTestAccessor {
public:
    static void set_receive_deadline(WinHttpManager& manager, u32 request, int ms) {
        manager.m_requests.at(request).receive_timeout.milliseconds = ms;
    }
    static std::string authorization(WinHttpManager& manager, u32 request) {
        return manager.m_requests.at(request).authorization;
    }
    static bool is_websocket_upgrade(WinHttpManager& manager, u32 request) {
        const auto it = manager.m_requests.find(request);
        return it != manager.m_requests.end() &&
               it->second.websocket_upgrade;
    }

    static void set_response_header(
        WinHttpManager& manager, u32 request, std::string name,
        std::string value) {
        WinHttpManager::RequestInfo info;
        info.sent = true;
        info.status_code = 200;
        info.response_headers.emplace(std::move(name), std::move(value));
        manager.m_requests[request] = std::move(info);
        manager.m_objects[request].kind = WinHttpManager::Kind::Request;
    }
};

} // namespace vitreo::winapi

namespace {

ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

class WinHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        cpu.gpr.rsp = 0x1F000;
        marshaler.attach_cpu(cpu);
        exports = create_winhttp_thunks(manager);
    }

    u64 call(const char* name, u64 rcx = 0, u64 rdx = 0,
             u64 r8 = 0, u64 r9 = 0) {
        auto* entry = find_export(exports, name);
        EXPECT_NE(entry, nullptr);
        if (!entry) return 0;
        cpu.gpr.rcx = rcx;
        cpu.gpr.rdx = rdx;
        cpu.gpr.r8 = r8;
        cpu.gpr.r9 = r9;
        entry->handler(marshaler);
        return cpu.gpr.rax;
    }

    u64 call_args(const char* name, bool wide, std::initializer_list<u64> args) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x1F000;
        u64* registers[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
        unsigned index = 0;
        for (u64 value : args) {
            if (wide && index < 4) *registers[index] = value;
            else if (wide) (void)memory.write_u64(cpu.gpr.rsp + 0x28 + (index - 4) * 8, value);
            else (void)memory.write_u32(cpu.gpr.rsp + 4 + index * 4, static_cast<u32>(value));
            ++index;
        }
        find_export(exports, name)->handler(marshaler);
        return cpu.gpr.rax;
    }

    /// A bump allocator over a mapped window stands in for GlobalAlloc.
    void use_heap() {
        ASSERT_TRUE(memory.map_region(0x40000, 0x40000, MemoryProtection::ReadWrite).is_ok());
        manager.set_guest_heap(
            [this](u64 size) { const GuestAddr a = heap_next; heap_next += (size + 15) & ~u64{15}; ++live_blocks; return a; },
            [this](GuestAddr) { --live_blocks; });
    }
    GuestAddr heap_next = 0x40000;
    int live_blocks = 0;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    WinHttpManager manager;
    DllExports exports;
};

} // namespace

TEST_F(WinHttpTest, BasicCredentialsMatchWindowsValidationAndLatin1EncodingBothModes) {
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        const u32 session = call_args("WinHttpOpen", wide, {0,0,0,0,0});
        const u32 connection = call_args("WinHttpConnect", wide, {session,0,80,0});
        const u32 request = call_args("WinHttpOpenRequest", wide, {connection,0,0,0,0,0,0});
        const u32 sibling = call_args("WinHttpOpenRequest", wide, {connection,0,0,0,0,0,0});
        const GuestAddr user = 0x10100, password = 0x10200;
        const u16 user_data[] = {'u',0xe9,0};
        const u16 password_data[] = {'p',0xff,0};
        ASSERT_TRUE(memory.write_bytes(user, reinterpret_cast<const Byte*>(user_data), sizeof(user_data)).is_ok());
        ASSERT_TRUE(memory.write_bytes(password,reinterpret_cast<const Byte*>(password_data),sizeof(password_data)).is_ok());
        for (u32 handle : {0u, session, connection}) {
            EXPECT_EQ(call_args("WinHttpSetCredentials",wide,{handle,0,1,user,password,0}),0u);
            EXPECT_EQ(get_last_error(),6u);
        }
        EXPECT_EQ(call_args("WinHttpSetCredentials",wide,{request,2,1,user,password,0}),0u);
        EXPECT_EQ(get_last_error(),87u);
        EXPECT_EQ(call_args("WinHttpSetCredentials",wide,{request,0,3,user,password,0}),0u);
        EXPECT_EQ(get_last_error(),87u);
        EXPECT_EQ(call_args("WinHttpSetCredentials",wide,{request,0,1,0,password,0}),0u);
        EXPECT_EQ(get_last_error(),87u);
        EXPECT_EQ(call_args("WinHttpSetCredentials",wide,{request,0,1,user,0,0}),0u);
        EXPECT_EQ(get_last_error(),87u);
        // Windows ignores the reserved parameter and clears last-error on success.
        EXPECT_EQ(call_args("WinHttpSetCredentials",wide,{request,0,1,user,password,1}),1u);
        EXPECT_EQ(get_last_error(),0u);
        EXPECT_EQ(WinHttpContractTestAccessor::authorization(manager,request),"Basic dek6cP8=");
        EXPECT_TRUE(WinHttpContractTestAccessor::authorization(manager,sibling).empty());
        ASSERT_TRUE(memory.write_u16(user + 2,0x4e2d).is_ok());
        EXPECT_EQ(call_args("WinHttpSetCredentials",wide,{request,0,1,user,password,0}),0u);
        EXPECT_EQ(get_last_error(),87u);
        EXPECT_EQ(WinHttpContractTestAccessor::authorization(manager,request),"Basic dek6cP8=");
    }
}

TEST_F(WinHttpTest, BasicCredentialsReachTheServer) {
    const int listener = socket(AF_INET,SOCK_STREAM,0);
    ASSERT_GE(listener,0);
    sockaddr_in address{};
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)),0);
    ASSERT_EQ(listen(listener,1),0);
    socklen_t length=sizeof(address);
    ASSERT_EQ(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length),0);
    auto observed = std::async(std::launch::async,[listener] {
        pollfd event{listener,POLLIN,0};
        if (poll(&event,1,5000)!=1) { close(listener); return std::string("timeout"); }
        const int client=accept(listener,nullptr,nullptr);
        close(listener);
        char buffer[4096]{};
        const ssize_t count=recv(client,buffer,sizeof(buffer),0);
        const char response[]="HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(client,response,sizeof(response)-1,0);
        close(client);
        return count>0 ? std::string(buffer,static_cast<size_t>(count)) : std::string();
    });
    ASSERT_TRUE(marshaler.write_wide_string(0x10100,"127.0.0.1").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(0x10200,"user").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(0x10300,"password").is_ok());
    const u32 session=call_args("WinHttpOpen",true,{0,1,0,0,0});
    const u32 connection=call_args("WinHttpConnect",true,{session,0x10100,ntohs(address.sin_port),0});
    const u32 request=call_args("WinHttpOpenRequest",true,{connection,0,0,0,0,0,0});
    ASSERT_EQ(call_args("WinHttpSetCredentials",true,{request,0,1,0x10200,0x10300,0}),1u);
    ASSERT_EQ(call_args("WinHttpSendRequest",true,{request,0,0,0,0,0,0}),1u);
    EXPECT_NE(observed.get().find("Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n"),std::string::npos);
    EXPECT_EQ(call_args("WinHttpReceiveResponse",true,{request,0}),1u);
}

TEST_F(WinHttpTest, TimeoutValidationAndInheritanceBothModes) {
    for (bool wide : {false,true}) {
        const u32 session=static_cast<u32>(call_args("WinHttpOpen",wide,{0,1,0,0,0}));
        const u32 connection=static_cast<u32>(call_args("WinHttpConnect",wide,{session,0,80,0}));
        for (u32 bad : {0u,connection}) {
            EXPECT_EQ(call_args("WinHttpSetTimeouts",wide,{bad,0,0,0,1}),0u);
            EXPECT_EQ(get_last_error(),12018u);
        }
        for (unsigned parameter=1;parameter<=4;++parameter) {
            u64 values[4]{};values[parameter-1]=0xfffffffeu;
            EXPECT_EQ(call_args("WinHttpSetTimeouts",wide,{session,values[0],values[1],values[2],values[3]}),0u);
            EXPECT_EQ(get_last_error(),87u);
        }
        EXPECT_EQ(call_args("WinHttpSetTimeouts",wide,{session,0,0,0,0}),1u);
        EXPECT_EQ(get_last_error(),0u);
        const u32 request=static_cast<u32>(call_args("WinHttpOpenRequest",wide,{connection,0,0,0,0,0,0}));
        ASSERT_TRUE(memory.write_u32(0x10200,4).is_ok());
        EXPECT_EQ(call_args("WinHttpQueryOption",wide,{request,6,0x10100,0x10200}),1u);
        EXPECT_EQ(memory.read_u32(0x10100).value(),0u);
        // Session changes affect later requests, not an existing request.
        EXPECT_EQ(call_args("WinHttpSetTimeouts",wide,{session,0,0,0,0xffffffffu}),1u);
        EXPECT_EQ(call_args("WinHttpQueryOption",wide,{request,6,0x10100,0x10200}),1u);
        EXPECT_EQ(memory.read_u32(0x10100).value(),0u);
        EXPECT_EQ(call_args("WinHttpSetTimeouts",wide,{request,0,1,0,25}),0u);
        EXPECT_EQ(get_last_error(),12004u); // explicit unsupported transport phase
        EXPECT_EQ(call_args("WinHttpSetTimeouts",wide,{request,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu}),1u);
        for (u32 option : {2u,3u,5u,6u}) {
            EXPECT_EQ(call_args("WinHttpQueryOption",wide,{request,option,0x10100,0x10200}),1u);
            EXPECT_EQ(memory.read_u32(0x10100).value(),0xffffffffu);
        }
    }
}

TEST_F(WinHttpTest, ReceiveCancellationInfrastructureStopsTransport) {
    const int listener=socket(AF_INET,SOCK_STREAM,0);
    ASSERT_GE(listener,0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)),0);
    ASSERT_EQ(listen(listener,1),0);
    socklen_t length=sizeof(address);
    ASSERT_EQ(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length),0);
    auto closed=std::async(std::launch::async,[listener] {
        pollfd event{listener,POLLIN,0};
        if (poll(&event,1,5000)!=1) { close(listener);return false; }
        const int client=accept(listener,nullptr,nullptr);close(listener);
        char request[4096];(void)recv(client,request,sizeof(request),0);
        event={client,POLLIN,0};
        const bool result=poll(&event,1,5000)==1 && recv(client,request,sizeof(request),0)<=0;
        close(client);return result;
    });
    ASSERT_TRUE(marshaler.write_wide_string(0x10100,"127.0.0.1").is_ok());
    const u32 session=static_cast<u32>(call_args("WinHttpOpen",true,{0,1,0,0,0}));
    const u32 connection=static_cast<u32>(call_args("WinHttpConnect",true,{session,0x10100,ntohs(address.sin_port),0}));
    ASSERT_EQ(call_args("WinHttpSetTimeouts",true,{session,0,0,0,0}),1u);
    const u32 request=static_cast<u32>(call_args("WinHttpOpenRequest",true,{connection,0,0,0,0,0,0}));
    // Infrastructure test only: finite receive deadlines remain rejected at
    // the public setter until SendRequest exposes the completed-send boundary.
    WinHttpContractTestAccessor::set_receive_deadline(manager,request,100);
    ASSERT_EQ(call_args("WinHttpSendRequest",true,{request,0,0,0,0,0,0}),1u);
    EXPECT_EQ(call_args("WinHttpReceiveResponse",true,{request,0}),0u);
    EXPECT_EQ(get_last_error(),12002u);
    EXPECT_TRUE(closed.get());
}

TEST_F(WinHttpTest, RequestInheritsSessionStatusCallbackAtCreation) {
    const u32 session = static_cast<u32>(call("WinHttpOpen"));
    ASSERT_NE(session, 0u);

    EXPECT_EQ(call("WinHttpSetStatusCallback", session, 0x12345678,
                   0xFFFFFFFFu, 0), 0u);

    const u32 connection = static_cast<u32>(call("WinHttpConnect", session));
    ASSERT_NE(connection, 0u);
    const u32 request =
        static_cast<u32>(call("WinHttpOpenRequest", connection));
    ASSERT_NE(request, 0u);

    EXPECT_EQ(call("WinHttpSetStatusCallback", request, 0x87654321,
                   0xFFFFFFFFu, 0), 0x12345678u);
}

TEST_F(WinHttpTest, SetStatusCallbackRejectsAnUnknownHandle) {
    // http_winhttp_handles_probe setcb-bogus: WINHTTP_INVALID_STATUS_CALLBACK
    // (all pointer bits) with ERROR_INVALID_HANDLE.
    EXPECT_EQ(call("WinHttpSetStatusCallback", 0xBAD0, 0x12345678,
                   0xFFFFFFFFu, 0), ~0ull);
    EXPECT_EQ(get_last_error(), 6u);
}

TEST_F(WinHttpTest, DefaultProxyInfoUsesTheX64StructureLayout) {
    constexpr GuestAddr info = 0x10100;
    std::array<Byte, 24> bytes{};
    bytes.fill(Byte{0xA5});
    ASSERT_TRUE(memory.write_bytes(info, bytes.data(), bytes.size()).is_ok());

    EXPECT_EQ(call("WinHttpGetDefaultProxyConfiguration", info), 1u);
    EXPECT_EQ(memory.read_u32(info).value(), 1u);
    EXPECT_EQ(memory.read_u64(info + 8).value(), 0u);
    EXPECT_EQ(memory.read_u64(info + 16).value(), 0u);
}

TEST_F(WinHttpTest, CurrentUserProxyInfoComesFromSystemSettingsBothModes) {
    // WINHTTP_CURRENT_USER_IE_PROXY_CONFIG: fAutoDetect, lpszAutoConfigUrl,
    // lpszProxy, lpszProxyBypass; x64 32 bytes (0/8/16/24), x86 16 bytes
    // (probe layout ie-config=32 / 16). Strings are GlobalAlloc'd.
    use_heap();
    manager.set_system_proxy_source([] {
        SystemProxyConfig c;
        c.auto_detect = true;
        c.auto_config_url = "http://pac.example/proxy.pac";
        c.proxy = "http=p.example:8080";
        return c;
    });
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        constexpr GuestAddr info = 0x10100;
        std::array<Byte, 32> bytes{};
        bytes.fill(Byte{0xA5});
        ASSERT_TRUE(memory.write_bytes(info, bytes.data(), bytes.size()).is_ok());
        EXPECT_EQ(call_args("WinHttpGetIEProxyConfigForCurrentUser", wide, {info}), 1u);
        const u32 p = wide ? 8 : 4;
        auto ptr = [&](u32 off) { return wide ? memory.read_u64(info + off).value() : memory.read_u32(info + off).value(); };
        EXPECT_EQ(memory.read_u32(info).value(), 1u);
        EXPECT_EQ(marshaler.read_wide_string(ptr(p)).value(), "http://pac.example/proxy.pac");
        EXPECT_EQ(marshaler.read_wide_string(ptr(2 * p)).value(), "http=p.example:8080");
        EXPECT_EQ(ptr(3 * p), 0u);
        EXPECT_EQ(call_args("WinHttpGetIEProxyConfigForCurrentUser", wide, {0}), 0u);
        EXPECT_EQ(get_last_error(), 87u);  // probe ie-config-null
    }
}

TEST_F(WinHttpTest, SendRequestCompletionOnlyInAsyncMode) {
    // Learn WinHttpSendRequest: SENDREQUEST_COMPLETE is delivered only in
    // asynchronous mode; a synchronous session gets no completion callback.
    constexpr GuestAddr server_name = 0x10100;
    ASSERT_TRUE(marshaler.write_wide_string(server_name, "127.0.0.1").is_ok());
    constexpr u32 send_complete = 0x00400000;
    for (u32 flags : {0u, 0x10000000u /* WINHTTP_FLAG_ASYNC */}) {
        SCOPED_TRACE(flags);
        const u32 session = static_cast<u32>(call_args("WinHttpOpen", true, {0, 1, 0, 0, flags}));
        const u32 connection = static_cast<u32>(call("WinHttpConnect", session, server_name, 9));
        const u32 request = static_cast<u32>(call("WinHttpOpenRequest", connection));
        ASSERT_NE(request, 0u);
        constexpr GuestAddr callback_proc = 0x12345678;
        ASSERT_EQ(call("WinHttpSetStatusCallback", request, callback_proc, send_complete, 0), 0u);
        // winhttp_async_flow_probe.expected: SENDREQUEST_COMPLETE comes from a
        // WinHTTP thread after the call returned, never inside it.
        bool inline_completion = false;
        manager.set_status_callback_invoker(
            [&](GuestAddr, u32, u64, u32, GuestAddr, u32) { inline_completion = true; });
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> returned{false};
        u32 observed_status = 0, observed_handle = 0;
        bool after_return = false;
        manager.set_async_callback_invoker(
            [&](GuestAddr, u32 handle, u64, u32 status, GuestAddr, u32) {
                std::lock_guard lock(mutex);
                observed_handle = handle;
                observed_status = status;
                after_return = returned.load();
                cv.notify_all();
            });
        EXPECT_EQ(call_args("WinHttpSendRequest", true, {request, 0, 0, 0, 0, 0, 0}), 1u);
        returned = true;
        EXPECT_FALSE(inline_completion);
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, std::chrono::seconds(5), [&] { return observed_status != 0; });
        }
        if (flags != 0) {
            // Delivery is posted at the end of the call; it may race the
            // test's own flag, but never runs on the calling thread.
            EXPECT_EQ(observed_status, send_complete);
            EXPECT_EQ(observed_handle, request);
        } else {
            EXPECT_EQ(observed_status, 0u);
        }
        (void)after_return;
        call_args("WinHttpCloseHandle", true, {request});
    }
}

// The asynchronous request flow of winhttp_async_flow_probe.expected, driven
// from the completions: each call returns TRUE at once, its completion comes
// from the service thread, and lpvStatusInformation carries what Windows
// gives: DATA_AVAILABLE -> DWORD byte count (len 4), READ_COMPLETE -> the
// caller's buffer and the byte count, the last DATA_AVAILABLE -> 0.
TEST_F(WinHttpTest, AsyncRequestCompletesThroughServiceThreadCallbacks) {
    use_heap();
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    auto server = std::async(std::launch::async, [listener] {
        pollfd event{listener, POLLIN, 0};
        if (poll(&event, 1, 5000) != 1) { close(listener); return; }
        const int client = accept(listener, nullptr, nullptr);
        close(listener);
        char buffer[4096]{};
        (void)recv(client, buffer, sizeof(buffer), 0);
        const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
        (void)send(client, response, sizeof(response) - 1, 0);
        close(client);
    });

    struct Event { u32 status; GuestAddr info; u32 len; u32 dword; bool inside_call; };
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Event> events;
    std::atomic<bool> in_call{false};
    const auto calling_thread = std::this_thread::get_id();
    bool on_calling_thread = false;
    manager.set_status_callback_invoker(
        [&](GuestAddr, u32, u64, u32, GuestAddr, u32) { on_calling_thread = true; });
    manager.set_async_callback_invoker(
        [&](GuestAddr, u32, u64, u32 status, GuestAddr info, u32 len) {
            if (std::this_thread::get_id() == calling_thread) on_calling_thread = true;
            const u32 dword = (info != 0 && len == 4) ? memory.read_u32(info).value() : 0;
            std::lock_guard lock(mutex);
            events.push_back({status, info, len, dword, in_call.load()});
            cv.notify_all();
        });
    auto next = [&](u32 status) -> Event {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !events.empty() && events.back().status == status;
        });
        return events.empty() ? Event{} : events.back();
    };
    auto api = [&](const char* name, std::initializer_list<u64> args) {
        in_call = true;
        const u64 result = call_args(name, true, args);
        in_call = false;
        return result;
    };

    ASSERT_TRUE(marshaler.write_wide_string(0x10100, "127.0.0.1").is_ok());
    const u32 session = static_cast<u32>(call_args("WinHttpOpen", true, {0, 1, 0, 0, 0x10000000u}));
    const u32 connection = static_cast<u32>(call_args(
        "WinHttpConnect", true, {session, 0x10100, ntohs(address.sin_port), 0}));
    const u32 request = static_cast<u32>(call_args("WinHttpOpenRequest", true, {connection, 0, 0, 0, 0, 0, 0}));
    ASSERT_NE(request, 0u);
    ASSERT_EQ(call_args("WinHttpSetStatusCallback", true, {request, 0x1000, 0xFFFFFFFFu, 0}), 0u);

    constexpr u32 SENDREQUEST_COMPLETE = 0x00400000, HEADERS_AVAILABLE = 0x00020000,
                  DATA_AVAILABLE = 0x00040000, READ_COMPLETE = 0x00080000;
    ASSERT_EQ(api("WinHttpSendRequest", {request, 0, 0, 0, 0, 0, 0}), 1u);
    EXPECT_EQ(next(SENDREQUEST_COMPLETE).status, SENDREQUEST_COMPLETE);

    ASSERT_EQ(api("WinHttpReceiveResponse", {request, 0}), 1u);
    EXPECT_EQ(next(HEADERS_AVAILABLE).len, 0u);

    // Async callers pass NULL for the count (Learn).
    ASSERT_EQ(api("WinHttpQueryDataAvailable", {request, 0}), 1u);
    const Event available = next(DATA_AVAILABLE);
    EXPECT_EQ(available.len, 4u);
    EXPECT_EQ(available.dword, 5u);

    constexpr GuestAddr body = 0x10800;
    ASSERT_EQ(api("WinHttpReadData", {request, body, 5, 0}), 1u);
    const Event read = next(READ_COMPLETE);
    EXPECT_EQ(read.info, body);
    EXPECT_EQ(read.len, 5u);
    std::array<Byte, 5> text{};
    ASSERT_TRUE(memory.read_bytes(body, text.data(), text.size()).is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(text.data()), text.size()), "hello");

    {
        std::lock_guard lock(mutex);
        events.clear();
    }
    ASSERT_EQ(api("WinHttpQueryDataAvailable", {request, 0}), 1u);
    const Event end = next(DATA_AVAILABLE);
    EXPECT_EQ(end.len, 4u);
    EXPECT_EQ(end.dword, 0u) << "end of response";

    EXPECT_FALSE(on_calling_thread);
    call_args("WinHttpCloseHandle", true, {request});
    call_args("WinHttpCloseHandle", true, {connection});
    call_args("WinHttpCloseHandle", true, {session});
    server.wait();
}

TEST_F(WinHttpTest, CustomResponseHeaderUsesWinHttpBufferContract) {
    constexpr u32 request = 0xA0014321u;
    constexpr GuestAddr name = 0x10100;
    constexpr GuestAddr output = 0x10300;
    constexpr GuestAddr length = 0x10500;
    constexpr GuestAddr index = 0x10510;
    const std::string proof = "proof-value=:signature:";
    WinHttpContractTestAccessor::set_response_header(
        manager, request, "x-cup-server-proof", proof);
    ASSERT_TRUE(marshaler.write_wide_string(
        name, "X-Cup-Server-Proof").is_ok());
    ASSERT_TRUE(memory.write_u32(length, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(index, 0).is_ok());

    cpu.gpr.rcx = request;
    cpu.gpr.rdx = 65535u; // WINHTTP_QUERY_CUSTOM
    cpu.gpr.r8 = name;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, length).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, index).is_ok());
    auto* query = find_export(exports, "WinHttpQueryHeaders");
    ASSERT_NE(query, nullptr);
    query->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(length).value(),
              static_cast<u32>((proof.size() + 1u) * 2u));

    cpu.gpr.r9 = output;
    query->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(length).value(),
              static_cast<u32>(proof.size() * 2u));
    EXPECT_EQ(memory.read_u32(index).value(), 1u);
    EXPECT_EQ(marshaler.read_wide_string(output).value(), proof);
}

TEST_F(WinHttpTest, UpgradeOptionMarksOnlyARequestHandle) {
    constexpr u32 upgrade_to_websocket = 114;
    const u32 session = static_cast<u32>(call("WinHttpOpen"));
    const u32 connection =
        static_cast<u32>(call("WinHttpConnect", session));
    const u32 request =
        static_cast<u32>(call("WinHttpOpenRequest", connection));

    EXPECT_EQ(call("WinHttpSetOption", request, upgrade_to_websocket, 0, 0),
              1u);
    EXPECT_TRUE(WinHttpContractTestAccessor::is_websocket_upgrade(
        manager, request));

    EXPECT_EQ(call("WinHttpSetOption", session, upgrade_to_websocket, 0, 0),
              0u);
    EXPECT_EQ(get_last_error(), 12018u /* INCORRECT_HANDLE_TYPE */);
}

TEST_F(WinHttpTest, CompleteUpgradeRequiresValidated101Handshake) {
    constexpr u32 upgrade_to_websocket = 114;
    const u32 session = static_cast<u32>(call("WinHttpOpen"));
    const u32 connection =
        static_cast<u32>(call("WinHttpConnect", session));
    const u32 request =
        static_cast<u32>(call("WinHttpOpenRequest", connection));
    ASSERT_EQ(call("WinHttpSetOption", request, upgrade_to_websocket, 0, 0),
              1u);

    EXPECT_EQ(call("WinHttpWebSocketCompleteUpgrade", request, 0x1234), 0u);
    EXPECT_EQ(get_last_error(), 12019u /* INCORRECT_HANDLE_STATE */);
}

// ============================================================================
// Expected values below come from tools/oracle_probe/*.expected (Windows 11).
// ============================================================================

namespace {

// Write a URL_COMPONENTS for one bitness: all lengths set to @p len, no
// buffers (crack in place) unless @p host_buffer is non-zero.
void write_components(GuestMemory& memory, GuestAddr at, bool wide, u32 len,
                      GuestAddr host_buffer = 0, u32 host_len = 0) {
    const auto l = urlcrack::layout(wide);
    std::vector<Byte> zero(l.size, Byte{0});
    ASSERT_TRUE(memory.write_bytes(at, zero.data(), zero.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(at, l.size).is_ok());
    for (int c = 0; c < 6; ++c) ASSERT_TRUE(memory.write_u32(at + l.len[c], len).is_ok());
    ASSERT_TRUE(memory.write_u32(at + l.nscheme, 0x55).is_ok());
    ASSERT_TRUE(memory.write_u16(at + l.port, 0x5555).is_ok());
    if (host_buffer) {
        if (wide) ASSERT_TRUE(memory.write_u64(at + l.ptr[1], host_buffer).is_ok());
        else ASSERT_TRUE(memory.write_u32(at + l.ptr[1], static_cast<u32>(host_buffer)).is_ok());
        ASSERT_TRUE(memory.write_u32(at + l.len[1], host_len).is_ok());
    }
}

} // namespace

TEST_F(WinHttpTest, UrlComponentsLayoutMatchesTheSdkBothBitnesses) {
    // http_crackurl_probe(.x86).expected line 1.
    const auto x64 = urlcrack::layout(true);
    EXPECT_EQ(x64.size, 104u);
    EXPECT_EQ(x64.ptr[0], 8u); EXPECT_EQ(x64.nscheme, 20u); EXPECT_EQ(x64.ptr[1], 24u);
    EXPECT_EQ(x64.port, 36u); EXPECT_EQ(x64.ptr[2], 40u); EXPECT_EQ(x64.ptr[3], 56u);
    EXPECT_EQ(x64.ptr[4], 72u); EXPECT_EQ(x64.ptr[5], 88u); EXPECT_EQ(x64.len[5], 96u);
    const auto x86 = urlcrack::layout(false);
    EXPECT_EQ(x86.size, 60u);
    EXPECT_EQ(x86.ptr[0], 4u); EXPECT_EQ(x86.nscheme, 12u); EXPECT_EQ(x86.ptr[1], 16u);
    EXPECT_EQ(x86.port, 24u); EXPECT_EQ(x86.ptr[2], 28u); EXPECT_EQ(x86.ptr[3], 36u);
    EXPECT_EQ(x86.ptr[4], 44u); EXPECT_EQ(x86.ptr[5], 52u); EXPECT_EQ(x86.len[5], 56u);
}

TEST_F(WinHttpTest, CrackUrlInPlaceCopyAndErrorsBothModes) {
    constexpr GuestAddr url = 0x10100, comps = 0x10800, host = 0x10A00;
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        const auto l = urlcrack::layout(wide);
        auto ptr = [&](int c) { return wide ? memory.read_u64(comps + l.ptr[c]).value() : memory.read_u32(comps + l.ptr[c]).value(); };
        auto len = [&](int c) { return memory.read_u32(comps + l.len[c]).value(); };
        // H-inplace#0
        ASSERT_TRUE(marshaler.write_wide_string(url, "http://user:pass@www.example.com:8080/path/file.html?query=1#frag").is_ok());
        write_components(memory, comps, wide, 1);
        set_last_error(0xDEAD);
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0, comps}), 1u);
        EXPECT_EQ(get_last_error(), 0u);
        EXPECT_EQ(memory.read_u32(comps + l.nscheme).value(), 1u);
        EXPECT_EQ(memory.read_u16(comps + l.port).value(), 8080u);
        EXPECT_EQ(ptr(1), url + 17 * 2); EXPECT_EQ(len(1), 15u);
        EXPECT_EQ(ptr(2), url + 7 * 2); EXPECT_EQ(len(2), 4u);
        EXPECT_EQ(ptr(4), url + 37 * 2); EXPECT_EQ(len(4), 15u);
        EXPECT_EQ(ptr(5), url + 52 * 2); EXPECT_EQ(len(5), 13u);
        // H-smallhost: copy with a 3-character host buffer.
        ASSERT_TRUE(marshaler.write_wide_string(url, "http://abcd/x?y").is_ok());
        write_components(memory, comps, wide, 0, host, 3);
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0, comps}), 0u);
        EXPECT_EQ(get_last_error(), 122u);
        EXPECT_EQ(len(1), 5u);
        EXPECT_EQ(memory.read_u16(host).value(), 0u);
        EXPECT_EQ(memory.read_u32(comps + l.nscheme).value(), 1u);
        // H-copy with a 5-character buffer succeeds.
        write_components(memory, comps, wide, 0, host, 5);
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0, comps}), 1u);
        EXPECT_EQ(len(1), 4u);
        EXPECT_EQ(marshaler.read_wide_string(host).value(), "abcd");
        // H-inplace#4 (file:) and #8 ("http://").
        ASSERT_TRUE(marshaler.write_wide_string(url, "file:///C:/dir/file.txt").is_ok());
        write_components(memory, comps, wide, 1);
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0, comps}), 0u);
        EXPECT_EQ(get_last_error(), 12006u);
        EXPECT_EQ(memory.read_u32(comps + l.nscheme).value(), 0x55u);
        ASSERT_TRUE(marshaler.write_wide_string(url, "http://").is_ok());
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0, comps}), 0u);
        EXPECT_EQ(get_last_error(), 12005u);
        // H-size-other / H-decode-inplace / H-flag1 / H-nullcomp.
        ASSERT_TRUE(marshaler.write_wide_string(url, "http://h/p").is_ok());
        ASSERT_TRUE(memory.write_u32(comps, wide ? 60u : 104u).is_ok());
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0, comps}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
        write_components(memory, comps, wide, 1);
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0x10000000u, comps}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 1, comps}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
        EXPECT_EQ(call_args("WinHttpCrackUrl", wide, {url, 0, 0, 0}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
    }
}

TEST_F(WinHttpTest, CrackUrlEscapeAndDecodeRules) {
    // H-escape-copy, H-decode-copy (x64).
    constexpr GuestAddr url = 0x10100, comps = 0x10800, path = 0x10A00, extra = 0x10C00;
    auto crack = [&](const char* text, u32 flags) {
        std::u16string units;
        for (const char* p = text; *p; ++p) units.push_back(*p == 1 ? u'\u00e9' : static_cast<char16_t>(static_cast<u8>(*p)));
        for (usize i = 0; i <= units.size(); ++i)
            (void)memory.write_u16(url + i * 2, i < units.size() ? units[i] : 0);
        write_components(memory, comps, true, 0);
        const auto l = urlcrack::layout(true);
        (void)memory.write_u64(comps + l.ptr[4], path); (void)memory.write_u32(comps + l.len[4], 64);
        (void)memory.write_u64(comps + l.ptr[5], extra); (void)memory.write_u32(comps + l.len[5], 64);
        return call_args("WinHttpCrackUrl", true, {url, 0, flags, comps});
    };
    ASSERT_EQ(crack("http://host/a b\x01<>\"{}|^`?x y#f g", 0x80000000u), 1u);
    EXPECT_EQ(marshaler.read_wide_string(path).value(), "/a%20b%C3%A9%3C%3E%22%7B%7D%7C%5E%60");
    EXPECT_EQ(marshaler.read_wide_string(extra).value(), "?x%20y%23f%20g");
    ASSERT_EQ(crack("http://h/a%20b%zz%7e?x=%41%2", 0x10000000u), 1u);
    EXPECT_EQ(marshaler.read_wide_string(path).value(), "/a b%zz~");
    EXPECT_EQ(marshaler.read_wide_string(extra).value(), "?x=A%2");
}

TEST_F(WinHttpTest, SetStatusCallbackErrorsAndSentinelBothModes) {
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        const u64 invalid = wide ? ~0ull : 0xFFFFFFFFull;
        const u32 session = static_cast<u32>(call_args("WinHttpOpen", wide, {0, 1, 0, 0, 0}));
        EXPECT_EQ(call_args("WinHttpSetStatusCallback", wide, {0, 0x1000, 0xFFFFFFFFu, 0}) & invalid, invalid);
        EXPECT_EQ(get_last_error(), 12018u);  // setcb-null
        EXPECT_EQ(call_args("WinHttpSetStatusCallback", wide, {session, 0x1000, 0, 0}) & invalid, invalid);
        EXPECT_EQ(get_last_error(), 87u);     // setcb-flags0
        EXPECT_EQ(call_args("WinHttpSetStatusCallback", wide, {session, 0x1000, 0x800, 5}) & invalid, invalid);
        EXPECT_EQ(get_last_error(), 87u);     // setcb-reserved
        set_last_error(0xDEAD);
        EXPECT_EQ(call_args("WinHttpSetStatusCallback", wide, {session, 0x1000, 0x800, 0}), 0u);
        EXPECT_EQ(get_last_error(), 0u);      // setcb-first
        EXPECT_EQ(call_args("WinHttpSetStatusCallback", wide, {session, 0, 0, 0}), 0x1000u);
    }
}

TEST_F(WinHttpTest, CloseHandleErrorsAndHandleClosingOrderBothModes) {
    use_heap();
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        EXPECT_EQ(call_args("WinHttpCloseHandle", wide, {0}), 0u);
        EXPECT_EQ(get_last_error(), 6u);      // close-null
        EXPECT_EQ(call_args("WinHttpCloseHandle", wide, {0x1234}), 0u);
        EXPECT_EQ(get_last_error(), 6u);      // close-bogus
        std::vector<std::tuple<u32, u32, u64, u64, u32>> seen;
        manager.set_status_callback_invoker(
            [&](GuestAddr, u32 handle, u64 context, u32 status, GuestAddr info, u32 len) {
                const u64 value = wide ? memory.read_u64(info).value() : memory.read_u32(info).value();
                seen.emplace_back(handle, status, context, value, len);
            });
        const u32 session = static_cast<u32>(call_args("WinHttpOpen", wide, {0, 1, 0, 0, 0}));
        ASSERT_EQ(call_args("WinHttpSetStatusCallback", wide, {session, 0x1000, 0xC00, 0}), 0u);
        ASSERT_TRUE(memory.write_u64(0x10300, 0x1111).is_ok());
        ASSERT_EQ(call_args("WinHttpSetOption", wide, {session, 45, 0x10300, wide ? 8u : 4u}), 1u);
        const u32 conn = static_cast<u32>(call_args("WinHttpConnect", wide, {session, 0, 9, 0}));
        // created-connect-ctx: HANDLE_CREATED with the inherited context,
        // lpvStatusInformation -> the handle, length = pointer size.
        ASSERT_EQ(seen.size(), 1u);
        EXPECT_EQ(std::get<0>(seen[0]), conn);
        EXPECT_EQ(std::get<1>(seen[0]), 0x400u);
        EXPECT_EQ(std::get<2>(seen[0]), 0x1111u);
        EXPECT_EQ(std::get<3>(seen[0]), conn);
        EXPECT_EQ(std::get<4>(seen[0]), wide ? 8u : 4u);
        seen.clear();
        // closing-parent-first: nothing until the child closes, then the
        // child's and the parent's HANDLE_CLOSING, in that order.
        EXPECT_EQ(call_args("WinHttpCloseHandle", wide, {session}), 1u);
        EXPECT_TRUE(seen.empty());
        EXPECT_EQ(call_args("WinHttpCloseHandle", wide, {conn}), 1u);
        ASSERT_EQ(seen.size(), 2u);
        EXPECT_EQ(std::get<0>(seen[0]), conn);
        EXPECT_EQ(std::get<1>(seen[0]), 0x800u);
        EXPECT_EQ(std::get<0>(seen[1]), session);
        EXPECT_EQ(call_args("WinHttpCloseHandle", wide, {conn}), 0u);
        EXPECT_EQ(get_last_error(), 6u);      // close-twice
    }
    EXPECT_EQ(live_blocks, 0);
}

TEST_F(WinHttpTest, BadHandlesUseMeasuredErrors) {
    // http_winhttp_handles_probe *-null / *-bogus / *-session.
    const u32 session = static_cast<u32>(call("WinHttpOpen"));
    for (const char* api : {"WinHttpReceiveResponse", "WinHttpQueryDataAvailable"}) {
        SCOPED_TRACE(api);
        EXPECT_EQ(call(api, 0, 0x10100), 0u);
        EXPECT_EQ(get_last_error(), 6u);
        EXPECT_EQ(call(api, session, 0x10100), 0u);
        EXPECT_EQ(get_last_error(), 12018u);
    }
    ASSERT_TRUE(memory.write_u32(0x10100, 77).is_ok());
    EXPECT_EQ(call("WinHttpReadData", 0x1234, 0x10200, 4, 0x10100), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(memory.read_u32(0x10100).value(), 77u);  // count untouched
    EXPECT_EQ(call("WinHttpSetOption", 0, 45, 0x10200, 8), 0u);
    EXPECT_EQ(get_last_error(), 12018u);
    EXPECT_EQ(call("WinHttpSetOption", session, 45, 0x10200, 4), 0u);
    EXPECT_EQ(get_last_error(), 122u);  // set-option-session (x64)
    const u32 conn = static_cast<u32>(call("WinHttpConnect", session));
    const u32 request = static_cast<u32>(call("WinHttpOpenRequest", conn));
    EXPECT_EQ(call("WinHttpReceiveResponse", request), 0u);
    EXPECT_EQ(get_last_error(), 12019u);  // receive-unsent
    EXPECT_EQ(call("WinHttpWriteData", request, 0x10200, 1, 0x10100), 0u);
    EXPECT_EQ(get_last_error(), 12019u);  // write-before-send
    EXPECT_EQ(memory.read_u32(0x10100).value(), 77u);
}

TEST_F(WinHttpTest, AddRequestHeadersModifiers) {
    // http_request_probe add-* and the echoed wire headers.
    const u32 session = static_cast<u32>(call("WinHttpOpen"));
    const u32 request = static_cast<u32>(call("WinHttpOpenRequest", static_cast<u32>(call("WinHttpConnect", session))));
    auto add = [&](const char* text, u32 modifiers) {
        (void)marshaler.write_wide_string(0x10100, text);
        return call("WinHttpAddRequestHeaders", request, 0x10100, 0xFFFFFFFFu, modifiers);
    };
    auto request_header = [&](const char* name, u32 index) -> std::string {
        (void)marshaler.write_wide_string(0x10300, name);
        (void)memory.write_u32(0x10500, 512);
        (void)memory.write_u32(0x10510, index);
        cpu.gpr.rcx = request; cpu.gpr.rdx = 0x80000000u | 65535u; cpu.gpr.r8 = 0x10300; cpu.gpr.r9 = 0x10600;
        (void)memory.write_u64(cpu.gpr.rsp + 0x28, 0x10500);
        (void)memory.write_u64(cpu.gpr.rsp + 0x30, 0x10510);
        find_export(exports, "WinHttpQueryHeaders")->handler(marshaler);
        return cpu.gpr.rax ? marshaler.read_wide_string(0x10600).value() : std::string("<none>");
    };
    EXPECT_EQ(add("X-D: 1", 0x20000000), 1u);
    EXPECT_EQ(add("X-D: 2", 0x20000000), 1u);
    EXPECT_EQ(request_header("X-D", 1), "2");
    EXPECT_EQ(add("X-E: 1", 0x20000000), 1u);
    EXPECT_EQ(add("X-E: 2", 0x10000000), 0u);
    EXPECT_EQ(get_last_error(), 12155u);
    EXPECT_EQ(add("X-F: 1", 0x20000000), 1u);
    EXPECT_EQ(add("X-F: 2", 0x40000000), 1u);
    EXPECT_EQ(request_header("X-F", 0), "1, 2");
    EXPECT_EQ(add("X-G: 1", 0x20000000), 1u);
    EXPECT_EQ(add("X-G: 2", 0x01000000), 1u);
    EXPECT_EQ(request_header("X-G", 0), "1; 2");
    EXPECT_EQ(add("X-H: 1", 0x20000000), 1u);
    EXPECT_EQ(add("X-H: 2", 0x80000000), 1u);
    EXPECT_EQ(request_header("X-H", 0), "2");
    EXPECT_EQ(add("X-I:", 0x80000000), 0u);
    EXPECT_EQ(get_last_error(), 12150u);
    EXPECT_EQ(add("X-J: 2", 0x80000000), 0u);
    EXPECT_EQ(get_last_error(), 12150u);
    EXPECT_EQ(add("X-K: 2", 0xA0000000), 1u);
    EXPECT_EQ(add("NoColon", 0x20000000), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(add("", 0x20000000), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(add("X-R: 1", 1), 1u);  // index bits ignored
}

TEST_F(WinHttpTest, DefaultPortAndTlsComeOnlyFromTheFlag) {
    // http_request_probe url-port0 / -secure / url-port443-plain.
    ASSERT_TRUE(marshaler.write_wide_string(0x10100, "127.0.0.1").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(0x10180, "/p").is_ok());
    const u32 session = static_cast<u32>(call("WinHttpOpen"));
    struct Case { u16 port; u32 flags; const char* url; };
    for (const Case& c : {Case{0, 0, "http://127.0.0.1/p"}, Case{0, 0x00800000, "https://127.0.0.1/p"},
                          Case{443, 0, "http://127.0.0.1:443/p"}, Case{80, 0x00800000, "https://127.0.0.1:80/p"}}) {
        const u32 conn = static_cast<u32>(call("WinHttpConnect", session, 0x10100, c.port));
        const u32 request = static_cast<u32>(call_args("WinHttpOpenRequest", true, {conn, 0, 0x10180, 0, 0, 0, c.flags}));
        ASSERT_TRUE(memory.write_u32(0x10500, 256).is_ok());
        cpu.init_long_mode(); cpu.gpr.rsp = 0x1F000;
        EXPECT_EQ(call("WinHttpQueryOption", request, 34, 0x10600, 0x10500), 1u);
        EXPECT_EQ(marshaler.read_wide_string(0x10600).value(), c.url);
    }
}

TEST_F(WinHttpTest, PacResultParsingFollowsWindows) {
    // http_winhttp_handles_probe ex-* result entries.
    auto e = parse_pac_result("PROXY p1.example:8080; SOCKS s1.example:1080; DIRECT");
    ASSERT_EQ(e.size(), 3u);
    EXPECT_TRUE(e[0].proxy); EXPECT_EQ(e[0].scheme, 1u); EXPECT_EQ(e[0].host, "p1.example"); EXPECT_EQ(e[0].port, 8080u);
    EXPECT_EQ(e[1].scheme, 4u); EXPECT_EQ(e[1].port, 1080u);
    EXPECT_FALSE(e[2].proxy); EXPECT_TRUE(e[2].bypass); EXPECT_EQ(e[2].scheme, 0u);
    e = parse_pac_result("HTTPS h.example:8443; PROXY p2.example");
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].scheme, 2u); EXPECT_EQ(e[1].port, 80u); EXPECT_EQ(e[1].text, "p2.example");
    EXPECT_EQ(parse_pac_result("PROXY a.example:1; DIRECT; PROXY b.example:2").size(), 2u);
    EXPECT_EQ(parse_pac_result("SOCKS5 s.example:1").size(), 0u);
    EXPECT_EQ(parse_pac_result("proxy l.example:3").size(), 0u);
    e = parse_pac_result("");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_TRUE(e[0].bypass);
    e = parse_pac_result("  PROXY   w.example:5 ;;  DIRECT  ");
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].host, "w.example");
}

TEST_F(WinHttpTest, ProxyScriptArgumentsMatchWindows) {
    // http_winhttp_handles_probe ex-urlhex-* and url/url2.pac.
    std::string url, host;
    ASSERT_EQ(urlcrack::proxy_script_arguments(u"http://u:p@A.Example:81/x?y#z", url, host), 0u);
    EXPECT_EQ(url, "http://A.Example:81/x?y#z");
    EXPECT_EQ(host, "A.Example");
    ASSERT_EQ(urlcrack::proxy_script_arguments(u"https://u:p@A.Example:444/x?y#z", url, host), 0u);
    EXPECT_EQ(url, "https://A.Example:444/");
    ASSERT_EQ(urlcrack::proxy_script_arguments(u"HTTP://Example.COM", url, host), 0u);
    EXPECT_EQ(url, "http://Example.COM/");
    EXPECT_EQ(urlcrack::proxy_script_arguments(u"not a url", url, host), 12006u);
}

TEST_F(WinHttpTest, GetProxyForUrlFormatsProxyInfoBothModes) {
    use_heap();
    ProxyResolution next;
    manager.set_proxy_engine([&](const ProxyQuery&) { return next; });
    ASSERT_TRUE(marshaler.write_wide_string(0x10100, "http://a.example/x").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(0x10200, "http://pac/x.pac").is_ok());
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        const u32 session = static_cast<u32>(call_args("WinHttpOpen", wide, {0, 1, 0, 0, 0}));
        // WINHTTP_AUTOPROXY_OPTIONS {CONFIG_URL, 0, url}
        std::vector<Byte> options(32, Byte{0});
        ASSERT_TRUE(memory.write_bytes(0x10400, options.data(), options.size()).is_ok());
        ASSERT_TRUE(memory.write_u32(0x10400, 2).is_ok());
        if (wide) ASSERT_TRUE(memory.write_u64(0x10408, 0x10200).is_ok());
        else ASSERT_TRUE(memory.write_u32(0x10408, 0x10200).is_ok());
        const u32 p = wide ? 8 : 4;
        next = ProxyResolution{ProxyStatus::Ok, parse_pac_result("HTTPS h.example:8443; PROXY p2.example")};
        EXPECT_EQ(call_args("WinHttpGetProxyForUrl", wide, {session, 0x10100, 0x10400, 0x10500}), 1u);
        EXPECT_EQ(memory.read_u32(0x10500).value(), 3u);
        const u64 text = wide ? memory.read_u64(0x10500 + p).value() : memory.read_u32(0x10500 + p).value();
        EXPECT_EQ(marshaler.read_wide_string(text).value(), "https://h.example:8443;p2.example");
        next = ProxyResolution{ProxyStatus::Ok, parse_pac_result("SOCKS s.example:1; PROXY p.example:2")};
        EXPECT_EQ(call_args("WinHttpGetProxyForUrl", wide, {session, 0x10100, 0x10400, 0x10500}), 1u);
        EXPECT_EQ(memory.read_u32(0x10500).value(), 1u);
        next = ProxyResolution{ProxyStatus::ScriptError, {}};
        EXPECT_EQ(call_args("WinHttpGetProxyForUrl", wide, {session, 0x10100, 0x10400, 0x10500}), 0u);
        EXPECT_EQ(get_last_error(), 12166u);
        EXPECT_EQ(memory.read_u32(0x10500).value(), 0u);
        next = ProxyResolution{ProxyStatus::DownloadFailed, {}};
        EXPECT_EQ(call_args("WinHttpGetProxyForUrl", wide, {session, 0x10100, 0x10400, 0x10500}), 0u);
        EXPECT_EQ(get_last_error(), 12167u);
        EXPECT_EQ(call_args("WinHttpGetProxyForUrl", wide, {0, 0x10100, 0x10400, 0x10500}), 0u);
        EXPECT_EQ(get_last_error(), 6u);
        EXPECT_EQ(call_args("WinHttpGetProxyForUrl", wide, {session, 0, 0x10400, 0x10500}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
    }
}

TEST_F(WinHttpTest, ProxyResolverCompletesOnTheServiceThreadBothModes) {
    use_heap();
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::tuple<u32, u64, u32, u64, u32>> callbacks;  // handle, ctx, status, api, error
    manager.set_async_callback_invoker(
        [&](GuestAddr, u32 handle, u64 context, u32 status, GuestAddr info, u32 len) {
            u64 api = 0; u32 error = 0;
            if (info) {
                api = len == 16 ? memory.read_u64(info).value() : memory.read_u32(info).value();
                error = memory.read_u32(info + len / 2).value();
            }
            std::lock_guard lock(mutex);
            callbacks.emplace_back(handle, context, status, api, error);
            cv.notify_all();
        });
    ProxyResolution next;
    manager.set_proxy_engine([&](const ProxyQuery&) { return next; });
    ASSERT_TRUE(marshaler.write_wide_string(0x10100, "http://a.example/x").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(0x10200, "http://pac/x.pac").is_ok());
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        const u32 p = wide ? 8 : 4;
        const u32 sync = static_cast<u32>(call_args("WinHttpOpen", wide, {0, 1, 0, 0, 0}));
        const u32 session = static_cast<u32>(call_args("WinHttpOpen", wide, {0, 1, 0, 0, 0x10000000u}));
        ASSERT_EQ(call_args("WinHttpSetStatusCallback", wide, {session, 0x1000, 0x01200000u, 0}), 0u);
        EXPECT_EQ(call_args("WinHttpCreateProxyResolver", wide, {0, 0x10300}), 6u);
        EXPECT_EQ(call_args("WinHttpCreateProxyResolver", wide, {sync, 0x10300}), 12018u);
        set_last_error(0xDEAD);
        EXPECT_EQ(call_args("WinHttpCreateProxyResolver", wide, {session, 0x10300}), 0u);
        EXPECT_EQ(get_last_error(), 0u);
        const u32 resolver = memory.read_u32(0x10300).value();
        EXPECT_EQ(call_args("WinHttpGetProxyResult", wide, {resolver, 0x10600}), 12019u);
        std::vector<Byte> options(32, Byte{0});
        ASSERT_TRUE(memory.write_bytes(0x10400, options.data(), options.size()).is_ok());
        EXPECT_EQ(call_args("WinHttpGetProxyForUrlEx", wide, {resolver, 0x10100, 0x10400, 0x4242}), 87u);
        ASSERT_TRUE(memory.write_u32(0x10400, 2).is_ok());
        if (wide) ASSERT_TRUE(memory.write_u64(0x10408, 0x10200).is_ok());
        else ASSERT_TRUE(memory.write_u32(0x10408, 0x10200).is_ok());
        EXPECT_EQ(call_args("WinHttpGetProxyForUrlEx", wide, {session, 0x10100, 0x10400, 0x4242}), 12018u);
        next = ProxyResolution{ProxyStatus::Ok, parse_pac_result("PROXY p1.example:8080; SOCKS s1.example:1080; DIRECT")};
        {
            std::lock_guard lock(mutex);
            callbacks.clear();
        }
        EXPECT_EQ(call_args("WinHttpGetProxyForUrlEx", wide, {resolver, 0x10100, 0x10400, 0x4242}), 997u);
        EXPECT_EQ(get_last_error(), 1008u);
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return !callbacks.empty(); }));
            EXPECT_EQ(std::get<0>(callbacks[0]), resolver);
            EXPECT_EQ(std::get<1>(callbacks[0]), 0x4242u);
            EXPECT_EQ(std::get<2>(callbacks[0]), 0x01000000u);
        }
        EXPECT_EQ(call_args("WinHttpGetProxyResult", wide, {resolver, 0x10600}), 0u);
        EXPECT_EQ(memory.read_u32(0x10600).value(), 3u);
        const u64 entries = wide ? memory.read_u64(0x10600 + p).value() : memory.read_u32(0x10600 + p).value();
        const u32 entry_size = wide ? 32 : 20;  // probe layout entry=32 / 20
        EXPECT_EQ(memory.read_u32(entries).value(), 1u);
        EXPECT_EQ(memory.read_u32(entries + 8).value(), 1u);
        const u64 host = wide ? memory.read_u64(entries + 16).value() : memory.read_u32(entries + 12).value();
        EXPECT_EQ(marshaler.read_wide_string(host).value(), "p1.example");
        EXPECT_EQ(memory.read_u16(entries + (wide ? 24 : 16)).value(), 8080u);
        EXPECT_EQ(memory.read_u32(entries + entry_size + 8).value(), 4u);
        EXPECT_EQ(memory.read_u32(entries + 2 * entry_size + 4).value(), 1u);  // DIRECT: fBypass
        call_args("WinHttpFreeProxyResult", wide, {0x10600});
        EXPECT_EQ(memory.read_u32(0x10600).value(), 0u);
        EXPECT_EQ(call_args("WinHttpGetProxyForUrlEx", wide, {resolver, 0x10100, 0x10400, 1}), 12019u);

        // Failure: REQUEST_ERROR with WINHTTP_ASYNC_RESULT {API_GET_PROXY_FOR_URL, error}.
        ASSERT_EQ(call_args("WinHttpCreateProxyResolver", wide, {session, 0x10300}), 0u);
        const u32 second = memory.read_u32(0x10300).value();
        next = ProxyResolution{ProxyStatus::ScriptError, {}};
        {
            std::lock_guard lock(mutex);
            callbacks.clear();
        }
        EXPECT_EQ(call_args("WinHttpGetProxyForUrlEx", wide, {second, 0x10100, 0x10400, 7}), 997u);
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return !callbacks.empty(); }));
            EXPECT_EQ(std::get<2>(callbacks[0]), 0x00200000u);
            EXPECT_EQ(std::get<3>(callbacks[0]), 6u);
            EXPECT_EQ(std::get<4>(callbacks[0]), 12177u);
        }
        EXPECT_EQ(call_args("WinHttpGetProxyResult", wide, {second, 0x10600}), 12019u);
        EXPECT_EQ(call_args("WinHttpCloseHandle", wide, {second}), 1u);
        EXPECT_EQ(call_args("WinHttpGetProxyResult", wide, {second, 0x10600}), 6u);
    }
}
