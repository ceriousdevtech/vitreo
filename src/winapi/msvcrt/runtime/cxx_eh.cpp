// SPDX-License-Identifier: LGPL-2.1-only
/// @file cxx_eh.cpp
/// @brief MSVC C++ exception handler personality routine (__CxxFrameHandler3).
///
/// Implements the documented MSVC C++ exception handling ABI for 32-bit x86:
/// parses FuncInfo / TryBlockMap / HandlerType, matches the thrown type's
/// CatchableType chain against each catch's TypeDescriptor (by mangled name),
/// and on a match writes the catch parameter into the function's frame and
/// transfers control to the catch handler via the SEH dispatcher's CONTEXT
/// record (returning ExceptionContinueExecution).
///
/// This is the minimum-viable implementation: it does NOT call unwind
/// destructor funclets between the current state and tryLow-1, so any
/// RAII objects on the unwound stack will leak. Adding that requires
/// invoking guest code from inside the handler (run_guest_callback) and
/// is deferred to a follow-up.

#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/seh/seh32.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace vitreo::winapi {

// =============================================================================
// MSVC C++ EH structures (32-bit)
// =============================================================================

namespace {

constexpr u32 CXX_EH_MAGIC_VC6  = 0x19930520;
constexpr u32 CXX_EH_MAGIC_VC7  = 0x19930521;
constexpr u32 CXX_EH_MAGIC_VC8  = 0x19930522;

constexpr u32 HT_IS_CONST     = 0x01;
constexpr u32 HT_IS_VOLATILE  = 0x02;
constexpr u32 HT_IS_REFERENCE = 0x08;

constexpr u32 EXCEPTION_CPP_MSVC = 0xE06D7363;

// 32-bit on-disk layouts — read with read_bytes since guest is little-endian
// matching host (ARM64 macOS).

struct FuncInfo32 {
    u32 magic;
    i32 max_state;
    u32 unwind_map_ptr;
    u32 n_try_blocks;
    u32 try_block_map_ptr;
    u32 n_ip_to_state;
    u32 ip_to_state_ptr;
    // u32 es_type_list;   // present only if magic >= 0x19930521
    // u32 eh_flags;       // present only if magic >= 0x19930522
};

struct TryBlockMapEntry32 {
    i32 try_low;
    i32 try_high;
    i32 catch_high;
    i32 n_catches;
    u32 handler_array_ptr;
};

struct HandlerType32 {
    u32 adjectives;
    u32 type_desc_ptr;
    i32 disp_catch_obj;
    u32 address_of_handler;
};

struct ThrowInfo32 {
    u32 attributes;
    u32 pmfn_unwind;
    u32 forward_compat;
    u32 catchable_type_array_ptr;
};

struct CatchableType32 {
    u32 properties;
    u32 type_desc_ptr;
    i32 pmd_mdisp;
    i32 pmd_pdisp;
    i32 pmd_vdisp;
    i32 size_or_offset;
    u32 copy_function;
};

// Read a NUL-terminated mangled type-info name starting at type_desc + 8.
// TypeDescriptor layout: u32 vftable; u32 spare; char name[];
std::string read_type_name(GuestMemory& mem, u32 type_desc_addr) {
    if (type_desc_addr == 0) return {};
    std::string name;
    name.reserve(64);
    GuestAddr p = type_desc_addr + 8;
    for (int i = 0; i < 1024; ++i) {
        auto r = mem.read_u8(p + i);
        if (r.is_err()) break;
        u8 c = r.value();
        if (c == 0) break;
        name.push_back(static_cast<char>(c));
    }
    return name;
}

bool type_matches(GuestMemory& mem, u32 catch_type_desc, u32 throw_type_desc) {
    // Ellipsis catch ("catch(...)") has a null type descriptor in HandlerType
    if (catch_type_desc == 0) return true;
    if (throw_type_desc == 0) return false;
    if (catch_type_desc == throw_type_desc) return true;
    std::string ct = read_type_name(mem, catch_type_desc);
    std::string tt = read_type_name(mem, throw_type_desc);
    return !ct.empty() && ct == tt;
}

} // namespace

// =============================================================================
// dispatch_cxx_exception_32
// =============================================================================

