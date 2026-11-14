/* fickle's screen (e9, docs/plugins.md "e9"): the owner's design "Fickle" (the canvas mock-up's renderVals()): a
 * two-footswitch stompbox in 3/4 view, drawn as wireframe, that wobbles and leans.
 *
 * The projection is orthographic: P(x, y, z) = (160 + x c - y s, kCY - ((x s + y c) sin(el) + z cos(el))), el = 1 rad,
 * yaw th = lean + 0.05 sin(...). Every flat part (the faces, the top face's art, the knob tops, the knob sides) is a
 * group in its own plane's coordinates (gen_fickle_svg.py), moved by ONE screen-space matrix a frame
 * M(O, U, V) = [P(O + U) - P(O), P(O + V) - P(O), P(O)] with unit U and V (SetMatrix6): no nested transforms, never a
 * scale over 1 (whether R246 scales a stroke with its matrix or not, it stays at about 1.5). Dots are translated in
 * screen space. What changes shape every frame is raster lines over the SVG (coop_scr_line).
 *
 *   the clock (white turn)    THE SCREEN'S FRAME RATE: the pose (every motion's time) is sample-and-held at
 *                             4 + 56 x clock^2 Hz, so at a low clock the pedal moves in steps, like a low sample rate;
 *                             the wobble's speed follows the clock too; the big white knob's pointer
 *   the lean                  towards whichever channel is louder (the loop's level against the wet's: display words)
 *   time, modify, mix         the blue, green and orange knobs' pointers (-135 .. +135 degrees)
 *   freeze (orange click)     the left footswitch pressed, its LED lit orange; the wet glyph orange (and still)
 *   loop (white click)        the right footswitch pressed while the loop plays, its LED lit white (blinking at 2 Hz
 *                             while dubbing); the loop ring white, its ticks the loop's envelope (24 bins, a display
 *                             word), the head dot white at the read head. Listening: the ring in white's dark tone, a
 *                             shimmer of ticks in the body tone, the dot in the pencil at the write head
 *   dub (green click)         the green ring inside the loop ring (only while the loop plays: dub acts only then)
 *   the wet mode (fn + blue)  the wet glyph on the left half (pencil): reverb seven dots in a sunflower, spread with
 *                             modify, turning; delay five bars drifting right once per delay time, each smaller and
 *                             dimmer with modify (the feedback); slip a zigzag with an arrowhead (the direction), its
 *                             height with modify's distance from noon
 *   routing (fn + green)      the lever between the halves: leaning to the wet side (in), up (both), to the loop (loop)
 *   the loop mode (fn + white) the glyph in the ring: tape (and ping) two reels whose spokes turn at the speed
 *                             (backwards in reverse); env a spike (taller for hard), green for a moment on a retrigger;
 *                             stretch three strokes (longer for str slow)
 *
 * The core's popup names a control top left while it is turned or clicked; nothing here is text, and the box sits
 * below the popup's corner. GUI thread only; no float, no libc, no static constructors, no divide but a few a frame.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stddef.h>
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx_screen.h"
#include "fickle_tables.h"
#include "state.h"

namespace {

using namespace coop_scr;

/* The geometry (gen_fickle_svg.py), model units; Q6 where the projection takes them. */
const int32_t kW = 78, kD = 55, kH = 32;
const int32_t kCX = 160, kCY = 86;                     /* the box's centre on screen (the mock-up's 80, 6 lower: the
                                                        * back edge clears the popup's corner) */
const int32_t kSinEl = 27574, kCosEl = 17704;          /* sin, cos of the 1 rad elevation, Q15 */
struct Knob {
    int32_t u, v, r, h;
};
const Knob kKnob[6] = { { -54, 34, 9, 8 }, { -22, 34, 9, 8 }, { 16, 32, 13, 10 }, { 54, 34, 9, 8 },
                        { -38, -38, 8, 7 }, { 36, -38, 8, 7 } };
const int32_t kFootDownQ6 = 160;                       /* a pressed footswitch's height, 2.5 units in Q6 */
const int32_t kRingU = 36, kRingV = 2, kRingR = 18;
const int32_t kWetU = -38, kWetV = 2;
const uint32_t kBlinkMs = 250;                         /* the dub blink: 2 Hz */

