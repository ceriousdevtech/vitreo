// SPDX-License-Identifier: LGPL-2.1-only
/// @file http_bridge.mm
/// @brief HTTP client using macOS NSURLSession.
///
/// Provides:
///  1. `http_request` — synchronous one-shot helper (used by WinInet).
///  2. `HttpStream`   — streaming pull-based client (used by WinHTTP so the
///     guest sees headers as soon as they arrive and reads the body
///     progressively instead of waiting for the full payload).

#include "http_bridge.hpp"

#include <vitreo/common.hpp>

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#import <JavaScriptCore/JavaScriptCore.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

// =============================================================================
// Synchronous one-shot request (back-compat for WinInet)
// =============================================================================

HttpResponse http_request(
    const std::string& method,
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::vector<uint8_t>& body)
{
    HttpResponse result;

    @autoreleasepool {
        NSString* nsUrl = [[NSString alloc] initWithUTF8String:url.c_str()];
        NSURL* nsURL = [NSURL URLWithString:nsUrl];
        if (!nsURL) return result;

        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsURL];
        request.HTTPMethod = [[NSString alloc] initWithUTF8String:method.c_str()];
        request.timeoutInterval = 30.0;

        // Set headers
        for (const auto& [key, value] : headers) {
            NSString* nsKey = [[NSString alloc] initWithUTF8String:key.c_str()];
            NSString* nsVal = [[NSString alloc] initWithUTF8String:value.c_str()];
            [request setValue:nsVal forHTTPHeaderField:nsKey];
        }

        // Set body
        if (!body.empty()) {
            request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        }

        // Synchronous request using semaphore
        __block NSData* responseData = nil;
        __block NSHTTPURLResponse* httpResponse = nil;
        __block NSError* requestError = nil;

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task = [[NSURLSession sharedSession]
            dataTaskWithRequest:request
            completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                // The session releases these when the handler returns, before
                // the waiting thread reads them; keep owned references.
                responseData = [data retain];
                httpResponse = (NSHTTPURLResponse*)[response retain];
                requestError = [error retain];
                dispatch_semaphore_signal(sem);
            }];
        [task resume];
        const bool timed_out =
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC)) != 0;
        // Built without ARC: release the +1 from dispatch_semaphore_create.
        // The completion block holds its own reference, so this is safe even
        // when the wait timed out and the task is still in flight.
        dispatch_release(sem);
        if (timed_out) {
            [task cancel];
            return result;
        }
        struct ReleaseOnExit {
            NSData* data; NSHTTPURLResponse* response; NSError* error;
            ~ReleaseOnExit() { [data release]; [response release]; [error release]; }
        } release_on_exit{responseData, httpResponse, requestError};

        if (requestError || !httpResponse) {
            return result;
        }

        result.status_code = static_cast<int>(httpResponse.statusCode);
        result.success = true;

        // Copy response headers
        NSDictionary* respHeaders = httpResponse.allHeaderFields;
        for (NSString* key in respHeaders) {
            NSString* val = respHeaders[key];
            std::string lkey = std::string([key UTF8String]);
            // Lowercase the key
            for (auto& c : lkey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            result.headers[lkey] = std::string([val UTF8String]);
        }

        // Copy response body
        if (responseData && responseData.length > 0) {
            const uint8_t* bytes = static_cast<const uint8_t*>(responseData.bytes);
            result.body.assign(bytes, bytes + responseData.length);
        }
    }

    return result;
}

// =============================================================================
// Streaming HttpStream
// =============================================================================

} // namespace vitreo::winapi

// Obj-C delegate bridging NSURLSession callbacks to our C++ stream.
@class VitreoHttpStreamDelegate;

namespace vitreo::winapi {

struct HttpStream::Impl {
    // Session, task and delegate, each owned (+1) by this Impl. The file is
    // built without ARC, and NSURLSession keeps a task only while it runs: an
    // unretained task is freed once it completes, and a later cancel()
    // messages freed memory.
    NSURLSession*            session  = nil;
    NSURLSessionDataTask*    task     = nil;
    VitreoHttpStreamDelegate* delegate = nil;

    // Synchronization
    mutable std::mutex       mu;
    std::condition_variable  cv;

    // Headers
    bool                     headers_ready = false;
    int                      status_code   = 0;
    std::unordered_map<std::string, std::string> headers;
    long long                content_length = -1;

    // Body queue (producer: delegate; consumer: guest thread)
    std::deque<std::vector<uint8_t>> chunks;
    std::size_t              queued_bytes  = 0;
    std::size_t              bytes_read    = 0;

    // Terminal state
    bool                     finished      = false;
    bool                     cancelled     = false;
    std::string              error;
    int                      error_code    = 0;

    // Streamed request body (bound stream pair; writer end).
    CFWriteStreamRef         body_writer   = nullptr;

    void push_headers(int code,
                      std::unordered_map<std::string, std::string> hdrs,
                      long long cl)
    {
        std::lock_guard<std::mutex> lk(mu);
        status_code    = code;
        headers        = std::move(hdrs);
        content_length = cl;
        headers_ready  = true;
        cv.notify_all();
    }

    void push_chunk(const uint8_t* data, std::size_t n)
    {
        if (n == 0) return;
        std::lock_guard<std::mutex> lk(mu);
        if (cancelled) return;
        chunks.emplace_back(data, data + n);
        queued_bytes += n;
        cv.notify_all();
    }

    void push_finished(std::string err = {}, int code = 0)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!err.empty() && error.empty()) error = std::move(err);
        if (code != 0 && error_code == 0) error_code = code;
        finished      = true;
        headers_ready = true; // ensure wait_headers unblocks on failure
        cv.notify_all();
    }
};

} // namespace vitreo::winapi

@interface VitreoHttpStreamDelegate : NSObject
    <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
@property (nonatomic, assign) vitreo::winapi::HttpStream::Impl* impl;
@end

