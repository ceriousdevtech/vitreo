// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle_client.hpp"
#include "oracle_bench_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

// Apple-private flag that disables ASLR for the spawned process. This is
// essential for the oracle runner: our fuzz snippets reference scratch-page
// addresses by literal value, so the runner's MAP_FIXED at those VAs must
// succeed reliably across invocations. Without this flag, Rosetta's dyld
// randomization sometimes places libraries on top of the chosen page.
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif

namespace vitreo::oracle {

namespace {

#ifndef VITREO_ORACLE_RUNNER_PATH
#define VITREO_ORACLE_RUNNER_PATH "/tmp/x86_oracle_runner"
#endif

const char* runner_path() {
    if (const char* env = std::getenv("VITREO_ORACLE_RUNNER")) return env;
    return VITREO_ORACLE_RUNNER_PATH;
}

bool write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n) {
        ssize_t r = ::write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool read_all(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

}  // namespace

bool available() {
    const char* path = runner_path();
    if (::access(path, X_OK) != 0) return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);

    pid_t pid = -1;
    char* const argv[] = { const_cast<char*>(path), nullptr };
    int spawn_rc = ::posix_spawn(&pid, path, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) return false;

    int wstatus = 0;
    (void)::waitpid(pid, &wstatus, 0);
    return true;
}

bool execute(const State& in, const std::vector<uint8_t>& code, State* out) {
    if (!out) return false;
    if (code.size() > 3800) return false;  // runner caps snippet to leave room for prologue/epilogue

    int to_child[2];
    int from_child[2];
    if (pipe(to_child) != 0) return false;
    if (pipe(from_child) != 0) {
        ::close(to_child[0]); ::close(to_child[1]);
        return false;
    }

    pid_t pid = -1;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attrs);
    posix_spawn_file_actions_adddup2(&actions, to_child[0], 0);
    posix_spawn_file_actions_adddup2(&actions, from_child[1], 1);
    posix_spawn_file_actions_addclose(&actions, to_child[0]);
    posix_spawn_file_actions_addclose(&actions, to_child[1]);
    posix_spawn_file_actions_addclose(&actions, from_child[0]);
    posix_spawn_file_actions_addclose(&actions, from_child[1]);
    posix_spawnattr_setflags(&attrs, _POSIX_SPAWN_DISABLE_ASLR);
    const char* path = runner_path();
    char* const argv[] = { const_cast<char*>(path), nullptr };
    int spawn_rc = ::posix_spawn(&pid, path, &actions, &attrs, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    if (spawn_rc != 0) {
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        return false;
    }

    ::close(to_child[0]);
    ::close(from_child[1]);

    // Write request: State + uint32 code_len + code bytes.
    bool write_ok = write_all(to_child[1], &in, sizeof(in));
    if (write_ok) {
        uint32_t len = static_cast<uint32_t>(code.size());
        write_ok = write_all(to_child[1], &len, sizeof(len));
    }
    if (write_ok && !code.empty()) {
        write_ok = write_all(to_child[1], code.data(), code.size());
    }
    ::close(to_child[1]);

    // Read response
    State resp{};
    bool read_ok = write_ok && read_all(from_child[0], &resp, sizeof(resp));
    ::close(from_child[0]);

    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);

    if (!read_ok) return false;
    *out = resp;
    return true;
}

std::string diff(const State& got, const State& want) {
    std::ostringstream os;
    const bool w64 = got.is64() || want.is64();
    static const char* gpr32_names[16] = {
        "EAX","ECX","EDX","EBX","ESP","EBP","ESI","EDI",
        "R8D","R9D","R10D","R11D","R12D","R13D","R14D","R15D"};
    static const char* gpr64_names[16] = {
        "RAX","RCX","RDX","RBX","RSP","RBP","RSI","RDI",
        "R8","R9","R10","R11","R12","R13","R14","R15"};
    const int ngpr = w64 ? 16 : 8;
    for (int i = 0; i < ngpr; ++i) {
        if (got.gpr[i] != want.gpr[i]) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "  %s: got=%016llx want=%016llx\n",
                          (w64 ? gpr64_names : gpr32_names)[i],
                          static_cast<unsigned long long>(got.gpr[i]),
                          static_cast<unsigned long long>(want.gpr[i]));
            os << buf;
        }
    }
    if (got.rflags != want.rflags) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "  RFLAGS: got=%016llx want=%016llx (xor=%016llx)\n",
                      static_cast<unsigned long long>(got.rflags),
                      static_cast<unsigned long long>(want.rflags),
                      static_cast<unsigned long long>(got.rflags ^ want.rflags));
        os << buf;
    }
    if (got.mxcsr != want.mxcsr) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "  MXCSR: got=%08x want=%08x\n",
                      got.mxcsr, want.mxcsr);
        os << buf;
    }
    for (int r = 0; r < (w64 ? 16 : 8); ++r) {
        if (got.xmm[r] != want.xmm[r]) {
            os << "  XMM" << r << ": got=";
            char hex[3];
            for (int b = 15; b >= 0; --b) {
                std::snprintf(hex, sizeof(hex), "%02x", got.xmm[r][b]);
                os << hex;
            }
            os << " want=";
            for (int b = 15; b >= 0; --b) {
                std::snprintf(hex, sizeof(hex), "%02x", want.xmm[r][b]);
                os << hex;
            }
            os << "\n";
        }
    }
    if (got.status != want.status) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "  status: got=%u want=%u\n",
                      got.status, want.status);
        os << buf;
    }
    return os.str();
}

