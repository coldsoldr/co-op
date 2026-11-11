/* coop-fw plugin host (plugin_host.cc): boot-time loading of the .coop files in /yaffs2/plugins and the master FX slot hook. */
#ifndef COOP_PLUGIN_HOST_H
#define COOP_PLUGIN_HOST_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void coop_plugins_boot(void);                          /* main thread, once, after the filesystem is up */
int coop_fx_plugin_render(int32_t *l, int32_t *r, int n);   /* audio interrupt: 1 if a plugin rendered the block */
int coop_fx_plugin_loaded(void);                         /* 1 once a plugin holds the master slot */
#ifdef __cplusplus
}
#endif
#endif
