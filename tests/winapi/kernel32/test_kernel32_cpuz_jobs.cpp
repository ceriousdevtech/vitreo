/// @file test_kernel32_cpuz_jobs.cpp
/// @brief Unit tests for KERNEL32 job-object contracts

#include "kernel32_cpuz_fixture.hpp"
#include <chrono>
#include <winapi/kernel32/process/process.hpp>
#include <ipc/process_handle.hpp>
#include <array>
#include <unistd.h>
#include <sys/wait.h>
#include <thread>

TEST_F(Kernel32CpuzTest, IsProcessInJobReflectsAssignProcessToJobObject) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    HandleTable handles;
    Kernel32ExtManager mgr(memory, vfs);
    mgr.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    constexpr GuestAddr stack = 0x8000;
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr result_addr = 0x3000;
    ASSERT_TRUE(memory.write_u32(result_addr, 0xFFFFFFFFu).is_ok());

    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 0;
    mgr.impl_CreateJobObjectW(marshaler);
    const Handle job = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(job, NULL_HANDLE);

    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = 0; // any job
    cpu.gpr.r8 = result_addr;
    mgr.impl_IsProcessInJob(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(result_addr).value_or(1), 0u);

    cpu.gpr.rcx = job;
    cpu.gpr.rdx = CURRENT_PROCESS_PSEUDO;
    mgr.impl_AssignProcessToJobObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = result_addr;
    mgr.impl_IsProcessInJob(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(result_addr).value_or(0), 1u);

    cpu.gpr.rcx = CURRENT_PROCESS_PSEUDO;
    cpu.gpr.rdx = job;
    cpu.gpr.r8 = result_addr;
    mgr.impl_IsProcessInJob(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(result_addr).value_or(0), 1u);
}

TEST_F(Kernel32CpuzTest, JobObjectLimitsRoundTripAndHandleCloses) {
    const GuestAddr stack = cpu.sp();
    constexpr GuestAddr limits = DATA_BASE + 0x100;
    constexpr GuestAddr queried = DATA_BASE + 0x200;
    constexpr GuestAddr returned = DATA_BASE + 0x300;
    constexpr u32 x86_extended_limit_size = 112;

    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_CreateJobObjectW(marshaler);
    const Handle job = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(job, NULL_HANDLE);
    EXPECT_EQ(handles.get_type(job), HandleType::Job);

    std::array<u8, x86_extended_limit_size> value{};
    value[16] = 0x08; // a representative LimitFlags byte
    value[28] = 1;    // ActiveProcessLimit
    ASSERT_TRUE(memory.write_bytes(
        limits, value.data(), value.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, job).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 9).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, limits).is_ok());
    ASSERT_TRUE(memory.write_u32(
        stack + 16, x86_extended_limit_size).is_ok());
    mgr.impl_SetInformationJobObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(stack + 4, job).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 9).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, queried).is_ok());
    ASSERT_TRUE(memory.write_u32(
        stack + 16, x86_extended_limit_size).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, returned).is_ok());
    mgr.impl_QueryInformationJobObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(returned).value_or(0),
              x86_extended_limit_size);
    std::array<u8, x86_extended_limit_size> round_trip{};
    ASSERT_TRUE(memory.read_bytes(
        queried, round_trip.data(), round_trip.size()).is_ok());
    EXPECT_EQ(round_trip, value);

    EXPECT_TRUE(handles.close(job));
    EXPECT_EQ(handles.get_type(job), HandleType::Invalid);
    EXPECT_TRUE(mgr.on_job_handle_close(job));
}

