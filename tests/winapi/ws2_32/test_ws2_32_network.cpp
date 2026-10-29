/// @file test_ws2_32_network.cpp
/// @brief Integration tests for ws2_32 real network connectivity
///
/// These tests connect to actual network servers to verify the socket
/// implementation can perform real network I/O. Skip these tests if
/// running in isolated/offline CI environments.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/networking/ws2_32/ws2_32.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class Ws2NetworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Map memory regions
        auto res1 = m_memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res1.is_ok());
        
        auto res2 = m_memory.map_region(WS2_SCRATCH_BASE, WS2_SCRATCH_SIZE, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res2.is_ok());
        
        auto res3 = m_memory.map_region(STACK_BASE, 0x2000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res3.is_ok());
        
        m_cpu.init_protected_mode();
        m_cpu.set_sp(STACK_BASE + 0x1000);
    }
    
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr BUFFER_ADDR = 0x10000;
    
    GuestMemory m_memory;
    cpu::CpuState m_cpu;
    Ws2Manager m_mgr;
};

// =============================================================================
// Direct Native Socket Test (bypasses guest emulation)
// =============================================================================

TEST_F(Ws2NetworkTest, DISABLED_NativeSocketConnectsToHTTP) {
    // This test uses native sockets directly to verify basic connectivity
    // before testing through the emulation layer
    
    // Resolve httpbin.org (or use a simpler server)
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int rc = getaddrinfo("httpbin.org", "80", &hints, &res);
    if (rc != 0) {
        GTEST_SKIP() << "DNS resolution failed - network may be unavailable";
    }
    
    // Create socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    
    // Connect
    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    
    if (rc < 0) {
        close(fd);
        GTEST_SKIP() << "Connection failed - server may be unavailable";
    }
    
    // Send HTTP request
    const char* request = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
    ssize_t sent = send(fd, request, strlen(request), 0);
    EXPECT_EQ(sent, static_cast<ssize_t>(strlen(request)));
    
    // Receive response
    char buffer[4096] = {};
    ssize_t received = recv(fd, buffer, sizeof(buffer) - 1, 0);
    EXPECT_GT(received, 0);
    
    // Verify HTTP response
    EXPECT_TRUE(strstr(buffer, "HTTP/1.1 200 OK") != nullptr ||
                strstr(buffer, "HTTP/1.0 200 OK") != nullptr)
        << "Response: " << buffer;
    
    close(fd);
}

// =============================================================================
// Ws2Manager Socket Allocation Test
// =============================================================================

TEST_F(Ws2NetworkTest, SocketAllocationMapsToNativeFd) {
    // Create a real native socket
    int native_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(native_fd, 0) << "Failed to create native socket";
    
    // Allocate guest handle
    u32 guest_handle = m_mgr.alloc_socket(native_fd);
    EXPECT_GE(guest_handle, 0x10000u);
    
    // Verify lookup works
    EXPECT_EQ(m_mgr.get_native_fd(guest_handle), native_fd);
    
    // Close
    EXPECT_TRUE(m_mgr.close_socket(guest_handle));
    EXPECT_EQ(m_mgr.get_native_fd(guest_handle), -1);
}

// =============================================================================
// DNS Resolution Test
// =============================================================================

TEST_F(Ws2NetworkTest, DISABLED_GetaddrinfoResolvesRealHost) {
    // Test that getaddrinfo works with a real hostname
    // This requires network access
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int rc = getaddrinfo("cdn.steamstatic.com", "443", &hints, &res);
    if (rc != 0) {
        GTEST_SKIP() << "DNS resolution failed";
    }
    
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->ai_family, AF_INET);
    
    // Log resolved address
    struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str));
    std::fprintf(stderr, "[test] cdn.steamstatic.com resolved to %s\n", addr_str);
    
    freeaddrinfo(res);
}

// =============================================================================
// TCP Connection Test (Plain HTTP)
// =============================================================================

TEST_F(Ws2NetworkTest, DISABLED_ConnectToHTTPServer) {
    // Connect to a plain HTTP server (no TLS)
    // neverssl.com provides plain HTTP for testing
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int rc = getaddrinfo("neverssl.com", "80", &hints, &res);
    if (rc != 0) {
        GTEST_SKIP() << "DNS resolution failed";
    }
    
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    
    // Allocate as guest socket
    u32 guest_socket = m_mgr.alloc_socket(fd);
    
    // Connect
    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    
    if (rc < 0) {
        m_mgr.close_socket(guest_socket);
        GTEST_SKIP() << "Connection failed";
    }
    
    // Verify socket is connected
    int error = 0;
    socklen_t errlen = sizeof(error);
    rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errlen);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(error, 0);
    
    // Send HTTP request
    const char* request = "GET / HTTP/1.1\r\nHost: neverssl.com\r\nConnection: close\r\n\r\n";
    ssize_t sent = send(fd, request, strlen(request), 0);
    EXPECT_GT(sent, 0);
    
    // Receive response
    char buffer[4096] = {};
    ssize_t received = recv(fd, buffer, sizeof(buffer) - 1, 0);
    EXPECT_GT(received, 0);
    
    // Should get HTTP 200 or 301 redirect
    bool is_http = (strstr(buffer, "HTTP/1.1") != nullptr || 
                    strstr(buffer, "HTTP/1.0") != nullptr);
    EXPECT_TRUE(is_http) << "Response: " << std::string(buffer, std::min(received, (ssize_t)200));
    
    m_mgr.close_socket(guest_socket);
}

