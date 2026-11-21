/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_NATIVE_FUNCTIONS_H
#define DOLRECOMP_NATIVE_FUNCTIONS_H

#include <stdio.h>
#include "common/types.h"
#include "frontend/decoder.h"

typedef enum {
    DR_NATIVE_FUNCTION,
    DR_NATIVE_JUMP_TABLE,
    DR_NATIVE_POTENTIAL_JUMP_TABLE,
    DR_NATIVE_SAVE_GPR,
    DR_NATIVE_RESTORE_GPR,
    DR_NATIVE_SAVE_FPR,
    DR_NATIVE_RESTORE_FPR,
} DrNativeSymbolKind;

typedef struct {
    const char* name;
    u32 address;
    u32 size;
    bool executable;
} DrNativeSection;

typedef struct {
    u32 address;
    u32 size;
    DrNativeSymbolKind kind;
    u8 first_register;
} DrNativeSymbol;

typedef struct {
    DrNativeSymbol* entries;
    size_t count;
} DrNativeSymbols;

/* With no sections supplied, dtk offsets are DOL absolute addresses.
 * REL callers pass the section names and their compiler-local addresses. */
bool dr_native_symbols_read(FILE* input, const DrNativeSection* sections,
                            size_t section_count, DrNativeSymbols* result);
void dr_native_symbols_free(DrNativeSymbols* symbols);
const DrNativeSymbol* dr_native_symbol_at(const DrNativeSymbols* symbols, u32 address);
const DrNativeSymbol* dr_native_function_containing(const DrNativeSymbols* symbols, u32 address);

typedef enum {
    DR_NATIVE_STRAIGHT_LINE,
    DR_NATIVE_BRANCH_LOCAL,
    DR_NATIVE_BRANCH_FUNCTION,
    DR_NATIVE_CALL_FUNCTION,
    DR_NATIVE_CAPTURE_PC,
    DR_NATIVE_CALL_INTERIOR,
    DR_NATIVE_SAVE_RESTORE,
    DR_NATIVE_RETURN,
    DR_NATIVE_BRANCH_INDIRECT,
    DR_NATIVE_CALL_INDIRECT,
    DR_NATIVE_UNBOUND,
} DrNativeTransferKind;

typedef struct {
    DrNativeTransferKind kind;
    const DrNativeSymbol* symbol;
    u32 target;
    bool local_back_edge;
} DrNativeTransfer;

DrNativeTransfer dr_native_transfer(const DrNativeSymbols* symbols,
                                    const DrNativeSymbol* function, const PPCInst* instruction);

#endif
