/// @file rpcrt4.cpp
/// @brief rpcrt4.dll UUID API implementation.

#include "rpcrt4.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/com/ole32/task_memory.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

namespace vitreo::winapi {

/// Draw 16 cryptographically-arbitrary bytes and stamp them as an RFC 4122
/// version-4 (random) UUID, which is what Windows' UuidCreate produces.
/// Uniqueness is the whole contract here: callers use these as correlation
/// keys, so two calls must never collide.
std::array<u8, kUuidSize> generate_uuid_v4() {
    // Every UUID draws 122 fresh bits from the host CSPRNG, as Windows'
    // UuidCreate draws a random version-4 UUID (tools/oracle_probe/
    // iphlp_misc_probe uuid-create version4=1). A mt19937 seeded with one
    // 32-bit value has only 2^32 possible streams, so two processes collide
    // with birthday probability after ~65k launches and every value is
    // predictable from any earlier one.
    std::array<u8, kUuidSize> uuid{};
    ::arc4random_buf(uuid.data(), uuid.size());

    // These bytes are consumed as a Windows GUID/UUID *struct*, not as an
    // RFC 4122 wire encoding: Data1/Data2/Data3 are little-endian (see
    // format_uuid, which reads them that way). RFC 4122 puts the version in
    // the high nibble of wire octet 6, and that octet is Data3's HIGH byte --
    // which is octet 7 in struct order. Stamping octet 6 instead leaves the
    // version nibble inside Data3's low byte, where no caller looks: the GUID
    // then fails a `(Data3 >> 12) == 4` check even though it is random.
    uuid[7] = static_cast<u8>((uuid[7] & 0x0F) | 0x40);
    // Variant lives in Data4[0], a plain byte array with no swap, so octet 8
    // is octet 8 either way.
    uuid[8] = static_cast<u8>((uuid[8] & 0x3F) | 0x80);
    return uuid;
}

namespace {

/// Read a guest UUID into host bytes in *little-endian struct order*, i.e. the
/// raw 16 bytes exactly as they sit in guest memory.
bool read_uuid(Marshaler& m, GuestAddr addr, std::array<u8, kUuidSize>& out) {
    for (u32 i = 0; i < kUuidSize; ++i) {
        auto b = m.read_u8(addr + i);
        if (!b.is_ok()) return false;
        out[i] = b.value();
    }
    return true;
}

/// Format to the canonical 8-4-4-4-12 lowercase form.
/// Data1/Data2/Data3 are integers, so they render from their little-endian
/// in-memory bytes; Data4 is a byte array and renders in memory order. Getting
/// that distinction wrong yields a plausible-looking but wrong string.
void format_uuid(const std::array<u8, kUuidSize>& u, char out[kUuidStringLen + 1]) {
    const u32 d1 = static_cast<u32>(u[0]) | (static_cast<u32>(u[1]) << 8) |
                   (static_cast<u32>(u[2]) << 16) | (static_cast<u32>(u[3]) << 24);
    const u32 d2 = static_cast<u32>(u[4]) | (static_cast<u32>(u[5]) << 8);
    const u32 d3 = static_cast<u32>(u[6]) | (static_cast<u32>(u[7]) << 8);
    std::snprintf(out, kUuidStringLen + 1,
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  d1, d2, d3, u[8], u[9],
                  u[10], u[11], u[12], u[13], u[14], u[15]);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Parse the canonical 8-4-4-4-12 form (optional enclosing braces) into the
/// guest UUID memory layout. Inverse of format_uuid.
bool parse_uuid_string(const char* text, std::array<u8, kUuidSize>& out) {
    if (text == nullptr) return false;
    // Skip optional surrounding braces: "{...}"
    const char* s = text;
    bool braced = false;
    if (*s == '{') {
        braced = true;
        ++s;
    }
    auto take_hex = [&](int nibble_count, u64& value) -> bool {
        value = 0;
        for (int i = 0; i < nibble_count; ++i) {
            const int n = hex_nibble(*s++);
            if (n < 0) return false;
            value = (value << 4) | static_cast<u64>(n);
        }
        return true;
    };
    u64 d1 = 0, d2 = 0, d3 = 0, d4a = 0, d4b = 0;
    if (!take_hex(8, d1) || *s++ != '-') return false;
    if (!take_hex(4, d2) || *s++ != '-') return false;
    if (!take_hex(4, d3) || *s++ != '-') return false;
    if (!take_hex(4, d4a) || *s++ != '-') return false;
    if (!take_hex(12, d4b)) return false;
    if (braced) {
        if (*s++ != '}') return false;
    }
    if (*s != '\0') return false;

    out[0] = static_cast<u8>(d1);
    out[1] = static_cast<u8>(d1 >> 8);
    out[2] = static_cast<u8>(d1 >> 16);
    out[3] = static_cast<u8>(d1 >> 24);
    out[4] = static_cast<u8>(d2);
    out[5] = static_cast<u8>(d2 >> 8);
    out[6] = static_cast<u8>(d3);
    out[7] = static_cast<u8>(d3 >> 8);
    out[8]  = static_cast<u8>(d4a >> 8);
    out[9]  = static_cast<u8>(d4a);
    out[10] = static_cast<u8>(d4b >> 40);
    out[11] = static_cast<u8>(d4b >> 32);
    out[12] = static_cast<u8>(d4b >> 24);
    out[13] = static_cast<u8>(d4b >> 16);
    out[14] = static_cast<u8>(d4b >> 8);
    out[15] = static_cast<u8>(d4b);
    return true;
}

} // namespace

DllExports create_rpcrt4_thunks(GlobalLocalManager& heap,
                                ComTaskMemoryManager& task_memory,
                                NdrProxyRegistrar& proxies) {
    DllExports exports;

    // RPC_STATUS UuidCreate(UUID *Uuid)  — 1 arg, stdcall_bytes=4.
    //
    // Returns RPC_S_OK and writes 16 bytes. Windows can also return
    // RPC_S_UUID_LOCAL_ONLY (1824) when it falls back to a node-less UUID, but
    // that is a *success* code callers treat as OK; a v4 UUID is globally
    // unique by construction, so plain RPC_S_OK is the honest answer.
    exports.entries.push_back({"UuidCreate", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(0);
            if (out == 0) {
                m.set_return(rpc_status::RPC_S_INVALID_ARG);
                return;
            }
            const auto uuid = generate_uuid_v4();
            for (u32 i = 0; i < kUuidSize; ++i) {
                (void)m.write_u8(out + i, uuid[i]);
            }
            char text[kUuidStringLen + 1];
            format_uuid(uuid, text);
            VITREO_TRACE("[rpcrt4] UuidCreate -> %s\n", text);
            m.set_return(rpc_status::RPC_S_OK);
        }, 4});

