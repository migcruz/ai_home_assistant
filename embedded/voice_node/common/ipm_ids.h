#pragma once

/*
 * IPM message IDs shared between appcpu and procpu.
 * This is the complete IPC contract between the two cores.
 *
 * Direction      ID  Payload
 * ──────────────────────────────────────────────────────────────────────────
 * appcpu→procpu  0   char[]    log string (null-terminated)
 * procpu→appcpu  1   uint8_t   command: CMD_START / CMD_STOP
 * appcpu→procpu  2   uint8_t   recording done (byte_count in struct)
 * procpu→appcpu  3   uint32_t  audio record buffer PSRAM address (boot only)
 * procpu→appcpu  4   uint8_t   play signal — WAV ready in play_psram_buf
 * procpu→appcpu  5   uint32_t  play buffer PSRAM address (boot only)
 */

#define IPM_ID_LOG         0U
#define IPM_ID_CMD         1U
#define IPM_ID_DONE        2U
#define IPM_ID_BUFADDR     3U
#define IPM_ID_PLAY        4U
#define IPM_ID_PLAYBUFADDR 5U

#define CMD_STOP   0U
#define CMD_START  1U
