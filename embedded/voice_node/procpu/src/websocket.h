#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Server coordinates ────────────────────────────────────────────────────
 * Override in prj.conf (or credentials.conf) if vulcan.local mDNS does not
 * resolve — set VOICE_SERVER_HOST to the server's LAN IP instead.
 */
#ifndef VOICE_SERVER_HOST
#define VOICE_SERVER_HOST  "vulcan.local"
#endif
#ifndef VOICE_SERVER_PORT
#define VOICE_SERVER_PORT  443
#endif
#define VOICE_SERVER_PATH  "/voice/converse"

/*
 * Spawn the net_thread.  Must be called after wifi_init().
 * The thread blocks on wifi_wait_for_dhcp(), then opens the WebSocket and
 * enters the receive loop.  Retries automatically on disconnect.
 */
void net_thread_start(void);

/* True while the WebSocket handshake has succeeded and the connection is open. */
bool ws_is_connected(void);

/*
 * Send a UTF-8 text frame.  Returns 0 on success, negative errno on failure.
 * Returns -ENOTCONN if the WebSocket is not currently open.
 */
int ws_send_text(const char *msg);

/*
 * Send a binary frame.  Returns 0 on success, negative errno on failure.
 * Returns -ENOTCONN if the WebSocket is not currently open.
 *
 * TODO (Milestone 3): called from main thread after audio capture —
 * add a mutex here if concurrent sends become a concern.
 */
int ws_send_binary(const uint8_t *data, size_t len);