CxxDispatchResult dispatch_cxx_exception_32(
    GuestMemory& memory,
    u32 func_info_addr,
    u32 establisher_frame,
    u32 exception_record_addr,
    u32 context_record_addr)
{
    CxxDispatchResult r;

    // 1. Validate this is a C++ exception (code = 0xE06D7363, magic = 0x19930520+)
    auto code_r = memory.read_u32(exception_record_addr + cpu::EREC32_CODE);
    if (code_r.is_err()) return r;
    if (code_r.value() != EXCEPTION_CPP_MSVC) return r;

    auto nparams_r = memory.read_u32(exception_record_addr + cpu::EREC32_NUM_PARAMS);
    if (nparams_r.is_err() || nparams_r.value() < 3) return r;

    auto magic_param_r = memory.read_u32(exception_record_addr + cpu::EREC32_PARAMS);
    if (magic_param_r.is_err()) return r;
    u32 magic_param = magic_param_r.value();
    if (magic_param != CXX_EH_MAGIC_VC6 && magic_param != CXX_EH_MAGIC_VC7 &&
        magic_param != CXX_EH_MAGIC_VC8) {
        return r;
    }

    auto obj_r = memory.read_u32(exception_record_addr + cpu::EREC32_PARAMS + 4);
    if (obj_r.is_err()) return r;
    u32 throw_object_addr = obj_r.value();

    auto ti_r = memory.read_u32(exception_record_addr + cpu::EREC32_PARAMS + 8);
    if (ti_r.is_err()) return r;
    u32 throw_info_addr = ti_r.value();
    if (throw_info_addr == 0) return r;

    // 2. Read FuncInfo (passed in EAX by the __ehhandler thunk)
    FuncInfo32 fi{};
    if (memory.read_bytes(func_info_addr, reinterpret_cast<Byte*>(&fi),
                          sizeof(fi)).is_err()) {
        return r;
    }
    if (fi.magic != CXX_EH_MAGIC_VC6 && fi.magic != CXX_EH_MAGIC_VC7 &&
        fi.magic != CXX_EH_MAGIC_VC8) {
        return r;
    }
    if (fi.n_try_blocks == 0 || fi.try_block_map_ptr == 0) return r;
    if (fi.n_try_blocks > 512) return r;

    // 3. Current state lives in the EH registration node at offset 8.
    //    Standard MSVC x86 layout: [pNext(4)][handler(4)][state(4)].
    auto state_r = memory.read_u32(establisher_frame + 8);
    if (state_r.is_err()) return r;
    i32 current_state = static_cast<i32>(state_r.value());
    // If state == -1 we're outside any try block — no match possible.
    if (current_state < 0) return r;

    // 4. Read ThrowInfo and CatchableTypeArray header
    ThrowInfo32 ti{};
    if (memory.read_bytes(throw_info_addr, reinterpret_cast<Byte*>(&ti),
                          sizeof(ti)).is_err()) {
        return r;
    }
    if (ti.catchable_type_array_ptr == 0) return r;

    auto nct_r = memory.read_u32(ti.catchable_type_array_ptr);
    if (nct_r.is_err()) return r;
    i32 n_catchable = static_cast<i32>(nct_r.value());
    if (n_catchable <= 0 || n_catchable > 256) return r;

    // 5. Walk TryBlockMap; for each try block whose state range covers
    //    current_state, walk HandlerArray and match catch type against
    //    each entry in the throw's CatchableTypeArray (most-derived first,
    //    matching MSVC's intended type-resolution order).
    for (u32 i = 0; i < fi.n_try_blocks; ++i) {
        TryBlockMapEntry32 tbe{};
        u32 tbe_addr = fi.try_block_map_ptr +
                       i * static_cast<u32>(sizeof(TryBlockMapEntry32));
        if (memory.read_bytes(tbe_addr, reinterpret_cast<Byte*>(&tbe),
                              sizeof(tbe)).is_err()) {
            continue;
        }
        if (current_state < tbe.try_low || current_state > tbe.try_high) {
            continue;
        }
        if (tbe.n_catches <= 0 || tbe.handler_array_ptr == 0 ||
            tbe.n_catches > 256) {
            continue;
        }

        for (i32 j = 0; j < tbe.n_catches; ++j) {
            HandlerType32 ht{};
            u32 ht_addr = tbe.handler_array_ptr +
                          static_cast<u32>(j) *
                          static_cast<u32>(sizeof(HandlerType32));
            if (memory.read_bytes(ht_addr, reinterpret_cast<Byte*>(&ht),
                                  sizeof(ht)).is_err()) {
                continue;
            }

            for (i32 k = 0; k < n_catchable; ++k) {
                u32 ct_ptr_slot = ti.catchable_type_array_ptr + 4 +
                                  static_cast<u32>(k) * 4;
                auto ct_ptr_r = memory.read_u32(ct_ptr_slot);
                if (ct_ptr_r.is_err()) continue;
                u32 ct_addr = ct_ptr_r.value();
                if (ct_addr == 0) continue;

                CatchableType32 ct{};
                if (memory.read_bytes(ct_addr, reinterpret_cast<Byte*>(&ct),
                                      sizeof(ct)).is_err()) {
                    continue;
                }
                if (!type_matches(memory, ht.type_desc_ptr, ct.type_desc_ptr)) {
                    continue;
                }

                // Match! Fill out the result and return.
                r.match              = true;
                r.handler_addr       = ht.address_of_handler;
                r.disp_catch_obj     = ht.disp_catch_obj;
                r.adjectives         = ht.adjectives;
                r.try_high           = tbe.try_high;
                r.try_low            = tbe.try_low;
                r.catchable_type_addr= ct_addr;
                r.throw_object_addr  = throw_object_addr;
                r.catchable_size     = static_cast<u32>(ct.size_or_offset);
                r.catchable_copy_fn  = ct.copy_function;
                r.catchable_pmd_mdisp= ct.pmd_mdisp;
                return r;
            }
        }
    }

    return r;
}

