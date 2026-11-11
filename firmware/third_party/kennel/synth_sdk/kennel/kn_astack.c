/* The audio callback on its own L1 stack: the region, its fill, the switch and the guards (kn_astack.h).
 * The SP switch itself is kn_astack_run, top-level asm below. Integers only, no libc.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "kn_astack.h"

#ifdef OP1_HOST
uint32_t kn_astack_host[KN_ASTACK_BYTES / 4];
#define REGION     (kn_astack_host)
static uint32_t as_irq_off(void) { return 0; }
static void as_irq_on(uint32_t m) { (void)m; }
void *volatile kn_astack_top;
uint32_t kn_astack_lo;
void kn_astack_run(void (*fn)(void *, void *, void *), void *a, void *b, void *c) { fn(a, b, c); }
#define TOP_PTR()  ((void *)((uint8_t *)kn_astack_host + KN_ASTACK_BYTES - KN_ASTACK_CAP))
#define LO_ADDR()  ((uint32_t)(uintptr_t)kn_astack_host)
#else
#define REGION     ((volatile uint32_t *)KN_ASTACK_LO)
static uint32_t as_irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void as_irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
void *volatile kn_astack_top;                    /* (kn_astack_run reads both) */
uint32_t kn_astack_lo = KN_ASTACK_LO;
/* kn_astack_run(fn, a, b, c): call fn(a, b, c) on the L1 audio stack (kn_astack.h, ARCHITECTURE.md "L1 audio stack").
 * R0 = fn, R1 = a, R2 = b, c at [entry SP + 12] (the Blackfin C ABI). kn_astack_top == 0 (off), or SP already in
 * [kn_astack_lo, kn_astack_top) (nested: cannot happen for IVG11, checked anyway): fn runs on the current stack.
 * Otherwise the old SP is kept in P5 (callee-saved, so fn preserves it), SP = kn_astack_top, 12 bytes of argument
 * area are reserved as the ABI asks (R246's callback reads [entry SP + 0] and [+4], dead loads, both inside them),
 * fn runs, SP comes back from P5. Nothing else is stored on the L1 stack by this code. Interrupts that arrive while
 * SP is in L1 push their frames there (sized for in kn_astack.h / ARCHITECTURE.md).
 * (Top-level asm here rather than a .S file, so a tier 2 guest can include this file: harness/tier2/astack_guest.c.) */
__asm__(
    ".text\n"
    ".align 4\n"
    ".global _kn_astack_run\n"
    ".type _kn_astack_run, STT_FUNC\n"
    "_kn_astack_run:\n"
    "    [--SP] = RETS;\n"
    "    [--SP] = P5;\n"
    "    P1 = R0;                        /* fn */\n"
    "    R0 = R1;                        /* a */\n"
    "    R1 = R2;                        /* b */\n"
    "    R2 = [SP + 20];                 /* c: entry SP + 12, two words pushed since */\n"
    "    P0.L = _kn_astack_top;\n"
    "    P0.H = _kn_astack_top;\n"
    "    P2 = [P0];\n"
    "    CC = P2 == 0;\n"
    "    IF CC JUMP .Ldirect;            /* off */\n"
    "    P0.L = _kn_astack_lo;\n"
    "    P0.H = _kn_astack_lo;\n"
    "    P0 = [P0];\n"
    "    CC = SP < P2 (IU);\n"
    "    IF !CC JUMP .Lswitch;           /* SP above the top: not on it */\n"
    "    CC = SP < P0 (IU);\n"
    "    IF !CC JUMP .Ldirect;           /* lo <= SP < top: already on it */\n"
    ".Lswitch:\n"
    "    P5 = SP;\n"
    "    SP = P2;\n"
    "    SP += -12;\n"
    "    CALL (P1);\n"
    "    SP = P5;\n"
    "    P5 = [SP++];\n"
    "    RETS = [SP++];\n"
    "    RTS;\n"
    ".Ldirect:\n"
    "    SP += -12;\n"
    "    CALL (P1);\n"
    "    SP += 12;\n"
    "    P5 = [SP++];\n"
    "    RETS = [SP++];\n"
    "    RTS;\n"
    ".size _kn_astack_run, .-_kn_astack_run\n"
    ".previous\n");
#define TOP_PTR()  ((void *)(uintptr_t)(KN_ASTACK_HI - KN_ASTACK_CAP))
#define LO_ADDR()  (KN_ASTACK_LO)
#endif

#define WORDS      (KN_ASTACK_BYTES / 4)
#define RED_W      (KN_ASTACK_RED / 4)
#define CAP_W      (KN_ASTACK_CAP / 4)
_Static_assert(KN_ASTACK_LO % 32 == 0 && KN_ASTACK_HI % 32 == 0 && KN_ASTACK_LO >= 0xFF802FB4u &&
               KN_ASTACK_HI <= 0xFF803BD0u - 512u, "inside R246's kernel stack area, below its observed use");

static volatile uint32_t state = KN_AS_NONE;

static int intact(uint32_t from, uint32_t to)
{
    volatile uint32_t *r = REGION;
    for (uint32_t i = from; i < to; i++) if (r[i] != KN_ASTACK_FILL) return 0;
    return 1;
}
static void fill(void)                                    /* thread level, interrupts masked by the caller */
{
    volatile uint32_t *r = REGION;
    for (uint32_t i = 0; i < WORDS; i++) r[i] = KN_ASTACK_FILL;
    kn_astack_lo = LO_ADDR();
}

void kn_astack_mark(void)
{
    if (state == KN_AS_TRIP) return;                      /* the evidence of a trip stays */
    uint32_t m = as_irq_off();
    fill();                                               /* no frame of ours (or the kernel's) is live at thread level */
    if (state == KN_AS_NONE) state = KN_AS_OFF;
    as_irq_on(m);
}

int kn_astack_enable(int on)
{
    uint32_t m = as_irq_off();
    if (on) {
        if (state == KN_AS_OFF || state == KN_AS_NONE) { fill(); state = KN_AS_ON; kn_astack_top = TOP_PTR(); }
    } else if (state == KN_AS_ON) {
        kn_astack_top = 0;
        state = KN_AS_OFF;
    }
    as_irq_on(m);
    return (int)state;
}

void kn_astack_check(void)
{
    if (state != KN_AS_ON) return;
    if (intact(0, RED_W) && intact(WORDS - CAP_W, WORDS)) return;
    kn_astack_top = 0;                                    /* back to the interrupted stack, for good */
    state = KN_AS_TRIP;
}

uint32_t kn_astack_state(void) { return state; }
#ifdef OP1_HOST
void kn_astack_host_reset(void) { state = KN_AS_NONE; kn_astack_top = 0; }   /* tests: a fresh boot */
#endif

uint32_t kn_astack_used(void)
{
    if (state == KN_AS_NONE) return 0;
    volatile uint32_t *r = REGION;
    uint32_t i = 0;
    while (i < WORDS - CAP_W && r[i] == KN_ASTACK_FILL) i++;
    return (WORDS - CAP_W - i) * 4;
}
