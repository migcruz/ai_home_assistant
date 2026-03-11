#pragma once

#include <zephyr/drivers/ipm.h>

/*
 * PDM microphone capture via I2S0 (GPIO41 CLK, GPIO42 DATA).
 * 16kHz, 16-bit, stereo DMA (mono mic data on left channel).
 *
 * Shared PSRAM layout written by pdm_record():
 *   0x3C000000 .. 0x3C07FFFF  raw stereo PCM (512 KB ≈ 8 s)
 */
#define AUDIO_PSRAM_BASE  0x3C000000UL
#define AUDIO_BUF_MAX     (512U * 1024U)

#define SAMPLE_RATE    16000
#define SAMPLE_WIDTH   16
#define CHANNELS       2
#define BLOCK_MS       20

/* Initialise the I2S driver. ipm_dev is used for log forwarding to procpu. */
int pdm_init(const struct device *ipm_dev);

/*
 * Capture PCM into PSRAM until stop_flag becomes true or the buffer is full.
 * Returns the number of bytes written.
 */
uint32_t pdm_record(volatile bool *stop_flag);
