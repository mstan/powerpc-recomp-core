/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_NATIVE_CONTEXT_H
#define DOLRECOMP_NATIVE_CONTEXT_H

#include <setjmp.h>
#include <stdbool.h>

#if defined(_WIN32) && defined(_M_X64) && defined(_MSC_VER) && defined(__clang__)
#define DR_NATIVE_COMPACT_CONTEXT 1

/* Selected before guest execution. Compact continuations only cross generated
 * C frames. The machine retains the CRT path under Windows shadow-stack or
 * instruction-pointer validation policy. Fatal exits also keep CRT unwind.
 * This storage is a live host continuation, never persistent guest state. */
extern bool dr_native_use_compact_context;

#define DR_NATIVE_CAPTURE_CONTEXT(buffer, resumed) do {                       \
    if (dr_native_use_compact_context) {                                      \
        if (__builtin_setjmp((void**)(buffer))) {                             \
            (resumed) = true;                                                 \
        } else {                                                              \
            /* Win64 unwind info can encode an RBP displaced from the frame   \
             * base that Clang saved in slot zero. Its compact longjmp loads  \
             * RBP directly, so preserve the actual register. */               \
            void* frame_register;                                             \
            __asm__ volatile ("movq %%rbp, %0" : "=r"(frame_register));        \
            ((void**)(buffer))[0] = frame_register;                           \
            (resumed) = false;                                                \
        }                                                                     \
    } else if (setjmp(buffer)) {                                              \
        (resumed) = true;                                                     \
    } else {                                                                  \
        (resumed) = false;                                                    \
    }                                                                         \
} while (0)

static inline _Noreturn void dr_native_resume_context(jmp_buf buffer) {
    if (dr_native_use_compact_context)
        __builtin_longjmp((void**)buffer, 1);
    longjmp(buffer,1);
}

#else
#define DR_NATIVE_CAPTURE_CONTEXT(buffer, resumed) do {                       \
    if (setjmp(buffer)) { (resumed) = true; }                                  \
    else { (resumed) = false; }                                               \
} while (0)

static inline _Noreturn void dr_native_resume_context(jmp_buf buffer) {
    longjmp(buffer,1);
}
#endif

#endif
