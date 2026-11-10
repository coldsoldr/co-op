/*
 * rings' screen (docs/engines.md "Screens"), te-op1-cfw's ports/knl/rings/rings_screen_knl.cc on the coop screen calls
 * (sdk/coop_screen.h). The drawing is the kennel one (rings.svg, gen_rings_knl_svg.py, the canvas "Rings kennel
 * screen"); what changed is where its inputs come from: the knob words from coop_scr_words instead of kennel's
 * addresses, and the MODEL / POLY highlight follows fn held (coop_scr_mods: fn + blue and fn + green move them)
 * instead of kennel's page-2 toggle. Every layer is held (guidelines §4).
 *
 *   ring0..ring5   lit from the core out by structure (1..6); dimmed while fn is held
 *   ray0..ray7     brightness (0..8 lit), flashing white with the loudest voice; dimmed while fn is held
 *   pad            damping: drawn deep in, lifts off the rings as damping rises
 *   pos            position: the strike dot, out along the radius
 *   rip0..rip5     one ripple per voice, scaled out from the strike with its age, faded with its level
 *   core           the glow: brightness sets its rest, the voices flare it
 *   mlabelN, modelN, mbox / plabelN, voiceN, pbox   the MODEL and POLY rows; bracketed with a white label while fn
 *                  is held
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <cstdint>

#include "coop_plugin.h"
#include "coop_screen.h"
#include "op1/settings.h"

extern "C" void RingsScreenVoices(int16_t* level, uint16_t* age);   // rings.cc

namespace {

const int32_t kVoices = 6;
const int32_t kRays = 8;
const int32_t kCX = 104, kCY = 72;
const int32_t kRippleDrawn = 30;      // the ripple circle's radius in the file
const int32_t kRippleEnd = 58;        // hidden past this radius
const int32_t kRippleTravel = 54;     // the core to the rim
const int32_t kLevelThreshold = 160;  // a voice below this (peak, int16) is quiet

const char* const kRingIds[6] = { "ring0", "ring1", "ring2", "ring3", "ring4", "ring5" };
const char* const kRayIds[kRays] = { "ray0", "ray1", "ray2", "ray3", "ray4", "ray5", "ray6", "ray7" };
const char* const kRippleIds[kVoices] = { "rip0", "rip1", "rip2", "rip3", "rip4", "rip5" };
const char* const kModelIds[6] = { "model0", "model1", "model2", "model3", "model4", "model5" };
const char* const kModelLabelIds[6] = { "mlabel0", "mlabel1", "mlabel2", "mlabel3", "mlabel4", "mlabel5" };
const char* const kVoiceIds[6] = { "voice0", "voice1", "voice2", "voice3", "voice4", "voice5" };
const char* const kPolyLabelIds[4] = { "plabel0", "plabel1", "plabel2", "plabel3" };
const char* const kNodeIds[5] = { "pad", "pos", "core", "mbox", "pbox" };
const char* const kPaletteIds[7] = { "palwhite", "paldim", "palblue", "palgreen", "paldimgreen", "palgrey", "palblack" };
enum { N_PAD, N_POS, N_CORE, N_MBOX, N_PBOX };
enum { C_WHITE, C_DIM, C_BLUE, C_GREEN, C_DIMGREEN, C_GREY, C_BLACK };

struct Colour {
  uint8_t rgba[4];
};

bool ready;
uint8_t* ring_styles[6];
uint8_t* ray_styles[kRays];
uint8_t* ripple_styles[kVoices];
uint8_t* model_line_styles[6];
uint8_t* model_fill_styles[6];
uint8_t* model_label_styles[6];
uint8_t* voice_styles[6];
uint8_t* poly_label_styles[4];
uint8_t* core_fill;
void* ripple_nodes[kVoices];
void* model_label_nodes[6];
void* poly_label_nodes[4];
void* nodes[5];
Colour pal[7];

void ReadColour(Colour* c, const uint8_t* style) {
  for (int32_t i = 0; i < 4; ++i) c->rgba[i] = style[i];
}

void WriteColour(uint8_t* style, const Colour& c) {
  for (int32_t i = 0; i < 4; ++i) style[i] = c.rgba[i];
}

// From a to b by v (0..32767), each byte on its own.
Colour Blend(const Colour& a, const Colour& b, int32_t v) {
  Colour c;
  for (int32_t i = 0; i < 4; ++i) c.rgba[i] = static_cast<uint8_t>(a.rgba[i] + (((b.rgba[i] - a.rgba[i]) * v) >> 15));
  return c;
}

inline int32_t Knob(int16_t k) { return k < 0 ? 0 : k; }
inline int32_t Clamp15(int32_t v) { return v > 32767 ? 32767 : v; }

// SVG's matrix(a, 0, 0, d, e, f), Q10.
void SetMatrix(void* node, int32_t a, int32_t d, int32_t e, int32_t f) {
  int32_t m[9] = { a, 0, 0, 0, d, 0, e, f, 0x400 };
  coop_scr_transform(node, m);
}

void SetTranslate(void* node, int32_t tx_q10, int32_t ty_q10) { SetMatrix(node, 0x400, 0x400, tx_q10, ty_q10); }

// Scale by s (Q10) about the centre.
void ScaleAbout(void* node, int32_t s) { SetMatrix(node, s, s, kCX * (0x400 - s), kCY * (0x400 - s)); }

// A variant set: only member k shown.
void ShowOne(void* const* set, int32_t n, int32_t k) {
  for (int32_t i = 0; i < n; ++i) coop_scr_visible(set[i], i == k);
}

// Every node of a set found, and each one's style where asked: the screen checks its ids before using any.
bool FindAll(void* scr, const char* const* ids, void** out, int32_t n) {
  for (int32_t i = 0; i < n; ++i) {
    out[i] = coop_scr_find(scr, ids[i]);
    if (!out[i]) return false;
  }
  return true;
}

bool Styles(uint8_t** out, void* scr, const char* const* ids, int32_t n, bool fill) {
  for (int32_t i = 0; i < n; ++i) {
    out[i] = fill ? coop_scr_fill_rgba(scr, ids[i]) : coop_scr_line_rgba(scr, ids[i]);
    if (!out[i]) return false;
  }
  return true;
}

void ScreenInit(void* scr) {
  ready = false;
  void* tmp[kRays];
  if (!FindAll(scr, kRingIds, tmp, 6) || !FindAll(scr, kRayIds, tmp, kRays) ||
      !FindAll(scr, kRippleIds, ripple_nodes, kVoices) || !FindAll(scr, kModelIds, tmp, 6) ||
      !FindAll(scr, kModelLabelIds, model_label_nodes, 6) || !FindAll(scr, kVoiceIds, tmp, 6) ||
      !FindAll(scr, kPolyLabelIds, poly_label_nodes, 4) || !FindAll(scr, kNodeIds, nodes, 5) ||
      !FindAll(scr, kPaletteIds, tmp, 7)) {
    return;
  }
  uint8_t* palette[7];
  if (!Styles(palette, scr, kPaletteIds, 7, false) || !Styles(ring_styles, scr, kRingIds, 6, false) ||
      !Styles(model_line_styles, scr, kModelIds, 6, false) || !Styles(model_fill_styles, scr, kModelIds, 6, true) ||
      !Styles(model_label_styles, scr, kModelLabelIds, 6, false) || !Styles(voice_styles, scr, kVoiceIds, 6, false) ||
      !Styles(ripple_styles, scr, kRippleIds, kVoices, false) || !Styles(ray_styles, scr, kRayIds, kRays, false) ||
      !Styles(poly_label_styles, scr, kPolyLabelIds, 4, false)) {
    return;
  }
  for (int32_t i = 0; i < 7; ++i) ReadColour(&pal[i], palette[i]);
  core_fill = coop_scr_fill_rgba(scr, "core");
  if (!core_fill) return;
  ready = true;
}

void ScreenUpdate(void* scr, int dt) {
  if (ready) {
    int16_t words[8];
    coop_scr_words(words);
    const int32_t structure = Knob(words[0]);
    const int32_t brightness = Knob(words[1]);
    const int32_t damping = Knob(words[2]);
    const int32_t position = Knob(words[3]);
    const bool fn_layer = (coop_scr_mods() & 2) != 0;   // fn held: blue moves the model, green the polyphony

    int16_t level[kVoices];
    uint16_t age[kVoices];
    RingsScreenVoices(level, age);
    int32_t loudest = 0, sum = 0, sounding = 0;
    for (int32_t v = 0; v < kVoices; ++v) {
      const int32_t l = level[v] < 0 ? 0 : level[v];
      if (l > loudest) loudest = l;
      sum += l;
      if (l >= kLevelThreshold) ++sounding;
    }
    const int32_t loud15 = Clamp15(loudest * 4);   // a voice at a quarter of full scale is "full"
    const int32_t sum15 = Clamp15(sum * 4);

    // Structure: the rings, lit from the core; all dim while fn is held.
    const int32_t lit = 1 + ((structure * 5 + 16384) >> 15);
    for (int32_t i = 0; i < 6; ++i) WriteColour(ring_styles[i], i < lit && !fn_layer ? pal[C_BLUE] : pal[C_DIM]);

    // Brightness: the rays (0..8 lit), flashing towards white with the loudest voice. round(b x 8 / 32767), by a
    // multiply and a shift (8 x 32767 / 2^15 = 7.9998: the same steps as kennel's division, checked for 0..32767).
    const int32_t rays = (brightness * kRays + 16384) >> 15;
    const Colour ray_lit = Blend(pal[C_GREEN], pal[C_WHITE], loud15);
    for (int32_t i = 0; i < kRays; ++i) WriteColour(ray_styles[i], i < rays && !fn_layer ? ray_lit : pal[C_DIMGREEN]);

    // Damping: the pad lifts 25.5 px out along the diagonal as damping rises (Q10: 25.5 x 1024 / 32767 = 0.797).
    const int32_t pad = (damping * 816) >> 10;
    SetTranslate(nodes[N_PAD], pad, pad);
    // Position: the strike dot, 48 px out along the radius (Q10: 1.5).
    SetTranslate(nodes[N_POS], (position * 1536) >> 10, 0);

    // The core's glow: brightness sets its rest, the voices flare it.
    WriteColour(core_fill, Blend(Blend(pal[C_BLACK], pal[C_GREY], brightness), pal[C_WHITE], sum15));

    // The ripples: r = 4 + age x 54 / 256 (the rim at 0.37 s), faded with the voice and the distance left.
    for (int32_t v = 0; v < kVoices; ++v) {
      const int32_t l = level[v] < 0 ? 0 : level[v];
      const int32_t r = 4 + (((age[v] > 255 ? 255 : age[v]) * kRippleTravel) >> 8);
      const bool shown = l >= kLevelThreshold && r < kRippleEnd;
      coop_scr_visible(ripple_nodes[v], shown);
      if (!shown) continue;
      ScaleAbout(ripple_nodes[v], (r * 0x400) / kRippleDrawn);
      const int32_t k = (Clamp15(l * 4) * (kRippleEnd - r)) / kRippleTravel;
      WriteColour(ripple_styles[v], Blend(pal[C_BLACK], pal[C_WHITE], k));
    }

    // MODEL row.
    const int32_t model = op1::ModelSetting(words);
    ShowOne(model_label_nodes, 6, model);
    WriteColour(model_label_styles[model], fn_layer ? pal[C_WHITE] : pal[C_BLUE]);
    for (int32_t i = 0; i < 6; ++i) {
      const bool on = i == model;
      WriteColour(model_line_styles[i], on || fn_layer ? pal[C_BLUE] : pal[C_DIM]);
      WriteColour(model_fill_styles[i], on ? pal[C_BLUE] : pal[C_BLACK]);
    }
    coop_scr_visible(nodes[N_MBOX], fn_layer);

    // POLY row: a tick per voice the setting allows, bright green while that many voices sound, dim green at rest,
    // near black beyond the setting. While fn is held, the setting itself: the allowed ticks bright, the rest grey.
    const int32_t poly = op1::PolySetting(words);
    const int32_t voices = op1::kPolyVoices[poly];
    ShowOne(poly_label_nodes, 4, poly);
    WriteColour(poly_label_styles[poly], fn_layer ? pal[C_WHITE] : pal[C_GREEN]);
    for (int32_t i = 0; i < 6; ++i) {
      const Colour& c = fn_layer ? (i < voices ? pal[C_GREEN] : pal[C_GREY])
                        : i < voices ? (i < sounding ? pal[C_GREEN] : pal[C_DIMGREEN])
                                     : pal[C_DIM];
      WriteColour(voice_styles[i], c);
    }
    coop_scr_visible(nodes[N_PBOX], fn_layer);
  }
  coop_scr_draw(scr, dt);
}

}  // namespace

extern "C" const coop_screen_t SCREEN_rings = { ScreenInit, ScreenUpdate };
