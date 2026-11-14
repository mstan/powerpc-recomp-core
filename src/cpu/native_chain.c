/* SPDX-License-Identifier: MIT */
#include "cpu/native_chain.h"
DrNativeChain* dr_native_chain_state;
bool dr_native_chain_bind(DrNativeChain* state) {
    if(dr_native_chain_state && dr_native_chain_state->depth)return false;
    if(dr_native_chain_state)dr_native_chain_state->admitted=false;
    if(state){state->depth=0;state->admitted=false;}
    dr_native_chain_state=state;
    return true;
}
