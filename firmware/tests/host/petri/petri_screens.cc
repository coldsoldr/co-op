// petri's screen previews (host only): the plugin's screen (plugins/petri/petri_screen.cc) driven as the core drives it,
// with the core's screen calls (sdk/coop_screen.h) replaced by a recorder, and each frame rasterised from petri.svg
// plus what the screen set (matrices, visibility, colours, raster lines) into a PNG, 960 x 480 (3 pixels a unit):
// build/petri_screens/<n>_<scene>.png. The audio runs underneath (the riff through the effect), so the voices, the write
// head and the onset hops are the render's own display words.
//
// The rasteriser knows only what petri.svg uses (groups, rects, circles, lines; fill, stroke, stroke-width) and draws a
// stroke in the group's local units (so a squashed face squashes its strokes; R246 may keep them 1.5 wide: see the
// README). It is a preview for the owner's eyes, not the device's renderer.
//
//   petri_screens SVG OUTDIR
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "coop_plugin.h"
#include "coop_screen.h"
#include "state.h"

extern "C" const coop_fx_t FX_petri;
extern "C" const coop_fx_screen_t SCREEN_petri;
extern "C" void* coop_fx_scratch(int slot, uint32_t* bytes);
uint32_t g_petri_host_cycles;

namespace {

struct Elem {
  char kind;                     // 'r' rect, 'c' circle, 'l' line
  std::string id;
  double a[4];                   // rect x y w h; circle cx cy r; line x1 y1 x2 y2
  double sw;
  bool has_fill, has_stroke;
  uint8_t fill[4], stroke[4];
};

struct Group {
  std::string id;
  std::vector<Elem> elems;
  int32_t m[6];                  // a b c d e f, Q10
  bool visible;
};

std::vector<Group> g_groups;
struct RLine { int32_t p0[2], p1[2], w; uint8_t rgba[4]; };
std::vector<RLine> g_lines;

std::string Attr(const std::string& tag, const char* name) {
  const std::string key = std::string(" ") + name + "=\"";
  const size_t i = tag.find(key);
  if (i == std::string::npos) return "";
  const size_t j = tag.find('"', i + key.size());
  return tag.substr(i + key.size(), j - i - key.size());
}

bool Colour(const std::string& v, uint8_t* out) {
  if (v.size() != 7 || v[0] != '#') return false;
  const unsigned long x = std::strtoul(v.c_str() + 1, 0, 16);
  out[0] = static_cast<uint8_t>(x >> 16);
  out[1] = static_cast<uint8_t>(x >> 8);
  out[2] = static_cast<uint8_t>(x);
  out[3] = 255;
  return true;
}

double Num(const std::string& tag, const char* name) { return std::atof(Attr(tag, name).c_str()); }

void Parse(const std::string& svg) {
  size_t pos = 0;
  Group* cur = 0;
  while ((pos = svg.find('<', pos)) != std::string::npos) {
    const size_t end = svg.find('>', pos);
    const std::string tag = svg.substr(pos, end - pos + 1);
    pos = end + 1;
    if (tag.compare(0, 3, "<g ") == 0) {
      Group g;
      g.id = Attr(tag, "id");
      g.m[0] = 1024; g.m[1] = 0; g.m[2] = 0; g.m[3] = 1024; g.m[4] = 0; g.m[5] = 0;
      g.visible = Attr(tag, "display") != "none";
      g_groups.push_back(g);
      cur = &g_groups.back();
    } else if (tag == "</g>") {
      cur = 0;
    } else if (cur && (tag.compare(0, 5, "<rect") == 0 || tag.compare(0, 7, "<circle") == 0 || tag.compare(0, 5, "<line") == 0)) {
      Elem e;
      e.kind = tag[1] == 'r' ? 'r' : (tag[1] == 'c' ? 'c' : 'l');
      e.id = Attr(tag, "id");
      if (e.kind == 'r') { e.a[0] = Num(tag, "x"); e.a[1] = Num(tag, "y"); e.a[2] = Num(tag, "width"); e.a[3] = Num(tag, "height"); }
      if (e.kind == 'c') { e.a[0] = Num(tag, "cx"); e.a[1] = Num(tag, "cy"); e.a[2] = Num(tag, "r"); e.a[3] = 0; }
      if (e.kind == 'l') { e.a[0] = Num(tag, "x1"); e.a[1] = Num(tag, "y1"); e.a[2] = Num(tag, "x2"); e.a[3] = Num(tag, "y2"); }
      e.sw = Num(tag, "stroke-width");
      e.has_fill = Colour(Attr(tag, "fill"), e.fill);
      e.has_stroke = Colour(Attr(tag, "stroke"), e.stroke);
      cur->elems.push_back(e);
    }
  }
}

Elem* FindElem(const char* id) {
  for (auto& g : g_groups)
    for (auto& e : g.elems)
      if (e.id == id) return &e;
  return 0;
}

// ---- the rasteriser ----
const int kScale = 3, kWidth = 320 * kScale, kHeight = 160 * kScale;
std::vector<uint8_t> g_px;

void Plot(int x, int y, const uint8_t* c) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  uint8_t* p = &g_px[(y * kWidth + x) * 3];
  p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
}

