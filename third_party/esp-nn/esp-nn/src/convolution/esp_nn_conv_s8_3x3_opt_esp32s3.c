/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Optimized 3x3 convolution for ESP32-S3.
 *
 * Key optimization vs the general aligned asm:
 * The general asm reloads input for each output channel (128× per pixel).
 * This version pre-loads the 3x3 input window into scratch (9 rows × in_ch bytes),
 * then iterates output channels with the input in L1 cache.
 *
 * For Conv[11] (26×26×128→12×12×128, 3×3 s2):
 * - Input window: 3 × 3 × 128 = 1,152 bytes (fits in L1)
 * - Filter per OC: 3 × 3 × 128 = 1,152 bytes
 * - Total for all 128 OC: 147,456 bytes (cycles through L1)
 * - Input loaded once vs 128× in the general asm
 */

#include <stdint.h>
#include "../common/esp_nn_filter_sum_esp32s3.h"
#include <string.h>
#include <esp_nn_defs.h>
#include <common_functions.h>

/*
 * Check if a conv can use the optimized 3x3 path.
 * Requirements:
 * - filter_wd == 3 && filter_ht == 3
 * - in_channels >= 16 (SIMD worth it)
 * - in_channels % 16 == 0 (aligned for ee.vld.128)
 */
int esp_nn_conv_s8_3x3_can_use(int filter_wd, int filter_ht,
                                int in_channels, int out_channels)
{
    /* out_channels gate is a measured profitability bound: the per-pixel
     * im2col build amortizes across the output-channel dots, and at
     * out_channels == 1 the path measured 3.8x SLOWER than the general
     * kernel on a 3x3x32 map (S3), while out_channels == 16 wins. */
    return (filter_wd == 3 && filter_ht == 3 &&
            in_channels >= 16 && (in_channels % 16) == 0 &&
            out_channels >= 16);
}

/*
 * Scratch size for the 3x3 optimized path:
 * - im2col buffer: 3 × 3 × in_channels bytes (input window)
 * - corrections: out_channels × 4 bytes
 */
int esp_nn_conv_s8_3x3_scratch_size(int in_channels, int out_channels)
{
    int window_len_aligned = (9 * in_channels + 15) & ~15;
    int im2col = window_len_aligned;
    int filter_copy = out_channels * window_len_aligned;  /* aligned, zero-padded rows */
    int corrections = out_channels * 4;
    return im2col + filter_copy + corrections + 32;
}

/*
 * 3x3 convolution: im2col per pixel, then dot product per output channel.
 * Uses ACCX dot product (ee.vmulas.s8.accx) for the 3×3×in_ch window.
 */
void esp_nn_conv_s8_3x3_opt(const int8_t *input,
                             const uint16_t input_wd,
                             const uint16_t input_ht,
                             const uint16_t in_channels,
                             const int32_t input_offset,
                             const uint16_t pad_wd,
                             const uint16_t pad_ht,
                             const uint16_t stride_wd,
                             const uint16_t stride_ht,
                             const int8_t *filter_data,
                             const int32_t *bias,
                             int8_t *out_data,
                             const uint16_t out_wd,
                             const uint16_t out_ht,
                             const uint16_t out_channels,
                             const int32_t out_offset,
                             const int32_t *out_shift,
                             const int32_t *out_mult,
                             const int32_t activation_min,
                             const int32_t activation_max,
                             void *scratch)
{
    const int window_len = 9 * in_channels; /* 3×3 window */
    const int window_len_aligned = (window_len + 15) & ~15;

    /* Scratch layout: [im2col_buf | filter_aligned | corrections] */
    int8_t *im2col_buf = (int8_t *)((uintptr_t)((int8_t *)scratch + 15) & ~15);
    int8_t *filter_aligned = im2col_buf + window_len_aligned;
    int32_t *corrections = (int32_t *)(filter_aligned
                                       + out_channels * window_len_aligned);

    /* The inner loop is the plain aligned dot - no unaligned SIMD, no
     * priming, no reads outside either operand. in_ch % 16 == 0 makes
     * window_len a multiple of 16 already, so when the filter pointer is
     * 16-byte aligned (TFLM arena/flash weights are) the filter is used in
     * place; only a misaligned caller pays the aligned copy. */
    const int copy_filter = (((uintptr_t)filter_data & 15) != 0)
                            || (window_len_aligned != window_len);
    const int8_t *filter_base = copy_filter ? filter_aligned : filter_data;
    const int8_t *f_ptr = filter_data;
    for (int oc = 0; oc < out_channels; oc++) {
        if (copy_filter) {
            int8_t *slot = filter_aligned + oc * window_len_aligned;
            memcpy(slot, f_ptr, window_len);
            memset(slot + window_len, 0, window_len_aligned - window_len);
        }
        int32_t corr = bias ? bias[oc] : 0;
        if (input_offset != 0) {
            corr += esp_nn_filter_sum_s8_esp32s3(f_ptr, window_len)
                    * input_offset;
        }
        corrections[oc] = corr;
        f_ptr += window_len;
    }

    /* Zero-pad the tail of im2col buffer for aligned SIMD reads */
    memset(im2col_buf + window_len, 0, window_len_aligned - window_len);

    const int in_row_stride = input_wd * in_channels;

    /* Padding value: a padded position must contribute (pad_q + offset) * w
     * = 0, and the corrections term already adds offset * w for EVERY filter
     * position - so the padded cell holds -offset, cancelling it. Covers
     * explicit padding and TFLite's implicit trailing pad through the same
     * bounds checks. */
    const int8_t pad_val = (int8_t)(-input_offset);

    for (int out_y = 0; out_y < out_ht; out_y++) {
        for (int out_x = 0; out_x < out_wd; out_x++) {
            /* Phase 1: Build im2col for this output pixel (one-time per pixel) */
            const int in_y = out_y * stride_ht - pad_ht;
            const int in_x = out_x * stride_wd - pad_wd;
            int8_t *dst = im2col_buf;
            for (int fy = 0; fy < 3; fy++) {
                const int y = in_y + fy;
                if (y < 0 || y >= input_ht) {
                    memset(dst, pad_val, 3 * in_channels);
                } else if (in_x >= 0 && in_x + 3 <= input_wd) {
                    /* interior: single copy (the common case) */
                    memcpy(dst, input + y * in_row_stride + in_x * in_channels,
                           3 * in_channels);
                } else {
                    for (int fx = 0; fx < 3; fx++) {
                        const int x = in_x + fx;
                        if (x < 0 || x >= input_wd) {
                            memset(dst + fx * in_channels, pad_val, in_channels);
                        } else {
                            memcpy(dst + fx * in_channels,
                                   input + y * in_row_stride + x * in_channels,
                                   in_channels);
                        }
                    }
                }
                dst += 3 * in_channels;
            }

            /* Phase 2: dot against each output channel's aligned filter copy */
            for (int oc = 0; oc < out_channels; oc++) {
                int32_t acc = esp_nn_dot_s8_aligned_esp32s3(
                        im2col_buf, filter_base + oc * window_len_aligned,
                        window_len_aligned);
                acc += corrections[oc];
                acc = esp_nn_multiply_by_quantized_mult(acc, out_mult[oc], out_shift[oc]);
                acc += out_offset;
                acc = max(acc, activation_min);
                acc = min(acc, activation_max);
                *out_data++ = (int8_t)acc;
            }
        }
    }
}