// =============================================================================
// Diagnostics
// =============================================================================

std::string describe_cxx_throw_32(GuestMemory& memory,
                                  u32 exception_record_addr) {
    auto code_r = memory.read_u32(exception_record_addr + cpu::EREC32_CODE);
    if (code_r.is_err() || code_r.value() != EXCEPTION_CPP_MSVC) return {};

    auto nparams_r =
        memory.read_u32(exception_record_addr + cpu::EREC32_NUM_PARAMS);
    if (nparams_r.is_err() || nparams_r.value() < 3) return {};

    auto obj_r = memory.read_u32(exception_record_addr + cpu::EREC32_PARAMS + 4);
    auto ti_r = memory.read_u32(exception_record_addr + cpu::EREC32_PARAMS + 8);
    if (obj_r.is_err() || ti_r.is_err()) return {};

    char head[64];
    std::snprintf(head, sizeof(head), "obj=0x%08x ti=0x%08x types=[",
                  obj_r.value(), ti_r.value());
    std::string out(head);

    if (ti_r.value() == 0) return out + "<rethrow>]";

    ThrowInfo32 ti{};
    if (memory.read_bytes(ti_r.value(), reinterpret_cast<Byte*>(&ti),
                          sizeof(ti)).is_err() ||
        ti.catchable_type_array_ptr == 0) {
        return out + "<unreadable>]";
    }
    auto nct_r = memory.read_u32(ti.catchable_type_array_ptr);
    if (nct_r.is_err()) return out + "<unreadable>]";

    const i32 n = static_cast<i32>(nct_r.value());
    for (i32 k = 0; k < n && k < 32; ++k) {
        auto ct_ptr_r =
            memory.read_u32(ti.catchable_type_array_ptr + 4 +
                            static_cast<u32>(k) * 4);
        if (ct_ptr_r.is_err() || ct_ptr_r.value() == 0) continue;
        CatchableType32 ct{};
        if (memory.read_bytes(ct_ptr_r.value(),
                              reinterpret_cast<Byte*>(&ct),
                              sizeof(ct)).is_err()) {
            continue;
        }
        if (k != 0) out += ", ";
        out += read_type_name(memory, ct.type_desc_ptr);
    }
    return out + "]";
}

