// SPDX-FileCopyrightText: Copyright 2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: MIT
//

#define GGML_COMMON_IMPL_C
#include "ggml-common.h"

#include "ggml-quants.h"
#include "ggml-impl.h"
#include "ggml-cpu-impl.h"

#include <math.h>
#include <string.h>
#include <assert.h>
#include <float.h>
#include <stdlib.h> // for qsort
#include <stdio.h>  // for GGML_ASSERT

#include "ggml-aarch64.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Woverlength-strings"
#elif defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

#define UNUSED GGML_UNUSED

// Functions to create the interleaved data layout formats

// interleave 4 block_q4_0s in blocks of blck_size_interleave
// returns an interleaved block_q4_0x4
// in the interleaved block_q4_0x4, place deltas for 4 block_q4_0 blocks
// first, then interleave quants from 4 block_q4_0s in blocks of blck_size_interleave
//
// - in                  : an array of block_q4_0 pointers
// - blck_size_interleave : the block_q4_0 quants bytes are interleaved in blocks of
//                         blck_size_interleave bytes
// - xor_mask            : the mask to convert the nibbles in block_q4_0 quants bytes
//                         from bias offset form to pure sign form (this saves subtract
//                         operations durin unpacking)
//
#if defined(__AVX__)
#if defined(__F16C__)
#if defined(__AVX512F__)
#define GGML_F32Cx8x2_LOAD(x, y)     _mm512_cvtph_ps(_mm256_set_m128i(_mm_loadu_si128((const __m128i *)(y)), _mm_loadu_si128((const __m128i *)(x))))
#define GGML_F32Cx16_REPEAT_LOAD(x)  _mm512_cvtph_ps(_mm256_set_m128i(x, x))
#endif
// the  _mm256_cvt intrinsics require F16C
#define GGML_F32Cx8_LOAD(x)     _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(x)))
#define GGML_F32Cx8_REPEAT_LOAD(x, loadMask)     _mm256_cvtph_ps(_mm_shuffle_epi32(_mm_maskload_epi32((int const*)(x), loadMask), 68))
#define GGML_F32Cx8_REARRANGE_LOAD(x, arrangeMask)     _mm256_cvtph_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *) x), arrangeMask))
#else
#if defined(__AVX512F__)
static inline __m512 __avx512_f32cx8x2_load(ggml_fp16_t *x, ggml_fp16_t *y) {
    float tmp[16];

    for (int i = 0; i < 8; i++) {
        tmp[i] = GGML_FP16_TO_FP32(x[i]);
    }

    for (int i = 0; i < 8; i++) {
        tmp[i + 8] = GGML_FP16_TO_FP32(y[i]);
    }

    return _mm512_loadu_ps(tmp);
}
static inline __m512 __avx512_repeat_f32cx16_load(__m128i x) {
    float tmp[16];
    uint16_t tmphalf[8];
    _mm_storeu_si128((__m128i*)tmphalf, x);

    for (int i = 0; i < 4; i++) {
        tmp[i] = GGML_FP16_TO_FP32(tmphalf[i]);
        tmp[i + 4] = GGML_FP16_TO_FP32(tmphalf[i]);
        tmp[i + 8] = GGML_FP16_TO_FP32(tmphalf[i]);
        tmp[i + 12] = GGML_FP16_TO_FP32(tmphalf[i]);
    }

    return _mm512_loadu_ps(tmp);
}
#endif
static inline __m256 __avx_f32cx8_load(ggml_fp16_t *x) {
    float tmp[8];

    for (int i = 0; i < 8; i++) {
        tmp[i] = GGML_FP16_TO_FP32(x[i]);
    }

    return _mm256_loadu_ps(tmp);
}
static inline __m256 __avx_repeat_f32cx8_load(ggml_fp16_t *x) {
    float tmp[8];

    for (int i = 0; i < 4; i++) {
        tmp[i] = GGML_FP16_TO_FP32(x[i]);
        tmp[i + 4] = GGML_FP16_TO_FP32(x[i]);
    }

    return _mm256_loadu_ps(tmp);
}
static inline __m256 __avx_rearranged_f32cx8_load(ggml_fp16_t *x, __m128i arrangeMask) {
    uint16_t tmphalf[8];
    float tmp[8];

    _mm_storeu_si128((__m128i*)tmphalf, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *) x), arrangeMask));
    for (int i = 0; i < 8; i++) {
        tmp[i] = GGML_FP16_TO_FP32(tmphalf[i]);
    }

    return _mm256_loadu_ps(tmp);
}

#define GGML_F32Cx8_LOAD(x)     __avx_f32cx8_load(x)
#define GGML_F32Cx8_REPEAT_LOAD(x, loadMask)     __avx_repeat_f32cx8_load(x)
#define GGML_F32Cx8_REARRANGE_LOAD(x, arrangeMask)     __avx_rearranged_f32cx8_load(x, arrangeMask)
#if defined(__AVX512F__)
#define GGML_F32Cx8x2_LOAD(x, y)     __avx512_f32cx8x2_load(x, y)
#define GGML_F32Cx16_REPEAT_LOAD(x)  __avx512_repeat_f32cx16_load(x)
#endif
#endif
#endif


#if defined(__AVX2__) || defined(__AVX512F__)
#if defined(__AVX512F__)
// add int16_t pairwise and return as 512 bit int vector
static inline __m512i sum_i16_pairs_int_32x16(const __m512i x) {
    const __m512i ones = _mm512_set1_epi16(1);
    return _mm512_madd_epi16(ones, x);
}

static inline __m512i mul_sum_us8_pairs_int32x16(const __m512i ax, const __m512i sy) {
#if defined(__AVXVNNI__) || (defined(__AVX512VNNI__) && defined(__AVX512VL__))
    const __m512i zero = _mm512_setzero_si512();
    return _mm512_dpbusd_epi32(zero, ax, sy);
#else
    // Perform multiplication and create 16-bit values
    const __m512i dot = _mm512_maddubs_epi16(ax, sy);
    return sum_i16_pairs_int_32x16(dot);
#endif
}

// multiply int8_t, add results pairwise twice and return as 512 bit int vector
static inline __m512i mul_sum_i8_pairs_int32x16(const __m512i x, const __m512i y) {
    const __m512i zero = _mm512_setzero_si512();
    // Get absolute values of x vectors
    const __m512i ax = _mm512_abs_epi8(x);
    // Sign the values of the y vectors
    __mmask64 blt0 = _mm512_movepi8_mask(x);
    const __m512i sy = _mm512_mask_sub_epi8(y, blt0, zero, y);
    return mul_sum_us8_pairs_int32x16(ax, sy);
}
#endif

// add int16_t pairwise and return as 256 bit int vector
static inline __m256i sum_i16_pairs_int32x8(const __m256i x) {
    const __m256i ones = _mm256_set1_epi16(1);
    return _mm256_madd_epi16(ones, x);
}

static inline __m256i mul_sum_us8_pairs_int32x8(const __m256i ax, const __m256i sy) {
#if defined(__AVXVNNI__) || (defined(__AVX512VNNI__) && defined(__AVX512VL__))
    const __m256i zero = _mm256_setzero_si256();
    return _mm256_dpbusd_epi32(zero, ax, sy);
#else
    // Perform multiplication and create 16-bit values
    const __m256i dot = _mm256_maddubs_epi16(ax, sy);
    return sum_i16_pairs_int32x8(dot);
#endif
}

// Integer variant of the function defined in ggml-quants.c
// multiply int8_t, add results pairwise twice and return as 256 bit int vector
static inline __m256i mul_sum_i8_pairs_int32x8(const __m256i x, const __m256i y) {
#if __AVXVNNIINT8__
    const __m256i zero = _mm256_setzero_si256();
    return _mm256_dpbssd_epi32(zero, x, y);
#else
    // Get absolute values of x vectors
    const __m256i ax = _mm256_sign_epi8(x, x);
    // Sign the values of the y vectors
    const __m256i sy = _mm256_sign_epi8(y, x);
    return mul_sum_us8_pairs_int32x8(ax, sy);
#endif
}
#endif

static block_q4_0x4 make_block_q4_0x4(block_q4_0 * in, unsigned int blck_size_interleave, unsigned int xor_mask) {
    block_q4_0x4 out;

    for (int i = 0; i < 4; i++) {
        out.d[i] = in[i].d;
    }

    for (int i = 0; i < QK4_0 * 2; i++) {
        int src_offset = (i / (4 * blck_size_interleave)) * blck_size_interleave;
        int src_id = (i % (4 * blck_size_interleave)) / blck_size_interleave;
        src_offset += (i % blck_size_interleave);

        out.qs[i] = in[src_id].qs[src_offset] ^ xor_mask;
    }

    return out;
}

// interleave 8 block_q4_0s in blocks of blck_size_interleave
// returns an interleaved block_q4_0x8
// in the interleaved block_q4_0x8, place deltas for 8 block_q4_0 blocks
// first, then interleave quants from 8 block_q4_0s in blocks of blck_size_interleave
static block_q4_0x8 make_block_q4_0x8(block_q4_0 * in, unsigned int blck_size_interleave, unsigned int xor_mask) {
    block_q4_0x8 out;

    for (int i = 0; i < 8; i++) {
        out.d[i] = in[i].d;
    }

    for (int i = 0; i < QK4_0 * 4; i++) {
        int src_offset = (i / (8 * blck_size_interleave)) * blck_size_interleave;
        int src_id = (i % (8 * blck_size_interleave)) / blck_size_interleave;
        src_offset += (i % blck_size_interleave);

        out.qs[i] = in[src_id].qs[src_offset] ^ xor_mask;
    }

    return out;
}

void quantize_q8_0_4x4(const float * restrict x, void * restrict vy, int64_t k) {
    assert(QK8_0 == 32);
    assert(k % QK8_0 == 0);
    const int nb = k / QK8_0;

    block_q8_0x4 * restrict y = (block_q8_0x4 *) vy;

#if defined(__ARM_NEON)
    float32x4_t srcv[4][8];
    float id[4];

    for (int i = 0; i < nb; i++) {
        float32x4_t asrcv[8];
        float32x4_t amaxv[8];

        for (int row_iter = 0; row_iter < 4; row_iter++) {
            for (int j = 0; j < 8; j++) srcv[row_iter][j] = vld1q_f32(x + row_iter * k + i * 32 + 4 * j);
            for (int j = 0; j < 8; j++) asrcv[j] = vabsq_f32(srcv[row_iter][j]);

            for (int j = 0; j < 4; j++) amaxv[2 * j] = vmaxq_f32(asrcv[2 * j], asrcv[2 * j + 1]);
            for (int j = 0; j < 2; j++) amaxv[4 * j] = vmaxq_f32(amaxv[4 * j], amaxv[4 * j + 2]);
            for (int j = 0; j < 1; j++) amaxv[8 * j] = vmaxq_f32(amaxv[8 * j], amaxv[8 * j + 4]);

            const float amax = vmaxvq_f32(amaxv[0]);

            const float d = amax / ((1 << 7) - 1);
            id[row_iter] = d ? 1.0f / d : 0.0f;

            y[i].d[row_iter] = GGML_FP32_TO_FP16(d);
        }

        for (int j = 0; j < 8; j++) {
            float32x4_t v = vmulq_n_f32(srcv[0][j], id[0]);
            int32x4_t vi = vcvtnq_s32_f32(v);
            y[i].qs[16 * j + 0] = vgetq_lane_s32(vi, 0);
            y[i].qs[16 * j + 1] = vgetq_lane_s32(vi, 1);
            y[i].qs[16 * j + 2] = vgetq_lane_s32(vi, 2);
            y[i].qs[16 * j + 3] = vgetq_lane_s32(vi, 3);

            v = vmulq_n_f32(srcv[1][j], id[1]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[16 * j + 4] = vgetq_lane_s32(vi, 0);
            y[i].qs[16 * j + 5] = vgetq_lane_s32(vi, 1);
            y[i].qs[16 * j + 6] = vgetq_lane_s32(vi, 2);
            y[i].qs[16 * j + 7] = vgetq_lane_s32(vi, 3);

            v = vmulq_n_f32(srcv[2][j], id[2]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[16 * j + 8] = vgetq_lane_s32(vi, 0);
            y[i].qs[16 * j + 9] = vgetq_lane_s32(vi, 1);
            y[i].qs[16 * j + 10] = vgetq_lane_s32(vi, 2);
            y[i].qs[16 * j + 11] = vgetq_lane_s32(vi, 3);

            v = vmulq_n_f32(srcv[3][j], id[3]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[16 * j + 12] = vgetq_lane_s32(vi, 0);
            y[i].qs[16 * j + 13] = vgetq_lane_s32(vi, 1);
            y[i].qs[16 * j + 14] = vgetq_lane_s32(vi, 2);
            y[i].qs[16 * j + 15] = vgetq_lane_s32(vi, 3);
        }
    }
#else
    // scalar
    const int blck_size_interleave = 4;
    float srcv[4][QK8_0];
    float id[4];

    for (int i = 0; i < nb; i++) {
        for (int row_iter = 0; row_iter < 4; row_iter++) {
            float amax = 0.0f; // absolute max

            for (int j = 0; j < QK8_0; j++) {
                srcv[row_iter][j] = x[row_iter * k + i * QK8_0 + j];
                amax = MAX(amax, fabsf(srcv[row_iter][j]));
            }

            const float d = amax / ((1 << 7) - 1);
            id[row_iter] = d ? 1.0f / d : 0.0f;

            y[i].d[row_iter] = GGML_FP32_TO_FP16(d);
        }

        for (int j = 0; j < QK8_0 * 4; j++) {
            int src_offset = (j / (4 * blck_size_interleave)) * blck_size_interleave;
            int src_id = (j % (4 * blck_size_interleave)) / blck_size_interleave;
            src_offset += (j % blck_size_interleave);

            float x0 = srcv[src_id][src_offset] * id[src_id];
            y[i].qs[j] = roundf(x0);
        }
    }
#endif
}

void quantize_q8_0_4x8(const float * restrict x, void * restrict vy, int64_t k) {
    assert(QK8_0 == 32);
    assert(k % QK8_0 == 0);
    const int nb = k / QK8_0;

    block_q8_0x4 * restrict y = (block_q8_0x4 *) vy;

#if defined(__ARM_NEON)
    float32x4_t srcv[4][8];
    float id[4];

    for (int i = 0; i < nb; i++) {
        float32x4_t asrcv[8];
        float32x4_t amaxv[8];

        for (int row_iter = 0; row_iter < 4; row_iter++) {
            for (int j = 0; j < 8; j++) srcv[row_iter][j] = vld1q_f32(x + row_iter * k + i * 32 + 4 * j);
            for (int j = 0; j < 8; j++) asrcv[j] = vabsq_f32(srcv[row_iter][j]);

            for (int j = 0; j < 4; j++) amaxv[2 * j] = vmaxq_f32(asrcv[2 * j], asrcv[2 * j + 1]);
            for (int j = 0; j < 2; j++) amaxv[4 * j] = vmaxq_f32(amaxv[4 * j], amaxv[4 * j + 2]);
            for (int j = 0; j < 1; j++) amaxv[8 * j] = vmaxq_f32(amaxv[8 * j], amaxv[8 * j + 4]);

            const float amax = vmaxvq_f32(amaxv[0]);

            const float d = amax / ((1 << 7) - 1);
            id[row_iter] = d ? 1.0f / d : 0.0f;

            y[i].d[row_iter] = GGML_FP32_TO_FP16(d);
        }

        for (int j = 0; j < 4; j++) {
            float32x4_t v = vmulq_n_f32(srcv[0][2 * j], id[0]);
            int32x4_t vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 0] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 1] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 2] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 3] = vgetq_lane_s32(vi, 3);
            v = vmulq_n_f32(srcv[0][2 * j + 1], id[0]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 4] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 5] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 6] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 7] = vgetq_lane_s32(vi, 3);

            v = vmulq_n_f32(srcv[1][2 * j], id[1]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 8] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 9] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 10] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 11] = vgetq_lane_s32(vi, 3);
            v = vmulq_n_f32(srcv[1][2 * j + 1], id[1]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 12] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 13] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 14] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 15] = vgetq_lane_s32(vi, 3);

            v = vmulq_n_f32(srcv[2][2 * j], id[2]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 16] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 17] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 18] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 19] = vgetq_lane_s32(vi, 3);
            v = vmulq_n_f32(srcv[2][2 * j + 1], id[2]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 20] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 21] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 22] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 23] = vgetq_lane_s32(vi, 3);

            v = vmulq_n_f32(srcv[3][2 * j], id[3]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 24] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 25] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 26] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 27] = vgetq_lane_s32(vi, 3);
            v = vmulq_n_f32(srcv[3][2 * j + 1], id[3]);
            vi = vcvtnq_s32_f32(v);
            y[i].qs[32 * j + 28] = vgetq_lane_s32(vi, 0);
            y[i].qs[32 * j + 29] = vgetq_lane_s32(vi, 1);
            y[i].qs[32 * j + 30] = vgetq_lane_s32(vi, 2);
            y[i].qs[32 * j + 31] = vgetq_lane_s32(vi, 3);
        }
    }
#elif defined(__AVX2__) || defined(__AVX__)
    float id[4];
    __m256 srcv[4][4];
    __m256 idvec[4];

    for (int i = 0; i < nb; i++) {
        for (int row_iter = 0; row_iter < 4; row_iter++) {
            // Load elements into 4 AVX vectors
            __m256 v0 = _mm256_loadu_ps( x + row_iter * k + i * 32 );
            __m256 v1 = _mm256_loadu_ps( x + row_iter * k + i * 32 + 8 );
            __m256 v2 = _mm256_loadu_ps( x + row_iter * k + i * 32 + 16 );
            __m256 v3 = _mm256_loadu_ps( x + row_iter * k + i * 32 + 24 );

            // Compute max(abs(e)) for the block
            const __m256 signBit = _mm256_set1_ps( -0.0f );
            __m256 maxAbs = _mm256_andnot_ps( signBit, v0 );
            maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v1 ) );
            maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v2 ) );
            maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v3 ) );

            __m128 max4 = _mm_max_ps( _mm256_extractf128_ps( maxAbs, 1 ), _mm256_castps256_ps128( maxAbs ) );
            max4 = _mm_max_ps( max4, _mm_movehl_ps( max4, max4 ) );
            max4 = _mm_max_ss( max4, _mm_movehdup_ps( max4 ) );
            const float maxScalar = _mm_cvtss_f32( max4 );

            // Divided by 127.f to mirror results in quantize_row_q8_0
            const float d = maxScalar  / 127.f;
            id[row_iter] = ( maxScalar != 0.0f ) ? 127.f / maxScalar : 0.0f; //d ? 1.0f / d : 0.0f;

            // Store the scale for the individual block
            y[i].d[row_iter] = GGML_FP32_TO_FP16(d);

            // Store the values in blocks of eight values - Aim is to use these later for block interleaving
            srcv[row_iter][0] = v0;
            srcv[row_iter][1] = v1;
            srcv[row_iter][2] = v2;
            srcv[row_iter][3] = v3;
            idvec[row_iter] = _mm256_set1_ps(id[row_iter]);
        }

        // The loop iterates four times - The aim is to get 4 corresponding chunks of eight bytes from the original weight blocks that are interleaved
        for (int j = 0; j < 4; j++) {
            // Apply the multiplier
            __m256 v0 = _mm256_mul_ps(srcv[0][j], idvec[0]);
            __m256 v1 = _mm256_mul_ps(srcv[1][j], idvec[1]);
            __m256 v2 = _mm256_mul_ps(srcv[2][j], idvec[2]);
            __m256 v3 = _mm256_mul_ps(srcv[3][j], idvec[3]);

            // Round to nearest integer
            v0 = _mm256_round_ps( v0, _MM_ROUND_NEAREST );
            v1 = _mm256_round_ps( v1, _MM_ROUND_NEAREST );
            v2 = _mm256_round_ps( v2, _MM_ROUND_NEAREST );
            v3 = _mm256_round_ps( v3, _MM_ROUND_NEAREST );

            // Convert floats to integers
            __m256i i0 = _mm256_cvtps_epi32( v0 );
            __m256i i1 = _mm256_cvtps_epi32( v1 );
            __m256i i2 = _mm256_cvtps_epi32( v2 );
            __m256i i3 = _mm256_cvtps_epi32( v3 );

#if defined(__AVX2__)
            // Convert int32 to int16
            i0 = _mm256_packs_epi32( i0, i1 );
            i2 = _mm256_packs_epi32( i2, i3 );
            // Convert int16 to int8
            i0 = _mm256_packs_epi16( i0, i2 );

            //  Permute and store the quantized weights in the required order after the pack instruction
            const __m256i perm = _mm256_setr_epi32( 0, 4, 1, 5, 2, 6, 3, 7 );
            i0 = _mm256_permutevar8x32_epi32( i0, perm );

            _mm256_storeu_si256((__m256i *)(y[i].qs + 32 * j), i0);
#else
            // Since we don't have in AVX some necessary functions,
            // we split the registers in half and call AVX2 analogs from SSE
            __m128i ni0 = _mm256_castsi256_si128( i0 );
            __m128i ni1 = _mm256_extractf128_si256( i0, 1);
            __m128i ni2 = _mm256_castsi256_si128( i1 );
            __m128i ni3 = _mm256_extractf128_si256( i1, 1);
            __m128i ni4 = _mm256_castsi256_si128( i2 );
            __m128i ni5 = _mm256_extractf128_si256( i2, 1);
            __m128i ni6 = _mm256_castsi256_si128( i3 );
            __m128i ni7 = _mm256_extractf128_si256( i3, 1);

            // Convert int32 to int16
            ni0 = _mm_packs_epi32( ni0, ni1 );
            ni2 = _mm_packs_epi32( ni2, ni3 );
            ni4 = _mm_packs_epi32( ni4, ni5 );
            ni6 = _mm_packs_epi32( ni6, ni7 );
            // Convert int16 to int8
            ni0 = _mm_packs_epi16( ni0, ni2 );
            ni4 = _mm_packs_epi16( ni4, ni6 );
            _mm_storeu_si128((__m128i *)(y[i].qs + 32 * j), ni0);
            _mm_storeu_si128((__m128i *)(y[i].qs + 32 * j + 16), ni4);
#endif
        }
    }
#else
    // scalar
    const int blck_size_interleave = 8;
    float srcv[4][QK8_0];
    float id[4];

    for (int i = 0; i < nb; i++) {
        for (int row_iter = 0; row_iter < 4; row_iter++) {
            float amax = 0.0f; // absolute max

            for (int j = 0; j < QK8_0; j++) {
                srcv[row_iter][j] = x[row_iter * k + i * QK8_0 + j];
                amax = MAX(amax, fabsf(srcv[row_iter][j]));
            }

            const float d = amax / ((1 << 7) - 1);
            id[row_iter] = d ? 1.0f / d : 0.0f;

            y[i].d[row_iter] = GGML_FP32_TO_FP16(d);
        }

        for (int j = 0; j < QK8_0 * 4; j++) {
            int src_offset = (j / (4 * blck_size_interleave)) * blck_size_interleave;
            int src_id = (j % (4 * blck_size_interleave)) / blck_size_interleave;
            src_offset += (j % blck_size_interleave);

            float x0 = srcv[src_id][src_offset] * id[src_id];
            y[i].qs[j] = roundf(x0);
        }
    }
#endif
}

void quantize_mat_q8_0(const float * restrict x, void * restrict vy, int64_t nrow, int64_t n_per_row, int64_t blck_size_interleave) {
    assert(nrow == 4);
    UNUSED(nrow);
    if (blck_size_interleave == 4) {
        quantize_q8_0_4x4(x, vy, n_per_row);
    } else if (blck_size_interleave == 8) {
        quantize_q8_0_4x8(x, vy, n_per_row);
    } else {
        assert(false);
    }
}

static size_t quantize_q4_0_nr_bl(const float * restrict src, void * restrict dst, int64_t nrow, int64_t n_per_row, int nrows_interleaved, int blck_size_interleave) {
    assert(n_per_row % QK4_0 == 0);
    const int nb = n_per_row / QK4_0;

    void * out_ptr = NULL;
    if (nrows_interleaved == 8) {
        out_ptr = (block_q4_0x8 *) dst;
    }
    else if (nrows_interleaved == 4) {
        out_ptr = (block_q4_0x4 *) dst;
    }
    assert(nrows_interleaved <= 8);
    block_q4_0 dst_tmp[8];

    for (int b = 0; b < (nrow * n_per_row); b += nrows_interleaved * n_per_row) {

        for (int64_t x = 0; x < nb; x++) {

            for (int i  = 0; i < nrows_interleaved; i++ ) {
                quantize_row_q4_0_ref(src + b + i * n_per_row + x * QK4_0, (block_q4_0 *) dst_tmp + i, QK4_0);
            }

            if (nrows_interleaved == 8) {
                *(block_q4_0x8 *) out_ptr = make_block_q4_0x8(dst_tmp, blck_size_interleave, 0x88);
                out_ptr = (block_q4_0x8 *) out_ptr + 1;
            }
            else if (nrows_interleaved == 4) {
                *(block_q4_0x4 *) out_ptr = make_block_q4_0x4(dst_tmp, blck_size_interleave, 0x88);
                out_ptr = (block_q4_0x4 *) out_ptr + 1;
            }
        }
    }

    return ((nrow * n_per_row) / QK4_0 * sizeof(block_q4_0));
}

size_t quantize_q4_0_4x4(const float * restrict src, void * restrict dst, int64_t nrow, int64_t n_per_row, const float * quant_weights) {
    UNUSED(quant_weights);
    return quantize_q4_0_nr_bl(src, dst, nrow, n_per_row, 4, 4);
}

size_t quantize_q4_0_4x8(const float * restrict src, void * restrict dst, int64_t nrow, int64_t n_per_row, const float * quant_weights) {
    UNUSED(quant_weights);
    return quantize_q4_0_nr_bl(src, dst, nrow, n_per_row, 4, 8);
}

size_t quantize_q4_0_8x8(const float * restrict src, void * restrict dst, int64_t nrow, int64_t n_per_row, const float * quant_weights) {
    UNUSED(quant_weights);
    return quantize_q4_0_nr_bl(src, dst, nrow, n_per_row, 8, 8);
}