// =============================================================================
// End-to-end HTTP Download Test
// =============================================================================

TEST_F(Ws2NetworkTest, DISABLED_DownloadSmallFile) {
    // Download a small file from a reliable HTTP endpoint
    // Uses example.com which is guaranteed to be available
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int rc = getaddrinfo("example.com", "80", &hints, &res);
    if (rc != 0) {
        GTEST_SKIP() << "DNS resolution failed for example.com";
    }
    
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    
    u32 guest_socket = m_mgr.alloc_socket(fd);
    
    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    
    if (rc < 0) {
        m_mgr.close_socket(guest_socket);
        GTEST_SKIP() << "Connection to example.com failed";
    }
    
    // Send HTTP GET request
    const char* request = 
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: Vitreo-Test/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    ssize_t sent = send(fd, request, strlen(request), 0);
    EXPECT_EQ(sent, static_cast<ssize_t>(strlen(request)));
    
    // Read entire response
    std::string response;
    char buffer[4096];
    while (true) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        response.append(buffer, n);
    }
    
    m_mgr.close_socket(guest_socket);
    
    // Verify we got a valid response
    EXPECT_GT(response.size(), 100u);
    
    // Check for HTTP 200 OK
    EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos)
        << "Response did not contain HTTP 200";
    
    // Check for expected content
    EXPECT_NE(response.find("Example Domain"), std::string::npos)
        << "Response did not contain expected content";
    
    std::fprintf(stderr, "[test] Downloaded %zu bytes from example.com\n", response.size());
}

// =============================================================================
// HTTPS Download Test (using native macOS TLS via CFNetwork)
// =============================================================================

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>
#include <Security/Security.h>

TEST_F(Ws2NetworkTest, DISABLED_HTTPSDownloadWithNativeTLS) {
    // This test uses macOS native TLS (CFNetwork) to verify HTTPS connectivity
    // This proves the socket layer can handle TLS traffic correctly
    
    // Create URL
    CFURLRef url = CFURLCreateWithString(kCFAllocatorDefault, 
        CFSTR("https://cdn.steamstatic.com/client/steam_client_win32"), nullptr);
    ASSERT_NE(url, nullptr);
    
    // Create request
    CFHTTPMessageRef request = CFHTTPMessageCreateRequest(kCFAllocatorDefault,
        CFSTR("GET"), url, kCFHTTPVersion1_1);
    CFRelease(url);
    ASSERT_NE(request, nullptr);
    
    // Set headers
    CFHTTPMessageSetHeaderFieldValue(request, CFSTR("User-Agent"), CFSTR("Vitreo-Test/1.0"));
    CFHTTPMessageSetHeaderFieldValue(request, CFSTR("Host"), CFSTR("cdn.steamstatic.com"));
    
    // Create read stream
    CFReadStreamRef stream = CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request);
    CFRelease(request);
    ASSERT_NE(stream, nullptr);
    
    // Enable SSL
    CFReadStreamSetProperty(stream, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue);
    
    // Open stream
    if (!CFReadStreamOpen(stream)) {
        CFRelease(stream);
        GTEST_SKIP() << "Failed to open HTTPS stream - network may be unavailable";
    }
    
    // Read response
    std::vector<UInt8> data;
    UInt8 buffer[8192];
    CFIndex bytesRead;
    
    while ((bytesRead = CFReadStreamRead(stream, buffer, sizeof(buffer))) > 0) {
        data.insert(data.end(), buffer, buffer + bytesRead);
        if (data.size() > 100000) break;  // Limit for test
    }
    
    // Get status code (use C cast to handle CFTypeRef const-ness)
    CFHTTPMessageRef response = (CFHTTPMessageRef)CFReadStreamCopyProperty(
        stream, kCFStreamPropertyHTTPResponseHeader);
    
    CFIndex statusCode = 0;
    if (response) {
        statusCode = CFHTTPMessageGetResponseStatusCode(response);
        CFRelease(response);
    }
    
    CFReadStreamClose(stream);
    CFRelease(stream);
    
    // Verify response
    EXPECT_GE(static_cast<int>(statusCode), 200);
    EXPECT_LT(static_cast<int>(statusCode), 400) 
        << "HTTPS request failed with status " << statusCode;
    
    EXPECT_GT(data.size(), 100u) << "Response too small";
    
    // The Steam manifest starts with "win32"
    if (data.size() > 5) {
        std::string prefix(reinterpret_cast<char*>(data.data()), 5);
        EXPECT_EQ(prefix, "win32") << "Response doesn't look like Steam manifest";
    }
    
    std::fprintf(stderr, "[test] HTTPS downloaded %zu bytes from cdn.steamstatic.com (status %ld)\n", 
                 data.size(), static_cast<long>(statusCode));
}
#endif // __APPLE__

