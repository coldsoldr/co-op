/* Bounded native-font notification shared by the firmware and host renderer.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef KN_TOAST_H
#define KN_TOAST_H
/* 47 input characters, at most 24 per line. Break at a space when possible;
 * split long words so a filename cannot run off the panel. Three rows suffice. */
static int kn_toast_lines(const char *s, char lines[3][25])
{
    int row = 0;
    while (*s && row < 3) {
        int n = 0, space = -1;
        while (s[n] && n < 24) { if (s[n] == ' ') space = n; n++; }
        if (s[n] && space > 0) n = space;
        for (int i = 0; i < n; i++) lines[row][i] = s[i];
        lines[row++][n] = 0; s += n;
        while (*s == ' ') s++;
    }
    return row;
}
static uint32_t kn_toast_obj[6][OP1_TEXT_SIZE / 4];
static int kn_toast_rows;
static const int32_t kn_toast_ident[9] = {1024,0,0,0,1024,0,0,0,1024};
static void kn_toast_prepare(const char *text)
{
    static const uint8_t white[4] = {255,255,255,255};
    static int made;
    char lines[3][25];
    /* Each row-count layout is constructed once; native text objects own allocations. */
    kn_toast_rows = kn_toast_lines(text, lines);
    int offset = kn_toast_rows * (kn_toast_rows - 1) / 2;
    if (!(made & (1 << kn_toast_rows))) {
        for (int i = 0; i < kn_toast_rows; i++)
            op1_text_ctor(kn_toast_obj[offset + i], OP1_Q10(28), 83 - kn_toast_rows * 9 + 18 * i, 12, white);
        made |= 1 << kn_toast_rows;
    }
    for (int i = 0; i < kn_toast_rows; i++) op1_text_set(kn_toast_obj[offset + i], lines[i]);
}
/* Overlapping bands avoid raster gaps at the display's 1.25 scale. */
static void kn_toast_fill(void *r, int x0, int y0, int x1, int y1, const uint8_t *colour)
{
    static const uint8_t inset[3] = {4,1,0};
    /* The solid centre is one raster primitive, not a scanline per row. */
    op1_pt a = {OP1_Q10(x0), OP1_Q10(y0 + y1) / 2}, b = {OP1_Q10(x1), OP1_Q10(y0 + y1) / 2};
    /* Native coordinates scale 5/4 to the 400x200 raster, stroke widths do not. */
    op1_raster_line(r, &a, &b, OP1_Q10(y1 - y0 - 10) * 5 / 4, colour);
    for (int i = 0; i < 3; i++) {
        a.x = OP1_Q10(x0 + inset[i]); b.x = OP1_Q10(x1 - inset[i]);
        a.y = b.y = OP1_Q10(y0 + i * 2 + 1);
        op1_raster_line(r, &a, &b, OP1_Q10(3), colour);
        a.y = b.y = OP1_Q10(y1 - i * 2 - 1);
        op1_raster_line(r, &a, &b, OP1_Q10(3), colour);
    }
}
static void kn_toast_draw(void *r)
{
    static const uint8_t back[4] = {10,12,15,255}, edge[4] = {72,76,82,255};
    int top = 71 - kn_toast_rows * 9, bottom = 89 + kn_toast_rows * 9;
    kn_toast_fill(r, 18, top, 302, bottom, edge);
    kn_toast_fill(r, 19, top + 1, 301, bottom - 1, back);
    int offset = kn_toast_rows * (kn_toast_rows - 1) / 2;
    for (int i = 0; i < kn_toast_rows; i++) op1_text_draw(kn_toast_obj[offset + i], r, kn_toast_ident);
}
#endif
