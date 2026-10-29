// SPDX-License-Identifier: LGPL-2.1-only

/// @file callstream.cpp
/// @brief See callstream.hpp.

#include "runtime/diagnostics/callstream.hpp"

#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vitreo::diagnostics {
namespace {

struct Recorder {
    std::mutex mu;
    std::FILE* out = nullptr;
    bool tried = false;
    u64 seq = 0;
    u64 image_base = 0;
    u64 image_size = 0;
    // Thread ids and handle-shaped values are reported as first-seen ordinals:
    // the raw numbers differ every run and between hosts, so comparing them
    // would drown the diff in noise that means nothing.
    std::unordered_map<u32, u32> tid_ordinal;
    std::unordered_map<u64, u32> handle_ordinal;
};

Recorder& rec() {
    static Recorder r;
    return r;
}

std::FILE* stream() {
    Recorder& r = rec();
    if (!r.tried) {
        r.tried = true;
        if (const char* path = std::getenv("VITREO_CALLSTREAM");
            path != nullptr && path[0] != '\0') {
            // One file PER PROCESS. Every guest process in the tree inherits
            // the same VITREO_CALLSTREAM, and they all used to open that one
            // path with "w", so a multi-process guest (a launcher plus its
            // helpers) left whichever process wrote last, and a diff against
            // it silently compared two different programs.
            // That is precisely the blind spot this instrument exists to
            // avoid, so the pid is not optional.
            char with_pid[1200];
            std::snprintf(with_pid, sizeof with_pid, "%s.%d", path,
                          static_cast<int>(getpid()));
            r.out = std::fopen(with_pid, "w");
        }
    }
    return r.out;
}

template <typename K>
u32 ordinal_for(std::unordered_map<K, u32>& table, K raw) {
    auto it = table.find(raw);
    if (it != table.end()) return it->second;
    const u32 next = static_cast<u32>(table.size()) + 1;
    table.emplace(raw, next);
    return next;
}

/// Does this word look like a Windows HANDLE the guest has been given before?
bool looks_like_known_handle(const Recorder& r, u64 v) {
    return r.handle_ordinal.find(v) != r.handle_ordinal.end();
}

/// Read a short guest string, if the pointer addresses one. Strings are the
/// single most useful argument to see in a diff -- a path or a class name
/// identifies the call far better than an address does.
bool short_string_at(GuestMemory& mem, u64 addr, std::string& out, bool& wide) {
    if (addr < 0x1000) return false;
    // ANSI first: printable run terminated inside 96 bytes.
    std::string ansi;
    for (u32 i = 0; i < 260; ++i) {
        auto b = mem.read_u8(addr + i);
        if (b.is_err()) return false;
        const u8 c = b.value();
        if (c == 0) {
            if (ansi.size() >= 2) { out = ansi; wide = false; return true; }
            break;
        }
        if (c < 0x20 || c > 0x7E) break;
        ansi.push_back(static_cast<char>(c));
    }
    // UTF-16: ASCII characters with zero high bytes.
    std::string w;
    for (u32 i = 0; i < 260; ++i) {
        auto u = mem.read_u16(addr + i * 2);
        if (u.is_err()) return false;
        const u16 c = u.value();
        if (c == 0) {
            if (w.size() >= 2) { out = w; wide = true; return true; }
            return false;
        }
        if (c < 0x20 || c > 0x7E) return false;
        w.push_back(static_cast<char>(c));
    }
    return false;
}

/// Classify one argument word into a form that is stable across runs.
///
/// Absolute addresses are useless in a diff: they move with every allocation
/// and differ between execution backends. What IS comparable is the
/// *kind* of thing an argument is, plus its content when that content is
/// meaningful (a small integer, a string, a known handle).
std::string classify(Recorder& r, GuestMemory& mem, const cpu::CpuState& cpu,
                     u64 v) {
    char buf[560];
    if (v == 0) return "0";
    if (looks_like_known_handle(r, v)) {
        std::snprintf(buf, sizeof buf, "H%u", r.handle_ordinal[v]);
        return buf;
    }
    // Small values are almost always flags, counts or enum members, and their
    // exact value matters.
    if (v < 0x10000u) {
        std::snprintf(buf, sizeof buf, "%u", v);
        return buf;
    }
    if (r.image_size != 0 && v >= r.image_base && v < r.image_base + r.image_size) {
        std::snprintf(buf, sizeof buf, "img+%llx",
                      static_cast<unsigned long long>(v - r.image_base));
        return buf;
    }
    // Try the string FIRST, before deciding the argument is "just a stack
    // pointer". Programs build paths in stack buffers constantly, and a path
    // is the identity of the call. CreateFileW("...\\settings.ini") says what
    // happened; "stk" says nothing. Classifying by region first threw that
    // away and made four consecutive .ini opens indistinguishable.
    std::string text;
    bool wide = false;
    if (short_string_at(mem, v, text, wide)) {
        std::snprintf(buf, sizeof buf, "%s\"%s\"", wide ? "L" : "", text.c_str());
        return buf;
    }
    const u64 sp = cpu.gpr.rsp;
    if (sp != 0 && v > sp - 0x10000 && v < sp + 0x10000) {
        return "stk";
    }
    // A readable address with no other interpretation: report that it is a
    // valid pointer, not where it happens to point.
    if (mem.read_u8(v).is_ok()) return "ptr";
    std::snprintf(buf, sizeof buf, "%#x", v);
    return buf;
}

} // namespace

