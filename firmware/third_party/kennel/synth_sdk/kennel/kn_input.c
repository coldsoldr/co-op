/* Selected-input producer at R246's post-decimator high-pass entry. No encrypted patch.
 * The assembly entry passes the caller's local has-input flag; only the verified
 * PmSynth call site may change it. Input source/mode and sampler taps stay intact.
 * GPL-3.0-or-later with OP-1 firmware permission. */
#include "kn_input.h"
#include "../op1_synth.h"
#include "../op1_feature.h"
#include "../op1_feature_sound.h"
const op1_input_engine_t *op1_input_engines[OP1_SLOTS];
static const op1_input_engine_t *active;
static int active_slot=-1;
static uint32_t active_rate,active_source,active_mode;
static uint32_t rate=22050, previous_clock, clock_sum, clock_count;
static int block_touched;
#define PLAY_MODE (*(volatile uint32_t *)0xFF801038)
#define SLOT (*(volatile uint32_t *)0xFF8016D4&7)
#define TYPE (*(volatile uint32_t *)(0xFF8016E4+4*SLOT))
#define INPUT_MODE (*(volatile uint32_t *)0xFF8010B0)
#define INPUT_SOURCE (*(volatile uint32_t *)0xFF8010B4)
#define SCREEN (*(volatile int *)0x018B65C8)
#define TABLE ((void **)0x01DF4E54)
static int current(void){
 if(PLAY_MODE!=0||TYPE!=14)return 0;
 int n=op1_synth_knobs(SLOT)[4]-OP1_ENGINE_MAGIC;
 return n>0&&n<OP1_SLOTS&&op1_input_engines[n]&&op1_sound_engine(n)?n:0;
}
void kn_input_clock(uint32_t now){
 uint32_t d=now-previous_clock;previous_clock=now;block_touched=0;
 /* Derive the two supported rates from the actual 400MHz callback cadence,
  * ignoring startup pauses. Change only after 16 consistent valid periods. */
 if(d<400000u||d>1600000u){clock_sum=clock_count=0;return;}
 clock_sum+=d;
 if(++clock_count==16){rate=clock_sum>13931968u?22050:44100;clock_sum=clock_count=0;}
}
/* CPU cycles in one audio block at the measured cadence: 64 frames at 44.1 kHz on the unit (1.451 ms), twice that
 * at the emulator's default 22.05 kHz link (docs: OP1-Emulator docs/emu-audio-clock-2026-10-21.md). kennel/kn_shed.c */
uint32_t kn_audio_block_cycles(void){return rate==44100?580499u:1160997u;}
extern void kn_input_highpass_stock(void);
void kn_input_bus(uint32_t *monitor,uint32_t caller){
 if(caller!=0xFFA02896u){kn_input_highpass_stock();return;}
 int n=current();
 if(!n){kn_input_highpass_stock();return;}
 const op1_input_engine_t *d=op1_input_engines[n];int slot=(int)SLOT;
 if(d!=active||slot!=active_slot||active_rate!=rate||active_source!=INPUT_SOURCE||active_mode!=INPUT_MODE){if(active)active->reset(-1,rate);active=d;active_slot=slot;active_rate=rate;active_source=INPUT_SOURCE;active_mode=INPUT_MODE;d->reset(slot,rate);}
 block_touched=1;
 int16_t words[8];for(int k=0;k<8;k++)words[k]=op1_synth_knobs(slot)[k];
 /* Resampling the master would feed this producer back into itself. Silence
  * explicitly while that source is selected; do not rewrite stock source flags. */
 static const int16_t silence[64];
 const int16_t *input=(*monitor&&INPUT_MODE==0)?(const int16_t *)0xFF800CB4:silence;
 uint32_t t=op1_cycles();
 d->process(input,(int16_t *)0xFF800BB4,words,rate);
 uint32_t elapsed=op1_cycles()-t;sm_cy[SMC_SYNTH]+=elapsed;kn_input_budget(n,elapsed);
 *monitor=0; /* only the caller's two dry-monitor additions; input buffer unchanged */
}
void kn_input_end(void){if(!block_touched&&active){active->reset(-1,rate);active=0;active_slot=-1;active_rate=0;}}
void kn_input_service(void){int n=current();if(n&&op1_input_engines[n]->service)op1_input_engines[n]->service();}
/* T2 objects are ordinary table entries, so the shared-screen lifetime fix covers
 * their drawing caches. Allocate only for installed providers, once at boot. */
static uint32_t *objects[OP1_SLOTS];
static void *prior,*shown;
void *kn_alloc(uint32_t);
void kn_input_ctor(void){
 for(int n=1;n<OP1_SLOTS;n++)if(op1_input_engines[n]&&op1_input_engines[n]->tuner){
  uint32_t *s=kn_alloc(128),*v=kn_alloc(40);if(!s||!v)continue;
  void *save=TABLE[22];((void *(*)(void *,int,const char *))0x01A3BAB0)(s,22,"usbout.svg");TABLE[22]=save;
  for(int i=0;i<10;i++)v[i]=((const uint32_t *)0x019103FC)[i];
  v[7]=(uint32_t)op1_input_engines[n]->tuner->update;v[9]=(uint32_t)op1_input_engines[n]->tuner->init;s[3]=(uint32_t)v;
  objects[n]=s;
 }
}
void kn_input_load(void){for(int n=1;n<OP1_SLOTS;n++)if(objects[n]){
 uint32_t *s=objects[n];s[0]=(uint32_t)((void *(*)(void *))0x01A503D8)((void *)s[2]);
 if(s[0])op1_input_engines[n]->tuner->init(s);else objects[n]=0;
}}
void kn_input_screen_sync(void){
 int n=current();void *target=n?objects[n]:0;
 if(shown&&TABLE[22]!=shown){shown=0;prior=0;} /* another scoped host restored stock */
 if(!target){if(shown&&TABLE[22]==shown)TABLE[22]=prior;shown=prior=0;return;}
 if(TABLE[22]!=target){if(!shown)prior=TABLE[22];TABLE[22]=target;shown=target;}
}
int kn_input_tuner_visible(void){int n=current();return SCREEN==22&&n&&objects[n]&&TABLE[22]==objects[n];}
int kn_input_control(int type,int code,int down){
 (void)down;
 if(!kn_input_tuner_visible())return 0;
 return type==OP1_IN_KNOB||(type==OP1_IN_KEY&&code>=0x3B&&code<=0x3E);
}

int op1_input_tuner_active(void){return kn_input_tuner_visible();}
