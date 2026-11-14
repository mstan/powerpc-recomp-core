/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_NATIVE_CHAIN_INLINE_H
#define DOLRECOMP_NATIVE_CHAIN_INLINE_H
/* Include only after the hosting runtime's complete CPUState declaration.
 * No CPUState layout is duplicated or linked into the state library. */
#include "cpu/native_chain.h"
static inline bool dr_native_chain_next(struct CPUState* cpu) {
    DrNativeChain* state=dr_native_chain_state;
    if(!state || state->cpu!=cpu || !state->admitted ||
       state->depth>=DR_NATIVE_CHAIN_MAX_DEPTH || (state->halted && *state->halted) || cpu->exception ||
       !cpu->cycle_deadline || cpu->cycles>=cpu->cycle_deadline ||
       (state->stop_pc && cpu->pc==state->stop_pc) || (cpu->pc&3u))return false;
    const uint32_t physical=cpu->pc&0x1fffffffu;
    if(physical>=0x01800000u)return false;
    unsigned bank;
    switch(cpu->pc>>29u){case 0:bank=0;break;case 4:bank=1;break;case 6:bank=2;break;default:return false;}
    const unsigned page=physical>>12u;
    const size_t slot=(size_t)bank*DR_NATIVE_CHAIN_MEM1_PAGES+page;
    if(!state->entries || slot>=state->entry_count ||
       (state->excluded_pages && state->excluded_pages[page]) || !state->entries[slot])return false;
    ++state->depth;
    bool allowed=!state->before || state->before(cpu,state->user);
    /* Host diagnostics may revoke admission or invalidate a binding. Never
     * retain an entry loaded before that observer's boundary. */
    DrNativeChainFn next=allowed && state->admitted && !(state->halted && *state->halted) &&
        state->entries && slot<state->entry_count &&
        !(state->excluded_pages && state->excluded_pages[page])?state->entries[slot]:NULL;
    if(next) {
        next(cpu);
        if(state->dispatches)++*state->dispatches;
    }
    --state->depth;
    return next!=NULL;
}
#endif
