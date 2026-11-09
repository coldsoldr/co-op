# Native plugin screens

<!-- Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). -->

A plugin may embed an SVG and expose `op1_screen_t` init/update callbacks. Resolve named nodes after loading and check lookups before animating them. Use `op1_synth_knobs_live` or `op1_fx_knobs_live` for display values.

## Native format and layout

The coordinate space is 320×160; C geometry uses Q10. R246 supports a limited SVG subset. Use simple paths, groups, lines, circles and polylines. Convert quadratic curves to cubic curves and rotation expressions to matrix transforms. Emit at most four decimal places, with no CSS, browser fonts, opacity or unsupported transforms.

Each filled contour needs its own path. Disconnected subpaths can become joined triangles. Long compound paths can exceed the rasterizer’s per-path allocation limit even when the file is small. Preserve path order, paint, precision, IDs and grouping when compacting. Do not introduce inherited paint or merge contours. Keep new screen assets below the project’s 8 KiB artwork target; the KNPL whole-file limit is separate.

Leave space below artwork, labels and animation extremes. The established effect-screen layout lifts its composition 12 pixels; do not apply that correction twice. Move static art, procedural geometry, text, pivots and clipping consistently. Full-canvas dimming veils keep their full coverage. Check actual native captures: browser bounds and raster previews do not prove OLED visibility.

## Animation and caching

Use stable node IDs. Keep screen objects and parsed documents alive until restart. The core owns shared-screen drawing-cache retirement; never free graphics buffers from input/audio code.

`op1_screen_draw_cached(scr, dt, first_live)` optionally caches an immutable, always-visible prefix of top-level SVG children. Draw mutable labels, transforms and overlays after that boundary in their original order. Pass the first mutable child by ID, or NULL only for wholly static artwork. The core uses two bounded 96 KiB records and falls back to native drawing when the cache or panel is unsuitable. Do not use this helper when moving and static paint are interleaved.

## Assets and credit

Keep editable artwork and its generator with the plugin. Record author, source and license. Include no extracted TE fonts or generic firmware artwork in the source bundle. Keep component notices intact. Use native stroke text helpers for labels and readouts.

See `../../../PLUGIN_GUIDE.md`, API headers and existing plugin generators for complete examples.
