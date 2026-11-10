/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/cpu.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    CPUState cpu;
    CHECK(cpu_init(&cpu));
    const u16 registers[] = {560,561,562,563,564,565,566,567,
                             568,569,570,571,572,573,574,575,1011};
    for (unsigned i=0; i<sizeof registers/sizeof *registers; ++i) {
        u16 spr=registers[i];
        cpu.spr[287]=PPC_GEKKO_PVR;
        cpu.exception=0;
        ppc_mtspr(&cpu,spr,0x90000002u,0x3400);
        CHECK(cpu.exception & PPC_EXC_PROGRAM);
        CHECK(cpu.spr[spr]==0);
        cpu.exception=0;
        ppc_mfspr(&cpu,spr,0x3400);
        CHECK(cpu.exception & PPC_EXC_PROGRAM);
        cpu.spr[287]=PPC_BROADWAY_PVR;
        cpu.exception=0;
        u32 value=0x80000002u+i;
        ppc_mtspr(&cpu,spr,value,0x3400);
        CHECK(cpu.exception==0);
        CHECK(ppc_mfspr(&cpu,spr,0x3400)==value && cpu.exception==0);
        cpu.spr[spr]=0;
    }
    cpu.exception=0;
    ppc_mtspr(&cpu,559,1,0x3400);
    CHECK(cpu.exception & PPC_EXC_PROGRAM);
    cpu.exception=0;
    ppc_mtspr(&cpu,287,PPC_GEKKO_PVR,0x3400);
    CHECK(cpu.exception & PPC_EXC_PROGRAM);
    CHECK(cpu.spr[287]==PPC_BROADWAY_PVR);
    cpu_free(&cpu);
    return 0;
}
