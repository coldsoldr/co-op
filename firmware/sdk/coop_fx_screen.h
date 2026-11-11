/*
 * coop-fw e9: helpers for effect screens (C++, header only), on top of the core's screen calls (sdk/coop_screen.h).
 * They are te-op1-cfw's ports/fx/op1/fx_screen.h and screen_anim.h (what the kennel effect screens used) with every
 * firmware address replaced by a core export, so a kennel screen ports line for line:
 *
 *   kennel / te-op1-cfw                       coop (this header)
 *   DocumentOf(scr), Find(document, id)        Find(scr, id)  (coop_scr_find looks in the screen's document)
 *   FindAll(document, ids, out, n)             FindAll(scr, ids, out, n)
 *   SetVisible / SetTranslate / SetMatrix /    the same names
 *   ScaleX / ShowOne / ShowFirst / Steps
 *   LineStyle / FillStyle (assert on a typo)   LineStyle / FillStyle (0 for an id the SVG lacks: check it)
 *   op1_screen_draw(scr, dt)                   coop_scr_draw(scr, dt)
 *   knl::ScreenPos(), op1_fx_knobs_live        view->slot, view->knobs (sdk/coop_plugin.h coop_fx_view_t)
 *   knl::ScreenWord(5..7), knl::Pressed(latch) view->words[0..2], view->latches
 *   knl::Pressed(pos, k) for a page-2 press    held layers: view->mods (bit 1 fn, bit 0 shift); e6 has no pages
 *   the DSP's volatile meters in .bss          display words in the state: view->state while view->live
 *   op1fx::FrameClock (CYCLES)                 FrameClock (the same: the CYCLES register, no firmware address)
 *   op1fx::SinQ15 (Clouds' 513-entry table)    SinQ15 (a 65-entry quarter wave, interpolated: screens only)
 *
 * GUI thread only; no float, no libc, no static constructors (every object here is plain data).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_FX_SCREEN_H
#define COOP_FX_SCREEN_H
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_screen.h"

namespace coop_scr {

inline void* Find(void* scr, const char* id) { return coop_scr_find(scr, id); }

// Every node of a set found (a screen checks its ids before using any).
inline bool FindAll(void* scr, const char* const* ids, void** out, int32_t n) {
  for (int32_t i = 0; i < n; ++i) {
    out[i] = coop_scr_find(scr, ids[i]);
    if (!out[i]) return false;
  }
  return true;
}

inline void SetVisible(void* node, bool visible) { coop_scr_visible(node, visible ? 1 : 0); }

inline void SetTranslate(void* node, int32_t tx_q10, int32_t ty_q10) {
  const int32_t m[9] = { 0x400, 0, 0, 0, 0x400, 0, tx_q10, ty_q10, 0x400 };
  coop_scr_transform(node, m);
}

// SVG's matrix(a, 0, 0, d, e, f), each Q10 (e, f in units x 1024).
inline void SetMatrix(void* node, int32_t a, int32_t d, int32_t e, int32_t f) {
  const int32_t m[9] = { a, 0, 0, 0, d, 0, e, f, 0x400 };
  coop_scr_transform(node, m);
}

// The full matrix(a, b, c, d, e, f), Q10.
inline void SetMatrix6(void* node, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
  const int32_t m[9] = { a, b, 0, c, d, 0, e, f, 0x400 };
  coop_scr_transform(node, m);
}

// Scale x by s (Q10) about x = anchor (units).
inline void ScaleX(void* node, int32_t s, int32_t anchor) { SetMatrix(node, s, 0x400, anchor * (0x400 - s), 0); }

inline uint8_t* LineStyle(void* scr, const char* id) { return coop_scr_line_rgba(scr, id); }
inline uint8_t* FillStyle(void* scr, const char* id) { return coop_scr_fill_rgba(scr, id); }

// A variant set: only member k shown (none for k out of range).
inline void ShowOne(void* const* set, int32_t n, int32_t k) {
  for (int32_t i = 0; i < n; ++i) SetVisible(set[i], i == k);
}

// A count set: the first `count` shown.
inline void ShowFirst(void* const* set, int32_t n, int32_t count) {
  for (int32_t i = 0; i < n; ++i) SetVisible(set[i], i < count);
}

// round(v / 32767 x steps), v in 0..32767.
inline int32_t Steps(int32_t v, int32_t steps) { return (v * steps + 16383) / 32767; }

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

struct Colour {
  uint8_t rgba[4];
};

inline void ReadColour(Colour* c, const uint8_t* style) {
  for (int32_t i = 0; i < 4; ++i) c->rgba[i] = style ? style[i] : 0;
}

inline void WriteColour(uint8_t* style, const Colour& c) {
  if (!style) return;
  for (int32_t i = 0; i < 4; ++i) style[i] = c.rgba[i];
}

// From a to b by v (0..32767), each byte on its own.
inline Colour Blend(const Colour& a, const Colour& b, int32_t v) {
  Colour c;
  for (int32_t i = 0; i < 4; ++i) c.rgba[i] = static_cast<uint8_t>(a.rgba[i] + (((b.rgba[i] - a.rgba[i]) * v) >> 15));
  return c;
}

// The mix bar the kennel designs share: rect x 232, 56 units wide, scaled from its left end (at least 0.02).
inline void SetMixBar(void* node, int32_t v15) {
  int32_t s = v15 >> 5;
  if (s < 20) s = 20;
  ScaleX(node, s, 232);
}

// ---- motion (screen_anim.h) ----

// sin(2 pi phase / 2^32) in Q15: a quarter wave of 65 entries, interpolated (about 0.1 % off: a screen's motion).
inline int32_t SinQ15(uint32_t phase) {
  static const int16_t kQuarter[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767 };
  const uint32_t q = phase >> 30, f = phase & 0x3fffffffu;   // the quadrant, the position in it
  uint32_t x = (q & 1) ? 0x40000000u - f : f;                  // 0..2^30 across the quarter
  const int32_t i = static_cast<int32_t>(x >> 24);             // 0..64
  const int32_t frac = static_cast<int32_t>((x >> 8) & 0xffff);
  const int32_t a = kQuarter[i], b = kQuarter[i < 64 ? i + 1 : 64];
  const int32_t v = a + (((b - a) * frac) >> 16);
  return (q & 2) ? -v : v;
}

inline int32_t CosQ15(uint32_t phase) { return SinQ15(phase + 0x40000000u); }

inline uint32_t Cycles() {
#ifdef __bfin__
  uint32_t v;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
  return v;
#else
  return 0;                                      // host builds (tests/host): the screen code compiles, never animates
#endif
}

// The time between frames, in CPU cycles (at most a fifth of a second, so a screen shown again doesn't jump).
struct FrameClock {
  uint32_t last;
  uint32_t Tick() {
    const uint32_t now = Cycles();
    const uint32_t d = now - last;
    last = now;
    return d > 80000000u ? 80000000u : d;
  }
};

// A phase turning at hz_q16 Hz (Q16) for `cycles` CPU cycles at 400 MHz (screen_anim.h PhaseStep).
inline uint32_t PhaseStep(int32_t hz_q16, uint32_t cycles) {
  const uint64_t x = (static_cast<uint64_t>(hz_q16) * cycles) >> 8;
  return static_cast<uint32_t>((x * 703687u) >> 24);
}

// The input gain word (Q15 over -6..+6 dB) in tenths of a dB, -60..60 (kennel's readout).
inline int32_t GainTenths(int32_t word) { return (Clamp15(word) * 120 + 16383) / 32767 - 60; }

// ---- display data (sdk/coop_plugin.h "Display data") ----

// The display words the render published, or 0 while the state is not this effect's (or too small for them).
template <class T>
inline const volatile T* Published(const coop_fx_view_t* v, uint32_t offset = 0) {
  if (!v || !v->live || !v->state || v->state_bytes < offset + sizeof(T)) return 0;
  return reinterpret_cast<const volatile T*>(reinterpret_cast<const volatile uint8_t*>(v->state) + offset);
}

}  // namespace coop_scr

#endif
