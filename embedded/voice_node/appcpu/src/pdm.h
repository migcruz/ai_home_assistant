#pragma once

#include <zephyr/drivers/ipm.h>
#include "audio_shared.h"

/*
 * PDM microphone capture via I2S0.
 *   GPIO42 = PDM_CLK (I2S WS output, PDM bit clock ~1 MHz)
 *   GPIO41 = PDM_DATA (I2S SD input, bias-pull-down)
 * 16kHz, 16-bit, stereo DMA (mic data in left slot, right slot = zeros).
 * Per-block mono extraction: left slot only written to PSRAM.
 *
 * Shared PSRAM layout (struct audio_shared header + raw mono PCM):
 *   See embedded/common/audio_shared.h
 *
 * AUDIO_PSRAM_BASE: compile-time fallback only — the real address is sent
 * by procpu at boot via IPM_ID_BUFADDR and applied via pdm_set_audio_buf().
 * Verify after a procpu rebuild:
 *   grep audio_psram_buf build/procpu/zephyr/zephyr_final.map
 */
#define AUDIO_PSRAM_BASE  0x3C0B0000UL

#define SAMPLE_WIDTH   16
#define CHANNELS       2    /* DMA requires stereo; mic data is in left slot */
#define BLOCK_MS       20

/*
 * Set the PSRAM audio buffer address before calling pdm_init().
 * The address is received from procpu via IPM at startup so both cores
 * always agree on the linker-allocated location.
 */
void pdm_set_audio_buf(uint32_t addr);

/* Initialise the I2S driver. ipm_dev is used for log forwarding to procpu. */
int pdm_init(const struct device *ipm_dev);

/*
 * Capture PCM into PSRAM until stop_flag becomes true or the buffer is full.
 * Returns the number of bytes written (PCM only, excluding header).
 */
uint32_t pdm_record(volatile bool *stop_flag);