@implementation VitreoHttpStreamDelegate

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)dataTask
didReceiveResponse:(NSURLResponse*)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler
{
    (void)session; (void)dataTask;
    if (!self.impl) { completionHandler(NSURLSessionResponseCancel); return; }

    int code = 0;
    long long cl = -1;
    std::unordered_map<std::string, std::string> hdrs;
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
        NSHTTPURLResponse* h = (NSHTTPURLResponse*)response;
        code = static_cast<int>(h.statusCode);
        for (NSString* key in h.allHeaderFields) {
            NSString* val = h.allHeaderFields[key];
            std::string lkey = std::string([key UTF8String]);
            for (auto& c : lkey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            hdrs[lkey] = std::string([val UTF8String]);
        }
        NSString* clStr = [h.allHeaderFields objectForKey:@"Content-Length"];
        if (!clStr) clStr = [h.allHeaderFields objectForKey:@"content-length"];
        if (clStr) {
            cl = static_cast<long long>([clStr longLongValue]);
        } else if (response.expectedContentLength != NSURLResponseUnknownLength) {
            cl = static_cast<long long>(response.expectedContentLength);
        }
    }
    self.impl->push_headers(code, std::move(hdrs), cl);
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveData:(NSData*)data
{
    (void)session; (void)dataTask;
    if (!self.impl || data.length == 0) return;
    // NSData may be non-contiguous; enumerate byte ranges to avoid a copy
    // through [data bytes] when the data is large.
    [data enumerateByteRangesUsingBlock:^(const void* bytes,
                                          NSRange byteRange,
                                          BOOL* stop)
    {
        (void)stop;
        self.impl->push_chunk(static_cast<const uint8_t*>(bytes), byteRange.length);
    }];
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
didCompleteWithError:(NSError*)err
{
    (void)session; (void)task;
    if (!self.impl) return;
    std::string e;
    int code = 0;
    if (err) {
        e = std::string([[err localizedDescription] UTF8String]);
        code = [err.domain isEqualToString:NSURLErrorDomain] ? static_cast<int>(err.code) : -1;
    }
    self.impl->push_finished(std::move(e), code);
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
 needNewBodyStream:(void (^)(NSInputStream*))completionHandler
{
    // A streamed body cannot be replayed (redirect/authentication resend).
    (void)session; (void)task;
    completionHandler(nil);
}

@end

namespace vitreo::winapi {

HttpStream::HttpStream() : m_impl(std::make_unique<Impl>()) {}

HttpStream::~HttpStream()
{
    cancel();
    if (m_impl && m_impl->body_writer) {
        CFWriteStreamClose(m_impl->body_writer);
        CFRelease(m_impl->body_writer);
        m_impl->body_writer = nullptr;
    }
    if (m_impl) {
        // Drop the delegate's raw pointer to us before releasing.
        @autoreleasepool {
            if (m_impl->delegate) m_impl->delegate.impl = nullptr;
            [m_impl->session finishTasksAndInvalidate];
            [m_impl->task release];
            [m_impl->session release];
            [m_impl->delegate release];
            m_impl->task     = nil;
            m_impl->session  = nil;
            m_impl->delegate = nil;
        }
    }
}

bool HttpStream::start(const std::string& method,
                       const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& hdrs,
                       const std::vector<uint8_t>& body,
                       bool unbounded_transport,
                       uint64_t streamed_total)
{
    if (!m_impl) return false;
    @autoreleasepool {
        NSString* nsUrl = [[NSString alloc] initWithUTF8String:url.c_str()];
        NSURL* nsURL = [NSURL URLWithString:nsUrl];
        if (!nsURL) {
            m_impl->push_finished("invalid URL");
            return false;
        }

        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsURL];
        req.HTTPMethod = [[NSString alloc] initWithUTF8String:method.c_str()];
        req.timeoutInterval = unbounded_transport ? DBL_MAX : 60.0;
        for (const auto& [k, v] : hdrs) {
            NSString* nk = [NSString stringWithUTF8String:k.c_str()];
            NSString* nv = [NSString stringWithUTF8String:v.c_str()];
            if (!nk || !nv) continue;
            [req addValue:nv forHTTPHeaderField:nk];
        }
        const bool streamed = streamed_total > body.size();
        if (streamed) {
            CFReadStreamRef reader = nullptr;
            CFWriteStreamRef writer = nullptr;
            CFStreamCreateBoundPair(kCFAllocatorDefault, &reader, &writer, 64 * 1024);
            if (!reader || !writer || !CFWriteStreamOpen(writer)) {
                if (reader) CFRelease(reader);
                if (writer) CFRelease(writer);
                m_impl->push_finished("cannot create request body stream");
                return false;
            }
            req.HTTPBodyStream = (NSInputStream*)reader;
            CFRelease(reader);
            [req setValue:[NSString stringWithFormat:@"%llu",
                               static_cast<unsigned long long>(streamed_total)]
                forHTTPHeaderField:@"Content-Length"];
            m_impl->body_writer = writer;
        } else if (!body.empty()) {
            req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        }

        VitreoHttpStreamDelegate* del = [[VitreoHttpStreamDelegate alloc] init];
        del.impl = m_impl.get();
        m_impl->delegate = del;

        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest  = unbounded_transport ? DBL_MAX : 60.0;
        cfg.timeoutIntervalForResource = unbounded_transport ? DBL_MAX : 3600.0;
        cfg.HTTPMaximumConnectionsPerHost = 8;

        m_impl->session = [[NSURLSession sessionWithConfiguration:cfg
                                                         delegate:del
                                                    delegateQueue:nil] retain];
        m_impl->task = [[m_impl->session dataTaskWithRequest:req] retain];
        [m_impl->task resume];
        if (streamed && !body.empty() && !write_body(body.data(), body.size())) return false;
    }
    return true;
}

bool HttpStream::write_body(const void* data, std::size_t size)
{
    if (!m_impl || !m_impl->body_writer) return false;
    const auto* bytes = static_cast<const UInt8*>(data);
    while (size > 0) {
        {
            std::lock_guard<std::mutex> lk(m_impl->mu);
            if (m_impl->cancelled || m_impl->finished) return false;
        }
        const CFIndex n = CFWriteStreamWrite(m_impl->body_writer, bytes,
                                             static_cast<CFIndex>(size));
        if (n <= 0) return false;
        bytes += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void HttpStream::finish_body()
{
    if (!m_impl || !m_impl->body_writer) return;
    CFWriteStreamClose(m_impl->body_writer);
    CFRelease(m_impl->body_writer);
    m_impl->body_writer = nullptr;
}

int HttpStream::error_code() const noexcept
{
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->error_code;
}

bool HttpStream::wait_headers(int timeout_ms)
{
    if (!m_impl) return false;
    std::unique_lock<std::mutex> lk(m_impl->mu);
    if (m_impl->headers_ready) return m_impl->status_code > 0;
    if (timeout_ms < 0) {
        m_impl->cv.wait(lk, [&] { return m_impl->headers_ready; });
    } else {
        m_impl->cv.wait_for(lk,
                            std::chrono::milliseconds(timeout_ms),
                            [&] { return m_impl->headers_ready; });
    }
    return m_impl->status_code > 0;
}

bool HttpStream::headers_ready() const noexcept
{
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->headers_ready;
}

bool HttpStream::finished() const noexcept
{
    if (!m_impl) return true;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->finished && m_impl->queued_bytes == 0;
}

int HttpStream::status_code() const noexcept
{
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->status_code;
}

const std::unordered_map<std::string, std::string>&
HttpStream::headers() const noexcept
{
    // m_impl is always non-null after construction; headers() is only valid
    // after wait_headers() so the map is stable at that point.
    return m_impl->headers;
}

long long HttpStream::content_length() const noexcept
{
    if (!m_impl) return -1;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->content_length;
}

std::size_t HttpStream::available() const noexcept
{
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->queued_bytes;
}

std::size_t HttpStream::bytes_read() const noexcept
{
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->bytes_read;
}

bool HttpStream::wait_available(int timeout_ms, std::size_t& available)
{
    available = 0;
    if (!m_impl) return false;
    std::unique_lock<std::mutex> lk(m_impl->mu);
    auto ready = [&] {
        return m_impl->queued_bytes > 0 || m_impl->finished || m_impl->cancelled;
    };
    if (!ready()) {
        if (timeout_ms < 0) m_impl->cv.wait(lk, ready);
        else m_impl->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), ready);
    }
    if (m_impl->queued_bytes > 0) { available = m_impl->queued_bytes; return true; }
    if (m_impl->cancelled) return false;
    if (m_impl->finished) return m_impl->error.empty();
    return false;
}

int HttpStream::read(void* dst, std::size_t cap, int timeout_ms)
{
    if (!m_impl || !dst || cap == 0) return -1;
    std::unique_lock<std::mutex> lk(m_impl->mu);

    auto has_data_or_end = [&] {
        return m_impl->queued_bytes > 0 || m_impl->finished || m_impl->cancelled;
    };

    if (!has_data_or_end()) {
        if (timeout_ms < 0) {
            m_impl->cv.wait(lk, has_data_or_end);
        } else {
            m_impl->cv.wait_for(lk,
                                std::chrono::milliseconds(timeout_ms),
                                has_data_or_end);
        }
    }

    if (m_impl->queued_bytes == 0) {
        if (m_impl->cancelled) return -1;
        // A transfer that ended with an error is a failure, not end-of-data.
        if (m_impl->finished)  return m_impl->error.empty() ? 0 : -1;
        return -1;                       // timed out
    }

    std::size_t copied = 0;
    auto* out = static_cast<uint8_t*>(dst);
    while (copied < cap && !m_impl->chunks.empty()) {
        auto& front = m_impl->chunks.front();
        std::size_t want = std::min(cap - copied, front.size());
        std::memcpy(out + copied, front.data(), want);
        copied += want;
        m_impl->queued_bytes -= want;
        if (want == front.size()) {
            m_impl->chunks.pop_front();
        } else {
            front.erase(front.begin(),
                        front.begin() + static_cast<std::ptrdiff_t>(want));
        }
    }
    m_impl->bytes_read += copied;
    return static_cast<int>(copied);
}

void HttpStream::cancel()
{
    if (!m_impl) return;
    @autoreleasepool {
        NSURLSessionDataTask* t = nil;
        {
            std::lock_guard<std::mutex> lk(m_impl->mu);
            if (m_impl->cancelled) return;
            m_impl->cancelled = true;
            t = m_impl->task;
        }
        if (t) [t cancel];
        m_impl->push_finished("cancelled");
    }
}

const std::string& HttpStream::error() const noexcept
{
    if (!m_impl) {
        static const std::string k_empty;
        return k_empty;
    }
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->error;
}

} // namespace vitreo::winapi

// =============================================================================
// WinHTTP WebSocket transport
// =============================================================================

@class VitreoWebSocketDelegate;

namespace vitreo::winapi {

struct WebSocketStream::Impl {
    // Owned (+1), as in HttpStream::Impl: this file is built without ARC.
    NSURLSession* session = nil;
    NSURLSessionWebSocketTask* task = nil;
    VitreoWebSocketDelegate* delegate = nil;

    mutable std::mutex mu;
    std::condition_variable cv;
    bool opened = false;
    bool closed = false;
    bool cancelled = false;
    bool receive_pending = false;
    bool message_ready = false;
    bool message_utf8 = false;
    std::vector<uint8_t> message;
    std::size_t message_offset = 0;
    std::string error;

    void mark_open() {
        std::lock_guard<std::mutex> lock(mu);
        opened = true;
        cv.notify_all();
    }

    void mark_closed(std::string detail = {}) {
        std::lock_guard<std::mutex> lock(mu);
        if (!detail.empty() && error.empty()) error = std::move(detail);
        closed = true;
        receive_pending = false;
        cv.notify_all();
    }

    void push_message(std::vector<uint8_t> bytes, bool utf8) {
        std::lock_guard<std::mutex> lock(mu);
        message = std::move(bytes);
        message_offset = 0;
        message_utf8 = utf8;
        message_ready = true;
        receive_pending = false;
        cv.notify_all();
    }
};

} // namespace vitreo::winapi

@interface VitreoWebSocketDelegate : NSObject <NSURLSessionWebSocketDelegate>
@property (nonatomic, assign) vitreo::winapi::WebSocketStream::Impl* impl;
@end

@implementation VitreoWebSocketDelegate

- (void)URLSession:(NSURLSession*)session
     webSocketTask:(NSURLSessionWebSocketTask*)webSocketTask
didOpenWithProtocol:(NSString*)protocol
{
    (void)session; (void)webSocketTask; (void)protocol;
    if (self.impl) self.impl->mark_open();
}

- (void)URLSession:(NSURLSession*)session
     webSocketTask:(NSURLSessionWebSocketTask*)webSocketTask
  didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode
             reason:(NSData*)reason
{
    (void)session; (void)webSocketTask; (void)closeCode; (void)reason;
    if (self.impl) self.impl->mark_closed();
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
didCompleteWithError:(NSError*)error
{
    (void)session; (void)task;
    if (!self.impl || !error) return;
    self.impl->mark_closed(std::string([[error localizedDescription] UTF8String]));
}

@end

namespace vitreo::winapi {

WebSocketStream::WebSocketStream() : m_impl(std::make_unique<Impl>()) {}

WebSocketStream::~WebSocketStream() {
    cancel();
    if (!m_impl) return;
    @autoreleasepool {
        if (m_impl->delegate) m_impl->delegate.impl = nullptr;
        [m_impl->session finishTasksAndInvalidate];
        [m_impl->task release];
        [m_impl->session release];
        [m_impl->delegate release];
        m_impl->task = nil;
        m_impl->session = nil;
        m_impl->delegate = nil;
    }
}

bool WebSocketStream::start(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers) {
    if (!m_impl) return false;
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithUTF8String:url.c_str()];
        NSURL* endpoint = [NSURL URLWithString:text];
        if (!endpoint) {
            m_impl->mark_closed("invalid WebSocket URL");
            return false;
        }

        NSMutableURLRequest* request =
            [NSMutableURLRequest requestWithURL:endpoint];
        request.timeoutInterval = 60.0;
        for (const auto& [key, value] : headers) {
            std::string lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (lower == "connection" || lower == "upgrade" ||
                lower == "host" || lower.starts_with("sec-websocket-")) {
                continue;
            }
            NSString* native_key =
                [[NSString alloc] initWithUTF8String:key.c_str()];
            NSString* native_value =
                [[NSString alloc] initWithUTF8String:value.c_str()];
            [request setValue:native_value forHTTPHeaderField:native_key];
        }

        VitreoWebSocketDelegate* delegate =
            [[VitreoWebSocketDelegate alloc] init];
        delegate.impl = m_impl.get();
        m_impl->delegate = delegate;

        NSURLSessionConfiguration* config =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = 60.0;
        config.timeoutIntervalForResource = 3600.0;
        m_impl->session =
            [[NSURLSession sessionWithConfiguration:config
                                           delegate:delegate
                                      delegateQueue:nil] retain];
        m_impl->task = [[m_impl->session webSocketTaskWithRequest:request] retain];
        [m_impl->task resume];
    }
    return true;
}

bool WebSocketStream::wait_open(int timeout_ms) {
    if (!m_impl) return false;
    std::unique_lock<std::mutex> lock(m_impl->mu);
    const auto ready = [&] { return m_impl->opened || m_impl->closed; };
    if (timeout_ms < 0) {
        m_impl->cv.wait(lock, ready);
    } else {
        (void)m_impl->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 ready);
    }
    return m_impl->opened && !m_impl->closed;
}

int WebSocketStream::send(const void* data, std::size_t size, bool utf8,
                          int timeout_ms) {
    if (!m_impl || (data == nullptr && size != 0)) return -1;
    @autoreleasepool {
        NSURLSessionWebSocketMessage* message = nil;
        if (utf8) {
            NSString* string = [[NSString alloc]
                initWithBytes:data length:size encoding:NSUTF8StringEncoding];
            if (!string) return -1;
            message = [[NSURLSessionWebSocketMessage alloc]
                initWithString:string];
        } else {
            NSData* bytes = [NSData dataWithBytes:data length:size];
            message = [[NSURLSessionWebSocketMessage alloc]
                initWithData:bytes];
        }

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSError* result_error = nil;
        [m_impl->task sendMessage:message completionHandler:^(NSError* error) {
            result_error = error;
            dispatch_semaphore_signal(done);
        }];
        const dispatch_time_t deadline = timeout_ms < 0
            ? DISPATCH_TIME_FOREVER
            : dispatch_time(DISPATCH_TIME_NOW,
                            static_cast<int64_t>(timeout_ms) * NSEC_PER_MSEC);
        // Built without ARC: release the +1 from dispatch_semaphore_create on
        // both exits. The completion block holds its own reference.
        const bool timed_out = dispatch_semaphore_wait(done, deadline) != 0;
        dispatch_release(done);
        if (timed_out) return -1;
        return result_error == nil ? 0 : -1;
    }
}

int WebSocketStream::receive(void* data, std::size_t capacity,
                             std::size_t& bytes_read, bool& utf8, bool& final,
                             bool& closed, int timeout_ms) {
    bytes_read = 0;
    utf8 = false;
    final = true;
    closed = false;
    if (!m_impl || data == nullptr || capacity == 0) return -1;

    std::unique_lock<std::mutex> lock(m_impl->mu);
    if (!m_impl->message_ready && !m_impl->closed &&
        !m_impl->receive_pending) {
        m_impl->receive_pending = true;
        VitreoWebSocketDelegate* delegate = m_impl->delegate;
        [m_impl->task receiveMessageWithCompletionHandler:
            ^(NSURLSessionWebSocketMessage* message, NSError* error) {
                auto* impl = delegate.impl;
                if (!impl) return;
                if (error || !message) {
                    impl->mark_closed(error
                        ? std::string([[error localizedDescription] UTF8String])
                        : std::string{});
                    return;
                }
                if (message.type == NSURLSessionWebSocketMessageTypeString) {
                    NSData* encoded =
                        [message.string dataUsingEncoding:NSUTF8StringEncoding];
                    const auto* begin =
                        static_cast<const uint8_t*>(encoded.bytes);
                    impl->push_message(
                        std::vector<uint8_t>(begin, begin + encoded.length),
                        true);
                } else {
                    const auto* begin =
                        static_cast<const uint8_t*>(message.data.bytes);
                    impl->push_message(std::vector<uint8_t>(
                        begin, begin + message.data.length), false);
                }
            }];
    }

    const auto ready = [&] {
        return m_impl->message_ready || m_impl->closed || m_impl->cancelled;
    };
    if (!ready()) {
        if (timeout_ms < 0) {
            m_impl->cv.wait(lock, ready);
        } else {
            (void)m_impl->cv.wait_for(
                lock, std::chrono::milliseconds(timeout_ms), ready);
        }
    }
    if (!m_impl->message_ready) {
        closed = m_impl->closed;
        return closed ? 0 : -1;
    }

    const std::size_t remaining =
        m_impl->message.size() - m_impl->message_offset;
    bytes_read = std::min(capacity, remaining);
    std::memcpy(data, m_impl->message.data() + m_impl->message_offset,
                bytes_read);
    m_impl->message_offset += bytes_read;
    utf8 = m_impl->message_utf8;
    final = m_impl->message_offset == m_impl->message.size();
    if (final) {
        m_impl->message.clear();
        m_impl->message_offset = 0;
        m_impl->message_ready = false;
    }
    return 0;
}

int WebSocketStream::shutdown(std::uint16_t status, const void* reason,
                              std::size_t reason_size) {
    if (!m_impl || (reason == nullptr && reason_size != 0) ||
        reason_size > 123) {
        return -1;
    }
    @autoreleasepool {
        NSData* native_reason = reason_size == 0
            ? nil : [NSData dataWithBytes:reason length:reason_size];
        [m_impl->task cancelWithCloseCode:
            static_cast<NSURLSessionWebSocketCloseCode>(status)
                                      reason:native_reason];
    }
    return 0;
}

void WebSocketStream::cancel() {
    if (!m_impl) return;
    @autoreleasepool {
        {
            std::lock_guard<std::mutex> lock(m_impl->mu);
            if (m_impl->cancelled) return;
            m_impl->cancelled = true;
            m_impl->closed = true;
            m_impl->cv.notify_all();
        }
        [m_impl->task cancel];
    }
}

const std::string& WebSocketStream::error() const noexcept {
    if (!m_impl) {
        static const std::string empty;
        return empty;
    }
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->error;
}

} // namespace vitreo::winapi

