/*
 * coop-fw screen calls for plugins (E2, docs/engines.md "Screens"). A plugin with a screen ships an SVG (320 x 160
 * user units) at the front of its file and a coop_screen_t (sdk/coop_plugin.h); the core builds the screen object and
 * calls init once and update every frame, on the GUI thread, with that object as `scr`. These calls are core exports
 * (sdk/coop_exports.cc): the loader resolves them, a plugin never names a firmware address.
 *
 * Geometry is in Q10 user units (1.0 = 1024). Matrices are 3 x 3, row-vector convention [a b 0; c d 0; tx ty 1], Q10.
 * Colours are RGBA bytes. R246's SVG parser has limits the packer checks (tools/plugin/pack.py): path commands M L H V
 * C S Z only, at most 4 decimals, no turn-backs in a stroke; guide/plugin-guidelines.md §5 and the owner's manifesto
 * say how a screen should look.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_SCREEN_H
#define COOP_SCREEN_H
#include <stdint.h>

#define COOP_Q10(v) ((int32_t)((v) * 1024))

#ifdef __cplusplus
extern "C" {
#endif

void *coop_scr_find(void *scr, const char *id);                  /* the SVG element with this id, 0 if none */
void coop_scr_transform(void *node, const int32_t *m9);          /* set the node's matrix (Q10) */
void coop_scr_visible(void *node, int on);                       /* show / hide; a hidden group hides its children */
uint8_t *coop_scr_line_rgba(void *scr, const char *id);          /* the element's stroke colour, writable (4 bytes);
                                                                    0 if the id is not in the SVG */
uint8_t *coop_scr_fill_rgba(void *scr, const char *id);          /* its fill colour, writable; 0 likewise */
void coop_scr_draw(void *scr, int dt);                           /* draw the SVG: call it from update */
void *coop_scr_raster(void);                                     /* the frame's raster, for coop_scr_line */
void coop_scr_line(void *raster, const int32_t *p0, const int32_t *p1, int32_t width_q10, const uint8_t *rgba);
                                                                 /* p0, p1: {x, y} in Q10 */
void coop_scr_knobs(int16_t *out4);                              /* the shown slot's four knob words (0..32767) */

#ifdef __cplusplus
}
#endif
#endif
