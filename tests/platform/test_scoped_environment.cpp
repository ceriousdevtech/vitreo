// SPDX-License-Identifier: LGPL-2.1-only
//
// VITREO_SCOPED_ENV: a switch meant for one process of a guest tree must either
// reach the knobs that process reads at static initialisation, or say loudly
// that it did not. The defect these tests pin (measured on a Chromium guest,
// 2026-10-23): the variables were setenv()'d when the guest launched, ~290
// knobs had already been read into namespace-scope constants, and every
// process printed "scoped env: VITREO_JIT_MAP=1" while the knob did nothing.
//
// The parsing and delivery rules are tested on their own; the claim itself is
// tested with a real process (scoped_environment_probe.cpp) that latches a knob
// at static initialisation the way jit_finalize.cpp does.

#include "platform/scoped_environment.hpp"

#include <gtest/gtest.h>

#include <crt_externs.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::platform {
namespace {

constexpr const char* kKnob = "VITREO_SCOPED_ENV_PROBE_KNOB";

std::optional<ScopedEnvironment> parse(std::string_view text) {
    std::string error;
    return parse_scoped_environment(text, error);
}

std::string parse_error(std::string_view text) {
    std::string error;
    EXPECT_FALSE(parse_scoped_environment(text, error).has_value());
    return error;
}

/// Sets (or clears) a variable for one scope.
class EnvGuard {
public:
    EnvGuard(const char* name, const char* value) : m_name(name) {
        if (const char* old = std::getenv(name)) m_old = old;
        if (value != nullptr) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~EnvGuard() {
        if (m_old) {
            ::setenv(m_name.c_str(), m_old->c_str(), 1);
        } else {
            ::unsetenv(m_name.c_str());
        }
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    std::string m_name;
    std::optional<std::string> m_old;
};

/// What `delivery` writes to its log.
template <typename Fn>
std::string captured(Fn&& delivery) {
    std::FILE* log = std::tmpfile();
    EXPECT_NE(log, nullptr);
    if (log == nullptr) return {};
    delivery(log);
    std::fflush(log);
    std::rewind(log);
    std::string text;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), log) != nullptr) text += buffer;
    std::fclose(log);
    return text;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t found = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++found;
    }
    return found;
}

// ---------------------------------------------------------------------------
// The directive
// ---------------------------------------------------------------------------

TEST(ScopedEnvironmentParse, SubstringAndOneAssignment) {
    const auto directive = parse("renderer=>VITREO_JIT_MAP=1");
    ASSERT_TRUE(directive.has_value());
    EXPECT_EQ(directive->needle, "renderer");
    ASSERT_EQ(directive->assignments.size(), 1u);
    EXPECT_EQ(directive->assignments[0], (ScopedEnvAssignment{"VITREO_JIT_MAP", "1"}));
    EXPECT_TRUE(directive->rejected_items.empty());
}

TEST(ScopedEnvironmentParse, SeveralAssignmentsKeepTheirOrder) {
    const auto directive = parse("--type=gpu=>A=1,B=two,C=");
    ASSERT_TRUE(directive.has_value());
    // The substring is everything before the FIRST "=>"; the substring may
    // itself hold '='.
    EXPECT_EQ(directive->needle, "--type=gpu");
    ASSERT_EQ(directive->assignments.size(), 3u);
    EXPECT_EQ(directive->assignments[0], (ScopedEnvAssignment{"A", "1"}));
    EXPECT_EQ(directive->assignments[1], (ScopedEnvAssignment{"B", "two"}));
    EXPECT_EQ(directive->assignments[2], (ScopedEnvAssignment{"C", ""}));
}

TEST(ScopedEnvironmentParse, ValueMayContainEqualsButNotComma) {
    const auto directive = parse("x=>FLAGS=a=b=c,NEXT=1");
    ASSERT_TRUE(directive.has_value());
    ASSERT_EQ(directive->assignments.size(), 2u);
    EXPECT_EQ(directive->assignments[0], (ScopedEnvAssignment{"FLAGS", "a=b=c"}));
    EXPECT_EQ(directive->assignments[1], (ScopedEnvAssignment{"NEXT", "1"}));
}

TEST(ScopedEnvironmentParse, EmptyItemsAreSkippedAndRepeatedNamesTakeTheLastValue) {
    const auto directive = parse("x=>A=1,,B=2,A=3,");
    ASSERT_TRUE(directive.has_value());
    ASSERT_EQ(directive->assignments.size(), 2u);
    EXPECT_EQ(directive->assignments[0], (ScopedEnvAssignment{"A", "3"}));
    EXPECT_EQ(directive->assignments[1], (ScopedEnvAssignment{"B", "2"}));
    EXPECT_TRUE(directive->rejected_items.empty());
}

