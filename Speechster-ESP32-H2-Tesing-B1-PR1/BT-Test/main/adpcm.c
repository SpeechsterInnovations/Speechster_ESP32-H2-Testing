#include "adpcm.h"

// Step table for IMA ADPCM
static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

// Index adjustment table
static const int index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

void adpcm_init_state(adpcm_state_t *state) {
    state->valprev = 0;
    state->index = 0;
}

void adpcm_reset_state(adpcm_state_t *state) {
    state->valprev = 0;
    state->index = 0;
}

// Encode PCM samples to ADPCM
size_t adpcm_encode(adpcm_state_t *state, const int16_t *in, size_t nsamples, uint8_t *out) {
    size_t out_bytes = 0;
    uint8_t buffer = 0;
    int buffer_ready = 0;

    int16_t prev = state->valprev;
    int idx = state->index;

    for (size_t i = 0; i < nsamples; i++) {
        int16_t sample = in[i];
        int diff = sample - prev;
        uint8_t code = 0;

        if (diff < 0) {
            code = 8;
            diff = -diff;
        }

        int step = step_table[idx];
        if (diff >= step) { code |= 4; diff -= step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; }
        step >>= 1;
        if (diff >= step) { code |= 1; }

        // Update predicted value
        int delta = 0;
        step = step_table[idx];
        if (code & 4) delta += step;
        if (code & 2) delta += step >> 1;
        if (code & 1) delta += step >> 2;
        delta += step >> 3;
        if (code & 8)
            prev -= delta;
        else
            prev += delta;

        // Clamp to int16
        if (prev > 32767) prev = 32767;
        if (prev < -32768) prev = -32768;

        // Update index
        idx += index_table[code & 0x0F];
        if (idx < 0) idx = 0;
        if (idx > 88) idx = 88;

        // Pack 2x4-bit samples into a byte
        if (buffer_ready) {
            out[out_bytes++] = (code & 0x0F) | (buffer << 4);
            buffer_ready = 0;
        } else {
            buffer = code & 0x0F;
            buffer_ready = 1;
        }
    }

    if (buffer_ready) {
        out[out_bytes++] = buffer & 0x0F;
    }

    state->valprev = prev;
    state->index = idx;

    return out_bytes;
}
