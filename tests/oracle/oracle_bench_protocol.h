// SPDX-License-Identifier: LGPL-2.1-only
// Persistent host x86 runner bench protocol. One-shot OracleState is 936 bytes
// (512-byte mem blob). This file is only the warm-loop command overlay.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORACLE_BENCH_CMD_LOAD   1u
#define ORACLE_BENCH_CMD_WARMUP 2u
#define ORACLE_BENCH_CMD_RUN    3u
#define ORACLE_BENCH_CMD_TIMES  4u
#define ORACLE_BENCH_CMD_UNLOAD 5u
#define ORACLE_BENCH_CMD_QUIT   6u
#define ORACLE_BENCH_CMD_ONCE   7u

typedef struct {
    uint32_t cmd;
    uint32_t n;
} OracleBenchCmd;

typedef struct {
    uint64_t warmup_ns;
    uint64_t run_ns;
    uint64_t iters;
    uint32_t status;
    uint32_t reserved;
} OracleBenchTimes;

#ifdef __cplusplus
}
#endif