TEST(ScopedEnvironmentParse, ItemsThatAreNotAssignmentsAreReportedNotDropped) {
    const auto directive = parse("x=>A=1,JUSTANAME,=novalue,B=2");
    ASSERT_TRUE(directive.has_value());
    EXPECT_EQ(directive->assignments.size(), 2u);
    EXPECT_EQ(directive->rejected_items,
              (std::vector<std::string>{"JUSTANAME", "=novalue"}));
}

TEST(ScopedEnvironmentParse, MalformedDirectivesNameTheirDefect) {
    EXPECT_NE(parse_error("no arrow here").find("'=>'"), std::string::npos);
    EXPECT_NE(parse_error("=>A=1").find("substring"), std::string::npos);
    EXPECT_NE(parse_error("x=>").find("NAME=VALUE"), std::string::npos);
    EXPECT_NE(parse_error("x=>JUSTANAME").find("JUSTANAME"), std::string::npos);
}

TEST(ScopedEnvironmentMatch, SubstringOfTheGuestCommandLine) {
    const auto directive = parse("--type=renderer=>A=1");
    ASSERT_TRUE(directive.has_value());
    EXPECT_TRUE(scoped_environment_matches(
        *directive, "\"C:\\App\\host.exe\" --type=renderer --lang=en"));
    EXPECT_FALSE(scoped_environment_matches(
        *directive, "\"C:\\App\\host.exe\" --type=gpu-process"));
    EXPECT_FALSE(scoped_environment_matches(*directive, ""));
}

// ---------------------------------------------------------------------------
// The environment a re-exec builds
// ---------------------------------------------------------------------------

TEST(ScopedEnvironmentBuild, ReplacesHeldNamesAddsNewOnesAndMarksThePid) {
    const std::vector<std::string> before = {
        "PATH=/bin", "A=old", "KEEP=1", "VITREO_SCOPED_ENV_REEXEC=77", "Z=z"};
    const auto after = environment_with_scoped_assignments(
        before, {{"A", "new"}, {"B", "added"}}, 1234);

    // No name appears twice (lookup order for duplicates is implementation
    // defined), unrelated entries keep their order, and a stale marker from an
    // ancestor is replaced by this pid's.
    std::map<std::string, int> seen;
    for (const auto& entry : after) ++seen[entry.substr(0, entry.find('='))];
    for (const auto& [name, times] : seen) EXPECT_EQ(times, 1) << name;
    EXPECT_EQ(after.front(), "PATH=/bin");
    EXPECT_EQ(after[1], "KEEP=1");
    EXPECT_EQ(after[2], "Z=z");
    EXPECT_NE(std::find(after.begin(), after.end(), "A=new"), after.end());
    EXPECT_NE(std::find(after.begin(), after.end(), "B=added"), after.end());
    EXPECT_NE(std::find(after.begin(), after.end(), "VITREO_SCOPED_ENV_REEXEC=1234"),
              after.end());
}

TEST(ScopedEnvironmentMissing, OnlyWhatTheEnvironmentLacksOrHoldsDifferently) {
    EnvGuard set_held("VITREO_SCOPED_TEST_HELD", "1");
    EnvGuard set_other("VITREO_SCOPED_TEST_OTHER", "old");
    EnvGuard set_unset("VITREO_SCOPED_TEST_UNSET", nullptr);
    const auto directive = parse(
        "x=>VITREO_SCOPED_TEST_HELD=1,VITREO_SCOPED_TEST_OTHER=new,"
        "VITREO_SCOPED_TEST_UNSET=1");
    ASSERT_TRUE(directive.has_value());
    const auto missing = scoped_environment_missing(*directive);
    ASSERT_EQ(missing.size(), 2u);
    EXPECT_EQ(missing[0].name, "VITREO_SCOPED_TEST_OTHER");
    EXPECT_EQ(missing[1].name, "VITREO_SCOPED_TEST_UNSET");
}

// ---------------------------------------------------------------------------
// What delivery tells the person reading the log
// ---------------------------------------------------------------------------

