/* Windows backend for runtime/compat/ucontext.h: a register-level context
 * switch (x86_64, Microsoft ABI).
 *
 * A switch saves the registers the compiler expects to survive a call —
 * rbx, rbp, rsi, rdi, r12-r15, xmm6-xmm15 — plus rsp and rip, and restores
 * the target's. A fresh context (makecontext) has its rsp in the provided
 * stack; the entry runs there, and the slot below it holds entry_returned,
 * so a coroutine that returns anyway fails loudly instead of running off.
 *
 * MXCSR and the x87 control word are not saved: nothing here changes them
 * from their process-wide defaults. */
#ifdef _WIN32

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucontext.h"

#if !defined(__x86_64__) && !defined(_M_X64)
#error "runtime/compat/ucontext_win.c: only x86_64 is implemented"
#endif

/* The asm below indexes ucontext_t by literal offset; keep these in step. */
_Static_assert(offsetof(ucontext_t, rsp) == 24, "ucontext rsp offset");
_Static_assert(offsetof(ucontext_t, rip) == 96, "ucontext rip offset");
_Static_assert(offsetof(ucontext_t, xmm) == 104, "ucontext xmm offset");
_Static_assert(sizeof(ucontext_t) == 264, "ucontext size");

static void entry_returned(void);

int getcontext(ucontext_t *uc)
{
    if (!uc)
        return -1;
    memset(uc, 0, sizeof *uc);
    return 0;
}

void makecontext(ucontext_t *uc, void (*fn)(void), int argc, ...)
{
    (void)argc;
    if (!uc->uc_stack.ss_sp || !uc->uc_stack.ss_size) {
        fprintf(stderr, "ucontext_win: makecontext without a stack\n");
        abort();
    }
    uintptr_t top = ((uintptr_t)uc->uc_stack.ss_sp + uc->uc_stack.ss_size) & ~(uintptr_t)15;
    top -= 8;                    /* as after a call: rsp % 16 == 8 at the entry */
    *(void **)top = (void *)entry_returned;
    uc->rsp = (uint64_t)top;
    uc->rip = (uint64_t)(uintptr_t)fn;
}

static void ct_ctx_switch(ucontext_t *from, ucontext_t *to);

int swapcontext(ucontext_t *oucp, const ucontext_t *ucp)
{
    ucontext_t scratch;
    if (!ucp)
        return -1;
    if (!oucp) {
        memset(&scratch, 0, sizeof scratch);
        oucp = &scratch;
    }
    ct_ctx_switch(oucp, (ucontext_t *)(uintptr_t)ucp);
    return 0;
}

/* Reached by a ret out of a coroutine that returned: rsp % 16 == 0 here,
   which is what the call below needs. abort does not return. */
__attribute__((naked)) static void entry_returned(void)
{
    __asm__("call abort");
}

__attribute__((naked)) static void ct_ctx_switch(ucontext_t *from, ucontext_t *to)
{
    __asm__(
        /* save into rcx */
        "movq %rsp, 24(%rcx)\n\t"
        "movq %rbp, 32(%rcx)\n\t"
        "movq %rbx, 40(%rcx)\n\t"
        "movq %rsi, 48(%rcx)\n\t"
        "movq %rdi, 56(%rcx)\n\t"
        "movq %r12, 64(%rcx)\n\t"
        "movq %r13, 72(%rcx)\n\t"
        "movq %r14, 80(%rcx)\n\t"
        "movq %r15, 88(%rcx)\n\t"
        "leaq 1f(%rip), %rax\n\t"
        "movq %rax, 96(%rcx)\n\t"
        "movups %xmm6, 104(%rcx)\n\t"
        "movups %xmm7, 120(%rcx)\n\t"
        "movups %xmm8, 136(%rcx)\n\t"
        "movups %xmm9, 152(%rcx)\n\t"
        "movups %xmm10, 168(%rcx)\n\t"
        "movups %xmm11, 184(%rcx)\n\t"
        "movups %xmm12, 200(%rcx)\n\t"
        "movups %xmm13, 216(%rcx)\n\t"
        "movups %xmm14, 232(%rcx)\n\t"
        "movups %xmm15, 248(%rcx)\n\t"
        /* restore from rdx */
        "movups 104(%rdx), %xmm6\n\t"
        "movups 120(%rdx), %xmm7\n\t"
        "movups 136(%rdx), %xmm8\n\t"
        "movups 152(%rdx), %xmm9\n\t"
        "movups 168(%rdx), %xmm10\n\t"
        "movups 184(%rdx), %xmm11\n\t"
        "movups 200(%rdx), %xmm12\n\t"
        "movups 216(%rdx), %xmm13\n\t"
        "movups 232(%rdx), %xmm14\n\t"
        "movups 248(%rdx), %xmm15\n\t"
        "movq 24(%rdx), %rsp\n\t"
        "movq 32(%rdx), %rbp\n\t"
        "movq 40(%rdx), %rbx\n\t"
        "movq 48(%rdx), %rsi\n\t"
        "movq 56(%rdx), %rdi\n\t"
        "movq 64(%rdx), %r12\n\t"
        "movq 72(%rdx), %r13\n\t"
        "movq 80(%rdx), %r14\n\t"
        "movq 88(%rdx), %r15\n\t"
        "movq 96(%rdx), %rax\n\t"
        "jmp *%rax\n\t"
        "1:\n\t"
        "ret\n\t");
}

#endif /* _WIN32 */