// =============================================================================
// System proxy settings and PAC evaluation (WinHTTP autoproxy)
// =============================================================================

namespace vitreo::winapi {
namespace {

std::string cf_string(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    CFStringRef s = static_cast<CFStringRef>(value);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

int cf_int(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = CFDictionaryGetValue(d, key);
    int out = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &out);
    return out;
}

// First IPv4 address of @p host, or empty.
std::string resolve_ipv4(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || !list) return {};
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(list->ai_addr)->sin_addr, text, sizeof(text));
    freeaddrinfo(list);
    return text;
}

// Every address of @p host (IPv6 and IPv4), ';'-separated.
std::string resolve_all(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || !list) return {};
    std::string out;
    for (addrinfo* a = list; a; a = a->ai_next) {
        char text[INET6_ADDRSTRLEN] = {};
        if (a->ai_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, text, sizeof(text));
        else if (a->ai_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr, text, sizeof(text));
        else
            continue;
        if (out.find(text) != std::string::npos) continue;
        if (!out.empty()) out += ';';
        out += text;
    }
    freeaddrinfo(list);
    return out;
}

// Addresses of the up, non-loopback interfaces (IPv4 first when @p v4_only).
std::string local_addresses(bool v4_only) {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return v4_only ? "127.0.0.1" : "";
    std::string out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char text[INET6_ADDRSTRLEN] = {};
        if (i->ifa_addr->sa_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof(text));
        else if (!v4_only && i->ifa_addr->sa_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(i->ifa_addr)->sin6_addr, text, sizeof(text));
        else
            continue;
        if (v4_only) { out = text; break; }
        if (!out.empty()) out += ';';
        out += text;
    }
    freeifaddrs(list);
    if (v4_only && out.empty()) out = "127.0.0.1";
    return out;
}

