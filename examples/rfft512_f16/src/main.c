/*
 * SPDX-FileCopyrightText: Copyright 2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * rfft512_f16 — CMSIS-DSP 512-point real FFT in float16 on Cortex-M55
 *
 * Steps:
 *   1. Fill a 512-element float16 input buffer with pseudo-random samples in [-1, 1).
 *   2. Run the CMSIS-DSP 512-point forward real FFT (arm_rfft_fast_f16).
 *   3. Compute the complex sum of all output bins.
 *   4. Print the result over UART (retargeted to semihosting on the FVP).
 *
 * Output buffer layout (CMSIS-DSP RFFT, N=512):
 *   output[0]       = X[0].re     (DC bin, imaginary = 0)
 *   output[1]       = X[N/2].re   (Nyquist bin, imaginary = 0; packed into [1])
 *   output[2*k]     = X[k].re     for k = 1 .. N/2-1
 *   output[2*k+1]   = X[k].im     for k = 1 .. N/2-1
 */

#include <stdint.h>
#include <stdio.h>

#include "arm_math_f16.h" /* float16_t, arm_rfft_fast_instance_f16, ... */

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

#define FFT_SIZE 512u /* must be a power of two supported by CMSIS-DSP   */

/* -------------------------------------------------------------------------
 * Buffers  (static so they land in BSS / data, not on the stack)
 * ------------------------------------------------------------------------- */

static arm_rfft_fast_instance_f16 fft_inst;
static float16_t input[FFT_SIZE];
static float16_t output[FFT_SIZE]; /* N/2 complex pairs, interleaved re/im */

/* -------------------------------------------------------------------------
 * Pseudo-random float16 generator  (32-bit Galois LCG, reproducible)
 * Returns a value in [-1.0, 1.0).
 * ------------------------------------------------------------------------- */

static uint32_t lcg_state = 0xDEADBEEFu;

static float16_t rand_f16(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    /* Interpret as signed 32-bit → normalise to [-1, 1) */
    float val = (float)(int32_t)lcg_state * (1.0f / 2147483648.0f);
    return (float16_t)val;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* ------------------------------------------------------------------
     * 1. Initialise input with pseudo-random float16 samples.
     * ------------------------------------------------------------------ */
    for (uint32_t i = 0u; i < FFT_SIZE; ++i)
        input[i] = rand_f16();

    /* ------------------------------------------------------------------
     * 2. Initialise the RFFT instance and run the forward FFT.
     * ------------------------------------------------------------------ */
    arm_rfft_fast_init_f16(&fft_inst, FFT_SIZE);
    arm_rfft_fast_f16(&fft_inst, input, output, 0 /* 0 = forward */);

    /* ------------------------------------------------------------------
     * 3. Compute the complex sum of all N/2 output bins.
     *
     *    DC  (bin 0) : real = output[0], imaginary = 0
     *    Nyquist     : real = output[1], imaginary = 0  (packed in [1])
     *    Bins 1..255 : real = output[2k], imag = output[2k+1]
     * ------------------------------------------------------------------ */
    float sum_re = (float)output[0] + (float)output[1]; /* DC + Nyquist */
    float sum_im = 0.0f;

    for (uint32_t k = 1u; k < FFT_SIZE / 2u; ++k)
    {
        sum_re += (float)output[2u * k];
        sum_im += (float)output[2u * k + 1u];
    }

    /* ------------------------------------------------------------------
     * 4. Print the result over UART (semihosting retarget on FVP).
     * ------------------------------------------------------------------ */
    //printf("RFFT-512 F16 input: %u pseudo-random float16 samples\n", FFT_SIZE);
    printf("Complex sum of output bins: re = %.6f  im = %.6f\n", sum_re, sum_im);

    return 0;
}