double SegDist(double px, double py, double x0, double y0, double x1, double y1) {
  const double dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
  double t = l2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  const double qx = x0 + t * dx - px, qy = y0 + t * dy - py;
  return std::sqrt(qx * qx + qy * qy);
}

void DrawGroup(const Group& g) {
  const double a = g.m[0] / 1024.0, b = g.m[1] / 1024.0, c = g.m[2] / 1024.0, d = g.m[3] / 1024.0;
  const double e = g.m[4] / 1024.0, f = g.m[5] / 1024.0, det = a * d - b * c;
  if (std::fabs(det) < 1e-9) return;
  for (const Elem& el : g.elems) {
    double lx0, ly0, lx1, ly1;                                       // the local bounding box, with the stroke
    const double h = el.sw / 2 + 0.5;
    if (el.kind == 'r') { lx0 = el.a[0] - h; ly0 = el.a[1] - h; lx1 = el.a[0] + el.a[2] + h; ly1 = el.a[1] + el.a[3] + h; }
    else if (el.kind == 'c') { lx0 = el.a[0] - el.a[2] - h; ly0 = el.a[1] - el.a[2] - h; lx1 = el.a[0] + el.a[2] + h; ly1 = el.a[1] + el.a[2] + h; }
    else { lx0 = std::fmin(el.a[0], el.a[2]) - h; ly0 = std::fmin(el.a[1], el.a[3]) - h; lx1 = std::fmax(el.a[0], el.a[2]) + h; ly1 = std::fmax(el.a[1], el.a[3]) + h; }
    double sx0 = 1e9, sy0 = 1e9, sx1 = -1e9, sy1 = -1e9;
    const double cx[4] = { lx0, lx1, lx0, lx1 }, cy[4] = { ly0, ly0, ly1, ly1 };
    for (int k = 0; k < 4; ++k) {
      const double X = cx[k] * a + cy[k] * c + e, Y = cx[k] * b + cy[k] * d + f;
      sx0 = std::fmin(sx0, X); sy0 = std::fmin(sy0, Y); sx1 = std::fmax(sx1, X); sy1 = std::fmax(sy1, Y);
    }
    for (int py = static_cast<int>(sy0 * kScale) - 1; py <= static_cast<int>(sy1 * kScale) + 1; ++py) {
      for (int px = static_cast<int>(sx0 * kScale) - 1; px <= static_cast<int>(sx1 * kScale) + 1; ++px) {
        const double X = (px + 0.5) / kScale - e, Y = (py + 0.5) / kScale - f;
        const double x = (X * d - Y * c) / det, y = (Y * a - X * b) / det;   // back to the group's units
        bool in_fill = false, in_stroke = false;
        const double hw = el.sw / 2;
        if (el.kind == 'r') {
          in_fill = x >= el.a[0] && x <= el.a[0] + el.a[2] && y >= el.a[1] && y <= el.a[1] + el.a[3];
          const double ex = std::fmin(std::fabs(x - el.a[0]), std::fabs(x - el.a[0] - el.a[2]));
          const double ey = std::fmin(std::fabs(y - el.a[1]), std::fabs(y - el.a[1] - el.a[3]));
          const bool inx = x >= el.a[0] - hw && x <= el.a[0] + el.a[2] + hw, iny = y >= el.a[1] - hw && y <= el.a[1] + el.a[3] + hw;
          in_stroke = (inx && iny) && (ex <= hw || ey <= hw);
        } else if (el.kind == 'c') {
          const double r = std::sqrt((x - el.a[0]) * (x - el.a[0]) + (y - el.a[1]) * (y - el.a[1]));
          in_fill = r <= el.a[2];
          in_stroke = std::fabs(r - el.a[2]) <= hw;
        } else {
          in_stroke = SegDist(x, y, el.a[0], el.a[1], el.a[2], el.a[3]) <= hw;
        }
        if (in_fill && el.has_fill) Plot(px, py, el.fill);
        if (in_stroke && el.has_stroke && hw > 0) Plot(px, py, el.stroke);
      }
    }
  }
}