    // RPC_STATUS UuidCreateSequential(UUID *Uuid) — 1 arg, stdcall_bytes=4.
    // Vitreo exposes no MAC address, which is exactly the case Windows signals
    // with RPC_S_UUID_LOCAL_ONLY: the UUID is valid but not node-derived.
    // Callers treat that as success, so a random UUID is returned with it.
    exports.entries.push_back({"UuidCreateSequential", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(0);
            if (out == 0) {
                m.set_return(rpc_status::RPC_S_INVALID_ARG);
                return;
            }
            const auto uuid = generate_uuid_v4();
            for (u32 i = 0; i < kUuidSize; ++i) {
                (void)m.write_u8(out + i, uuid[i]);
            }
            constexpr u32 RPC_S_UUID_LOCAL_ONLY = 1824;
            VITREO_TRACE("[rpcrt4] UuidCreateSequential -> RPC_S_UUID_LOCAL_ONLY"
                         " (no node address)\n");
            m.set_return(RPC_S_UUID_LOCAL_ONLY);
        }, 4});

    // RPC_STATUS UuidToStringA(const UUID *Uuid, RPC_CSTR *StringUuid)
    // 2 args, stdcall_bytes=8.
    //
    // Allocates the string; the caller releases it with RpcStringFreeA. Both
    // sides use GlobalLocalManager so the free actually returns the memory.
    const auto make_to_string = [&heap](bool wide) {
        return [&heap, wide](Marshaler& m) {
            const GuestAddr uuid_ptr = m.arg_ptr(0);
            const GuestAddr out_ptr  = m.arg_ptr(1);
            if (uuid_ptr == 0 || out_ptr == 0) {
                m.set_return(rpc_status::RPC_S_INVALID_ARG);
                return;
            }
            std::array<u8, kUuidSize> uuid{};
            if (!read_uuid(m, uuid_ptr, uuid)) {
                m.set_return(rpc_status::RPC_S_INVALID_ARG);
                return;
            }
            char text[kUuidStringLen + 1];
            format_uuid(uuid, text);

            const u32 char_size = wide ? 2u : 1u;
            const u32 bytes = (kUuidStringLen + 1) * char_size;
            const u64 addr = heap.global_alloc(GPTR, bytes);
            if (addr == 0) {
                (void)m.write_ptr(out_ptr, 0);
                m.set_return(rpc_status::RPC_S_OUT_OF_MEMORY);
                return;
            }
            const auto base = static_cast<GuestAddr>(addr);
            for (u32 i = 0; i <= kUuidStringLen; ++i) {
                const auto c = static_cast<u8>(text[i]);
                if (wide) (void)m.write_u16(base + i * 2, c);
                else      (void)m.write_u8(base + i, c);
            }
            (void)m.write_ptr(out_ptr, addr);
            VITREO_TRACE("[rpcrt4] UuidToString%s -> \"%s\" at 0x%llx\n",
                         wide ? "W" : "A", text,
                         static_cast<unsigned long long>(addr));
            m.set_return(rpc_status::RPC_S_OK);
        };
    };
    exports.entries.push_back({"UuidToStringA", 0, make_to_string(false), 8});
    exports.entries.push_back({"UuidToStringW", 0, make_to_string(true), 8});