bool BenchSession::write_cmd(uint32_t cmd, uint32_t n) {
    if (m_to < 0) return false;
    OracleBenchCmd hdr{cmd, n};
    return write_all(m_to, &hdr, sizeof(hdr));
}

bool BenchSession::open() {
    close();
    int to_child[2];
    int from_child[2];
    if (pipe(to_child) != 0) return false;
    if (pipe(from_child) != 0) {
        ::close(to_child[0]); ::close(to_child[1]);
        return false;
    }

    pid_t pid = -1;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attrs);
    posix_spawn_file_actions_adddup2(&actions, to_child[0], 0);
    posix_spawn_file_actions_adddup2(&actions, from_child[1], 1);
    posix_spawn_file_actions_addclose(&actions, to_child[0]);
    posix_spawn_file_actions_addclose(&actions, to_child[1]);
    posix_spawn_file_actions_addclose(&actions, from_child[0]);
    posix_spawn_file_actions_addclose(&actions, from_child[1]);
    posix_spawnattr_setflags(&attrs, _POSIX_SPAWN_DISABLE_ASLR);
    const char* path = runner_path();
    char bench[] = "--bench";
    char* const argv[] = { const_cast<char*>(path), bench, nullptr };
    int spawn_rc = ::posix_spawn(&pid, path, &actions, &attrs, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    if (spawn_rc != 0) {
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        return false;
    }
    ::close(to_child[0]);
    ::close(from_child[1]);
    m_pid = pid;
    m_to = to_child[1];
    m_from = from_child[0];
    return true;
}

void BenchSession::close() {
    if (m_to >= 0) {
        (void)write_cmd(ORACLE_BENCH_CMD_QUIT, 0);
        ::close(m_to);
        m_to = -1;
    }
    if (m_from >= 0) {
        ::close(m_from);
        m_from = -1;
    }
    if (m_pid > 0) {
        int wstatus = 0;
        (void)::waitpid(m_pid, &wstatus, 0);
        m_pid = -1;
    }
}

bool BenchSession::load(const State& in, const std::vector<uint8_t>& code) {
    if (m_pid <= 0 && !open()) return false;
    if (code.size() > 3800) return false;
    if (!write_cmd(ORACLE_BENCH_CMD_LOAD, 0)) return false;
    if (!write_all(m_to, &in, sizeof(in))) return false;
    uint32_t len = static_cast<uint32_t>(code.size());
    if (!write_all(m_to, &len, sizeof(len))) return false;
    if (!code.empty() && !write_all(m_to, code.data(), code.size())) return false;
    uint32_t status = 6;
    if (!read_all(m_from, &status, sizeof(status))) return false;
    return status == 0;
}

bool BenchSession::execute_once(State* out) {
    if (!out || m_to < 0) return false;
    if (!write_cmd(ORACLE_BENCH_CMD_ONCE, 0)) return false;
    return read_all(m_from, out, sizeof(*out));
}

bool BenchSession::warmup(uint32_t n) {
    if (m_to < 0) return false;
    if (!write_cmd(ORACLE_BENCH_CMD_WARMUP, n)) return false;
    uint32_t status = 6;
    if (!read_all(m_from, &status, sizeof(status))) return false;
    return status == 0;
}

bool BenchSession::run(uint32_t n) {
    if (m_to < 0) return false;
    if (!write_cmd(ORACLE_BENCH_CMD_RUN, n)) return false;
    uint32_t status = 6;
    if (!read_all(m_from, &status, sizeof(status))) return false;
    return status == 0;
}

bool BenchSession::times(BenchTimes* out) {
    if (!out || m_to < 0) return false;
    if (!write_cmd(ORACLE_BENCH_CMD_TIMES, 0)) return false;
    OracleBenchTimes wire{};
    if (!read_all(m_from, &wire, sizeof(wire))) return false;
    out->warmup_ns = wire.warmup_ns;
    out->run_ns = wire.run_ns;
    out->iters = wire.iters;
    out->status = wire.status;
    out->reserved = wire.reserved;
    return true;
}

}  // namespace vitreo::oracle
