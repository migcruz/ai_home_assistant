#include "sd_wav.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <ff.h>

/* Place in PSRAM — sizeof(FATFS) ≈ 560 bytes, too large for tight DRAM budget. */
static FATFS sd_wav_fatfs __attribute__((section(".ext_ram.bss")));

/* ── Generic SD write ──────────────────────────────────────────────────────── */

int sd_write_file(const char *path, const struct sd_seg *segs, size_t n_segs)
{
	static struct fs_mount_t mp = {
		.type        = FS_FATFS,
		.mnt_point   = SD_WAV_MOUNT_POINT,
		.fs_data     = &sd_wav_fatfs,
		.storage_dev = (void *)SD_WAV_DISK_NAME,
	};
	struct fs_file_t f;
	int ret;

	ret = disk_access_ioctl(SD_WAV_DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret < 0) {
		printk("sd: disk init failed: %d\n", ret);
		return ret;
	}

	ret = fs_mount(&mp);
	if (ret < 0) {
		printk("sd: mount failed at %s: %d\n", SD_WAV_MOUNT_POINT, ret);
		return ret;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		printk("sd: open failed (%s): %d\n", path, ret);
		(void)fs_unmount(&mp);
		return ret;
	}

	for (size_t i = 0; i < n_segs; i++) {
		ssize_t wr = fs_write(&f, segs[i].data, segs[i].len);

		if (wr < 0 || (size_t)wr != segs[i].len) {
			ret = (wr < 0) ? (int)wr : -EIO;
			printk("sd: write failed (seg %zu): %d\n", i, ret);
			(void)fs_close(&f);
			(void)fs_unmount(&mp);
			return ret;
		}
	}

	(void)fs_sync(&f);
	(void)disk_access_ioctl(SD_WAV_DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);
	(void)fs_close(&f);
	(void)fs_unmount(&mp);

	printk("sd: saved %s\n", path);
	return 0;
}

/* ── WAV helpers ───────────────────────────────────────────────────────────── */

void sd_wav_header(uint8_t hdr[44], uint32_t data_bytes,
		   uint16_t channels, uint32_t sample_rate)
{
	uint16_t bits        = 16;
	uint16_t block_align = channels * (bits / 8);
	uint32_t byte_rate   = sample_rate * (uint32_t)block_align;
	uint16_t audio_fmt   = 1; /* PCM */
	uint32_t fmt_size    = 16;
	uint32_t riff_size   = 36 + data_bytes;

	memcpy(hdr +  0, "RIFF", 4);  memcpy(hdr +  4, &riff_size,   4);
	memcpy(hdr +  8, "WAVE", 4);  memcpy(hdr + 12, "fmt ", 4);
	memcpy(hdr + 16, &fmt_size,   4);  memcpy(hdr + 20, &audio_fmt,   2);
	memcpy(hdr + 22, &channels,   2);  memcpy(hdr + 24, &sample_rate, 4);
	memcpy(hdr + 28, &byte_rate,  4);  memcpy(hdr + 32, &block_align, 2);
	memcpy(hdr + 34, &bits,       2);  memcpy(hdr + 36, "data", 4);
	memcpy(hdr + 40, &data_bytes, 4);
}

int sd_wav_write(const char *path, const struct audio_shared *shdr)
{
	if (shdr->magic != AUDIO_SHARED_MAGIC) {
		printk("sd_wav: bad magic 0x%08X\n", shdr->magic);
		return -EINVAL;
	}

	if (shdr->byte_count == 0U) {
		printk("sd_wav: empty capture, skipping\n");
		return 0;
	}

	uint8_t hdr[44];

	sd_wav_header(hdr, shdr->byte_count, shdr->channels, shdr->sample_rate);

	struct sd_seg segs[] = {
		{ hdr,       sizeof(hdr)      },
		{ shdr->pcm, shdr->byte_count },
	};

	return sd_write_file(path, segs, ARRAY_SIZE(segs));
}