std::string js_string(JSContextRef ctx, JSValueRef value) {
    JSStringRef s = JSValueToStringCopy(ctx, value, nullptr);
    if (!s) return {};
    const size_t max = JSStringGetMaximumUTF8CStringSize(s);
    std::string out(max, '\0');
    const size_t n = JSStringGetUTF8CString(s, out.data(), max);
    JSStringRelease(s);
    out.resize(n > 0 ? n - 1 : 0);
    return out;
}

JSValueRef js_make_string(JSContextRef ctx, const std::string& text) {
    JSStringRef s = JSStringCreateWithUTF8CString(text.c_str());
    JSValueRef v = JSValueMakeString(ctx, s);
    JSStringRelease(s);
    return v;
}

JSValueRef native_resolve(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                          const JSValueRef argv[], JSValueRef*) {
    if (argc < 1) return JSValueMakeNull(ctx);
    const std::string ip = resolve_ipv4(js_string(ctx, argv[0]));
    return ip.empty() ? JSValueMakeNull(ctx) : js_make_string(ctx, ip);
}
JSValueRef native_resolve_all(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                              const JSValueRef argv[], JSValueRef*) {
    return js_make_string(ctx, argc < 1 ? std::string() : resolve_all(js_string(ctx, argv[0])));
}
JSValueRef native_my_ip(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t,
                        const JSValueRef[], JSValueRef*) {
    return js_make_string(ctx, local_addresses(true));
}
JSValueRef native_my_ip_all(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t,
                            const JSValueRef[], JSValueRef*) {
    return js_make_string(ctx, local_addresses(false));
}

