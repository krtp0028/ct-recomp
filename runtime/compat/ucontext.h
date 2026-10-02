/* <ucontext.h> for platforms that have none: Windows and the PSP.
 *
 * Implements the subset the runtime uses (getcontext, makecontext,
 * swapcontext, for a coroutine that yields on demand) with a plain
 * register-context switch: a context is data, as POSIX ucontext has it, so
 * a longjmp taken inside a coroutine and landing outside it (the tests'
 * fatal hook does this) leaves nothing thread-side behind.
 *
 * uc_stack is the coroutine's stack. Backends: ucontext_win.c (x86_64) and
 * ucontext_psp.c (MIPS). */
#ifndef CT_RUNTIME_COMPAT_UCONTEXT_H
#define CT_RUNTIME_COMPAT_UCONTEXT_H

#if !defined(_WIN32) && !defined(__PSP__)
#include_next <ucontext.h>   /* a real one exists: use it */
#else

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

typedef struct ucontext_t {
    struct {
        void *ss_sp;
        size_t ss_size;
    } uc_stack;
    struct ucontext_t *uc_link;
    /* Switch state; the offsets are fixed by the asm in ucontext_win.c
       and guarded by _Static_asserts there. */
    uint64_t rsp, rbp, rbx, rsi, rdi, r12, r13, r14, r15, rip;
    uint64_t xmm[10][2];   /* xmm6-xmm15: nonvolatile in the x64 ABI */
} ucontext_t;

#else /* __PSP__ */

/* MIPS o32: callee-saved $ra,$sp,$gp,$fp,$s0-$s7 and $f20-$f30 + FCSR.
   Offsets fixed by the asm in ucontext_psp.c, guarded there. */
typedef struct ucontext_t {
    struct {
        void *ss_sp;
        size_t ss_size;
    } uc_stack;
    struct ucontext_t *uc_link;
    uint32_t ra, sp, gp, fp;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t f20, f22, f24, f26, f28, f30;
    uint32_t fcsr;
} ucontext_t;

#endif /* _WIN32 */

int getcontext(ucontext_t *uc);
void makecontext(ucontext_t *uc, void (*fn)(void), int argc, ...);
int swapcontext(ucontext_t *oucp, const ucontext_t *ucp);

#ifdef __cplusplus
}
#endif

#endif /* not Windows and not PSP */
#endif