void DrawLine(const RLine& l) {
  const double x0 = l.p0[0] / 1024.0, y0 = l.p0[1] / 1024.0, x1 = l.p1[0] / 1024.0, y1 = l.p1[1] / 1024.0;
  const double hw = l.w / 2048.0;
  for (int py = static_cast<int>((std::fmin(y0, y1) - hw) * kScale) - 1; py <= static_cast<int>((std::fmax(y0, y1) + hw) * kScale) + 1; ++py)
    for (int px = static_cast<int>((std::fmin(x0, x1) - hw) * kScale) - 1; px <= static_cast<int>((std::fmax(x0, x1) + hw) * kScale) + 1; ++px)
      if (SegDist((px + 0.5) / kScale, (py + 0.5) / kScale, x0, y0, x1, y1) <= hw) Plot(px, py, l.rgba);
}

// ---- PNG (stored deflate blocks: no zlib) ----
uint32_t g_crc[256];
uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
  for (size_t i = 0; i < n; ++i) c = g_crc[(c ^ p[i]) & 255] ^ (c >> 8);
  return c;
}
void Be32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void Chunk(std::FILE* f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> c;
  Be32(c, static_cast<uint32_t>(data.size()));
  c.insert(c.end(), type, type + 4);
  c.insert(c.end(), data.begin(), data.end());
  Be32(c, Crc(c.data() + 4, c.size() - 4) ^ 0xffffffffu);
  std::fwrite(c.data(), 1, c.size(), f);
}
bool WritePng(const char* path) {
  for (uint32_t n = 0; n < 256; ++n) { uint32_t c = n; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; g_crc[n] = c; }
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  std::fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  Be32(ihdr, kWidth); Be32(ihdr, kHeight);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  Chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  for (int y = 0; y < kHeight; ++y) { raw.push_back(0); raw.insert(raw.end(), &g_px[y * kWidth * 3], &g_px[(y + 1) * kWidth * 3]); }
  std::vector<uint8_t> z = { 0x78, 0x01 };
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
    z.push_back(i + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n)); z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n)); z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + static_cast<long>(i), raw.begin() + static_cast<long>(i + n));
  }
  Be32(z, (s2 << 16) | s1);
  Chunk(f, "IDAT", z);
  Chunk(f, "IEND", std::vector<uint8_t>());
  std::fclose(f);
  return true;
}

void Frame(const char* path) {
  g_px.assign(static_cast<size_t>(kWidth) * kHeight * 3, 0);
  for (const Group& g : g_groups) if (g.visible) DrawGroup(g);
  for (const RLine& l : g_lines) DrawLine(l);
  const uint8_t dim[3] = { 0x20, 0x20, 0x28 };                        // the popup's corner (kept clear), and the 8 / 16 margins
  for (int x = 0; x < kWidth; x += 6) { Plot(x, 140 * kScale, dim); }
  if (!WritePng(path)) std::fprintf(stderr, "cannot write %s\n", path);
}

}  // namespace

// ---- the core's screen calls (sdk/coop_screen.h), recorded ----
extern "C" {
void* coop_scr_find(void*, const char* id) {
  for (auto& g : g_groups) if (g.id == id) return &g;
  return 0;
}
void coop_scr_transform(void* node, const int32_t* m9) {
  Group* g = static_cast<Group*>(node);
  g->m[0] = m9[0]; g->m[1] = m9[1]; g->m[2] = m9[3]; g->m[3] = m9[4]; g->m[4] = m9[6]; g->m[5] = m9[7];
}
void coop_scr_visible(void* node, int on) { static_cast<Group*>(node)->visible = on != 0; }
uint8_t* coop_scr_line_rgba(void*, const char* id) { Elem* e = FindElem(id); return e ? e->stroke : 0; }
uint8_t* coop_scr_fill_rgba(void*, const char* id) { Elem* e = FindElem(id); return e ? e->fill : 0; }
void coop_scr_draw(void*, int) { g_lines.clear(); }                // the frame starts: the raster lines follow
void* coop_scr_raster(void) { return &g_lines; }
void coop_scr_line(void*, const int32_t* p0, const int32_t* p1, int32_t w, const uint8_t* rgba) {
  RLine l;
  l.p0[0] = p0[0]; l.p0[1] = p0[1]; l.p1[0] = p1[0]; l.p1[1] = p1[1]; l.w = w;
  std::memcpy(l.rgba, rgba, 4);
  g_lines.push_back(l);
}
void coop_scr_knobs(int16_t* out4) { for (int i = 0; i < 4; i++) out4[i] = 0; }
void coop_scr_words(int16_t* out8) { for (int i = 0; i < 8; i++) out8[i] = 0; }
int coop_scr_mods(void) { return 0; }
}

