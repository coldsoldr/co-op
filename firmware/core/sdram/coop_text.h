/*
 * coop-fw: text in the stock font for coop's overlays (u2). The firmware's Text object, as kennel's op1_synth.h binds
 * it (hundekraft 3.1.4, "firmware text (stock font, docs/18)"): 0xA4 bytes, built once on the GUI thread after boot
 * with its place, size and colour (ctor 0x01A61904: obj, x Q10, y px, size, rgba, 0; size 12 = the battery page's
 * text, template BatteryScreen 0x0193D248), given a string (0x01A61AAC: reallocates in the TE heap, so GUI thread
 * only and only on change), drawn into the frame's raster with a 3 x 3 Q10 matrix (0x01A6259C). The align word at
 * +0x60 (kennel chord.c T_SET_ALIGN: 1 = centred on x). Write lower case: the font draws capitals as icons.
 *
 * One object is one place + size + colour, so an overlay keeps an object per text it can show, and draws the ones
 * it needs each frame. No libc; fixed point only.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_TEXT_H
#define COOP_TEXT_H
#include <stdint.h>

#define OP1_TEXT_SIZE 0xA4
#define op1_text_ctor(obj, x_q10, y, size, rgba) \
    (((void *(*)(void *, int32_t, int32_t, int32_t, const uint8_t *, int32_t))0x01A61904u)((obj), (x_q10), (y), (size), (rgba), 0))
#define op1_text_set(t, str) (((void (*)(void *, const char *))0x01A61AACu)((t), (str)))
#define op1_text_draw(t, r, m9) (((void (*)(void *, void *, const int32_t *))0x01A6259Cu)((t), (r), (m9)))

struct coop_text { uint32_t obj[OP1_TEXT_SIZE / 4]; char shown[24]; uint8_t made; };
static const int32_t coop_text_ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };

/* GUI thread. x, y in screen units (320 x 160); align 1 = centred on x. The object is made at its first draw and
 * keeps that place, size and colour; the string is set again only when it changes. */
static inline void coop_text_draw(struct coop_text *t, void *r, int32_t x, int32_t y, int32_t size, const uint8_t *rgba,
                                  const char *s, int align)
{
    if (!t->made) {
        op1_text_ctor(t->obj, x << 10, y, size, rgba);
        if (align) *(int32_t *)((uint8_t *)t->obj + 0x60) = align;
        t->made = 1;
        t->shown[0] = 1; t->shown[1] = 0;                                 /* never equal to a real string: the first set happens */
    }
    int same = 1;
    for (int i = 0; ; i++) { if (t->shown[i] != s[i]) { same = 0; break; } if (!s[i]) break; }
    if (!same) {
        int k = 0;
        for (; s[k] && k < 23; k++) t->shown[k] = s[k];
        t->shown[k] = 0;
        op1_text_set(t->obj, t->shown);
    }
    op1_text_draw(t->obj, r, coop_text_ident);
}
#endif