void ggml_gemv_q4_0_4x4_q8_0(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy, int nr, int nc) {
    const int qk = QK8_0;
    const int nb = n / qk;
    const int ncols_interleaved = 4;
    const int blocklen = 4;

    assert (n % qk == 0);
    assert (nc % ncols_interleaved == 0);

    UNUSED(s);
    UNUSED(bs);
    UNUSED(vx);
    UNUSED(vy);
    UNUSED(nr);
    UNUSED(nc);
    UNUSED(nb);
    UNUSED(ncols_interleaved);
    UNUSED(blocklen);

#if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON)
    if (ggml_cpu_has_neon()) {
        const void * b_ptr = vx;
        const void * a_ptr = vy;
        float * res_ptr = s;

        __asm__ __volatile__(
            "movi v31.16b, #0x4\n"
            "movi v30.16b, #0xf0\n"
            "add %x[b_ptr], %x[b_ptr], #0x8\n"
            "1:"  // Column loop
            "add x22, %x[a_ptr], #0x2\n"
            "movi v29.16b, #0x0\n"
            "mov x21, %x[nb]\n"
            "2:"  // Block loop
            "ldr q28, [%x[b_ptr], #0x0]\n"
            "ldr q27, [x22, #0x0]\n"
            "movi v26.4s, #0x0\n"
            "sub x20, x22, #0x2\n"
            "ldr q25, [x22, #0x10]\n"
            "ldr q24, [%x[b_ptr], #0x10]\n"
            "sub x21, x21, #0x1\n"
            "add x22, x22, #0x22\n"
            "ldr q23, [%x[b_ptr], #0x20]\n"
            "ldr q22, [%x[b_ptr], #0x30]\n"
            "ld1r { v21.8h }, [x20]\n"
            "ldr q20, [%x[b_ptr], #-0x8]\n"
            "sshl v16.16b, v28.16b, v31.16b\n"
            "and v28.16b, v28.16b, v30.16b\n"
            "sshl v19.16b, v24.16b, v31.16b\n"
            "and v24.16b, v24.16b, v30.16b\n"
            "add %x[b_ptr], %x[b_ptr], #0x48\n"
            "sshl v18.16b, v23.16b, v31.16b\n"
            "and v23.16b, v23.16b, v30.16b\n"
            ".inst 0x4f9be21a  // sdot v26.4s, v16.16b, v27.4b[0]\n"
            "sshl v17.16b, v22.16b, v31.16b\n"
            "and v22.16b, v22.16b, v30.16b\n"
            "fcvtl v21.4s, v21.4h\n"
            "fcvtl v16.4s, v20.4h\n"
            ".inst 0x4f99e39a  // sdot v26.4s, v28.16b, v25.4b[0]\n"
            "fmul v16.4s, v16.4s, v21.4s\n"
            ".inst 0x4fbbe27a  // sdot v26.4s, v19.16b, v27.4b[1]\n"
            ".inst 0x4fb9e31a  // sdot v26.4s, v24.16b, v25.4b[1]\n"
            ".inst 0x4f9bea5a  // sdot v26.4s, v18.16b, v27.4b[2]\n"
            ".inst 0x4f99eafa  // sdot v26.4s, v23.16b, v25.4b[2]\n"
            ".inst 0x4fbbea3a  // sdot v26.4s, v17.16b, v27.4b[3]\n"
            ".inst 0x4fb9eada  // sdot v26.4s, v22.16b, v25.4b[3]\n"
            "scvtf v26.4s, v26.4s, #0x4\n"
            "fmla v29.4s, v26.4s, v16.4s\n"
            "cbnz x21, 2b\n"
            "sub %x[nc], %x[nc], #0x4\n"
            "str q29, [%x[res_ptr], #0x0]\n"
            "add %x[res_ptr], %x[res_ptr], #0x10\n"
            "cbnz %x[nc], 1b\n"
            : [b_ptr] "+&r" (b_ptr), [res_ptr] "+&r" (res_ptr), [nc] "+&r" (nc)
            : [a_ptr] "r" (a_ptr), [nb] "r" (nb)
            : "memory", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "x20", "x21", "x22"
            );
        return;
    }
#endif // #if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON)
    float sumf[4];
    int sumi;

    const block_q8_0 * a_ptr = (const block_q8_0 *) vy;
    for (int x = 0; x < nc / ncols_interleaved; x++) {
        const block_q4_0x4 * b_ptr = (const block_q4_0x4 *) vx + (x * nb);

        for (int j = 0; j < ncols_interleaved; j++) sumf[j] = 0.0;
        for (int l = 0; l < nb; l++) {
            for (int k = 0; k < (qk / (2 * blocklen)); k++) {
                for (int j = 0; j < ncols_interleaved; j++) {
                    sumi = 0;
                    for (int i = 0; i < blocklen; ++i) {
                        const int v0 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] << 4);
                        const int v1 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] & 0xF0);
                        sumi += ((v0 * a_ptr[l].qs[k * blocklen + i]) + (v1 * a_ptr[l].qs[k * blocklen + i + qk / 2])) >> 4;
                    }
                    sumf[j] += sumi * GGML_FP16_TO_FP32(b_ptr[l].d[j]) * GGML_FP16_TO_FP32(a_ptr[l].d);
                }
            }
        }
        for (int j = 0; j < ncols_interleaved; j++) s[x * ncols_interleaved + j] = sumf[j];
    }
}

void ggml_gemv_q4_0_4x8_q8_0(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy, int nr, int nc) {
    const int qk = QK8_0;
    const int nb = n / qk;
    const int ncols_interleaved = 4;
    const int blocklen = 8;

    assert (n % qk == 0);
    assert (nc % ncols_interleaved == 0);

    UNUSED(s);
    UNUSED(bs);
    UNUSED(vx);
    UNUSED(vy);
    UNUSED(nr);
    UNUSED(nc);
    UNUSED(nb);
    UNUSED(ncols_interleaved);
    UNUSED(blocklen);

#if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON) && defined(__ARM_FEATURE_MATMUL_INT8)
    if (ggml_cpu_has_neon() && ggml_cpu_has_matmul_int8()) {
        const void * b_ptr = vx;
        const void * a_ptr = vy;
        float * res_ptr = s;

        __asm__ __volatile__(
            "movi v2.16b, #0x4\n"
            "movi v1.16b, #0xf0\n"
            "add %x[b_ptr], %x[b_ptr], #0x8\n"
            "1:"  // Column loop
            "add x23, %x[a_ptr], #0x2\n"
            "movi v0.16b, #0x0\n"
            "mov x22, %x[nb]\n"
            "2:"  // Block loop
            "ldr q31, [%x[b_ptr], #0x0]\n"
            "ldr q30, [%x[b_ptr], #0x10]\n"
            "mov x21, x23\n"
            "movi v29.4s, #0x0\n"
            "ldr q28, [%x[b_ptr], #0x20]\n"
            "ldr q27, [%x[b_ptr], #0x30]\n"
            "movi v26.4s, #0x0\n"
            "sub x20, x23, #0x2\n"
            "ld1r { v25.8h }, [x20]\n"
            "ldr q24, [%x[b_ptr], #-0x8]\n"
            "sub x22, x22, #0x1\n"
            "add x23, x23, #0x22\n"
            "ld1r { v23.2d }, [x21], #0x8\n"
            "sshl v22.16b, v31.16b, v2.16b\n"
            "sshl v16.16b, v30.16b, v2.16b\n"
            "add %x[b_ptr], %x[b_ptr], #0x48\n"
            "ld1r { v21.2d }, [x21], #0x8\n"
            "sshl v20.16b, v28.16b, v2.16b\n"
            "sshl v19.16b, v27.16b, v2.16b\n"
            "ld1r { v18.2d }, [x21], #0x8\n"
            "ld1r { v17.2d }, [x21], #0x8\n"
            "and v31.16b, v31.16b, v1.16b\n"
            "and v30.16b, v30.16b, v1.16b\n"
            ".inst 0x4e9796dd  // sdot v29.4s, v22.16b, v23.16b\n"
            ".inst 0x4e97961a  // sdot v26.4s, v16.16b, v23.16b\n"
            "and v28.16b, v28.16b, v1.16b\n"
            "and v27.16b, v27.16b, v1.16b\n"
            "fcvtl v25.4s, v25.4h\n"
            "fcvtl v16.4s, v24.4h\n"
            ".inst 0x4e95969d  // sdot v29.4s, v20.16b, v21.16b\n"
            ".inst 0x4e95967a  // sdot v26.4s, v19.16b, v21.16b\n"
            "fmul v16.4s, v16.4s, v25.4s\n"
            ".inst 0x4e9297fd  // sdot v29.4s, v31.16b, v18.16b\n"
            ".inst 0x4e9297da  // sdot v26.4s, v30.16b, v18.16b\n"
            ".inst 0x4e91979d  // sdot v29.4s, v28.16b, v17.16b\n"
            ".inst 0x4e91977a  // sdot v26.4s, v27.16b, v17.16b\n"
            "addp v29.4s, v29.4s, v26.4s\n"
            "scvtf v29.4s, v29.4s, #0x4\n"
            "fmla v0.4s, v29.4s, v16.4s\n"
            "cbnz x22, 2b\n"
            "sub %x[nc], %x[nc], #0x4\n"
            "str q0, [%x[res_ptr], #0x0]\n"
            "add %x[res_ptr], %x[res_ptr], #0x10\n"
            "cbnz %x[nc], 1b\n"
            : [b_ptr] "+&r" (b_ptr), [res_ptr] "+&r" (res_ptr), [nc] "+&r" (nc)
            : [a_ptr] "r" (a_ptr), [nb] "r" (nb)
            : "memory", "v0", "v1", "v2", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "x20", "x21", "x22", "x23"
        );
        return;
    }
#endif // #if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON) && defined(__ARM_FEATURE_MATMUL_INT8)
    float sumf[4];
    int sumi;

    const block_q8_0 * a_ptr = (const block_q8_0 *) vy;
    for (int x = 0; x < nc / ncols_interleaved; x++) {
        const block_q4_0x4 * b_ptr = (const block_q4_0x4 *) vx + (x * nb);

        for (int j = 0; j < ncols_interleaved; j++) sumf[j] = 0.0;
        for (int l = 0; l < nb; l++) {
            for (int k = 0; k < (qk / (2 * blocklen)); k++) {
                for (int j = 0; j < ncols_interleaved; j++) {
                    sumi = 0;
                    for (int i = 0; i < blocklen; ++i) {
                        const int v0 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] << 4);
                        const int v1 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] & 0xF0);
                        sumi += ((v0 * a_ptr[l].qs[k * blocklen + i]) + (v1 * a_ptr[l].qs[k * blocklen + i + qk / 2])) >> 4;
                    }
                    sumf[j] += sumi * GGML_FP16_TO_FP32(b_ptr[l].d[j]) * GGML_FP16_TO_FP32(a_ptr[l].d);
                }
            }
        }
        for (int j = 0; j < ncols_interleaved; j++) s[x * ncols_interleaved + j] = sumf[j];
    }
}

void ggml_gemv_q4_0_8x8_q8_0(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy, int nr, int nc) {
    const int qk = QK8_0;
    const int nb = n / qk;
    const int ncols_interleaved = 8;
    const int blocklen = 8;

    assert (n % qk == 0);
    assert (nc % ncols_interleaved == 0);

    UNUSED(s);
    UNUSED(bs);
    UNUSED(vx);
    UNUSED(vy);
    UNUSED(nr);
    UNUSED(nc);
    UNUSED(nb);
    UNUSED(ncols_interleaved);
    UNUSED(blocklen);

#if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__)
#if defined(__ARM_FEATURE_SVE)
    if (ggml_cpu_has_sve() && ggml_cpu_get_sve_cnt() == QK8_0) {
        const void * b_ptr = vx;
        const void * a_ptr = vy;
        float * res_ptr = s;

        __asm__ __volatile__(
            "ptrue p0.b\n"
            "add %x[b_ptr], %x[b_ptr], #0x10\n"
            "1:"  // Column loop
            "add x22, %x[a_ptr], #0x2\n"
            "mov z31.b, #0x0\n"
            "mov x21, %x[nb]\n"
            "2:"  // Block loop
            "ld1b { z30.b }, p0/Z, [%x[b_ptr]]\n"
            "ld1b { z29.b }, p0/Z, [%x[b_ptr], #1, MUL VL]\n"
            "mov z28.s, #0x0\n"
            "mov z27.s, #0x0\n"
            "ld1rd { z26.d }, p0/Z, [x22]\n"
            "ld1b { z25.b }, p0/Z, [%x[b_ptr], #2, MUL VL]\n"
            "sub x20, x22, #0x2\n"
            "sub x21, x21, #0x1\n"
            "ld1b { z24.b }, p0/Z, [%x[b_ptr], #3, MUL VL]\n"
            "ld1rd { z23.d }, p0/Z, [x22, #8]\n"
            "lsl z22.b, z30.b, #0x4\n"
            "lsl z16.b, z29.b, #0x4\n"
            "and z30.b, z30.b, #0xf0\n"
            "and z29.b, z29.b, #0xf0\n"
            "ld1rd { z21.d }, p0/Z, [x22, #16]\n"
            "ld1rd { z20.d }, p0/Z, [x22, #24]\n"
            "lsl z19.b, z25.b, #0x4\n"
            "and z25.b, z25.b, #0xf0\n"
            "ld1rh { z17.h }, p0/Z, [x20]\n"
            "ld1h { z18.s }, p0/Z, [%x[b_ptr], #-1, MUL VL]\n"
            "sdot z28.s, z22.b, z26.b\n"
            "sdot z27.s, z16.b, z26.b\n"
            "lsl z16.b, z24.b, #0x4\n"
            "add x22, x22, #0x22\n"
            "and z24.b, z24.b, #0xf0\n"
            "add %x[b_ptr], %x[b_ptr], #0x90\n"
            "fcvt z17.s, p0/m, z17.h\n"
            "fcvt z18.s, p0/m, z18.h\n"
            "sdot z28.s, z19.b, z23.b\n"
            "sdot z27.s, z16.b, z23.b\n"
            "fmul z18.s, z18.s, z17.s\n"
            "sdot z28.s, z30.b, z21.b\n"
            "sdot z27.s, z29.b, z21.b\n"
            "sdot z28.s, z25.b, z20.b\n"
            "sdot z27.s, z24.b, z20.b\n"
            "uzp1 z17.s, z28.s, z27.s\n"
            "uzp2 z16.s, z28.s, z27.s\n"
            "add z17.s, z17.s, z16.s\n"
            "asr z17.s, z17.s, #0x4\n"
            "scvtf z17.s, p0/m, z17.s\n"
            "fmla z31.s, p0/M, z17.s, z18.s\n"
            "cbnz x21, 2b\n"
            "sub %x[nc], %x[nc], #0x8\n"
            "st1w { z31.s }, p0, [%x[res_ptr]]\n"
            "add %x[res_ptr], %x[res_ptr], #0x20\n"
            "cbnz %x[nc], 1b\n"
            : [b_ptr] "+&r" (b_ptr), [res_ptr] "+&r" (res_ptr), [nc] "+&r" (nc)
            : [a_ptr] "r" (a_ptr), [nb] "r" (nb)
            : "memory", "p0", "x20", "x21", "x22", "z16", "z17", "z18", "z19", "z20", "z21", "z22", "z23", "z24", "z25", "z26", "z27", "z28", "z29", "z30", "z31"
        );
        return;
    }
#endif // #if defined(__ARM_FEATURE_SVE)
#elif defined(__AVX2__)
    // Lookup table to convert signed nibbles to signed bytes
    __m256i signextendlut = _mm256_castsi128_si256(_mm_set_epi8(-1, -2, -3, -4, -5, -6, -7, -8, 7, 6, 5, 4, 3, 2, 1, 0));
    signextendlut = _mm256_permute2f128_si256(signextendlut, signextendlut, 0);
    __m128i changemask = _mm_set_epi8(15, 14, 7, 6, 13, 12, 5, 4, 11, 10, 3, 2, 9, 8, 1, 0);
    __m256i finalpermutemask = _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0);

    // Permute mask used for easier vector processing at later stages
    const __m256i m4b = _mm256_set1_epi8(0x0F);

    int64_t b_nb = n / QK4_0;

    const block_q4_0x8 * b_ptr_start = (const block_q4_0x8 *)vx;
    const block_q8_0 * a_ptr_start = (const block_q8_0 *)vy;

    // Process Q8_0 blocks one by one
    for (int64_t y = 0; y < nr; y++) {

        // Pointers to LHS blocks of block_q8_0 format
        const block_q8_0 * a_ptr = a_ptr_start + (y * nb);

        // Take group of eight block_q4_0x8 structures at each pass of the loop and perform dot product operation
        for (int64_t x = 0; x < nc / 8; x++) {

            // Pointers to RHS blocks
            const block_q4_0x8 * b_ptr = b_ptr_start + (x * b_nb);

            // Master FP accumulator
            __m256 acc_row = _mm256_setzero_ps();

            for (int64_t b = 0; b < nb; b++) {
                // Load 8 blocks of Q4_0 interleaved as 8 bytes (B0 - B7)
                const __m256i rhs_raw_vec_0123_0 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs));
                const __m256i rhs_raw_vec_4567_0 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs) + 1);
                const __m256i rhs_raw_vec_0123_1 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs) + 2);
                const __m256i rhs_raw_vec_4567_1 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs) + 3);

                // 4-bit -> 8-bit - Sign is maintained
                const __m256i rhs_vec_0123_0 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_vec_0123_0, m4b)); // B0(0-7) B1(0-7) B2(0-7) B3(0-7)
                const __m256i rhs_vec_4567_0 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_vec_4567_0, m4b)); // B4(0-7) B5(0-7) B6(0-7) B7(0-7)
                const __m256i rhs_vec_0123_1 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_vec_0123_1, m4b)); // B0(8-15) B1(8-15) B2(8-15) B3(8-15)
                const __m256i rhs_vec_4567_1 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_vec_4567_1, m4b)); // B0(8-15) B1(8-15) B2(8-15) B3(8-15)

                const __m256i rhs_vec_0123_2 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_vec_0123_0, 4), m4b)); // B0(16-23) B1(16-23) B2(16-23) B3(16-23)
                const __m256i rhs_vec_4567_2 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_vec_4567_0, 4), m4b)); // B4(16-23) B5(16-23) B6(16-23) B7(16-23)
                const __m256i rhs_vec_0123_3 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_vec_0123_1, 4), m4b)); // B0(24-31) B1(24-31) B2(24-31) B3(24-31)
                const __m256i rhs_vec_4567_3 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_vec_4567_1, 4), m4b)); // B4(24-31) B5(24-31) B6(24-31) B7(24-31)

                // Load the scale values for the 8 blocks interleaved in block_q4_0x8
                const __m256 col_scale_f32 = GGML_F32Cx8_REARRANGE_LOAD(b_ptr[b].d, changemask);

                // Load and convert to FP32 scale from block_q8_0
                const __m256 row_scale_f32 = _mm256_set1_ps(GGML_FP16_TO_FP32(a_ptr[b].d));

                // Load the block values in block_q8_0 in batches of 16 bytes and replicate the same across 256 bit vector
                __m256i lhs_vec_0 = _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)a_ptr[b].qs));
                __m256i lhs_vec_1 = _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)(a_ptr[b].qs + 16)));

                lhs_vec_0 = _mm256_permute2f128_si256(lhs_vec_0, lhs_vec_0, 0); // A0 (0-15) A0(0-15)
                lhs_vec_1 = _mm256_permute2f128_si256(lhs_vec_1, lhs_vec_1, 0); // A0 (16-31) A0(16-31))

                __m256i iacc = _mm256_setzero_si256();

                // Dot product done within 32 bit lanes and accumulated in the same vector
                // B0(0-3) B4(0-3) B1(0-3) B5(0-3) B2(0-3) B6(0-3) B3(0-3) B7(0-3) with A0(0-3)
                // B0(4-7) B4(4-7) B1(4-7) B5(4-7) B2(4-7) B6(4-7) B3(4-7) B7(4-7) with A0(4-7)
                // ...........................................................................
                // B0(28-31) B4(28-31) B1(28-31) B5(28-31) B2(28-31) B6(28-31) B3(28-31) B7(28-31) with A0(28-31)

                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(rhs_vec_0123_0 ,_mm256_shuffle_epi32(rhs_vec_4567_0, 177), 170), _mm256_shuffle_epi32(lhs_vec_0, 0)));
                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(_mm256_shuffle_epi32(rhs_vec_0123_0, 177) ,rhs_vec_4567_0, 170), _mm256_shuffle_epi32(lhs_vec_0, 85)));

                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(rhs_vec_0123_1 ,_mm256_shuffle_epi32(rhs_vec_4567_1, 177), 170), _mm256_shuffle_epi32(lhs_vec_0, 170)));
                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(_mm256_shuffle_epi32(rhs_vec_0123_1, 177) ,rhs_vec_4567_1, 170), _mm256_shuffle_epi32(lhs_vec_0, 255)));

                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(rhs_vec_0123_2 ,_mm256_shuffle_epi32(rhs_vec_4567_2, 177), 170), _mm256_shuffle_epi32(lhs_vec_1, 0)));
                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(_mm256_shuffle_epi32(rhs_vec_0123_2, 177) ,rhs_vec_4567_2, 170), _mm256_shuffle_epi32(lhs_vec_1, 85)));

                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(rhs_vec_0123_3 ,_mm256_shuffle_epi32(rhs_vec_4567_3, 177), 170), _mm256_shuffle_epi32(lhs_vec_1, 170)));
                iacc = _mm256_add_epi32(iacc, mul_sum_i8_pairs_int32x8(_mm256_blend_epi32(_mm256_shuffle_epi32(rhs_vec_0123_3, 177) ,rhs_vec_4567_3, 170), _mm256_shuffle_epi32(lhs_vec_1, 255)));

                // Accumulated values multipled with appropriate scales
                acc_row = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc), _mm256_mul_ps(col_scale_f32, row_scale_f32), acc_row);
            }

            // Accumulated output values permuted so as to be stored in appropriate order post accumulation
            acc_row = _mm256_permutevar8x32_ps(acc_row, finalpermutemask);
            _mm256_storeu_ps(s + (y * nr + x * 8), acc_row);
        }
    }
    return;
#endif // #if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__)
    {
        float sumf[8];
        int sumi;

        const block_q8_0 * a_ptr = (const block_q8_0 *) vy;
        for (int x = 0; x < nc / ncols_interleaved; x++) {
            const block_q4_0x8 * b_ptr = (const block_q4_0x8 *) vx + (x * nb);

            for (int j = 0; j < ncols_interleaved; j++) sumf[j] = 0.0;
            for (int l = 0; l < nb; l++) {
                for (int k = 0; k < (qk / (2 * blocklen)); k++) {
                    for (int j = 0; j < ncols_interleaved; j++) {
                        sumi = 0;
                        for (int i = 0; i < blocklen; ++i) {
                            const int v0 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] << 4);
                            const int v1 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] & 0xF0);
                            sumi += ((v0 * a_ptr[l].qs[k * blocklen + i]) + (v1 * a_ptr[l].qs[k * blocklen + i + qk / 2])) >> 4;
                        }
                        sumf[j] += sumi * GGML_FP16_TO_FP32(b_ptr[l].d[j]) * GGML_FP16_TO_FP32(a_ptr[l].d);
                    }
                }
            }
            for (int j = 0; j < ncols_interleaved; j++) s[x * ncols_interleaved + j] = sumf[j];
        }
    }
}

