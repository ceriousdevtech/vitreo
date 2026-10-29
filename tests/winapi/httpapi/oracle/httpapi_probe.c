// HTTP Server API (httpapi.dll) contract probe.
// Build (MSVC, Windows SDK 10.0.26100.0 http.h):
//   vcvars64 & cl /nologo /O1 /W3 httpapi_probe.c /Fe:httpapi_probe64.exe httpapi.lib ws2_32.lib
//   vcvars32 & cl /nologo /O1 /W3 httpapi_probe.c /Fe:httpapi_probe32.exe httpapi.lib ws2_32.lib
// Run: (httpapi_probe64.exe & httpapi_probe64.exe refcount) > httpapi_probe64.expected (same for 32).
// Records layouts, return codes and the raw bytes a TCP client sees. The
// Date header value and the per-boot opaque ids are masked.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <http.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define PORT 48213
#define PORTS "48213"
#define BASE L"http://localhost:" L"48213"

static const HTTPAPI_VERSION V1 = {1, 0};
static const HTTPAPI_VERSION V2 = {2, 0};

// ---------------------------------------------------------------------------
// TCP client running on a worker thread
// ---------------------------------------------------------------------------

typedef struct {
    const char* request;
    int request_len;
    int delay_ms;
    int keep_open_ms;       // wait for more data this long after each read
    int server_closed;      // recv returned 0 (FIN) rather than timing out
    int last_err;           // WSAGetLastError when recv failed (10054 RST, 10060 timeout)
    char response[8192];
    int response_len;
    HANDLE thread;
} Client;

static DWORD WINAPI client_main(LPVOID p) {
    Client* c = (Client*)p;
    SOCKET s;
    struct sockaddr_in a;
    DWORD to;
    if (c->delay_ms) Sleep(c->delay_ms);
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) {
        c->response_len = -1;
        closesocket(s);
        return 0;
    }
    send(s, c->request, c->request_len, 0);
    to = c->keep_open_ms ? c->keep_open_ms : 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    for (;;) {
        int n = recv(s, c->response + c->response_len,
                     (int)sizeof c->response - 1 - c->response_len, 0);
        if (n == 0) c->server_closed = 1;
        if (n < 0) c->last_err = WSAGetLastError();
        if (n <= 0) break;
        c->response_len += n;
    }
    c->response[c->response_len < 0 ? 0 : c->response_len] = 0;
    closesocket(s);
    return 0;
}

static void client_start(Client* c, const char* req, int delay_ms) {
    memset(c, 0, sizeof *c);
    c->request = req;
    c->request_len = (int)strlen(req);
    c->delay_ms = delay_ms;
    c->thread = CreateThread(NULL, 0, client_main, c, 0, NULL);
}

static void client_join(Client* c) {
    WaitForSingleObject(c->thread, 10000);
    CloseHandle(c->thread);
}

// Print the response bytes escaped, the Date header value masked.
static void print_wire(const char* tag, const Client* c) {
    const char* p = c->response;
    printf("%s: len=%d closed=%d err=%d wire=\"", tag, c->response_len, c->server_closed, c->last_err);
    if (c->response_len < 0) { printf("<connect failed>\"\n"); return; }
    while (*p) {
        if (_strnicmp(p, "\r\nDate: ", 8) == 0) {
            printf(strncmp(p + 8, "Thu, 01 Jan 2026", 16) == 0 ? "\\r\\nDate: <caller date>" : "\\r\\nDate: <date>");
            p += 8;
            while (*p && *p != '\r') ++p;
            continue;
        }
        if (*p == '\r') printf("\\r");
        else if (*p == '\n') printf("\\n");
        else if (*p == '"') printf("\\\"");
        else if (*p == '\\') printf("\\\\");
        else if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7f) printf("\\x%02x", (unsigned char)*p);
        else putchar(*p);
        ++p;
    }
    printf("\"\n");
}

// Status line and header names only (http.sys error bodies are not recorded).
static void print_status(const char* tag, const Client* c) {
    const char* p = c->response;
    const char* body;
    printf("%s: ", tag);
    if (c->response_len <= 0) { printf("len=%d closed=%d err=%d\n", c->response_len, c->server_closed, c->last_err); return; }
    while (*p && *p != '\r') putchar(*p++);
    body = strstr(c->response, "\r\n\r\n");
    printf(" headers=");
    while (*p == '\r' && p[1] == '\n' && p[2] != '\r') {
        const char* colon;
        p += 2;
        colon = strchr(p, ':');
        if (!colon) break;
        printf("%.*s", (int)(colon - p), p);
        if (_strnicmp(p, "Content-Length", 14) == 0 || _strnicmp(p, "Content-Type", 12) == 0 ||
            _strnicmp(p, "Connection", 10) == 0 || _strnicmp(p, "Server", 6) == 0) {
            const char* e = strchr(colon, '\r');
            printf("=%.*s", (int)(e - colon - 2), colon + 2);
        }
        putchar(',');
        while (*p && *p != '\r') ++p;
    }
    printf(" body_len=%d\n", body ? (int)(c->response_len - (body + 4 - c->response)) : -1);
}

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------

#define OFF(T, f) printf(" " #f "=%u", (unsigned)offsetof(T, f))

static void layouts(void) {
    printf("sizes: HTTPAPI_VERSION=%u HTTP_VERSION=%u HTTP_KNOWN_HEADER=%u HTTP_UNKNOWN_HEADER=%u "
           "HTTP_DATA_CHUNK=%u HTTP_COOKED_URL=%u HTTP_TRANSPORT_ADDRESS=%u HTTP_REQUEST_HEADERS=%u "
           "HTTP_RESPONSE_HEADERS=%u HTTP_REQUEST_V1=%u HTTP_REQUEST_V2=%u HTTP_RESPONSE_V1=%u "
           "HTTP_RESPONSE_V2=%u HTTP_REQUEST_INFO=%u HTTP_RESPONSE_INFO=%u HTTP_MULTIPLE_KNOWN_HEADERS=%u "
           "HTTP_BINDING_INFO=%u HTTP_STATE_INFO=%u HTTP_TIMEOUT_LIMIT_INFO=%u HTTP_QOS_SETTING_INFO=%u "
           "HTTP_SERVER_AUTHENTICATION_INFO=%u HTTP_LOGGING_INFO=%u HTTP_CHANNEL_BIND_INFO=%u "
           "HTTP_PROTECTION_LEVEL_INFO=%u HTTP_LISTEN_ENDPOINT_INFO=%u HTTP_CACHE_POLICY=%u HTTP_LOG_FIELDS_DATA=%u "
           "HTTP_BYTE_RANGE=%u HTTP_REQUEST_AUTH_INFO=%u\n",
           (unsigned)sizeof(HTTPAPI_VERSION), (unsigned)sizeof(HTTP_VERSION), (unsigned)sizeof(HTTP_KNOWN_HEADER),
           (unsigned)sizeof(HTTP_UNKNOWN_HEADER), (unsigned)sizeof(HTTP_DATA_CHUNK), (unsigned)sizeof(HTTP_COOKED_URL),
           (unsigned)sizeof(HTTP_TRANSPORT_ADDRESS), (unsigned)sizeof(HTTP_REQUEST_HEADERS),
           (unsigned)sizeof(HTTP_RESPONSE_HEADERS), (unsigned)sizeof(HTTP_REQUEST_V1), (unsigned)sizeof(HTTP_REQUEST_V2),
           (unsigned)sizeof(HTTP_RESPONSE_V1), (unsigned)sizeof(HTTP_RESPONSE_V2), (unsigned)sizeof(HTTP_REQUEST_INFO),
           (unsigned)sizeof(HTTP_RESPONSE_INFO), (unsigned)sizeof(HTTP_MULTIPLE_KNOWN_HEADERS),
           (unsigned)sizeof(HTTP_BINDING_INFO), (unsigned)sizeof(HTTP_STATE_INFO), (unsigned)sizeof(HTTP_TIMEOUT_LIMIT_INFO),
           (unsigned)sizeof(HTTP_QOS_SETTING_INFO), (unsigned)sizeof(HTTP_SERVER_AUTHENTICATION_INFO),
           (unsigned)sizeof(HTTP_LOGGING_INFO), (unsigned)sizeof(HTTP_CHANNEL_BIND_INFO),
           (unsigned)sizeof(HTTP_PROTECTION_LEVEL_INFO), (unsigned)sizeof(HTTP_LISTEN_ENDPOINT_INFO),
           (unsigned)sizeof(HTTP_CACHE_POLICY), (unsigned)sizeof(HTTP_LOG_FIELDS_DATA),
           (unsigned)sizeof(HTTP_BYTE_RANGE), (unsigned)sizeof(HTTP_REQUEST_AUTH_INFO));
    printf("HTTP_REQUEST_V2:");
    OFF(HTTP_REQUEST_V2, Flags); OFF(HTTP_REQUEST_V2, ConnectionId); OFF(HTTP_REQUEST_V2, RequestId);
    OFF(HTTP_REQUEST_V2, UrlContext); OFF(HTTP_REQUEST_V2, Version); OFF(HTTP_REQUEST_V2, Verb);
    OFF(HTTP_REQUEST_V2, UnknownVerbLength); OFF(HTTP_REQUEST_V2, RawUrlLength); OFF(HTTP_REQUEST_V2, pUnknownVerb);
    OFF(HTTP_REQUEST_V2, pRawUrl); OFF(HTTP_REQUEST_V2, CookedUrl); OFF(HTTP_REQUEST_V2, Address);
    OFF(HTTP_REQUEST_V2, Headers); OFF(HTTP_REQUEST_V2, BytesReceived); OFF(HTTP_REQUEST_V2, EntityChunkCount);
    OFF(HTTP_REQUEST_V2, pEntityChunks); OFF(HTTP_REQUEST_V2, RawConnectionId); OFF(HTTP_REQUEST_V2, pSslInfo);
    OFF(HTTP_REQUEST_V2, RequestInfoCount); OFF(HTTP_REQUEST_V2, pRequestInfo);
    printf("\nHTTP_COOKED_URL:");
    OFF(HTTP_COOKED_URL, FullUrlLength); OFF(HTTP_COOKED_URL, HostLength); OFF(HTTP_COOKED_URL, AbsPathLength);
    OFF(HTTP_COOKED_URL, QueryStringLength); OFF(HTTP_COOKED_URL, pFullUrl); OFF(HTTP_COOKED_URL, pHost);
    OFF(HTTP_COOKED_URL, pAbsPath); OFF(HTTP_COOKED_URL, pQueryString);
    printf("\nHTTP_REQUEST_HEADERS:");
    OFF(HTTP_REQUEST_HEADERS, UnknownHeaderCount); OFF(HTTP_REQUEST_HEADERS, pUnknownHeaders);
    OFF(HTTP_REQUEST_HEADERS, TrailerCount); OFF(HTTP_REQUEST_HEADERS, pTrailers); OFF(HTTP_REQUEST_HEADERS, KnownHeaders);
    printf("\nHTTP_RESPONSE_V2:");
    OFF(HTTP_RESPONSE_V2, Flags); OFF(HTTP_RESPONSE_V2, Version); OFF(HTTP_RESPONSE_V2, StatusCode);
    OFF(HTTP_RESPONSE_V2, ReasonLength); OFF(HTTP_RESPONSE_V2, pReason); OFF(HTTP_RESPONSE_V2, Headers);
    OFF(HTTP_RESPONSE_V2, EntityChunkCount); OFF(HTTP_RESPONSE_V2, pEntityChunks);
    OFF(HTTP_RESPONSE_V2, ResponseInfoCount); OFF(HTTP_RESPONSE_V2, pResponseInfo);
    printf("\nHTTP_DATA_CHUNK:");
    OFF(HTTP_DATA_CHUNK, DataChunkType); OFF(HTTP_DATA_CHUNK, FromMemory.pBuffer);
    OFF(HTTP_DATA_CHUNK, FromMemory.BufferLength); OFF(HTTP_DATA_CHUNK, FromFileHandle.ByteRange);
    OFF(HTTP_DATA_CHUNK, FromFileHandle.FileHandle); OFF(HTTP_DATA_CHUNK, Trailers.TrailerCount);
    OFF(HTTP_DATA_CHUNK, Trailers.pTrailers);
    printf("\nHTTP_UNKNOWN_HEADER:");
    OFF(HTTP_UNKNOWN_HEADER, NameLength); OFF(HTTP_UNKNOWN_HEADER, RawValueLength);
    OFF(HTTP_UNKNOWN_HEADER, pName); OFF(HTTP_UNKNOWN_HEADER, pRawValue);
    printf("\nHTTP_KNOWN_HEADER:"); OFF(HTTP_KNOWN_HEADER, RawValueLength); OFF(HTTP_KNOWN_HEADER, pRawValue);
    printf("\nHTTP_TIMEOUT_LIMIT_INFO:");
    OFF(HTTP_TIMEOUT_LIMIT_INFO, EntityBody); OFF(HTTP_TIMEOUT_LIMIT_INFO, DrainEntityBody);
    OFF(HTTP_TIMEOUT_LIMIT_INFO, RequestQueue); OFF(HTTP_TIMEOUT_LIMIT_INFO, IdleConnection);
    OFF(HTTP_TIMEOUT_LIMIT_INFO, HeaderWait); OFF(HTTP_TIMEOUT_LIMIT_INFO, MinSendRate);
    printf("\nHTTP_BINDING_INFO:"); OFF(HTTP_BINDING_INFO, RequestQueueHandle);
    printf("\nHTTP_STATE_INFO:"); OFF(HTTP_STATE_INFO, State);
    printf("\nHTTP_LOG_FIELDS_DATA:"); OFF(HTTP_LOG_FIELDS_DATA, UserNameLength); OFF(HTTP_LOG_FIELDS_DATA, ProtocolStatus);
    printf("\n");
}