// The PAC helper library (Netscape proxy auto-config specification plus the
// Microsoft IPv6 extensions). Semantics checked against Windows' own library
// by http_winhttp_handles_probe (helpers.pac): dnsDomainIs and shExpMatch are
// case-sensitive, shExpMatch matches the whole string with only * and ?.
constexpr const char* kPacPrelude = R"JS(
function isPlainHostName(host) { return host.indexOf('.') < 0; }
function dnsDomainIs(host, domain) {
  return host.length >= domain.length && host.substring(host.length - domain.length) == domain;
}
function localHostOrDomainIs(host, hostdom) {
  return host == hostdom || (host.indexOf('.') < 0 && hostdom.lastIndexOf(host + '.', 0) == 0);
}
function dnsResolve(host) { return __vitreoResolve(String(host)); }
function dnsResolveEx(host) { return __vitreoResolveAll(String(host)); }
function isResolvable(host) { return __vitreoResolve(String(host)) !== null; }
function isResolvableEx(host) { return __vitreoResolveAll(String(host)) !== ""; }
function myIpAddress() { return __vitreoMyIp(); }
function myIpAddressEx() { return __vitreoMyIpAll(); }
function dnsDomainLevels(host) { return host.split('.').length - 1; }
function __ipv4(text) {
  var parts = String(text).split('.');
  if (parts.length != 4) return null;
  var value = 0;
  for (var i = 0; i < 4; ++i) {
    if (!/^[0-9]+$/.test(parts[i])) return null;
    var n = parseInt(parts[i], 10);
    if (n > 255) return null;
    value = value * 256 + n;
  }
  return value;
}
function isInNet(host, pattern, mask) {
  var a = __ipv4(host);
  if (a === null) { var r = __vitreoResolve(String(host)); if (r === null) return false; a = __ipv4(r); }
  var p = __ipv4(pattern), m = __ipv4(mask);
  if (a === null || p === null || m === null) return false;
  return ((a & m) >>> 0) == ((p & m) >>> 0);
}
function __ipv6(text) {
  var s = String(text); var halves = s.split('::');
  if (halves.length > 2) return null;
  function groups(t) { return t === '' ? [] : t.split(':'); }
  var head = groups(halves[0]), tail = halves.length == 2 ? groups(halves[1]) : [];
  var missing = 8 - head.length - tail.length;
  if (halves.length == 1 && missing != 0) return null;
  if (missing < 0) return null;
  var all = head.concat(new Array(halves.length == 2 ? missing : 0).fill('0'), tail);
  var bytes = [];
  for (var i = 0; i < 8; ++i) {
    if (!/^[0-9a-fA-F]{1,4}$/.test(all[i])) return null;
    var v = parseInt(all[i], 16); bytes.push(v >> 8, v & 255);
  }
  return bytes;
}
function isInNetEx(ip, prefix) {
  var slash = String(prefix).indexOf('/');
  if (slash < 0) return false;
  var bits = parseInt(String(prefix).substring(slash + 1), 10), net = String(prefix).substring(0, slash);
  var a4 = __ipv4(ip), n4 = __ipv4(net);
  if (a4 !== null && n4 !== null) {
    if (bits < 0 || bits > 32) return false;
    var mask = bits == 0 ? 0 : (0xFFFFFFFF << (32 - bits)) >>> 0;
    return ((a4 & mask) >>> 0) == ((n4 & mask) >>> 0);
  }
  var a6 = __ipv6(ip), n6 = __ipv6(net);
  if (a6 === null || n6 === null || bits < 0 || bits > 128) return false;
  for (var i = 0; i < 16 && bits > 0; ++i, bits -= 8) {
    var m = bits >= 8 ? 255 : (255 << (8 - bits)) & 255;
    if ((a6[i] & m) != (n6[i] & m)) return false;
  }
  return true;
}
function sortIpAddressList(list) {
  var items = String(list).split(';');
  var v6 = [], v4 = [];
  for (var i = 0; i < items.length; ++i) {
    if (__ipv4(items[i]) !== null) v4.push(items[i]);
    else if (__ipv6(items[i]) !== null) v6.push(items[i]);
    else return false;
  }
  function cmp6(x, y) { var a = __ipv6(x), b = __ipv6(y); for (var i = 0; i < 16; ++i) if (a[i] != b[i]) return a[i] - b[i]; return 0; }
  v6.sort(cmp6); v4.sort(function (x, y) { return __ipv4(x) - __ipv4(y); });
  return v6.concat(v4).join(';');
}
function getClientVersion() { return "1.0"; }
function shExpMatch(str, shexp) {
  var re = '';
  for (var i = 0; i < shexp.length; ++i) {
    var c = shexp.charAt(i);
    if (c == '*') re += '.*';
    else if (c == '?') re += '.';
    else re += c.replace(/[\\^$.|+()\[\]{}\/]/g, '\\$&');
  }
  return new RegExp('^' + re + '$').test(String(str));
}
var __days = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"];
var __months = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"];
function __args(a) {
  var list = Array.prototype.slice.call(a), gmt = false;
  if (list.length && list[list.length - 1] === "GMT") { gmt = true; list.pop(); }
  return { list: list, now: new Date(), gmt: gmt };
}
function weekdayRange() {
  var x = __args(arguments), day = x.gmt ? x.now.getUTCDay() : x.now.getDay();
  var a = __days.indexOf(x.list[0]), b = x.list.length > 1 ? __days.indexOf(x.list[1]) : a;
  if (a < 0 || b < 0) return false;
  return a <= b ? (day >= a && day <= b) : (day >= a || day <= b);
}
function dateRange() {
  var x = __args(arguments), n = x.now;
  var cur = { d: x.gmt ? n.getUTCDate() : n.getDate(), m: x.gmt ? n.getUTCMonth() : n.getMonth(),
              y: x.gmt ? n.getUTCFullYear() : n.getFullYear() };
  function field(v) { return typeof v === "string" ? ["m", __months.indexOf(v)] : v > 31 ? ["y", v] : ["d", v]; }
  var list = x.list;
  if (list.length == 1) { var f = field(list[0]); return cur[f[0]] == f[1]; }
  if (list.length % 2) return false;
  var half = list.length / 2, lo = {}, hi = {}, keys = [];
  for (var i = 0; i < half; ++i) { var p = field(list[i]), q = field(list[half + i]); lo[p[0]] = p[1]; hi[q[0]] = q[1]; if (keys.indexOf(p[0]) < 0) keys.push(p[0]); }
  function key(o) { return (keys.indexOf("y") >= 0 ? o.y : 0) * 10000 + (keys.indexOf("m") >= 0 ? o.m : 0) * 100 + (keys.indexOf("d") >= 0 ? o.d : 0); }
  var l = key(lo), h = key(hi), c = key(cur);
  return l <= h ? (c >= l && c <= h) : (c >= l || c <= h);
}
function timeRange() {
  var x = __args(arguments), n = x.now, list = x.list;
  var hh = x.gmt ? n.getUTCHours() : n.getHours(), mm = x.gmt ? n.getUTCMinutes() : n.getMinutes(),
      ss = x.gmt ? n.getUTCSeconds() : n.getSeconds();
  if (list.length == 1) return hh == list[0];
  var c = hh * 3600 + mm * 60 + ss, l, h;
  if (list.length == 2) { l = list[0] * 3600; h = list[1] * 3600; }
  else if (list.length == 4) { l = list[0] * 3600 + list[1] * 60; h = list[2] * 3600 + list[3] * 60; }
  else if (list.length == 6) { l = list[0] * 3600 + list[1] * 60 + list[2]; h = list[3] * 3600 + list[4] * 60 + list[5]; }
  else return false;
  return l <= h ? (c >= l && c < h) : (c >= l || c < h);
}
function alert(message) {}
)JS";

