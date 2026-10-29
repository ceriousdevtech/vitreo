// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle/cpu_perf_oracle.hpp"
#include "oracle/cpu_perf_windows_protocol.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef VITREO_CPU_PERF_WIN_RUNNER
#define VITREO_CPU_PERF_WIN_RUNNER ""
#endif
#ifndef VITREO_CPU_PERF_WIN64_RUNNER
#define VITREO_CPU_PERF_WIN64_RUNNER ""
#endif
#ifndef VITREO_ORACLE_SSH_EXE
#define VITREO_ORACLE_SSH_EXE "ssh"
#endif
#ifndef VITREO_ORACLE_SCP_EXE
#define VITREO_ORACLE_SCP_EXE "scp"
#endif
#ifndef VITREO_ORACLE_WINDOWS_HOST
#define VITREO_ORACLE_WINDOWS_HOST ""
#endif

extern char** environ;

namespace vitreo::oracle::perf {
namespace {

constexpr const char* kRemoteRunner32 =
    "C:\\oracle\\bin\\cpu_perf_windows_runner_x86.exe";
constexpr const char* kRemoteRunner64 =
    "C:\\oracle\\bin\\cpu_perf_windows_runner_x64.exe";

[[nodiscard]] std::string quote(const std::string& value) {
    std::string result{"'"};
    for (const char ch : value) {
        if (ch == '\'') {
            result += "'\\''";
        } else {
            result += ch;
        }
    }
    result += '\'';
    return result;
}

[[nodiscard]] int run_quiet(const std::string& command) {
    return std::system((command + " >/dev/null 2>&1").c_str());
}

[[nodiscard]] bool read_exact(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, p + got, n - got);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

[[nodiscard]] bool write_exact(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t put = 0;
    while (put < n) {
        const ssize_t w = ::write(fd, p + put, n - put);
        if (w > 0) {
            put += static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

class Session {
public:
    Session(uint32_t arch_bits, const char* remote_runner)
        : arch_bits_(arch_bits), remote_runner_(remote_runner) {}

    ~Session() { close(); }

    [[nodiscard]] bool open(std::string& error) {
        if (pid_ > 0) return true;

        std::signal(SIGPIPE, SIG_IGN);

        int in_pipe[2];
        int out_pipe[2];
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
            error = "pipe failed";
            return false;
        }

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
        posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
        posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
        posix_spawn_file_actions_addclose(&fa, out_pipe[1]);

        std::vector<std::string> args;
        args.emplace_back(VITREO_ORACLE_SSH_EXE);
        args.emplace_back("-T");
        args.emplace_back("-o");
        args.emplace_back("BatchMode=yes");
        args.emplace_back("-o");
        args.emplace_back("ConnectTimeout=8");
        args.emplace_back(VITREO_ORACLE_WINDOWS_HOST);
        args.emplace_back(remote_runner_);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& s : args) argv.push_back(s.data());
        argv.push_back(nullptr);

        pid_t pid = -1;
        const int rc =
            posix_spawnp(&pid, args[0].c_str(), &fa, nullptr, argv.data(),
                         environ);
        posix_spawn_file_actions_destroy(&fa);
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        if (rc != 0) {
            ::close(in_pipe[1]);
            ::close(out_pipe[0]);
            error = "ssh spawn failed";
            return false;
        }
        pid_ = pid;
        write_fd_ = in_pipe[1];
        read_fd_ = out_pipe[0];

        CpuPerfWinHello hello{};
        if (!call(CPU_PERF_WIN_OP_HELLO, nullptr, 0, &hello, sizeof(hello),
                  error)) {
            close();
            return false;
        }
        if (hello.version != CPU_PERF_WIN_VERSION ||
            hello.arch_bits != arch_bits_) {
            error = "windows runner protocol/architecture mismatch";
            close();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool bench(const std::vector<uint8_t>& body, const State& in,
                             uint32_t warmup, uint32_t iters,
                             BenchTimes& out, Windows64BenchResult* observed,
                             std::string& error) {
        if ((body.empty() && arch_bits_ != CPU_PERF_WIN_ARCH_X64) ||
            body.size() > CPU_PERF_WIN_MAX_CODE) {
            error = "windows body size out of range";
            return false;
        }
        const size_t request_size = arch_bits_ == CPU_PERF_WIN_ARCH_X64
                                        ? sizeof(CpuPerfWinBenchReq64)
                                        : sizeof(CpuPerfWinBenchReq);
        std::vector<uint8_t> payload(request_size + body.size());
        if (arch_bits_ == CPU_PERF_WIN_ARCH_X64) {
            CpuPerfWinBenchReq64 req{};
            req.code_len = static_cast<uint32_t>(body.size());
            req.warmup = warmup;
            req.iters = iters;
            std::memcpy(req.gpr, in.gpr.data(), sizeof(req.gpr));
            req.rflags = in.rflags;
            req.mxcsr = in.mxcsr;
            std::memcpy(req.xmm, in.xmm.data(), sizeof(req.xmm));
            req.fcw = in.fcw;
            req.fsw = in.fsw;
            req.ftw = in.ftw;
            std::memcpy(req.st, in.st.data(), sizeof(req.st));
            std::memcpy(req.mem, in.mem.data(),
                        std::min(in.mem.size(),
                                 static_cast<size_t>(CPU_PERF_WIN_MEM)));
            std::memcpy(payload.data(), &req, sizeof(req));
        } else {
            CpuPerfWinBenchReq req{};
            req.code_len = static_cast<uint32_t>(body.size());
            req.warmup = warmup;
            req.iters = iters;
            for (int i = 0; i < 8; ++i) {
                req.gpr[static_cast<size_t>(i)] =
                    static_cast<uint32_t>(in.gpr[static_cast<size_t>(i)]);
            }
            std::memcpy(req.mem, in.mem.data(),
                        std::min(in.mem.size(),
                                 static_cast<size_t>(CPU_PERF_WIN_MEM)));
            std::memcpy(payload.data(), &req, sizeof(req));
        }
        if (!body.empty()) {
            std::memcpy(payload.data() + request_size, body.data(), body.size());
        }

        CpuPerfWinBenchResp timing{};
        if (arch_bits_ == CPU_PERF_WIN_ARCH_X64) {
            CpuPerfWinBenchResp64 resp{};
            if (!call(CPU_PERF_WIN_OP_BENCH, payload.data(),
                      static_cast<uint32_t>(payload.size()), &resp,
                      sizeof(resp), error)) {
                return false;
            }
            timing = resp.timing;
            if (resp.observed.parameter_count > 3u) {
                error = "windows x64 invalid fault parameter count";
                return false;
            }
            if (observed != nullptr) {
                std::memcpy(observed->gpr.data(), resp.observed.gpr,
                            sizeof(resp.observed.gpr));
                observed->rflags = resp.observed.rflags;
                observed->rip = resp.observed.rip;
                observed->initial_rsp = resp.observed.initial_rsp;
                observed->code_base = resp.observed.code_base;
                observed->mxcsr = resp.observed.mxcsr;
                std::memcpy(observed->xmm.data(), resp.observed.xmm,
                            sizeof(resp.observed.xmm));
                observed->fcw = resp.observed.fcw;
                observed->fsw = resp.observed.fsw;
                observed->ftw = resp.observed.ftw;
                std::memcpy(observed->st.data(), resp.observed.st,
                            sizeof(resp.observed.st));
                std::memcpy(observed->mem.data(), resp.observed.mem,
                            sizeof(resp.observed.mem));
                observed->fault.code = resp.observed.fault_code;
                observed->fault.flags = resp.observed.fault_flags;
                observed->fault.parameter_count =
                    resp.observed.parameter_count;
                observed->fault.address = resp.observed.fault_address;
                std::memcpy(observed->fault.information.data(),
                            resp.observed.fault_information,
                            sizeof(resp.observed.fault_information));
            }
        } else {
            if (!call(CPU_PERF_WIN_OP_BENCH, payload.data(),
                      static_cast<uint32_t>(payload.size()), &timing,
                      sizeof(timing), error)) {
                return false;
            }
        }
        out.warmup_ns = timing.warmup_ns;
        out.run_ns = timing.run_ns;
        out.iters = timing.iters;
        out.status = timing.status;
        if (observed != nullptr) {
            observed->times = out;
        }
        return true;
    }

    void close() {
        if (write_fd_ >= 0 && pid_ > 0) {
            CpuPerfWinFrame hdr{};
            hdr.magic = CPU_PERF_WIN_MAGIC;
            hdr.op = CPU_PERF_WIN_OP_QUIT;
            hdr.payload_len = 0;
            (void)write_exact(write_fd_, &hdr, sizeof(hdr));
        }
        if (write_fd_ >= 0) {
            ::close(write_fd_);
            write_fd_ = -1;
        }
        if (read_fd_ >= 0) {
            ::close(read_fd_);
            read_fd_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            (void)waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

private:
    [[nodiscard]] bool call(uint32_t op, const void* req, uint32_t req_len,
                            void* resp, uint32_t resp_len, std::string& error) {
        CpuPerfWinFrame hdr{};
        hdr.magic = CPU_PERF_WIN_MAGIC;
        hdr.op = op;
        hdr.payload_len = req_len;
        if (!write_exact(write_fd_, &hdr, sizeof(hdr)) ||
            (req_len && !write_exact(write_fd_, req, req_len))) {
            error = "windows write failed";
            return false;
        }
        CpuPerfWinFrame rh{};
        if (!read_exact(read_fd_, &rh, sizeof(rh))) {
            error = "windows read failed (runner died?)";
            return false;
        }
        if (rh.magic != CPU_PERF_WIN_MAGIC || rh.op != op) {
            error = "windows bad response frame";
            return false;
        }
        if (rh.payload_len != resp_len) {
            error = "windows payload size mismatch";
            return false;
        }
        if (resp_len && !read_exact(read_fd_, resp, resp_len)) {
            error = "windows short payload";
            return false;
        }
        return true;
    }

    pid_t pid_ = -1;
    int write_fd_ = -1;
    int read_fd_ = -1;
    uint32_t arch_bits_;
    const char* remote_runner_;
};

enum class Ready { Unknown, Yes, No };

Ready g_ready = Ready::Unknown;
std::string g_ready_why;
Ready g_ready64 = Ready::Unknown;
std::string g_ready64_why;
Session g_session{CPU_PERF_WIN_ARCH_X86, kRemoteRunner32};
Session g_session64{CPU_PERF_WIN_ARCH_X64, kRemoteRunner64};
bool g_deployed = false;
bool g_deployed64 = false;

[[nodiscard]] bool runner_exists(const char* path) {
    return path != nullptr && path[0] != '\0' &&
           std::filesystem::exists(path);
}

[[nodiscard]] bool probe_host() {
    std::ostringstream cmd;
    cmd << quote(VITREO_ORACLE_SSH_EXE) << " -o BatchMode=yes "
        << "-o ConnectTimeout=3 -T " << quote(VITREO_ORACLE_WINDOWS_HOST)
        << " echo ok";
    FILE* f = popen(cmd.str().c_str(), "r");
    if (!f) return false;
    char buf[16] = {};
    (void)std::fgets(buf, sizeof(buf), f);
    const int rc = pclose(f);
    return rc == 0 && std::strncmp(buf, "ok", 2) == 0;
}

[[nodiscard]] bool deploy_runner(const char* local_path,
                                 const char* remote_filename,
                                 const char* process_name, bool& deployed,
                                 std::string& error) {
    if (deployed) return true;

    const std::string host = VITREO_ORACLE_WINDOWS_HOST;
    const std::string prepare =
        "New-Item -ItemType Directory -Force -Path "
        "C:\\oracle\\bin,C:\\oracle\\out | Out-Null";
    std::ostringstream mkdir_command;
    mkdir_command << quote(VITREO_ORACLE_SSH_EXE) << " -o BatchMode=yes "
                  << quote(host) << " " << quote(prepare);
    if (run_quiet(mkdir_command.str()) != 0) {
        error = "windows: could not create remote oracle directories";
        return false;
    }

    // Terminate any runner left over from a previous session before copying.
    // The runner is a long-lived process (see PerfSession), so a test run that
    // died without closing its session leaves one holding an open handle on the
    // exe -- and Windows then refuses the overwrite. That presented as
    // "could not deploy", which reads like a permissions or path problem and
    // is neither: the directory, the credentials and the disk were all fine,
    // and copying to any *other* filename succeeded. Best-effort, so a host
    // with no stale process is not an error.
    std::ostringstream kill_command;
    kill_command << quote(VITREO_ORACLE_SSH_EXE) << " -o BatchMode=yes "
                 << quote(host) << " "
                 << quote(std::string("Get-Process -Name ") + process_name +
                          " -ErrorAction SilentlyContinue | "
                          "Stop-Process -Force -ErrorAction SilentlyContinue");
    (void)run_quiet(kill_command.str());

    std::ostringstream copy_command;
    copy_command << quote(VITREO_ORACLE_SCP_EXE) << " -q "
                 << quote(local_path) << " "
                 << quote(std::string(host) + ":C:/oracle/bin/" +
                          remote_filename);
    if (run_quiet(copy_command.str()) != 0) {
        error = std::string("windows: could not deploy ") + remote_filename +
                " "
                "(a running instance may still hold the file open)";
        return false;
    }
    deployed = true;
    return true;
}

}  // namespace

namespace {

[[nodiscard]] bool ensure_windows_ready(
    Ready& ready, std::string& ready_why, Session& session,
    const char* local_runner, const char* remote_filename,
    const char* process_name, bool& deployed, std::string& why) {
    if (ready == Ready::Yes) {
        why.clear();
        return true;
    }
    if (ready == Ready::No) {
        why = ready_why;
        return false;
    }

    if (!runner_exists(local_runner)) {
        ready = Ready::No;
        ready_why = "windows runner not built for requested architecture";
        why = ready_why;
        return false;
    }
    if (!probe_host()) {
        ready = Ready::No;
        ready_why = "Windows SSH host unreachable";
        why = ready_why;
        return false;
    }
    if (!deploy_runner(local_runner, remote_filename, process_name, deployed,
                       ready_why)) {
        ready = Ready::No;
        why = ready_why;
        return false;
    }
    if (!session.open(ready_why)) {
        ready = Ready::No;
        why = ready_why;
        return false;
    }
    ready = Ready::Yes;
    ready_why.clear();
    why.clear();
    return true;
}

template <typename ReadyFn>
[[nodiscard]] bool bench_with_session(
    const std::vector<uint8_t>& body, const State& in, uint32_t warmup,
    uint32_t iters, BenchTimes& out, std::string& error, Ready& ready,
    Session& session, ReadyFn ready_fn, Windows64BenchResult* observed,
    bool accept_fault) {
    std::string why;
    if (!ready_fn(why)) {
        error = why;
        return false;
    }
    auto once = [&](uint32_t n, BenchTimes& times, std::string& err) {
        if (session.bench(body, in, warmup, n, times, observed, err)) return true;
        session.close();
        ready = Ready::Unknown;
        if (!ready_fn(why)) {
            err = why;
            return false;
        }
        return session.bench(body, in, warmup, n, times, observed, err);
    };

    // Native x86 is fast enough that an oracle-sized loop sits inside one
    // QPC tick. Grow the run until it lasts ~1 ms so ns/iter is real.
    uint32_t n = std::max(iters, 1000u);
    if (!once(n, out, error)) return false;
    if (out.status != CPU_PERF_WIN_OK) {
        if (accept_fault && out.status == CPU_PERF_WIN_FAULT) {
            error.clear();
            return true;
        }
        error = "windows status=" + std::to_string(out.status);
        return false;
    }
    constexpr uint64_t kMinRunNs = 1000000ull;
    if (out.run_ns > 0 && out.run_ns < kMinRunNs) {
        const uint64_t scaled =
            std::min<uint64_t>(CPU_PERF_WIN_MAX_ITERS,
                               (kMinRunNs * static_cast<uint64_t>(n) * 2ull) /
                                   out.run_ns);
        if (scaled > n) {
            if (!once(static_cast<uint32_t>(scaled), out, error)) return false;
        }
    }
    if (out.status != CPU_PERF_WIN_OK || out.run_ns == 0 || out.iters == 0) {
        if (accept_fault && out.status == CPU_PERF_WIN_FAULT) {
            error.clear();
            return true;
        }
        error = "windows status=" + std::to_string(out.status);
        return false;
    }
    error.clear();
    return true;
}

}  // namespace

bool windows_ready(std::string& why) {
    return ensure_windows_ready(
        g_ready, g_ready_why, g_session, VITREO_CPU_PERF_WIN_RUNNER,
        "cpu_perf_windows_runner_x86.exe", "cpu_perf_windows_runner_x86",
        g_deployed, why);
}

bool windows64_ready(std::string& why) {
    return ensure_windows_ready(
        g_ready64, g_ready64_why, g_session64,
        VITREO_CPU_PERF_WIN64_RUNNER, "cpu_perf_windows_runner_x64.exe",
        "cpu_perf_windows_runner_x64", g_deployed64, why);
}

bool bench_on_windows(const std::vector<uint8_t>& body, const State& in,
                      uint32_t warmup, uint32_t iters, BenchTimes& out,
                      std::string& error) {
    return bench_with_session(body, in, warmup, iters, out, error, g_ready,
                              g_session, windows_ready, nullptr, false);
}

bool bench_on_windows64(const std::vector<uint8_t>& body, const State& in,
                        uint32_t warmup, uint32_t iters, BenchTimes& out,
                        std::string& error) {
    return bench_with_session(body, in, warmup, iters, out, error, g_ready64,
                              g_session64, windows64_ready, nullptr, false);
}

bool bench_on_windows64_observed(const std::vector<uint8_t>& body,
                                 const State& in, uint32_t warmup,
                                 uint32_t iters, Windows64BenchResult& result,
                                 std::string& error) {
    result = {};
    return bench_with_session(body, in, warmup, iters, result.times, error,
                              g_ready64, g_session64, windows64_ready, &result,
                              true);
}

bool normalize_bench_times(const BenchTimes& raw, const BenchTimes& wrapper,
                           NormalizedNs& result) {
    result = {};
    if (raw.status != CPU_PERF_WIN_OK ||
        wrapper.status != CPU_PERF_WIN_OK || raw.run_ns == 0 ||
        wrapper.run_ns == 0 || raw.iters == 0 || wrapper.iters == 0) {
        return false;
    }
    result.raw_ns_per_iter =
        static_cast<double>(raw.run_ns) / static_cast<double>(raw.iters);
    result.wrapper_ns_per_iter = static_cast<double>(wrapper.run_ns) /
                                 static_cast<double>(wrapper.iters);
    result.net_ns_per_iter =
        result.raw_ns_per_iter - result.wrapper_ns_per_iter;
    return true;
}

bool bench_on_windows64_normalized(
    const std::vector<uint8_t>& body, const State& in, uint32_t warmup,
    uint32_t iters, Windows64NormalizedBenchResult& result,
    std::string& error) {
    result = {};
    if (body.empty() || body.size() > CPU_PERF_WIN_MAX_CODE) {
        error = "windows normalized body size out of range";
        return false;
    }
    std::string why;
    if (!windows64_ready(why)) {
        error = why;
        return false;
    }
    auto exact = [&](const std::vector<uint8_t>& bytes, uint32_t count,
                     Windows64BenchResult& out) {
        if (g_session64.bench(bytes, in, warmup, count, out.times, &out,
                              error)) {
            return true;
        }
        g_session64.close();
        g_ready64 = Ready::Unknown;
        if (!windows64_ready(why)) {
            error = why;
            return false;
        }
        return g_session64.bench(bytes, in, warmup, count, out.times, &out,
                                 error);
    };

    uint32_t count = std::max(iters, 1000u);
    Windows64BenchResult wrapper{};
    if (!exact(body, count, result.raw) || !exact({}, count, wrapper)) {
        return false;
    }
    if (result.raw.times.status != CPU_PERF_WIN_OK ||
        wrapper.times.status != CPU_PERF_WIN_OK ||
        result.raw.times.run_ns == 0 || wrapper.times.run_ns == 0) {
        error = "windows normalized benchmark status failure";
        return false;
    }

    constexpr uint64_t kMinRunNs = 1000000ull;
    const uint64_t shorter =
        std::min(result.raw.times.run_ns, wrapper.times.run_ns);
    if (shorter < kMinRunNs) {
        const uint64_t scaled = std::min<uint64_t>(
            CPU_PERF_WIN_MAX_ITERS,
            (kMinRunNs * static_cast<uint64_t>(count) * 2ull) / shorter);
        if (scaled > count) {
            count = static_cast<uint32_t>(scaled);
            if (!exact(body, count, result.raw) ||
                !exact({}, count, wrapper)) {
                return false;
            }
        }
    }
    result.wrapper = wrapper.times;
    if (result.raw.times.iters != result.wrapper.iters ||
        !normalize_bench_times(result.raw.times, result.wrapper, result.ns)) {
        error = "windows normalized benchmark invalid counters";
        return false;
    }
    error.clear();
    return true;
}

void attach_windows(SnippetBench& row, uint32_t warmup, uint32_t iters) {
    attach_windows(row, warmup, iters, scratch_state());
}

void attach_windows(SnippetBench& row, uint32_t warmup, uint32_t iters,
                    const State& in) {
    row.windows = {};
    row.windows_skip.clear();
    if (!row.skip.empty() || row.body.empty()) return;
    std::string why;
    if (!windows_ready(why)) {
        row.windows_skip = why;
        return;
    }
    if (!bench_on_windows(row.body, in, warmup, iters, row.windows,
                          why)) {
        row.windows_skip = why;
    }
}

}  // namespace vitreo::oracle::perf