// ---------------------------------------------------------------------------
// Init / session / group / queue argument errors
// ---------------------------------------------------------------------------

static void init_errors(void) {
    HTTPAPI_VERSION v3 = {3, 0}, v21 = {2, 1}, v0 = {0, 0}, v11 = {1, 1};
    HTTP_SERVER_SESSION_ID sid = 0x1111;
    HTTP_URL_GROUP_ID gid = 0x2222;
    HANDLE q = (HANDLE)0x3333;
    ULONG rc;
    int dummy;

    // Before any HttpInitialize in this process.
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("pre-init session: rc=%lu sid_nonzero=%d\n", rc, sid != 0 && sid != 0x1111);
    if (rc == 0) HttpCloseServerSession(sid);
    q = (HANDLE)0x3333;
    rc = HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
    printf("pre-init queue: rc=%lu handle=%s\n", rc, q == (HANDLE)0x3333 ? "untouched" : q == NULL ? "null" : "other");
    if (rc == 0) HttpCloseRequestQueue(q);
    rc = HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    printf("pre-init terminate: rc=%lu\n", rc);

    printf("init v1 server: rc=%lu\n", HttpInitialize(V1, HTTP_INITIALIZE_SERVER, NULL));
    printf("init v2 server: rc=%lu\n", HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL));
    printf("init v2 config: rc=%lu\n", HttpInitialize(V2, HTTP_INITIALIZE_CONFIG, NULL));
    printf("init v2 both: rc=%lu\n", HttpInitialize(V2, HTTP_INITIALIZE_SERVER | HTTP_INITIALIZE_CONFIG, NULL));
    printf("init v3: rc=%lu\n", HttpInitialize(v3, HTTP_INITIALIZE_SERVER, NULL));
    printf("init v2.1: rc=%lu\n", HttpInitialize(v21, HTTP_INITIALIZE_SERVER, NULL));
    printf("init v1.1: rc=%lu\n", HttpInitialize(v11, HTTP_INITIALIZE_SERVER, NULL));
    printf("init v0: rc=%lu\n", HttpInitialize(v0, HTTP_INITIALIZE_SERVER, NULL));
    printf("init flags0: rc=%lu\n", HttpInitialize(V2, 0, NULL));
    printf("init flags8: rc=%lu\n", HttpInitialize(V2, 8, NULL));
    printf("init flags-cbt: rc=%lu\n", HttpInitialize(V2, HTTP_INITIALIZE_SERVER | HTTP_DEMAND_CBT, NULL));
    printf("init reserved: rc=%lu\n", HttpInitialize(V2, HTTP_INITIALIZE_SERVER, &dummy));
    printf("terminate flags0: rc=%lu\n", HttpTerminate(0, NULL));
    printf("terminate flags8: rc=%lu\n", HttpTerminate(8, NULL));
    printf("terminate reserved: rc=%lu\n", HttpTerminate(HTTP_INITIALIZE_CONFIG, &dummy));

    sid = 0x1111;
    rc = HttpCreateServerSession(V1, &sid, 0);
    printf("session v1: rc=%lu sid_untouched=%d\n", rc, sid == 0x1111);
    if (rc == 0) HttpCloseServerSession(sid);
    sid = 0x1111;
    rc = HttpCreateServerSession(v3, &sid, 0);
    printf("session v3: rc=%lu sid=%s\n", rc, sid == 0x1111 ? "untouched" : sid == 0 ? "zero" : "other");
    sid = 0x1111;
    printf("session null-id: rc=%lu\n", HttpCreateServerSession(V2, NULL, 0));
    rc = HttpCreateServerSession(V2, &sid, 1);
    printf("session reserved: rc=%lu sid=%s\n", rc, sid == 0x1111 ? "untouched" : sid == 0 ? "zero" : "other");
    printf("close-session 0: rc=%lu\n", HttpCloseServerSession(0));
    printf("close-session bogus: rc=%lu\n", HttpCloseServerSession(0x1234567812345678ull));

    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("session ok: rc=%lu sid_nonzero=%d\n", rc, sid != 0);
    gid = 0x2222;
    rc = HttpCreateUrlGroup(0x1234567812345678ull, &gid, 0);
    printf("group bogus-session: rc=%lu gid=%s\n", rc, gid == 0x2222 ? "untouched" : gid == 0 ? "zero" : "other");
    gid = 0x2222;
    printf("group zero-session: rc=%lu\n", HttpCreateUrlGroup(0, &gid, 0));
    printf("group null-out: rc=%lu\n", HttpCreateUrlGroup(sid, NULL, 0));
    rc = HttpCreateUrlGroup(sid, &gid, 1);
    printf("group reserved: rc=%lu gid=%s\n", rc, gid == 0x2222 ? "untouched" : gid == 0 ? "zero" : "other");
    rc = HttpCreateUrlGroup(sid, &gid, 0);
    printf("group ok: rc=%lu gid_nonzero=%d gid_ne_sid=%d\n", rc, gid != 0, gid != sid);
    printf("close-group bogus: rc=%lu\n", HttpCloseUrlGroup(0x1234567812345678ull));
    printf("close-group 0: rc=%lu\n", HttpCloseUrlGroup(0));
    printf("close-group as-session: rc=%lu\n", HttpCloseServerSession(gid));
    printf("close-session as-group: rc=%lu\n", HttpCloseUrlGroup(sid));
    rc = HttpCloseUrlGroup(gid);
    printf("close-group ok: rc=%lu", rc);
    printf(" again=%lu\n", HttpCloseUrlGroup(gid));
    rc = HttpCreateUrlGroup(sid, &gid, 0);
    rc = HttpCloseServerSession(sid);
    printf("close-session with-group: rc=%lu", rc);
    printf(" then-group-close=%lu\n", HttpCloseUrlGroup(gid));
    printf("close-session again: rc=%lu\n", HttpCloseServerSession(sid));

    q = (HANDLE)0x3333;
    rc = HttpCreateRequestQueue(V1, NULL, NULL, 0, &q);
    printf("queue v1: rc=%lu handle_valid=%d\n", rc, q != NULL && q != (HANDLE)0x3333);
    if (rc == 0) HttpCloseRequestQueue(q);
    q = (HANDLE)0x3333;
    printf("queue null-out: rc=%lu\n", HttpCreateRequestQueue(V2, NULL, NULL, 0, NULL));
    rc = HttpCreateRequestQueue(V2, NULL, NULL, 0x10, &q);
    printf("queue flags0x10: rc=%lu handle=%s\n", rc, q == (HANDLE)0x3333 ? "untouched" : q == NULL ? "null" : "other");
    q = (HANDLE)0x3333;
    rc = HttpCreateRequestQueue(V2, NULL, NULL, HTTP_CREATE_REQUEST_QUEUE_FLAG_OPEN_EXISTING, &q);
    printf("queue open-existing unnamed: rc=%lu\n", rc);
    q = (HANDLE)0x3333;
    rc = HttpCreateRequestQueue(V2, L"vprobe_missing_queue", NULL, HTTP_CREATE_REQUEST_QUEUE_FLAG_OPEN_EXISTING, &q);
    printf("queue open-existing missing: rc=%lu handle=%s\n", rc, q == (HANDLE)0x3333 ? "untouched" : q == NULL ? "null" : "other");
    rc = HttpCreateRequestQueue(V2, NULL, NULL, HTTP_CREATE_REQUEST_QUEUE_FLAG_CONTROLLER, &q);
    printf("queue controller unnamed: rc=%lu\n", rc);
    if (rc == 0) HttpCloseRequestQueue(q);
    rc = HttpCreateRequestQueue(V2, L"vprobe_named_queue", NULL, 0, &q);
    printf("queue named: rc=%lu\n", rc);
    if (rc == 0) {
        HANDLE q2 = (HANDLE)0x3333;
        printf("queue named dup: rc=%lu\n", HttpCreateRequestQueue(V2, L"vprobe_named_queue", NULL, 0, &q2));
        HttpCloseRequestQueue(q);
    }
    rc = HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
    printf("queue ok: rc=%lu handle_valid=%d\n", rc, q != NULL && q != INVALID_HANDLE_VALUE);
    {
        DWORD flags = 0xdead;
        BOOL ok = GetHandleInformation(q, &flags);
        printf("queue handle-info: ok=%d flags=0x%lx\n", ok, flags);
    }
    rc = HttpCloseRequestQueue(q);
    printf("close-queue ok: rc=%lu", rc);
    printf(" again=%lu\n", HttpCloseRequestQueue(q));
    printf("close-queue null: rc=%lu\n", HttpCloseRequestQueue(NULL));
    printf("close-queue invalid: rc=%lu\n", HttpCloseRequestQueue(INVALID_HANDLE_VALUE));
    {
        HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        printf("close-queue event: rc=%lu\n", HttpCloseRequestQueue(ev));
        printf("shutdown-queue event: rc=%lu\n", HttpShutdownRequestQueue(ev));
        CloseHandle(ev);
    }
    printf("shutdown-queue null: rc=%lu\n", HttpShutdownRequestQueue(NULL));
    rc = HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
    printf("queue closehandle: rc=%lu close=%d\n", rc, CloseHandle(q));
    // Undo every successful HttpInitialize above.
    HttpTerminate(HTTP_INITIALIZE_SERVER | HTTP_INITIALIZE_CONFIG, NULL);
}

