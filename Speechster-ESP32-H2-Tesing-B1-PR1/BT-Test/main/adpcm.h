#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int16_t valprev;  // previous output sample
    int index;        // step index
} adpcm_state_t;

// Initialize ADPCM state
void adpcm_init_state(adpcm_state_t *state);

// Encode a block of PCM samples to ADPCM bytes
// Returns number of bytes written
size_t adpcm_encode(adpcm_state_t *state, const int16_t *in, size_t nsamples, uint8_t *out);

// Reset ADPCM state
void adpcm_reset_state(adpcm_state_t *state);

#endif // ADPCM_H
