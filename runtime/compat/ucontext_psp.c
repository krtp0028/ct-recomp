/* PSP backend for runtime/compat/ucontext.h: a MIPS o32 register-context
 * switch.
 *
 * A switch saves the registers the compiler expects to survive a call —
 * $ra, $sp, $gp, $fp, $s0-$s7 and (hard-float) $f20-$f30 plus FCSR — and
 * restores the target's. A fresh context has its $sp in the provided
 * stack with a 16-byte o32 argument area; ct_ctx_entry enters the function
 * there, with $ra set to ct_ctx_returned so a coroutine that returns fails
 * loudly instead of running off.
 *
 * GCC ignores __attribute__((naked)) on MIPS, so the switch is file-scope
 * assembly. */
#ifdef __PSP__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucontext.h"

/* The asm below indexes ucontext_t by literal offset; keep these in step. */
_Static_assert(offsetof(ucontext_t, ra) == 12, "ucontext ra offset");
_Static_assert(offsetof(ucontext_t, s0) == 28, "ucontext s0 offset");
_Static_assert(offsetof(ucontext_t, f20) == 60, "ucontext f20 offset");
_Static_assert(offsetof(ucontext_t, fcsr) == 84, "ucontext fcsr offset");
_Static_assert(sizeof(ucontext_t) == 88, "ucontext size");

extern void ct_ctx_switch(ucontext_t *from, ucontext_t *to);
extern void ct_ctx_entry(void);
extern void ct_ctx_returned(void);
extern uint32_t ct_read_gp(void);

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
        fprintf(stderr, "ucontext_psp: makecontext without a stack\n");
        abort();
    }
    uintptr_t top = ((uintptr_t)uc->uc_stack.ss_sp + uc->uc_stack.ss_size) & ~(uintptr_t)7;
    uc->sp = (uint32_t)(top - 16);   /* the o32 16-byte argument area */
    uc->gp = ct_read_gp();
    uc->ra = (uint32_t)(uintptr_t)ct_ctx_entry;
    uc->s0 = (uint32_t)(uintptr_t)fn;
    uc->s1 = (uint32_t)(uintptr_t)ct_ctx_returned;
}

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

__asm__(
    ".text\n"
    ".set noreorder\n"

    ".globl ct_ctx_switch\n"
    ".ent ct_ctx_switch\n"
    "ct_ctx_switch:\n"
    /* save into a0 */
    "sw   $ra, 12($a0)\n"
    "sw   $sp, 16($a0)\n"
    "sw   $gp, 20($a0)\n"
    "sw   $fp, 24($a0)\n"
    "sw   $s0, 28($a0)\n"
    "sw   $s1, 32($a0)\n"
    "sw   $s2, 36($a0)\n"
    "sw   $s3, 40($a0)\n"
    "sw   $s4, 44($a0)\n"
    "sw   $s5, 48($a0)\n"
    "sw   $s6, 52($a0)\n"
    "sw   $s7, 56($a0)\n"
    "swc1 $f20, 60($a0)\n"
    "swc1 $f22, 64($a0)\n"
    "swc1 $f24, 68($a0)\n"
    "swc1 $f26, 72($a0)\n"
    "swc1 $f28, 76($a0)\n"
    "swc1 $f30, 80($a0)\n"
    "cfc1 $t0, $31\n"
    "sw   $t0, 84($a0)\n"
    /* restore from a1 */
    "lw   $ra, 12($a1)\n"
    "lw   $sp, 16($a1)\n"
    "lw   $gp, 20($a1)\n"
    "lw   $fp, 24($a1)\n"
    "lw   $s0, 28($a1)\n"
    "lw   $s1, 32($a1)\n"
    "lw   $s2, 36($a1)\n"
    "lw   $s3, 40($a1)\n"
    "lw   $s4, 44($a1)\n"
    "lw   $s5, 48($a1)\n"
    "lw   $s6, 52($a1)\n"
    "lw   $s7, 56($a1)\n"
    "lwc1 $f20, 60($a1)\n"
    "lwc1 $f22, 64($a1)\n"
    "lwc1 $f24, 68($a1)\n"
    "lwc1 $f26, 72($a1)\n"
    "lwc1 $f28, 76($a1)\n"
    "lwc1 $f30, 80($a1)\n"
    "lw   $t0, 84($a1)\n"
    "ctc1 $t0, $31\n"
    "jr   $ra\n"
    "nop\n"
    ".end ct_ctx_switch\n"

    /* A fresh context enters here: $s0 is the entry, $s1 the return trap. */
    ".globl ct_ctx_entry\n"
    ".ent ct_ctx_entry\n"
    "ct_ctx_entry:\n"
    "move $ra, $s1\n"
    "jr   $s0\n"
    "nop\n"
    ".end ct_ctx_entry\n"

    /* The coroutines here never return; if one does, fail loudly. */
    ".globl ct_ctx_returned\n"
    ".ent ct_ctx_returned\n"
    "ct_ctx_returned:\n"
    "jal  abort\n"
    "nop\n"
    ".end ct_ctx_returned\n"

    ".globl ct_read_gp\n"
    ".ent ct_read_gp\n"
    "ct_read_gp:\n"
    "jr   $ra\n"
    "move $v0, $gp\n"
    ".end ct_read_gp\n"

    ".set reorder\n");

#endif /* __PSP__ */
