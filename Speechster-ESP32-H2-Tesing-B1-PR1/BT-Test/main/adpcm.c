// adpcm.c - Small IMA ADPCM encoder (mono). Works in streaming fashion.
#include "adpcm.h"

/* IMA ADPCM step table & index table */
static const int step_table[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int index_table[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

void adpcm_init_state(adpcm_state_t *st) {
    if (!st) return;
    st->predictor = 0;
    st->index = 0;
}

void adpcm_reset_state(void) {
    /* nothing global to reset in this implementation;
       user can maintain adpcm_state_t if desired */
}

/* Encode int16 pcm to IMA ADPCM (4-bit packed) */
size_t adpcm_encode(adpcm_state_t *st, const int16_t *pcm, size_t samples, uint8_t *out) {
    if (!st || !pcm || !out) return 0;
    int index = st->index;
    int32_t predictor = st->predictor;
    size_t out_i = 0;
    int bufferstep = 1;
    uint8_t outputbuffer = 0;

    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = pcm[i];
        int32_t diff = sample - predictor;
        int sign = 0;
        if (diff < 0) { sign = 8; diff = -diff; }

        int step = step_table[index];
        int delta = 0;
        int temp = step;
        if (diff >= temp) { delta = 4; diff -= temp; }
        temp >>= 1;
        if (diff >= temp) { delta |= 2; diff -= temp; }
        temp >>= 1;
        if (diff >= temp) { delta |= 1; }

        int diffq = step >> 3;
        if (delta & 4) diffq += step;
        if (delta & 2) diffq += step >> 1;
        if (delta & 1) diffq += step >> 2;

        if (sign) predictor -= diffq; else predictor += diffq;

        if (predictor > 32767) predictor = 32767;
        else if (predictor < -32768) predictor = -32768;

        delta |= sign;

        index += index_table[delta & 0x0F];
        if (index < 0) index = 0;
        if (index > 88) index = 88;

        if (bufferstep) {
            outputbuffer = (delta & 0x0F);
            bufferstep = 0;
        } else {
            uint8_t to_write = ((delta & 0x0F) << 4) | (outputbuffer & 0x0F);
            out[out_i++] = to_write;
            bufferstep = 1;
        }
    }

    /* if odd number of samples, flush last nibble */
    if (!bufferstep) {
        uint8_t to_write = (outputbuffer & 0x0F);
        out[out_i++] = to_write;
    }

    st->index = index;
    st->predictor = predictor;
    return out_i;
}
