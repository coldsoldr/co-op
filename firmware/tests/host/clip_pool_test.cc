/*
 * coop-fw host test: the stereo clipboard's right-channel pool (core/sdram/tape_edit.cc, e12).
 *
 * e12 gives a track's old clip back when the track is lifted again: the samples above it move down in the pool and the
 * other slots follow (slot_release). The emulator reaches that move only through the tape page; this test runs the
 * firmware's own function on the PC: tape_edit.cc is included whole (its file calls are never made here), the pool's
 * address range is mapped at its firmware address, and random lifts on the four tracks are checked after every step:
 * every slot still holds exactly the samples it was given, the slots are packed from the pool's base without overlap,
 * a lift that does not fit takes nothing (the mono fallback), and the pool never exceeds its size.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <vector>

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
volatile int32_t g_coop_track_base[4];
static uint32_t g_test_pool_bytes;
extern "C" uint32_t coop_clip_pool_bytes(void) { return g_test_pool_bytes; }
extern "C" void coop_markers_clear(void) {}
#include "../../core/sdram/tape_edit.cc"

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

/* coop_lift's pool lines (the file I/O left out): release the track's old clip, then take n samples if they fit */
static int lift(int ch, uint32_t n, std::vector<int16_t> shadow[TRACKS])
{
    slot_release(ch);
    shadow[ch].clear();
    uint32_t pool = COOP_POOL_SAMPLES;
    int16_t *right = 0;
    if (g_pool_used <= pool && n <= pool - g_pool_used) { right = COOP_POOL + g_pool_used; g_pool_used += n; }
    g_r_slot[ch] = right; g_r_len[ch] = right ? n : 0;
    if (!right) return 0;
    for (uint32_t i = 0; i < n; i++) { int16_t v = (int16_t)rnd(); right[i] = v; shadow[ch].push_back(v); }
    return 1;
}

static int check(const std::vector<int16_t> shadow[TRACKS], const char *what)
{
    uint32_t sum = 0;
    for (int t = 0; t < TRACKS; t++) {
        if (!g_r_slot[t]) { if (g_r_len[t] || !shadow[t].empty()) { printf("FAIL %s: track %d has no slot but a length\n", what, t); return 0; } continue; }
        if (g_r_len[t] != shadow[t].size()) { printf("FAIL %s: track %d length\n", what, t); return 0; }
        uint32_t off = (uint32_t)(g_r_slot[t] - COOP_POOL);
        if (off + g_r_len[t] > g_pool_used) { printf("FAIL %s: track %d outside the used pool\n", what, t); return 0; }
        for (int u = 0; u < t; u++) {
            if (!g_r_slot[u]) continue;
            uint32_t o2 = (uint32_t)(g_r_slot[u] - COOP_POOL);
            if (off < o2 + g_r_len[u] && o2 < off + g_r_len[t]) { printf("FAIL %s: tracks %d and %d overlap\n", what, t, u); return 0; }
        }
        for (uint32_t i = 0; i < g_r_len[t]; i++)
            if (g_r_slot[t][i] != shadow[t][i]) { printf("FAIL %s: track %d sample %u changed\n", what, t, (unsigned)i); return 0; }
        sum += g_r_len[t];
    }
    if (sum != g_pool_used) { printf("FAIL %s: the pool is not packed (%u used, %u in slots)\n", what, (unsigned)g_pool_used, (unsigned)sum); return 0; }
    if (g_pool_used > COOP_POOL_SAMPLES) { printf("FAIL %s: pool over its size\n", what); return 0; }
    return 1;
}

int main(void)
{
    const uint32_t region = 4u << 20;
    void *m = mmap((void *)(uintptr_t)COOP_POOL_LO, region, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)(uintptr_t)COOP_POOL_LO) { printf("clip_pool: cannot map the pool at its address on this host: skipped\n"); return 0; }
    std::vector<int16_t> shadow[TRACKS];
    int fallbacks = 0, lifts = 0;
    /* the floor (1.5 MiB), then the whole region, then a tiny pool */
    const uint32_t sizes[3] = { COOP_CLIP_FLOOR, region, 65536u };
    for (int s = 0; s < 3; s++) {
        g_test_pool_bytes = sizes[s];
        g_pool_used = 0;
        for (int t = 0; t < TRACKS; t++) { g_r_slot[t] = 0; g_r_len[t] = 0; shadow[t].clear(); }
        uint32_t pool = COOP_POOL_SAMPLES;
        for (int step = 0; step < 600; step++) {
            int ch = (int)(rnd() % TRACKS);
            uint32_t n = 1 + rnd() % (pool / 2 + pool / 8);                /* now and then longer than what is left */
            if (step % 97 == 0) n = pool + 1;                              /* longer than the whole pool: never fits */
            if (step % 53 == 0) n = pool - (g_pool_used - g_r_len[ch]);    /* exactly what is left once its old clip goes */
            if (!n) n = 1;
            int ok = lift(ch, n, shadow);
            lifts++;
            if (!ok) fallbacks++;
            if (n > pool && ok) { printf("FAIL: a lift longer than the pool was given room\n"); return 1; }
            if (!check(shadow, "after a lift")) return 1;
        }
    }
    if (g_edit_errors) { printf("FAIL: %u pool errors\n", (unsigned)g_edit_errors); return 1; }
    if (!g_edit_pool_moves || !fallbacks) { printf("FAIL: the test did not reach a move (%u) or a fallback (%d)\n", (unsigned)g_edit_pool_moves, fallbacks); return 1; }
    printf("clip_pool: %d lifts on three pool sizes, %u slot moves, %d did not fit (mono): every slot intact, the pool packed\n",
           lifts, (unsigned)g_edit_pool_moves, fallbacks);
    return 0;
}