namespace detail {

// Definition for the header's inline gate.
std::atomic<int> g_callstream_state{-1};

[[gnu::noinline]] bool callstream_init_slow() noexcept {
    const bool on = (stream() != nullptr);
    g_callstream_state.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

}  // namespace detail

bool callstream_keep_fastpath() noexcept {
    static const bool keep = [] {
        const char* e = std::getenv("VITREO_CALLSTREAM_KEEP_FASTPATH");
        return e != nullptr && e[0] == '1';
    }();
    return keep;
}

void callstream_set_image_range(u64 base, u64 size) noexcept {
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    r.image_base = base;
    r.image_size = size;
}

void callstream_record(const std::string& name, u32 tid,
                       const u64* args, u32 argc, u64 retval,
                       GuestMemory& memory, const cpu::CpuState& cpu,
                       u64 caller) {
    std::FILE* f = stream();
    if (f == nullptr) return;
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);

    std::string line;
    line.reserve(560);
    char head[64];
    std::snprintf(head, sizeof head, "%llu T%u ",
                  static_cast<unsigned long long>(r.seq++),
                  ordinal_for(r.tid_ordinal, tid));
    line += head;
    line += name;
    line += '(';
    for (u32 i = 0; i < argc && i < 12; ++i) {
        if (i != 0) line += ',';
        line += classify(r, memory, cpu, args[i]);
    }
    line += ")=";

    // A non-trivial return that is not obviously a number is very likely a
    // handle; remember it so later calls that pass it back are recognisable.
    // Compared against the 32-bit sentinel too, because a 64-bit guest still
    // returns INVALID_HANDLE_VALUE sign-extended.
    if (retval > 0x10000u && retval != 0xFFFFFFFFu &&
        retval != 0xFFFFFFFFFFFFFFFFull &&
        r.handle_ordinal.find(retval) == r.handle_ordinal.end() &&
        memory.read_u8(retval).is_err()) {
        (void)ordinal_for(r.handle_ordinal, retval);
    }
    line += classify(r, memory, cpu, retval);
    if (caller != 0) {
        char cb[48];
        if (r.image_size != 0 && caller >= r.image_base &&
            caller < r.image_base + r.image_size) {
            std::snprintf(cb, sizeof cb, " @img+%llx",
                          static_cast<unsigned long long>(caller - r.image_base));
        } else {
            std::snprintf(cb, sizeof cb, " @%#llx",
                          static_cast<unsigned long long>(caller));
        }
        line += cb;
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), f);

    // Flush every record. The whole value of this stream is finding the FIRST
    // divergence, and the runs worth diffing are the ones that crash -- where
    // anything still sitting in the stdio buffer is lost, and the lost part is
    // exactly the tail that matters. A run that ends in a guest fault used to
    // leave an empty file. The cost is irrelevant: the recorder is off unless
    // VITREO_CALLSTREAM names a path.
    std::fflush(f);
}

void callstream_close() noexcept {
    // The cached gate must follow the stream: after a close, callstream_record
    // is a no-op, and callstream_enabled() has to say so too.
    detail::g_callstream_state.store(0, std::memory_order_relaxed);
    Recorder& r = rec();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.out != nullptr) {
        std::fclose(r.out);
        r.out = nullptr;
    }
}

} // namespace vitreo::diagnostics