TEST(ScopedEnvironmentDeliver, ALateValueIsAppliedAndSaysKnobsAtStartupDidNotSeeIt) {
    EnvGuard clear(kKnob, nullptr);
    const auto directive = parse(std::string("x=>") + kKnob + "=on");
    ASSERT_TRUE(directive.has_value());
    const ScopedEnvStartup startup;  // nothing was held at image start

    const std::string log = captured(
        [&](std::FILE* out) { deliver_scoped_environment(*directive, startup, out); });

    EXPECT_TRUE(contains(log, std::string("[process] scoped env: ") + kKnob + "=on LATE"))
        << log;
    EXPECT_TRUE(contains(log, "did NOT see it")) << log;
    // The success wording must not appear for a value the knobs missed.
    EXPECT_FALSE(contains(log, "before static initialisation")) << log;
    // It is still applied, for the knobs read lazily.
    ASSERT_NE(std::getenv(kKnob), nullptr);
    EXPECT_STREQ(std::getenv(kKnob), "on");
}

TEST(ScopedEnvironmentDeliver, AHeldValueIsReportedAsSeenByEveryKnob) {
    EnvGuard set(kKnob, "on");
    const auto directive = parse(std::string("x=>") + kKnob + "=on");
    ASSERT_TRUE(directive.has_value());
    ScopedEnvStartup startup;
    startup.held = directive->assignments;
    startup.reexeced = true;

    const std::string log = captured(
        [&](std::FILE* out) { deliver_scoped_environment(*directive, startup, out); });
    EXPECT_TRUE(contains(log, std::string("[process] scoped env: ") + kKnob + "=on"))
        << log;
    EXPECT_TRUE(contains(log, "re-executed with it")) << log;
    EXPECT_FALSE(contains(log, "LATE")) << log;

    startup.reexeced = false;  // the whole tree already had it
    const std::string inherited = captured(
        [&](std::FILE* out) { deliver_scoped_environment(*directive, startup, out); });
    EXPECT_TRUE(contains(inherited, "already in the environment at process start"))
        << inherited;
    EXPECT_FALSE(contains(inherited, "LATE")) << inherited;
}

TEST(ScopedEnvironmentDeliver, AFailedReexecIsNamedAsTheReason) {
    EnvGuard clear(kKnob, nullptr);
    const auto directive = parse(std::string("x=>") + kKnob + "=on");
    ASSERT_TRUE(directive.has_value());
    ScopedEnvStartup startup;
    startup.reexec_errno = ENOENT;

    const std::string log = captured(
        [&](std::FILE* out) { deliver_scoped_environment(*directive, startup, out); });
    EXPECT_TRUE(contains(log, "re-exec failed")) << log;
    EXPECT_TRUE(contains(log, "LATE")) << log;
}

TEST(ScopedEnvironmentDeliver, ItemsThatAreNotAssignmentsAreIgnoredLoudly) {
    EnvGuard clear(kKnob, nullptr);
    const auto directive = parse(std::string("x=>JUSTANAME,") + kKnob + "=on");
    ASSERT_TRUE(directive.has_value());
    const ScopedEnvStartup startup;

    const std::string log = captured(
        [&](std::FILE* out) { deliver_scoped_environment(*directive, startup, out); });
    EXPECT_TRUE(contains(log, "'JUSTANAME' IGNORED")) << log;
}

TEST(ScopedEnvironmentDeliverForGuest, SilentWhenUnsetOrNotSelected) {
    {
        EnvGuard unset(kScopedEnvironmentVariable, nullptr);
        EXPECT_EQ(captured([&](std::FILE* out) {
                      deliver_scoped_environment_for_guest("anything", out);
                  }),
                  "");
    }
    {
        EnvGuard other(kScopedEnvironmentVariable,
                       (std::string("selected=>") + kKnob + "=on").c_str());
        EnvGuard clear(kKnob, nullptr);
        EXPECT_EQ(captured([&](std::FILE* out) {
                      deliver_scoped_environment_for_guest("not this one", out);
                  }),
                  "");
        EXPECT_EQ(std::getenv(kKnob), nullptr);
    }
}

TEST(ScopedEnvironmentDeliverForGuest, AMalformedDirectiveIsReportedNotSwallowed) {
    // "A directive that selects nothing reads as a clean negative" is the
    // failure being removed; a typo must not be silent either.
    EnvGuard bad(kScopedEnvironmentVariable, "renderer VITREO_JIT_MAP=1");
    const std::string log = captured([&](std::FILE* out) {
        deliver_scoped_environment_for_guest("renderer", out);
    });
    EXPECT_TRUE(contains(log, "VITREO_SCOPED_ENV IGNORED")) << log;
    EXPECT_TRUE(contains(log, "'=>'")) << log;
}

