/* SPDX-License-Identifier: MIT */
#include "analysis/native_functions.h"
#include <stdlib.h>
#include <string.h>

static bool starts_with(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool parse_u32(const char* text, char** end, int base, u32* result) {
    const unsigned long long value = strtoull(text, end, base);
    if (*end == text || value > 0xffffffffull)
        return false;
    *result = (u32)value;
    return true;
}

static const DrNativeSection* find_section(const DrNativeSection* sections, size_t count,
                                          const char* name, size_t length) {
    for (size_t i = 0; i < count; ++i)
        if (strlen(sections[i].name) == length && strncmp(sections[i].name, name, length) == 0)
            return &sections[i];
    return NULL;
}

static bool save_restore(const char* name, const char* name_end, DrNativeSymbol* symbol) {
    static const struct {
        const char* prefix;
        DrNativeSymbolKind kind;
    } helpers[] = {
        {"_savegpr_", DR_NATIVE_SAVE_GPR}, {"_restgpr_", DR_NATIVE_RESTORE_GPR},
        {"_savefpr_", DR_NATIVE_SAVE_FPR}, {"_restfpr_", DR_NATIVE_RESTORE_FPR},
    };
    for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); ++i) {
        const char* prefix = helpers[i].prefix;
        const size_t length = strlen(prefix);
        if (!starts_with(name, prefix))
            continue;
        char* end;
        u32 reg;
        if (!parse_u32(name + length, &end, 10, &reg) || end != name_end || reg < 14 || reg > 31)
            return false;
        symbol->first_register = (u8)reg;
        symbol->kind = helpers[i].kind;
        return true;
    }
    return false;
}

/* -1 is a malformed selected record, 0 is an irrelevant symbol. */
static int parse_symbol(char* line, const DrNativeSection* sections, size_t section_count,
                        DrNativeSymbol* symbol) {
    char* equals = strstr(line, " = ");
    const char* type = equals ? strstr(equals + 3, "type:") : NULL;
    if (!type)
        return 0;
    type += 5;
    const char* section_name = equals + 3;
    const char* colon = strchr(section_name, ':');
    const bool helper = (starts_with(type, "label") || starts_with(type, "function")) &&
                        save_restore(line, equals, symbol);
    if (!helper) {
        if (starts_with(type, "function"))
            symbol->kind = DR_NATIVE_FUNCTION;
        else if (starts_with(type, "object") && starts_with(line, "jumptable_"))
            symbol->kind = DR_NATIVE_JUMP_TABLE;
        else if (starts_with(type, "object") &&
                 (starts_with(section_name, ".data:") || starts_with(section_name, ".rodata:")))
            symbol->kind = DR_NATIVE_POTENTIAL_JUMP_TABLE;
        else
            return 0;
    }

    const bool potential = symbol->kind == DR_NATIVE_POTENTIAL_JUMP_TABLE;
    char* end;
    u32 offset;
    if (!colon || !parse_u32(colon + 1, &end, 0, &offset) || *end != ';' || (offset & 3u))
        return potential ? 0 : -1;
    if (!helper) {
        const char* size = strstr(type, "size:");
        if (!size || !parse_u32(size + 5, &end, 0, &symbol->size) || !symbol->size || (symbol->size & 3u))
            return potential ? 0 : -1;
    }
    if (potential && symbol->size < 8)
        return 0;

    u64 address = offset;
    if (sections) {
        const DrNativeSection* section = find_section(sections, section_count, section_name,
                                                     (size_t)(colon - section_name));
        if (!section && symbol->kind == DR_NATIVE_JUMP_TABLE)
            return 0; /* REL readers may recover table targets from relocations. */
        if (!section || offset >= section->size || symbol->size > section->size - offset ||
            (symbol->kind != DR_NATIVE_JUMP_TABLE && !potential && !section->executable))
            return potential ? 0 : -1;
        address += section->address;
    }
    if (address + symbol->size > 0xffffffffull || (address & 3u))
        return potential ? 0 : -1;
    symbol->address = (u32)address;
    return 1;
}

static int address_order(const void* lhs, const void* rhs) {
    const DrNativeSymbol* a = lhs;
    const DrNativeSymbol* b = rhs;
    if (a->address != b->address)
        return a->address < b->address ? -1 : 1;
    return (int)a->kind - (int)b->kind;
}

