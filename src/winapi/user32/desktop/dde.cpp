// SPDX-License-Identifier: LGPL-2.1-only

/// @file dde.cpp
/// @brief The DDEML client surface: DdeInitializeW, DdeUninitialize,
/// DdeCreateStringHandleW, DdeFreeStringHandle, DdeConnect, DdeDisconnect,
/// DdeClientTransaction, DdeGetLastError.
///
/// Contract: Microsoft Learn (ddeml.h). Measured on Windows 11
/// (tools/oracle_probe/s1win_misc_probe.c "-- dde", .expected):
///  * DdeInitializeW succeeds (also without a callback) and fails with
///    DMLERR_INVALIDPARAMETER when *pidInst is not 0;
///  * a string handle needs CP_WINUNICODE (W) or CP_WINANSI; another code page
///    returns 0 and records DMLERR_INVALIDPARAMETER on the instance;
///  * DdeGetLastError returns the instance's last error and clears it; an
///    unknown instance reports DMLERR_INVALIDPARAMETER;
///  * DdeConnect with no server for the service/topic returns NULL and
///    records DMLERR_NO_CONV_ESTABLISHED; DdeClientTransaction on a NULL
///    conversation returns 0 and records DMLERR_INVALIDPARAMETER on the
///    calling thread's instance; DdeDisconnect(NULL) is FALSE;
///  * DdeFreeStringHandle of an already freed handle still returns TRUE;
///    DdeUninitialize of an unknown instance is FALSE.
///
///
/// Data handles (u32_char_dde_probe "-- ddeml", both bitnesses):
///  * DdeCreateDataHandle places the cb source bytes at OBJECT offset cbOff
///    (object size cb + cbOff; create_off), a NULL pSrc gives an object of cb
///    bytes, cb 0 an empty object; afCmd other than HDATA_APPOWNED is 0 with
///    DMLERR_INVALIDPARAMETER, and so is an unknown instance -- recorded on
///    the calling thread's instances; hszItem is not validated;
///  * DdeGetData with a NULL buffer returns size - cbOff; with a buffer it
///    copies min(cbMax, size - cbOff); cbOff at or past the end (so any call
///    on an empty object) is 0 with DMLERR_INVALIDPARAMETER;
///  * DdeAccessData returns the same pointer every time and the size;
///    DdeUnaccessData is TRUE for any live handle, accessed or not;
///  * DdeAddData writes at cbOff, growing the object to cbOff + cb, and
///    returns the same handle (the old handle stays valid);
///  * an unknown, freed or NULL handle is 0/FALSE from DdeAccessData,
///    DdeAddData, DdeUnaccessData and DdeFreeDataHandle with no DDEML error,
///    while DdeGetData records DMLERR_INVALIDPARAMETER on the calling
///    thread's instances; DdeUninitialize frees the instance's objects, owned
///    or not;
///  * a handle passed to DdeClientTransaction without a conversation is not
///    consumed (transact / free_after_transact).
/// No conversation is ever established here, so no DDEML function or DDE
/// callback ever takes ownership of an unowned handle.
///
/// Every Vitreo guest process is its own host process and there is no
/// cross-process DDE broker, so no DDE server is ever found: a client sees
/// exactly what Windows reports when the service is not running.

#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/memory/global_local.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace vitreo::winapi {