// ---------------------------------------------------------------------------
// A real process: a knob latched at static initialisation
// ---------------------------------------------------------------------------

struct ProbeRun {
    pid_t spawned_pid = -1;
    std::string output;
    int exit_status = -1;
    bool timed_out = false;

    /// The `key=value` line of the probe's own report.
    std::string value(std::string_view key) const {
        const std::string wanted = std::string(key) + "=";
        std::size_t at = 0;
        while (at < output.size()) {
            const std::size_t end = output.find('\n', at);
            const std::string line =
                output.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (line.rfind(wanted, 0) == 0) return line.substr(wanted.size());
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return "<absent>";
    }
};

/// Runs the probe with exactly `environment` and returns its stdout+stderr.
ProbeRun run_probe(const std::string& guest_command_line,
                   const std::vector<std::string>& probe_flags,
                   const std::vector<std::string>& environment) {
    ProbeRun result;
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        ADD_FAILURE() << "pipe failed";
        return result;
    }

    std::vector<std::string> arguments = {VITREO_SCOPED_ENV_PROBE_PATH,
                                          guest_command_line};
    arguments.insert(arguments.end(), probe_flags.begin(), probe_flags.end());
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_storage = environment;
    std::vector<char*> envp;
    for (auto& entry : env_storage) envp.push_back(entry.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    const int rc = ::posix_spawn(&result.spawned_pid, VITREO_SCOPED_ENV_PROBE_PATH,
                                 &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    if (rc != 0) {
        ::close(pipe_fds[0]);
        ADD_FAILURE() << "posix_spawn failed: " << rc;
        return result;
    }

    // A re-exec loop is the failure to catch, so bound the run.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            result.timed_out = true;
            ::kill(result.spawned_pid, SIGKILL);
            break;
        }
        pollfd waiting{pipe_fds[0], POLLIN, 0};
        const int ready = ::poll(&waiting, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        char buffer[1024];
        const ssize_t got = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (got <= 0) break;
        result.output.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(pipe_fds[0]);
    int status = 0;
    (void)::waitpid(result.spawned_pid, &status, 0);
    result.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::vector<std::string> base_environment() {
    return {"PATH=/usr/bin:/bin", "HOME=/tmp"};
}

std::string directive_for(std::string_view needle, std::string_view value = "on") {
    return "VITREO_SCOPED_ENV=" + std::string(needle) + "=>" + kKnob + "=" +
           std::string(value);
}

TEST(ScopedEnvironmentProcess, AKnobLatchedAtStaticInitialisationSeesTheScopedValue) {
    auto env = base_environment();
    env.push_back(directive_for("renderer"));
    const ProbeRun run =
        run_probe("\"C:\\App\\host.exe\" --type=renderer", {}, env);

    ASSERT_FALSE(run.timed_out) << run.output;
    EXPECT_EQ(run.exit_status, 0) << run.output;
    // The claim: the constant the static initialiser filled holds the value.
    EXPECT_EQ(run.value("latched"), "on") << run.output;
    // Same pid before and after: this is exec, not a spawn.
    EXPECT_EQ(run.value("pid"), std::to_string(run.spawned_pid)) << run.output;
    EXPECT_EQ(run.value("reexeced"), "1") << run.output;
    EXPECT_TRUE(contains(run.output, std::string("re-executing to deliver ") + kKnob +
                                         "=on before static initialisation"))
        << run.output;
    EXPECT_TRUE(contains(run.output,
                         std::string("[process] scoped env: ") + kKnob + "=on"))
        << run.output;
    EXPECT_TRUE(contains(run.output, "re-executed with it")) << run.output;
    EXPECT_FALSE(contains(run.output, "LATE")) << run.output;
    // Exactly one re-exec.
    EXPECT_EQ(count(run.output, "re-executing to deliver"), 1u) << run.output;
}

TEST(ScopedEnvironmentProcess, AProcessTheDirectiveDoesNotSelectIsLeftAlone) {
    auto env = base_environment();
    env.push_back(directive_for("renderer"));
    const ProbeRun run =
        run_probe("\"C:\\App\\host.exe\" --type=gpu-process", {}, env);

    ASSERT_FALSE(run.timed_out) << run.output;
    EXPECT_EQ(run.value("latched"), "<unset>") << run.output;
    EXPECT_EQ(run.value("reexeced"), "0") << run.output;
    EXPECT_FALSE(contains(run.output, "scoped env")) << run.output;
}

TEST(ScopedEnvironmentProcess, ValuesTheTreeAlreadyHasNeedNoReexec) {
    auto env = base_environment();
    env.push_back(directive_for("renderer"));
    env.push_back(std::string(kKnob) + "=on");
    const ProbeRun run =
        run_probe("\"C:\\App\\host.exe\" --type=renderer", {}, env);

    ASSERT_FALSE(run.timed_out) << run.output;
    EXPECT_EQ(run.value("latched"), "on") << run.output;
    EXPECT_EQ(run.value("reexeced"), "0") << run.output;
    EXPECT_FALSE(contains(run.output, "re-executing")) << run.output;
    EXPECT_TRUE(contains(run.output, "already in the environment at process start"))
        << run.output;
}

TEST(ScopedEnvironmentProcess, ADifferentInheritedValueIsReplacedNotDuplicated) {
    auto env = base_environment();
    env.push_back(directive_for("renderer", "on"));
    env.push_back(std::string(kKnob) + "=off");
    const ProbeRun run =
        run_probe("\"C:\\App\\host.exe\" --type=renderer", {}, env);

    ASSERT_FALSE(run.timed_out) << run.output;
    EXPECT_EQ(run.value("latched"), "on") << run.output;
    EXPECT_EQ(run.value("lazy"), "on") << run.output;
}

TEST(ScopedEnvironmentProcess, AMarkerInheritedFromAnAncestorDoesNotSuppressDelivery) {
    // The marker carries the pid of the re-exec that wrote it. A descendant has
    // another pid, so it must still be delivered to.
    auto env = base_environment();
    env.push_back(directive_for("renderer"));
    env.push_back(std::string(kScopedEnvironmentReexecMarker) + "=1");
    const ProbeRun run =
        run_probe("\"C:\\App\\host.exe\" --type=renderer", {}, env);

    ASSERT_FALSE(run.timed_out) << run.output;
    EXPECT_EQ(run.value("latched"), "on") << run.output;
    EXPECT_EQ(run.value("pid"), std::to_string(run.spawned_pid)) << run.output;
}

TEST(ScopedEnvironmentProcess, WithoutAnExecutablePathTheValueIsLateAndSaysSo) {
    auto env = base_environment();
    env.push_back(directive_for("renderer"));
    const ProbeRun run = run_probe("\"C:\\App\\host.exe\" --type=renderer",
                                   {"--no-exe-path"}, env);

    ASSERT_FALSE(run.timed_out) << run.output;
    // The latched constant did not see it: the log must not claim otherwise.
    EXPECT_EQ(run.value("latched"), "<unset>") << run.output;
    EXPECT_EQ(run.value("lazy"), "on") << run.output;
    EXPECT_TRUE(contains(run.output, std::string("[process] scoped env: ") + kKnob +
                                         "=on LATE"))
        << run.output;
    EXPECT_TRUE(contains(run.output, "did NOT see it")) << run.output;
    EXPECT_FALSE(contains(run.output, "re-executed with it")) << run.output;
}

TEST(ScopedEnvironmentProcess, AFailedExecIsReportedAndTheValueIsLate) {
    auto env = base_environment();
    env.push_back(directive_for("renderer"));
    const ProbeRun run = run_probe("\"C:\\App\\host.exe\" --type=renderer",
                                   {"--bad-exe-path"}, env);

    ASSERT_FALSE(run.timed_out) << run.output;
    EXPECT_EQ(run.value("latched"), "<unset>") << run.output;
    EXPECT_TRUE(contains(run.output, "re-exec of '/nonexistent/probe' FAILED"))
        << run.output;
    EXPECT_TRUE(contains(run.output, "re-exec failed")) << run.output;
    EXPECT_TRUE(contains(run.output, "LATE")) << run.output;
}

TEST(ScopedEnvironmentProcess, AValueThatVanishesAfterTheReexecDoesNotLoop) {
    auto env = base_environment();
    env.push_back(directive_for("renderer"));
    const ProbeRun run = run_probe("\"C:\\App\\host.exe\" --type=renderer",
                                   {"--clobber"}, env);

    ASSERT_FALSE(run.timed_out) << "re-exec loop: " << run.output;
    EXPECT_EQ(run.exit_status, 0) << run.output;
    EXPECT_EQ(count(run.output, "re-executing to deliver"), 1u) << run.output;
    EXPECT_TRUE(contains(run.output, "not re-executing again")) << run.output;
}

}  // namespace
}  // namespace vitreo::platform