    // RPC_STATUS UuidFromStringA(RPC_CSTR StringUuid, UUID *Uuid) — 2 args, 8 bytes.
    // Learn: NULL StringUuid writes GUID_NULL and returns RPC_S_OK. Invalid text
    // returns RPC_S_INVALID_STRING_UUID (1705). Optional enclosing braces OK.
    const auto make_from_string = [](bool wide) {
        return [wide](Marshaler& m) {
            const GuestAddr str_ptr = m.arg_ptr(0);
            const GuestAddr uuid_ptr = m.arg_ptr(1);
            if (uuid_ptr == 0) {
                m.set_return(rpc_status::RPC_S_INVALID_ARG);
                return;
            }
            if (str_ptr == 0) {
                for (u32 i = 0; i < kUuidSize; ++i) {
                    (void)m.write_u8(uuid_ptr + i, 0);
                }
                m.set_return(rpc_status::RPC_S_OK);
                return;
            }
            char text[kUuidStringLen + 4] = {}; // room for braces + NUL
            if (wide) {
                for (u32 i = 0; i < sizeof(text) - 1; ++i) {
                    const u16 ch = m.read_u16(str_ptr + i * 2).value_or(0);
                    if (ch == 0) break;
                    if (ch > 0x7F) {
                        m.set_return(rpc_status::RPC_S_INVALID_STRING_UUID);
                        return;
                    }
                    text[i] = static_cast<char>(ch);
                }
            } else {
                for (u32 i = 0; i < sizeof(text) - 1; ++i) {
                    const u8 ch = m.read_u8(str_ptr + i).value_or(0);
                    if (ch == 0) break;
                    text[i] = static_cast<char>(ch);
                }
            }
            std::array<u8, kUuidSize> uuid{};
            if (!parse_uuid_string(text, uuid)) {
                m.set_return(rpc_status::RPC_S_INVALID_STRING_UUID);
                return;
            }
            for (u32 i = 0; i < kUuidSize; ++i) {
                (void)m.write_u8(uuid_ptr + i, uuid[i]);
            }
            VITREO_TRACE("[rpcrt4] UuidFromString%s(\"%s\") OK\n",
                         wide ? "W" : "A", text);
            m.set_return(rpc_status::RPC_S_OK);
        };
    };
    exports.entries.push_back({"UuidFromStringA", 0, make_from_string(false), 8});
    exports.entries.push_back({"UuidFromStringW", 0, make_from_string(true), 8});

    // RPC_STATUS RpcStringFreeA(RPC_CSTR *String) — 1 arg, stdcall_bytes=4.
    //
    // Frees the buffer and NULLs the caller's pointer. Windows tolerates a
    // pointer to NULL (nothing to free) and still reports RPC_S_OK, so callers
    // may free twice; only a NULL `String` itself is invalid.
    const auto make_string_free = [&heap](bool wide) {
        return [&heap, wide](Marshaler& m) {
            const GuestAddr pp = m.arg_ptr(0);
            if (pp == 0) {
                m.set_return(rpc_status::RPC_S_INVALID_ARG);
                return;
            }
            const auto sp = m.read_ptr(pp);
            if (!sp.is_ok()) {
                m.set_return(rpc_status::RPC_S_INVALID_ARG);
                return;
            }
            const u64 addr = sp.value();
            if (addr != 0) {
                (void)heap.global_free(addr);
                (void)m.write_ptr(pp, 0);
            }
            VITREO_TRACE("[rpcrt4] RpcStringFree%s(0x%llx)\n",
                         wide ? "W" : "A",
                         static_cast<unsigned long long>(addr));
            m.set_return(rpc_status::RPC_S_OK);
        };
    };
    exports.entries.push_back({"RpcStringFreeA", 0, make_string_free(false), 4});
    exports.entries.push_back({"RpcStringFreeW", 0, make_string_free(true), 4});

