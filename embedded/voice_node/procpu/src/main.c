/*
 * procpu — PRO CPU (Core 0)
 *
 * Drives the BOOT button (interrupt-driven) and waits for audio done from
 * appcpu. On done, reads PCM from shared PSRAM, builds a WAV file, strips
 * the silent right channel, and streams mono PCM to the voice service.
 *
 * IPM message IDs (shared with appcpu/src/main.c):
 *   id=0  appcpu → procpu   log string
 *   id=1  procpu → appcpu   command: uint8_t 1=start, 0=stop
 *   id=2  appcpu → procpu   done:    signal only (byte_count lives in struct audio_shared)
 *   id=3  procpu → appcpu   buf_addr: uint32_t address of audio_psram_buf
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <stdint.h>
#include <string.h>
#include "storage.h"
#include "button.h"
#include "audio.h"
#include "wifi.h"
#include "websocket.h"
#include "ipm_ids.h"
/* #include "sd_wav.h" — re-enable with SD/FAT Kconfig for audio debug */

LOG_MODULE_REGISTER(procpu, LOG_LEVEL_DBG);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

// #define SD_DISK_NAME "SD"
// #define SD_MOUNT_POINT "/SD:"
// #define SD_WAV_PATH "/SD:/VOICEDBG.WAV"
// #define SD_WAV_PATH_ALT "/SD:VOICEDBG.WAV"

static const struct device *ipm_dev;

static volatile bool audio_ready = false;

static void ipm_rx_cb(const struct device *dev, void *ctx,
		      uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ctx);

	if (id == IPM_ID_LOG) {
		LOG_INF("%s", (const char *)data);
	} else if (id == IPM_ID_DONE) {
		audio_ready = true;
	}
}

/*
 * Build a 44-byte standard WAV header for 16kHz, 16-bit, mono PCM.
 * data_bytes is the number of raw PCM bytes that follow.
 */
static void build_wav_header(uint8_t *hdr, uint32_t data_bytes)
{
	uint32_t riff_size   = 36 + data_bytes;
	uint32_t sample_rate = AUDIO_SAMPLE_RATE;
	uint32_t byte_rate   = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8);
	uint16_t audio_fmt   = 1;     /* PCM */
	uint16_t channels    = AUDIO_CHANNELS;
	uint16_t block_align = AUDIO_CHANNELS * (AUDIO_BITS / 8);
	uint16_t bits        = AUDIO_BITS;
	uint32_t fmt_size    = 16;

	memcpy(hdr +  0, "RIFF", 4);  memcpy(hdr +  4, &riff_size,   4);
	memcpy(hdr +  8, "WAVE", 4);  memcpy(hdr + 12, "fmt ", 4);
	memcpy(hdr + 16, &fmt_size,   4);  memcpy(hdr + 20, &audio_fmt,   2);
	memcpy(hdr + 22, &channels,   2);  memcpy(hdr + 24, &sample_rate, 4);
	memcpy(hdr + 28, &byte_rate,  4);  memcpy(hdr + 32, &block_align, 2);
	memcpy(hdr + 34, &bits,       2);  memcpy(hdr + 36, "data", 4);
	memcpy(hdr + 40, &data_bytes, 4);
}

/*
 * Send the PSRAM audio buffer to the voice service as a WAV file.
 *
 * PSRAM layout: stereo-interleaved 16-bit PCM at 16kHz.
 *   [L0, R0, L1, R1, ...]  — L = mic (left), R = zeros (PDM mono mic)
 * We strip the right channel and send mono left-channel PCM only.
 *
 * Assumes log_audio_samples() has already been called (invalidates cache).
 */
#define WAV_HEADER_LEN   44
/* Must not exceed CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN (4096). */
#define PCM_CHUNK_BYTES  4096

static int send_wav_header_ws(uint32_t mono_bytes)
{
	uint8_t hdr[WAV_HEADER_LEN];

	build_wav_header(hdr, mono_bytes);
	if (ws_send_binary(hdr, WAV_HEADER_LEN) < 0) {
		LOG_ERR("[C0] WAV header send failed");
		return -1;
	}

	return 0;
}

static void send_audio_as_wav(void)
{
	const struct audio_shared *shdr = (const struct audio_shared *)audio_buf;

	/* Invalidate the header first so we can read byte_count, then the PCM. */
	sys_cache_data_invd_range((void *)audio_buf, sizeof(struct audio_shared));
	uint32_t mono_bytes = shdr->byte_count;
	sys_cache_data_invd_range((void *)shdr->pcm, mono_bytes);

	/* Debug: write mono PCM to SD card as a WAV for offline inspection. */
	// sd_wav_write("/SD:/PROCREC.WAV", shdr);

	if (!ws_is_connected()) {
		LOG_WRN("[C0] WS not connected — dropping audio (%u bytes)",
			mono_bytes);
		return;
	}

	if (send_wav_header_ws(mono_bytes) < 0) {
		return;
	}

	const uint8_t *pcm = (const uint8_t *)shdr->pcm;
	uint32_t remaining = mono_bytes;

	while (remaining > 0) {
		uint32_t chunk = remaining > PCM_CHUNK_BYTES ? PCM_CHUNK_BYTES : remaining;

		if (ws_send_binary(pcm, chunk) < 0) {
			LOG_ERR("[C0] PCM chunk send failed");
			return;
		}
		pcm += chunk;
		remaining -= chunk;
	}

	if (ws_send_text("{\"type\":\"end_audio\"}") < 0) {
		LOG_ERR("[C0] end_audio send failed");
		return;
	}

	LOG_INF("[C0] sent %u bytes WAV mono to server", mono_bytes);
}

int main(void)
{
	LOG_INF("[C0] procpu starting");

	k_sleep(K_MSEC(500));
	storage_init();
	wifi_init();

	ipm_dev = DEVICE_DT_GET(DT_NODELABEL(ipm0));
	if (!device_is_ready(ipm_dev)) {
		LOG_ERR("[C0] IPM not ready");
		return -1;
	}
	ipm_register_callback(ipm_dev, ipm_rx_cb, NULL);

	/* Tell appcpu the exact PSRAM address of the shared audio buffer.
	 * appcpu registers its IPM callback before its startup sleep, so it
	 * will be ready to receive this by the time we send it here. */
	uint32_t buf_addr = (uint32_t)(uintptr_t)audio_psram_buf;
	ipm_send(ipm_dev, 1, IPM_ID_BUFADDR, &buf_addr, sizeof(buf_addr));
	LOG_INF("[C0] sent audio buf addr 0x%08X to appcpu", buf_addr);

	uint32_t play_addr = (uint32_t)(uintptr_t)play_buf;
	ipm_send(ipm_dev, 1, IPM_ID_PLAYBUFADDR, &play_addr, sizeof(play_addr));
	LOG_INF("[C0] sent play buf addr 0x%08X to appcpu", play_addr);

	if (button_init(ipm_dev) < 0) {
		return -1;
	}

	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	net_thread_start();

	LOG_INF("[C0] ready — hold BOOT button to record; "
		"connect WiFi via: wifi connect -s <SSID> -p <PSK> -k 1");

	uint32_t blink_ticks = 0;

	while (1) {
		if (audio_ready) {
			audio_ready = false;
			LOG_INF("[C0] audio ready in PSRAM");
			send_audio_as_wav();
		}

		if (++blink_ticks >= 25) {
			gpio_pin_toggle_dt(&led);
			blink_ticks = 0;
		}

		k_sleep(K_MSEC(20));
	}

	return 0;
}
