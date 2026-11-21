/* coop-fw e13: the plugin memory placement policy (core/sdram/coop_place.cc, docs/plugins.md "e13").
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COOP_PLACE_H
#define COOP_PLACE_H
#include <stdint.h>
#define COOP_IN_ARENA 0
#define COOP_IN_HEAP  1
enum { COOP_PLACE_DEFAULT = 0, COOP_PLACE_FILE = 1, COOP_PLACE_MALFORMED = 2, COOP_PLACE_MARKER = 3 };
/* eng / fx: an engine's / an effect's image; voice: the engines' voice state; buf: the effect position buffers.
 * src: where the policy came from (the enum); probe: the live fill probe is on. */
struct coop_place { uint8_t eng, fx, voice, buf, src, probe; };
#ifdef __cplusplus
extern "C" {
#endif
extern struct coop_place g_coop_place;
void coop_place_boot(void);                    /* main thread, before anything is placed */
void coop_place_service(void);                 /* main loop: the boot marker goes once the boot is through */
int coop_place_parse(const char *text, int len, struct coop_place *p, int *pf, int *probe);   /* 1 = the whole text is valid */
int coop_place_text(char *out, int cap);       /* "eng=heap,fx=heap,voice=heap,buf=arena" */
const char *coop_place_src_word(void);         /* default | file | malformed | marker */
#ifdef __cplusplus
}
#endif
#endif