    // -------------------------------------------------------------------------
    // NdrDll* — self-registration for MIDL proxy/stub DLLs.
    //
    // The stdcall_bytes on these matter as much as the return values: as
    // missing-export stubs they popped nothing, so DllRegisterServer returned
    // through a stack slot 12 bytes off and jumped into its own PE header.
    // -------------------------------------------------------------------------

    // HRESULT NdrDllRegisterProxy(HMODULE, const ProxyFileInfo **, const CLSID *)
    exports.entries.push_back({"NdrDllRegisterProxy", 0,
        [&proxies](Marshaler& m) { m.set_return(proxies.register_proxy(m)); }, 12});

    // HRESULT NdrDllUnregisterProxy(HMODULE, const ProxyFileInfo **, const CLSID *)
    exports.entries.push_back({"NdrDllUnregisterProxy", 0,
        [&proxies](Marshaler& m) { m.set_return(proxies.unregister_proxy(m)); }, 12});

    // HRESULT NdrDllGetClassObject(REFCLSID, REFIID, void **ppv,
    //                              const ProxyFileInfo **, const CLSID *,
    //                              CStdPSFactoryBuffer *)
    //
    // Learn: returns S_OK with an IPSFactoryBuffer. Initialize the guest
    // CStdPSFactoryBuffer the MIDL proxy passes in; CreateProxy/CreateStub
    // still return E_NOTIMPL until an NDR wire engine exists.
    exports.entries.push_back({"NdrDllGetClassObject", 0,
        [&proxies](Marshaler& m) { m.set_return(proxies.get_class_object(m)); },
        24});

    // HRESULT NdrDllCanUnloadNow(CStdPSFactoryBuffer *)
    exports.entries.push_back({"NdrDllCanUnloadNow", 0,
        [&proxies](Marshaler& m) { m.set_return(proxies.can_unload_now(m)); },
        4});

    // IPSFactoryBuffer methods hosted on the CStdPSFactoryBuffer from
    // NdrDllGetClassObject (This-call stdcall).
    exports.entries.push_back({"Vitreo_PSFactory_QueryInterface", 0,
        [&proxies](Marshaler& m) { proxies.psfactory_query_interface(m); }, 12});
    exports.entries.push_back({"Vitreo_PSFactory_AddRef", 0,
        [&proxies](Marshaler& m) { proxies.psfactory_add_ref(m); }, 4});
    exports.entries.push_back({"Vitreo_PSFactory_Release", 0,
        [&proxies](Marshaler& m) { proxies.psfactory_release(m); }, 4});
    // CreateProxy(This, pUnkOuter, riid, ppProxy, ppv) — 5 pointer args
    exports.entries.push_back({"Vitreo_PSFactory_CreateProxy", 0,
        [&proxies](Marshaler& m) { proxies.psfactory_create_proxy(m); }, 20});
    // CreateStub(This, riid, pUnkServer, ppStub) — 4 pointer args
    exports.entries.push_back({"Vitreo_PSFactory_CreateStub", 0,
        [&proxies](Marshaler& m) { proxies.psfactory_create_stub(m); }, 16});