std::string describe_cxx_frame_catches_32(GuestMemory& memory,
                                          u32 func_info_addr,
                                          u32 establisher_frame) {
    FuncInfo32 fi{};
    if (memory.read_bytes(func_info_addr, reinterpret_cast<Byte*>(&fi),
                          sizeof(fi)).is_err()) {
        return {};
    }
    auto state_r = memory.read_u32(establisher_frame + 8);

    char head[96];
    std::snprintf(head, sizeof(head),
                  "magic=0x%08x state=%d nTry=%u tries=[",
                  fi.magic,
                  state_r.is_ok() ? static_cast<int>(state_r.value()) : -999,
                  fi.n_try_blocks);
    std::string out(head);

    if (fi.try_block_map_ptr == 0 || fi.n_try_blocks > 512) return out + "]";
    for (u32 i = 0; i < fi.n_try_blocks; ++i) {
        TryBlockMapEntry32 tbe{};
        if (memory.read_bytes(fi.try_block_map_ptr +
                                  i * static_cast<u32>(sizeof(tbe)),
                              reinterpret_cast<Byte*>(&tbe),
                              sizeof(tbe)).is_err()) {
            continue;
        }
        char range[48];
        std::snprintf(range, sizeof(range), "%s%d..%d->", i ? ", " : "",
                      tbe.try_low, tbe.try_high);
        out += range;
        if (tbe.handler_array_ptr == 0 || tbe.n_catches <= 0 ||
            tbe.n_catches > 256) {
            out += "<none>";
            continue;
        }
        for (i32 j = 0; j < tbe.n_catches; ++j) {
            HandlerType32 ht{};
            if (memory.read_bytes(tbe.handler_array_ptr +
                                      static_cast<u32>(j) *
                                          static_cast<u32>(sizeof(ht)),
                                  reinterpret_cast<Byte*>(&ht),
                                  sizeof(ht)).is_err()) {
                continue;
            }
            if (j != 0) out += "|";
            out += ht.type_desc_ptr == 0
                       ? std::string("...")
                       : read_type_name(memory, ht.type_desc_ptr);
        }
    }
    return out + "]";
}

// =============================================================================
// install_catch_param_32
// =============================================================================

bool install_catch_param_32(
    GuestMemory& memory,
    const CxxDispatchResult& r,
    u32 establisher_frame)
{
    if (!r.match) return false;
    if (r.disp_catch_obj == 0) {
        // catch(...) — no catch parameter to set up
        return true;
    }

    // MSVC x86 standard layout: parent EBP = establisher_frame + 12
    // The HandlerType's disp_catch_obj is a frame-relative offset from EBP.
    // dispCatchObj is a signed offset and may be negative (locals are below EBP).
    u32 parent_ebp = establisher_frame + 12;
    GuestAddr slot = static_cast<GuestAddr>(
        static_cast<i64>(parent_ebp) + static_cast<i64>(r.disp_catch_obj));

    if (r.adjectives & HT_IS_REFERENCE) {
        // catch by reference: write a pointer to the throw object.
        // Apply the PMD this-displacement so the pointer addresses the
        // matched base-class subobject when catching a base of the throw.
        u32 ptr = r.throw_object_addr +
                  static_cast<u32>(static_cast<i32>(r.catchable_pmd_mdisp));
        return memory.write_u32(slot, ptr).is_ok();
    }

    // catch by value: copy the bytes.
    // If a copy function is provided we should call it — but invoking guest
    // code from inside a thunk handler is non-trivial.  Fall back to a raw
    // byte copy, which is correct for trivially-copyable types (most C++
    // exception payloads).  TODO: invoke copy_function for non-trivial.
    if (r.catchable_size == 0 || r.catchable_size > 65536) return false;
    std::vector<Byte> buf(r.catchable_size);
    u32 src = r.throw_object_addr +
              static_cast<u32>(static_cast<i32>(r.catchable_pmd_mdisp));
    if (memory.read_bytes(src, buf.data(), r.catchable_size).is_err()) {
        return false;
    }
    return memory.write_bytes(slot, buf.data(), r.catchable_size).is_ok();
}

// =============================================================================
// catch_funclet_callback_stack_top_32
// =============================================================================

std::optional<u32> catch_funclet_callback_stack_top_32(
    GuestMemory& memory,
    u32 establisher_frame)
{
    if (establisher_frame < sizeof(u32)) return std::nullopt;

    // MSVC's x86 EH prologue stores the stack value needed by the shared
    // epilogue immediately before the registration record. run_guest_callback
    // pushes a four-byte sentinel before entering guest code, so its starting
    // stack must be one word above the value the funclet expects in ESP.
    auto resume_sp = memory.read_u32(establisher_frame - sizeof(u32));
    if (resume_sp.is_err() || resume_sp.value() == 0 ||
        resume_sp.value() > UINT32_MAX - sizeof(u32)) {
        return std::nullopt;
    }
    return resume_sp.value() + sizeof(u32);
}