bool dr_native_symbols_read(FILE* input, const DrNativeSection* sections,
                            size_t section_count, DrNativeSymbols* result) {
    *result = (DrNativeSymbols){0};
    size_t capacity = 0, line_number = 0;
    char line[8192];
    while (fgets(line, sizeof(line), input)) {
        ++line_number;
        DrNativeSymbol symbol = {0};
        const int parsed = parse_symbol(line, sections, section_count, &symbol);
        if (parsed < 0 || (!strchr(line, '\n') && !feof(input))) {
            fprintf(stderr, "invalid native symbol at line %zu\n", line_number);
            goto fail;
        }
        if (!parsed)
            continue;
        if (result->count == capacity) {
            const size_t next = capacity ? capacity * 2 : 128;
            DrNativeSymbol* entries = realloc(result->entries, next * sizeof(*entries));
            if (!entries)
                goto fail;
            capacity = next;
            result->entries = entries;
        }
        result->entries[result->count++] = symbol;
    }
    if (ferror(input) || !result->count)
        goto fail;
    qsort(result->entries, result->count, sizeof(*result->entries), address_order);
    size_t unique = 0;
    u32 function_end = 0;
    for (size_t i = 0; i < result->count; ++i) {
        const DrNativeSymbol* symbol = &result->entries[i];
        if (unique && symbol->address == result->entries[unique-1].address) {
            DrNativeSymbol* prev = &result->entries[unique-1];
            if (symbol->size == prev->size && symbol->kind == prev->kind &&
                symbol->first_register == prev->first_register)
                continue;
            if (symbol->kind == DR_NATIVE_POTENTIAL_JUMP_TABLE) {
                if (prev->kind == DR_NATIVE_POTENTIAL_JUMP_TABLE && symbol->size > prev->size)
                    prev->size = symbol->size;
                continue;
            }
            /* CodeWarrior names the complete body and labels its first
             * register-specific entry at the same address. Preserve the
             * body's extent while making callers use the inline entry. */
            if (prev->kind == DR_NATIVE_FUNCTION && symbol->kind >= DR_NATIVE_SAVE_GPR) {
                prev->kind = symbol->kind;
                prev->first_register = symbol->first_register;
                continue;
            }
            fprintf(stderr, "ambiguous native entries at %08X\n", symbol->address);
            goto fail;
        }
        if (symbol->kind == DR_NATIVE_FUNCTION) {
            if (symbol->address < function_end) {
                fprintf(stderr, "overlapping native functions at %08X\n", symbol->address);
                goto fail;
            }
            function_end = symbol->address + symbol->size;
        }
        result->entries[unique++] = *symbol;
    }
    result->count = unique;
    return true;

fail:
    dr_native_symbols_free(result);
    return false;
}

void dr_native_symbols_free(DrNativeSymbols* symbols) {
    free(symbols->entries);
    *symbols = (DrNativeSymbols){0};
}

const DrNativeSymbol* dr_native_symbol_at(const DrNativeSymbols* symbols, u32 address) {
    size_t first = 0, end = symbols->count;
    while (first < end) {
        const size_t mid = first + (end - first) / 2;
        const DrNativeSymbol* symbol = &symbols->entries[mid];
        if (symbol->address == address)
            return symbol;
        if (symbol->address < address)
            first = mid + 1;
        else
            end = mid;
    }
    return NULL;
}

const DrNativeSymbol* dr_native_function_containing(const DrNativeSymbols* symbols, u32 address) {
    size_t first = 0, end = symbols->count;
    while (first < end) {
        const size_t mid = first + (end - first)/2;
        if (symbols->entries[mid].address <= address)
            first = mid + 1;
        else
            end = mid;
    }
    while (first) {
        const DrNativeSymbol* symbol = &symbols->entries[--first];
        if (symbol->kind == DR_NATIVE_FUNCTION)
            return address - symbol->address < symbol->size ? symbol : NULL;
    }
    return NULL;
}

DrNativeTransfer dr_native_transfer(const DrNativeSymbols* symbols,
                                    const DrNativeSymbol* function, const PPCInst* in) {
    DrNativeTransfer edge = {DR_NATIVE_STRAIGHT_LINE, NULL, 0, false};
    if (in->op == PPC_OP_BCLR) {
        edge.kind = in->lk ? DR_NATIVE_CALL_INDIRECT : DR_NATIVE_RETURN;
        return edge;
    }
    if (in->op == PPC_OP_BCCTR) {
        edge.kind = in->lk ? DR_NATIVE_CALL_INDIRECT : DR_NATIVE_BRANCH_INDIRECT;
        return edge;
    }
    if (in->op != PPC_OP_B && in->op != PPC_OP_BC)
        return edge;
    const u32 target = in->branch_target;
    edge.target = target;
    edge.symbol = dr_native_symbol_at(symbols, target);
    if (in->lk && target == in->address + 4u) {
        edge.kind = DR_NATIVE_CAPTURE_PC;
    } else if (target == function->address && in->lk) {
        edge.kind = DR_NATIVE_CALL_FUNCTION;
    } else if (target >= function->address && target - function->address < function->size) {
        edge.kind = in->lk ? DR_NATIVE_CALL_INTERIOR : DR_NATIVE_BRANCH_LOCAL;
        edge.local_back_edge = !in->lk && target <= in->address;
    } else if (!edge.symbol || edge.symbol->kind == DR_NATIVE_JUMP_TABLE ||
               edge.symbol->kind == DR_NATIVE_POTENTIAL_JUMP_TABLE) {
        edge.kind = DR_NATIVE_UNBOUND;
    } else if (edge.symbol->kind == DR_NATIVE_FUNCTION) {
        edge.kind = in->lk ? DR_NATIVE_CALL_FUNCTION : DR_NATIVE_BRANCH_FUNCTION;
    } else {
        edge.kind = DR_NATIVE_SAVE_RESTORE;
    }
    return edge;
}