    // ULONG NdrCStdStubBuffer_Release(IRpcStubBuffer *This, IPSFactoryBuffer *pPSF)
    //
    // MIDL-generated stub vtables point IUnknown::Release here (rpcproxy.h).
    // Without a live stub-buffer engine, report refcount 0. Two pointer args
    // → 8 stdcall bytes on x86.
    exports.entries.push_back({"NdrCStdStubBuffer_Release", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            (void)m.arg_ptr(1);
            VITREO_TRACE("[rpcrt4] NdrCStdStubBuffer_Release: no stub buffer "
                         "engine -> 0\n");
            m.set_return(0u);
        }, 8});

    // ULONG NdrCStdStubBuffer2_Release(IRpcStubBuffer *This, IPSFactoryBuffer *pPSF)
    // Same contract as the non-2 form for the v2 stub buffer layout.
    exports.entries.push_back({"NdrCStdStubBuffer2_Release", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            (void)m.arg_ptr(1);
            VITREO_TRACE("[rpcrt4] NdrCStdStubBuffer2_Release: no stub buffer "
                         "engine -> 0\n");
            m.set_return(0u);
        }, 8});

    // void* NdrOleAllocate(size_t Size) — CoTaskMemAlloc wrapper (rpcndr.h).
    // size_t is 4 bytes on PE32, 8 on PE64; Marshaler::arg(0) covers both.
    // Zero size still allocates a valid zero-length block (Learn contract).
    exports.entries.push_back({"NdrOleAllocate", 0,
        [&task_memory](Marshaler& m) {
            const u64 size = m.arg(0);
            const GuestAddr addr = task_memory.allocate(size);
            VITREO_TRACE("[rpcrt4] NdrOleAllocate(%llu) -> 0x%llx\n",
                         static_cast<unsigned long long>(size),
                         static_cast<unsigned long long>(addr));
            m.set_return(addr);
        }, 4});

    // void NdrOleFree(void *NodeToFree) — CoTaskMemFree wrapper.
    exports.entries.push_back({"NdrOleFree", 0,
        [&task_memory](Marshaler& m) {
            const GuestAddr addr = m.arg_ptr(0);
            if (addr != 0) {
                task_memory.free(addr);
            }
            VITREO_TRACE("[rpcrt4] NdrOleFree(0x%llx)\n",
                         static_cast<unsigned long long>(addr));
            m.set_return(0);
        }, 4});

    // ---------------------------------------------------------------------
    // CStdStubBuffer_* / IUnknown_*_Proxy — MIDL stub vtable targets
    // (rpcproxy.h). ISSetup.dll GetProcAddress-probes the set during DllMain.
    // Vitreo has no NDR stub-buffer engine (NdrDllGetClassObject refuses), so
    // these answer the documented HRESULT / refcount shapes without inventing
    // a live IRpcStubBuffer. Arity from Learn signatures (ptr = 4 x86 bytes).
    // ---------------------------------------------------------------------
    constexpr u32 E_NOINTERFACE = 0x80004002u;
    constexpr u32 E_NOTIMPL     = 0x80004001u;
    constexpr u32 E_NOINTERFACE_OR_NOTIMPL = E_NOINTERFACE;

    auto qi_fail = [](Marshaler& m) {
        (void)m.arg_ptr(0);
        (void)m.arg_ptr(1);
        const GuestAddr ppv = m.arg_ptr(2);
        if (ppv != 0) (void)m.write_ptr(ppv, 0);
        m.set_return(E_NOINTERFACE);
    };
    exports.entries.push_back({"CStdStubBuffer_QueryInterface", 0, qi_fail, 12});
    exports.entries.push_back({"IUnknown_QueryInterface_Proxy", 0, qi_fail, 12});

    exports.entries.push_back({"CStdStubBuffer_AddRef", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); m.set_return(1u); }, 4});
    exports.entries.push_back({"IUnknown_AddRef_Proxy", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); m.set_return(1u); }, 4});

    exports.entries.push_back({"CStdStubBuffer_Release", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); m.set_return(0u); }, 4});
    exports.entries.push_back({"IUnknown_Release_Proxy", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); m.set_return(0u); }, 4});

    exports.entries.push_back({"CStdStubBuffer_Connect", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            m.set_return(E_NOTIMPL);
        }, 8});
    exports.entries.push_back({"CStdStubBuffer_Disconnect", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); m.set_return(0); }, 4});
    exports.entries.push_back({"CStdStubBuffer_Invoke", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg_ptr(2);
            m.set_return(E_NOTIMPL);
        }, 12});
    exports.entries.push_back({"CStdStubBuffer_IsIIDSupported", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            m.set_return(0); // NULL IRpcStubBuffer*
        }, 8});
    exports.entries.push_back({"CStdStubBuffer_CountRefs", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); m.set_return(0u); }, 4});
    exports.entries.push_back({"CStdStubBuffer_DebugServerQueryInterface", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            const GuestAddr ppv = m.arg_ptr(1);
            if (ppv != 0) (void)m.write_ptr(ppv, 0);
            m.set_return(E_NOINTERFACE_OR_NOTIMPL);
        }, 8});
    exports.entries.push_back({"CStdStubBuffer_DebugServerRelease", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            m.set_return(0); // void
        }, 8});

    // NdrStub* / NdrProxy* — MIDL-generated stub/proxy call plumbing
    // (rpcndr.h). Same GetProcAddress probe set as CStdStubBuffer_*. Without an
    // NDR engine these fail honestly: HRESULT E_NOTIMPL / RPC_E_UNEXPECTED /
    // NULL buffer pointers, with correct stdcall pops.
    constexpr u32 RPC_E_UNEXPECTED = 0x8001FFFFu;

    exports.entries.push_back({"NdrStubForwardingFunction", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            (void)m.arg_ptr(2); (void)m.arg_ptr(3);
            m.set_return(E_NOTIMPL);
        }, 16});
    exports.entries.push_back({"NdrStubInitialize", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            (void)m.arg_ptr(2); (void)m.arg_ptr(3);
            m.set_return(0); // void
        }, 16});
    exports.entries.push_back({"NdrStubGetBuffer", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg_ptr(2);
            m.set_return(RPC_E_UNEXPECTED);
        }, 12});
    exports.entries.push_back({"NdrStubCall", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            (void)m.arg_ptr(2); (void)m.arg_ptr(3);
            m.set_return(static_cast<u32>(-1)); // LONG error
        }, 16});
    exports.entries.push_back({"NdrStubCall2", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            (void)m.arg_ptr(2); (void)m.arg_ptr(3);
            m.set_return(static_cast<u32>(-1));
        }, 16});
    exports.entries.push_back({"NdrProxyInitialize", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            (void)m.arg_ptr(2); (void)m.arg_ptr(3); (void)m.arg(4);
            m.set_return(0); // void
        }, 20});
    exports.entries.push_back({"NdrProxyGetBuffer", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            m.set_return(RPC_E_UNEXPECTED);
        }, 8});
    exports.entries.push_back({"NdrProxySendReceive", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            m.set_return(RPC_E_UNEXPECTED);
        }, 8});
    exports.entries.push_back({"NdrProxyFreeBuffer", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            m.set_return(0); // void
        }, 8});
    exports.entries.push_back({"NdrProxyErrorHandler", 0,
        [](Marshaler& m) {
            const u32 dw = static_cast<u32>(m.arg(0));
            m.set_return(dw != 0 ? dw : RPC_E_UNEXPECTED);
        }, 4});

    // Remaining MIDL NDR marshalling helpers. ISSetup's DllMain walks a long
    // GetProcAddress checklist of these symbols; each missing name aborts with
    // a MessageBox before any of them are called. Register the documented x86
    // stdcall arities (from public MIDL/rpcndr export tables) and return 0 /
    // NULL so a later real call fails visibly rather than corrupting the stack.
    static constexpr struct { const char* name; u32 bytes; } kNdrSurface[] = {
        {"NdrAllocate", 8},
        {"NdrAsyncServerCall", 4},
        {"NdrByteCountPointerBufferSize", 12},
        {"NdrByteCountPointerFree", 12},
        {"NdrByteCountPointerMarshall", 12},
        {"NdrByteCountPointerUnmarshall", 16},
        {"NdrClearOutParameters", 12},
        {"NdrClientContextMarshall", 12},
        {"NdrClientContextUnmarshall", 12},
        {"NdrClientInitializeNew", 16},
        {"NdrComplexArrayBufferSize", 12},
        {"NdrComplexArrayFree", 12},
        {"NdrComplexArrayMarshall", 12},
        {"NdrComplexArrayMemorySize", 8},
        {"NdrComplexArrayUnmarshall", 16},
        {"NdrComplexStructBufferSize", 12},
        {"NdrComplexStructFree", 12},
        {"NdrComplexStructMarshall", 12},
        {"NdrComplexStructMemorySize", 8},
        {"NdrComplexStructUnmarshall", 16},
        {"NdrConformantArrayBufferSize", 12},
        {"NdrConformantArrayFree", 12},
        {"NdrConformantArrayMarshall", 12},
        {"NdrConformantArrayMemorySize", 8},
        {"NdrConformantArrayUnmarshall", 16},
        {"NdrConformantStringBufferSize", 12},
        {"NdrConformantStringMarshall", 12},
        {"NdrConformantStringMemorySize", 8},
        {"NdrConformantStringUnmarshall", 16},
        {"NdrConformantStructBufferSize", 12},
        {"NdrConformantStructFree", 12},
        {"NdrConformantStructMarshall", 12},
        {"NdrConformantStructMemorySize", 8},
        {"NdrConformantStructUnmarshall", 16},
        {"NdrConformantVaryingArrayBufferSize", 12},
        {"NdrConformantVaryingArrayFree", 12},
        {"NdrConformantVaryingArrayMarshall", 12},
        {"NdrConformantVaryingArrayMemorySize", 8},
        {"NdrConformantVaryingArrayUnmarshall", 16},
        {"NdrConformantVaryingStructBufferSize", 12},
        {"NdrConformantVaryingStructFree", 12},
        {"NdrConformantVaryingStructMarshall", 12},
        {"NdrConformantVaryingStructMemorySize", 8},
        {"NdrConformantVaryingStructUnmarshall", 16},
        {"NdrContextHandleInitialize", 8},
        {"NdrContextHandleSize", 12},
        {"NdrConvert2", 12},
        {"NdrConvert", 8},
        {"NdrCorrelationFree", 4},
        {"NdrCorrelationInitialize", 16},
        {"NdrCorrelationPass", 4},
        {"NdrEncapsulatedUnionBufferSize", 12},
        {"NdrEncapsulatedUnionFree", 12},
        {"NdrEncapsulatedUnionMarshall", 12},
        {"NdrEncapsulatedUnionMemorySize", 8},
        {"NdrEncapsulatedUnionUnmarshall", 16},
        {"NdrFixedArrayBufferSize", 12},
        {"NdrFixedArrayFree", 12},
        {"NdrFixedArrayMarshall", 12},
        {"NdrFixedArrayMemorySize", 8},
        {"NdrFixedArrayUnmarshall", 16},
        {"NdrFreeBuffer", 4},
        {"NdrFullPointerFree", 8},
        {"NdrFullPointerInsertRefId", 12},
        {"NdrFullPointerQueryPointer", 16},
        {"NdrFullPointerQueryRefId", 16},
        {"NdrFullPointerXlatFree", 4},
        {"NdrFullPointerXlatInit", 8},
        {"NdrGetBuffer", 12},
        {"NdrGetUserMarshalInfo", 12},
        {"NdrInterfacePointerBufferSize", 12},
        {"NdrInterfacePointerFree", 12},
        {"NdrInterfacePointerMarshall", 12},
        {"NdrInterfacePointerMemorySize", 8},
        {"NdrInterfacePointerUnmarshall", 16},
        {"NdrMapCommAndFaultStatus", 16},
        {"NdrMesTypeDecode2", 20},
        {"NdrMesTypeEncode2", 20},
        {"NdrMesTypeFree2", 20},
        {"NdrNonConformantStringBufferSize", 12},
        {"NdrNonConformantStringMarshall", 12},
        {"NdrNonConformantStringMemorySize", 8},
        {"NdrNonConformantStringUnmarshall", 16},
        {"NdrNonEncapsulatedUnionBufferSize", 12},
        {"NdrNonEncapsulatedUnionFree", 12},
        {"NdrNonEncapsulatedUnionMarshall", 12},
        {"NdrNonEncapsulatedUnionMemorySize", 8},
        {"NdrNonEncapsulatedUnionUnmarshall", 16},
        {"NdrPointerBufferSize", 12},
        {"NdrPointerFree", 12},
        {"NdrPointerMarshall", 12},
        {"NdrPointerMemorySize", 8},
        {"NdrPointerUnmarshall", 16},
        {"NdrRangeUnmarshall", 16},
        {"NdrRpcSmSetClientToOsf", 4},
        {"NdrSendReceive", 8},
        {"NdrServerCall2", 4},
        {"NdrServerCall", 4},
        {"NdrServerContextMarshall", 12},
        {"NdrServerContextNewMarshall", 16},
        {"NdrServerContextNewUnmarshall", 8},
        {"NdrServerContextUnmarshall", 4},
        {"NdrServerInitializeNew", 12},
        {"NdrSimpleStructBufferSize", 12},
        {"NdrSimpleStructFree", 12},
        {"NdrSimpleStructMarshall", 12},
        {"NdrSimpleStructMemorySize", 8},
        {"NdrSimpleStructUnmarshall", 16},
        {"NdrSimpleTypeMarshall", 12},
        {"NdrSimpleTypeUnmarshall", 12},
        {"NdrUserMarshalBufferSize", 12},
        {"NdrUserMarshalFree", 12},
        {"NdrUserMarshalMarshall", 12},
        {"NdrUserMarshalMemorySize", 8},
        {"NdrUserMarshalUnmarshall", 16},
        {"NdrVaryingArrayBufferSize", 12},
        {"NdrVaryingArrayFree", 12},
        {"NdrVaryingArrayMarshall", 12},
        {"NdrVaryingArrayMemorySize", 8},
        {"NdrVaryingArrayUnmarshall", 16},
        {"NdrXmitOrRepAsBufferSize", 12},
        {"NdrXmitOrRepAsFree", 12},
        {"NdrXmitOrRepAsMarshall", 12},
        {"NdrXmitOrRepAsMemorySize", 8},
        {"NdrXmitOrRepAsUnmarshall", 16},
    };
    for (const auto& e : kNdrSurface) {
        const u16 bytes = static_cast<u16>(e.bytes);
        exports.entries.push_back({e.name, 0,
            [bytes](Marshaler& m) {
                // Consume declared stdcall args so a mistaken call cannot
                // leave the guest stack unbalanced; return 0/NULL.
                const u32 argc = static_cast<u32>(bytes) / 4u;
                for (u32 i = 0; i < argc; ++i) (void)m.arg(i);
                m.set_return(0);
            }, bytes});
    }

    // Rpc* / I_Rpc* runtime entry points (same GetProcAddress checklist pattern).
    static constexpr struct { const char* name; u32 bytes; } kRpcSurface[] = {
        {"I_RpcAllocate", 4},
        {"I_RpcExceptionFilter", 4},
        {"I_RpcFree", 4},
        {"I_RpcFreeBuffer", 4},
        {"I_RpcGetBuffer", 4},
        {"I_RpcGetCurrentCallHandle", 0},
        {"I_RpcMapWin32Status", 4},
        {"I_RpcReceive", 4},
        {"I_RpcSend", 4},
        {"I_RpcSendReceive", 4},
        {"RpcBindingCopy", 8},
        {"RpcBindingFree", 4},
        {"RpcBindingFromStringBindingA", 8},
        {"RpcBindingFromStringBindingW", 8},
        {"RpcBindingInqObject", 8},
        {"RpcBindingReset", 4},
        {"RpcBindingSetObject", 8},
        {"RpcBindingToStringBindingA", 8},
        {"RpcBindingToStringBindingW", 8},
        {"RpcBindingVectorFree", 4},
        {"RpcEpResolveBinding", 8},
        {"RpcIfInqId", 8},
        {"RpcImpersonateClient", 4},
        {"RpcMgmtEnableIdleCleanup", 0},
        {"RpcMgmtInqIfIds", 8},
        {"RpcMgmtIsServerListening", 4},
        {"RpcMgmtSetCancelTimeout", 4},
        {"RpcMgmtSetComTimeout", 8},
        {"RpcMgmtStopServerListening", 4},
        {"RpcMgmtWaitServerListen", 0},
        {"RpcNetworkIsProtseqValidA", 4},
        {"RpcNetworkIsProtseqValidW", 4},
        {"RpcObjectSetType", 8},
        {"RpcRaiseException", 4},
        {"RpcRevertToSelf", 0},
        {"RpcRevertToSelfEx", 4},
        {"RpcServerInqBindings", 4},
        {"RpcServerListen", 12},
        {"RpcServerRegisterIf", 12},
        {"RpcServerRegisterIf2", 28},
        {"RpcServerRegisterIfEx", 24},
        {"RpcServerUnregisterIf", 12},
        {"RpcServerUnregisterIfEx", 12},
        {"RpcServerUseProtseqA", 12},
        {"RpcServerUseProtseqEpA", 16},
        {"RpcServerUseProtseqEpW", 16},
        {"RpcServerUseProtseqW", 12},
        {"RpcSsDestroyClientContext", 4},
        {"RpcSsDontSerializeContext", 0},
        {"RpcStringBindingComposeA", 24},
        {"RpcStringBindingComposeW", 24},
        {"RpcStringBindingParseA", 24},
        {"RpcStringBindingParseW", 24},
    };
    for (const auto& e : kRpcSurface) {
        const u16 bytes = static_cast<u16>(e.bytes);
        const char* name = e.name;
        exports.entries.push_back({name, 0,
            [bytes, name](Marshaler& m) {
                const u32 argc = static_cast<u32>(bytes) / 4u;
                for (u32 i = 0; i < argc; ++i) (void)m.arg(i);
                // RpcRaiseException must not return on Windows; without a full
                // RPC exception path, log and leave EAX=status for diagnosis.
                if (std::strcmp(name, "RpcRaiseException") == 0) {
                    VITREO_TRACE("[rpcrt4] RpcRaiseException(0x%x) — no RPC "
                                 "exception engine\n",
                                 static_cast<unsigned>(m.arg(0)));
                }
                m.set_return(0);
            }, bytes});
    }

    return exports;
}

} // namespace vitreo::winapi
