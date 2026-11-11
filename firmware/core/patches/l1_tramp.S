/* L1 trampoline: bridges an L1 CALL to a coop SDRAM function (L1 cannot reach SDRAM by pcrel24).
 * Placed in free L1 (0xffa0bfb4, kennel's .l1tramp range). Saves RETS across the indirect call so the
 * retargeted call site returns correctly. The P0 address is filled in by tools/l1_insert.py. */
    .text
    .global _coop_l1_tramp
_coop_l1_tramp:
    [--SP] = RETS;
    P0.L = 0x0000;      /* <- coop_master_apply low 16  (filled by l1_insert.py) */
    P0.H = 0x0000;      /* <- coop_master_apply high 16 */
    CALL (P0);
    RETS = [SP++];
    RTS;
