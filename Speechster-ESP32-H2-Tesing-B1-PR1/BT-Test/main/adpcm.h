// adpcm.h - IMA ADPCM minimal API (drop-in)
#ifndef ADPCM_H
#define ADPCM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t predictor;
    int index;
} adpcm_state_t;

/* initialize state */
void adpcm_init_state(adpcm_state_t *st);
/* reset global state (helper) */
void adpcm_reset_state(void);
/* encode pcm16 samples -> IMA ADPCM bytes
 *  st: pointer to encoder state
 *  pcm: pointer to int16 samples
 *  samples: number of samples
 *  out: pointer to output buffer (must be large enough; worst-case samples/2 + 16)
 * returns: number of bytes written to out
 */
size_t adpcm_encode(adpcm_state_t *st, const int16_t *pcm, size_t samples, uint8_t *out);

#endif // ADPCM_H