namespace {

constexpr u32 DMLERR_NO_ERROR = 0;
constexpr u32 DMLERR_DLL_USAGE = 0x4004;
constexpr u32 DMLERR_INVALIDPARAMETER = 0x4006;
constexpr u32 DMLERR_MEMORY_ERROR = 0x4008;
constexpr u32 DMLERR_NO_CONV_ESTABLISHED = 0x400A;
constexpr u32 CP_WINANSI = 1004;
constexpr u32 CP_WINUNICODE = 1200;

struct DdeInstance {
    u32 thread = 0;
    u32 last_error = DMLERR_NO_ERROR;
    u32 af_cmd = 0;                 ///< DdeInitialize afCmd
    std::set<u32> services;         ///< DdeNameService DNS_REGISTER names
    bool filter = false;            ///< DNS_FILTERON
};

struct DdeData {
    u32 instance = 0;
    u64 memory = 0;   ///< FIXED global block (0 when the object is empty)
    u32 size = 0;
};

struct DdeState {
    std::mutex mutex;
    std::map<u32, DdeInstance> instances;
    std::map<u64, DdeData> data;
    std::map<std::wstring, u32> string_atoms;   // upper-cased text -> HSZ
    u32 next_instance = 0x00010001;
    u32 next_hsz = 0xC000;
    u64 next_data = 0x00DD0004;
};

constexpr u32 HDATA_APPOWNED = 0x0001;
constexpr u32 GMEM_ZEROINIT = 0x0040;

/// Record @p error on every instance the calling thread created (the
/// failure names no instance of its own).
void set_thread_error(DdeState& st, u32 error) {
    const u32 thread = ThreadManager::get_current_thread_id_static();
    for (auto& [inst, rec] : st.instances) {
        if (rec.thread == thread) rec.last_error = error;
    }
}

/// Resize @p obj to @p size, keeping its bytes; new bytes are zero.
bool resize_data(Marshaler& m, GlobalLocalManager& gl, DdeData& obj, u32 size) {
    if (size == obj.size) return true;
    u64 block = 0;
    if (size != 0) {
        block = gl.global_alloc(GMEM_ZEROINIT, size);
        if (block == 0) return false;
        const u32 keep = std::min(size, obj.size);
        if (keep != 0) {
            std::vector<u8> bytes(keep);
            if (m.memory().read_bytes(static_cast<GuestAddr>(obj.memory), bytes.data(),
                                      keep).is_err() ||
                m.memory().write_bytes(static_cast<GuestAddr>(block), bytes.data(),
                                       keep).is_err()) {
                (void)gl.global_free(block);
                return false;
            }
        }
    }
    if (obj.memory != 0) (void)gl.global_free(obj.memory);
    obj.memory = block;
    obj.size = size;
    return true;
}

/// Copy @p cb guest bytes from @p src into @p obj at @p offset.
bool copy_into(Marshaler& m, const DdeData& obj, GuestAddr src, u32 cb, u32 offset) {
    if (src == 0 || cb == 0) return true;
    std::vector<u8> bytes(cb);
    return m.memory().read_bytes(src, bytes.data(), cb).is_ok() &&
           m.memory().write_bytes(static_cast<GuestAddr>(obj.memory) + offset,
                                  bytes.data(), cb).is_ok();
}

DdeState& state() {
    static DdeState s;
    return s;
}

} // namespace