// ---------------------------------------------------------------------------
// URL group properties and URL registration
// ---------------------------------------------------------------------------

typedef struct {
    HTTP_SERVER_SESSION_ID sid;
    HTTP_URL_GROUP_ID gid;
    HANDLE q;
} Server;

static void server_open(Server* s) {
    HTTP_BINDING_INFO b;
    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    HttpCreateServerSession(V2, &s->sid, 0);
    HttpCreateUrlGroup(s->sid, &s->gid, 0);
    HttpCreateRequestQueue(V2, NULL, NULL, 0, &s->q);
    b.Flags.Present = 1;
    b.RequestQueueHandle = s->q;
    HttpSetUrlGroupProperty(s->gid, HttpServerBindingProperty, &b, sizeof b);
}

static void server_close(Server* s) {
    HttpCloseUrlGroup(s->gid);
    HttpCloseServerSession(s->sid);
    HttpCloseRequestQueue(s->q);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
}

static void properties(void) {
    Server s;
    HTTP_BINDING_INFO b;
    HTTP_STATE_INFO st;
    HTTP_TIMEOUT_LIMIT_INFO t;
    HTTP_QOS_SETTING_INFO qos;
    HTTP_SERVER_AUTHENTICATION_INFO auth;
    HTTP_LOGGING_INFO log;
    ULONG qlen = 100, rc, ret = 0xdead;
    BYTE buf[64];

    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    HttpCreateServerSession(V2, &s.sid, 0);
    HttpCreateUrlGroup(s.sid, &s.gid, 0);
    HttpCreateRequestQueue(V2, NULL, NULL, 0, &s.q);

    memset(&b, 0, sizeof b);
    b.Flags.Present = 1;
    b.RequestQueueHandle = s.q;
    printf("bind ok: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b));
    printf("bind short: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b - 1));
    printf("bind long: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b + 4));
    printf("bind null: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, NULL, sizeof b));
    printf("bind bogus-group: rc=%lu\n", HttpSetUrlGroupProperty(0x1234567812345678ull, HttpServerBindingProperty, &b, sizeof b));
    printf("bind on-session: rc=%lu\n", HttpSetUrlGroupProperty(s.sid, HttpServerBindingProperty, &b, sizeof b));
    printf("session bind: rc=%lu\n", HttpSetServerSessionProperty(s.sid, HttpServerBindingProperty, &b, sizeof b));
    b.RequestQueueHandle = (HANDLE)(ULONG_PTR)0x7ffc;
    printf("bind bogus-handle: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b));
    b.RequestQueueHandle = NULL;
    printf("bind null-handle: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b));
    {
        HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        b.RequestQueueHandle = ev;
        printf("bind event-handle: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b));
        CloseHandle(ev);
    }
    b.Flags.Present = 0;
    b.RequestQueueHandle = NULL;
    printf("unbind: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b));
    memset(buf, 0xcc, sizeof buf);
    ret = 0xdead;
    rc = HttpQueryUrlGroupProperty(s.gid, HttpServerBindingProperty, buf, sizeof buf, &ret);
    printf("query bind: rc=%lu ret=%lu\n", rc, ret);

    st.Flags.Present = 1;
    st.State = HttpEnabledStateInactive;
    printf("state inactive: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st));
    st.State = (HTTP_ENABLED_STATE)2;
    printf("state 2: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st));
    st.State = HttpEnabledStateActive;
    st.Flags.Present = 0;
    printf("state not-present: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st));
    st.Flags.Present = 1;
    printf("state active: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st));
    memset(buf, 0xcc, sizeof buf);
    ret = 0xdead;
    rc = HttpQueryUrlGroupProperty(s.gid, HttpServerStateProperty, buf, sizeof buf, &ret);
    printf("query state: rc=%lu ret=%lu present=%u state=%lu\n", rc, ret, ((HTTP_STATE_INFO*)buf)->Flags.Present,
           (unsigned long)((HTTP_STATE_INFO*)buf)->State);

    memset(&t, 0, sizeof t);
    t.Flags.Present = 1;
    t.IdleConnection = 30;
    t.HeaderWait = 20;
    printf("timeouts: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerTimeoutsProperty, &t, sizeof t));
    t.MinSendRate = 10;
    printf("timeouts minsend10: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerTimeoutsProperty, &t, sizeof t));
    t.MinSendRate = 0;
    t.RequestQueue = 5;
    printf("timeouts requestqueue-on-group: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerTimeoutsProperty, &t, sizeof t));
    memset(buf, 0xcc, sizeof buf);
    ret = 0xdead;
    rc = HttpQueryUrlGroupProperty(s.gid, HttpServerTimeoutsProperty, buf, sizeof buf, &ret);
    printf("query timeouts: rc=%lu ret=%lu idle=%u header=%u\n", rc, ret,
           ((HTTP_TIMEOUT_LIMIT_INFO*)buf)->IdleConnection, ((HTTP_TIMEOUT_LIMIT_INFO*)buf)->HeaderWait);

    printf("queuelength on-group: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerQueueLengthProperty, &qlen, sizeof qlen));
    printf("queuelength on-queue: rc=%lu\n", HttpSetRequestQueueProperty(s.q, HttpServerQueueLengthProperty, &qlen, sizeof qlen, 0, NULL));
    printf("property 6: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, (HTTP_SERVER_PROPERTY)6, &qlen, sizeof qlen));
    printf("property 99: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, (HTTP_SERVER_PROPERTY)99, &qlen, sizeof qlen));

    memset(&qos, 0, sizeof qos);
    printf("qos zero: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerQosProperty, &qos, sizeof qos));
    memset(&auth, 0, sizeof auth);
    auth.Flags.Present = 0;
    printf("auth not-present: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerAuthenticationProperty, &auth, sizeof auth));
    auth.Flags.Present = 1;
    printf("auth present-none: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerAuthenticationProperty, &auth, sizeof auth));
    memset(&log, 0, sizeof log);
    printf("logging not-present: rc=%lu\n", HttpSetUrlGroupProperty(s.gid, HttpServerLoggingProperty, &log, sizeof log));

    HttpCloseUrlGroup(s.gid);
    HttpCloseServerSession(s.sid);
    HttpCloseRequestQueue(s.q);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
}

static void urls(void) {
    Server s, s2;
    ULONG rc;
    server_open(&s);
    printf("addurl ok: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 7, 0));
    printf("addurl dup: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 7, 0));
    printf("addurl dup-case: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"HTTP://LOCALHOST:" L"48213" L"/VPROBE/", 7, 0));
    printf("addurl nested: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/sub/", 8, 0));
    printf("addurl no-slash: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe2", 0, 0));
    printf("addurl no-port: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"http://localhost/vprobe3/", 0, 0));
    printf("addurl port0: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"http://localhost:0/vprobe3/", 0, 0));
    printf("addurl port70000: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"http://localhost:70000/vprobe3/", 0, 0));
    printf("addurl ftp: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"ftp://localhost:" L"48213" L"/vprobe3/", 0, 0));
    printf("addurl garbage: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"not a url", 0, 0));
    printf("addurl empty: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"", 0, 0));
    printf("addurl null: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, NULL, 0, 0));
    printf("addurl reserved: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe4/", 0, 1));
    printf("addurl bogus-group: rc=%lu\n", HttpAddUrlToUrlGroup(0x1234567812345678ull, BASE L"/vprobe4/", 0, 0));
    printf("addurl query: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe5/?x=1", 0, 0));
    printf("addurl strong: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"http://+:" L"48213" L"/vprobe6/", 0, 0));
    printf("addurl weak: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"http://*:" L"48213" L"/vprobe7/", 0, 0));
    printf("addurl ip: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, L"http://127.0.0.1:" L"48213" L"/vprobe8/", 0, 0));

    server_open(&s2);
    printf("addurl other-group same: rc=%lu\n", HttpAddUrlToUrlGroup(s2.gid, BASE L"/vprobe/", 9, 0));
    printf("addurl other-group child: rc=%lu\n", HttpAddUrlToUrlGroup(s2.gid, BASE L"/vprobe/other/", 9, 0));
    server_close(&s2);

    printf("removeurl missing: rc=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, BASE L"/nothere/", 0));
    printf("removeurl no-slash: rc=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, BASE L"/vprobe2", 0));
    printf("removeurl flags2: rc=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, BASE L"/vprobe/", 2));
    printf("removeurl null-noflag: rc=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, NULL, 0));
    printf("removeurl all-with-url: rc=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, BASE L"/vprobe/", HTTP_URL_FLAG_REMOVE_ALL));
    printf("removeurl bogus-group: rc=%lu\n", HttpRemoveUrlFromUrlGroup(0x1234567812345678ull, BASE L"/vprobe/", 0));
    rc = HttpRemoveUrlFromUrlGroup(s.gid, BASE L"/vprobe/sub/", 0);
    printf("removeurl ok: rc=%lu", rc);
    printf(" again=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, BASE L"/vprobe/sub/", 0));
    printf("removeurl case: rc=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, L"HTTP://LOCALHOST:" L"48213" L"/VPROBE/", 0));
    rc = HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 7, 0);
    printf("removeurl all: re-add=%lu rc=%lu", rc, HttpRemoveUrlFromUrlGroup(s.gid, NULL, HTTP_URL_FLAG_REMOVE_ALL));
    printf(" again=%lu\n", HttpRemoveUrlFromUrlGroup(s.gid, NULL, HTTP_URL_FLAG_REMOVE_ALL));
    server_close(&s);
}

// ---------------------------------------------------------------------------
// Request receive
// ---------------------------------------------------------------------------

static void wstr(const char* tag, const WCHAR* p, USHORT bytes) {
    USHORT i;
    printf(" %s=", tag);
    if (!p) { printf("(null)"); return; }
    putchar('"');
    for (i = 0; i < bytes / 2; ++i) putchar((char)p[i]);
    putchar('"');
}

static void astr(const char* tag, const char* p, USHORT bytes) {
    printf(" %s=", tag);
    if (!p) { printf("(null)"); return; }
    printf("\"%.*s\"", bytes, p);
}

static void dump_request(const char* tag, const HTTP_REQUEST_V2* r, ULONG returned, ULONG buffer_len) {
    int i;
    const BYTE* base = (const BYTE*)r;
    printf("%s: flags=0x%lx conn_nonzero=%d req_nonzero=%d ctx=%llu ver=%u.%u verb=%d", tag, r->Flags,
           r->ConnectionId != 0, r->RequestId != 0, (unsigned long long)r->UrlContext, r->Version.MajorVersion,
           r->Version.MinorVersion, (int)r->Verb);
    astr("unknown_verb", r->pUnknownVerb, r->UnknownVerbLength);
    astr("raw", r->pRawUrl, r->RawUrlLength);
    printf(" raw_nul=%d", r->pRawUrl ? r->pRawUrl[r->RawUrlLength] == 0 : -1);
    wstr("full", r->CookedUrl.pFullUrl, r->CookedUrl.FullUrlLength);
    wstr("host", r->CookedUrl.pHost, r->CookedUrl.HostLength);
    wstr("abs", r->CookedUrl.pAbsPath, r->CookedUrl.AbsPathLength);
    wstr("query", r->CookedUrl.pQueryString, r->CookedUrl.QueryStringLength);
    printf(" full_nul=%d host_in_full=%d abs_in_full=%d",
           r->CookedUrl.pFullUrl ? r->CookedUrl.pFullUrl[r->CookedUrl.FullUrlLength / 2] == 0 : -1,
           r->CookedUrl.pHost == r->CookedUrl.pFullUrl + 7,
           r->CookedUrl.pAbsPath == r->CookedUrl.pHost + r->CookedUrl.HostLength / 2);
    if (r->Address.pRemoteAddress) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)r->Address.pRemoteAddress;
        const struct sockaddr_in* l = (const struct sockaddr_in*)r->Address.pLocalAddress;
        printf(" remote_family=%u remote_ip=0x%08lx local_family=%u local_port=%u local_ip=0x%08lx", a->sin_family,
               (unsigned long)ntohl(a->sin_addr.s_addr), l->sin_family, ntohs(l->sin_port),
               (unsigned long)ntohl(l->sin_addr.s_addr));
    }
    printf(" known=[");
    for (i = 0; i < HttpHeaderRequestMaximum; ++i) {
        const HTTP_KNOWN_HEADER* h = &r->Headers.KnownHeaders[i];
        if (h->RawValueLength || h->pRawValue)
            printf("%d:\"%.*s\"(nul=%d),", i, h->RawValueLength, h->pRawValue ? h->pRawValue : "",
                   h->pRawValue ? h->pRawValue[h->RawValueLength] == 0 : -1);
    }
    printf("] unknown=[");
    for (i = 0; i < r->Headers.UnknownHeaderCount; ++i) {
        const HTTP_UNKNOWN_HEADER* h = &r->Headers.pUnknownHeaders[i];
        printf("\"%.*s\":\"%.*s\",", h->NameLength, h->pName, h->RawValueLength, h->pRawValue);
    }
    printf("] trailers=%u bytes_received=%llu chunks=%u", r->Headers.TrailerCount,
           (unsigned long long)r->BytesReceived, r->EntityChunkCount);
    for (i = 0; i < r->EntityChunkCount; ++i) {
        const HTTP_DATA_CHUNK* c = &r->pEntityChunks[i];
        printf(" chunk%d(type=%d len=%lu \"%.*s\")", i, c->DataChunkType, c->FromMemory.BufferLength,
               (int)c->FromMemory.BufferLength, (const char*)c->FromMemory.pBuffer);
    }
    printf(" raw_conn_nonzero=%d ssl=%d info_count=%u", r->RawConnectionId != 0, r->pSslInfo != NULL,
           r->RequestInfoCount);
    for (i = 0; i < r->RequestInfoCount; ++i) printf(" info%d(type=%d len=%lu)", i, r->pRequestInfo[i].InfoType,
                                                   r->pRequestInfo[i].InfoLength);
    // Where the variable data lands: inside the caller's buffer, after the fixed part.
    printf(" raw_off_ok=%d returned=%lu in_buffer=%d\n",
           r->pRawUrl ? ((const BYTE*)r->pRawUrl >= base + sizeof(HTTP_REQUEST_V2)) : -1, returned,
           r->pRawUrl ? ((const BYTE*)r->pRawUrl < base + buffer_len) : -1);
}

static ULONG send_simple(HANDLE q, HTTP_REQUEST_ID id, USHORT status, const char* reason, const char* body,
                         ULONG flags, ULONG* sent) {
    HTTP_RESPONSE_V2 resp;
    HTTP_DATA_CHUNK chunk;
    memset(&resp, 0, sizeof resp);
    resp.Version.MajorVersion = 1;
    resp.Version.MinorVersion = 1;
    resp.StatusCode = status;
    resp.pReason = reason;
    resp.ReasonLength = (USHORT)strlen(reason);
    if (body) {
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = (PVOID)body;
        chunk.FromMemory.BufferLength = (ULONG)strlen(body);
        resp.EntityChunkCount = 1;
        resp.pEntityChunks = &chunk;
    }
    return HttpSendHttpResponse(q, id, flags, (PHTTP_RESPONSE)&resp, NULL, sent, NULL, 0, NULL, NULL);
}

static __declspec(align(8)) BYTE g_buf[16384];

// A receive that gives up after 3 s (a request http.sys rejects itself never
// reaches the queue): ERROR_TIMEOUT then.
static ULONG recv_timed(HANDLE q, ULONG flags, ULONG* got) {
    OVERLAPPED ov;
    DWORD n = 0;
    ULONG rc;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    rc = HttpReceiveHttpRequest(q, HTTP_NULL_ID, flags, (PHTTP_REQUEST)g_buf, sizeof g_buf, NULL, &ov);
    if (rc == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 3000) != WAIT_OBJECT_0) {
            CancelIoEx(q, &ov);
            GetOverlappedResult(q, &ov, &n, TRUE);
            CloseHandle(ov.hEvent);
            return ERROR_TIMEOUT;
        }
        rc = GetOverlappedResult(q, &ov, &n, FALSE) ? 0 : GetLastError();
    } else if (rc == 0) {
        GetOverlappedResult(q, &ov, &n, TRUE);
    }
    if (got) *got = n;
    CloseHandle(ov.hEvent);
    return rc;
}

static void receive_sync(void) {
    Server s;
    Client c;
    ULONG rc, got;
    HTTP_REQUEST_V2* r = (HTTP_REQUEST_V2*)g_buf;
    HTTP_REQUEST_ID id;

    server_open(&s);
    HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 7, 0);
    HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/deep/", 8, 0);

    client_start(&c, "GET /vprobe/a%20b/c?x=1&y=%41 HTTP/1.1\r\nHost: localhost:" PORTS "\r\n"
                     "User-Agent: vprobe\r\nAccept: */*\r\nX-Custom:   padded value  \r\nx-lower: v\r\n"
                     "Cookie: a=1\r\nCookie: b=2\r\n\r\n", 0);
    got = 0xdead;
    memset(g_buf, 0xcc, sizeof g_buf);
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
    printf("recv get: rc=%lu\n", rc);
    if (rc == 0) {
        dump_request("recv get", r, got, sizeof g_buf);
        got = 0xdead;
        rc = send_simple(s.q, r->RequestId, 200, "OK", "hello", 0, &got);
        printf("send basic: rc=%lu sent=%lu\n", rc, got);
        printf("send again: rc=%lu\n", send_simple(s.q, r->RequestId, 200, "OK", "x", 0, NULL));
    }
    client_join(&c);
    print_wire("send basic", &c);

    // Longest-prefix routing and the URL context.
    client_start(&c, "GET /vprobe/deep/x HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
    printf("recv deep: rc=%lu ctx=%llu\n", rc, (unsigned long long)r->UrlContext);
    if (rc == 0) send_simple(s.q, r->RequestId, 204, "No Content", NULL, 0, NULL);
    client_join(&c);
    print_wire("send 204", &c);

    // Buffer too small for the variable part, then the retry by RequestId.
    client_start(&c, "GET /vprobe/small HTTP/1.1\r\nHost: localhost:" PORTS "\r\nUser-Agent: vprobe\r\n\r\n", 0);
    got = 0xdead;
    memset(g_buf, 0xcc, sizeof g_buf);
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof(HTTP_REQUEST_V2), &got, NULL);
    id = r->RequestId;
    printf("recv small: rc=%lu got=%lu needed_gt_fixed=%d id_nonzero=%d flags=0x%lx verb=%d raw_ptr_null=%d\n", rc,
           got, got > sizeof(HTTP_REQUEST_V2), id != 0, r->Flags, (int)r->Verb, r->pRawUrl == NULL);
    {
        ULONG got2 = 0xdead;
        rc = HttpReceiveHttpRequest(s.q, id, 0, (PHTTP_REQUEST)r, got - 1, &got2, NULL);
        printf("recv retry-short: rc=%lu got=%lu same=%d\n", rc, got2, got2 == got);
        got2 = 0xdead;
        rc = HttpReceiveHttpRequest(s.q, id, 0, (PHTTP_REQUEST)r, got, &got2, NULL);
        printf("recv retry: rc=%lu got=%lu same=%d raw_ok=%d\n", rc, got2, got2 == got,
               r->pRawUrl && strncmp(r->pRawUrl, "/vprobe/small", 13) == 0);
        rc = HttpReceiveHttpRequest(s.q, id, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got2, NULL);
        printf("recv retry-after-success: rc=%lu", rc);
        printf(" send=%lu\n", send_simple(s.q, id, 200, "OK", "", 0, NULL));
    }
    client_join(&c);
    print_wire("send empty-body", &c);

    // Below the fixed size.
    client_start(&c, "GET /vprobe/tiny HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
    got = 0xdead;
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof(HTTP_REQUEST_V1), &got, NULL);
    printf("recv v1-size: rc=%lu got_gt=%d\n", rc, got != 0xdead && got > sizeof(HTTP_REQUEST_V1));
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, 16, &got, NULL);
    printf("recv 16-bytes: rc=%lu\n", rc);
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, NULL, sizeof g_buf, &got, NULL);
    printf("recv null-buffer: rc=%lu\n", rc);
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 4, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
    printf("recv flags4: rc=%lu\n", rc);
    rc = HttpReceiveHttpRequest(s.q, 0x1234567812345678ull, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
    printf("recv bogus-id: rc=%lu\n", rc);
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
    printf("recv after-small: rc=%lu raw_ok=%d\n", rc, rc == 0 && strncmp(r->pRawUrl, "/vprobe/tiny", 12) == 0);
    if (rc == 0) send_simple(s.q, r->RequestId, 200, "OK", "t", HTTP_SEND_RESPONSE_FLAG_DISCONNECT, NULL);
    client_join(&c);
    print_wire("send disconnect", &c);

    // POST body without and with COPY_BODY, then HttpReceiveRequestEntityBody.
    client_start(&c, "POST /vprobe/post HTTP/1.1\r\nHost: localhost:" PORTS "\r\nContent-Length: 11\r\n"
                     "Content-Type: text/plain\r\n\r\nhello world", 0);
    Sleep(200);
    rc = recv_timed(s.q, 0, &got);
    if (rc == 0) {
        char body[64];
        ULONG n = 0xdead;
        dump_request("recv post", r, got, sizeof g_buf);
        id = r->RequestId;
        memset(body, 0, sizeof body);
        rc = HttpReceiveRequestEntityBody(s.q, id, 0, body, 5, &n, NULL);
        printf("entity first5: rc=%lu n=%lu \"%.*s\"\n", rc, n, (int)n, body);
        rc = HttpReceiveRequestEntityBody(s.q, id, 0, body, sizeof body, &n, NULL);
        printf("entity rest: rc=%lu n=%lu \"%.*s\"\n", rc, n, (int)n, body);
        n = 0xdead;
        rc = HttpReceiveRequestEntityBody(s.q, id, 0, body, sizeof body, &n, NULL);
        printf("entity eof: rc=%lu n=%lu\n", rc, n);
        send_simple(s.q, id, 201, "Created", "ok", 0, NULL);
    } else {
        printf("recv post: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("send 201", &c);

    client_start(&c, "POST /vprobe/post HTTP/1.1\r\nHost: localhost:" PORTS "\r\nContent-Length: 11\r\n\r\nhello world", 0);
    Sleep(200);
    rc = recv_timed(s.q, HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY, &got);
    if (rc == 0) {
        char body[16];
        ULONG n = 0xdead;
        dump_request("recv post copy", r, got, sizeof g_buf);
        rc = HttpReceiveRequestEntityBody(s.q, r->RequestId, 0, body, sizeof body, &n, NULL);
        printf("entity after-copy: rc=%lu n=%lu\n", rc, n);
        send_simple(s.q, r->RequestId, 200, "OK", NULL, 0, NULL);
    } else {
        printf("recv post copy: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("send no-body", &c);

    // Chunked request body with COPY_BODY.
    client_start(&c, "POST /vprobe/chunked HTTP/1.1\r\nHost: localhost:" PORTS "\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", 0);
    Sleep(200);
    rc = recv_timed(s.q, HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY, &got);
    if (rc == 0) {
        char body[32];
        ULONG n = 0xdead;
        dump_request("recv chunked", r, got, sizeof g_buf);
        rc = HttpReceiveRequestEntityBody(s.q, r->RequestId, 0, body, sizeof body, &n, NULL);
        printf("entity chunked-rest: rc=%lu n=%lu \"%.*s\"\n", rc, rc == 0 ? n : 0, rc == 0 ? (int)n : 0, body);
        send_simple(s.q, r->RequestId, 200, "OK", "c", 0, NULL);
    } else {
        printf("recv chunked: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("send after-chunked", &c);

    // HTTP/1.0, HEAD, unknown verb, absolute-URI request line.
    client_start(&c, "GET /vprobe/old HTTP/1.0\r\n\r\n", 0);
    rc = recv_timed(s.q, 0, &got);
    printf("recv http10: rc=%lu\n", rc);
    if (rc == 0) { dump_request("recv http10", r, got, sizeof g_buf); send_simple(s.q, r->RequestId, 200, "OK", "old", 0, NULL); }
    client_join(&c);
    print_wire("send http10", &c);

    client_start(&c, "GET /vprobe/old HTTP/1.0\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
    rc = recv_timed(s.q, 0, &got);
    if (rc == 0) { dump_request("recv http10 host", r, got, sizeof g_buf); send_simple(s.q, r->RequestId, 200, "OK", "old", 0, NULL); }
    client_join(&c);
    print_wire("send http10 host", &c);

    client_start(&c, "HEAD /vprobe/head HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
    rc = recv_timed(s.q, 0, &got);
    if (rc == 0) { dump_request("recv head", r, got, sizeof g_buf); send_simple(s.q, r->RequestId, 200, "OK", "headbody", 0, NULL); }
    client_join(&c);
    print_wire("send head", &c);

    client_start(&c, "BREW /vprobe/pot HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
    rc = recv_timed(s.q, 0, &got);
    if (rc == 0) { dump_request("recv brew", r, got, sizeof g_buf); send_simple(s.q, r->RequestId, 418, "Teapot", NULL, 0, NULL); }
    client_join(&c);
    print_wire("send 418", &c);

    client_start(&c, "GET http://localhost:" PORTS "/vprobe/abs?q HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
    rc = recv_timed(s.q, 0, &got);
    if (rc == 0) { dump_request("recv absolute", r, got, sizeof g_buf); send_simple(s.q, r->RequestId, 200, "OK", NULL, 0, NULL); }
    client_join(&c);

    client_start(&c, "GET /vprobe/nohost HTTP/1.1\r\n\r\n", 0);
    client_join(&c);
    print_status("nohost", &c);

    client_start(&c, "GET /elsewhere HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
    client_join(&c);
    print_status("unregistered", &c);

    client_start(&c, "garbage\r\n\r\n", 0);
    client_join(&c);
    print_status("garbage", &c);

    // Keep-alive: two requests on one connection.
    {
        Client k;
        HTTP_CONNECTION_ID c1;
        client_start(&k, "GET /vprobe/k1 HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n"
                         "GET /vprobe/k2 HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
        rc = recv_timed(s.q, 0, &got);
        c1 = r->ConnectionId;
        printf("keepalive first: rc=%lu raw=\"%.*s\"\n", rc, r->RawUrlLength, r->pRawUrl);
        send_simple(s.q, r->RequestId, 200, "OK", "1", 0, NULL);
        rc = recv_timed(s.q, 0, &got);
        printf("keepalive second: rc=%lu raw=\"%.*s\" same_conn=%d\n", rc, r->RawUrlLength, r->pRawUrl,
               c1 == r->ConnectionId);
        send_simple(s.q, r->RequestId, 200, "OK", "2", 0, NULL);
        client_join(&k);
        print_wire("keepalive", &k);
    }

    // Group state inactive: the request is refused by the server.
    {
        HTTP_STATE_INFO st;
        st.Flags.Present = 1;
        st.State = HttpEnabledStateInactive;
        HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st);
        client_start(&c, "GET /vprobe/inactive HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
        client_join(&c);
        print_status("inactive", &c);
        st.State = HttpEnabledStateActive;
        HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st);
    }
    // Unbound group.
    {
        HTTP_BINDING_INFO b;
        b.Flags.Present = 0;
        b.RequestQueueHandle = NULL;
        HttpSetUrlGroupProperty(s.gid, HttpServerBindingProperty, &b, sizeof b);
        client_start(&c, "GET /vprobe/unbound HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n", 0);
        client_join(&c);
        print_status("unbound", &c);
    }
    server_close(&s);
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

static ULONG recv_one(Server* s, const char* req, Client* c) {
    ULONG got, rc;
    client_start(c, req, 0);
    rc = recv_timed(s->q, 0, &got);
    return rc;
}

static void responses(void) {
    Server s;
    Client c;
    HTTP_REQUEST_V2* r = (HTTP_REQUEST_V2*)g_buf;
    HTTP_RESPONSE_V2 resp;
    HTTP_DATA_CHUNK chunks[3];
    HTTP_UNKNOWN_HEADER unk[2];
    ULONG rc, sent;
    const char* get = "GET /vprobe/r HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n";

    server_open(&s);
    HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 1, 0);

    // Known and unknown headers, explicit Content-Length, two chunks.
    if (recv_one(&s, get, &c) == 0) {
        memset(&resp, 0, sizeof resp);
        resp.Version.MajorVersion = 1;
        resp.Version.MinorVersion = 1;
        resp.StatusCode = 200;
        resp.pReason = "Fine";
        resp.ReasonLength = 4;
        resp.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = "text/html";
        resp.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = 9;
        resp.Headers.KnownHeaders[HttpHeaderContentLength].pRawValue = "8";
        resp.Headers.KnownHeaders[HttpHeaderContentLength].RawValueLength = 1;
        resp.Headers.KnownHeaders[HttpHeaderLocation].pRawValue = "/x";
        resp.Headers.KnownHeaders[HttpHeaderLocation].RawValueLength = 2;
        resp.Headers.KnownHeaders[HttpHeaderCacheControl].pRawValue = "no-cache";
        resp.Headers.KnownHeaders[HttpHeaderCacheControl].RawValueLength = 8;
        unk[0].pName = "X-Probe";
        unk[0].NameLength = 7;
        unk[0].pRawValue = "one";
        unk[0].RawValueLength = 3;
        unk[1].pName = "X-Two";
        unk[1].NameLength = 5;
        unk[1].pRawValue = "2";
        unk[1].RawValueLength = 1;
        resp.Headers.UnknownHeaderCount = 2;
        resp.Headers.pUnknownHeaders = unk;
        chunks[0].DataChunkType = HttpDataChunkFromMemory;
        chunks[0].FromMemory.pBuffer = "abcd";
        chunks[0].FromMemory.BufferLength = 4;
        chunks[1].DataChunkType = HttpDataChunkFromMemory;
        chunks[1].FromMemory.pBuffer = "efgh";
        chunks[1].FromMemory.BufferLength = 4;
        resp.EntityChunkCount = 2;
        resp.pEntityChunks = chunks;
        sent = 0xdead;
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, &sent, NULL, 0, NULL, NULL);
        printf("send headers: rc=%lu sent=%lu\n", rc, sent);
    }
    client_join(&c);
    print_wire("send headers", &c);

    // Server / Date / Connection given by the caller.
    if (recv_one(&s, get, &c) == 0) {
        memset(&resp, 0, sizeof resp);
        resp.Version.MajorVersion = 1;
        resp.Version.MinorVersion = 1;
        resp.StatusCode = 200;
        resp.Headers.KnownHeaders[HttpHeaderServer].pRawValue = "Mine/1";
        resp.Headers.KnownHeaders[HttpHeaderServer].RawValueLength = 6;
        resp.Headers.KnownHeaders[HttpHeaderDate].pRawValue = "Thu, 01 Jan 2026 00:00:00 GMT";
        resp.Headers.KnownHeaders[HttpHeaderDate].RawValueLength = 29;
        resp.Headers.KnownHeaders[HttpHeaderConnection].pRawValue = "close";
        resp.Headers.KnownHeaders[HttpHeaderConnection].RawValueLength = 5;
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        printf("send caller-server: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("send caller-server", &c);

    // Version 1.0 in the response, no reason.
    if (recv_one(&s, get, &c) == 0) {
        memset(&resp, 0, sizeof resp);
        resp.Version.MajorVersion = 1;
        resp.Version.MinorVersion = 0;
        resp.StatusCode = 404;
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        printf("send v10-noreason: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("send v10-noreason", &c);

    // Version 0.0 in the response.
    if (recv_one(&s, get, &c) == 0) {
        memset(&resp, 0, sizeof resp);
        resp.StatusCode = 500;
        resp.pReason = "Bad";
        resp.ReasonLength = 3;
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        printf("send v00: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("send v00", &c);

    // MORE_DATA then HttpSendResponseEntityBody (no Content-Length: chunked?).
    if (recv_one(&s, get, &c) == 0) {
        HTTP_REQUEST_ID id = r->RequestId;
        memset(&resp, 0, sizeof resp);
        resp.Version.MajorVersion = 1;
        resp.Version.MinorVersion = 1;
        resp.StatusCode = 200;
        resp.pReason = "OK";
        resp.ReasonLength = 2;
        chunks[0].DataChunkType = HttpDataChunkFromMemory;
        chunks[0].FromMemory.pBuffer = "part1";
        chunks[0].FromMemory.BufferLength = 5;
        resp.EntityChunkCount = 1;
        resp.pEntityChunks = chunks;
        sent = 0xdead;
        rc = HttpSendHttpResponse(s.q, id, HTTP_SEND_RESPONSE_FLAG_MORE_DATA, (PHTTP_RESPONSE)&resp, NULL, &sent,
                                  NULL, 0, NULL, NULL);
        printf("send more-data: rc=%lu sent=%lu\n", rc, sent);
        chunks[0].FromMemory.pBuffer = "part2";
        sent = 0xdead;
        rc = HttpSendResponseEntityBody(s.q, id, HTTP_SEND_RESPONSE_FLAG_MORE_DATA, 1, chunks, &sent, NULL, 0, NULL, NULL);
        printf("entity-send more: rc=%lu sent=%lu\n", rc, sent);
        sent = 0xdead;
        rc = HttpSendResponseEntityBody(s.q, id, 0, 0, NULL, &sent, NULL, 0, NULL, NULL);
        printf("entity-send final-empty: rc=%lu sent=%lu\n", rc, sent);
        rc = HttpSendResponseEntityBody(s.q, id, 0, 1, chunks, NULL, NULL, 0, NULL, NULL);
        printf("entity-send after-final: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("send streamed", &c);

    // MORE_DATA with an explicit Content-Length.
    if (recv_one(&s, get, &c) == 0) {
        HTTP_REQUEST_ID id = r->RequestId;
        memset(&resp, 0, sizeof resp);
        resp.Version.MajorVersion = 1;
        resp.Version.MinorVersion = 1;
        resp.StatusCode = 200;
        resp.Headers.KnownHeaders[HttpHeaderContentLength].pRawValue = "6";
        resp.Headers.KnownHeaders[HttpHeaderContentLength].RawValueLength = 1;
        rc = HttpSendHttpResponse(s.q, id, HTTP_SEND_RESPONSE_FLAG_MORE_DATA, (PHTTP_RESPONSE)&resp, NULL, NULL,
                                  NULL, 0, NULL, NULL);
        chunks[0].DataChunkType = HttpDataChunkFromMemory;
        chunks[0].FromMemory.pBuffer = "abc";
        chunks[0].FromMemory.BufferLength = 3;
        chunks[1] = chunks[0];
        chunks[1].FromMemory.pBuffer = "def";
        printf("send cl-more: rc=%lu", rc);
        printf(" entity=%lu\n", HttpSendResponseEntityBody(s.q, id, 0, 2, chunks, NULL, NULL, 0, NULL, NULL));
    }
    client_join(&c);
    print_wire("send cl-streamed", &c);

    // Argument errors.
    if (recv_one(&s, get, &c) == 0) {
        HTTP_REQUEST_ID id = r->RequestId;
        memset(&resp, 0, sizeof resp);
        resp.Version.MajorVersion = 1;
        resp.Version.MinorVersion = 1;
        resp.StatusCode = 1000;
        printf("send status1000: rc=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        resp.StatusCode = 200;
        // A NULL pHttpResponse faults inside httpapi.dll (access violation), not recorded.
        printf("send bad-flags: rc=%lu\n", HttpSendHttpResponse(s.q, id, 0x80000000u, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        printf("send reserved2: rc=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 1, NULL, NULL));
        printf("send bogus-id: rc=%lu\n", HttpSendHttpResponse(s.q, 0x1234567812345678ull, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        printf("send null-id: rc=%lu\n", HttpSendHttpResponse(s.q, 0, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        resp.EntityChunkCount = 1;
        resp.pEntityChunks = NULL;
        printf("send null-chunks: rc=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        chunks[0].DataChunkType = (HTTP_DATA_CHUNK_TYPE)99;
        resp.pEntityChunks = chunks;
        printf("send bad-chunk-type: rc=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        resp.EntityChunkCount = 0;
        resp.pEntityChunks = NULL;
        unk[0].pName = NULL;
        unk[0].NameLength = 3;
        resp.Headers.UnknownHeaderCount = 1;
        resp.Headers.pUnknownHeaders = unk;
        printf("send null-header-name: rc=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        resp.Headers.UnknownHeaderCount = 0;
        resp.Headers.pUnknownHeaders = NULL;
        printf("send after-errors: rc=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
    }
    client_join(&c);
    print_wire("send after-errors", &c);

    if (recv_one(&s, get, &c) == 0) {
        HTTP_REQUEST_ID id = r->RequestId;
        memset(&resp, 0, sizeof resp);
        resp.StatusCode = 99;
        printf("send status99: rc=%lu", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        resp.StatusCode = 200;
        printf(" then-200=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
    }
    client_join(&c);
    print_wire("send status99", &c);
    if (recv_one(&s, get, &c) == 0) {
        HTTP_REQUEST_ID id = r->RequestId;
        memset(&resp, 0, sizeof resp);
        resp.StatusCode = 100;
        printf("send status100: rc=%lu", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
        resp.StatusCode = 999;
        printf(" then-999=%lu\n", HttpSendHttpResponse(s.q, id, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
    }
    client_join(&c);
    print_wire("send status100", &c);

    // A response after the client went away.
    {
        Client gone;
        client_start(&gone, get, 0);
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)g_buf, sizeof g_buf, &sent, NULL);
        client_join(&gone);  // the client times out after 1.5 s and closes
        Sleep(300);
        printf("send client-gone: recv=%lu rc=%lu\n", rc, send_simple(s.q, r->RequestId, 200, "OK", "late", 0, NULL));
    }
    server_close(&s);
}

// ---------------------------------------------------------------------------
// Overlapped and IOCP receive, shutdown, cancel
// ---------------------------------------------------------------------------

static void overlapped_io(void) {
    Server s;
    Client c;
    HTTP_REQUEST_V2* r = (HTTP_REQUEST_V2*)g_buf;
    OVERLAPPED ov;
    ULONG rc, got;
    DWORD n;
    BOOL ok;
    const char* get = "GET /vprobe/o HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n";

    server_open(&s);
    HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 3, 0);

    // Pending receive, completed by a later request (event).
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, TRUE, NULL);
    got = 0xdead;
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, &ov);
    printf("ovl pending: rc=%lu got=0x%lx internal=0x%llx event_reset=%d\n", rc, got,
           (unsigned long long)ov.Internal, WaitForSingleObject(ov.hEvent, 0) == WAIT_TIMEOUT);
    client_start(&c, get, 100);
    n = 0;
    ok = GetOverlappedResult(s.q, &ov, &n, TRUE);
    printf("ovl complete: ok=%d n_eq_used=%d internal=0x%llx raw=\"%.*s\"\n", ok, n > sizeof(HTTP_REQUEST_V2),
           (unsigned long long)ov.Internal, r->RawUrlLength, r->pRawUrl);
    send_simple(s.q, r->RequestId, 200, "OK", "o", 0, NULL);
    client_join(&c);

    // Request already queued when the overlapped receive is issued.
    client_start(&c, get, 0);
    Sleep(300);
    memset(&ov.Internal, 0, sizeof ov - sizeof ov.hEvent);
    got = 0xdead;
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, &ov);
    printf("ovl immediate: rc=%lu got_set=%d internal=0x%llx event=%d\n", rc, got != 0xdead,
           (unsigned long long)ov.Internal, WaitForSingleObject(ov.hEvent, 0) == WAIT_OBJECT_0);
    ok = GetOverlappedResult(s.q, &ov, &n, TRUE);
    printf("ovl immediate result: ok=%d n_eq_got=%d\n", ok, got == 0xdead || n == got);
    send_simple(s.q, r->RequestId, 200, "OK", "o", 0, NULL);
    client_join(&c);

    // Overlapped receive with a buffer that is too small.
    client_start(&c, get, 0);
    Sleep(300);
    memset(&ov.Internal, 0, sizeof ov - sizeof ov.hEvent);
    got = 0xdead;
    rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof(HTTP_REQUEST_V2), &got, &ov);
    n = 0xdead;
    ok = GetOverlappedResult(s.q, &ov, &n, TRUE);
    printf("ovl small: rc=%lu got=0x%lx internal=0x%llx result_ok=%d le=%lu n_gt_fixed=%d id_nonzero=%d\n", rc, got,
           (unsigned long long)ov.Internal, ok, ok ? 0 : GetLastError(), n != 0xdead && n > sizeof(HTTP_REQUEST_V2),
           r->RequestId != 0);
    rc = HttpReceiveHttpRequest(s.q, r->RequestId, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
    printf("ovl small retry: rc=%lu\n", rc);
    if (rc == 0) send_simple(s.q, r->RequestId, 200, "OK", "o", 0, NULL);
    client_join(&c);
    CloseHandle(ov.hEvent);

    // IOCP.
    {
        HANDLE port = CreateIoCompletionPort(s.q, NULL, 0x77, 0);
        ULONG_PTR key = 0;
        LPOVERLAPPED po = NULL;
        printf("iocp associate: ok=%d\n", port != NULL);
        memset(&ov, 0, sizeof ov);
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, NULL, &ov);
        client_start(&c, get, 100);
        n = 0;
        ok = GetQueuedCompletionStatus(port, &n, &key, &po, 5000);
        printf("iocp pending: rc=%lu gqcs=%d key=0x%llx ov_match=%d n_gt_fixed=%d\n", rc, ok,
               (unsigned long long)key, po == &ov, n > sizeof(HTTP_REQUEST_V2));
        memset(&ov, 0, sizeof ov);
        rc = send_simple(s.q, r->RequestId, 200, "OK", "iocp", 0, NULL);
        printf("iocp sync-send: rc=%lu\n", rc);
        client_join(&c);
        ok = GetQueuedCompletionStatus(port, &n, &key, &po, 200);
        printf("iocp after-sync-send: packet=%d\n", ok);

        // Overlapped send through the port.
        memset(&ov, 0, sizeof ov);
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, NULL, &ov);
        client_start(&c, get, 100);
        ok = GetQueuedCompletionStatus(port, &n, &key, &po, 5000);
        {
            HTTP_RESPONSE_V2 resp;
            HTTP_DATA_CHUNK chunk;
            OVERLAPPED sov;
            ULONG sent = 0xdead;
            memset(&resp, 0, sizeof resp);
            memset(&sov, 0, sizeof sov);
            resp.Version.MajorVersion = 1;
            resp.Version.MinorVersion = 1;
            resp.StatusCode = 200;
            chunk.DataChunkType = HttpDataChunkFromMemory;
            chunk.FromMemory.pBuffer = "ovsend";
            chunk.FromMemory.BufferLength = 6;
            resp.EntityChunkCount = 1;
            resp.pEntityChunks = &chunk;
            rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, &sent, NULL, 0, &sov, NULL);
            n = 0xdead;
            ok = GetQueuedCompletionStatus(port, &n, &key, &po, 5000);
            printf("iocp ovl-send: rc=%lu sent=0x%lx gqcs=%d n=%lu ov_match=%d\n", rc, sent, ok, n, po == &sov);
        }
        client_join(&c);
        print_wire("iocp ovl-send", &c);

        // Immediate completion still posts a packet?
        client_start(&c, get, 0);
        Sleep(300);
        memset(&ov, 0, sizeof ov);
        got = 0xdead;
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, &ov);
        ok = GetQueuedCompletionStatus(port, &n, &key, &po, 1000);
        printf("iocp immediate: rc=%lu packet=%d\n", rc, ok);
        send_simple(s.q, r->RequestId, 200, "OK", "o", 0, NULL);
        client_join(&c);

        // CancelIoEx on a pending receive.
        memset(&ov, 0, sizeof ov);
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, NULL, &ov);
        ok = CancelIoEx(s.q, &ov);
        n = 0xdead;
        {
            BOOL q_ok = GetQueuedCompletionStatus(port, &n, &key, &po, 2000);
            printf("cancel pending: rc=%lu cancel=%d gqcs=%d le=%lu n=%lu internal=0x%llx\n", rc, ok, q_ok,
                   q_ok ? 0 : GetLastError(), n, (unsigned long long)ov.Internal);
        }
        printf("cancel nothing: ok=%d le=%lu\n", CancelIoEx(s.q, &ov), GetLastError());

        // Shutdown with a receive pending.
        memset(&ov, 0, sizeof ov);
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, NULL, &ov);
        printf("shutdown: pending=%lu rc=%lu", rc, HttpShutdownRequestQueue(s.q));
        n = 0xdead;
        ok = GetQueuedCompletionStatus(port, &n, &key, &po, 2000);
        printf(" gqcs=%d le=%lu internal=0x%llx\n", ok, ok ? 0 : GetLastError(), (unsigned long long)ov.Internal);
        printf("shutdown again: rc=%lu\n", HttpShutdownRequestQueue(s.q));
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
        printf("after-shutdown sync-recv: rc=%lu\n", rc);
        memset(&ov, 0, sizeof ov);
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, NULL, &ov);
        printf("after-shutdown ovl-recv: rc=%lu\n", rc);
        printf("after-shutdown addurl: rc=%lu\n", HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe9/", 0, 0));
        client_start(&c, get, 0);
        client_join(&c);
        print_status("after-shutdown request", &c);
        CloseHandle(port);
    }
    server_close(&s);

    // Sync receive released by HttpShutdownRequestQueue from another thread,
    // and a receive on a closed handle.
    {
        HANDLE q;
        HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
        HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
        HttpCloseRequestQueue(q);
        rc = HttpReceiveHttpRequest(q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
        printf("recv closed-handle: rc=%lu\n", rc);
        rc = send_simple(q, 1, 200, "OK", NULL, 0, NULL);
        printf("send closed-handle: rc=%lu\n", rc);
        rc = HttpReceiveHttpRequest(NULL, HTTP_NULL_ID, 0, (PHTTP_REQUEST)r, sizeof g_buf, &got, NULL);
        printf("recv null-handle: rc=%lu\n", rc);
        HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    }
}

static DWORD WINAPI shutdown_later(LPVOID p) {
    Sleep(300);
    HttpShutdownRequestQueue((HANDLE)p);
    return 0;
}

static DWORD WINAPI close_later(LPVOID p) {
    Sleep(300);
    HttpCloseRequestQueue((HANDLE)p);
    return 0;
}

static void blocking_release(void) {
    HANDLE q, t;
    ULONG rc, got;
    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
    t = CreateThread(NULL, 0, shutdown_later, q, 0, NULL);
    rc = HttpReceiveHttpRequest(q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)g_buf, sizeof g_buf, &got, NULL);
    printf("sync-recv released by shutdown: rc=%lu\n", rc);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    HttpCloseRequestQueue(q);
    HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
    t = CreateThread(NULL, 0, close_later, q, 0, NULL);
    rc = HttpReceiveHttpRequest(q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)g_buf, sizeof g_buf, &got, NULL);
    printf("sync-recv released by close: rc=%lu\n", rc);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
}

// ---------------------------------------------------------------------------
// Follow-ups: refcounts, what a failed send leaves, RST vs held connections
// ---------------------------------------------------------------------------

static void extra(void) {
    Server s;
    Client c;
    HTTP_REQUEST_V2* r = (HTTP_REQUEST_V2*)g_buf;
    HTTP_RESPONSE_V2 resp;
    HTTP_UNKNOWN_HEADER unk;
    HTTP_SERVER_SESSION_ID sid;
    ULONG rc, rc2, got;
    const char* get = "GET /vprobe/x HTTP/1.1\r\nHost: localhost:" PORTS "\r\n\r\n";

    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("refcount init2-term1 session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("refcount init2-term2 session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    HttpInitialize(V2, HTTP_INITIALIZE_CONFIG, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("config-only session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    HttpTerminate(HTTP_INITIALIZE_CONFIG, NULL);
    {
        HANDLE q;
        HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
        rc = HttpCreateServerSession(V2, &sid, 0);
        HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
        printf("session survives terminate: close=%lu\n", HttpCloseServerSession(sid));
        HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
        rc = HttpCreateRequestQueue(V1, NULL, NULL, 0, &q);
        HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
        printf("queue survives terminate: shutdown=%lu\n", HttpShutdownRequestQueue(q));
        HttpCloseRequestQueue(q);
    }

    server_open(&s);
    HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 1, 0);
    memset(&resp, 0, sizeof resp);
    resp.Version.MajorVersion = 1;
    resp.Version.MinorVersion = 1;

    if (recv_one(&s, get, &c) == 0) {
        resp.StatusCode = 1000;
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        resp.StatusCode = 200;
        rc2 = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        printf("status1000 then-200: rc=%lu then=%lu\n", rc, rc2);
    }
    client_join(&c);
    print_wire("status1000 then-200", &c);

    if (recv_one(&s, get, &c) == 0) {
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0x80000000u, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        rc2 = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        printf("bad-flags then-200: rc=%lu then=%lu\n", rc, rc2);
    }
    client_join(&c);
    print_wire("bad-flags then-200", &c);

    if (recv_one(&s, get, &c) == 0) {
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0x10, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        rc2 = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        printf("flags0x10 then-200: rc=%lu then=%lu\n", rc, rc2);
    }
    client_join(&c);
    print_wire("flags0x10 then-200", &c);

    if (recv_one(&s, get, &c) == 0) {
        unk.pName = NULL;
        unk.NameLength = 3;
        unk.pRawValue = "v";
        unk.RawValueLength = 1;
        resp.Headers.UnknownHeaderCount = 1;
        resp.Headers.pUnknownHeaders = &unk;
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        resp.Headers.UnknownHeaderCount = 0;
        resp.Headers.pUnknownHeaders = NULL;
        rc2 = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        printf("null-header-name then-200: rc=%lu then=%lu\n", rc, rc2);
    }
    client_join(&c);
    print_wire("null-header-name then-200", &c);

    if (recv_one(&s, get, &c) == 0) {
        unk.pName = "Bad Name";
        unk.NameLength = 8;
        resp.Headers.UnknownHeaderCount = 1;
        resp.Headers.pUnknownHeaders = &unk;
        rc = HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL);
        resp.Headers.UnknownHeaderCount = 0;
        resp.Headers.pUnknownHeaders = NULL;
        printf("space-header-name: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("space-header-name", &c);

    // A request id delivered earlier, received again.
    if (recv_one(&s, get, &c) == 0) {
        HTTP_REQUEST_ID id = r->RequestId;
        rc = HttpReceiveHttpRequest(s.q, id, 0, (PHTTP_REQUEST)g_buf, sizeof g_buf, &got, NULL);
        printf("re-receive delivered: rc=%lu\n", rc);
    }
    client_join(&c);
    print_wire("re-receive delivered", &c);

    // CancelHttpRequest on a received request.
    if (recv_one(&s, get, &c) == 0) {
        rc = HttpCancelHttpRequest(s.q, r->RequestId, NULL);
        printf("cancel-request: rc=%lu then-send=%lu\n", rc,
               HttpSendHttpResponse(s.q, r->RequestId, 0, (PHTTP_RESPONSE)&resp, NULL, NULL, NULL, 0, NULL, NULL));
    }
    client_join(&c);
    print_wire("cancel-request", &c);

    // IOCP with a buffer that is too small: is a packet posted?
    {
        HANDLE port = CreateIoCompletionPort(s.q, NULL, 0x55, 0);
        OVERLAPPED ov;
        DWORD n = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED po = NULL;
        BOOL ok;
        client_start(&c, get, 0);
        Sleep(300);
        memset(&ov, 0, sizeof ov);
        got = 0xdead;
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)g_buf, sizeof(HTTP_REQUEST_V2), &got, &ov);
        ok = GetQueuedCompletionStatus(port, &n, &key, &po, 1000);
        printf("iocp small: rc=%lu packet=%d", rc, ok || po != NULL);
        printf(" le=%lu n_eq_got=%d\n", ok ? 0 : GetLastError(), n == got);
        rc = HttpReceiveHttpRequest(s.q, r->RequestId, 0, (PHTTP_REQUEST)g_buf, sizeof g_buf, &got, NULL);
        if (rc == 0) send_simple(s.q, r->RequestId, 200, "OK", NULL, 0, NULL);
        client_join(&c);

        // Pending overlapped send completion with BytesSent NULL and IOCP skip-on-success.
        SetFileCompletionNotificationModes(s.q, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
        client_start(&c, get, 0);
        Sleep(300);
        memset(&ov, 0, sizeof ov);
        rc = HttpReceiveHttpRequest(s.q, HTTP_NULL_ID, 0, (PHTTP_REQUEST)g_buf, sizeof g_buf, &got, &ov);
        ok = GetQueuedCompletionStatus(port, &n, &key, &po, 500);
        printf("iocp skip-on-success immediate: rc=%lu packet=%d\n", rc, ok);
        send_simple(s.q, r->RequestId, 200, "OK", NULL, 0, NULL);
        client_join(&c);
        CloseHandle(port);
    }

    // Inactive group, with a longer client wait.
    {
        HTTP_STATE_INFO st;
        st.Flags.Present = 1;
        st.State = HttpEnabledStateInactive;
        HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st);
        client_start(&c, get, 0);
        c.keep_open_ms = 4000;
        client_join(&c);
        print_status("inactive long", &c);
        st.State = HttpEnabledStateActive;
        HttpSetUrlGroupProperty(s.gid, HttpServerStateProperty, &st, sizeof st);
    }
    // Close the group: its URLs become free for another group.
    {
        Server s2;
        server_open(&s2);
        printf("other-group before close: rc=%lu\n", HttpAddUrlToUrlGroup(s2.gid, BASE L"/vprobe/", 2, 0));
        HttpCloseUrlGroup(s.gid);
        printf("other-group after close: rc=%lu\n", HttpAddUrlToUrlGroup(s2.gid, BASE L"/vprobe/", 2, 0));
        client_start(&c, get, 0);
        rc = recv_timed(s2.q, 0, &got);
        printf("routed to other: rc=%lu ctx=%llu\n", rc, rc == 0 ? (unsigned long long)r->UrlContext : 0ull);
        if (rc == 0) send_simple(s2.q, r->RequestId, 200, "OK", NULL, 0, NULL);
        client_join(&c);
        // Close the queue bound to the group.
        HttpCloseRequestQueue(s2.q);
        client_start(&c, get, 0);
        client_join(&c);
        print_status("queue closed", &c);
        HttpCloseUrlGroup(s2.gid);
        HttpCloseServerSession(s2.sid);
        HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    }
    HttpCloseServerSession(s.sid);
    HttpCloseRequestQueue(s.q);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);

    // Shutdown queue: what does a new request see?
    server_open(&s);
    HttpAddUrlToUrlGroup(s.gid, BASE L"/vprobe/", 1, 0);
    HttpShutdownRequestQueue(s.q);
    client_start(&c, get, 0);
    c.keep_open_ms = 4000;
    client_join(&c);
    print_status("shutdown long", &c);
    server_close(&s);

}

// Run in a fresh process: HttpInitialize / HttpTerminate counting.
static void refcount(void) {
    HTTP_SERVER_SESSION_ID sid;
    HANDLE q;
    ULONG rc;
    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("fresh init2-term1 session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("fresh init2-term2 session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    q = (HANDLE)0x3333;
    rc = HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
    printf("fresh init2-term2 queue: rc=%lu\n", rc);
    if (rc == 0) HttpCloseRequestQueue(q);
    printf("fresh terminate-extra: rc=%lu\n", HttpTerminate(HTTP_INITIALIZE_SERVER, NULL));
    HttpInitialize(V2, HTTP_INITIALIZE_CONFIG, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("fresh config-only session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    rc = HttpCreateRequestQueue(V2, NULL, NULL, 0, &q);
    printf("fresh config-only queue: rc=%lu\n", rc);
    if (rc == 0) HttpCloseRequestQueue(q);
    HttpTerminate(HTTP_INITIALIZE_CONFIG, NULL);
    HttpInitialize(V2, HTTP_INITIALIZE_SERVER | HTTP_INITIALIZE_CONFIG, NULL);
    HttpTerminate(HTTP_INITIALIZE_CONFIG, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("fresh both-then-term-config session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    HttpInitialize(V2, 0, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    printf("fresh flags0 session: rc=%lu\n", rc);
    if (rc == 0) HttpCloseServerSession(sid);
    HttpInitialize(V2, HTTP_INITIALIZE_SERVER, NULL);
    rc = HttpCreateServerSession(V2, &sid, 0);
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    printf("fresh session-after-terminate: create=%lu close=%lu\n", rc, HttpCloseServerSession(sid));
}

int main(int argc, char** argv) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "refcount") == 0) {
        refcount();
        return 0;
    }
    layouts();
    init_errors();
    properties();
    urls();
    receive_sync();
    responses();
    overlapped_io();
    blocking_release();
    extra();
    return 0;
}