bool evaluate_pac(const std::string& script, const ProxyQuery& query, std::string& result,
                  bool& has_result) {
    has_result = false;
    JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
    if (!ctx) return false;
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    struct Native { const char* name; JSObjectCallAsFunctionCallback fn; };
    const Native natives[] = {
        {"__vitreoResolve", native_resolve}, {"__vitreoResolveAll", native_resolve_all},
        {"__vitreoMyIp", native_my_ip}, {"__vitreoMyIpAll", native_my_ip_all},
    };
    for (const auto& n : natives) {
        JSStringRef name = JSStringCreateWithUTF8CString(n.name);
        JSObjectSetProperty(ctx, global, name, JSObjectMakeFunctionWithCallback(ctx, name, n.fn),
                            kJSPropertyAttributeNone, nullptr);
        JSStringRelease(name);
    }
    bool ok = true;
    const char* const sources[2] = {kPacPrelude, script.c_str()};
    for (const char* source : sources) {
        JSStringRef text = JSStringCreateWithUTF8CString(source);
        JSValueRef exception = nullptr;
        JSEvaluateScript(ctx, text, nullptr, nullptr, 1, &exception);
        JSStringRelease(text);
        if (exception) { ok = false; break; }
    }
    if (ok) {
        JSStringRef name = JSStringCreateWithUTF8CString("FindProxyForURL");
        JSValueRef fn = JSObjectGetProperty(ctx, global, name, nullptr);
        JSStringRelease(name);
        if (!fn || !JSValueIsObject(ctx, fn) ||
            !JSObjectIsFunction(ctx, JSValueToObject(ctx, fn, nullptr))) {
            ok = false;
        } else {
            JSValueRef args[2] = {js_make_string(ctx, query.script_url),
                                  js_make_string(ctx, query.script_host)};
            JSValueRef exception = nullptr;
            JSValueRef value = JSObjectCallAsFunction(ctx, JSValueToObject(ctx, fn, nullptr),
                                                      nullptr, 2, args, &exception);
            if (exception || !value) {
                ok = false;
            } else if (JSValueIsString(ctx, value)) {
                result = js_string(ctx, value);
                has_result = true;
            }
        }
    }
    JSGlobalContextRelease(ctx);
    return ok;
}

