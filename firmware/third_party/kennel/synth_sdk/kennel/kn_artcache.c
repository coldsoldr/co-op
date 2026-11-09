/* Bounded, Gfx-only cache of native RGB565 static SVG prefixes. The original renderer supplies every pixel;
 * no geometry, anti-aliasing, cap or paint-order approximation. Parsed trees remain owned by R246.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission (LICENSE, NOTICE). */
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#include <stdint.h>
/* Row-aware codec: framebuffer padding is never sampled or overwritten. A zero result means overflow. */
uint32_t kn_artcache_encode(uint32_t *out,uint32_t cap,const uint16_t *fb,uint32_t w,uint32_t h,uint32_t stride)
{
    uint32_t used=0,n=0;uint16_t color=fb[0];
    for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){
        uint16_t c=fb[y*stride+x];
        if(c==color && n<65535u)n++;
        else {if(used==cap)return 0;out[used++]=(n<<16)|color;color=c;n=1;}
    }
    if(used==cap)return 0;
    out[used++]=(n<<16)|color;return used;
}
void kn_artcache_decode(const uint32_t *in,uint32_t used,uint16_t *fb,uint32_t w,uint32_t stride)
{
    uint32_t x=0;uint16_t *row=fb;
    for(uint32_t i=0;i<used;i++){
        uint32_t run=in[i],n=run>>16;uint16_t color=(uint16_t)run;
        while(n){
            uint32_t count=n<w-x?n:w-x;n-=count;
            uint16_t *p=row+x,*last=p+count;x+=count;
            while(p<last)*p++=color;
            if(x==w){x=0;row+=stride;}
        }
    }
}
#ifndef OP1_HOST
#include "../op1_plugin.h"
#include "kn_screens.h"
#include "../kn_mem.h"
#define CACHE_WORDS (KN_ARTCACHE_BYTES / 2u / 4u)
typedef struct { void *doc, *boundary; uint32_t used, age; uint32_t *runs; } art_entry;
static art_entry entries[2];
static uint32_t age;
volatile uint32_t kn_artcache_stats[3]; /* hits, misses, last encoded bytes; emulator/debugger only */
uint32_t kn_artcache_bytes;             /* arena bytes the cache took this boot (0: no user, or no room) */
extern volatile uint32_t kn_next, kn_arena_hi;
void *kn_alloc(uint32_t);
/* Allocation happens once, after all plugins load, before Gfx starts, and only when something can use it: a loaded
 * file that imports op1_screen_draw_cached (users, kn_boot.c) or compiled-in code that calls it
 * (kn_artcache_builtin, make_firmware.py's engines_table.c). Without the cache op1_screen_draw_cached draws
 * normally; a full arena likewise simply disables caching. */
extern const uint8_t kn_artcache_builtin;
void kn_artcache_init(int users)
{
    if(!users && !kn_artcache_builtin)return;
    if(kn_next>kn_arena_hi-KN_ARTCACHE_BYTES-KN_ARTCACHE_HEADROOM)return;
    uint32_t *p=kn_alloc(2u*CACHE_WORDS*4u);
    if(p){entries[0].runs=p;entries[1].runs=p+CACHE_WORDS;kn_artcache_bytes=2u*CACHE_WORDS*4u;}
}
static void reset(void *doc)
{
    uint8_t *v=*(uint8_t **)((uint8_t *)doc+0x54);
    ((void(*)(void *))*(void **)(v+0x7c))((uint8_t *)doc+*(int16_t *)(v+0x78));
}
/* Group::draw reads its matrix, child vector and visibility only. Copy that group header on the Gfx
 * stack so input-side lookup always sees the complete immutable parsed tree, even during a cache miss. */
static void draw_range(void *doc,void **begin,void **end,int dt)
{
    uint32_t copy[0x58/4];
    for(unsigned i=0;i<0x58/4;i++)copy[i]=((const uint32_t *)doc)[i];
    copy[0x44/4]=(uint32_t)begin;copy[0x48/4]=(uint32_t)end;
    /* Document::draw at0x01976560 sets R1=this before Group::draw. Keep that ORIGINAL document context:
     * it owns the colour/style vectors beyond the group header. The fourth argument is the native raster. */
    (void)dt;
    ((void(*)(void *,void *,const void *,void *))0x019eb818)(copy,doc,(const void *)0x01b60fd8,op1_raster());
}
/* Opt-in contract: a static, always-visible top-level prefix, drawn first onto the cleared native raster.
 * first_live is the first top-level child after that prefix, or NULL for an entirely static document.
 * This implementation deliberately falls back on non-default panels; it never guesses their row layout. */
void op1_screen_draw_cached(void *scr,int dt,void *first_live)
{
    uint32_t *r=op1_raster();
    void *doc=*(void **)scr;
    if(!entries[0].runs || !doc || !*(volatile uint32_t *)0x01b60ffc || r[1]!=198 || r[2]!=512 || r[3]!=400){op1_screen_draw(scr,dt);return;}
    /* Document::draw is the verified 26-byte forwarder to Group::draw. */
    uint8_t *vt=*(uint8_t **)((uint8_t *)doc+0x54);
    if(*(uint32_t *)(vt+0x14)!=0x01976560u || *(int16_t *)(vt+0x10)!=0){op1_screen_draw(scr,dt);return;}
    void **begin=*(void ***)((uint8_t *)doc+0x44), **end=*(void ***)((uint8_t *)doc+0x48), **stop=end;
    if(first_live){for(stop=begin;stop<end && *stop!=first_live;stop++);if(stop==end){op1_screen_draw(scr,dt);return;}}
    if(stop==begin || end-begin>1024){op1_screen_draw(scr,dt);return;}
    /* Prefix nodes may not be transformed/recoloured/hidden by callers. All mutable nodes belong to the suffix. */
    art_entry *e=0;for(int i=0;i<2;i++)if(entries[i].doc==doc && entries[i].boundary==first_live)e=&entries[i];
    uint16_t *fb=(uint16_t *)(uintptr_t)r[0];
    if(e){
        if(!e->used){op1_screen_draw(scr,dt);return;} /* an oversized prefix retains normal SVG caching */
        kn_artcache_stats[0]++;
        kn_artcache_decode(e->runs,e->used,fb,198,256);
    }else{
        e=entries[0].age<=entries[1].age?&entries[0]:&entries[1];e->doc=0;e->used=0;
        draw_range(doc,begin,stop,dt);
        e->used=kn_artcache_encode(e->runs,CACHE_WORDS,fb,198,400,256);
        e->doc=doc;e->boundary=first_live;
        kn_artcache_stats[1]++;kn_artcache_stats[2]=e->used*4u;
        /* Release the hundreds of per-shape raster chunks; the bitmap owns the static pixels now. */
        if(e->used)reset(doc);
        else { e->doc=doc;e->boundary=first_live; }
    }
    e->age=++age;
    if(stop!=end){
        draw_range(doc,stop,end,dt);
    }
}

#endif
