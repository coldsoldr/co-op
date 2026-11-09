/* Continuous selected-input synth extension. GPL-3.0-or-later with OP-1 firmware permission.
 * A .knl synth with input_engine:true extends its unchanged 16-byte voice prefix.
 * Old cores reject the new flag before executing it. No fake note or per-voice processing.
 */
#ifndef OP1_INPUT_H
#define OP1_INPUT_H
#include "op1_plugin.h"
typedef struct {
    op1_engine_t voice; /* silent ordinary voice callbacks; keyboard never gates the input */
    void (*reset)(int slot, uint32_t rate); /* audio thread: slot -1 deactivates; discard DSP/capture epoch */
    void (*process)(const int16_t *input, int16_t *output, const int16_t *params, uint32_t rate);
    void (*service)(void); /* main loop: bounded nonblocking work, optional */
    const op1_screen_t *tuner; /* optional T2 replacement, same lifetime as the plugin */
} op1_input_engine_t;
int op1_input_tuner_active(void); /* cheap audio/main/Gfx query; only this engine’s T2 */
#ifndef OP1_HOST
typedef char op1_input_size[(sizeof(op1_input_engine_t)==32)?1:-1];
#endif
#endif