bool is_http_url(const std::string& url) {
    auto lower = url.substr(0, 8);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

// WPAD discovery: DHCP option 252, then the DNS name "wpad".
std::vector<std::string> discover_wpad(bool dhcp, bool dns) {
    std::vector<std::string> urls;
    if (dhcp) {
        if (CFDictionaryRef info = SCDynamicStoreCopyDHCPInfo(nullptr, nullptr)) {
            if (CFDataRef option = DHCPInfoGetOptionData(info, 252)) {
                std::string url(reinterpret_cast<const char*>(CFDataGetBytePtr(option)),
                                static_cast<size_t>(CFDataGetLength(option)));
                while (!url.empty() && (url.back() == '\0' || std::isspace(static_cast<unsigned char>(url.back()))))
                    url.pop_back();
                if (!url.empty()) urls.push_back(url);
            }
            CFRelease(info);
        }
    }
    if (dns && !resolve_all("wpad").empty()) urls.push_back("http://wpad/wpad.dat");
    return urls;
}

ProxyResolution run_script_url(const std::string& url, const ProxyQuery& query) {
    ProxyResolution out;
    if (!is_http_url(url)) { out.status = ProxyStatus::UnrecognizedPacScheme; return out; }
    const HttpResponse response = http_request("GET", url);
    if (!response.success || response.status_code != 200) {
        out.status = ProxyStatus::DownloadFailed;
        return out;
    }
    std::string script(response.body.begin(), response.body.end());
    std::string result;
    bool has_result = false;
    if (!evaluate_pac(script, query, result, has_result)) {
        out.status = ProxyStatus::ScriptError;
        return out;
    }
    if (has_result) out.entries = parse_pac_result(result);
    return out;
}

} // namespace

SystemProxyConfig system_proxy_config() {
    SystemProxyConfig config;
    CFDictionaryRef d = CFNetworkCopySystemProxySettings();
    if (!d) return config;
    config.auto_detect = cf_int(d, kCFNetworkProxiesProxyAutoDiscoveryEnable) != 0;
    if (cf_int(d, kCFNetworkProxiesProxyAutoConfigEnable) != 0)
        config.auto_config_url = cf_string(CFDictionaryGetValue(d, kCFNetworkProxiesProxyAutoConfigURLString));
    struct Kind { const char* name; CFStringRef enable; CFStringRef host; CFStringRef port; };
    const Kind kinds[] = {
        {"http", kCFNetworkProxiesHTTPEnable, kCFNetworkProxiesHTTPProxy, kCFNetworkProxiesHTTPPort},
        {"https", kCFNetworkProxiesHTTPSEnable, kCFNetworkProxiesHTTPSProxy, kCFNetworkProxiesHTTPSPort},
        {"ftp", kCFNetworkProxiesFTPEnable, kCFNetworkProxiesFTPProxy, kCFNetworkProxiesFTPPort},
        {"socks", kCFNetworkProxiesSOCKSEnable, kCFNetworkProxiesSOCKSProxy, kCFNetworkProxiesSOCKSPort},
    };
    for (const auto& k : kinds) {
        if (cf_int(d, k.enable) == 0) continue;
        const std::string host = cf_string(CFDictionaryGetValue(d, k.host));
        if (host.empty()) continue;
        if (!config.proxy.empty()) config.proxy += ';';
        config.proxy += std::string(k.name) + "=" + host;
        if (const int port = cf_int(d, k.port); port != 0) config.proxy += ":" + std::to_string(port);
    }
    if (CFTypeRef list = CFDictionaryGetValue(d, kCFNetworkProxiesExceptionsList);
        list && CFGetTypeID(list) == CFArrayGetTypeID()) {
        CFArrayRef array = static_cast<CFArrayRef>(list);
        for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
            const std::string item = cf_string(CFArrayGetValueAtIndex(array, i));
            if (item.empty()) continue;
            if (!config.bypass.empty()) config.bypass += ';';
            config.bypass += item;
        }
    }
    if (cf_int(d, kCFNetworkProxiesExcludeSimpleHostnames) != 0) {
        if (!config.bypass.empty()) config.bypass += ';';
        config.bypass += "<local>";
    }
    CFRelease(d);
    return config;
}

std::vector<std::string> discover_wpad_urls(bool dhcp, bool dns) {
    return discover_wpad(dhcp, dns);
}

ProxyResolution resolve_proxy(const ProxyQuery& query) {
    @autoreleasepool {
        if (query.auto_detect) {
            for (const std::string& url : discover_wpad(query.detect_dhcp, query.detect_dns)) {
                ProxyResolution r = run_script_url(url, query);
                if (r.status == ProxyStatus::Ok) return r;
            }
            if (query.config_url.empty()) {
                ProxyResolution failed;
                failed.status = ProxyStatus::AutoDetectFailed;
                return failed;
            }
        }
        return run_script_url(query.config_url, query);
    }
}

} // namespace vitreo::winapi