std::optional<u32> catch_funclet_entry_stack_32(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 throw_site_sp)
{
    if (throw_site_sp < 0x1000) return std::nullopt;

    const auto epilogue_top =
        catch_funclet_callback_stack_top_32(memory, establisher_frame);
    if (!epilogue_top.has_value()) return std::nullopt;

    // The throw site must genuinely be deeper than the catch frame. If it is
    // not, this is not a stack the dispatcher walked up and there is nothing
    // below the catch to protect.
    if (throw_site_sp >= epilogue_top.value()) return std::nullopt;

    // Round down so the funclet enters 16-byte aligned, as a call site would.
    // The four bytes below hold run_guest_callback's return sentinel and are
    // already dead: nothing outlives the raise below this point.
    const u32 aligned = throw_site_sp & ~static_cast<u32>(0xF);
    if (aligned < 0x1000) return std::nullopt;
    return aligned;
}

// =============================================================================
// patch_context_for_catch_32
// =============================================================================

void patch_context_for_catch_32(
    GuestMemory& memory,
    const CxxDispatchResult& r,
    u32 establisher_frame,
    u32 context_record_addr,
    u32 continuation_addr)
{
    const u32 parent_ebp = establisher_frame + 12;
    (void)memory.write_u32(context_record_addr + cpu::CTX32_EIP,
                           continuation_addr != 0 ? continuation_addr
                                                  : r.handler_addr);
    (void)memory.write_u32(context_record_addr + cpu::CTX32_EBP, parent_ebp);

    // MSVC's EH prologue records the epilogue stack at EBP-0x10 (immediately
    // before the registration node). Restoring it lets the continuation's
    // compiler-generated epilogue recover nonvolatile registers and return
    // from the parent function instead of consuming the throw site's stack.
    auto resume_sp = memory.read_u32(establisher_frame - 4);
    if (resume_sp.is_ok() && resume_sp.value() != 0) {
        (void)memory.write_u32(context_record_addr + cpu::CTX32_ESP,
                               resume_sp.value());
    }

    // Update the EH frame's state to (try_high + 1), matching what the
    // compiler-generated state transition would do when entering the catch
    // block.  Without this, a re-throw inside the catch (or any subsequent
    // exception in this function) would incorrectly target this same try.
    (void)memory.write_u32(establisher_frame + 8,
                            static_cast<u32>(r.try_high + 1));
}

// =============================================================================
// Rethrow support
// =============================================================================

namespace {

/// One exception a catch block on this thread is currently handling.
struct ActiveCxxException {
    u32 throw_info_addr{0};
    u32 object_addr{0};
    std::vector<Byte> object_bytes; ///< Snapshot taken when the catch was entered.
};

/// Innermost-last. Nested catches (and a rethrow caught by an outer catch)
/// each push, so `throw;` always re-raises the right one.
thread_local std::vector<ActiveCxxException> t_active_exceptions;

/// Exceptions thrown on this thread that have not yet entered a catch.
thread_local int t_uncaught_exceptions = 0;

/// Bump offset into the rethrow scratch buffer.
thread_local u32 t_rethrow_scratch_offset = 0;

/// Size of the thrown object, from the first catchable type.
[[nodiscard]] u32 throw_object_size(GuestMemory& memory, u32 throw_info_addr) {
    if (throw_info_addr == 0) return 0;
    ThrowInfo32 ti{};
    if (memory.read_bytes(throw_info_addr, reinterpret_cast<Byte*>(&ti),
                          sizeof(ti)).is_err()) {
        return 0;
    }
    if (ti.catchable_type_array_ptr == 0) return 0;
    auto count = memory.read_u32(ti.catchable_type_array_ptr);
    if (count.is_err() || count.value() == 0) return 0;
    auto first = memory.read_u32(ti.catchable_type_array_ptr + 4);
    if (first.is_err() || first.value() == 0) return 0;
    CatchableType32 ct{};
    if (memory.read_bytes(first.value(), reinterpret_cast<Byte*>(&ct),
                          sizeof(ct)).is_err()) {
        return 0;
    }
    const i32 size = ct.size_or_offset;
    if (size <= 0 || size > 65536) return 0;
    return static_cast<u32>(size);
}

} // namespace