const uint8_t kBlue[4] = { 0x69, 0x8E, 0xFF, 0xFF }, kGreen[4] = { 0x00, 0xED, 0x95, 0xFF };
const uint8_t kWhite[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, kOrange[4] = { 0xFF, 0x3A, 0x5D, 0xFF };
const uint8_t kPencil[4] = { 0xDF, 0xD9, 0xFF, 0xFF }, kBody[4] = { 0x38, 0x35, 0x4D, 0xFF };
const uint8_t kBlack[4] = { 0x01, 0x01, 0x01, 0xFF };
const uint8_t kWhiteD[4] = { 0x5C, 0x58, 0x70, 0xFF }, kOrangeD[4] = { 0x6E, 0x1A, 0x2B, 0xFF };
const uint8_t *const kKnobCol[4] = { kBlue, kGreen, kWhite, kOrange };

/* sqrt((i + 0.5) / 7) in Q10: the sunflower's radii; i x 2.4 rad as turns (2^32): its angles */
const int32_t kSunR[7] = { 274, 474, 612, 724, 821, 908, 987 };
const uint32_t kSunA[7] = { 0u, 1640555213u, 3281110426u, 626698343u, 2267253556u, 3907808769u, 1253396686u };
/* the routing lever: -0.6, 0, +0.6 rad: sin, cos, Q15 */
const int32_t kLeverSin[3] = { -18501, 0, 18501 }, kLeverCos[3] = { 27044, 32767, 27044 };
/* the reels' turn for each loop mode, half-Hz units, signed (tape speeds; reverse backwards) */
const int16_t kReel[fickle::L_MODES] = { 2, 4, 1, 3, -2, -4, -1, 2, 2, 2, 1, 1, 1 };

const char *const kSideIds[8] = { "s0", "s1", "s2", "s3", "s4u", "s4d", "s5u", "s5d" };
const char *const kTopIds[6] = { "k0", "k1", "k2", "k3", "k4", "k5" };
const char *const kFaceIds[3] = { "front", "side", "top" };
const char *const kPartIds[9] = { "ring", "dubring", "reel1", "reel2", "envs", "envh", "strs", "strl", "head" };
enum { N_RING, N_DUB, N_REEL1, N_REEL2, N_ENVS, N_ENVH, N_STRS, N_STRL, N_HEAD };
const char *const kDotIds[8] = { "tip", "wd0", "wd1", "wd2", "wd3", "wd4", "wd5", "wd6" };
const char *const kDotFillIds[7] = { "wdc0", "wdc1", "wdc2", "wdc3", "wdc4", "wdc5", "wdc6" };

bool ready;
void *faces[3], *sides[8], *tops[6], *parts[9], *dots[8];
uint8_t *ring_line, *ledf_fill, *ledf_line, *ledl_fill, *ledl_line, *head_fill, *wd_fill[7], *envs_line, *envh_line;

/* The motion (one screen object serves every position: it starts again when the shown slot changes). */
int32_t last_slot = -1;
uint32_t real_ms;                                      /* real time (the blink) */
int32_t pose_ms;                                       /* since the pose was taken */
uint32_t wob_ph, rot_ph, drift_ph, spoke_ph;           /* the motions in real time ... */
uint32_t p_wob, p_rot, p_drift, p_spoke, p_step;       /* ... and as the pose holds them */
int32_t lean_ph;                                       /* the lean, in turns (2^32) */
int32_t flash_ms, last_trig;

inline void Copy(uint8_t *dst, const uint8_t *src)
{
    if (!dst) return;
    for (int32_t i = 0; i < 4; ++i) dst[i] = src[i];
}

inline void BlendTo(uint8_t *dst, const uint8_t *a, const uint8_t *b, int32_t k15)
{
    if (!dst) return;
    k15 = Clamp15(k15);
    for (int32_t i = 0; i < 4; ++i) dst[i] = static_cast<uint8_t>(a[i] + (((b[i] - a[i]) * k15) >> 15));
}

/* Out of line: the screen calls them dozens of times a frame (GUI thread: size matters more than a call). */
__attribute__((noinline)) int32_t Sin(uint32_t a) { return SinQ15(a); }
__attribute__((noinline)) int32_t Cos(uint32_t a) { return SinQ15(a + 0x40000000u); }

/* The projection of this frame. */
struct View {
    int32_t c, s;                                      /* cos, sin of the yaw, Q15 */
    /* a model point (Q6 units) on screen, Q10 */
    __attribute__((noinline)) void P(int32_t x, int32_t y, int32_t z, int32_t *out) const
    {
        out[0] = (kCX << 10) + (((x * c - y * s) >> 15) << 4);
        const int32_t depth = (x * s + y * c) >> 15;    /* Q6 */
        out[1] = (kCY << 10) - ((((depth * kSinEl) >> 15) + ((z * kCosEl) >> 15)) << 4);
    }
    /* a unit direction (Q15) on screen, Q10 */
    void Dir(int32_t dx, int32_t dy, int32_t dz, int32_t *out) const
    {
        out[0] = (dx * c - dy * s) >> 20;
        const int32_t depth = (dx * s + dy * c) >> 15;
        out[1] = -((((depth * kSinEl) >> 15) + ((dz * kCosEl) >> 15)) >> 5);
    }
    /* a plane's group: origin (Q6), unit U and V (Q15) */
    __attribute__((noinline)) void Plane(void *node, int32_t ox, int32_t oy, int32_t oz, const int32_t *u, const int32_t *v) const
    {
        int32_t o[2], a[2], b[2];
        P(ox, oy, oz, o);
        Dir(u[0], u[1], u[2], a);
        Dir(v[0], v[1], v[2], b);
        SetMatrix6(node, a[0], a[1], b[0], b[1], o[0], o[1]);
    }
    __attribute__((noinline)) void Line(void *r, int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1, int32_t w,
              const uint8_t *col) const
    {
        int32_t p0[2], p1[2];
        P(x0, y0, z0, p0);
        P(x1, y1, z1, p1);
        coop_scr_line(r, p0, p1, w, col);
    }
};

const int32_t kUx[3] = { 32767, 0, 0 }, kVy[3] = { 0, 32767, 0 }, kVz[3] = { 0, 0, 32767 };

void ScreenInit(void *scr)
{
    ready = false;
    if (!FindAll(scr, kFaceIds, faces, 3) || !FindAll(scr, kSideIds, sides, 8) || !FindAll(scr, kTopIds, tops, 6) ||
        !FindAll(scr, kPartIds, parts, 9) || !FindAll(scr, kDotIds, dots, 8)) {
        return;
    }
    ring_line = LineStyle(scr, "ring");
    ledf_fill = FillStyle(scr, "ledf");
    ledf_line = LineStyle(scr, "ledf");
    ledl_fill = FillStyle(scr, "ledl");
    ledl_line = LineStyle(scr, "ledl");
    head_fill = FillStyle(scr, "headc");
    envs_line = LineStyle(scr, "envs");
    envh_line = LineStyle(scr, "envh");
    for (int32_t i = 0; i < 7; ++i) wd_fill[i] = FillStyle(scr, kDotFillIds[i]);
    if (!ring_line || !ledf_fill || !ledl_fill || !head_fill) return;
    last_slot = -1;
    ready = true;
}

/* A phase step (2^32 a turn) for hz_q16 Hz over dt ms. */
inline uint32_t Step(int32_t hz_q16, int32_t dt) { return PhaseStep(hz_q16, static_cast<uint32_t>(dt) * 400000u); }

inline uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

inline int32_t ClampTo(int32_t v, int32_t n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }
inline int32_t Min(int32_t a, int32_t b) { return a < b ? a : b; }

void ScreenUpdate(void *scr, int dt, const coop_fx_view_t *v)
{
    if (!ready || !v) {
        coop_scr_draw(scr, dt);
        return;
    }
    if (dt < 0) dt = 0;
    if (dt > 200) dt = 200;
    if (v->slot != last_slot) {                        /* another position: the motion starts again */
        last_slot = v->slot;
        real_ms = 0;
        pose_ms = 1 << 20;
        wob_ph = rot_ph = drift_ph = spoke_ph = 0;
        p_rot = p_drift = 0;
        lean_ph = 0;
        flash_ms = 0;
        last_trig = 0;
    }
    const int32_t time = Clamp15(v->knobs[0]), modv = Clamp15(v->knobs[1]), clk = Clamp15(v->knobs[2]);
    const int32_t mix = Clamp15(v->knobs[3]);
    const int32_t wet = ClampTo(v->words[0], fickle::W_MODES);
    const int32_t route = ClampTo(v->words[1], fickle::R_ROUTES);
    const int32_t lmode = ClampTo(v->words[2], fickle::L_MODES);
    const bool freeze = (v->latches & 1) != 0;

    /* the render's display words (sdk/coop_plugin.h "Display data"), or a still pedal before the first render */
    const volatile fickle::Display *d = Published<fickle::Display>(v, offsetof(fickle::State, disp));
    int32_t wet_lvl = 0, loop_lvl = 0, loop_pos = 0, loop_state = 0, clock_hz = 24000, head = 0, trig = 0;
    uint8_t env[fickle::kEnvBins];
    for (int32_t i = 0; i < fickle::kEnvBins; ++i) env[i] = 0;
    if (d) {
        wet_lvl = Clamp15(d->wet_level);
        loop_lvl = Clamp15(d->loop_level);
        loop_pos = Clamp15(d->loop_pos);
        loop_state = d->loop_state < 0 || d->loop_state > 3 ? 0 : d->loop_state;
        clock_hz = d->clock_hz < 2000 ? 2000 : (d->clock_hz > 32000 ? 32000 : d->clock_hz);
        head = Clamp15(d->head);
        trig = d->trig;
        for (int32_t i = 0; i < fickle::kEnvBins / 4; ++i) {
            const uint32_t w = static_cast<uint32_t>(d->env[i]);
            for (int32_t j = 0; j < 4; ++j) env[4 * i + j] = static_cast<uint8_t>(w >> (8 * j));
        }
    }
    const bool playing = loop_state == 1 || loop_state == 2;
    const bool dubbing = loop_state == 2;

    /* ---- time: real, and the pose the clock samples ---- */
    real_ms += static_cast<uint32_t>(dt);
    wob_ph += Step(4174 + ((clk * 16687) >> 15), dt);          /* (0.4 + 1.6 clock) rad/s, as Hz (Q16) */
    rot_ph += Step(3130, dt);                                   /* 0.3 rad/s */
    {                                                           /* the delay: a bar's step a delay time */
        const int32_t dsamp = (head * fickle::kWetPairs) >> 15;
        int32_t hz = dsamp > 0 ? static_cast<int32_t>((static_cast<uint32_t>(clock_hz) << 12) / static_cast<uint32_t>(dsamp)) << 4 : 0;
        if (hz > (8 << 16)) hz = 8 << 16;
        drift_ph += Step(hz, dt);
    }
    spoke_ph += Step(kReel[lmode] * (1 << 15), dt);
    pose_ms += dt;
    const int32_t pose_hz = 4 + ((56 * ((clk * clk) >> 15)) >> 15);
    if (pose_ms * pose_hz >= 1000) {                            /* the clock ticks: a new pose */
        pose_ms = 0;
        p_wob = wob_ph;
        if (!freeze) {
            p_rot = rot_ph;
            p_drift = drift_ph;
        }
        p_spoke = spoke_ph;
        p_step++;
    }
    if (trig != last_trig) {
        last_trig = trig;
        flash_ms = 120;
    }
    flash_ms = flash_ms > dt ? flash_ms - dt : 0;

    /* ---- the yaw: the lean towards the louder channel, and the wobble ---- */
    const int32_t lean_target = (loop_lvl - wet_lvl) * 3338;   /* 0.16 rad for a full level's difference */
    lean_ph += (lean_target - lean_ph) / 20;
    const uint32_t th = static_cast<uint32_t>(lean_ph) + static_cast<uint32_t>(Sin(p_wob) * 1043);   /* 0.05 rad */
    View pv;
    pv.c = Cos(th);
    pv.s = Sin(th);
    const int32_t H6 = kH * 64;

    /* ---- the faces ---- */
    pv.Plane(faces[0], 0, -kD * 64, 0, kUx, kVz);                          /* front: u, z */
    pv.Plane(faces[1], (pv.s < 0 ? kW : -kW) * 64, 0, 0, kVy, kVz);        /* the side the eye sees: v, z */
    pv.Plane(faces[2], 0, 0, H6, kUx, kVy);                                /* top: u, v */

    /* ---- knobs and footswitches: the side across the silhouette (phi = -th), the top ---- */
    const int32_t across[3] = { -pv.c, pv.s, 0 };
    for (int32_t i = 0; i < 6; ++i) {
        const Knob &k = kKnob[i];
        const bool down = (i == 4 && freeze) || (i == 5 && playing);
        const int32_t h6 = down ? kFootDownQ6 : k.h * 64;
        const int32_t ox = (k.u * 64) + ((k.r * pv.c) >> 9), oy = (k.v * 64) - ((k.r * pv.s) >> 9);
        void *side = sides[i < 4 ? i : 4 + 2 * (i - 4) + (down ? 1 : 0)];
        pv.Plane(side, ox, oy, H6, across, kVz);
        if (i >= 4) {
            SetVisible(sides[4 + 2 * (i - 4)], !down);
            SetVisible(sides[5 + 2 * (i - 4)], down);
        }
        pv.Plane(tops[i], k.u * 64, k.v * 64, H6 + h6, kUx, kVy);
    }
    Copy(ledf_fill, freeze ? kOrange : kBlack);
    Copy(ledf_line, freeze ? kOrange : kOrangeD);
    const bool blink_on = !dubbing || ((real_ms / kBlinkMs) & 1) == 0;
    Copy(ledl_fill, playing && blink_on ? kWhite : kBlack);
    Copy(ledl_line, playing ? kWhite : kWhiteD);

    /* ---- the loop ring, the dub ring, the head, the loop mode's glyph ---- */
    Copy(ring_line, playing ? kWhite : kWhiteD);
    SetVisible(parts[N_DUB], dubbing);
    {
        const uint32_t a = static_cast<uint32_t>(loop_pos) << 17;
        int32_t p[2];
        pv.P((kRingU * 64) + ((kRingR * Sin(a)) >> 9), (kRingV * 64) + ((kRingR * Cos(a)) >> 9), H6, p);
        SetTranslate(parts[N_HEAD], p[0], p[1]);
        Copy(head_fill, playing ? kWhite : kPencil);
    }
    const bool tape = lmode <= fickle::L_PING;
    SetVisible(parts[N_REEL1], tape);
    SetVisible(parts[N_REEL2], tape);
    SetVisible(parts[N_ENVS], lmode == fickle::L_ENV_SOFT);
    SetVisible(parts[N_ENVH], lmode == fickle::L_ENV_HARD);
    SetVisible(parts[N_STRS], lmode == fickle::L_STRETCH || lmode == fickle::L_STR_REV);
    SetVisible(parts[N_STRL], lmode == fickle::L_STR_SLOW);
    Copy(envs_line, flash_ms ? kGreen : kWhite);
    Copy(envh_line, flash_ms ? kGreen : kWhite);

    /* ---- the reverb glyph's dots, the lever's tip ---- */
    const uint8_t *wc = freeze ? kOrange : kPencil;
    const int32_t spread = 4 * 1024 + ((modv * 9) >> 5);               /* 4 + 9 modify, Q10 */
    for (int32_t i = 0; i < 7; ++i) {
        SetVisible(dots[1 + i], wet == fickle::W_REVERB);
        if (wet != fickle::W_REVERB) continue;
        const uint32_t a = kSunA[i] + p_rot * static_cast<uint32_t>(1 + i % 3);
        const int32_t rr = (spread * kSunR[i]) >> 14;                  /* Q6 */
        int32_t p[2];
        pv.P((kWetU * 64) + ((rr * Cos(a)) >> 15), (kWetV * 64) + ((rr * Sin(a)) >> 15), H6, p);
        SetTranslate(dots[1 + i], p[0], p[1]);
        BlendTo(wd_fill[i], kBody, wc, 16384 + (Sin(p_rot * 7u + static_cast<uint32_t>(i) * 0x28000000u) >> 1));
    }
    const int32_t tip_x = (10 * kLeverSin[route]) >> 9, tip_z = H6 + ((11 * kLeverCos[route]) >> 9);
    {
        int32_t p[2];
        pv.P(tip_x, 2 * 64, tip_z, p);
        SetTranslate(dots[0], p[0], p[1]);
    }

    coop_scr_draw(scr, dt);

    /* ---- raster lines over the SVG ---- */
    void *r = coop_scr_raster();
    if (!r) return;
    const int32_t vals[4] = { time, modv, clk, mix };                   /* the pointers, -135 .. +135 degrees */
    for (int32_t i = 0; i < 4; ++i) {
        const Knob &k = kKnob[i];
        const uint32_t an = 0xA0000000u + static_cast<uint32_t>(vals[i]) * 98304u;
        const int32_t z = H6 + (k.h * 64), len = (k.r * 26214) >> 9;   /* 0.8 r, Q6 */
        pv.Line(r, k.u * 64, k.v * 64, z, (k.u * 64) + ((len * Sin(an)) >> 15), (k.v * 64) + ((len * Cos(an)) >> 15),
                z, 2048, kKnobCol[i]);
    }
    pv.Line(r, 0, 2 * 64, H6, tip_x, 2 * 64, tip_z, 2048, kGreen);    /* the routing lever */
    for (int32_t i = 0; i < fickle::kEnvBins; ++i) {                   /* the loop ring's ticks */
        const uint32_t a = static_cast<uint32_t>(i) * 178956971u;      /* i / 24 of a turn */
        int32_t amp;                                                   /* Q6 */
        if (playing) amp = 128 + ((env[i] * 320) >> 8);                /* 2 .. 7 units: the envelope */
        else amp = 96 + static_cast<int32_t>(Hash(static_cast<uint32_t>(i) + p_step * 31u) % 192u);   /* 1.5 .. 4.5 */
        const int32_t sn = Sin(a), cs = Cos(a), r0 = 17 * 64, r1 = r0 - amp;
        pv.Line(r, (kRingU * 64) + ((r0 * sn) >> 15), (kRingV * 64) + ((r0 * cs) >> 15), H6,
                (kRingU * 64) + ((r1 * sn) >> 15), (kRingV * 64) + ((r1 * cs) >> 15), H6, 1536, playing ? kPencil : kBody);
    }
    if (tape) {                                                        /* the reels' spokes */
        const int32_t sn = Sin(p_spoke), cs = Cos(p_spoke);
        for (int32_t j = 0; j < 2; ++j) {
            const int32_t cx = (kRingU * 64) + (j ? 288 : -288), cy = kRingV * 64;   /* +-4.5 units */
            pv.Line(r, cx, cy, H6, cx + ((192 * sn) >> 15), cy + ((192 * cs) >> 15), H6, 1536, kWhite);
        }
    }
    if (wet == fickle::W_DELAY) {                                      /* five bars, drifting a step a delay time */
        const int32_t ph = static_cast<int32_t>(p_drift >> 22);        /* 0..1023 */
        const int32_t ratio = 8192 + ((modv * 3) >> 2);                /* 0.25 + 0.75 modify, Q15 */
        const int32_t dim = 13107 + ((modv * 19661) >> 15);            /* 0.4 + 0.6 modify, Q15 */
        int32_t h = 13 * 64, k = 32767;                                /* Q6, Q15 */
        for (int32_t i = 0; i < 5; ++i) {
            const int32_t x = ((kWetU - 14 + 7 * i) * 64) + ((7 * 64 * ph) >> 10);
            if (x < (kWetU + 18) * 64) {
                uint8_t col[4];
                BlendTo(col, kBody, wc, k);
                pv.Line(r, x, (kWetV * 64) - h / 2, H6, x, (kWetV * 64) + h / 2, H6, 1536, col);
            }
            h = (h * ratio) >> 15;
            k = (k * dim) >> 15;
        }
    } else if (wet == fickle::W_SLIP) {                                /* a zigzag and the direction's arrowhead */
        const int32_t step = (modv * fickle::kSlipSteps) >> 15;
        const int32_t dir = step < 5 ? -1 : 1;                         /* kSlipSpeed: the first five play reversed */
        const int32_t dm2 = modv > 16384 ? 2 * (modv - 16384) : 2 * (16384 - modv);   /* 2 |modify - 0.5|, Q15 */
        const int32_t amp = (320 * (13107 + ((19661 * Min(dm2, 32767)) >> 15))) >> 15;   /* 5 (0.4 + 0.6 x), Q6 */
        int32_t px = (kWetU - 16) * 64, py = (kWetV * 64) - amp;
        for (int32_t i = 1; i <= 8; ++i) {
            const int32_t nx = (kWetU - 16 + 4 * i) * 64, ny = (kWetV * 64) + ((i & 1) ? amp : -amp);
            pv.Line(r, px, py, H6, nx, ny, H6, 1536, wc);
            px = nx;
            py = ny;
        }
        const int32_t ax = (kWetU + dir * 18) * 64, gy = kWetV * 64;
        pv.Line(r, ax - dir * 256, gy + 256, H6, ax, gy, H6, 1536, wc);
        pv.Line(r, ax, gy, H6, ax - dir * 256, gy - 256, H6, 1536, wc);
    }
}

}  // namespace

extern "C" const coop_fx_screen_t SCREEN_fickle = { ScreenInit, ScreenUpdate };
