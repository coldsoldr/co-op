/* OP-1 firmware R246 addresses used by coop-fw's own code.
 *
 * Every entry cites where it was confirmed. [C] = seen in bytes or on the device; [I] = inferred.
 * Sources: te-op1-cfw docs, te-op1-cfw/third_party/opus-kit/sdk/op1fw.h, the vendored kennel 3.1 core.
 * Generated L1 addresses (the decrypted audio core) will come from symbols/l1.sym once Phase 1 runs;
 * this header holds only the plaintext-SDRAM and MMR addresses known before the dump.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE, NOTICE).
 */
#ifndef COOP_FW_ADDRS_H
#define COOP_FW_ADDRS_H
#include <stdint.h>

/* ---- the phase engine voice vtable (plain SDRAM): the injection hook point ---- */
#define FW_PHASE_VTABLE_RENDER   0x01911150u   /* [C] engine-interface.md; stock 0x01a0bc5c */
#define FW_PHASE_RENDER_STOCK    0x01a0bc5cu
#define FW_PHASE_VTABLE_NOTEON   0x01911158u   /* stock 0x01a0be82 */
#define FW_PHASE_VTABLE_NOTEOFF  0x01911168u   /* stock 0x01a1b5ec */

/* ---- te-boot context block at the top of SDRAM (the app reads it) ---- */
#define FW_TEBOOT_CONTEXT        0x03ffff00u   /* [C] app-boot-chain.md; +0x10 holds the word-swapped OTP page 0xD0 */
#define FW_OTP_KEY_IN_CONTEXT    0x03ffff10u   /* [I] key copied here by the init DXE (0xffa00514..) */

/* ---- the ROM decrypt callback's data block (survives app load) ---- */
#define FW_ROM_CB_DATA           0xff804000u   /* [C] app-boot-chain.md: 188 B, key at ctx+0x10 originally read here */

/* ---- L1 regions (addresses, not contents) ---- */
#define L1_INSTR_BASE            0xffa00000u   /* [C] 48 KB instruction SRAM, DMA-only readable */
#define L1_INSTR_SIZE            0x0000c000u
#define L1_DATA_A_BASE           0xff800000u   /* [C] 12 KB loaded (incl. CPLB tables, stacks) */
#define L1_DATA_A_SIZE           0x00003000u
#define L1_DATA_B_BASE           0xff900000u   /* [C] 32 KB */
#define L1_DATA_B_SIZE           0x00008000u
#define L1_SCRATCH_BASE          0xffb00000u   /* [C] 4 KB scratchpad (voice/LFO/FX objects) */
#define L1_SCRATCH_SIZE          0x00001000u

/* ---- memory-to-memory DMA ----
 * MDMA0 (`0xffc00f00` dest / `0xffc00f40` src) is the firmware's display-copy engine [C]: it is in
 * constant use and must NOT be touched, or the display DMA breaks and the device hangs on the
 * loading screen (observed 2026-11-01). MDMA1 (`0xffc00f80` dest / `0xffc00fc0` src) has no
 * references in the disassembly [C], so the instruction-SRAM reader uses MDMA1. Channel register
 * offsets (BF52x HRM): START_ADDR +0x04, CONFIG +0x08, X_COUNT +0x10, X_MODIFY +0x14,
 * IRQ_STATUS +0x28. */
#define MDMA1_D_START_ADDR       0xffc00f84u   /* MDMA1 destination channel */
#define MDMA1_D_CONFIG           0xffc00f88u
#define MDMA1_D_X_COUNT          0xffc00f90u
#define MDMA1_D_X_MODIFY         0xffc00f94u
#define MDMA1_D_IRQ_STATUS       0xffc00fa8u
#define MDMA1_S_START_ADDR       0xffc00fc4u   /* MDMA1 source channel */
#define MDMA1_S_CONFIG           0xffc00fc8u
#define MDMA1_S_X_COUNT          0xffc00fd0u
#define MDMA1_S_X_MODIFY         0xffc00fd4u

/* ---- firmware functions in plain SDRAM (op1fw.h; verified addresses) ---- */
#define FW_YAFFS_OPEN            0x01A997F4u   /* int(const char* path, int oflag, int mode); O_RDWR 2, O_CREAT 0x40, O_TRUNC 0x200 */
#define FW_YAFFS_WRITE           0x01A99F84u   /* int(int fd, const void* buf, unsigned n) */
#define FW_YAFFS_CLOSE           0x01A99908u   /* int(int fd) */
#define FW_YAFFS_LSEEK           0x01A9A0E4u   /* int(int fd, int off, int whence) */
#define FW_MIDI_SEND             0x019E0D18u   /* void(int unused, const uint32_t* packets, int count); 4-byte USB-MIDI packets */
#define FW_USB_CONFIGURED        0x01A69360u   /* int(void): nonzero once the host configured USB */
#define FW_DCACHE_FLUSH          0x01AA185Cu   /* [C] op1fw.h fw_dcache_flush */

/* ---- audio callback seam (plain SDRAM imm16 pair installing the DMA callback) ---- */
#define FW_AUDIO_CB_IMM          0x0191AB52u   /* [C] kennel make_firmware.py IMM_PATCHES sm_audio_cb */
#define FW_AUDIO_CB_TARGET       0xFFA04804u   /* the L1 audio callback it points at */

/* ---- main-loop service CALL sites (safe place to do thread-context work like file writes) ---- */
#define FW_CI_SERVICE_CALL       0x019F0A24u   /* [C] kennel: CALL tape_c::service(0) in the main loop */

#endif  /* COOP_FW_ADDRS_H */
