#pragma once

#include <stdint.h>
#include <stddef.h>
#include "audio_shared.h"

/*
 * SD card write utilities — procpu only (SPI2 + FatFS on procpu DT overlay).
 *
 * Disk and mount point defaults can be overridden before including this
 * header by defining SD_WAV_DISK_NAME / SD_WAV_MOUNT_POINT.
 */

#ifndef SD_WAV_DISK_NAME
#define SD_WAV_DISK_NAME   "SD"
#endif

#ifndef SD_WAV_MOUNT_POINT
#define SD_WAV_MOUNT_POINT "/SD:"
#endif

/*
 * A contiguous data segment passed to sd_write_file().
 */
struct sd_seg {
	const void *data;
	size_t      len;
};

/*
 * Write one or more data segments to a file on the SD card.
 *
 * Mounts the card, creates/truncates the file at path, writes all segments
 * in order, syncs, closes, and unmounts.  A single mount/unmount cycle
 * covers all segments regardless of how many there are.
 *
 * Returns 0 on success, negative errno on failure.
 */
int sd_write_file(const char *path, const struct sd_seg *segs, size_t n_segs);

/*
 * Build a standard 44-byte PCM WAV header into hdr[].
 * hdr must point to at least 44 bytes.
 */
void sd_wav_header(uint8_t hdr[44], uint32_t data_bytes,
		   uint16_t channels, uint32_t sample_rate);

/*
 * Convenience: write a captured audio buffer (struct audio_shared) to the SD
 * card as a WAV file.  Validates the magic, builds the 44-byte header, then
 * calls sd_write_file() with two segments: header + shdr->pcm[].
 *
 * Returns 0 on success, negative errno on failure.
 */
int sd_wav_write(const char *path, const struct audio_shared *shdr);