TEST_F(Kernel32CpuzTest,
       JobCompletionPortAssociationQueuesExistingProcessX64) {
    constexpr GuestAddr association = DATA_BASE + 0x100;
    constexpr GuestAddr event_out = DATA_BASE + 0x200;
    constexpr GuestAddr key_out = DATA_BASE + 0x208;
    constexpr GuestAddr process_out = DATA_BASE + 0x210;
    constexpr u64 completion_key = 0x1234567887654321ULL;
    constexpr u32 JobObjectAssociateCompletionPortInformation = 7;
    constexpr u32 JOB_OBJECT_MSG_NEW_PROCESS = 6;

    cpu.init_long_mode();
    cpu.gpr.rsp = STACK_BASE + 0x800;
    marshaler.attach_cpu(cpu);
    const GuestAddr stack = cpu.sp();

    cpu.gpr.rcx = INVALID_HANDLE;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 0;
    mgr.impl_CreateJobObjectW(marshaler);
    const Handle job = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(job, NULL_HANDLE);

    cpu.gpr.rcx = job;
    cpu.gpr.rdx = CURRENT_PROCESS_PSEUDO;
    mgr.impl_AssignProcessToJobObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u64(association, completion_key).is_ok());
    ASSERT_TRUE(memory.write_u64(association + 8, port).is_ok());
    cpu.gpr.rcx = job;
    cpu.gpr.rdx = JobObjectAssociateCompletionPortInformation;
    cpu.gpr.r8 = association;
    cpu.gpr.r9 = 16;
    mgr.impl_SetInformationJobObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = port;
    cpu.gpr.rdx = event_out;
    cpu.gpr.r8 = key_out;
    cpu.gpr.r9 = process_out;
    ASSERT_TRUE(memory.write_u64(stack + 0x28, 0).is_ok());
    mgr.impl_GetQueuedCompletionStatus(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(event_out).value_or(0),
              JOB_OBJECT_MSG_NEW_PROCESS);
    EXPECT_EQ(memory.read_u64(key_out).value_or(0), completion_key);
    EXPECT_EQ(memory.read_u64(process_out).value_or(0),
              static_cast<u64>(::getpid()));
}

// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: when the job's LAST handle closes,
// every member process ends with exit code 0; a duplicate keeps it open
// (tools/oracle_probe/s4_job_probe.expected kill_on_close_first_handle
// wait=102, kill_on_close_last_handle code=0). Before, the flag was stored
// and ignored, so a Chromium child outlived its browser process.
TEST_F(Kernel32CpuzTest, KillOnJobCloseEndsMembersAtTheLastJobHandle) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ipc::ProcessObject* process = ipc::track_process(child);
    ASSERT_NE(process, nullptr);
    const Handle process_handle =
        handles.allocate(HandleType::Process, process, 0, HandleAccess::All);

    const GuestAddr stack = cpu.sp();
    constexpr GuestAddr limits = DATA_BASE + 0x100;
    constexpr u32 x86_extended_limit_size = 112;
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_CreateJobObjectW(marshaler);
    const Handle job = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(job, NULL_HANDLE);
    std::array<u8, x86_extended_limit_size> value{};
    value[17] = 0x20;  // LimitFlags (offset 16) = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    ASSERT_TRUE(memory.write_bytes(limits, value.data(), value.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, job).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 9).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, limits).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, x86_extended_limit_size).is_ok());
    mgr.impl_SetInformationJobObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    ASSERT_TRUE(memory.write_u32(stack + 4, job).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, process_handle).is_ok());
    mgr.impl_AssignProcessToJobObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    // TerminateJobObject on another job with no members succeeds.
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_CreateJobObjectW(marshaler);
    const Handle empty_job = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_TRUE(memory.write_u32(stack + 4, empty_job).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x55).is_ok());
    mgr.impl_TerminateJobObject(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    EXPECT_TRUE(handles.close(job));
    EXPECT_TRUE(mgr.on_job_handle_close(job));
    int status = 0;
    pid_t reaped = 0;
    for (int i = 0; i < 200 && reaped == 0; ++i) {
        reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(reaped, child) << "member was not terminated at job close";
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
    EXPECT_TRUE(handles.close(empty_job));
    EXPECT_TRUE(mgr.on_job_handle_close(empty_job));
}
