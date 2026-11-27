/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_NATIVE_CHAIN_H
#define DOLRECOMP_NATIVE_CHAIN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct CPUState;
typedef void (*DrNativeChainFn)(struct CPUState*);
/* Runs at the original chunk boundary with all CPU fields published. It may
 * record host diagnostics or deny admission, but must not execute guest code. */
typedef bool (*DrNativeChainBefore)(const struct CPUState*,void*);
enum {DR_NATIVE_CHAIN_MEM1_PAGES=6144,DR_NATIVE_CHAIN_MAX_DEPTH=8};
typedef struct {
    struct CPUState* cpu;
    const bool* halted; /* Host-only halt flag, absent in the reference CPU. */
    /* Live, content-verified MEM1 bindings, physical/cached/uncached banks.
     * The host's existing invalidation observer clears these same entries.
     * NULL means return to the host's functioning native compatibility path. */
    DrNativeChainFn const* entries;
    size_t entry_count;
    const uint8_t* excluded_pages; /* Physical page mask for host-owned hooks. */
    uint32_t stop_pc;
    unsigned depth;
    bool admitted;
    uint64_t* dispatches;
    DrNativeChainBefore before;
    void* user;
} DrNativeChain;
#ifdef __cplusplus
extern "C" {
#endif
extern DrNativeChain* dr_native_chain_state;
/* Single simulation owner only. Bind/unbind at a quiescent host boundary;
 * rebinding from any active chained call is rejected without changing state.
 * A successful bind starts disarmed. The host admits only eligible root
 * calls, suspending admission around nested guest/HLE ownership scopes. */
bool dr_native_chain_bind(DrNativeChain*);
#ifdef __cplusplus
}
#endif
#endif
