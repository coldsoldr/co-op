/* coop-fw L1 data pool (core/sdram/l1_pool.cc): hot per-block data of the coop stages in L1 data bank B.
 * coop_l1_alloc returns a zeroed, 32-byte aligned block or 0 (no pool: the stock Lander table was not as
 * expected, or the pool is full); callers keep an SDRAM fallback. Nothing is freed. */
#ifndef COOP_L1_H
#define COOP_L1_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int coop_l1_state(void);             /* 0 unchecked, 1 pool taken, 2 refused */
void *coop_l1_alloc(uint32_t bytes);
#ifdef __cplusplus
}
#endif
/* the block, or the caller's SDRAM fallback when there is no pool */
static inline void *coop_l1_or(uint32_t bytes, void *fallback) { void *p = coop_l1_alloc(bytes); return p ? p : fallback; }
#endif
