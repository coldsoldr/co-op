/* Exact 6-byte high-pass prologue interception. Caller stack belongs to Core.
 * GPL-3.0-or-later with OP-1 firmware permission. */
.section .hook_inputbus,"ax"
JUMP.L _kn_input_highpass_entry;
NOP;
.text
.align 2
.global _kn_input_highpass_entry
_kn_input_highpass_entry:
 R0 = SP;
 R0 += 0x34;
 R1 = RETS;
 /* PmSynth keeps its frame pointer in M1 across this call (0xFFA02896: P1 = M1); C code treats M1 as scratch,
  * so the bus and every input engine run inside a frame that saves it. */
 [--SP] = RETS;
 [--SP] = M1;
 SP += -12;
 CALL _kn_input_bus;
 SP += 12;
 M1 = [SP++];
 RETS = [SP++];
 RTS;
.global _kn_input_highpass_stock
_kn_input_highpass_stock:
 [--SP] = (R7:6);
 P0.L = 0xAC14;
 P1.L = 0x74FE;
 P1.H = 0x0196;
 JUMP (P1);