void ggml_gemm_q4_0_4x4_q8_0(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy, int nr, int nc) {
    const int qk = QK8_0;
    const int nb = n / qk;
    const int ncols_interleaved = 4;
    const int blocklen = 4;

    assert (n % qk == 0);
    assert (nr % 4 == 0);
    assert (nc % ncols_interleaved == 0);

    UNUSED(s);
    UNUSED(bs);
    UNUSED(vx);
    UNUSED(vy);
    UNUSED(nr);
    UNUSED(nc);
    UNUSED(nb);
    UNUSED(ncols_interleaved);
    UNUSED(blocklen);

#if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON)
    if (ggml_cpu_has_neon()) {
        const void * b_ptr = vx;
        const void * a_ptr = vy;
        float * res_ptr = s;
        size_t res_stride = bs * sizeof(float);

        __asm__ __volatile__(
            "mov x10, %x[nr]\n"
            "mov x9, #0x88\n"
            "cmp x10, #0x10\n"
            "mul x9, %x[nb], x9\n"
            "blt 4f\n"
            "1:"  // Row loop
            "add x28, %x[b_ptr], #0x8\n"
            "mov x27, %x[nc]\n"
            "add x26, %x[res_ptr], %x[res_stride], LSL #4\n"
            "2:"  // Column loop
            "add x25, %x[a_ptr], #0x8\n"
            "movi v15.16b, #0x0\n"
            "movi v19.16b, #0x0\n"
            "mov x24, %x[nb]\n"
            "add x23, x25, x9\n"
            "movi v18.16b, #0x0\n"
            "movi v14.16b, #0x0\n"
            "add x22, x23, x9\n"
            "movi v11.16b, #0x0\n"
            "movi v13.16b, #0x0\n"
            "add x21, x22, x9\n"
            "movi v23.16b, #0x0\n"
            "movi v16.16b, #0x0\n"
            "movi v25.16b, #0x0\n"
            "movi v7.16b, #0x0\n"
            "movi v0.16b, #0x0\n"
            "movi v4.16b, #0x0\n"
            "movi v5.16b, #0x0\n"
            "movi v21.16b, #0x0\n"
            "movi v8.16b, #0x0\n"
            "movi v1.16b, #0x0\n"
            "3:"  // Block loop
            "ldr q3, [x28, #0x0]\n"
            "ldr q31, [x25, #0x0]\n"
            "movi v28.16b, #0x4\n"
            "movi v10.4s, #0x0\n"
            "ldr q22, [x28, #0x10]\n"
            "ldr q6, [x25, #0x10]\n"
            "movi v29.4s, #0x0\n"
            "movi v9.4s, #0x0\n"
            "ldr q27, [x28, #0x20]\n"
            "ldr q30, [x28, #0x30]\n"
            "movi v20.4s, #0x0\n"
            "movi v24.16b, #0xf0\n"
            "ldr d2, [x25, #-0x8]\n"
            "ldr d26, [x23, #-0x8]\n"
            "sshl v12.16b, v3.16b, v28.16b\n"
            "sub x20, x28, #0x8\n"
            "ldr d17, [x20, #0x0]\n"
            "and v3.16b, v3.16b, v24.16b\n"
            "subs x24, x24, #0x1\n"
            "add x28, x28, #0x48\n"
            ".inst 0x4f9fe18a  // sdot v10.4s, v12.16b, v31.4b[0]\n"
            ".inst 0x4fbfe19d  // sdot v29.4s, v12.16b, v31.4b[1]\n"
            ".inst 0x4f9fe989  // sdot v9.4s, v12.16b, v31.4b[2]\n"
            ".inst 0x4fbfe994  // sdot v20.4s, v12.16b, v31.4b[3]\n"
            "sshl v31.16b, v22.16b, v28.16b\n"
            "and v22.16b, v22.16b, v24.16b\n"
            "fcvtl v17.4s, v17.4h\n"
            "fcvtl v2.4s, v2.4h\n"
            "fcvtl v26.4s, v26.4h\n"
            ".inst 0x4f86e3ea  // sdot v10.4s, v31.16b, v6.4b[0]\n"
            ".inst 0x4fa6e3fd  // sdot v29.4s, v31.16b, v6.4b[1]\n"
            ".inst 0x4f86ebe9  // sdot v9.4s, v31.16b, v6.4b[2]\n"
            ".inst 0x4fa6ebf4  // sdot v20.4s, v31.16b, v6.4b[3]\n"
            "sshl v6.16b, v27.16b, v28.16b\n"
            "sshl v28.16b, v30.16b, v28.16b\n"
            "and v27.16b, v27.16b, v24.16b\n"
            "and v30.16b, v30.16b, v24.16b\n"
            "ldr q24, [x25, #0x20]\n"
            ".inst 0x4f98e0ca  // sdot v10.4s, v6.16b, v24.4b[0]\n"
            ".inst 0x4fb8e0dd  // sdot v29.4s, v6.16b, v24.4b[1]\n"
            ".inst 0x4f98e8c9  // sdot v9.4s, v6.16b, v24.4b[2]\n"
            ".inst 0x4fb8e8d4  // sdot v20.4s, v6.16b, v24.4b[3]\n"
            "ldr q24, [x25, #0x30]\n"
            ".inst 0x4f98e38a  // sdot v10.4s, v28.16b, v24.4b[0]\n"
            ".inst 0x4fb8e39d  // sdot v29.4s, v28.16b, v24.4b[1]\n"
            ".inst 0x4f98eb89  // sdot v9.4s, v28.16b, v24.4b[2]\n"
            ".inst 0x4fb8eb94  // sdot v20.4s, v28.16b, v24.4b[3]\n"
            "ldr q24, [x25, #0x40]\n"
            ".inst 0x4f98e06a  // sdot v10.4s, v3.16b, v24.4b[0]\n"
            ".inst 0x4fb8e07d  // sdot v29.4s, v3.16b, v24.4b[1]\n"
            ".inst 0x4f98e869  // sdot v9.4s, v3.16b, v24.4b[2]\n"
            ".inst 0x4fb8e874  // sdot v20.4s, v3.16b, v24.4b[3]\n"
            "ldr q24, [x25, #0x50]\n"
            ".inst 0x4f98e2ca  // sdot v10.4s, v22.16b, v24.4b[0]\n"
            ".inst 0x4fb8e2dd  // sdot v29.4s, v22.16b, v24.4b[1]\n"
            ".inst 0x4f98eac9  // sdot v9.4s, v22.16b, v24.4b[2]\n"
            ".inst 0x4fb8ead4  // sdot v20.4s, v22.16b, v24.4b[3]\n"
            "ldr q24, [x25, #0x60]\n"
            ".inst 0x4f98e36a  // sdot v10.4s, v27.16b, v24.4b[0]\n"
            ".inst 0x4fb8e37d  // sdot v29.4s, v27.16b, v24.4b[1]\n"
            ".inst 0x4f98eb69  // sdot v9.4s, v27.16b, v24.4b[2]\n"
            ".inst 0x4fb8eb74  // sdot v20.4s, v27.16b, v24.4b[3]\n"
            "ldr q24, [x25, #0x70]\n"
            "add x25, x25, #0x88\n"
            ".inst 0x4f98e3ca  // sdot v10.4s, v30.16b, v24.4b[0]\n"
            ".inst 0x4fb8e3dd  // sdot v29.4s, v30.16b, v24.4b[1]\n"
            ".inst 0x4f98ebc9  // sdot v9.4s, v30.16b, v24.4b[2]\n"
            ".inst 0x4fb8ebd4  // sdot v20.4s, v30.16b, v24.4b[3]\n"
            "fmul v24.4s, v17.4s, v2.s[0]\n"
            "scvtf v10.4s, v10.4s, #0x4\n"
            "scvtf v29.4s, v29.4s, #0x4\n"
            "scvtf v9.4s, v9.4s, #0x4\n"
            "scvtf v20.4s, v20.4s, #0x4\n"
            "fmla v15.4s, v10.4s, v24.4s\n"
            "ldr q24, [x23, #0x0]\n"
            "fmul v10.4s, v17.4s, v2.s[1]\n"
            "fmla v19.4s, v29.4s, v10.4s\n"
            "ldr q10, [x23, #0x10]\n"
            "fmul v29.4s, v17.4s, v2.s[2]\n"
            "fmul v2.4s, v17.4s, v2.s[3]\n"
            "fmla v18.4s, v9.4s, v29.4s\n"
            "movi v9.4s, #0x0\n"
            "movi v29.4s, #0x0\n"
            ".inst 0x4f98e189  // sdot v9.4s, v12.16b, v24.4b[0]\n"
            ".inst 0x4fb8e19d  // sdot v29.4s, v12.16b, v24.4b[1]\n"
            "fmla v14.4s, v20.4s, v2.4s\n"
            "movi v20.4s, #0x0\n"
            "movi v2.4s, #0x0\n"
            ".inst 0x4f98e994  // sdot v20.4s, v12.16b, v24.4b[2]\n"
            ".inst 0x4fb8e982  // sdot v2.4s, v12.16b, v24.4b[3]\n"
            "ldr q24, [x23, #0x20]\n"
            ".inst 0x4f8ae3e9  // sdot v9.4s, v31.16b, v10.4b[0]\n"
            ".inst 0x4faae3fd  // sdot v29.4s, v31.16b, v10.4b[1]\n"
            ".inst 0x4f8aebf4  // sdot v20.4s, v31.16b, v10.4b[2]\n"
            ".inst 0x4faaebe2  // sdot v2.4s, v31.16b, v10.4b[3]\n"
            "ldr q10, [x23, #0x30]\n"
            ".inst 0x4f98e0c9  // sdot v9.4s, v6.16b, v24.4b[0]\n"
            ".inst 0x4fb8e0dd  // sdot v29.4s, v6.16b, v24.4b[1]\n"
            ".inst 0x4f98e8d4  // sdot v20.4s, v6.16b, v24.4b[2]\n"
            ".inst 0x4fb8e8c2  // sdot v2.4s, v6.16b, v24.4b[3]\n"
            "ldr q24, [x23, #0x40]\n"
            ".inst 0x4f8ae389  // sdot v9.4s, v28.16b, v10.4b[0]\n"
            ".inst 0x4faae39d  // sdot v29.4s, v28.16b, v10.4b[1]\n"
            ".inst 0x4f8aeb94  // sdot v20.4s, v28.16b, v10.4b[2]\n"
            ".inst 0x4faaeb82  // sdot v2.4s, v28.16b, v10.4b[3]\n"
            "ldr q10, [x23, #0x50]\n"
            ".inst 0x4f98e069  // sdot v9.4s, v3.16b, v24.4b[0]\n"
            ".inst 0x4fb8e07d  // sdot v29.4s, v3.16b, v24.4b[1]\n"
            ".inst 0x4f98e874  // sdot v20.4s, v3.16b, v24.4b[2]\n"
            ".inst 0x4fb8e862  // sdot v2.4s, v3.16b, v24.4b[3]\n"
            "ldr q24, [x23, #0x60]\n"
            ".inst 0x4f8ae2c9  // sdot v9.4s, v22.16b, v10.4b[0]\n"
            ".inst 0x4faae2dd  // sdot v29.4s, v22.16b, v10.4b[1]\n"
            ".inst 0x4f8aead4  // sdot v20.4s, v22.16b, v10.4b[2]\n"
            ".inst 0x4faaeac2  // sdot v2.4s, v22.16b, v10.4b[3]\n"
            "ldr q10, [x23, #0x70]\n"
            "add x23, x23, #0x88\n"
            ".inst 0x4f98e369  // sdot v9.4s, v27.16b, v24.4b[0]\n"
            ".inst 0x4fb8e37d  // sdot v29.4s, v27.16b, v24.4b[1]\n"
            ".inst 0x4f98eb74  // sdot v20.4s, v27.16b, v24.4b[2]\n"
            ".inst 0x4fb8eb62  // sdot v2.4s, v27.16b, v24.4b[3]\n"
            "ldr q24, [x22, #0x0]\n"
            ".inst 0x4f8ae3c9  // sdot v9.4s, v30.16b, v10.4b[0]\n"
            ".inst 0x4faae3dd  // sdot v29.4s, v30.16b, v10.4b[1]\n"
            ".inst 0x4f8aebd4  // sdot v20.4s, v30.16b, v10.4b[2]\n"
            ".inst 0x4faaebc2  // sdot v2.4s, v30.16b, v10.4b[3]\n"
            "fmul v10.4s, v17.4s, v26.s[0]\n"
            "scvtf v9.4s, v9.4s, #0x4\n"
            "scvtf v29.4s, v29.4s, #0x4\n"
            "scvtf v20.4s, v20.4s, #0x4\n"
            "scvtf v2.4s, v2.4s, #0x4\n"
            "fmla v11.4s, v9.4s, v10.4s\n"
            "ldr q9, [x22, #0x10]\n"
            "fmul v10.4s, v17.4s, v26.s[1]\n"
            "fmla v13.4s, v29.4s, v10.4s\n"
            "ldr d29, [x22, #-0x8]\n"
            "fmul v10.4s, v17.4s, v26.s[2]\n"
            "fmul v26.4s, v17.4s, v26.s[3]\n"
            "fcvtl v29.4s, v29.4h\n"
            "fmla v23.4s, v20.4s, v10.4s\n"
            "movi v20.4s, #0x0\n"
            "movi v10.4s, #0x0\n"
            "fmla v16.4s, v2.4s, v26.4s\n"
            "movi v26.4s, #0x0\n"
            "movi v2.4s, #0x0\n"
            ".inst 0x4f98e194  // sdot v20.4s, v12.16b, v24.4b[0]\n"
            ".inst 0x4fb8e18a  // sdot v10.4s, v12.16b, v24.4b[1]\n"
            ".inst 0x4f98e99a  // sdot v26.4s, v12.16b, v24.4b[2]\n"
            ".inst 0x4fb8e982  // sdot v2.4s, v12.16b, v24.4b[3]\n"
            "ldr q24, [x22, #0x20]\n"
            ".inst 0x4f89e3f4  // sdot v20.4s, v31.16b, v9.4b[0]\n"
            ".inst 0x4fa9e3ea  // sdot v10.4s, v31.16b, v9.4b[1]\n"
            ".inst 0x4f89ebfa  // sdot v26.4s, v31.16b, v9.4b[2]\n"
            ".inst 0x4fa9ebe2  // sdot v2.4s, v31.16b, v9.4b[3]\n"
            "ldr q9, [x22, #0x30]\n"
            ".inst 0x4f98e0d4  // sdot v20.4s, v6.16b, v24.4b[0]\n"
            ".inst 0x4fb8e0ca  // sdot v10.4s, v6.16b, v24.4b[1]\n"
            ".inst 0x4f98e8da  // sdot v26.4s, v6.16b, v24.4b[2]\n"
            ".inst 0x4fb8e8c2  // sdot v2.4s, v6.16b, v24.4b[3]\n"
            "ldr q24, [x22, #0x40]\n"
            ".inst 0x4f89e394  // sdot v20.4s, v28.16b, v9.4b[0]\n"
            ".inst 0x4fa9e38a  // sdot v10.4s, v28.16b, v9.4b[1]\n"
            ".inst 0x4f89eb9a  // sdot v26.4s, v28.16b, v9.4b[2]\n"
            ".inst 0x4fa9eb82  // sdot v2.4s, v28.16b, v9.4b[3]\n"
            "ldr q9, [x22, #0x50]\n"
            ".inst 0x4f98e074  // sdot v20.4s, v3.16b, v24.4b[0]\n"
            ".inst 0x4fb8e06a  // sdot v10.4s, v3.16b, v24.4b[1]\n"
            ".inst 0x4f98e87a  // sdot v26.4s, v3.16b, v24.4b[2]\n"
            ".inst 0x4fb8e862  // sdot v2.4s, v3.16b, v24.4b[3]\n"
            "ldr q24, [x22, #0x60]\n"
            ".inst 0x4f89e2d4  // sdot v20.4s, v22.16b, v9.4b[0]\n"
            ".inst 0x4fa9e2ca  // sdot v10.4s, v22.16b, v9.4b[1]\n"
            ".inst 0x4f89eada  // sdot v26.4s, v22.16b, v9.4b[2]\n"
            ".inst 0x4fa9eac2  // sdot v2.4s, v22.16b, v9.4b[3]\n"
            "ldr q9, [x22, #0x70]\n"
            "add x22, x22, #0x88\n"
            ".inst 0x4f98e374  // sdot v20.4s, v27.16b, v24.4b[0]\n"
            ".inst 0x4fb8e36a  // sdot v10.4s, v27.16b, v24.4b[1]\n"
            ".inst 0x4f98eb7a  // sdot v26.4s, v27.16b, v24.4b[2]\n"
            ".inst 0x4fb8eb62  // sdot v2.4s, v27.16b, v24.4b[3]\n"
            "ldr q24, [x21, #0x0]\n"
            ".inst 0x4f89e3d4  // sdot v20.4s, v30.16b, v9.4b[0]\n"
            ".inst 0x4fa9e3ca  // sdot v10.4s, v30.16b, v9.4b[1]\n"
            ".inst 0x4f89ebda  // sdot v26.4s, v30.16b, v9.4b[2]\n"
            ".inst 0x4fa9ebc2  // sdot v2.4s, v30.16b, v9.4b[3]\n"
            "fmul v9.4s, v17.4s, v29.s[0]\n"
            "scvtf v20.4s, v20.4s, #0x4\n"
            "scvtf v10.4s, v10.4s, #0x4\n"
            "scvtf v26.4s, v26.4s, #0x4\n"
            "scvtf v2.4s, v2.4s, #0x4\n"
            "fmla v25.4s, v20.4s, v9.4s\n"
            "ldr q9, [x21, #0x10]\n"
            "fmul v20.4s, v17.4s, v29.s[1]\n"
            "fmla v7.4s, v10.4s, v20.4s\n"
            "ldr d20, [x21, #-0x8]\n"
            "fmul v10.4s, v17.4s, v29.s[2]\n"
            "fmul v29.4s, v17.4s, v29.s[3]\n"
            "fcvtl v20.4s, v20.4h\n"
            "fmla v0.4s, v26.4s, v10.4s\n"
            "movi v26.4s, #0x0\n"
            "movi v10.4s, #0x0\n"
            "fmla v4.4s, v2.4s, v29.4s\n"
            "movi v2.4s, #0x0\n"
            "movi v29.4s, #0x0\n"
            ".inst 0x4f98e19a  // sdot v26.4s, v12.16b, v24.4b[0]\n"
            ".inst 0x4fb8e18a  // sdot v10.4s, v12.16b, v24.4b[1]\n"
            ".inst 0x4f98e982  // sdot v2.4s, v12.16b, v24.4b[2]\n"
            ".inst 0x4fb8e99d  // sdot v29.4s, v12.16b, v24.4b[3]\n"
            "ldr q12, [x21, #0x20]\n"
            "fmul v24.4s, v17.4s, v20.s[0]\n"
            ".inst 0x4f89e3fa  // sdot v26.4s, v31.16b, v9.4b[0]\n"
            ".inst 0x4fa9e3ea  // sdot v10.4s, v31.16b, v9.4b[1]\n"
            ".inst 0x4f89ebe2  // sdot v2.4s, v31.16b, v9.4b[2]\n"
            ".inst 0x4fa9ebfd  // sdot v29.4s, v31.16b, v9.4b[3]\n"
            "ldr q9, [x21, #0x30]\n"
            "fmul v31.4s, v17.4s, v20.s[1]\n"
            ".inst 0x4f8ce0da  // sdot v26.4s, v6.16b, v12.4b[0]\n"
            ".inst 0x4face0ca  // sdot v10.4s, v6.16b, v12.4b[1]\n"
            ".inst 0x4f8ce8c2  // sdot v2.4s, v6.16b, v12.4b[2]\n"
            ".inst 0x4face8dd  // sdot v29.4s, v6.16b, v12.4b[3]\n"
            "ldr q12, [x21, #0x40]\n"
            "fmul v6.4s, v17.4s, v20.s[2]\n"
            "fmul v20.4s, v17.4s, v20.s[3]\n"
            ".inst 0x4f89e39a  // sdot v26.4s, v28.16b, v9.4b[0]\n"
            ".inst 0x4fa9e38a  // sdot v10.4s, v28.16b, v9.4b[1]\n"
            ".inst 0x4f89eb82  // sdot v2.4s, v28.16b, v9.4b[2]\n"
            ".inst 0x4fa9eb9d  // sdot v29.4s, v28.16b, v9.4b[3]\n"
            "ldr q9, [x21, #0x50]\n"
            ".inst 0x4f8ce07a  // sdot v26.4s, v3.16b, v12.4b[0]\n"
            ".inst 0x4face06a  // sdot v10.4s, v3.16b, v12.4b[1]\n"
            ".inst 0x4f8ce862  // sdot v2.4s, v3.16b, v12.4b[2]\n"
            ".inst 0x4face87d  // sdot v29.4s, v3.16b, v12.4b[3]\n"
            "ldr q12, [x21, #0x60]\n"
            ".inst 0x4f89e2da  // sdot v26.4s, v22.16b, v9.4b[0]\n"
            ".inst 0x4fa9e2ca  // sdot v10.4s, v22.16b, v9.4b[1]\n"
            ".inst 0x4f89eac2  // sdot v2.4s, v22.16b, v9.4b[2]\n"
            ".inst 0x4fa9eadd  // sdot v29.4s, v22.16b, v9.4b[3]\n"
            "ldr q17, [x21, #0x70]\n"
            "add x21, x21, #0x88\n"
            ".inst 0x4f8ce37a  // sdot v26.4s, v27.16b, v12.4b[0]\n"
            ".inst 0x4face36a  // sdot v10.4s, v27.16b, v12.4b[1]\n"
            ".inst 0x4f8ceb62  // sdot v2.4s, v27.16b, v12.4b[2]\n"
            ".inst 0x4faceb7d  // sdot v29.4s, v27.16b, v12.4b[3]\n"
            ".inst 0x4f91e3da  // sdot v26.4s, v30.16b, v17.4b[0]\n"
            ".inst 0x4fb1e3ca  // sdot v10.4s, v30.16b, v17.4b[1]\n"
            ".inst 0x4f91ebc2  // sdot v2.4s, v30.16b, v17.4b[2]\n"
            ".inst 0x4fb1ebdd  // sdot v29.4s, v30.16b, v17.4b[3]\n"
            "scvtf v26.4s, v26.4s, #0x4\n"
            "scvtf v10.4s, v10.4s, #0x4\n"
            "fmla v5.4s, v26.4s, v24.4s\n"
            "scvtf v2.4s, v2.4s, #0x4\n"
            "scvtf v29.4s, v29.4s, #0x4\n"
            "fmla v21.4s, v10.4s, v31.4s\n"
            "fmla v8.4s, v2.4s, v6.4s\n"
            "fmla v1.4s, v29.4s, v20.4s\n"
            "bgt 3b\n"
            "mov x20, %x[res_ptr]\n"
            "subs x27, x27, #0x4\n"
            "add %x[res_ptr], %x[res_ptr], #0x10\n"
            "str q15, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q19, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q18, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q14, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q11, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q13, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q23, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q16, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q25, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q7, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q0, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q4, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q5, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q21, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q8, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q1, [x20, #0x0]\n"
            "bne 2b\n"
            "mov x20, #0x4\n"
            "sub x10, x10, #0x10\n"
            "cmp x10, #0x10\n"
            "mov %x[res_ptr], x26\n"
            "madd %x[a_ptr], x20, x9, %x[a_ptr]\n"
            "bge 1b\n"
            "4:"  // Row loop skip
            "cbz x10, 9f\n"
            "5:"  // Row tail: Row loop
            "add x24, %x[b_ptr], #0x8\n"
            "mov x23, %x[nc]\n"
            "add x22, %x[res_ptr], %x[res_stride], LSL #2\n"
            "6:"  // Row tail: Column loop
            "movi v15.16b, #0x0\n"
            "movi v19.16b, #0x0\n"
            "add x25, %x[a_ptr], #0x8\n"
            "mov x21, %x[nb]\n"
            "movi v18.16b, #0x0\n"
            "movi v14.16b, #0x0\n"
            "7:"  // Row tail: Block loop
            "ldr q7, [x24, #0x0]\n"
            "ldr q5, [x25, #0x0]\n"
            "movi v9.16b, #0x4\n"
            "movi v4.4s, #0x0\n"
            "ldr q3, [x24, #0x10]\n"
            "ldr q2, [x25, #0x10]\n"
            "movi v1.4s, #0x0\n"
            "movi v0.4s, #0x0\n"
            "ldr q13, [x24, #0x20]\n"
            "ldr q31, [x25, #0x20]\n"
            "movi v30.4s, #0x0\n"
            "movi v29.16b, #0xf0\n"
            "ldr q28, [x24, #0x30]\n"
            "ldr q27, [x25, #0x30]\n"
            "sshl v20.16b, v7.16b, v9.16b\n"
            "sub x20, x24, #0x8\n"
            "ldr q26, [x25, #0x40]\n"
            "ldr q25, [x25, #0x50]\n"
            "sshl v17.16b, v3.16b, v9.16b\n"
            "and v7.16b, v7.16b, v29.16b\n"
            "ldr q24, [x25, #0x60]\n"
            "ldr q16, [x25, #0x70]\n"
            "sshl v22.16b, v13.16b, v9.16b\n"
            "and v3.16b, v3.16b, v29.16b\n"
            "ldr d21, [x20, #0x0]\n"
            "ldr d12, [x25, #-0x8]\n"
            ".inst 0x4f85e284  // sdot v4.4s, v20.16b, v5.4b[0]\n"
            ".inst 0x4fa5e281  // sdot v1.4s, v20.16b, v5.4b[1]\n"
            ".inst 0x4f85ea80  // sdot v0.4s, v20.16b, v5.4b[2]\n"
            ".inst 0x4fa5ea9e  // sdot v30.4s, v20.16b, v5.4b[3]\n"
            "sshl v9.16b, v28.16b, v9.16b\n"
            "subs x21, x21, #0x1\n"
            "and v13.16b, v13.16b, v29.16b\n"
            "and v28.16b, v28.16b, v29.16b\n"
            "add x25, x25, #0x88\n"
            "add x24, x24, #0x48\n"
            "fcvtl v21.4s, v21.4h\n"
            "fcvtl v12.4s, v12.4h\n"
            ".inst 0x4f82e224  // sdot v4.4s, v17.16b, v2.4b[0]\n"
            ".inst 0x4fa2e221  // sdot v1.4s, v17.16b, v2.4b[1]\n"
            ".inst 0x4f82ea20  // sdot v0.4s, v17.16b, v2.4b[2]\n"
            ".inst 0x4fa2ea3e  // sdot v30.4s, v17.16b, v2.4b[3]\n"
            "fmul v11.4s, v21.4s, v12.s[0]\n"
            "fmul v23.4s, v21.4s, v12.s[1]\n"
            "fmul v17.4s, v21.4s, v12.s[2]\n"
            ".inst 0x4f9fe2c4  // sdot v4.4s, v22.16b, v31.4b[0]\n"
            "fmul v6.4s, v21.4s, v12.s[3]\n"
            ".inst 0x4fbfe2c1  // sdot v1.4s, v22.16b, v31.4b[1]\n"
            ".inst 0x4f9feac0  // sdot v0.4s, v22.16b, v31.4b[2]\n"
            ".inst 0x4fbfeade  // sdot v30.4s, v22.16b, v31.4b[3]\n"
            ".inst 0x4f9be124  // sdot v4.4s, v9.16b, v27.4b[0]\n"
            ".inst 0x4fbbe121  // sdot v1.4s, v9.16b, v27.4b[1]\n"
            ".inst 0x4f9be920  // sdot v0.4s, v9.16b, v27.4b[2]\n"
            ".inst 0x4fbbe93e  // sdot v30.4s, v9.16b, v27.4b[3]\n"
            ".inst 0x4f9ae0e4  // sdot v4.4s, v7.16b, v26.4b[0]\n"
            ".inst 0x4fbae0e1  // sdot v1.4s, v7.16b, v26.4b[1]\n"
            ".inst 0x4f9ae8e0  // sdot v0.4s, v7.16b, v26.4b[2]\n"
            ".inst 0x4fbae8fe  // sdot v30.4s, v7.16b, v26.4b[3]\n"
            ".inst 0x4f99e064  // sdot v4.4s, v3.16b, v25.4b[0]\n"
            ".inst 0x4fb9e061  // sdot v1.4s, v3.16b, v25.4b[1]\n"
            ".inst 0x4f99e860  // sdot v0.4s, v3.16b, v25.4b[2]\n"
            ".inst 0x4fb9e87e  // sdot v30.4s, v3.16b, v25.4b[3]\n"
            ".inst 0x4f98e1a4  // sdot v4.4s, v13.16b, v24.4b[0]\n"
            ".inst 0x4fb8e1a1  // sdot v1.4s, v13.16b, v24.4b[1]\n"
            ".inst 0x4f98e9a0  // sdot v0.4s, v13.16b, v24.4b[2]\n"
            ".inst 0x4fb8e9be  // sdot v30.4s, v13.16b, v24.4b[3]\n"
            ".inst 0x4f90e384  // sdot v4.4s, v28.16b, v16.4b[0]\n"
            ".inst 0x4fb0e381  // sdot v1.4s, v28.16b, v16.4b[1]\n"
            ".inst 0x4f90eb80  // sdot v0.4s, v28.16b, v16.4b[2]\n"
            ".inst 0x4fb0eb9e  // sdot v30.4s, v28.16b, v16.4b[3]\n"
            "scvtf v4.4s, v4.4s, #0x4\n"
            "scvtf v1.4s, v1.4s, #0x4\n"
            "scvtf v0.4s, v0.4s, #0x4\n"
            "fmla v15.4s, v4.4s, v11.4s\n"
            "scvtf v30.4s, v30.4s, #0x4\n"
            "fmla v19.4s, v1.4s, v23.4s\n"
            "fmla v18.4s, v0.4s, v17.4s\n"
            "fmla v14.4s, v30.4s, v6.4s\n"
            "bgt 7b\n"
            "mov x20, %x[res_ptr]\n"
            "cmp x10, #0x1\n"
            "str q15, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "cmp x10, #0x2\n"
            "str q19, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "cmp x10, #0x3\n"
            "str q18, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "str q14, [x20, #0x0]\n"
            "8:"  // Row tail: Accumulator store skip
            "subs x23, x23, #0x4\n"
            "add %x[res_ptr], %x[res_ptr], #0x10\n"
            "bne 6b\n"
            "subs x10, x10, #0x4\n"
            "add %x[a_ptr], %x[a_ptr], x9\n"
            "mov %x[res_ptr], x22\n"
            "bgt 5b\n"
            "9:"  // Row tail: Row loop skip
            : [a_ptr] "+&r" (a_ptr), [res_ptr] "+&r" (res_ptr)
            : [b_ptr] "r" (b_ptr), [nr] "r" (nr), [nb] "r" (nb), [res_stride] "r" (res_stride), [nc] "r" (nc)
            : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "x9", "x10", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28"
        );
        return;
    }
#endif // #if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON)
    {
        float sumf[4][4];
        int sumi;

        for (int y = 0; y < nr / 4; y++) {
            const block_q8_0x4 * a_ptr = (const block_q8_0x4 *) vy + (y * nb);
            for (int x = 0; x < nc / ncols_interleaved; x++) {
                const block_q4_0x4 * b_ptr = (const block_q4_0x4 *) vx + (x * nb);
                for (int m = 0; m < 4; m++) {
                    for (int j = 0; j < ncols_interleaved; j++) sumf[m][j] = 0.0;
                }
                for (int l = 0; l < nb; l++) {
                    for (int k = 0; k < (qk / (2 * blocklen)); k++) {
                        for (int m = 0; m < 4; m++) {
                            for (int j = 0; j < ncols_interleaved; j++) {
                                sumi = 0;
                                for (int i = 0; i < blocklen; ++i) {
                                    const int v0 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] << 4);
                                    const int v1 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] & 0xF0);
                                    sumi += ((v0 * a_ptr[l].qs[k * 4 * blocklen + m * blocklen + i]) +
                                            (v1 * a_ptr[l].qs[k * 4 * blocklen + m * blocklen + i + qk / 2 * 4])) >> 4;
                                }
                                sumf[m][j] += sumi * GGML_FP16_TO_FP32(b_ptr[l].d[j]) * GGML_FP16_TO_FP32(a_ptr[l].d[m]);
                            }
                        }
                    }
                }
                for (int m = 0; m < 4; m++) {
                    for (int j = 0; j < ncols_interleaved; j++)
                        s[(y * 4 + m) * bs + x * ncols_interleaved + j] = sumf[m][j];
                }
            }
        }
    }
}