namespace {

const int16_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int16_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };

struct Scene {
  const char* name;
  int effect;
  bool hold, rev, fn;
  int32_t knobs[4];
  int32_t filter, space;
};

// A plucked saw riff (as petri_test.cc's, shorter): a note every 250 ms.
void Riff(long b, int16_t* l, int16_t* r, uint32_t* ph) {
  static const uint32_t kInc[5] = { 21474836u, 27056516u, 32177512u, 42949673u, 54113032u };
  for (int i = 0; i < 64; ++i) {
    const long n = b * 64 + i;
    const int note = static_cast<int>((n / 11025) % 5);
    const int32_t env = 30000 - static_cast<int32_t>(((n % 11025) * 28000) / 11025);
    ph[0] += kInc[note];
    ph[1] += kInc[note] + (kInc[note] >> 9);
    l[i] = static_cast<int16_t>(((static_cast<int32_t>(ph[0] >> 17) - 16384) * env) >> 15);
    r[i] = static_cast<int16_t>(((static_cast<int32_t>(ph[1] >> 17) - 16384) * env) >> 15);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s petri.svg OUTDIR\n", argv[0]); return 2; }
  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) return 2;
  std::string svg;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) svg.append(buf, n);
  std::fclose(f);
  Parse(svg);
  int scr = 0;
  SCREEN_petri.init(&scr);
  const Scene scenes[] = {
    { "mosaic", 0, false, false, false, { 16384, 22000, 13107, 16384 }, 30000, 8000 },
    { "mosaic_hold", 0, true, false, false, { 16384, 22000, 20000, 16384 }, 30000, 8000 },
    { "haze_fn", 3, false, false, true, { 12000, 30000, 16000, 20000 }, 14000, 26000 },
    { "warp_reverse", 10, false, true, false, { 24000, 26000, 9000, 26000 }, 22000, 4000 },
    { "arp", 8, false, false, false, { 9000, 32767, 20000, 12000 }, 30000, 8000 },
    { "tunnel_hold_fn", 4, true, true, true, { 4000, 32767, 30000, 32767 }, 8000, 32767 },
  };
  int count = 0;
  for (const Scene& sc : scenes) {
    FX_petri.init(0);
    int16_t p[16] = { 0 };
    for (int k = 0; k < 4; ++k) p[k] = static_cast<int16_t>(kRangeLo[k] + ((kRangeHi[k] - kRangeLo[k]) * sc.knobs[k]) / 32767);
    p[4] = 0x5300;
    p[5] = static_cast<int16_t>(sc.effect);
    p[6] = static_cast<int16_t>(sc.filter);
    p[7] = static_cast<int16_t>(sc.space);
    uint32_t ph[2] = { 0, 0 };
    coop_fx_view_t v;
    std::memset(&v, 0, sizeof(v));
    v.slot = 0;
    for (int k = 0; k < 4; ++k) v.knobs[k] = static_cast<int16_t>(sc.knobs[k]);
    v.words[0] = static_cast<int16_t>(sc.effect);
    v.words[1] = static_cast<int16_t>(sc.filter);
    v.words[2] = static_cast<int16_t>(sc.space);
    uint32_t bytes = 0;
    v.state = coop_fx_scratch(0, &bytes);
    v.state_bytes = bytes;
    v.live = 1;
    // 2.5 s of audio, a frame every 16 ms (11 blocks); the latches and fn come on after the first second
    for (long b = 0; b < 1723; ++b) {
      const bool on = b > 690;
      p[8] = static_cast<int16_t>(((sc.hold && on) ? 1 : 0) | ((sc.rev && on) ? 2 : 0));
      int16_t l16[64], r16[64];
      Riff(b, l16, r16, ph);
      if (sc.hold && on) for (int i = 0; i < 64; ++i) { l16[i] = 0; r16[i] = 0; }   // held: nothing new comes in
      int32_t l[64], r[64];
      for (int i = 0; i < 64; ++i) { l[i] = static_cast<int32_t>(l16[i]) << 16; r[i] = static_cast<int32_t>(r16[i]) << 16; }
      g_petri_host_cycles += 20000;
      FX_petri.render(0, l, r, p, 64);
      if (b % 11 == 0) {
        v.latches = static_cast<uint16_t>(p[8]);
        v.mods = (sc.fn && on) ? 2 : 0;
        SCREEN_petri.update(&scr, 16, &v);
      }
    }
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%d_%s.png", argv[2], ++count, sc.name);
    Frame(path);
  }
  std::printf("petri screens: %d frames in %s\n", count, argv[2]);
  return 0;
}