#else
// Non-Apple: no HTTP support
namespace vitreo::winapi {

HttpResponse http_request(
    const std::string& /*method*/,
    const std::string& /*url*/,
    const std::unordered_map<std::string, std::string>& /*headers*/,
    const std::vector<uint8_t>& /*body*/)
{
    return HttpResponse{};
}

struct HttpStream::Impl {};

HttpStream::HttpStream()  : m_impl(std::make_unique<Impl>()) {}
HttpStream::~HttpStream() = default;

bool HttpStream::start(const std::string&, const std::string&,
                       const std::vector<std::pair<std::string, std::string>>&,
                       const std::vector<uint8_t>&, bool, uint64_t) { return false; }
bool HttpStream::write_body(const void*, std::size_t) { return false; }
void HttpStream::finish_body() {}
int HttpStream::error_code() const noexcept { return 0; }
bool HttpStream::wait_headers(int)           { return false; }
bool HttpStream::headers_ready() const noexcept { return false; }
bool HttpStream::finished() const noexcept   { return true; }
int  HttpStream::status_code() const noexcept { return 0; }
const std::unordered_map<std::string, std::string>&
     HttpStream::headers() const noexcept {
    static const std::unordered_map<std::string, std::string> k_empty;
    return k_empty;
}
long long   HttpStream::content_length() const noexcept { return -1; }
std::size_t HttpStream::available()      const noexcept { return 0; }
std::size_t HttpStream::bytes_read()     const noexcept { return 0; }
int  HttpStream::read(void*, std::size_t, int) { return -1; }
bool HttpStream::wait_available(int, std::size_t& available) { available = 0; return false; }
void HttpStream::cancel() {}
const std::string& HttpStream::error() const noexcept {
    static const std::string k_empty;
    return k_empty;
}

struct WebSocketStream::Impl {};
WebSocketStream::WebSocketStream() : m_impl(std::make_unique<Impl>()) {}
WebSocketStream::~WebSocketStream() = default;
bool WebSocketStream::start(
    const std::string&,
    const std::unordered_map<std::string, std::string>&) { return false; }
bool WebSocketStream::wait_open(int) { return false; }
int WebSocketStream::send(const void*, std::size_t, bool, int) { return -1; }
int WebSocketStream::receive(void*, std::size_t, std::size_t&, bool&, bool&,
                             bool&, int) { return -1; }
int WebSocketStream::shutdown(std::uint16_t, const void*, std::size_t) {
    return -1;
}
void WebSocketStream::cancel() {}
const std::string& WebSocketStream::error() const noexcept {
    static const std::string k_empty;
    return k_empty;
}

SystemProxyConfig system_proxy_config() { return {}; }
std::vector<std::string> discover_wpad_urls(bool, bool) { return {}; }
ProxyResolution resolve_proxy(const ProxyQuery&) {
    ProxyResolution r;
    r.status = ProxyStatus::DownloadFailed;
    return r;
}

} // namespace vitreo::winapi
#endif

// =============================================================================
// PAC result parsing (platform independent)
// =============================================================================

namespace vitreo::winapi {

#include "http_auth_constants.inc"

namespace {
std::string trim_pac(const std::string& text) {
    size_t b = 0, e = text.size();
    while (b < e && (text[b] == ' ' || text[b] == '\t')) ++b;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
    return text.substr(b, e - b);
}
} // namespace

// Measured on Windows (http_winhttp_handles_probe): tokens are ';'-separated
// and trimmed, empty tokens skipped; keywords are case-sensitive; PROXY is
// scheme http (default port 80), HTTPS is https, SOCKS is socks (SOCKS5 and
// lower-case keywords are dropped); DIRECT yields a bypass entry and ends the
// list; an empty result is DIRECT.
std::vector<ProxyEntry> parse_pac_result(const std::string& result) {
    std::vector<ProxyEntry> out;
    if (trim_pac(result).empty()) {
        ProxyEntry direct;
        direct.bypass = true;
        out.push_back(direct);
        return out;
    }
    size_t pos = 0;
    while (pos < result.size()) {
        size_t semi = result.find(';', pos);
        if (semi == std::string::npos) semi = result.size();
        const std::string token = trim_pac(result.substr(pos, semi - pos));
        pos = semi + 1;
        if (token.empty()) continue;
        const size_t space = token.find_first_of(" \t");
        const std::string keyword = token.substr(0, space);
        const std::string target = space == std::string::npos ? std::string() : trim_pac(token.substr(space));
        if (keyword == "DIRECT") {
            ProxyEntry direct;
            direct.bypass = true;
            out.push_back(direct);
            break;
        }
        ProxyEntry e;
        e.proxy = true;
        if (keyword == "PROXY") {
            e.scheme = http_contract::INTERNET_SCHEME_HTTP;
            e.port = static_cast<uint16_t>(http_contract::INTERNET_DEFAULT_HTTP_PORT);
        } else if (keyword == "HTTPS") {
            e.scheme = http_contract::INTERNET_SCHEME_HTTPS;
            e.port = static_cast<uint16_t>(http_contract::INTERNET_DEFAULT_HTTPS_PORT);
        } else if (keyword == "SOCKS") {
            e.scheme = http_contract::INTERNET_SCHEME_SOCKS;
            e.port = static_cast<uint16_t>(http_contract::INTERNET_DEFAULT_SOCKS_PORT);
        } else {
            continue;
        }
        if (target.empty()) continue;
        e.text = target;
        size_t colon = std::string::npos;
        if (target[0] == '[') {
            const size_t close = target.find(']');
            if (close != std::string::npos && close + 1 < target.size() && target[close + 1] == ':')
                colon = close + 1;
        } else {
            colon = target.rfind(':');
        }
        e.host = colon == std::string::npos ? target : target.substr(0, colon);
        if (colon != std::string::npos) {
            unsigned long port = 0;
            bool digits = colon + 1 < target.size();
            for (size_t i = colon + 1; i < target.size(); ++i) {
                if (target[i] < '0' || target[i] > '9') { digits = false; break; }
                port = port * 10 + static_cast<unsigned long>(target[i] - '0');
                if (port > 0xFFFF) { digits = false; break; }
            }
            if (!digits) continue;
            e.port = static_cast<uint16_t>(port);
        }
        out.push_back(e);
    }
    return out;
}

} // namespace vitreo::winapi