void ggml_gemm_q4_0_4x8_q8_0(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy, int nr, int nc) {
    const int qk = QK8_0;
    const int nb = n / qk;
    const int ncols_interleaved = 4;
    const int blocklen = 8;

    assert (n % qk == 0);
    assert (nr % 4 == 0);
    assert (nc % ncols_interleaved == 0);

    UNUSED(s);
    UNUSED(bs);
    UNUSED(vx);
    UNUSED(vy);
    UNUSED(nr);
    UNUSED(nc);
    UNUSED(nb);
    UNUSED(ncols_interleaved);
    UNUSED(blocklen);

#if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON) && defined(__ARM_FEATURE_MATMUL_INT8)
    if (ggml_cpu_has_neon() && ggml_cpu_has_matmul_int8()) {
        const void * b_ptr = vx;
        const void * a_ptr = vy;
        float * res_ptr = s;
        size_t res_stride = bs * sizeof(float);

        __asm__ __volatile__(
            "mov x10, %x[nr]\n"
            "mov x9, #0x88\n"
            "cmp x10, #0x10\n"
            "mul x9, %x[nb], x9\n"
            "blt 4f\n"
            "1:"  // Row loop
            "add x28, %x[b_ptr], #0x8\n"
            "mov x27, %x[nc]\n"
            "add x26, %x[res_ptr], %x[res_stride], LSL #4\n"
            "2:"  // Column loop
            "add x25, %x[a_ptr], #0x8\n"
            "movi v2.16b, #0x0\n"
            "movi v10.16b, #0x0\n"
            "mov x24, %x[nb]\n"
            "add x23, x25, x9\n"
            "movi v12.16b, #0x0\n"
            "movi v28.16b, #0x0\n"
            "add x22, x23, x9\n"
            "movi v11.16b, #0x0\n"
            "movi v13.16b, #0x0\n"
            "add x21, x22, x9\n"
            "movi v22.16b, #0x0\n"
            "movi v23.16b, #0x0\n"
            "movi v25.16b, #0x0\n"
            "movi v5.16b, #0x0\n"
            "movi v7.16b, #0x0\n"
            "movi v4.16b, #0x0\n"
            "movi v6.16b, #0x0\n"
            "movi v30.16b, #0x0\n"
            "movi v24.16b, #0x0\n"
            "movi v14.16b, #0x0\n"
            "3:"  // Block loop
            "ldr q21, [x28, #0x0]\n"
            "ldr q16, [x28, #0x10]\n"
            "movi v1.16b, #0x4\n"
            "movi v19.4s, #0x0\n"
            "ldr q27, [x25, #0x0]\n"
            "ldr q15, [x25, #0x10]\n"
            "movi v26.4s, #0x0\n"
            "movi v18.4s, #0x0\n"
            "ldr q29, [x28, #0x20]\n"
            "ldr q3, [x28, #0x30]\n"
            "movi v17.4s, #0x0\n"
            "movi v0.16b, #0xf0\n"
            "ldr d20, [x25, #-0x8]\n"
            "ldr d9, [x23, #-0x8]\n"
            "sshl v8.16b, v21.16b, v1.16b\n"
            "sshl v31.16b, v16.16b, v1.16b\n"
            "and v21.16b, v21.16b, v0.16b\n"
            "and v16.16b, v16.16b, v0.16b\n"
            "sub x20, x28, #0x8\n"
            "subs x24, x24, #0x1\n"
            "add x28, x28, #0x48\n"
            ".inst 0x4e88a773  // smmla v19.4s, v27.16b, v8.16b\n"
            ".inst 0x4e9fa77a  // smmla v26.4s, v27.16b, v31.16b\n"
            "ldr q27, [x25, #0x20]\n"
            ".inst 0x4e88a5f2  // smmla v18.4s, v15.16b, v8.16b\n"
            ".inst 0x4e9fa5f1  // smmla v17.4s, v15.16b, v31.16b\n"
            "sshl v15.16b, v29.16b, v1.16b\n"
            "sshl v1.16b, v3.16b, v1.16b\n"
            "and v29.16b, v29.16b, v0.16b\n"
            "and v3.16b, v3.16b, v0.16b\n"
            "ldr q0, [x25, #0x30]\n"
            "fcvtl v20.4s, v20.4h\n"
            ".inst 0x4e8fa773  // smmla v19.4s, v27.16b, v15.16b\n"
            "fcvtl v9.4s, v9.4h\n"
            ".inst 0x4e81a77a  // smmla v26.4s, v27.16b, v1.16b\n"
            "ldr q27, [x25, #0x40]\n"
            ".inst 0x4e8fa412  // smmla v18.4s, v0.16b, v15.16b\n"
            ".inst 0x4e81a411  // smmla v17.4s, v0.16b, v1.16b\n"
            "ldr q0, [x25, #0x50]\n"
            ".inst 0x4e95a773  // smmla v19.4s, v27.16b, v21.16b\n"
            ".inst 0x4e90a77a  // smmla v26.4s, v27.16b, v16.16b\n"
            "ldr q27, [x25, #0x60]\n"
            ".inst 0x4e95a412  // smmla v18.4s, v0.16b, v21.16b\n"
            ".inst 0x4e90a411  // smmla v17.4s, v0.16b, v16.16b\n"
            "ldr q0, [x25, #0x70]\n"
            "add x25, x25, #0x88\n"
            ".inst 0x4e9da773  // smmla v19.4s, v27.16b, v29.16b\n"
            ".inst 0x4e83a77a  // smmla v26.4s, v27.16b, v3.16b\n"
            "ldr d27, [x20, #0x0]\n"
            ".inst 0x4e9da412  // smmla v18.4s, v0.16b, v29.16b\n"
            ".inst 0x4e83a411  // smmla v17.4s, v0.16b, v3.16b\n"
            "fcvtl v27.4s, v27.4h\n"
            "uzp1 v0.2d, v19.2d, v26.2d\n"
            "uzp2 v26.2d, v19.2d, v26.2d\n"
            "fmul v19.4s, v27.4s, v20.s[0]\n"
            "scvtf v0.4s, v0.4s, #0x4\n"
            "scvtf v26.4s, v26.4s, #0x4\n"
            "fmla v2.4s, v0.4s, v19.4s\n"
            "ldr q19, [x23, #0x0]\n"
            "uzp1 v0.2d, v18.2d, v17.2d\n"
            "uzp2 v18.2d, v18.2d, v17.2d\n"
            "fmul v17.4s, v27.4s, v20.s[1]\n"
            "scvtf v0.4s, v0.4s, #0x4\n"
            "scvtf v18.4s, v18.4s, #0x4\n"
            "fmla v10.4s, v26.4s, v17.4s\n"
            "ldr q17, [x23, #0x10]\n"
            "fmul v26.4s, v27.4s, v20.s[2]\n"
            "fmul v20.4s, v27.4s, v20.s[3]\n"
            "fmla v12.4s, v0.4s, v26.4s\n"
            "ldr d0, [x22, #-0x8]\n"
            "ldr d26, [x21, #-0x8]\n"
            "fcvtl v0.4s, v0.4h\n"
            "fmla v28.4s, v18.4s, v20.4s\n"
            "movi v20.4s, #0x0\n"
            "movi v18.4s, #0x0\n"
            ".inst 0x4e88a674  // smmla v20.4s, v19.16b, v8.16b\n"
            ".inst 0x4e9fa672  // smmla v18.4s, v19.16b, v31.16b\n"
            "ldr q19, [x23, #0x20]\n"
            "fcvtl v26.4s, v26.4h\n"
            ".inst 0x4e8fa674  // smmla v20.4s, v19.16b, v15.16b\n"
            ".inst 0x4e81a672  // smmla v18.4s, v19.16b, v1.16b\n"
            "ldr q19, [x23, #0x40]\n"
            ".inst 0x4e95a674  // smmla v20.4s, v19.16b, v21.16b\n"
            ".inst 0x4e90a672  // smmla v18.4s, v19.16b, v16.16b\n"
            "ldr q19, [x23, #0x60]\n"
            ".inst 0x4e9da674  // smmla v20.4s, v19.16b, v29.16b\n"
            ".inst 0x4e83a672  // smmla v18.4s, v19.16b, v3.16b\n"
            "uzp1 v19.2d, v20.2d, v18.2d\n"
            "scvtf v19.4s, v19.4s, #0x4\n"
            "uzp2 v20.2d, v20.2d, v18.2d\n"
            "fmul v18.4s, v27.4s, v9.s[0]\n"
            "scvtf v20.4s, v20.4s, #0x4\n"
            "fmla v11.4s, v19.4s, v18.4s\n"
            "ldr q18, [x22, #0x0]\n"
            "fmul v19.4s, v27.4s, v9.s[1]\n"
            "fmla v13.4s, v20.4s, v19.4s\n"
            "movi v19.4s, #0x0\n"
            "movi v20.4s, #0x0\n"
            ".inst 0x4e88a633  // smmla v19.4s, v17.16b, v8.16b\n"
            ".inst 0x4e9fa634  // smmla v20.4s, v17.16b, v31.16b\n"
            "ldr q17, [x23, #0x30]\n"
            ".inst 0x4e8fa633  // smmla v19.4s, v17.16b, v15.16b\n"
            ".inst 0x4e81a634  // smmla v20.4s, v17.16b, v1.16b\n"
            "ldr q17, [x23, #0x50]\n"
            ".inst 0x4e95a633  // smmla v19.4s, v17.16b, v21.16b\n"
            ".inst 0x4e90a634  // smmla v20.4s, v17.16b, v16.16b\n"
            "ldr q17, [x23, #0x70]\n"
            "add x23, x23, #0x88\n"
            ".inst 0x4e9da633  // smmla v19.4s, v17.16b, v29.16b\n"
            ".inst 0x4e83a634  // smmla v20.4s, v17.16b, v3.16b\n"
            "uzp1 v17.2d, v19.2d, v20.2d\n"
            "scvtf v17.4s, v17.4s, #0x4\n"
            "uzp2 v20.2d, v19.2d, v20.2d\n"
            "fmul v19.4s, v27.4s, v9.s[2]\n"
            "fmul v9.4s, v27.4s, v9.s[3]\n"
            "scvtf v20.4s, v20.4s, #0x4\n"
            "fmla v22.4s, v17.4s, v19.4s\n"
            "ldr q17, [x22, #0x10]\n"
            "movi v19.4s, #0x0\n"
            ".inst 0x4e88a653  // smmla v19.4s, v18.16b, v8.16b\n"
            "fmla v23.4s, v20.4s, v9.4s\n"
            "movi v20.4s, #0x0\n"
            "movi v9.4s, #0x0\n"
            ".inst 0x4e9fa654  // smmla v20.4s, v18.16b, v31.16b\n"
            "ldr q18, [x22, #0x20]\n"
            ".inst 0x4e88a629  // smmla v9.4s, v17.16b, v8.16b\n"
            ".inst 0x4e8fa653  // smmla v19.4s, v18.16b, v15.16b\n"
            ".inst 0x4e81a654  // smmla v20.4s, v18.16b, v1.16b\n"
            "ldr q18, [x22, #0x40]\n"
            ".inst 0x4e95a653  // smmla v19.4s, v18.16b, v21.16b\n"
            ".inst 0x4e90a654  // smmla v20.4s, v18.16b, v16.16b\n"
            "ldr q18, [x22, #0x60]\n"
            ".inst 0x4e9da653  // smmla v19.4s, v18.16b, v29.16b\n"
            ".inst 0x4e83a654  // smmla v20.4s, v18.16b, v3.16b\n"
            "movi v18.4s, #0x0\n"
            ".inst 0x4e9fa632  // smmla v18.4s, v17.16b, v31.16b\n"
            "ldr q17, [x22, #0x30]\n"
            ".inst 0x4e8fa629  // smmla v9.4s, v17.16b, v15.16b\n"
            ".inst 0x4e81a632  // smmla v18.4s, v17.16b, v1.16b\n"
            "ldr q17, [x22, #0x50]\n"
            ".inst 0x4e95a629  // smmla v9.4s, v17.16b, v21.16b\n"
            ".inst 0x4e90a632  // smmla v18.4s, v17.16b, v16.16b\n"
            "ldr q17, [x22, #0x70]\n"
            "add x22, x22, #0x88\n"
            ".inst 0x4e9da629  // smmla v9.4s, v17.16b, v29.16b\n"
            ".inst 0x4e83a632  // smmla v18.4s, v17.16b, v3.16b\n"
            "uzp1 v17.2d, v19.2d, v20.2d\n"
            "uzp2 v20.2d, v19.2d, v20.2d\n"
            "fmul v19.4s, v27.4s, v0.s[0]\n"
            "scvtf v17.4s, v17.4s, #0x4\n"
            "scvtf v20.4s, v20.4s, #0x4\n"
            "fmla v25.4s, v17.4s, v19.4s\n"
            "ldr q19, [x21, #0x0]\n"
            "fmul v17.4s, v27.4s, v0.s[1]\n"
            "fmla v5.4s, v20.4s, v17.4s\n"
            "ldr q17, [x21, #0x10]\n"
            "uzp1 v20.2d, v9.2d, v18.2d\n"
            "uzp2 v9.2d, v9.2d, v18.2d\n"
            "fmul v18.4s, v27.4s, v0.s[2]\n"
            "fmul v0.4s, v27.4s, v0.s[3]\n"
            "scvtf v20.4s, v20.4s, #0x4\n"
            "scvtf v9.4s, v9.4s, #0x4\n"
            "fmla v7.4s, v20.4s, v18.4s\n"
            "movi v20.4s, #0x0\n"
            "movi v18.4s, #0x0\n"
            ".inst 0x4e88a674  // smmla v20.4s, v19.16b, v8.16b\n"
            ".inst 0x4e9fa672  // smmla v18.4s, v19.16b, v31.16b\n"
            "ldr q19, [x21, #0x20]\n"
            "fmla v4.4s, v9.4s, v0.4s\n"
            "movi v9.4s, #0x0\n"
            "movi v0.4s, #0x0\n"
            ".inst 0x4e88a629  // smmla v9.4s, v17.16b, v8.16b\n"
            "fmul v8.4s, v27.4s, v26.s[0]\n"
            ".inst 0x4e9fa620  // smmla v0.4s, v17.16b, v31.16b\n"
            "ldr q17, [x21, #0x30]\n"
            ".inst 0x4e8fa674  // smmla v20.4s, v19.16b, v15.16b\n"
            "fmul v31.4s, v27.4s, v26.s[1]\n"
            ".inst 0x4e81a672  // smmla v18.4s, v19.16b, v1.16b\n"
            "ldr q19, [x21, #0x40]\n"
            ".inst 0x4e8fa629  // smmla v9.4s, v17.16b, v15.16b\n"
            "fmul v15.4s, v27.4s, v26.s[2]\n"
            "fmul v27.4s, v27.4s, v26.s[3]\n"
            ".inst 0x4e81a620  // smmla v0.4s, v17.16b, v1.16b\n"
            "ldr q1, [x21, #0x50]\n"
            ".inst 0x4e95a674  // smmla v20.4s, v19.16b, v21.16b\n"
            ".inst 0x4e90a672  // smmla v18.4s, v19.16b, v16.16b\n"
            "ldr q26, [x21, #0x60]\n"
            ".inst 0x4e95a429  // smmla v9.4s, v1.16b, v21.16b\n"
            ".inst 0x4e90a420  // smmla v0.4s, v1.16b, v16.16b\n"
            "ldr q21, [x21, #0x70]\n"
            "add x21, x21, #0x88\n"
            ".inst 0x4e9da754  // smmla v20.4s, v26.16b, v29.16b\n"
            ".inst 0x4e83a752  // smmla v18.4s, v26.16b, v3.16b\n"
            ".inst 0x4e9da6a9  // smmla v9.4s, v21.16b, v29.16b\n"
            ".inst 0x4e83a6a0  // smmla v0.4s, v21.16b, v3.16b\n"
            "uzp1 v29.2d, v20.2d, v18.2d\n"
            "uzp2 v21.2d, v20.2d, v18.2d\n"
            "scvtf v29.4s, v29.4s, #0x4\n"
            "uzp1 v18.2d, v9.2d, v0.2d\n"
            "uzp2 v16.2d, v9.2d, v0.2d\n"
            "scvtf v21.4s, v21.4s, #0x4\n"
            "fmla v6.4s, v29.4s, v8.4s\n"
            "scvtf v18.4s, v18.4s, #0x4\n"
            "scvtf v16.4s, v16.4s, #0x4\n"
            "fmla v30.4s, v21.4s, v31.4s\n"
            "fmla v24.4s, v18.4s, v15.4s\n"
            "fmla v14.4s, v16.4s, v27.4s\n"
            "bgt 3b\n"
            "mov x20, %x[res_ptr]\n"
            "subs x27, x27, #0x4\n"
            "add %x[res_ptr], %x[res_ptr], #0x10\n"
            "str q2, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q10, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q12, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q28, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q11, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q13, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q22, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q23, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q25, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q5, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q7, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q4, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q6, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q30, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q24, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "str q14, [x20, #0x0]\n"
            "bne 2b\n"
            "mov x20, #0x4\n"
            "sub x10, x10, #0x10\n"
            "cmp x10, #0x10\n"
            "mov %x[res_ptr], x26\n"
            "madd %x[a_ptr], x20, x9, %x[a_ptr]\n"
            "bge 1b\n"
            "4:"  // Row loop skip
            "cbz x10, 9f\n"
            "5:"  // Row tail: Row loop
            "add x24, %x[b_ptr], #0x8\n"
            "mov x23, %x[nc]\n"
            "add x22, %x[res_ptr], %x[res_stride], LSL #2\n"
            "6:"  // Row tail: Column loop
            "movi v2.16b, #0x0\n"
            "movi v10.16b, #0x0\n"
            "add x25, %x[a_ptr], #0x8\n"
            "mov x21, %x[nb]\n"
            "movi v12.16b, #0x0\n"
            "movi v28.16b, #0x0\n"
            "7:"  // Row tail: Block loop
            "ldr q6, [x24, #0x0]\n"
            "ldr q5, [x24, #0x10]\n"
            "movi v17.16b, #0x4\n"
            "movi v8.4s, #0x0\n"
            "ldr q4, [x25, #0x0]\n"
            "ldr q13, [x25, #0x10]\n"
            "movi v27.4s, #0x0\n"
            "movi v0.4s, #0x0\n"
            "ldr q31, [x24, #0x20]\n"
            "ldr q14, [x24, #0x30]\n"
            "movi v29.4s, #0x0\n"
            "movi v22.16b, #0xf0\n"
            "ldr q11, [x25, #0x20]\n"
            "ldr q23, [x25, #0x30]\n"
            "sshl v21.16b, v6.16b, v17.16b\n"
            "sshl v16.16b, v5.16b, v17.16b\n"
            "ldr q20, [x25, #0x40]\n"
            "ldr q26, [x25, #0x50]\n"
            "and v6.16b, v6.16b, v22.16b\n"
            "and v5.16b, v5.16b, v22.16b\n"
            "ldr q25, [x25, #0x60]\n"
            "ldr q3, [x25, #0x70]\n"
            "sshl v19.16b, v31.16b, v17.16b\n"
            "sshl v18.16b, v14.16b, v17.16b\n"
            "ldr d17, [x25, #-0x8]\n"
            ".inst 0x4e95a488  // smmla v8.4s, v4.16b, v21.16b\n"
            ".inst 0x4e90a49b  // smmla v27.4s, v4.16b, v16.16b\n"
            "and v31.16b, v31.16b, v22.16b\n"
            ".inst 0x4e95a5a0  // smmla v0.4s, v13.16b, v21.16b\n"
            ".inst 0x4e90a5bd  // smmla v29.4s, v13.16b, v16.16b\n"
            "and v14.16b, v14.16b, v22.16b\n"
            "sub x20, x24, #0x8\n"
            "ldr d16, [x20, #0x0]\n"
            "subs x21, x21, #0x1\n"
            "add x25, x25, #0x88\n"
            "fcvtl v17.4s, v17.4h\n"
            "add x24, x24, #0x48\n"
            ".inst 0x4e93a568  // smmla v8.4s, v11.16b, v19.16b\n"
            ".inst 0x4e92a57b  // smmla v27.4s, v11.16b, v18.16b\n"
            ".inst 0x4e93a6e0  // smmla v0.4s, v23.16b, v19.16b\n"
            ".inst 0x4e92a6fd  // smmla v29.4s, v23.16b, v18.16b\n"
            "fcvtl v16.4s, v16.4h\n"
            ".inst 0x4e86a688  // smmla v8.4s, v20.16b, v6.16b\n"
            ".inst 0x4e85a69b  // smmla v27.4s, v20.16b, v5.16b\n"
            "fmul v23.4s, v16.4s, v17.s[0]\n"
            "fmul v21.4s, v16.4s, v17.s[1]\n"
            "fmul v1.4s, v16.4s, v17.s[2]\n"
            "fmul v20.4s, v16.4s, v17.s[3]\n"
            ".inst 0x4e86a740  // smmla v0.4s, v26.16b, v6.16b\n"
            ".inst 0x4e85a75d  // smmla v29.4s, v26.16b, v5.16b\n"
            ".inst 0x4e9fa728  // smmla v8.4s, v25.16b, v31.16b\n"
            ".inst 0x4e8ea73b  // smmla v27.4s, v25.16b, v14.16b\n"
            ".inst 0x4e9fa460  // smmla v0.4s, v3.16b, v31.16b\n"
            ".inst 0x4e8ea47d  // smmla v29.4s, v3.16b, v14.16b\n"
            "uzp1 v19.2d, v8.2d, v27.2d\n"
            "uzp2 v18.2d, v8.2d, v27.2d\n"
            "scvtf v19.4s, v19.4s, #0x4\n"
            "uzp1 v17.2d, v0.2d, v29.2d\n"
            "uzp2 v16.2d, v0.2d, v29.2d\n"
            "scvtf v18.4s, v18.4s, #0x4\n"
            "fmla v2.4s, v19.4s, v23.4s\n"
            "scvtf v17.4s, v17.4s, #0x4\n"
            "scvtf v16.4s, v16.4s, #0x4\n"
            "fmla v10.4s, v18.4s, v21.4s\n"
            "fmla v12.4s, v17.4s, v1.4s\n"
            "fmla v28.4s, v16.4s, v20.4s\n"
            "bgt 7b\n"
            "mov x20, %x[res_ptr]\n"
            "cmp x10, #0x1\n"
            "str q2, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "cmp x10, #0x2\n"
            "str q10, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "cmp x10, #0x3\n"
            "str q12, [x20, #0x0]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "str q28, [x20, #0x0]\n"
            "8:"  // Row tail: Accumulator store skip
            "subs x23, x23, #0x4\n"
            "add %x[res_ptr], %x[res_ptr], #0x10\n"
            "bne 6b\n"
            "subs x10, x10, #0x4\n"
            "add %x[a_ptr], %x[a_ptr], x9\n"
            "mov %x[res_ptr], x22\n"
            "bgt 5b\n"
            "9:"  // Row tail: Row loop skip
            : [a_ptr] "+&r" (a_ptr), [res_ptr] "+&r" (res_ptr)
            : [b_ptr] "r" (b_ptr), [nr] "r" (nr), [nb] "r" (nb), [res_stride] "r" (res_stride), [nc] "r" (nc)
            : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "x9", "x10", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28"
        );
        return;
    }
#endif // #if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__) && defined(__ARM_NEON) && defined(__ARM_FEATURE_MATMUL_INT8)
    float sumf[4][4];
    int sumi;

    for (int y = 0; y < nr / 4; y++) {
        const block_q8_0x4 * a_ptr = (const block_q8_0x4 *) vy + (y * nb);
        for (int x = 0; x < nc / ncols_interleaved; x++) {
            const block_q4_0x4 * b_ptr = (const block_q4_0x4 *) vx + (x * nb);
            for (int m = 0; m < 4; m++) {
                for (int j = 0; j < ncols_interleaved; j++) sumf[m][j] = 0.0;
            }
            for (int l = 0; l < nb; l++) {
                for (int k = 0; k < (qk / (2 * blocklen)); k++) {
                    for (int m = 0; m < 4; m++) {
                        for (int j = 0; j < ncols_interleaved; j++) {
                            sumi = 0;
                            for (int i = 0; i < blocklen; ++i) {
                                const int v0 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] << 4);
                                const int v1 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] & 0xF0);
                                sumi += ((v0 * a_ptr[l].qs[k * 4 * blocklen + m * blocklen + i]) +
                                        (v1 * a_ptr[l].qs[k * 4 * blocklen + m * blocklen + i + qk / 2 * 4])) >> 4;
                            }
                            sumf[m][j] += sumi * GGML_FP16_TO_FP32(b_ptr[l].d[j]) * GGML_FP16_TO_FP32(a_ptr[l].d[m]);
                        }
                    }
                }
            }
            for (int m = 0; m < 4; m++) {
                for (int j = 0; j < ncols_interleaved; j++)
                    s[(y * 4 + m) * bs + x * ncols_interleaved + j] = sumf[m][j];
            }
        }
    }
}

