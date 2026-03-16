#pragma once

#include <stdint.h>
#include "audio_shared.h"

/*
 * SD card WAV writing utility — usable from appcpu or procpu.
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
 * Write the audio in shdr to an SD card WAV file at the given path.
 *
 * Mounts the SD card, writes a 44-byte WAV header derived from shdr
 * metadata, writes shdr->pcm[0..shdr->byte_count-1], then unmounts.
 *
 * Returns 0 on success, negative errno on failure.
 */
int sd_wav_write(const char *path, const struct audio_shared *shdr);
