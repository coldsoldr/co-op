/* cs_hall's screen (e9, docs/plugins.md "e9"): te-op1-cfw's kennel screen (ports/knl/cs_hall/hall_knl.cc ScreenInit /
 * ScreenUpdate, the firmware build's hall.svg) on the coop effect-screen calls (sdk/coop_fx_screen.h). The drawing
 * and its maths are kennel's; what changed (README "Screen"):
 *   - inputs: the four turns from view->knobs (kennel's op1_fx_knobs_live), predelay / early / width from
 *     view->words[0..2] (words 5..7), freeze from view->latches bit 0 (kennel's Pressed(pos, 3) latch); kennel's
 *     screen read nothing from the DSP, so the render publishes nothing;
 *   - kennel's page 2 (AnyPage2: any of the three page knobs pressed) is fn held (view->mods bit 1): the fn layer
 *     (page2) shows and the plain turns' parts (page1) give way to the hall dimmed to its body tone, as kennel did;
 *   - kennel's page indicators are gone (fn is held, not a page); the freeze star lights orange (orange's click; kennel
 *     used decay's green); the motion marks are fixed art (motion is fixed in this build);
 *   - width moves at every position (coop runs the stereo render everywhere; kennel showed it at the master only).
 *
 *   size, room, refl   the hall (blue), its dimmed copy on the fn layer and the reflection path, scaled about the
 *                      floor's centre (150, 84) by size, 0.4..1.0
 *   decay, damp        the envelopes (green, white), stretched from the pre-delay's end x0 by decay; damp shortens the
 *                      white one; full length while infinite (decay at the top or frozen), decayinf shown then
 *   mix                the orange bar, scaled from its left end
 *   predelay           blue on the fn layer: the gap from 27 to x0 (x0 = 34 + predelay x 50 units)
 *   early, late        green on the fn layer: the early taps' height and the late tail's height by early, moved to x0
 *   widthL, widthR     white on the fn layer: apart by up to 14 units each way
 *   freeze             the star: body tone, orange while the latch is on
 *
 * GUI thread only. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"

namespace {

using namespace coop_scr;

const int32_t kHallInfinite = 32512;   // hall.h: decay at and above is infinite (no loss)

const char* const kNodeIds[] = { "page1", "page2", "size", "room", "refl", "decay", "decayinf", "damp",
                                 "mix", "predelay", "early", "late", "widthL", "widthR", "freeze", "palorange" };
enum { N_PAGE1, N_PAGE2, N_SIZE, N_ROOM, N_REFL, N_DECAY, N_DECAYINF, N_DAMP, N_MIX, N_PREDELAY,
       N_EARLY, N_LATE, N_WIDTHL, N_WIDTHR, N_FREEZE, N_PALORANGE, N_COUNT };
static_assert(sizeof(kNodeIds) / sizeof(kNodeIds[0]) == N_COUNT, "node ids");

bool ready;
void* nodes[N_COUNT];
uint8_t* freeze_line;
Colour freeze_off, freeze_on;

void ScreenInit(void* scr) {
  ready = false;
  if (!FindAll(scr, kNodeIds, nodes, N_COUNT)) return;
  freeze_line = LineStyle(scr, "freeze");
  const uint8_t* on = LineStyle(scr, "palorange");
  if (!freeze_line || !on) return;
  ReadColour(&freeze_off, freeze_line);
  ReadColour(&freeze_on, on);
  ready = true;
}

inline int32_t Px(int32_t v15, int32_t k) { return (v15 * k) >> 5; }

void ScreenUpdate(void* scr, int dt, const coop_fx_view_t* v) {
  if (ready) {
    const int32_t size = Clamp15(v->knobs[0]), decay = Clamp15(v->knobs[1]), damp = Clamp15(v->knobs[2]);
    const int32_t mix = Clamp15(v->knobs[3]);
    const int32_t predelay = Clamp15(v->words[0]), early = Clamp15(v->words[1]), width = Clamp15(v->words[2]);
    const bool frozen = (v->latches & 1) != 0;       // the orange click's latch (kennel: Pressed(pos, 3))
    const bool fn = (v->mods & 2) != 0;              // the fn layer (kennel: AnyPage2(pos, 0x7))
    const bool infinite = frozen || decay >= kHallInfinite;

    SetVisible(nodes[N_PAGE1], !fn);
    SetVisible(nodes[N_PAGE2], fn);
    WriteColour(freeze_line, frozen ? freeze_on : freeze_off);

    const int32_t sz = 410 + ((size * 614) >> 15);
    static const int32_t kHallNodes[3] = { N_SIZE, N_ROOM, N_REFL };
    for (int32_t n : kHallNodes) SetMatrix(nodes[n], sz, sz, 150 * (0x400 - sz), 84 * (0x400 - sz));
    const int32_t x0 = (34 << 10) + Px(predelay, 50);
    const int32_t sd = infinite ? 0x400 : 154 + ((decay * 870) >> 15);
    SetMatrix(nodes[N_DECAY], sd, 0x400, x0 - 34 * sd, 0);
    SetVisible(nodes[N_DECAYINF], infinite);
    const int32_t sh = (sd * (1024 - ((damp * 819) >> 15))) >> 10;
    SetMatrix(nodes[N_DAMP], sh, 0x400, x0 - 34 * sh, 0);
    SetMixBar(nodes[N_MIX], mix);

    int32_t sp = (x0 - (30 << 10)) >> 3;
    if (sp < 0) sp = 0;
    ScaleX(nodes[N_PREDELAY], sp, 27);
    const int32_t se = 205 + ((early * 819) >> 15);
    SetMatrix(nodes[N_EARLY], 0x400, se, x0 - (34 << 10), 134 * (0x400 - se));
    const int32_t sl = 0x400 - ((early * 717) >> 15);
    SetMatrix(nodes[N_LATE], sd, sl, x0 - (34 << 10) + 70 * (0x400 - sd), 134 * (0x400 - sl));
    const int32_t w = Px(width, 14);
    SetTranslate(nodes[N_WIDTHL], -w, 0);
    SetTranslate(nodes[N_WIDTHR], w, 0);
  }
  coop_scr_draw(scr, dt);
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_cs_hall = { ScreenInit, ScreenUpdate };