void add_dde_exports(DllExports& exports, GlobalLocalManager* const& global_local) {
    exports.entries.push_back(ExportEntry{
        "DdeInitializeW", 0,
        [](Marshaler& m) {
            // UINT DdeInitializeW(LPDWORD pidInst, PFNCALLBACK, DWORD afCmd, DWORD)
            const GuestAddr pinst = m.arg_ptr(0);
            if (pinst == 0 || m.memory().read_u32(pinst).value_or(1) != 0) {
                m.set_return(DMLERR_INVALIDPARAMETER);
                return;
            }
            auto& st = state();
            std::lock_guard lock(st.mutex);
            const u32 inst = st.next_instance++;
            DdeInstance rec;
            rec.thread = ThreadManager::get_current_thread_id_static();
            rec.af_cmd = static_cast<u32>(m.arg(2));
            st.instances[inst] = std::move(rec);
            (void)m.write_u32(pinst, inst);
            m.set_return(DMLERR_NO_ERROR);
        }, 16});
    exports.entries.push_back(ExportEntry{
        "DdeUninitialize", 0,
        [&global_local](Marshaler& m) {
            const u32 inst = static_cast<u32>(m.arg(0));
            auto& st = state();
            std::lock_guard lock(st.mutex);
            if (st.instances.erase(inst) == 0) {
                m.set_return(0u);
                return;
            }
            for (auto it = st.data.begin(); it != st.data.end();) {
                if (it->second.instance != inst) { ++it; continue; }
                if (it->second.memory != 0 && global_local != nullptr) {
                    (void)global_local->global_free(it->second.memory);
                }
                it = st.data.erase(it);
            }
            m.set_return(1u);
        }, 4});
    exports.entries.push_back(ExportEntry{
        "DdeCreateStringHandleW", 0,
        [](Marshaler& m) {
            // HSZ DdeCreateStringHandleW(DWORD idInst, LPCWSTR, int iCodePage)
            const u32 inst = static_cast<u32>(m.arg(0));
            const GuestAddr text = m.arg_ptr(1);
            const u32 cp = static_cast<u32>(m.arg(2));
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.instances.find(inst);
            if (it == st.instances.end()) {
                m.set_return(0u);
                return;
            }
            if ((cp != CP_WINUNICODE && cp != CP_WINANSI) || text == 0) {
                it->second.last_error = DMLERR_INVALIDPARAMETER;
                m.set_return(0u);
                return;
            }
            std::wstring key;
            if (cp == CP_WINUNICODE) {
                auto r = m.read_wide_string(text);
                if (r.is_ok()) key = utf8_to_wstring(r.value());
            } else {
                auto r = m.read_ansi_string(text);
                if (r.is_ok()) for (unsigned char c : r.value()) key.push_back(c);
            }
            for (auto& c : key) c = static_cast<wchar_t>(std::towupper(c));
            auto found = st.string_atoms.find(key);
            if (found == st.string_atoms.end()) {
                found = st.string_atoms.emplace(key, st.next_hsz++).first;
            }
            m.set_return(found->second);
        }, 12});
    exports.entries.push_back(ExportEntry{
        "DdeFreeStringHandle", 0,
        [](Marshaler& m) {
            // BOOL DdeFreeStringHandle(DWORD idInst, HSZ): the handle stays
            // valid for other users of the same string.
            auto& st = state();
            std::lock_guard lock(st.mutex);
            m.set_return(st.instances.count(static_cast<u32>(m.arg(0))) ? 1u : 0u);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "DdeGetLastError", 0,
        [](Marshaler& m) {
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.instances.find(static_cast<u32>(m.arg(0)));
            if (it == st.instances.end()) {
                m.set_return(DMLERR_INVALIDPARAMETER);
                return;
            }
            m.set_return(it->second.last_error);
            it->second.last_error = DMLERR_NO_ERROR;
        }, 4});
    // int DdeCmpStringHandles(HSZ, HSZ): compares the handle VALUES, -1/0/1,
    // with 0 below any handle and no validation (ddeml_nameservice_probe
    // "== cmp", x86 == x64). Handles of strings that differ only in case are
    // the same handle, so they compare equal.
    exports.entries.push_back(ExportEntry{
        "DdeCmpStringHandles", 0,
        [](Marshaler& m) {
            const u64 a = m.arg(0);
            const u64 b = m.arg(1);
            m.set_return(static_cast<u64>(static_cast<u32>(a < b ? -1 : a > b ? 1 : 0)));
        }, 8});
    // HDDEDATA DdeNameService(DWORD idInst, HSZ hsz1, HSZ hsz2, UINT afCmd)
    // (Learn; ddeml_nameservice_probe "== nameservice", x86 == x64): 1 on
    // success. DNS_REGISTER needs a live string handle (0 or an unknown one is
    // DMLERR_INVALIDPARAMETER) and fails with DMLERR_DLL_USAGE on an
    // APPCMD_CLIENTONLY instance; DNS_UNREGISTER with hsz1 0 drops every
    // name, and unregistering a name that is not registered succeeds;
    // DNS_FILTERON/OFF toggle XTYP_CONNECT filtering; unknown afCmd bits are
    // ignored. A non-zero hsz2 (reserved) or an unknown instance is
    // DMLERR_INVALIDPARAMETER. No other process can reach the name: Vitreo
    // has no cross-process DDE broker (see the file comment).
    exports.entries.push_back(ExportEntry{
        "DdeNameService", 0,
        [](Marshaler& m) {
            constexpr u32 DNS_REGISTER = 0x0001, DNS_UNREGISTER = 0x0002,
                          DNS_FILTERON = 0x0004, DNS_FILTEROFF = 0x0008;
            constexpr u32 APPCMD_CLIENTONLY = 0x00000010;
            const u32 inst = static_cast<u32>(m.arg(0));
            const u32 hsz1 = static_cast<u32>(m.arg(1));
            const u64 hsz2 = m.arg(2);
            const u32 cmd = static_cast<u32>(m.arg(3));
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.instances.find(inst);
            if (it == st.instances.end()) {
                m.set_return(0u);
                return;
            }
            DdeInstance& rec = it->second;
            const bool live_hsz = std::any_of(st.string_atoms.begin(), st.string_atoms.end(),
                                              [hsz1](const auto& kv) { return kv.second == hsz1; });
            if (hsz2 != 0 || ((cmd & DNS_REGISTER) && (hsz1 == 0 || !live_hsz))) {
                rec.last_error = DMLERR_INVALIDPARAMETER;
                m.set_return(0u);
                return;
            }
            if ((cmd & DNS_REGISTER) && (rec.af_cmd & APPCMD_CLIENTONLY)) {
                rec.last_error = DMLERR_DLL_USAGE;
                m.set_return(0u);
                return;
            }
            if (cmd & DNS_REGISTER) rec.services.insert(hsz1);
            if (cmd & DNS_UNREGISTER) {
                if (hsz1 == 0) rec.services.clear();
                else rec.services.erase(hsz1);
            }
            if (cmd & DNS_FILTERON) rec.filter = true;
            if (cmd & DNS_FILTEROFF) rec.filter = false;
            m.set_return(1u);
        }, 16});
    exports.entries.push_back(ExportEntry{
        "DdeConnect", 0,
        [](Marshaler& m) {
            // HCONV DdeConnect(DWORD idInst, HSZ hszService, HSZ hszTopic, PCONVCONTEXT)
            auto& st = state();
            std::lock_guard lock(st.mutex);
            // A service registered by an instance of this process would be
            // offered XTYP_CONNECT on Windows; Vitreo runs no DDE server
            // conversations, so that case fails loudly like a missing server.
            const u32 service = static_cast<u32>(m.arg(1));
            const bool local_server = std::any_of(
                st.instances.begin(), st.instances.end(),
                [service](const auto& kv) { return service != 0 && kv.second.services.count(service) != 0; });
            if (local_server) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!DdeConnect: conversation with a "
                                         "service registered in the same process\n");
                }
            }
            auto it = st.instances.find(static_cast<u32>(m.arg(0)));
            if (it != st.instances.end()) it->second.last_error = DMLERR_NO_CONV_ESTABLISHED;
            m.set_return(0u);
        }, 16});
    exports.entries.push_back(ExportEntry{
        "DdeDisconnect", 0,
        [](Marshaler& m) {
            // No conversation can exist (DdeConnect never establishes one).
            (void)m.arg(0);
            m.set_return(0u);
        }, 4});
    exports.entries.push_back(ExportEntry{
        "DdeClientTransaction", 0,
        [](Marshaler& m) {
            // HDDEDATA DdeClientTransaction(LPBYTE, DWORD, HCONV, HSZ, UINT,
            //                               UINT, DWORD, LPDWORD pdwResult)
            const GuestAddr result = m.arg_ptr(7);
            if (result != 0) (void)m.write_u32(result, 0);
            auto& st = state();
            std::lock_guard lock(st.mutex);
            const u32 thread = ThreadManager::get_current_thread_id_static();
            for (auto& [inst, rec] : st.instances) {
                if (rec.thread == thread) rec.last_error = DMLERR_INVALIDPARAMETER;
            }
            m.set_return(0u);
        }, 32});
    // HDDEDATA DdeCreateDataHandle(DWORD idInst, LPBYTE pSrc, DWORD cb,
    //                              DWORD cbOff, HSZ hszItem, UINT wFmt, UINT afCmd)
    exports.entries.push_back(ExportEntry{
        "DdeCreateDataHandle", 0,
        [&global_local](Marshaler& m) {
            const u32 inst = m.arg32(0);
            const GuestAddr src = m.arg_ptr(1);
            const u32 cb = m.arg32(2);
            const u32 offset = m.arg32(3);
            (void)m.arg(4);      // hszItem: kept by no transaction here
            (void)m.arg32(5);    // wFmt
            const u32 flags = m.arg32(6);
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.instances.find(inst);
            if (it == st.instances.end() || (flags & ~HDATA_APPOWNED) != 0 ||
                static_cast<u64>(cb) + offset > 0xFFFFFFFFull) {
                set_thread_error(st, DMLERR_INVALIDPARAMETER);
                m.set_return(0u);
                return;
            }
            if (global_local == nullptr) {
                it->second.last_error = DMLERR_MEMORY_ERROR;
                m.set_return(0u);
                return;
            }
            DdeData obj;
            obj.instance = inst;
            if (!resize_data(m, *global_local, obj, cb + offset) ||
                !copy_into(m, obj, src, cb, offset)) {
                if (obj.memory != 0) (void)global_local->global_free(obj.memory);
                it->second.last_error = DMLERR_MEMORY_ERROR;
                m.set_return(0u);
                return;
            }
            const u64 handle = st.next_data;
            st.next_data += 4;
            st.data.emplace(handle, obj);
            m.set_return(handle);
        }, 28});
    // HDDEDATA DdeAddData(HDDEDATA hData, LPBYTE pSrc, DWORD cb, DWORD cbOff)
    exports.entries.push_back(ExportEntry{
        "DdeAddData", 0,
        [&global_local](Marshaler& m) {
            const u64 handle = m.arg(0);
            const GuestAddr src = m.arg_ptr(1);
            const u32 cb = m.arg32(2);
            const u32 offset = m.arg32(3);
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.data.find(handle);
            if (it == st.data.end() || global_local == nullptr) {
                m.set_return(0u);
                return;
            }
            DdeData& obj = it->second;
            const u64 end = static_cast<u64>(offset) + cb;
            auto inst = st.instances.find(obj.instance);
            if (end > 0xFFFFFFFFull ||
                (end > obj.size && !resize_data(m, *global_local, obj, static_cast<u32>(end))) ||
                !copy_into(m, obj, src, cb, offset)) {
                if (inst != st.instances.end()) inst->second.last_error = DMLERR_MEMORY_ERROR;
                m.set_return(0u);
                return;
            }
            m.set_return(handle);
        }, 16});
    // DWORD DdeGetData(HDDEDATA hData, LPBYTE pDst, DWORD cbMax, DWORD cbOff)
    exports.entries.push_back(ExportEntry{
        "DdeGetData", 0,
        [](Marshaler& m) {
            const u64 handle = m.arg(0);
            const GuestAddr dst = m.arg_ptr(1);
            const u32 cb_max = m.arg32(2);
            const u32 offset = m.arg32(3);
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.data.find(handle);
            if (it == st.data.end()) {
                set_thread_error(st, DMLERR_INVALIDPARAMETER);
                m.set_return(0u);
                return;
            }
            const DdeData& obj = it->second;
            if (offset >= obj.size) {
                auto inst = st.instances.find(obj.instance);
                if (inst != st.instances.end()) inst->second.last_error = DMLERR_INVALIDPARAMETER;
                m.set_return(0u);
                return;
            }
            const u32 available = obj.size - offset;
            if (dst == 0) {
                m.set_return(available);
                return;
            }
            const u32 n = std::min(cb_max, available);
            std::vector<u8> bytes(n);
            if (n != 0 &&
                (m.memory().read_bytes(static_cast<GuestAddr>(obj.memory) + offset,
                                       bytes.data(), n).is_err() ||
                 m.memory().write_bytes(dst, bytes.data(), n).is_err())) {
                m.set_return(0u);
                return;
            }
            m.set_return(n);
        }, 16});
    // LPBYTE DdeAccessData(HDDEDATA hData, LPDWORD pcbDataSize)
    exports.entries.push_back(ExportEntry{
        "DdeAccessData", 0,
        [](Marshaler& m) {
            const u64 handle = m.arg(0);
            const GuestAddr size_out = m.arg_ptr(1);
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.data.find(handle);
            if (it == st.data.end()) {
                m.set_return(0u);
                return;
            }
            if (size_out != 0) (void)m.write_u32(size_out, it->second.size);
            m.set_return(it->second.memory);
        }, 8});
    // BOOL DdeUnaccessData(HDDEDATA hData)
    exports.entries.push_back(ExportEntry{
        "DdeUnaccessData", 0,
        [](Marshaler& m) {
            auto& st = state();
            std::lock_guard lock(st.mutex);
            m.set_return(st.data.count(m.arg(0)) ? 1u : 0u);
        }, 4});
    // BOOL DdeFreeDataHandle(HDDEDATA hData)
    exports.entries.push_back(ExportEntry{
        "DdeFreeDataHandle", 0,
        [&global_local](Marshaler& m) {
            auto& st = state();
            std::lock_guard lock(st.mutex);
            auto it = st.data.find(m.arg(0));
            if (it == st.data.end()) {
                m.set_return(0u);
                return;
            }
            if (it->second.memory != 0 && global_local != nullptr) {
                (void)global_local->global_free(it->second.memory);
            }
            st.data.erase(it);
            m.set_return(1u);
        }, 4});
}

} // namespace vitreo::winapi