void ggml_gemm_q4_0_8x8_q8_0(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy, int nr, int nc) {
    const int qk = QK8_0;
    const int nb = n / qk;
    const int ncols_interleaved = 8;
    const int blocklen = 8;

    assert (n % qk == 0);
    assert (nr % 4 == 0);
    assert (nc % ncols_interleaved == 0);

    UNUSED(s);
    UNUSED(bs);
    UNUSED(vx);
    UNUSED(vy);
    UNUSED(nr);
    UNUSED(nc);
    UNUSED(nb);
    UNUSED(ncols_interleaved);
    UNUSED(blocklen);

#if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__)
#if defined(__ARM_FEATURE_SVE) && defined(__ARM_FEATURE_MATMUL_INT8)
    if (ggml_cpu_has_sve() && ggml_cpu_has_matmul_int8() && ggml_cpu_get_sve_cnt() == QK8_0) {
        const void * b_ptr = vx;
        const void * a_ptr = vy;
        float * res_ptr = s;
        size_t res_stride = bs * sizeof(float);

        __asm__ __volatile__(
            "mov x20, #0x4\n"
            "mov x13, %x[nr]\n"
            "mov z28.s, #-0x4\n"
            "mov x12, #0x88\n"
            "ptrue p1.b\n"
            "whilelt p0.s, XZR, x20\n"
            "cmp x13, #0x10\n"
            "mul x12, %x[nb], x12\n"
            "blt 4f\n"
            "1:"  // Row loop
            "add x11, %x[b_ptr], #0x10\n"
            "mov x10, %x[nc]\n"
            "add x9, %x[res_ptr], %x[res_stride], LSL #4\n"
            "2:"  // Column loop
            "add x28, %x[a_ptr], #0x8\n"
            "mov z24.b, #0x0\n"
            "mov z15.b, #0x0\n"
            "mov x27, %x[nb]\n"
            "add x26, x28, x12\n"
            "mov z12.b, #0x0\n"
            "mov z0.b, #0x0\n"
            "add x25, x26, x12\n"
            "mov z13.b, #0x0\n"
            "mov z1.b, #0x0\n"
            "add x24, x25, x12\n"
            "mov z20.b, #0x0\n"
            "mov z25.b, #0x0\n"
            "mov z11.b, #0x0\n"
            "mov z16.b, #0x0\n"
            "mov z19.b, #0x0\n"
            "mov z26.b, #0x0\n"
            "mov z8.b, #0x0\n"
            "mov z29.b, #0x0\n"
            "mov z27.b, #0x0\n"
            "mov z10.b, #0x0\n"
            "3:"  // Block loop
            "ld1b { z30.b }, p1/Z, [x11]\n"
            "ld1b { z21.b }, p1/Z, [x11, #1, MUL VL]\n"
            "mov z18.s, #0x0\n"
            "mov z7.s, #0x0\n"
            "ld1rqb { z3.b }, p1/Z, [x28]\n"
            "ld1rqb { z5.b }, p1/Z, [x28, #16]\n"
            "mov z9.s, #0x0\n"
            "mov z22.s, #0x0\n"
            "ld1b { z4.b }, p1/Z, [x11, #2, MUL VL]\n"
            "ld1b { z17.b }, p1/Z, [x11, #3, MUL VL]\n"
            "sub x20, x11, #0x10\n"
            "sub x23, x28, #0x8\n"
            "lsl z31.b, z30.b, #0x4\n"
            "lsl z6.b, z21.b, #0x4\n"
            "ld1h { z23.s }, p1/Z, [x20]\n"
            "sub x22, x26, #0x8\n"
            "and z30.b, z30.b, #0xf0\n"
            "and z21.b, z21.b, #0xf0\n"
            "sub x21, x25, #0x8\n"
            "sub x20, x24, #0x8\n"
            "lsl z14.b, z4.b, #0x4\n"
            "lsl z2.b, z17.b, #0x4\n"
            "subs x27, x27, #0x1\n"
            "add x11, x11, #0x90\n"
            ".inst 0x451f9872  // smmla z18.s, z3.b, z31.b\n"
            ".inst 0x45069867  // smmla z7.s, z3.b, z6.b\n"
            "ld1rqb { z3.b }, p1/Z, [x28, #32]\n"
            "and z4.b, z4.b, #0xf0\n"
            ".inst 0x451f98a9  // smmla z9.s, z5.b, z31.b\n"
            ".inst 0x450698b6  // smmla z22.s, z5.b, z6.b\n"
            "ld1rqb { z5.b }, p1/Z, [x28, #48]\n"
            "and z17.b, z17.b, #0xf0\n"
            "fcvt z23.s, p1/m, z23.h\n"
            ".inst 0x450e9872  // smmla z18.s, z3.b, z14.b\n"
            ".inst 0x45029867  // smmla z7.s, z3.b, z2.b\n"
            "ld1rqb { z3.b }, p1/Z, [x28, #64]\n"
            ".inst 0x450e98a9  // smmla z9.s, z5.b, z14.b\n"
            ".inst 0x450298b6  // smmla z22.s, z5.b, z2.b\n"
            "ld1rqb { z5.b }, p1/Z, [x28, #80]\n"
            "fscale z23.s, p1/m, z23.s, z28.s\n"
            ".inst 0x451e9872  // smmla z18.s, z3.b, z30.b\n"
            ".inst 0x45159867  // smmla z7.s, z3.b, z21.b\n"
            "ld1rqb { z3.b }, p1/Z, [x28, #96]\n"
            ".inst 0x451e98a9  // smmla z9.s, z5.b, z30.b\n"
            ".inst 0x451598b6  // smmla z22.s, z5.b, z21.b\n"
            "ld1rqb { z5.b }, p1/Z, [x28, #112]\n"
            "add x28, x28, #0x88\n"
            ".inst 0x45049872  // smmla z18.s, z3.b, z4.b\n"
            ".inst 0x45119867  // smmla z7.s, z3.b, z17.b\n"
            "ld1h { z3.s }, p0/Z, [x23]\n"
            ".inst 0x450498a9  // smmla z9.s, z5.b, z4.b\n"
            ".inst 0x451198b6  // smmla z22.s, z5.b, z17.b\n"
            "fcvt z3.s, p1/m, z3.h\n"
            "uzp1 z5.d, z18.d, z7.d\n"
            "uzp2 z18.d, z18.d, z7.d\n"
            "mov z3.q, z3.q[0]\n"
            "uzp1 z7.d, z9.d, z22.d\n"
            "uzp2 z22.d, z9.d, z22.d\n"
            "fmul z9.s, z23.s, z3.s[0]\n"
            "scvtf z5.s, p1/m, z5.s\n"
            "scvtf z18.s, p1/m, z18.s\n"
            "scvtf z7.s, p1/m, z7.s\n"
            "scvtf z22.s, p1/m, z22.s\n"
            "fmla z24.s, p1/M, z5.s, z9.s\n"
            "ld1rqb { z5.b }, p1/Z, [x26]\n"
            "fmul z9.s, z23.s, z3.s[1]\n"
            "fmla z15.s, p1/M, z18.s, z9.s\n"
            "ld1rqb { z18.b }, p1/Z, [x26, #16]\n"
            "fmul z9.s, z23.s, z3.s[2]\n"
            "fmul z3.s, z23.s, z3.s[3]\n"
            "fmla z12.s, p1/M, z7.s, z9.s\n"
            "mov z9.s, #0x0\n"
            "ld1h { z7.s }, p0/Z, [x22]\n"
            ".inst 0x451f98a9  // smmla z9.s, z5.b, z31.b\n"
            "fmla z0.s, p1/M, z22.s, z3.s\n"
            "mov z22.s, #0x0\n"
            "ld1h { z3.s }, p0/Z, [x21]\n"
            ".inst 0x450698b6  // smmla z22.s, z5.b, z6.b\n"
            "ld1rqb { z5.b }, p1/Z, [x26, #32]\n"
            "fcvt z7.s, p1/m, z7.h\n"
            "fcvt z3.s, p1/m, z3.h\n"
            ".inst 0x450e98a9  // smmla z9.s, z5.b, z14.b\n"
            ".inst 0x450298b6  // smmla z22.s, z5.b, z2.b\n"
            "ld1rqb { z5.b }, p1/Z, [x26, #64]\n"
            "mov z7.q, z7.q[0]\n"
            "mov z3.q, z3.q[0]\n"
            ".inst 0x451e98a9  // smmla z9.s, z5.b, z30.b\n"
            ".inst 0x451598b6  // smmla z22.s, z5.b, z21.b\n"
            "ld1rqb { z5.b }, p1/Z, [x26, #96]\n"
            ".inst 0x450498a9  // smmla z9.s, z5.b, z4.b\n"
            ".inst 0x451198b6  // smmla z22.s, z5.b, z17.b\n"
            "uzp1 z5.d, z9.d, z22.d\n"
            "scvtf z5.s, p1/m, z5.s\n"
            "uzp2 z22.d, z9.d, z22.d\n"
            "fmul z9.s, z23.s, z7.s[0]\n"
            "scvtf z22.s, p1/m, z22.s\n"
            "fmla z13.s, p1/M, z5.s, z9.s\n"
            "ld1rqb { z9.b }, p1/Z, [x25]\n"
            "fmul z5.s, z23.s, z7.s[1]\n"
            "fmla z1.s, p1/M, z22.s, z5.s\n"
            "mov z5.s, #0x0\n"
            "mov z22.s, #0x0\n"
            ".inst 0x451f9a45  // smmla z5.s, z18.b, z31.b\n"
            ".inst 0x45069a56  // smmla z22.s, z18.b, z6.b\n"
            "ld1rqb { z18.b }, p1/Z, [x26, #48]\n"
            ".inst 0x450e9a45  // smmla z5.s, z18.b, z14.b\n"
            ".inst 0x45029a56  // smmla z22.s, z18.b, z2.b\n"
            "ld1rqb { z18.b }, p1/Z, [x26, #80]\n"
            ".inst 0x451e9a45  // smmla z5.s, z18.b, z30.b\n"
            ".inst 0x45159a56  // smmla z22.s, z18.b, z21.b\n"
            "ld1rqb { z18.b }, p1/Z, [x26, #112]\n"
            "add x26, x26, #0x88\n"
            ".inst 0x45049a45  // smmla z5.s, z18.b, z4.b\n"
            ".inst 0x45119a56  // smmla z22.s, z18.b, z17.b\n"
            "uzp1 z18.d, z5.d, z22.d\n"
            "scvtf z18.s, p1/m, z18.s\n"
            "uzp2 z22.d, z5.d, z22.d\n"
            "fmul z5.s, z23.s, z7.s[2]\n"
            "fmul z7.s, z23.s, z7.s[3]\n"
            "scvtf z22.s, p1/m, z22.s\n"
            "fmla z20.s, p1/M, z18.s, z5.s\n"
            "ld1rqb { z18.b }, p1/Z, [x25, #16]\n"
            "ld1h { z5.s }, p0/Z, [x20]\n"
            "fcvt z5.s, p1/m, z5.h\n"
            "fmla z25.s, p1/M, z22.s, z7.s\n"
            "mov z22.s, #0x0\n"
            "mov z7.s, #0x0\n"
            ".inst 0x451f9936  // smmla z22.s, z9.b, z31.b\n"
            ".inst 0x45069927  // smmla z7.s, z9.b, z6.b\n"
            "ld1rqb { z9.b }, p1/Z, [x25, #32]\n"
            "mov z5.q, z5.q[0]\n"
            ".inst 0x450e9936  // smmla z22.s, z9.b, z14.b\n"
            ".inst 0x45029927  // smmla z7.s, z9.b, z2.b\n"
            "ld1rqb { z9.b }, p1/Z, [x25, #64]\n"
            ".inst 0x451e9936  // smmla z22.s, z9.b, z30.b\n"
            ".inst 0x45159927  // smmla z7.s, z9.b, z21.b\n"
            "ld1rqb { z9.b }, p1/Z, [x25, #96]\n"
            ".inst 0x45049936  // smmla z22.s, z9.b, z4.b\n"
            ".inst 0x45119927  // smmla z7.s, z9.b, z17.b\n"
            "uzp1 z9.d, z22.d, z7.d\n"
            "scvtf z9.s, p1/m, z9.s\n"
            "uzp2 z22.d, z22.d, z7.d\n"
            "fmul z7.s, z23.s, z3.s[0]\n"
            "scvtf z22.s, p1/m, z22.s\n"
            "fmla z11.s, p1/M, z9.s, z7.s\n"
            "ld1rqb { z9.b }, p1/Z, [x24]\n"
            "fmul z7.s, z23.s, z3.s[1]\n"
            "fmla z16.s, p1/M, z22.s, z7.s\n"
            "mov z22.s, #0x0\n"
            "mov z7.s, #0x0\n"
            ".inst 0x451f9a56  // smmla z22.s, z18.b, z31.b\n"
            ".inst 0x45069a47  // smmla z7.s, z18.b, z6.b\n"
            "ld1rqb { z18.b }, p1/Z, [x25, #48]\n"
            ".inst 0x450e9a56  // smmla z22.s, z18.b, z14.b\n"
            ".inst 0x45029a47  // smmla z7.s, z18.b, z2.b\n"
            "ld1rqb { z18.b }, p1/Z, [x25, #80]\n"
            ".inst 0x451e9a56  // smmla z22.s, z18.b, z30.b\n"
            ".inst 0x45159a47  // smmla z7.s, z18.b, z21.b\n"
            "ld1rqb { z18.b }, p1/Z, [x25, #112]\n"
            "add x25, x25, #0x88\n"
            ".inst 0x45049a56  // smmla z22.s, z18.b, z4.b\n"
            ".inst 0x45119a47  // smmla z7.s, z18.b, z17.b\n"
            "uzp1 z18.d, z22.d, z7.d\n"
            "scvtf z18.s, p1/m, z18.s\n"
            "uzp2 z7.d, z22.d, z7.d\n"
            "fmul z22.s, z23.s, z3.s[2]\n"
            "fmul z3.s, z23.s, z3.s[3]\n"
            "scvtf z7.s, p1/m, z7.s\n"
            "fmla z19.s, p1/M, z18.s, z22.s\n"
            "ld1rqb { z18.b }, p1/Z, [x24, #16]\n"
            "fmul z22.s, z23.s, z5.s[0]\n"
            "fmla z26.s, p1/M, z7.s, z3.s\n"
            "mov z3.s, #0x0\n"
            "mov z7.s, #0x0\n"
            ".inst 0x451f9923  // smmla z3.s, z9.b, z31.b\n"
            ".inst 0x45069927  // smmla z7.s, z9.b, z6.b\n"
            "ld1rqb { z9.b }, p1/Z, [x24, #32]\n"
            ".inst 0x450e9923  // smmla z3.s, z9.b, z14.b\n"
            ".inst 0x45029927  // smmla z7.s, z9.b, z2.b\n"
            "mov z9.s, #0x0\n"
            ".inst 0x451f9a49  // smmla z9.s, z18.b, z31.b\n"
            "mov z31.s, #0x0\n"
            ".inst 0x45069a5f  // smmla z31.s, z18.b, z6.b\n"
            "ld1rqb { z6.b }, p1/Z, [x24, #48]\n"
            "ld1rqb { z18.b }, p1/Z, [x24, #64]\n"
            ".inst 0x450e98c9  // smmla z9.s, z6.b, z14.b\n"
            "fmul z14.s, z23.s, z5.s[1]\n"
            ".inst 0x450298df  // smmla z31.s, z6.b, z2.b\n"
            "ld1rqb { z6.b }, p1/Z, [x24, #80]\n"
            "fmul z2.s, z23.s, z5.s[2]\n"
            "fmul z23.s, z23.s, z5.s[3]\n"
            ".inst 0x451e9a43  // smmla z3.s, z18.b, z30.b\n"
            ".inst 0x45159a47  // smmla z7.s, z18.b, z21.b\n"
            "ld1rqb { z5.b }, p1/Z, [x24, #96]\n"
            ".inst 0x451e98c9  // smmla z9.s, z6.b, z30.b\n"
            ".inst 0x451598df  // smmla z31.s, z6.b, z21.b\n"
            "ld1rqb { z18.b }, p1/Z, [x24, #112]\n"
            "add x24, x24, #0x88\n"
            ".inst 0x450498a3  // smmla z3.s, z5.b, z4.b\n"
            ".inst 0x451198a7  // smmla z7.s, z5.b, z17.b\n"
            ".inst 0x45049a49  // smmla z9.s, z18.b, z4.b\n"
            ".inst 0x45119a5f  // smmla z31.s, z18.b, z17.b\n"
            "uzp1 z18.d, z3.d, z7.d\n"
            "uzp2 z5.d, z3.d, z7.d\n"
            "scvtf z18.s, p1/m, z18.s\n"
            "uzp1 z6.d, z9.d, z31.d\n"
            "uzp2 z9.d, z9.d, z31.d\n"
            "scvtf z5.s, p1/m, z5.s\n"
            "fmla z8.s, p1/M, z18.s, z22.s\n"
            "scvtf z6.s, p1/m, z6.s\n"
            "scvtf z9.s, p1/m, z9.s\n"
            "fmla z29.s, p1/M, z5.s, z14.s\n"
            "fmla z27.s, p1/M, z6.s, z2.s\n"
            "fmla z10.s, p1/M, z9.s, z23.s\n"
            "bgt 3b\n"
            "mov x20, %x[res_ptr]\n"
            "subs x10, x10, #0x8\n"
            "add %x[res_ptr], %x[res_ptr], #0x20\n"
            "st1w { z24.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z15.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z12.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z0.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z13.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z1.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z20.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z25.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z11.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z16.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z19.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z26.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z8.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z29.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z27.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "st1w { z10.s }, p1, [x20]\n"
            "bne 2b\n"
            "mov x20, #0x4\n"
            "sub x13, x13, #0x10\n"
            "cmp x13, #0x10\n"
            "mov %x[res_ptr], x9\n"
            "madd %x[a_ptr], x20, x12, %x[a_ptr]\n"
            "bge 1b\n"
            "4:"  // Row loop skip
            "cbz x13, 9f\n"
            "5:"  // Row tail: Row loop
            "add x25, %x[b_ptr], #0x10\n"
            "mov x24, %x[nc]\n"
            "add x23, %x[res_ptr], %x[res_stride], LSL #2\n"
            "6:"  // Row tail: Column loop
            "mov z24.b, #0x0\n"
            "mov z15.b, #0x0\n"
            "add x28, %x[a_ptr], #0x8\n"
            "mov x22, %x[nb]\n"
            "mov z12.b, #0x0\n"
            "mov z0.b, #0x0\n"
            "7:"  // Row tail: Block loop
            "ld1b { z3.b }, p1/Z, [x25]\n"
            "ld1b { z6.b }, p1/Z, [x25, #1, MUL VL]\n"
            "mov z2.s, #0x0\n"
            "mov z25.s, #0x0\n"
            "ld1rqb { z26.b }, p1/Z, [x28]\n"
            "ld1rqb { z21.b }, p1/Z, [x28, #16]\n"
            "mov z27.s, #0x0\n"
            "mov z19.s, #0x0\n"
            "ld1b { z29.b }, p1/Z, [x25, #2, MUL VL]\n"
            "ld1b { z16.b }, p1/Z, [x25, #3, MUL VL]\n"
            "sub x21, x25, #0x10\n"
            "sub x20, x28, #0x8\n"
            "lsl z20.b, z3.b, #0x4\n"
            "lsl z4.b, z6.b, #0x4\n"
            "ld1rqb { z10.b }, p1/Z, [x28, #32]\n"
            "ld1rqb { z23.b }, p1/Z, [x28, #48]\n"
            "and z3.b, z3.b, #0xf0\n"
            "and z6.b, z6.b, #0xf0\n"
            "ld1rqb { z11.b }, p1/Z, [x28, #64]\n"
            "ld1rqb { z7.b }, p1/Z, [x28, #80]\n"
            "lsl z8.b, z29.b, #0x4\n"
            "lsl z14.b, z16.b, #0x4\n"
            "ld1rqb { z18.b }, p1/Z, [x28, #96]\n"
            "ld1rqb { z30.b }, p1/Z, [x28, #112]\n"
            ".inst 0x45149b42  // smmla z2.s, z26.b, z20.b\n"
            ".inst 0x45049b59  // smmla z25.s, z26.b, z4.b\n"
            "and z29.b, z29.b, #0xf0\n"
            "ld1h { z17.s }, p1/Z, [x21]\n"
            ".inst 0x45149abb  // smmla z27.s, z21.b, z20.b\n"
            ".inst 0x45049ab3  // smmla z19.s, z21.b, z4.b\n"
            "and z16.b, z16.b, #0xf0\n"
            "ld1h { z4.s }, p0/Z, [x20]\n"
            "subs x22, x22, #0x1\n"
            "add x28, x28, #0x88\n"
            "fcvt z17.s, p1/m, z17.h\n"
            "add x25, x25, #0x90\n"
            ".inst 0x45089942  // smmla z2.s, z10.b, z8.b\n"
            ".inst 0x450e9959  // smmla z25.s, z10.b, z14.b\n"
            "fcvt z4.s, p1/m, z4.h\n"
            ".inst 0x45089afb  // smmla z27.s, z23.b, z8.b\n"
            ".inst 0x450e9af3  // smmla z19.s, z23.b, z14.b\n"
            "fscale z17.s, p1/m, z17.s, z28.s\n"
            "mov z4.q, z4.q[0]\n"
            ".inst 0x45039962  // smmla z2.s, z11.b, z3.b\n"
            ".inst 0x45069979  // smmla z25.s, z11.b, z6.b\n"
            "fmul z23.s, z17.s, z4.s[0]\n"
            "fmul z9.s, z17.s, z4.s[1]\n"
            "fmul z21.s, z17.s, z4.s[2]\n"
            "fmul z4.s, z17.s, z4.s[3]\n"
            ".inst 0x450398fb  // smmla z27.s, z7.b, z3.b\n"
            ".inst 0x450698f3  // smmla z19.s, z7.b, z6.b\n"
            ".inst 0x451d9a42  // smmla z2.s, z18.b, z29.b\n"
            ".inst 0x45109a59  // smmla z25.s, z18.b, z16.b\n"
            ".inst 0x451d9bdb  // smmla z27.s, z30.b, z29.b\n"
            ".inst 0x45109bd3  // smmla z19.s, z30.b, z16.b\n"
            "uzp1 z31.d, z2.d, z25.d\n"
            "uzp2 z13.d, z2.d, z25.d\n"
            "scvtf z31.s, p1/m, z31.s\n"
            "uzp1 z17.d, z27.d, z19.d\n"
            "uzp2 z18.d, z27.d, z19.d\n"
            "scvtf z13.s, p1/m, z13.s\n"
            "fmla z24.s, p1/M, z31.s, z23.s\n"
            "scvtf z17.s, p1/m, z17.s\n"
            "scvtf z18.s, p1/m, z18.s\n"
            "fmla z15.s, p1/M, z13.s, z9.s\n"
            "fmla z12.s, p1/M, z17.s, z21.s\n"
            "fmla z0.s, p1/M, z18.s, z4.s\n"
            "bgt 7b\n"
            "mov x20, %x[res_ptr]\n"
            "cmp x13, #0x1\n"
            "st1w { z24.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "cmp x13, #0x2\n"
            "st1w { z15.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "cmp x13, #0x3\n"
            "st1w { z12.s }, p1, [x20]\n"
            "add x20, x20, %x[res_stride]\n"
            "ble 8f\n"
            "st1w { z0.s }, p1, [x20]\n"
            "8:"  // Row tail: Accumulator store skip
            "subs x24, x24, #0x8\n"
            "add %x[res_ptr], %x[res_ptr], #0x20\n"
            "bne 6b\n"
            "subs x13, x13, #0x4\n"
            "add %x[a_ptr], %x[a_ptr], x12\n"
            "mov %x[res_ptr], x23\n"
            "bgt 5b\n"
            "9:"  // Row tail: Row loop skip
            : [a_ptr] "+&r" (a_ptr), [res_ptr] "+&r" (res_ptr)
            : [b_ptr] "r" (b_ptr), [nr] "r" (nr), [nb] "r" (nb), [res_stride] "r" (res_stride), [nc] "r" (nc)
            : "cc", "memory", "p0", "p1", "x9", "x10", "x11", "x12", "x13", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7", "z8", "z9", "z10", "z11", "z12", "z13", "z14", "z15", "z16", "z17", "z18", "z19", "z20", "z21", "z22", "z23", "z24", "z25", "z26", "z27", "z28", "z29", "z30", "z31"
        );
        return;
    }
#endif // #if defined(__ARM_FEATURE_SVE) && defined(__ARM_FEATURE_MATMUL_INT8)
#elif defined(__AVX2__) || defined(__AVX512F__)
    {
        const block_q4_0x8 * b_ptr_start = (const block_q4_0x8 *)vx;
        const block_q8_0x4 * a_ptr_start = (const block_q8_0x4 *)vy;
        int64_t b_nb = n / QK4_0;
        int64_t y = 0;
        // Mask to mask out nibbles from packed bytes
        const __m256i m4b = _mm256_set1_epi8(0x0F);
        const __m128i loadMask = _mm_blend_epi32(_mm_setzero_si128(), _mm_set1_epi32(0xFFFFFFFF), 3);
        // Lookup table to convert signed nibbles to signed bytes
        __m256i signextendlut = _mm256_castsi128_si256(_mm_set_epi8(-1, -2, -3, -4, -5, -6, -7, -8, 7, 6, 5, 4, 3, 2, 1, 0));
        signextendlut = _mm256_permute2f128_si256(signextendlut, signextendlut, 0);
        // Permute mask used for easier vector processing at later stages
        __m256i requiredOrder = _mm256_set_epi32(3, 2, 1, 0, 7, 6, 5, 4);
        int64_t xstart = 0;
        int anr = nr - nr%16; // Used to align nr with boundary of 16
    #ifdef __AVX512F__
        int anc = nc - nc%16; // Used to align nc with boundary of 16
        // Mask to mask out nibbles from packed bytes expanded to 512 bit length
        const __m512i m4bexpanded = _mm512_set1_epi8(0x0F);
        // Lookup table to convert signed nibbles to signed bytes expanded to 512 bit length
        __m512i signextendlutexpanded = _mm512_inserti32x8(_mm512_castsi256_si512(signextendlut), signextendlut, 1);

        // Take group of four block_q8_0x4 structures at each pass of the loop and perform dot product operation
        for (; y < anr / 4; y += 4) {

            const block_q8_0x4 * a_ptrs[4];

            a_ptrs[0] = a_ptr_start + (y * nb);
            for (int i = 0; i < 3; ++i) {
                a_ptrs[i + 1] = a_ptrs[i] + nb;
            }

            // Take group of two block_q4_0x8 structures at each pass of the loop and perform dot product operation
            for (int64_t x = 0; x < anc / 8; x += 2) {

                const block_q4_0x8 * b_ptr_0 = b_ptr_start + ((x)     * b_nb);
                const block_q4_0x8 * b_ptr_1 = b_ptr_start + ((x + 1) * b_nb);

                // Master FP accumulators
                __m512 acc_rows[16];
                for (int i = 0; i < 16; i++) {
                    acc_rows[i] = _mm512_setzero_ps();
                }

                for (int64_t b = 0; b < nb; b++) {
                    // Load the sixteen block_q4_0 quantized values interleaved with each other in chunks of eight - B0,B1 ....BE,BF
                    const __m256i rhs_raw_mat_0123_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs));
                    const __m256i rhs_raw_mat_4567_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs + 32));
                    const __m256i rhs_raw_mat_0123_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs + 64));
                    const __m256i rhs_raw_mat_4567_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs + 96));

                    const __m256i rhs_raw_mat_89AB_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs));
                    const __m256i rhs_raw_mat_CDEF_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs + 32));
                    const __m256i rhs_raw_mat_89AB_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs + 64));
                    const __m256i rhs_raw_mat_CDEF_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs + 96));

                    // Save the values in the following vectors in the formats B0B1B4B5B8B9BCBD, B2B3B6B7BABBBEBF for further processing and storing of values
                    const __m256i rhs_raw_mat_0145_0 = _mm256_blend_epi32(rhs_raw_mat_0123_0, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_0, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_0 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_0, requiredOrder), rhs_raw_mat_4567_0, 240);
                    const __m256i rhs_raw_mat_0145_1 = _mm256_blend_epi32(rhs_raw_mat_0123_1, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_1, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_1 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_1, requiredOrder), rhs_raw_mat_4567_1, 240);

                    const __m256i rhs_raw_mat_89CD_0 = _mm256_blend_epi32(rhs_raw_mat_89AB_0, _mm256_permutevar8x32_epi32(rhs_raw_mat_CDEF_0, requiredOrder), 240);
                    const __m256i rhs_raw_mat_ABEF_0 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_89AB_0, requiredOrder), rhs_raw_mat_CDEF_0, 240);
                    const __m256i rhs_raw_mat_89CD_1 = _mm256_blend_epi32(rhs_raw_mat_89AB_1, _mm256_permutevar8x32_epi32(rhs_raw_mat_CDEF_1, requiredOrder), 240);
                    const __m256i rhs_raw_mat_ABEF_1 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_89AB_1, requiredOrder), rhs_raw_mat_CDEF_1, 240);

                    const __m512i rhs_raw_mat_014589CD_0 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_0145_0), rhs_raw_mat_89CD_0, 1);
                    const __m512i rhs_raw_mat_2367ABEF_0 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_2367_0), rhs_raw_mat_ABEF_0, 1);
                    const __m512i rhs_raw_mat_014589CD_1 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_0145_1), rhs_raw_mat_89CD_1, 1);
                    const __m512i rhs_raw_mat_2367ABEF_1 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_2367_1), rhs_raw_mat_ABEF_1, 1);

                    // 4-bit -> 8-bit - Sign is maintained
                    const __m512i rhs_mat_014589CD_0 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_014589CD_0, m4bexpanded)); //B0(0-7) B1(0-7) B4(0-7) B5(0-7) B8(0-7) B9(0-7) BC(0-7) BD(0-7)
                    const __m512i rhs_mat_2367ABEF_0 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_2367ABEF_0, m4bexpanded)); //B2(0-7) B3(0-7) B6(0-7) B7(0-7) BA(0-7) BB(0-7) BE(0-7) BF(0-7)

                    const __m512i rhs_mat_014589CD_1 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_014589CD_1, m4bexpanded)); //B0(8-15) B1(8-15) B4(8-15) B5(8-15) B8(8-15) B9(8-15) BC(8-15) BD(8-15)
                    const __m512i rhs_mat_2367ABEF_1 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_2367ABEF_1, m4bexpanded)); //B2(8-15) B3(8-15) B6(8-15) B7(8-15) BA(8-15) BB(8-15) BE(8-15) BF(8-15)

                    const __m512i rhs_mat_014589CD_2 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_014589CD_0, 4), m4bexpanded)); //B0(16-23) B1(16-23) B4(16-23) B5(16-23) B8(16-23) B9(16-23) BC(16-23) BD(16-23)
                    const __m512i rhs_mat_2367ABEF_2 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_2367ABEF_0, 4), m4bexpanded)); //B2(16-23) B3(16-23) B6(16-23) B7(16-23) BA(16-23) BB(16-23) BE(16-23) BF(16-23)

                    const __m512i rhs_mat_014589CD_3 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_014589CD_1, 4), m4bexpanded)); //B0(24-31) B1(24-31) B4(24-31) B5(24-31) B8(24-31) B9(24-31) BC(24-31) BD(24-31)
                    const __m512i rhs_mat_2367ABEF_3 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_2367ABEF_1, 4), m4bexpanded)); //B2(24-31) B3(24-31) B6(24-31) B7(24-31) BA(24-31) BB(24-31) BE(24-31) BF(24-31)

                    // Shuffle pattern one - right side input
                    const __m512i rhs_mat_014589CD_0_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_0, 136); //B0(0-3) B1(0-3) B0(0-3) B1(0-3) B4(0-3) B5(0-3) B4(0-3) B5(0-3) B8(0-3) B9(0-3) B8(0-3) B9(0-3) BC(0-3) BD(0-3) BC(0-3) BD(0-3)
                    const __m512i rhs_mat_2367ABEF_0_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_0, 136); //B2(0-3) B3(0-3) B2(0-3) B3(0-3) B6(0-3) B7(0-3) B6(0-3) B7(0-3) BA(0-3) BB(0-3) BA(0-3) BB(0-3) BE(0-3) BF(0-3) BE(0-3) BF(0-3)

                    const __m512i rhs_mat_014589CD_1_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_1, 136); //B0(8-11) B1(8-11) B0(8-11) B1(8-11) B4(8-11) B5(8-11) B4(8-11) B5(8-11) B8(8-11) B9(8-11) B8(8-11) B9(8-11) BC(8-11) BD(8-11) BC(8-11) BD(8-11)
                    const __m512i rhs_mat_2367ABEF_1_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_1, 136); //B2(8-11) B3(8-11) B2(8-11) B3(8-11) B6(8-11) B7(8-11) B6(8-11) B7(8-11) BA(8-11) BB(8-11) BA(8-11) BB(8-11) BE(8-11) BF(8-11) BE(8-11) BF(8-11)

                    const __m512i rhs_mat_014589CD_2_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_2, 136); //B0(16-19) B1(16-19) B0(16-19) B1(16-19) B4(16-19) B5(16-19) B4(16-19) B5(16-19) B8(16-19) B9(16-19) B8(16-19) B9(16-19) BC(16-19) BD(16-19) BC(16-19) BD(16-19)
                    const __m512i rhs_mat_2367ABEF_2_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_2, 136); //B2(16-19) B3(16-19) B2(16-19) B3(16-19) B6(16-19) B7(16-19) B6(16-19) B7(16-19) BA(16-19) BB(16-19) BA(16-19) BB(16-19) BE(16-19) BF(16-19) BE(16-19) BF(16-19)

                    const __m512i rhs_mat_014589CD_3_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_3, 136); //B0(24-27) B1(24-27) B0(24-27) B1(24-27) B4(24-27) B5(24-27) B4(24-27) B5(24-27) B8(24-27) B9(24-27) B8(24-27) B9(24-27) BC(24-27) BD(24-27) BC(24-27) BD(24-27)
                    const __m512i rhs_mat_2367ABEF_3_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_3, 136); //B2(24-27) B3(24-27) B2(24-27) B3(24-27) B6(24-27) B7(24-27) B6(24-27) B7(24-27) BA(24-27) BB(24-27) BA(24-27) BB(24-27) BE(24-27) BF(24-27) BE(24-27) BF(24-27)

                    // Shuffle pattern two - right side input

                    const __m512i rhs_mat_014589CD_0_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_0, 221); //B0(4-7) B1(4-7) B0(4-7) B1(4-7) B4(4-7) B5(4-7) B4(4-7) B5(4-7) B8(4-7) B9(4-7) B8(4-7) B9(4-7) BC(4-7) BD(4-7) BC(4-7) BD(4-7)
                    const __m512i rhs_mat_2367ABEF_0_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_0, 221); //B2(4-7) B3(4-7) B2(4-7) B3(4-7) B6(4-7) B7(4-7) B6(4-7) B7(4-7) BA(4-7) BB(4-7) BA(4-7) BB(4-7) BE(4-7) BF(4-7) BE(4-7) BF(4-7)

                    const __m512i rhs_mat_014589CD_1_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_1, 221); //B0(12-15) B1(12-15) B0(12-15) B1(12-15) B4(12-15) B5(12-15) B4(12-15) B5(12-15) B8(12-15) B9(12-15) B8(12-15) B9(12-15) BC(12-15) BD(12-15) BC(12-15) BD(12-15)
                    const __m512i rhs_mat_2367ABEF_1_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_1, 221); //B2(12-15) B3(12-15) B2(12-15) B3(12-15) B6(12-15) B7(12-15) B6(12-15) B7(12-15) BA(12-15) BB(12-15) BA(12-15) BB(12-15) BE(12-15) BF(12-15) BE(12-15) BF(12-15)

                    const __m512i rhs_mat_014589CD_2_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_2, 221); //B0(20-23) B1(20-23) B0(20-23) B1(20-23) B4(20-23) B5(20-23) B4(20-23) B5(20-23) B8(20-23) B9(20-23) B8(20-23) B9(20-23) BC(20-23) BD(20-23) BC(20-23) BD(20-23)
                    const __m512i rhs_mat_2367ABEF_2_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_2, 221); //B2(20-23) B3(20-23) B2(20-23) B3(20-23) B6(20-23) B7(20-23) B6(20-23) B7(20-23) BA(20-23) BB(20-23) BA(20-23) BB(20-23) BE(20-23) BF(20-23) BE(20-23) BF(20-23)

                    const __m512i rhs_mat_014589CD_3_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_3, 221); //B0(28-31) B1(28-31) B0(28-31) B1(28-31) B4(28-31) B5(28-31) B4(28-31) B5(28-31) B8(28-31) B9(28-31) B8(28-31) B9(28-31) BC(28-31) BD(28-31) BC(28-31) BD(28-31)
                    const __m512i rhs_mat_2367ABEF_3_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_3, 221); //B2(28-31) B3(28-31) B2(28-31) B3(28-31) B6(28-31) B7(28-31) B6(28-31) B7(28-31) BA(28-31) BB(28-31) BA(28-31) BB(28-31) BE(28-31) BF(28-31) BE(28-31) BF(28-31)

                    // Scale values - Load the weight scale values of two block_q4_0x8
                    const __m512 col_scale_f32 = GGML_F32Cx8x2_LOAD(b_ptr_0[b].d, b_ptr_1[b].d);

                    // Process LHS in pairs of rows
                    for (int rp = 0; rp < 4; rp++) {

                        // Load the four block_q4_0 quantized values interleaved with each other in chunks of eight - A0,A1,A2,A3
                        // Loaded as set of 128 bit vectors and repeated and stored into a 256 bit vector before again repeating into 512 bit vector
                        __m256i lhs_mat_ymm_0123_0 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs)));
                        __m256i lhs_mat_ymm_01_0 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_0, lhs_mat_ymm_0123_0, 0);
                        __m256i lhs_mat_ymm_23_0 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_0, lhs_mat_ymm_0123_0, 17);
                        __m256i lhs_mat_ymm_0123_1 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs + 32)));
                        __m256i lhs_mat_ymm_01_1 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_1, lhs_mat_ymm_0123_1, 0);
                        __m256i lhs_mat_ymm_23_1 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_1, lhs_mat_ymm_0123_1, 17);
                        __m256i lhs_mat_ymm_0123_2 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs + 64)));
                        __m256i lhs_mat_ymm_01_2 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_2, lhs_mat_ymm_0123_2, 0);
                        __m256i lhs_mat_ymm_23_2 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_2, lhs_mat_ymm_0123_2, 17);
                        __m256i lhs_mat_ymm_0123_3 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs + 96)));
                        __m256i lhs_mat_ymm_01_3 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_3, lhs_mat_ymm_0123_3, 0);
                        __m256i lhs_mat_ymm_23_3 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_3, lhs_mat_ymm_0123_3, 17);

                        __m512i lhs_mat_01_0 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_0), lhs_mat_ymm_01_0, 1);
                        __m512i lhs_mat_23_0 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_0), lhs_mat_ymm_23_0, 1);
                        __m512i lhs_mat_01_1 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_1), lhs_mat_ymm_01_1, 1);
                        __m512i lhs_mat_23_1 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_1), lhs_mat_ymm_23_1, 1);
                        __m512i lhs_mat_01_2 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_2), lhs_mat_ymm_01_2, 1);
                        __m512i lhs_mat_23_2 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_2), lhs_mat_ymm_23_2, 1);
                        __m512i lhs_mat_01_3 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_3), lhs_mat_ymm_01_3, 1);
                        __m512i lhs_mat_23_3 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_3), lhs_mat_ymm_23_3, 1);

                        // Shuffle pattern one - left side input

                        const __m512i lhs_mat_01_0_sp1 = _mm512_shuffle_epi32(lhs_mat_01_0, 160);  //A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3)
                        const __m512i lhs_mat_23_0_sp1 = _mm512_shuffle_epi32(lhs_mat_23_0, 160);  //A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3)

                        const __m512i lhs_mat_01_1_sp1 = _mm512_shuffle_epi32(lhs_mat_01_1, 160);  //A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11)
                        const __m512i lhs_mat_23_1_sp1 = _mm512_shuffle_epi32(lhs_mat_23_1, 160);  //A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11)

                        const __m512i lhs_mat_01_2_sp1 = _mm512_shuffle_epi32(lhs_mat_01_2, 160);  //A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19)
                        const __m512i lhs_mat_23_2_sp1 = _mm512_shuffle_epi32(lhs_mat_23_2, 160);  //A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19)

                        const __m512i lhs_mat_01_3_sp1 = _mm512_shuffle_epi32(lhs_mat_01_3, 160);  //A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27)
                        const __m512i lhs_mat_23_3_sp1 = _mm512_shuffle_epi32(lhs_mat_23_3, 160);  //A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27)

                        // Shuffle pattern two - left side input

                        const __m512i lhs_mat_01_0_sp2 = _mm512_shuffle_epi32(lhs_mat_01_0, 245);  //A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7)
                        const __m512i lhs_mat_23_0_sp2 = _mm512_shuffle_epi32(lhs_mat_23_0, 245);  //A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7)

                        const __m512i lhs_mat_01_1_sp2 = _mm512_shuffle_epi32(lhs_mat_01_1, 245);  //A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15)
                        const __m512i lhs_mat_23_1_sp2 = _mm512_shuffle_epi32(lhs_mat_23_1, 245);  //A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15)

                        const __m512i lhs_mat_01_2_sp2 = _mm512_shuffle_epi32(lhs_mat_01_2, 245);  //A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23)
                        const __m512i lhs_mat_23_2_sp2 = _mm512_shuffle_epi32(lhs_mat_23_2, 245);  //A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23)

                        const __m512i lhs_mat_01_3_sp2 = _mm512_shuffle_epi32(lhs_mat_01_3, 245);  //A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31)
                        const __m512i lhs_mat_23_3_sp2 = _mm512_shuffle_epi32(lhs_mat_23_3, 245);  //A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31)

                        // The values arranged in shuffle patterns are operated with dot product operation within 32 bit lane i.e corresponding bytes and multiplied and added into 32 bit integers within 32 bit lane
                        // Resembles MMLAs into 2x2 matrices in ARM Version
                        __m512i iacc_mat_00_sp1 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp1, rhs_mat_014589CD_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp1, rhs_mat_014589CD_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp1, rhs_mat_014589CD_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp1, rhs_mat_014589CD_0_sp1));
                        __m512i iacc_mat_01_sp1 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp1, rhs_mat_2367ABEF_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp1, rhs_mat_2367ABEF_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp1, rhs_mat_2367ABEF_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp1, rhs_mat_2367ABEF_0_sp1));
                        __m512i iacc_mat_10_sp1 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp1, rhs_mat_014589CD_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp1, rhs_mat_014589CD_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp1, rhs_mat_014589CD_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp1, rhs_mat_014589CD_0_sp1));
                        __m512i iacc_mat_11_sp1 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp1, rhs_mat_2367ABEF_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp1, rhs_mat_2367ABEF_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp1, rhs_mat_2367ABEF_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp1, rhs_mat_2367ABEF_0_sp1));
                        __m512i iacc_mat_00_sp2 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp2, rhs_mat_014589CD_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp2, rhs_mat_014589CD_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp2, rhs_mat_014589CD_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp2, rhs_mat_014589CD_0_sp2));
                        __m512i iacc_mat_01_sp2 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp2, rhs_mat_2367ABEF_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp2, rhs_mat_2367ABEF_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp2, rhs_mat_2367ABEF_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp2, rhs_mat_2367ABEF_0_sp2));
                        __m512i iacc_mat_10_sp2 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp2, rhs_mat_014589CD_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp2, rhs_mat_014589CD_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp2, rhs_mat_014589CD_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp2, rhs_mat_014589CD_0_sp2));
                        __m512i iacc_mat_11_sp2 =
                            _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp2, rhs_mat_2367ABEF_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp2, rhs_mat_2367ABEF_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp2, rhs_mat_2367ABEF_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp2, rhs_mat_2367ABEF_0_sp2));

                        // Output of both shuffle patterns are added in order to sum dot product outputs of all 32 values in block
                        __m512i iacc_mat_00 = _mm512_add_epi32(iacc_mat_00_sp1, iacc_mat_00_sp2);
                        __m512i iacc_mat_01 = _mm512_add_epi32(iacc_mat_01_sp1, iacc_mat_01_sp2);
                        __m512i iacc_mat_10 = _mm512_add_epi32(iacc_mat_10_sp1, iacc_mat_10_sp2);
                        __m512i iacc_mat_11 = _mm512_add_epi32(iacc_mat_11_sp1, iacc_mat_11_sp2);


                        // Straighten out to make 4 row vectors
                        __m512i iacc_row_0 = _mm512_mask_blend_epi32(0xCCCC, iacc_mat_00, _mm512_shuffle_epi32(iacc_mat_01, 78));
                        __m512i iacc_row_1 = _mm512_mask_blend_epi32(0xCCCC, _mm512_shuffle_epi32(iacc_mat_00, 78), iacc_mat_01);
                        __m512i iacc_row_2 = _mm512_mask_blend_epi32(0xCCCC, iacc_mat_10, _mm512_shuffle_epi32(iacc_mat_11, 78));
                        __m512i iacc_row_3 = _mm512_mask_blend_epi32(0xCCCC, _mm512_shuffle_epi32(iacc_mat_10, 78), iacc_mat_11);

                        // Load the scale(d) values for all the 4 Q8_0 blocks and repeat it across lanes
                        const __m128i row_scale_f16 = _mm_shuffle_epi32(_mm_maskload_epi32((int const*)(a_ptrs[rp][b].d), loadMask), 68);
                        const __m512 row_scale_f32 = GGML_F32Cx16_REPEAT_LOAD(row_scale_f16);

                        // Multiply with appropiate scales and accumulate
                        acc_rows[rp * 4]     = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_0), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 0)),   acc_rows[rp * 4]);
                        acc_rows[rp * 4 + 1] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_1), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 85)),  acc_rows[rp * 4 + 1]);
                        acc_rows[rp * 4 + 2] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_2), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 170)), acc_rows[rp * 4 + 2]);
                        acc_rows[rp * 4 + 3] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_3), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 255)), acc_rows[rp * 4 + 3]);
                    }
                }

                // Store the accumulated values
                for (int i = 0; i < 16; i++) {
                    _mm512_storeu_ps((float *)(s + ((y * 4 + i) * bs + x * 8)), acc_rows[i]);
                }
            }
        }
        // Take a block_q8_0x4 structures at each pass of the loop and perform dot product operation
        for (; y < nr / 4; y ++) {

            const block_q8_0x4 * a_ptr = a_ptr_start + (y * nb);

            // Take group of two block_q4_0x8 structures at each pass of the loop and perform dot product operation
            for (int64_t x = 0; x < anc / 8; x += 2) {

                const block_q4_0x8 * b_ptr_0 = b_ptr_start + ((x)     * b_nb);
                const block_q4_0x8 * b_ptr_1 = b_ptr_start + ((x + 1) * b_nb);

                // Master FP accumulators
                __m512 acc_rows[4];
                for (int i = 0; i < 4; i++) {
                    acc_rows[i] = _mm512_setzero_ps();
                }

                for (int64_t b = 0; b < nb; b++) {
                    // Load the sixteen block_q4_0 quantized values interleaved with each other in chunks of eight - B0,B1 ....BE,BF
                    const __m256i rhs_raw_mat_0123_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs));
                    const __m256i rhs_raw_mat_4567_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs + 32));
                    const __m256i rhs_raw_mat_0123_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs + 64));
                    const __m256i rhs_raw_mat_4567_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_0[b].qs + 96));

                    const __m256i rhs_raw_mat_89AB_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs));
                    const __m256i rhs_raw_mat_CDEF_0 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs + 32));
                    const __m256i rhs_raw_mat_89AB_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs + 64));
                    const __m256i rhs_raw_mat_CDEF_1 = _mm256_loadu_si256((const __m256i *)(b_ptr_1[b].qs + 96));

                    // Save the values in the following vectors in the formats B0B1B4B5, B2B3B6B7 for further processing and storing of valuess
                    const __m256i rhs_raw_mat_0145_0 = _mm256_blend_epi32(rhs_raw_mat_0123_0, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_0, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_0 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_0, requiredOrder), rhs_raw_mat_4567_0, 240);
                    const __m256i rhs_raw_mat_0145_1 = _mm256_blend_epi32(rhs_raw_mat_0123_1, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_1, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_1 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_1, requiredOrder), rhs_raw_mat_4567_1, 240);

                    const __m256i rhs_raw_mat_89CD_0 = _mm256_blend_epi32(rhs_raw_mat_89AB_0, _mm256_permutevar8x32_epi32(rhs_raw_mat_CDEF_0, requiredOrder), 240);
                    const __m256i rhs_raw_mat_ABEF_0 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_89AB_0, requiredOrder), rhs_raw_mat_CDEF_0, 240);
                    const __m256i rhs_raw_mat_89CD_1 = _mm256_blend_epi32(rhs_raw_mat_89AB_1, _mm256_permutevar8x32_epi32(rhs_raw_mat_CDEF_1, requiredOrder), 240);
                    const __m256i rhs_raw_mat_ABEF_1 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_89AB_1, requiredOrder), rhs_raw_mat_CDEF_1, 240);

                    const __m512i rhs_raw_mat_014589CD_0 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_0145_0), rhs_raw_mat_89CD_0, 1);
                    const __m512i rhs_raw_mat_2367ABEF_0 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_2367_0), rhs_raw_mat_ABEF_0, 1);
                    const __m512i rhs_raw_mat_014589CD_1 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_0145_1), rhs_raw_mat_89CD_1, 1);
                    const __m512i rhs_raw_mat_2367ABEF_1 = _mm512_inserti32x8(_mm512_castsi256_si512(rhs_raw_mat_2367_1), rhs_raw_mat_ABEF_1, 1);

                    // 4-bit -> 8-bit - Sign is maintained
                    const __m512i rhs_mat_014589CD_0 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_014589CD_0, m4bexpanded)); //B0(0-7) B1(0-7) B4(0-7) B5(0-7) B8(0-7) B9(0-7) BC(0-7) BD(0-7)
                    const __m512i rhs_mat_2367ABEF_0 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_2367ABEF_0, m4bexpanded)); //B2(0-7) B3(0-7) B6(0-7) B7(0-7) BA(0-7) BB(0-7) BE(0-7) BF(0-7)

                    const __m512i rhs_mat_014589CD_1 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_014589CD_1, m4bexpanded)); //B0(8-15) B1(8-15) B4(8-15) B5(8-15) B8(8-15) B9(8-15) BC(8-15) BD(8-15)
                    const __m512i rhs_mat_2367ABEF_1 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(rhs_raw_mat_2367ABEF_1, m4bexpanded)); //B2(8-15) B3(8-15) B6(8-15) B7(8-15) BA(8-15) BB(8-15) BE(8-15) BF(8-15)

                    const __m512i rhs_mat_014589CD_2 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_014589CD_0, 4), m4bexpanded)); //B0(16-23) B1(16-23) B4(16-23) B5(16-23) B8(16-23) B9(16-23) BC(16-23) BD(16-23)
                    const __m512i rhs_mat_2367ABEF_2 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_2367ABEF_0, 4), m4bexpanded)); //B2(16-23) B3(16-23) B6(16-23) B7(16-23) BA(16-23) BB(16-23) BE(16-23) BF(16-23)

                    const __m512i rhs_mat_014589CD_3 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_014589CD_1, 4), m4bexpanded)); //B0(24-31) B1(24-31) B4(24-31) B5(24-31) B8(24-31) B9(24-31) BC(24-31) BD(24-31)
                    const __m512i rhs_mat_2367ABEF_3 = _mm512_shuffle_epi8(signextendlutexpanded, _mm512_and_si512(_mm512_srli_epi16(rhs_raw_mat_2367ABEF_1, 4), m4bexpanded)); //B2(24-31) B3(24-31) B6(24-31) B7(24-31) BA(24-31) BB(24-31) BE(24-31) BF(24-31)

                    // Shuffle pattern one - right side input
                    const __m512i rhs_mat_014589CD_0_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_0, 136); //B0(0-3) B1(0-3) B0(0-3) B1(0-3) B4(0-3) B5(0-3) B4(0-3) B5(0-3) B8(0-3) B9(0-3) B8(0-3) B9(0-3) BC(0-3) BD(0-3) BC(0-3) BD(0-3)
                    const __m512i rhs_mat_2367ABEF_0_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_0, 136); //B2(0-3) B3(0-3) B2(0-3) B3(0-3) B6(0-3) B7(0-3) B6(0-3) B7(0-3) BA(0-3) BB(0-3) BA(0-3) BB(0-3) BE(0-3) BF(0-3) BE(0-3) BF(0-3)

                    const __m512i rhs_mat_014589CD_1_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_1, 136); //B0(8-11) B1(8-11) B0(8-11) B1(8-11) B4(8-11) B5(8-11) B4(8-11) B5(8-11) B8(8-11) B9(8-11) B8(8-11) B9(8-11) BC(8-11) BD(8-11) BC(8-11) BD(8-11)
                    const __m512i rhs_mat_2367ABEF_1_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_1, 136); //B2(8-11) B3(8-11) B2(8-11) B3(8-11) B6(8-11) B7(8-11) B6(8-11) B7(8-11) BA(8-11) BB(8-11) BA(8-11) BB(8-11) BE(8-11) BF(8-11) BE(8-11) BF(8-11)

                    const __m512i rhs_mat_014589CD_2_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_2, 136); //B0(16-19) B1(16-19) B0(16-19) B1(16-19) B4(16-19) B5(16-19) B4(16-19) B5(16-19) B8(16-19) B9(16-19) B8(16-19) B9(16-19) BC(16-19) BD(16-19) BC(16-19) BD(16-19)
                    const __m512i rhs_mat_2367ABEF_2_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_2, 136); //B2(16-19) B3(16-19) B2(16-19) B3(16-19) B6(16-19) B7(16-19) B6(16-19) B7(16-19) BA(16-19) BB(16-19) BA(16-19) BB(16-19) BE(16-19) BF(16-19) BE(16-19) BF(16-19)

                    const __m512i rhs_mat_014589CD_3_sp1 = _mm512_shuffle_epi32(rhs_mat_014589CD_3, 136); //B0(24-27) B1(24-27) B0(24-27) B1(24-27) B4(24-27) B5(24-27) B4(24-27) B5(24-27) B8(24-27) B9(24-27) B8(24-27) B9(24-27) BC(24-27) BD(24-27) BC(24-27) BD(24-27)
                    const __m512i rhs_mat_2367ABEF_3_sp1 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_3, 136); //B2(24-27) B3(24-27) B2(24-27) B3(24-27) B6(24-27) B7(24-27) B6(24-27) B7(24-27) BA(24-27) BB(24-27) BA(24-27) BB(24-27) BE(24-27) BF(24-27) BE(24-27) BF(24-27)

                    // Shuffle pattern two - right side input

                    const __m512i rhs_mat_014589CD_0_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_0, 221); //B0(4-7) B1(4-7) B0(4-7) B1(4-7) B4(4-7) B5(4-7) B4(4-7) B5(4-7) B8(4-7) B9(4-7) B8(4-7) B9(4-7) BC(4-7) BD(4-7) BC(4-7) BD(4-7)
                    const __m512i rhs_mat_2367ABEF_0_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_0, 221); //B2(4-7) B3(4-7) B2(4-7) B3(4-7) B6(4-7) B7(4-7) B6(4-7) B7(4-7) BA(4-7) BB(4-7) BA(4-7) BB(4-7) BE(4-7) BF(4-7) BE(4-7) BF(4-7)

                    const __m512i rhs_mat_014589CD_1_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_1, 221); //B0(12-15) B1(12-15) B0(12-15) B1(12-15) B4(12-15) B5(12-15) B4(12-15) B5(12-15) B8(12-15) B9(12-15) B8(12-15) B9(12-15) BC(12-15) BD(12-15) BC(12-15) BD(12-15)
                    const __m512i rhs_mat_2367ABEF_1_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_1, 221); //B2(12-15) B3(12-15) B2(12-15) B3(12-15) B6(12-15) B7(12-15) B6(12-15) B7(12-15) BA(12-15) BB(12-15) BA(12-15) BB(12-15) BE(12-15) BF(12-15) BE(12-15) BF(12-15)

                    const __m512i rhs_mat_014589CD_2_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_2, 221); //B0(20-23) B1(20-23) B0(20-23) B1(20-23) B4(20-23) B5(20-23) B4(20-23) B5(20-23) B8(20-23) B9(20-23) B8(20-23) B9(20-23) BC(20-23) BD(20-23) BC(20-23) BD(20-23)
                    const __m512i rhs_mat_2367ABEF_2_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_2, 221); //B2(20-23) B3(20-23) B2(20-23) B3(20-23) B6(20-23) B7(20-23) B6(20-23) B7(20-23) BA(20-23) BB(20-23) BA(20-23) BB(20-23) BE(20-23) BF(20-23) BE(20-23) BF(20-23)

                    const __m512i rhs_mat_014589CD_3_sp2 = _mm512_shuffle_epi32(rhs_mat_014589CD_3, 221); //B0(28-31) B1(28-31) B0(28-31) B1(28-31) B4(28-31) B5(28-31) B4(28-31) B5(28-31) B8(28-31) B9(28-31) B8(28-31) B9(28-31) BC(28-31) BD(28-31) BC(28-31) BD(28-31)
                    const __m512i rhs_mat_2367ABEF_3_sp2 = _mm512_shuffle_epi32(rhs_mat_2367ABEF_3, 221); //B2(28-31) B3(28-31) B2(28-31) B3(28-31) B6(28-31) B7(28-31) B6(28-31) B7(28-31) BA(28-31) BB(28-31) BA(28-31) BB(28-31) BE(28-31) BF(28-31) BE(28-31) BF(28-31)


                    // Scale values - Load the weight scale values of two block_q4_0x8
                    const __m512 col_scale_f32 = GGML_F32Cx8x2_LOAD(b_ptr_0[b].d, b_ptr_1[b].d);

                    // Load the four block_q4_0 quantized values interleaved with each other in chunks of eight - A0,A1,A2,A3
                    // Loaded as set of 128 bit vectors and repeated and stored into a 256 bit vector before again repeating into 512 bit vector
                    __m256i lhs_mat_ymm_0123_0 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs)));
                    __m256i lhs_mat_ymm_01_0 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_0, lhs_mat_ymm_0123_0, 0);
                    __m256i lhs_mat_ymm_23_0 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_0, lhs_mat_ymm_0123_0, 17);
                    __m256i lhs_mat_ymm_0123_1 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs + 32)));
                    __m256i lhs_mat_ymm_01_1 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_1, lhs_mat_ymm_0123_1, 0);
                    __m256i lhs_mat_ymm_23_1 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_1, lhs_mat_ymm_0123_1, 17);
                    __m256i lhs_mat_ymm_0123_2 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs + 64)));
                    __m256i lhs_mat_ymm_01_2 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_2, lhs_mat_ymm_0123_2, 0);
                    __m256i lhs_mat_ymm_23_2 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_2, lhs_mat_ymm_0123_2, 17);
                    __m256i lhs_mat_ymm_0123_3 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs + 96)));
                    __m256i lhs_mat_ymm_01_3 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_3, lhs_mat_ymm_0123_3, 0);
                    __m256i lhs_mat_ymm_23_3 = _mm256_permute2f128_si256(lhs_mat_ymm_0123_3, lhs_mat_ymm_0123_3, 17);

                    __m512i lhs_mat_01_0 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_0), lhs_mat_ymm_01_0, 1);
                    __m512i lhs_mat_23_0 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_0), lhs_mat_ymm_23_0, 1);
                    __m512i lhs_mat_01_1 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_1), lhs_mat_ymm_01_1, 1);
                    __m512i lhs_mat_23_1 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_1), lhs_mat_ymm_23_1, 1);
                    __m512i lhs_mat_01_2 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_2), lhs_mat_ymm_01_2, 1);
                    __m512i lhs_mat_23_2 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_2), lhs_mat_ymm_23_2, 1);
                    __m512i lhs_mat_01_3 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_01_3), lhs_mat_ymm_01_3, 1);
                    __m512i lhs_mat_23_3 = _mm512_inserti32x8(_mm512_castsi256_si512(lhs_mat_ymm_23_3), lhs_mat_ymm_23_3, 1);

                    // Shuffle pattern one - left side input

                    const __m512i lhs_mat_01_0_sp1 = _mm512_shuffle_epi32(lhs_mat_01_0, 160);  //A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3)
                    const __m512i lhs_mat_23_0_sp1 = _mm512_shuffle_epi32(lhs_mat_23_0, 160);  //A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3)

                    const __m512i lhs_mat_01_1_sp1 = _mm512_shuffle_epi32(lhs_mat_01_1, 160);  //A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11)
                    const __m512i lhs_mat_23_1_sp1 = _mm512_shuffle_epi32(lhs_mat_23_1, 160);  //A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11)

                    const __m512i lhs_mat_01_2_sp1 = _mm512_shuffle_epi32(lhs_mat_01_2, 160);  //A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19)
                    const __m512i lhs_mat_23_2_sp1 = _mm512_shuffle_epi32(lhs_mat_23_2, 160);  //A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19)

                    const __m512i lhs_mat_01_3_sp1 = _mm512_shuffle_epi32(lhs_mat_01_3, 160);  //A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27)
                    const __m512i lhs_mat_23_3_sp1 = _mm512_shuffle_epi32(lhs_mat_23_3, 160);  //A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27)

                    // Shuffle pattern two - left side input

                    const __m512i lhs_mat_01_0_sp2 = _mm512_shuffle_epi32(lhs_mat_01_0, 245);  //A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7)
                    const __m512i lhs_mat_23_0_sp2 = _mm512_shuffle_epi32(lhs_mat_23_0, 245);  //A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7)

                    const __m512i lhs_mat_01_1_sp2 = _mm512_shuffle_epi32(lhs_mat_01_1, 245);  //A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15)
                    const __m512i lhs_mat_23_1_sp2 = _mm512_shuffle_epi32(lhs_mat_23_1, 245);  //A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15)

                    const __m512i lhs_mat_01_2_sp2 = _mm512_shuffle_epi32(lhs_mat_01_2, 245);  //A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23)
                    const __m512i lhs_mat_23_2_sp2 = _mm512_shuffle_epi32(lhs_mat_23_2, 245);  //A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23)

                    const __m512i lhs_mat_01_3_sp2 = _mm512_shuffle_epi32(lhs_mat_01_3, 245);  //A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31)
                    const __m512i lhs_mat_23_3_sp2 = _mm512_shuffle_epi32(lhs_mat_23_3, 245);  //A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31)

                    // The values arranged in shuffle patterns are operated with dot product operation within 32 bit lane i.e corresponding bytes and multiplied and added into 32 bit integers within 32 bit lane
                    // Resembles MMLAs into 2x2 matrices in ARM Version
                    __m512i iacc_mat_00_sp1 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp1, rhs_mat_014589CD_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp1, rhs_mat_014589CD_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp1, rhs_mat_014589CD_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp1, rhs_mat_014589CD_0_sp1));
                    __m512i iacc_mat_01_sp1 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp1, rhs_mat_2367ABEF_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp1, rhs_mat_2367ABEF_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp1, rhs_mat_2367ABEF_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp1, rhs_mat_2367ABEF_0_sp1));
                    __m512i iacc_mat_10_sp1 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp1, rhs_mat_014589CD_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp1, rhs_mat_014589CD_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp1, rhs_mat_014589CD_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp1, rhs_mat_014589CD_0_sp1));
                    __m512i iacc_mat_11_sp1 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp1, rhs_mat_2367ABEF_3_sp1), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp1, rhs_mat_2367ABEF_2_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp1, rhs_mat_2367ABEF_1_sp1)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp1, rhs_mat_2367ABEF_0_sp1));
                    __m512i iacc_mat_00_sp2 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp2, rhs_mat_014589CD_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp2, rhs_mat_014589CD_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp2, rhs_mat_014589CD_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp2, rhs_mat_014589CD_0_sp2));
                    __m512i iacc_mat_01_sp2 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_01_3_sp2, rhs_mat_2367ABEF_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_01_2_sp2, rhs_mat_2367ABEF_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_1_sp2, rhs_mat_2367ABEF_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_01_0_sp2, rhs_mat_2367ABEF_0_sp2));
                    __m512i iacc_mat_10_sp2 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp2, rhs_mat_014589CD_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp2, rhs_mat_014589CD_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp2, rhs_mat_014589CD_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp2, rhs_mat_014589CD_0_sp2));
                    __m512i iacc_mat_11_sp2 =
                        _mm512_add_epi32(_mm512_add_epi32(_mm512_add_epi32(mul_sum_i8_pairs_int32x16(lhs_mat_23_3_sp2, rhs_mat_2367ABEF_3_sp2), mul_sum_i8_pairs_int32x16(lhs_mat_23_2_sp2, rhs_mat_2367ABEF_2_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_1_sp2, rhs_mat_2367ABEF_1_sp2)), mul_sum_i8_pairs_int32x16(lhs_mat_23_0_sp2, rhs_mat_2367ABEF_0_sp2));

                    // Output of both shuffle patterns are added in order to sum dot product outputs of all 32 values in block
                    __m512i iacc_mat_00 = _mm512_add_epi32(iacc_mat_00_sp1, iacc_mat_00_sp2);
                    __m512i iacc_mat_01 = _mm512_add_epi32(iacc_mat_01_sp1, iacc_mat_01_sp2);
                    __m512i iacc_mat_10 = _mm512_add_epi32(iacc_mat_10_sp1, iacc_mat_10_sp2);
                    __m512i iacc_mat_11 = _mm512_add_epi32(iacc_mat_11_sp1, iacc_mat_11_sp2);


                    // Straighten out to make 4 row vectors
                    __m512i iacc_row_0 = _mm512_mask_blend_epi32(0xCCCC, iacc_mat_00, _mm512_shuffle_epi32(iacc_mat_01, 78));
                    __m512i iacc_row_1 = _mm512_mask_blend_epi32(0xCCCC, _mm512_shuffle_epi32(iacc_mat_00, 78), iacc_mat_01);
                    __m512i iacc_row_2 = _mm512_mask_blend_epi32(0xCCCC, iacc_mat_10, _mm512_shuffle_epi32(iacc_mat_11, 78));
                    __m512i iacc_row_3 = _mm512_mask_blend_epi32(0xCCCC, _mm512_shuffle_epi32(iacc_mat_10, 78), iacc_mat_11);

                    // Load the scale(d) values for all the 4 Q8_0 blocks and repeat it across lanes
                    const __m128i row_scale_f16 = _mm_shuffle_epi32(_mm_maskload_epi32((int const*)(a_ptr[b].d), loadMask), 68);
                    const __m512 row_scale_f32 = GGML_F32Cx16_REPEAT_LOAD(row_scale_f16);

                    // Multiply with appropiate scales and accumulate
                    acc_rows[0] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_0), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 0)),   acc_rows[0]);
                    acc_rows[1] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_1), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 85)),  acc_rows[1]);
                    acc_rows[2] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_2), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 170)), acc_rows[2]);
                    acc_rows[3] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(iacc_row_3), _mm512_mul_ps(col_scale_f32, _mm512_shuffle_ps(row_scale_f32, row_scale_f32, 255)), acc_rows[3]);
                }

                // Store the accumulated values
                for (int i = 0; i < 4; i++) {
                    _mm512_storeu_ps((float *)(s + ((y * 4 + i) * bs + x * 8)), acc_rows[i]);
                }
            }
        }
        if (anc != nc) {
            xstart = anc/8;
            y = 0;
        }
    #endif // __AVX512F__

        // Take group of four block_q8_0x4 structures at each pass of the loop and perform dot product operation

        for (; y < anr / 4; y += 4) {
            const block_q8_0x4 * a_ptrs[4];

            a_ptrs[0] = a_ptr_start + (y * nb);
            for (int i = 0; i < 3; ++i) {
                a_ptrs[i + 1] = a_ptrs[i] + nb;
            }

            // Take group of eight block_q4_0x8 structures at each pass of the loop and perform dot product operation
            for (int64_t x = xstart; x < nc / 8; x++) {

                const block_q4_0x8 * b_ptr = b_ptr_start + (x * b_nb);

                // Master FP accumulators
                __m256 acc_rows[16];
                for (int i = 0; i < 16; i++) {
                    acc_rows[i] = _mm256_setzero_ps();
                }

                for (int64_t b = 0; b < nb; b++) {
                    // Load the eight block_q4_0 quantized values interleaved with each other in chunks of eight - B0,B1 ....B6,B7
                    const __m256i rhs_raw_mat_0123_0 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs));
                    const __m256i rhs_raw_mat_4567_0 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs + 32));
                    const __m256i rhs_raw_mat_0123_1 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs + 64));
                    const __m256i rhs_raw_mat_4567_1 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs + 96));

                    // Save the values in the following vectors in the formats B0B1B4B5, B2B3B6B7 for further processing and storing of values
                    const __m256i rhs_raw_mat_0145_0 = _mm256_blend_epi32(rhs_raw_mat_0123_0, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_0, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_0 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_0, requiredOrder), rhs_raw_mat_4567_0, 240);
                    const __m256i rhs_raw_mat_0145_1 = _mm256_blend_epi32(rhs_raw_mat_0123_1, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_1, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_1 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_1, requiredOrder), rhs_raw_mat_4567_1, 240);

                    // 4-bit -> 8-bit - Sign is maintained
                    const __m256i rhs_mat_0145_0 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_0145_0, m4b)); //B0(0-7) B1(0-7) B4(0-7) B5(0-7)
                    const __m256i rhs_mat_2367_0 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_2367_0, m4b)); //B2(0-7) B3(0-7) B6(0-7) B7(0-7)

                    const __m256i rhs_mat_0145_1 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_0145_1, m4b)); //B0(8-15) B1(8-15) B4(8-15) B5(8-15)
                    const __m256i rhs_mat_2367_1 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_2367_1, m4b)); //B2(8-15) B3(8-15) B6(8-15) B7(8-15)

                    const __m256i rhs_mat_0145_2 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_0145_0, 4), m4b)); //B0(16-23) B1(16-23) B4(16-23) B5(16-23)
                    const __m256i rhs_mat_2367_2 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_2367_0, 4), m4b)); //B2(16-23) B3(16-23) B6(16-23) B7(16-23)

                    const __m256i rhs_mat_0145_3 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_0145_1, 4), m4b)); //B0(24-31) B1(24-31) B4(24-31) B5(24-31)
                    const __m256i rhs_mat_2367_3 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_2367_1, 4), m4b)); //B2(24-31) B3(24-31) B6(24-31) B7(24-31)

                    // Shuffle pattern one - right side input
                    const __m256i rhs_mat_0145_0_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_0, 136);  //B0(0-3) B1(0-3) B0(0-3) B1(0-3) B4(0-3) B5(0-3) B4(0-3) B5(0-3)
                    const __m256i rhs_mat_2367_0_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_0, 136);  //B2(0-3) B3(0-3) B2(0-3) B3(0-3) B6(0-3) B7(0-3) B6(0-3) B7(0-3)

                    const __m256i rhs_mat_0145_1_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_1, 136);  //B0(8-11) B1(8-11) B0(8-11) B1(8-11) B4(8-11) B5(8-11) B4(8-11) B5(8-11)
                    const __m256i rhs_mat_2367_1_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_1, 136);  //B2(8-11) B3(8-11) B2(8-11) B3(8-11) B6(8-11) B7(8-11) B6(8-11) B7(8-11)

                    const __m256i rhs_mat_0145_2_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_2, 136);  //B0(16-19) B1(16-19) B0(16-19) B1(16-19) B4(16-19) B5(16-19) B4(16-19) B5(16-19)
                    const __m256i rhs_mat_2367_2_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_2, 136);  //B2(16-19) B3(16-19) B2(16-19) B3(16-19) B6(16-19) B7(16-19) B6(16-19) B7(16-19)

                    const __m256i rhs_mat_0145_3_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_3, 136);  //B0(24-27) B1(24-27) B0(24-27) B1(24-27) B4(24-27) B5(24-27) B4(24-27) B5(24-27)
                    const __m256i rhs_mat_2367_3_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_3, 136);  //B2(24-27) B3(24-27) B2(24-27) B3(24-27) B6(24-27) B7(24-27) B6(24-27) B7(24-27)

                    // Shuffle pattern two - right side input

                    const __m256i rhs_mat_0145_0_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_0, 221);  //B0(4-7) B1(4-7) B0(4-7) B1(4-7) B4(4-7) B5(4-7) B4(4-7) B5(4-7)
                    const __m256i rhs_mat_2367_0_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_0, 221);  //B2(4-7) B3(4-7) B2(4-7) B3(4-7) B6(4-7) B7(4-7) B6(4-7) B7(4-7)

                    const __m256i rhs_mat_0145_1_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_1, 221);  //B0(12-15) B1(12-15) B0(12-15) B1(12-15) B4(12-15) B5(12-15) B4(12-15) B5(12-15)
                    const __m256i rhs_mat_2367_1_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_1, 221);  //B2(12-15) B3(12-15) B2(12-15) B3(12-15) B6(12-15) B7(12-15) B6(12-15) B7(12-15)

                    const __m256i rhs_mat_0145_2_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_2, 221);  //B0(20-23) B1(20-23) B0(20-23) B1(20-23) B4(20-23) B5(20-23) B4(20-23) B5(20-23)
                    const __m256i rhs_mat_2367_2_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_2, 221);  //B2(20-23) B3(20-23) B2(20-23) B3(20-23) B6(20-23) B7(20-23) B6(20-23) B7(20-23)

                    const __m256i rhs_mat_0145_3_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_3, 221);  //B0(28-31) B1(28-31) B0(28-31) B1(28-31) B4(28-31) B5(28-31) B4(28-31) B5(28-31)
                    const __m256i rhs_mat_2367_3_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_3, 221);  //B2(28-31) B3(28-31) B2(28-31) B3(28-31) B6(28-31) B7(28-31) B6(28-31) B7(28-31)

                    // Scale values - Load the wight scale values of block_q4_0x8
                    const __m256 col_scale_f32 = GGML_F32Cx8_LOAD(b_ptr[b].d);

                    // Process LHS in groups of four
                    for (int rp = 0; rp < 4; rp++) {
                        // Load the four block_q4_0 quantized values interleaved with each other in chunks of eight - A0,A1,A2,A3
                        // Loaded as set of 128 bit vectors and repeated into a 256 bit vector
                        __m256i lhs_mat_0123_0 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs)));
                        __m256i lhs_mat_01_0 = _mm256_permute2f128_si256(lhs_mat_0123_0, lhs_mat_0123_0, 0);
                        __m256i lhs_mat_23_0 = _mm256_permute2f128_si256(lhs_mat_0123_0, lhs_mat_0123_0, 17);
                        __m256i lhs_mat_0123_1 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs + 32)));
                        __m256i lhs_mat_01_1 = _mm256_permute2f128_si256(lhs_mat_0123_1, lhs_mat_0123_1, 0);
                        __m256i lhs_mat_23_1 = _mm256_permute2f128_si256(lhs_mat_0123_1, lhs_mat_0123_1, 17);
                        __m256i lhs_mat_0123_2 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs + 64)));
                        __m256i lhs_mat_01_2 = _mm256_permute2f128_si256(lhs_mat_0123_2, lhs_mat_0123_2, 0);
                        __m256i lhs_mat_23_2 = _mm256_permute2f128_si256(lhs_mat_0123_2, lhs_mat_0123_2, 17);
                        __m256i lhs_mat_0123_3 = _mm256_loadu_si256((const __m256i *)((a_ptrs[rp][b].qs + 96)));
                        __m256i lhs_mat_01_3 = _mm256_permute2f128_si256(lhs_mat_0123_3, lhs_mat_0123_3, 0);
                        __m256i lhs_mat_23_3 = _mm256_permute2f128_si256(lhs_mat_0123_3, lhs_mat_0123_3, 17);

                        // Shuffle pattern one - left side input
                        const __m256i lhs_mat_01_0_sp1 = _mm256_shuffle_epi32(lhs_mat_01_0, 160);  //A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3)
                        const __m256i lhs_mat_23_0_sp1 = _mm256_shuffle_epi32(lhs_mat_23_0, 160);  //A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3)

                        const __m256i lhs_mat_01_1_sp1 = _mm256_shuffle_epi32(lhs_mat_01_1, 160);  //A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11)
                        const __m256i lhs_mat_23_1_sp1 = _mm256_shuffle_epi32(lhs_mat_23_1, 160);  //A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11)

                        const __m256i lhs_mat_01_2_sp1 = _mm256_shuffle_epi32(lhs_mat_01_2, 160);  //A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19)
                        const __m256i lhs_mat_23_2_sp1 = _mm256_shuffle_epi32(lhs_mat_23_2, 160);  //A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19)

                        const __m256i lhs_mat_01_3_sp1 = _mm256_shuffle_epi32(lhs_mat_01_3, 160);  //A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27)
                        const __m256i lhs_mat_23_3_sp1 = _mm256_shuffle_epi32(lhs_mat_23_3, 160);  //A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27)

                        // Shuffle pattern two - left side input
                        const __m256i lhs_mat_01_0_sp2 = _mm256_shuffle_epi32(lhs_mat_01_0, 245);  //A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7)
                        const __m256i lhs_mat_23_0_sp2 = _mm256_shuffle_epi32(lhs_mat_23_0, 245);  //A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7)

                        const __m256i lhs_mat_01_1_sp2 = _mm256_shuffle_epi32(lhs_mat_01_1, 245);  //A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15)
                        const __m256i lhs_mat_23_1_sp2 = _mm256_shuffle_epi32(lhs_mat_23_1, 245);  //A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15)

                        const __m256i lhs_mat_01_2_sp2 = _mm256_shuffle_epi32(lhs_mat_01_2, 245);  //A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23)
                        const __m256i lhs_mat_23_2_sp2 = _mm256_shuffle_epi32(lhs_mat_23_2, 245);  //A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23)

                        const __m256i lhs_mat_01_3_sp2 = _mm256_shuffle_epi32(lhs_mat_01_3, 245);  //A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31)
                        const __m256i lhs_mat_23_3_sp2 = _mm256_shuffle_epi32(lhs_mat_23_3, 245);  //A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31)

                        // The values arranged in shuffle patterns are operated with dot product operation within 32 bit lane i.e corresponding bytes and multiplied and added into 32 bit integers within 32 bit lane
                        // Resembles MMLAs into 2x2 matrices in ARM Version
                        __m256i iacc_mat_00_sp1 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp1, rhs_mat_0145_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp1, rhs_mat_0145_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp1, rhs_mat_0145_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp1, rhs_mat_0145_0_sp1));
                        __m256i iacc_mat_01_sp1 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp1, rhs_mat_2367_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp1, rhs_mat_2367_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp1, rhs_mat_2367_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp1, rhs_mat_2367_0_sp1));
                        __m256i iacc_mat_10_sp1 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp1, rhs_mat_0145_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp1, rhs_mat_0145_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp1, rhs_mat_0145_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp1, rhs_mat_0145_0_sp1));
                        __m256i iacc_mat_11_sp1 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp1, rhs_mat_2367_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp1, rhs_mat_2367_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp1, rhs_mat_2367_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp1, rhs_mat_2367_0_sp1));
                        __m256i iacc_mat_00_sp2 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp2, rhs_mat_0145_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp2, rhs_mat_0145_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp2, rhs_mat_0145_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp2, rhs_mat_0145_0_sp2));
                        __m256i iacc_mat_01_sp2 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp2, rhs_mat_2367_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp2, rhs_mat_2367_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp2, rhs_mat_2367_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp2, rhs_mat_2367_0_sp2));
                        __m256i iacc_mat_10_sp2 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp2, rhs_mat_0145_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp2, rhs_mat_0145_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp2, rhs_mat_0145_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp2, rhs_mat_0145_0_sp2));
                        __m256i iacc_mat_11_sp2 =
                            _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp2, rhs_mat_2367_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp2, rhs_mat_2367_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp2, rhs_mat_2367_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp2, rhs_mat_2367_0_sp2));

                        // Output of both shuffle patterns are added in order to sum dot product outputs of all 32 values in block
                        __m256i iacc_mat_00 = _mm256_add_epi32(iacc_mat_00_sp1, iacc_mat_00_sp2);
                        __m256i iacc_mat_01 = _mm256_add_epi32(iacc_mat_01_sp1, iacc_mat_01_sp2);
                        __m256i iacc_mat_10 = _mm256_add_epi32(iacc_mat_10_sp1, iacc_mat_10_sp2);
                        __m256i iacc_mat_11 = _mm256_add_epi32(iacc_mat_11_sp1, iacc_mat_11_sp2);

                        // Straighten out to make 4 row vectors
                        __m256i iacc_row_0 = _mm256_blend_epi32(iacc_mat_00, _mm256_shuffle_epi32(iacc_mat_01, 78), 204);
                        __m256i iacc_row_1 = _mm256_blend_epi32(_mm256_shuffle_epi32(iacc_mat_00, 78), iacc_mat_01, 204);
                        __m256i iacc_row_2 = _mm256_blend_epi32(iacc_mat_10, _mm256_shuffle_epi32(iacc_mat_11, 78), 204);
                        __m256i iacc_row_3 = _mm256_blend_epi32(_mm256_shuffle_epi32(iacc_mat_10, 78), iacc_mat_11, 204);

                        // Load the scale(d) values for all the 4 Q8_0 blocks and repeat it across lanes
                        const __m256 row_scale_f32 = GGML_F32Cx8_REPEAT_LOAD(a_ptrs[rp][b].d, loadMask);

                        // Multiply with appropiate scales and accumulate
                        acc_rows[rp * 4] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_0), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32, 0)), acc_rows[rp * 4]);
                        acc_rows[rp * 4 + 1] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_1), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32, 85)), acc_rows[rp * 4 + 1]);
                        acc_rows[rp * 4 + 2] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_2), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32, 170)), acc_rows[rp * 4 + 2]);
                        acc_rows[rp * 4 + 3] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_3), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32,  255)), acc_rows[rp * 4 + 3]);
                    }
                }

                // Store the accumulated values
                for (int i = 0; i < 16; i++) {
                    _mm256_storeu_ps((float *)(s + ((y * 4 + i) * bs + x * 8)), acc_rows[i]);
                }
            }
        }

        // Take a block_q8_0x4 structures at each pass of the loop and perform dot product operation
        for (; y < nr / 4; y ++) {

            const block_q8_0x4 * a_ptr = a_ptr_start + (y * nb);

            // Load the eight block_q4_0 quantized values interleaved with each other in chunks of eight - B0,B1 ....B6,B7
            for (int64_t x = xstart; x < nc / 8; x++) {

                const block_q4_0x8 * b_ptr = b_ptr_start + (x * b_nb);

                // Master FP accumulators
                __m256 acc_rows[4];
                for (int i = 0; i < 4; i++) {
                    acc_rows[i] = _mm256_setzero_ps();
                }

                for (int64_t b = 0; b < nb; b++) {
                    // Load the eight block_q8_0 quantized values interleaved with each other in chunks of eight - B0,B1 ....B6,B7
                    const __m256i rhs_raw_mat_0123_0 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs));
                    const __m256i rhs_raw_mat_4567_0 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs + 32));
                    const __m256i rhs_raw_mat_0123_1 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs + 64));
                    const __m256i rhs_raw_mat_4567_1 = _mm256_loadu_si256((const __m256i *)(b_ptr[b].qs + 96));

                    // Save the values in the following vectors in the formats B0B1B4B5, B2B3B6B7 for further processing and storing of valuess
                    const __m256i rhs_raw_mat_0145_0 = _mm256_blend_epi32(rhs_raw_mat_0123_0, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_0, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_0 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_0, requiredOrder), rhs_raw_mat_4567_0, 240);
                    const __m256i rhs_raw_mat_0145_1 = _mm256_blend_epi32(rhs_raw_mat_0123_1, _mm256_permutevar8x32_epi32(rhs_raw_mat_4567_1, requiredOrder), 240);
                    const __m256i rhs_raw_mat_2367_1 = _mm256_blend_epi32(_mm256_permutevar8x32_epi32(rhs_raw_mat_0123_1, requiredOrder), rhs_raw_mat_4567_1, 240);

                    // 4-bit -> 8-bit - Sign is maintained
                    const __m256i rhs_mat_0145_0 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_0145_0, m4b));  //B0(0-7) B1(0-7) B4(0-7) B5(0-7)
                    const __m256i rhs_mat_2367_0 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_2367_0, m4b));  //B2(0-7) B3(0-7) B6(0-7) B7(0-7)

                    const __m256i rhs_mat_0145_1 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_0145_1, m4b));  //B0(8-15) B1(8-15) B4(8-15) B5(8-15)
                    const __m256i rhs_mat_2367_1 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(rhs_raw_mat_2367_1, m4b));  //B2(8-15) B3(8-15) B6(8-15) B7(8-15)

                    const __m256i rhs_mat_0145_2 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_0145_0, 4), m4b));  //B0(16-23) B1(16-23) B4(16-23) B5(16-23)
                    const __m256i rhs_mat_2367_2 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_2367_0, 4), m4b));  //B2(16-23) B3(16-23) B6(16-23) B7(16-23)

                    const __m256i rhs_mat_0145_3 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_0145_1, 4), m4b));  //B0(24-31) B1(24-31) B4(24-31) B5(24-31)
                    const __m256i rhs_mat_2367_3 = _mm256_shuffle_epi8(signextendlut, _mm256_and_si256(_mm256_srli_epi16(rhs_raw_mat_2367_1, 4), m4b));  //B2(24-31) B3(24-31) B6(24-31) B7(24-31)

                    // Shuffle pattern one - right side input
                    const __m256i rhs_mat_0145_0_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_0, 136);  //B0(0-3) B1(0-3) B0(0-3) B1(0-3) B4(0-3) B5(0-3) B4(0-3) B5(0-3)
                    const __m256i rhs_mat_2367_0_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_0, 136);  //B2(0-3) B3(0-3) B2(0-3) B3(0-3) B6(0-3) B7(0-3) B6(0-3) B7(0-3)

                    const __m256i rhs_mat_0145_1_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_1, 136);  //B0(8-11) B1(8-11) B0(8-11) B1(8-11) B4(8-11) B5(8-11) B4(8-11) B5(8-11)
                    const __m256i rhs_mat_2367_1_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_1, 136);  //B2(8-11) B3(8-11) B2(8-11) B3(8-11) B6(8-11) B7(8-11) B6(8-11) B7(8-11)

                    const __m256i rhs_mat_0145_2_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_2, 136);  //B0(16-19) B1(16-19) B0(16-19) B1(16-19) B4(16-19) B5(16-19) B4(16-19) B5(16-19)
                    const __m256i rhs_mat_2367_2_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_2, 136);  //B2(16-19) B3(16-19) B2(16-19) B3(16-19) B6(16-19) B7(16-19) B6(16-19) B7(16-19)

                    const __m256i rhs_mat_0145_3_sp1 = _mm256_shuffle_epi32(rhs_mat_0145_3, 136);  //B0(24-27) B1(24-27) B0(24-27) B1(24-27) B4(24-27) B5(24-27) B4(24-27) B5(24-27)
                    const __m256i rhs_mat_2367_3_sp1 = _mm256_shuffle_epi32(rhs_mat_2367_3, 136);  //B2(24-27) B3(24-27) B2(24-27) B3(24-27) B6(24-27) B7(24-27) B6(24-27) B7(24-27)

                    // Shuffle pattern two - right side input

                    const __m256i rhs_mat_0145_0_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_0, 221);  //B0(4-7) B1(4-7) B0(4-7) B1(4-7) B4(4-7) B5(4-7) B4(4-7) B5(4-7)
                    const __m256i rhs_mat_2367_0_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_0, 221);  //B2(4-7) B3(4-7) B2(4-7) B3(4-7) B6(4-7) B7(4-7) B6(4-7) B7(4-7)

                    const __m256i rhs_mat_0145_1_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_1, 221);  //B0(12-15) B1(12-15) B0(12-15) B1(12-15) B4(12-15) B5(12-15) B4(12-15) B5(12-15)
                    const __m256i rhs_mat_2367_1_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_1, 221);  //B2(12-15) B3(12-15) B2(12-15) B3(12-15) B6(12-15) B7(12-15) B6(12-15) B7(12-15)

                    const __m256i rhs_mat_0145_2_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_2, 221);  //B0(20-23) B1(20-23) B0(20-23) B1(20-23) B4(20-23) B5(20-23) B4(20-23) B5(20-23)
                    const __m256i rhs_mat_2367_2_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_2, 221);  //B2(20-23) B3(20-23) B2(20-23) B3(20-23) B6(20-23) B7(20-23) B6(20-23) B7(20-23)

                    const __m256i rhs_mat_0145_3_sp2 = _mm256_shuffle_epi32(rhs_mat_0145_3, 221);  //B0(28-31) B1(28-31) B0(28-31) B1(28-31) B4(28-31) B5(28-31) B4(28-31) B5(28-31)
                    const __m256i rhs_mat_2367_3_sp2 = _mm256_shuffle_epi32(rhs_mat_2367_3, 221);  //B2(28-31) B3(28-31) B2(28-31) B3(28-31) B6(28-31) B7(28-31) B6(28-31) B7(28-31)

                    // Scale values - Load the wight scale values of block_q4_0x8
                    const __m256 col_scale_f32 = GGML_F32Cx8_LOAD(b_ptr[b].d);

                    // Load the four block_q4_0 quantized values interleaved with each other in chunks of eight - A0,A1,A2,A3
                    // Loaded as set of 128 bit vectors and repeated into a 256 bit vector
                    __m256i lhs_mat_0123_0 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs)));
                    __m256i lhs_mat_01_0 = _mm256_permute2f128_si256(lhs_mat_0123_0, lhs_mat_0123_0, 0);
                    __m256i lhs_mat_23_0 = _mm256_permute2f128_si256(lhs_mat_0123_0, lhs_mat_0123_0, 17);
                    __m256i lhs_mat_0123_1 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs + 32)));
                    __m256i lhs_mat_01_1 = _mm256_permute2f128_si256(lhs_mat_0123_1, lhs_mat_0123_1, 0);
                    __m256i lhs_mat_23_1 = _mm256_permute2f128_si256(lhs_mat_0123_1, lhs_mat_0123_1, 17);
                    __m256i lhs_mat_0123_2 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs + 64)));
                    __m256i lhs_mat_01_2 = _mm256_permute2f128_si256(lhs_mat_0123_2, lhs_mat_0123_2, 0);
                    __m256i lhs_mat_23_2 = _mm256_permute2f128_si256(lhs_mat_0123_2, lhs_mat_0123_2, 17);
                    __m256i lhs_mat_0123_3 = _mm256_loadu_si256((const __m256i *)((a_ptr[b].qs + 96)));
                    __m256i lhs_mat_01_3 = _mm256_permute2f128_si256(lhs_mat_0123_3, lhs_mat_0123_3, 0);
                    __m256i lhs_mat_23_3 = _mm256_permute2f128_si256(lhs_mat_0123_3, lhs_mat_0123_3, 17);

                    // Shuffle pattern one - left side input

                    const __m256i lhs_mat_01_0_sp1 = _mm256_shuffle_epi32(lhs_mat_01_0, 160);  //A0(0-3) A0(0-3) A1(0-3) A1(0-3) A0(0-3) A0(0-3) A1(0-3) A1(0-3)
                    const __m256i lhs_mat_23_0_sp1 = _mm256_shuffle_epi32(lhs_mat_23_0, 160);  //A2(0-3) A2(0-3) A3(0-3) A3(0-3) A2(0-3) A2(0-3) A3(0-3) A3(0-3)

                    const __m256i lhs_mat_01_1_sp1 = _mm256_shuffle_epi32(lhs_mat_01_1, 160);  //A0(8-11) A0(8-11) A1(8-11) A1(8-11) A0(8-11) A0(8-11) A1(8-11) A1(8-11)
                    const __m256i lhs_mat_23_1_sp1 = _mm256_shuffle_epi32(lhs_mat_23_1, 160);  //A2(8-11) A2(8-11) A3(8-11) A3(8-11) A2(8-11) A2(8-11) A3(8-11) A3(8-11)

                    const __m256i lhs_mat_01_2_sp1 = _mm256_shuffle_epi32(lhs_mat_01_2, 160);  //A0(16-19) A0(16-19) A1(16-19) A1(16-19) A0(16-19) A0(16-19) A1(16-19) A1(16-19)
                    const __m256i lhs_mat_23_2_sp1 = _mm256_shuffle_epi32(lhs_mat_23_2, 160);  //A2(16-19) A2(16-19) A3(16-19) A3(16-19) A2(16-19) A2(16-19) A3(16-19) A3(16-19)

                    const __m256i lhs_mat_01_3_sp1 = _mm256_shuffle_epi32(lhs_mat_01_3, 160);  //A0(24-27) A0(24-27) A1(24-27) A1(24-27) A0(24-27) A0(24-27) A1(24-27) A1(24-27)
                    const __m256i lhs_mat_23_3_sp1 = _mm256_shuffle_epi32(lhs_mat_23_3, 160);  //A2(24-27) A2(24-27) A3(24-27) A3(24-27) A2(24-27) A2(24-27) A3(24-27) A3(24-27)

                    // Shuffle pattern two - left side input

                    const __m256i lhs_mat_01_0_sp2 = _mm256_shuffle_epi32(lhs_mat_01_0, 245);  //A0(4-7) A0(4-7) A1(4-7) A1(4-7) A0(4-7) A0(4-7) A1(4-7) A1(4-7)
                    const __m256i lhs_mat_23_0_sp2 = _mm256_shuffle_epi32(lhs_mat_23_0, 245);  //A2(4-7) A2(4-7) A3(4-7) A3(4-7) A2(4-7) A2(4-7) A3(4-7) A3(4-7)

                    const __m256i lhs_mat_01_1_sp2 = _mm256_shuffle_epi32(lhs_mat_01_1, 245);  //A0(12-15) A0(12-15) A1(12-15) A1(12-15) A0(12-15) A0(12-15) A1(12-15) A1(12-15)
                    const __m256i lhs_mat_23_1_sp2 = _mm256_shuffle_epi32(lhs_mat_23_1, 245);  //A2(12-15) A2(12-15) A3(12-15) A3(12-15) A2(12-15) A2(12-15) A3(12-15) A3(12-15)

                    const __m256i lhs_mat_01_2_sp2 = _mm256_shuffle_epi32(lhs_mat_01_2, 245);  //A0(20-23) A0(20-23) A1(20-23) A1(20-23) A0(20-23) A0(20-23) A1(20-23) A1(20-23)
                    const __m256i lhs_mat_23_2_sp2 = _mm256_shuffle_epi32(lhs_mat_23_2, 245);  //A2(20-23) A2(20-23) A3(20-23) A3(20-23) A2(20-23) A2(20-23) A3(20-23) A3(20-23)

                    const __m256i lhs_mat_01_3_sp2 = _mm256_shuffle_epi32(lhs_mat_01_3, 245);  //A0(28-31) A0(28-31) A1(28-31) A1(28-31) A0(28-31) A0(28-31) A1(28-31) A1(28-31)
                    const __m256i lhs_mat_23_3_sp2 = _mm256_shuffle_epi32(lhs_mat_23_3, 245);  //A2(28-31) A2(28-31) A3(28-31) A3(28-31) A2(28-31) A2(28-31) A3(28-31) A3(28-31)

                    // The values arranged in shuffle patterns are operated with dot product operation within 32 bit lane i.e corresponding bytes and multiplied and added into 32 bit integers within 32 bit lane
                    // Resembles MMLAs into 2x2 matrices in ARM Version
                    __m256i iacc_mat_00_sp1 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp1, rhs_mat_0145_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp1, rhs_mat_0145_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp1, rhs_mat_0145_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp1, rhs_mat_0145_0_sp1));
                    __m256i iacc_mat_01_sp1 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp1, rhs_mat_2367_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp1, rhs_mat_2367_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp1, rhs_mat_2367_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp1, rhs_mat_2367_0_sp1));
                    __m256i iacc_mat_10_sp1 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp1, rhs_mat_0145_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp1, rhs_mat_0145_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp1, rhs_mat_0145_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp1, rhs_mat_0145_0_sp1));
                    __m256i iacc_mat_11_sp1 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp1, rhs_mat_2367_3_sp1), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp1, rhs_mat_2367_2_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp1, rhs_mat_2367_1_sp1)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp1, rhs_mat_2367_0_sp1));
                    __m256i iacc_mat_00_sp2 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp2, rhs_mat_0145_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp2, rhs_mat_0145_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp2, rhs_mat_0145_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp2, rhs_mat_0145_0_sp2));
                    __m256i iacc_mat_01_sp2 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_01_3_sp2, rhs_mat_2367_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_01_2_sp2, rhs_mat_2367_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_1_sp2, rhs_mat_2367_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_01_0_sp2, rhs_mat_2367_0_sp2));
                    __m256i iacc_mat_10_sp2 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp2, rhs_mat_0145_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp2, rhs_mat_0145_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp2, rhs_mat_0145_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp2, rhs_mat_0145_0_sp2));
                    __m256i iacc_mat_11_sp2 =
                        _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(mul_sum_i8_pairs_int32x8(lhs_mat_23_3_sp2, rhs_mat_2367_3_sp2), mul_sum_i8_pairs_int32x8(lhs_mat_23_2_sp2, rhs_mat_2367_2_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_1_sp2, rhs_mat_2367_1_sp2)), mul_sum_i8_pairs_int32x8(lhs_mat_23_0_sp2, rhs_mat_2367_0_sp2));

                    // Output of both shuffle patterns are added in order to sum dot product outputs of all 32 values in block
                    __m256i iacc_mat_00 = _mm256_add_epi32(iacc_mat_00_sp1, iacc_mat_00_sp2);
                    __m256i iacc_mat_01 = _mm256_add_epi32(iacc_mat_01_sp1, iacc_mat_01_sp2);
                    __m256i iacc_mat_10 = _mm256_add_epi32(iacc_mat_10_sp1, iacc_mat_10_sp2);
                    __m256i iacc_mat_11 = _mm256_add_epi32(iacc_mat_11_sp1, iacc_mat_11_sp2);


                    // Straighten out to make 4 row vectors
                    __m256i iacc_row_0 = _mm256_blend_epi32(iacc_mat_00, _mm256_shuffle_epi32(iacc_mat_01, 78), 204);
                    __m256i iacc_row_1 = _mm256_blend_epi32(_mm256_shuffle_epi32(iacc_mat_00, 78), iacc_mat_01, 204);
                    __m256i iacc_row_2 = _mm256_blend_epi32(iacc_mat_10, _mm256_shuffle_epi32(iacc_mat_11, 78), 204);
                    __m256i iacc_row_3 = _mm256_blend_epi32(_mm256_shuffle_epi32(iacc_mat_10, 78), iacc_mat_11, 204);

                    // Load the scale(d) values for all the 4 Q8_0 blocks and repeat it across lanes
                    const __m256 row_scale_f32 = GGML_F32Cx8_REPEAT_LOAD(a_ptr[b].d, loadMask);

                    // Multiply with appropiate scales and accumulate
                    acc_rows[0] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_0), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32, 0)), acc_rows[0]);
                    acc_rows[1] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_1), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32, 85)), acc_rows[1]);
                    acc_rows[2] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_2), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32, 170)), acc_rows[2]);
                    acc_rows[3] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(iacc_row_3), _mm256_mul_ps(col_scale_f32, _mm256_shuffle_ps(row_scale_f32, row_scale_f32, 255)), acc_rows[3]);
                }

                // Store the accumulated values
                for (int i = 0; i < 4; i++) {
                    _mm256_storeu_ps((float *)(s + ((y * 4 + i) * bs + x * 8)), acc_rows[i]);
                }
            }
        }
        return;
    }
