/* cs_plate's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/cs_plate/plate_knl.cc
 * ScreenInit / ScreenUpdate, the firmware build's plate.svg animated as fx/op1/plate_fx.cc) on the coop effect-screen
 * calls (sdk/coop_fx_screen.h). The drawing and its maths are kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's op1_fx_knobs_live), size / diffusion / motion from
 *     view->words[0..2] (kennel's ScreenWord(5..7); size is the step index 0..7 here, kennel quantised a Q15 word),
 *     freeze from view->latches bit 0 (kennel's Pressed(pos, 3)). Kennel's screen read nothing from the DSP, so the
 *     render publishes nothing;
 *   - kennel's page 2 (any of the three page knobs pressed) is fn held (view->mods bit 1): the fn layer's marks show
 *     (size outline, diffusion bars, motion zigzags) and the parts the plain blue / green / white turns own dim to the
 *     body tone in place (kennel swapped the whole page and drew a dim copy of the decay curve). The plate's dim
 *     outline gives way to the size outline, as on kennel's page 2. Orange has no fn layer: the mix bar and the freeze
 *     star stay as they are;
 *   - the page dots and the fixed low-cut mark are gone (README).
 *
 *   plate / size     the plate outline at rest / the size outline under fn, scaled about the plate's centre by size
 *   ring1..ring4     count set by decay (1..4; all four while frozen)
 *   predelay         the gap before the plate, scaled from x 27 by pre-delay; the envelopes start at its end
 *   decay, damp      the envelopes, stretched from their start by decay; damp's shortened by damp
 *   diff1..diff8     count set by diffusion (1..8), under fn, following the pre-delay
 *   motionL, R       the zigzags at the plate's edges, scaled by motion, under fn
 *   mix              the mix bar (SetMixBar)
 *   freeze           the star: body tone, orange while frozen
 *
 * GUI thread only, no per-position state (nothing animates). Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the
 * OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"

namespace {

using namespace coop_scr;

const char* const kRingIds[4] = { "ring1", "ring2", "ring3", "ring4" };
const char* const kDiffIds[8] = { "diff1", "diff2", "diff3", "diff4", "diff5", "diff6", "diff7", "diff8" };
const char* const kNodeIds[] = { "plate", "size", "predelay", "decay", "damp", "mix", "motionL", "motionR", "freeze",
                                 "palorange" };
enum { N_PLATE, N_SIZE, N_PREDELAY, N_DECAY, N_DAMP, N_MIX, N_MOTIONL, N_MOTIONR, N_FREEZE, N_PALORANGE, N_COUNT };
static_assert(sizeof(kNodeIds) / sizeof(kNodeIds[0]) == N_COUNT, "node ids");

bool ready;
void* nodes[N_COUNT];
void* ring_nodes[4];
void* diff_nodes[8];
uint8_t* ring_line[4];
uint8_t* decay_line;
uint8_t* damp_line;
uint8_t* predelay_line;
uint8_t* freeze_line;
Colour blue, green, white, orange, dim;

void ScreenInit(void* scr) {
  ready = false;
  if (!FindAll(scr, kNodeIds, nodes, N_COUNT) || !FindAll(scr, kRingIds, ring_nodes, 4) ||
      !FindAll(scr, kDiffIds, diff_nodes, 8)) {
    return;
  }
  for (int32_t i = 0; i < 4; ++i) ring_line[i] = LineStyle(scr, kRingIds[i]);
  decay_line = LineStyle(scr, "decay");
  damp_line = LineStyle(scr, "damp");
  predelay_line = LineStyle(scr, "predelay");
  freeze_line = LineStyle(scr, "freeze");
  ReadColour(&blue, decay_line);
  ReadColour(&green, damp_line);
  ReadColour(&white, predelay_line);
  ReadColour(&dim, freeze_line);
  ReadColour(&orange, LineStyle(scr, "palorange"));
  ready = true;
}

inline int32_t Px(int32_t v15, int32_t k) { return (v15 * k) >> 5; }

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  if (ready) {
    const bool frozen = (v->latches & 1) != 0;          // the orange click's latch (kennel: Pressed(pos, 3))
    const bool fn = (v->mods & 2) != 0;                 // the fn layer (kennel: page 2)
    const int32_t decay = frozen ? 32767 : Clamp15(v->knobs[0]);
    const int32_t damp = Clamp15(v->knobs[1]), predelay = Clamp15(v->knobs[2]), mix = Clamp15(v->knobs[3]);
    int32_t size = v->words[0];                         // the step index 0..7 (kennel: Steps(word 5, 7))
    size = size < 0 ? 0 : (size > 7 ? 7 : size);
    const int32_t diffusion = Clamp15(v->words[1]), motion = Clamp15(v->words[2]);

    SetVisible(nodes[N_PLATE], !fn);
    SetVisible(nodes[N_SIZE], fn);
    for (int32_t i = 0; i < 4; ++i) WriteColour(ring_line[i], fn ? dim : blue);
    WriteColour(decay_line, fn ? dim : blue);
    WriteColour(damp_line, fn ? dim : green);
    WriteColour(predelay_line, fn ? dim : white);
    WriteColour(freeze_line, frozen ? orange : dim);

    const int32_t x0 = (34 << 10) + Px(predelay, 50);
    const int32_t sd = 154 + ((decay * 870) >> 15);
    SetMatrix(nodes[N_DECAY], sd, 0x400, x0 - 34 * sd, 0);
    const int32_t sh = (sd * (1024 - ((damp * 819) >> 15))) >> 10;
    SetMatrix(nodes[N_DAMP], sh, 0x400, x0 - 34 * sh, 0);
    int32_t rings = (decay * 4 + 32766) / 32767;
    if (rings < 1) rings = 1;
    ShowFirst(ring_nodes, 4, rings);
    int32_t sp = (x0 - (30 << 10)) >> 3;
    if (sp < 0) sp = 0;
    ScaleX(nodes[N_PREDELAY], sp, 27);
    SetMixBar(nodes[N_MIX], mix);

    const int32_t ss = 614 + 67 * size;
    SetMatrix(nodes[N_SIZE], ss, ss, 150 * (0x400 - ss), 54 * (0x400 - ss));
    ShowFirst(diff_nodes, 8, fn ? 1 + Steps(diffusion, 7) : 0);
    for (int32_t i = 0; i < 8; ++i) SetTranslate(diff_nodes[i], x0 - (54 << 10), 0);
    SetVisible(nodes[N_MOTIONL], fn);
    SetVisible(nodes[N_MOTIONR], fn);
    ScaleX(nodes[N_MOTIONL], motion >> 5, 56);
    ScaleX(nodes[N_MOTIONR], motion >> 5, 244);
  }
  coop_scr_draw(scr, dt);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_cs_plate = { ScreenInit, ScreenUpdate };