void push_active_cxx_exception(GuestMemory& memory, u32 exception_record_addr) {
    ActiveCxxException active;

    auto code = memory.read_u32(exception_record_addr + cpu::EREC32_CODE);
    auto nparams =
        memory.read_u32(exception_record_addr + cpu::EREC32_NUM_PARAMS);
    if (code.is_ok() && code.value() == EXCEPTION_CPP_MSVC &&
        nparams.is_ok() && nparams.value() >= 3) {
        auto obj = memory.read_u32(exception_record_addr + cpu::EREC32_PARAMS + 4);
        auto ti  = memory.read_u32(exception_record_addr + cpu::EREC32_PARAMS + 8);
        if (obj.is_ok() && ti.is_ok()) {
            active.object_addr     = obj.value();
            active.throw_info_addr = ti.value();

            // The object lives in the frame being unwound, so copy it now:
            // by the time a rethrow happens the catch block will have reused
            // that stack.
            const u32 size = throw_object_size(memory, active.throw_info_addr);
            if (size != 0 && active.object_addr != 0) {
                active.object_bytes.resize(size);
                if (memory.read_bytes(active.object_addr,
                                      active.object_bytes.data(),
                                      size).is_err()) {
                    active.object_bytes.clear();
                }
            }
        }
    }

    // An entry is pushed even when the record could not be read, so that the
    // matching pop always balances.
    t_active_exceptions.push_back(std::move(active));
}

void pop_active_cxx_exception() {
    if (!t_active_exceptions.empty()) t_active_exceptions.pop_back();
}

void note_cxx_exception_thrown() {
    ++t_uncaught_exceptions;
}

void note_cxx_exception_caught() {
    if (t_uncaught_exceptions > 0) --t_uncaught_exceptions;
}

int cxx_uncaught_exceptions() noexcept {
    return t_uncaught_exceptions;
}

bool cxx_uncaught_exception() noexcept {
    return t_uncaught_exceptions > 0;
}

bool resolve_cxx_rethrow_32(GuestMemory& memory, u32 code, u32 num_params,
                            u32* params) {
    if (code != EXCEPTION_CPP_MSVC || params == nullptr || num_params < 3) {
        return false;
    }
    const u32 magic = params[0];
    if (magic != CXX_EH_MAGIC_VC6 && magic != CXX_EH_MAGIC_VC7 &&
        magic != CXX_EH_MAGIC_VC8) {
        return false;
    }
    // A ThrowInfo is what makes an exception matchable; only its absence marks
    // a rethrow. A NULL object with a real ThrowInfo is a different thing.
    if (params[2] != 0) return false;
    if (t_active_exceptions.empty()) {
        VITREO_TRACE("[cxx_eh] rethrow with no active exception — leaving it "
                     "unmatchable\n");
        return false;
    }

    const ActiveCxxException& active = t_active_exceptions.back();
    if (active.throw_info_addr == 0) return false;

    u32 object_addr = active.object_addr;
    if (!active.object_bytes.empty()) {
        // Re-materialise the snapshot somewhere the unwind cannot reach.
        const u32 size = static_cast<u32>(active.object_bytes.size());
        if (t_rethrow_scratch_offset + size > CXX_RETHROW_SCRATCH_SIZE) {
            t_rethrow_scratch_offset = 0;
        }
        const GuestAddr slot =
            CXX_RETHROW_SCRATCH_BASE + t_rethrow_scratch_offset;
        if (memory.write_bytes(slot, active.object_bytes.data(), size).is_ok()) {
            object_addr = static_cast<u32>(slot);
            t_rethrow_scratch_offset += (size + 15u) & ~15u;
        }
    }

    params[1] = object_addr;
    params[2] = active.throw_info_addr;
    VITREO_TRACE("[cxx_eh] rethrow -> obj=0x%08x ThrowInfo=0x%08x (depth=%zu)\n",
                 object_addr, active.throw_info_addr,
                 t_active_exceptions.size());
    return true;
}

} // namespace vitreo::winapi
