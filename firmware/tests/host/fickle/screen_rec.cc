// A recording stand-in for the core's screen calls (sdk/coop_screen.h), for fickle's frame renders only (tests/host/fickle
// "screens"): every node the screen looks up keeps the matrix, the visibility and the colours the screen last gave it,
// and every raster line is kept; screen_rec_dump() writes them as text for render_frames.py, which draws the SVG with
// them to PNG. The audio tests link ../screen_host.cc (the calls doing nothing) instead.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "coop_screen.h"

namespace {

struct Node {
  char id[24];
  int32_t m[6];
  int set;          // a matrix was given
  int vis;          // -1: as the SVG has it
  uint8_t line[4], fill[4];   // alpha 0: as the SVG has it
};

Node g_nodes[96];
int g_n;
struct Line {
  int32_t x0, y0, x1, y1, w;
  uint8_t c[4];
};
Line g_lines[512];
int g_lines_n;
int g_raster;

Node* Get(const char* id) {
  for (int i = 0; i < g_n; ++i) if (!strcmp(g_nodes[i].id, id)) return &g_nodes[i];
  if (g_n >= 96) return 0;
  Node* n = &g_nodes[g_n++];
  memset(n, 0, sizeof(*n));
  strncpy(n->id, id, sizeof(n->id) - 1);
  n->vis = -1;
  return n;
}

}  // namespace

extern "C" {
void* coop_scr_find(void*, const char* id) { return Get(id); }
void coop_scr_transform(void* node, const int32_t* m9) {
  Node* n = static_cast<Node*>(node);
  if (!n) return;
  n->m[0] = m9[0]; n->m[1] = m9[1]; n->m[2] = m9[3]; n->m[3] = m9[4]; n->m[4] = m9[6]; n->m[5] = m9[7];
  n->set = 1;
}
void coop_scr_visible(void* node, int on) { if (node) static_cast<Node*>(node)->vis = on ? 1 : 0; }
uint8_t* coop_scr_line_rgba(void*, const char* id) { Node* n = Get(id); return n ? n->line : 0; }
uint8_t* coop_scr_fill_rgba(void*, const char* id) { Node* n = Get(id); return n ? n->fill : 0; }
void coop_scr_draw(void*, int) { g_lines_n = 0; }
void* coop_scr_raster(void) { return &g_raster; }
void coop_scr_line(void*, const int32_t* p0, const int32_t* p1, int32_t w, const uint8_t* rgba) {
  if (g_lines_n >= 512) return;
  Line& l = g_lines[g_lines_n++];
  l.x0 = p0[0]; l.y0 = p0[1]; l.x1 = p1[0]; l.y1 = p1[1]; l.w = w;
  memcpy(l.c, rgba, 4);
}
void coop_scr_knobs(int16_t* out4) { for (int i = 0; i < 4; i++) out4[i] = 0; }
void coop_scr_words(int16_t* out8) { for (int i = 0; i < 8; i++) out8[i] = 0; }
int coop_scr_mods(void) { return 0; }

// One frame's state: "node id set a b c d e f vis lr lg lb la fr fg fb fa" lines, then "line x0 y0 x1 y1 w r g b a".
void screen_rec_dump(FILE* f, const char* name) {
  fprintf(f, "frame %s\n", name);
  for (int i = 0; i < g_n; ++i) {
    const Node& n = g_nodes[i];
    fprintf(f, "node %s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", n.id, n.set, n.m[0], n.m[1], n.m[2], n.m[3],
            n.m[4], n.m[5], n.vis, n.line[0], n.line[1], n.line[2], n.line[3], n.fill[0], n.fill[1], n.fill[2], n.fill[3]);
  }
  for (int i = 0; i < g_lines_n; ++i) {
    const Line& l = g_lines[i];
    fprintf(f, "line %d %d %d %d %d %d %d %d %d\n", l.x0, l.y0, l.x1, l.y1, l.w, l.c[0], l.c[1], l.c[2], l.c[3]);
  }
  fprintf(f, "end\n");
}
}