#endif // #if ! ((defined(_MSC_VER)) && ! defined(__clang__)) && defined(__aarch64__)
    float sumf[4][8];
    int sumi;

    for (int y = 0; y < nr / 4; y++) {
        const block_q8_0x4 * a_ptr = (const block_q8_0x4 *) vy + (y * nb);
        for (int x = 0; x < nc / ncols_interleaved; x++) {
            const block_q4_0x8 * b_ptr = (const block_q4_0x8 *) vx + (x * nb);
            for (int m = 0; m < 4; m++) {
                for (int j = 0; j < ncols_interleaved; j++) sumf[m][j] = 0.0;
            }
            for (int l = 0; l < nb; l++) {
                for (int k = 0; k < (qk / (2 * blocklen)); k++) {
                    for (int m = 0; m < 4; m++) {
                        for (int j = 0; j < ncols_interleaved; j++) {
                            sumi = 0;
                            for (int i = 0; i < blocklen; ++i) {
                                const int v0 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] << 4);
                                const int v1 = (int8_t) (b_ptr[l].qs[k * ncols_interleaved * blocklen + j * blocklen + i] & 0xF0);
                                sumi += ((v0 * a_ptr[l].qs[k * 4 * blocklen + m * blocklen + i]) +
                                         (v1 * a_ptr[l].qs[k * 4 * blocklen + m * blocklen + i + qk / 2 * 4])) >> 4;
                            }
                            sumf[m][j] += sumi * GGML_FP16_TO_FP32(b_ptr[l].d[j]) * GGML_FP16_TO_FP32(a_ptr[l].d[m]);
                        }
                    }
                }
            }
            for (int m = 0; m < 4; m++) {
                for (int j = 0; j < ncols_interleaved; j++)
                    s[(y * 4 + m) * bs + x * ncols_interleaved + j] = sumf[m][j];
            }
        }
    }
}
