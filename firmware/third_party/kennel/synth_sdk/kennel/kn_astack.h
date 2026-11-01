/* The audio callback on its own L1 stack (kennel/kn_astack.c, ARCHITECTURE.md "L1 audio stack").
 * R246 runs its audio callback (IVG11) on whatever stack it interrupted: a thread stack in SDRAM heap 0 (or the L1
 * kernel stack under a DPC). Page 7's "stk" switch runs R246's callback 0xFFA04804 (the whole audio graph, every voice
 * render and FX, the dispatcher, the plugins) on a dedicated stack in L1 data A instead. Kennel's own bookkeeping in
 * sm_audio_cb (counters, kd_block_begin / end, the shed) stays on the interrupted stack.
 *
 * Region: the bottom of R246's kernel / start-up stack area 0xFF802FB4..0xFF803FB0 (clocks-memory.md 5.2), which R246
 * only uses from the top (0xFF803FA4 / 0xFF803FB0) down; its deepest use in the boot-to-main-screen snapshot is
 * 0xFF803BD0 (992 B, OP1-Emulator run/l1pool-2026-10-29/l1b-audit.md). Not in the L1 pool (kn_l1.c), not in L1 data B.
 *   KN_ASTACK_LO .. +KN_ASTACK_RED      bottom guard: overflow of ours is caught here, before R246's data below
 *   .. KN_ASTACK_HI - KN_ASTACK_CAP     the stack (SP starts at the top of this)
 *   .. KN_ASTACK_HI                     top guard: R246's kernel stack growing down into us is caught here
 * Nothing writes the region at boot. Page 7's open and every push fill it with KN_ASTACK_FILL, and so does switching
 * on, at thread level with interrupts masked (the kernel stack is only used at IVG14 and in thread start: no frame of
 * it is live at thread level). While off, the deepest changed word shows how far R246's kernel stack (a DPC with the
 * audio callback on top of it) reached into the region since the fill. Switch on: page 7 only, never persisted, off
 * at every boot. Every block while on, sm_audio_cb checks both guards after the callback returned; a guard word that
 * changed switches the stack off for good (until reboot) and page 7 says so.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef KN_ASTACK_H
#define KN_ASTACK_H
#include <stdint.h>

#define KN_ASTACK_LO    0xFF802FC0u              /* first 32-byte line above 0xFF802FB4 */
#define KN_ASTACK_HI    0xFF8039C0u              /* 2,560 B; R246's kernel stack keeps 0xFF8039C0..0xFF803FB0 (1,520 B) */
#define KN_ASTACK_BYTES (KN_ASTACK_HI - KN_ASTACK_LO)
#define KN_ASTACK_RED   512u                     /* bottom guard: a use this deep trips it, 512 B before any damage */
#define KN_ASTACK_CAP   128u                     /* top guard */
#define KN_ASTACK_FILL  0xA5C35A3Cu
#define KN_ASTACK_USABLE (KN_ASTACK_BYTES - KN_ASTACK_RED - KN_ASTACK_CAP)

enum { KN_AS_OFF, KN_AS_ON, KN_AS_TRIP, KN_AS_NONE };   /* none: page 7 never filled it */

/* run fn(a, b, c) on the L1 stack when kn_astack_top != 0 (and SP isn't on it already), else on the current one */
void kn_astack_run(void (*fn)(void *, void *, void *), void *a, void *b, void *c);
extern void *volatile kn_astack_top;             /* the SP to start from; 0 = off (kn_astack_run reads it) */
extern uint32_t kn_astack_lo;                    /* the region's first byte (nesting test) */

void kn_astack_mark(void);                       /* thread level (page 7 push / open): fill the region */
int kn_astack_enable(int on);                    /* page 7's switch: -> KN_AS_* */
void kn_astack_check(void);                      /* ISR, after the callback: both guards (only while on) */
uint32_t kn_astack_state(void);
uint32_t kn_astack_used(void);                   /* thread level: deepest use since the last fill (bytes below the SP top) */

#ifdef OP1_HOST
extern uint32_t kn_astack_host[KN_ASTACK_BYTES / 4];   /* stands in for KN_ASTACK_LO.. */
void kn_astack_host_reset(void);
#endif
#endif
