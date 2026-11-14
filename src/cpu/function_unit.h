/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_FUNCTION_UNIT_H
#define DOLRECOMP_FUNCTION_UNIT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct CPUState;
/* Called with canonical state at a logical instruction boundary. The owner
 * attests code/ownership and checks its stop/interrupt policy. Read-only: a
 * callback that executes guest code or mutates CPU state is not a guard. */
/* scheduling=false is identity/ownership safety ONLY. Do not apply a new
 * deadline/interrupt/stop test at a store or interior block: the original
 * branch/page checkpoints still own those scheduling semantics. */
typedef bool (*DrFunctionUnitGuard)(const struct CPUState*, uint32_t pc,
                                    uint64_t cycles, bool scheduling, void* user);
typedef struct {
    DrFunctionUnitGuard guard;
    void* user;
} DrFunctionUnitOwner;
typedef struct {
    uint32_t entry, end; /* Half-open original function extent. */
    const uint32_t* entries; /* Root + original opaque-call continuations. */
    size_t entry_count;
    const uint32_t* block_entries;
    size_t block_count;
} DrFunctionUnitInfo;
/* A generated bool body returns false only before any architectural/bus/cycle
 * progress. The consumer must then call its original native catalog binding;
 * false is not an interpreted instruction or a successful no-op dispatch.
 * True includes a faulting first instruction and a published partial prefix.
 * All unsupported/arbitrary interior entries retain that native fallback. */
#ifndef DOLRECOMP_FUNCTION_UNIT
#define DOLRECOMP_FUNCTION_UNIT 0
#endif
#endif
