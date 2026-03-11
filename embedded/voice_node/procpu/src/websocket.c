/*
 * websocket.c — TLS WebSocket client for procpu (Milestone 2)
 *
 * net_thread flow:
 *   wifi_wait_for_dhcp()          block until shell "wifi connect" completes
 *   tls_socket_connect()          DNS lookup + TLS handshake (VERIFY_NONE)
 *   websocket_connect()           HTTP 101 upgrade → ws_fd
 *   ws_send_text(config_msg)      announce wav/16kHz capability
 *   recv_loop()                   log all server messages; break on error
 *   k_sleep(3s)                   back-off before retry
 *   if WiFi dropped → wifi_wait_for_dhcp() again
 *
 * TLS peer verification is disabled (TLS_PEER_VERIFY_NONE) — the connection
 * is still encrypted; we simply skip certificate validation.  Proper cert
 * pinning via LittleFS is a future TODO (Milestone 8 — provisioning).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/websocket.h>
#include <zephyr/net/http/client.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "wifi.h"
#include "websocket.h"

LOG_MODULE_DECLARE(procpu, LOG_LEVEL_DBG);

/* ── Thread ──────────────────────────────────────────────────────────────── */
#define NET_STACK_SIZE  16384
#define NET_THREAD_PRIO 5

/*
 * Place the net_thread stack in PSRAM (.ext_ram.bss) instead of DRAM
 * (.noinit.stacks).  K_THREAD_STACK_DEFINE would put 16KB in DRAM, which is
 * enough to push the DRAM segment over budget when mbedTLS is enabled.
 * Xtensa requires 16-byte stack alignment; no MPU guard page is needed since
 * CONFIG_USERSPACE is not enabled.
 */
static uint8_t net_stack_mem[NET_STACK_SIZE]
	__attribute__((section(".ext_ram.bss"))) __aligned(16);
static struct k_thread net_thread_data;

/* ── Shared state ────────────────────────────────────────────────────────── */
static volatile int  ws_fd      = -1;
static volatile bool connected;

/* ── Buffers (stack-allocated inside net_thread — fits in 16KB) ─────────── */
#define HTTP_BUF_SIZE  512   /* HTTP 101 upgrade response */
#define RECV_BUF_SIZE  2048  /* incoming WebSocket frames  */

/* Config message sent immediately after the WebSocket upgrade */
#define CONFIG_MSG \
	"{\"type\":\"config\",\"tts\":true," \
	"\"audio_format\":\"wav\",\"sample_rate\":16000}"

/* ── TLS socket + TCP connect ────────────────────────────────────────────── */

static int tls_socket_connect(void)
{
	char port_str[6];
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;

	snprintf(port_str, sizeof(port_str), "%d", VOICE_SERVER_PORT);

	int ret = zsock_getaddrinfo(VOICE_SERVER_HOST, port_str, &hints, &res);

	if (ret != 0) {
		LOG_ERR("[C0] DNS lookup for %s failed: %d",
			VOICE_SERVER_HOST, ret);
		return -EHOSTUNREACH;
	}

	int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);

	if (sock < 0) {
		LOG_ERR("[C0] socket() failed: %d", errno);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	/* Disable peer certificate verification — self-signed LAN cert.
	 * TODO (Milestone 8): load /lfs/ca.crt and use TLS_PEER_VERIFY_REQUIRED
	 */
	int verify = TLS_PEER_VERIFY_NONE;

	ret = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
			       &verify, sizeof(verify));
	if (ret < 0) {
		LOG_ERR("[C0] setsockopt TLS_PEER_VERIFY failed: %d", errno);
		zsock_close(sock);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	LOG_INF("[C0] Connecting to %s:%d (TLS)...",
		VOICE_SERVER_HOST, VOICE_SERVER_PORT);

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);

	if (ret < 0) {
		LOG_ERR("[C0] connect() failed: %d", errno);
		zsock_close(sock);
		return -errno;
	}

	LOG_INF("[C0] TLS handshake complete");
	return sock;
}

/* ── WebSocket upgrade + recv loop ──────────────────────────────────────── */

static int ws_connect_and_run(void)
{
	int tcp_sock = tls_socket_connect();

	if (tcp_sock < 0) {
		return tcp_sock;
	}

	uint8_t http_buf[HTTP_BUF_SIZE];
	struct websocket_request req = {
		.host        = VOICE_SERVER_HOST,
		.url         = VOICE_SERVER_PATH,
		.cb          = NULL,
		.tmp_buf     = http_buf,
		.tmp_buf_len = sizeof(http_buf),
	};

	/* websocket_connect() takes ownership of tcp_sock */
	int wfd = websocket_connect(tcp_sock, &req, 5000, NULL);

	if (wfd < 0) {
		LOG_ERR("[C0] WebSocket upgrade failed: %d", wfd);
		zsock_close(tcp_sock);
		return wfd;
	}

	ws_fd     = wfd;
	connected = true;
	LOG_INF("[C0] WebSocket open — wss://%s%s",
		VOICE_SERVER_HOST, VOICE_SERVER_PATH);

	/* Announce audio format so the server parses WAV instead of webm */
	ws_send_text(CONFIG_MSG);

	/* ── Receive loop ─────────────────────────────────────────────────── */
	uint8_t  buf[RECV_BUF_SIZE];
	uint32_t msg_type;
	uint64_t remaining;

	while (1) {
		int ret = websocket_recv_msg(wfd, buf, sizeof(buf) - 1,
					     &msg_type, &remaining,
					     500);

		if (ret == -EAGAIN || ret == -ETIMEDOUT) {
			/* Timeout — nothing received, keep looping */
			continue;
		}

		if (ret < 0) {
			LOG_ERR("[C0] WS recv error: %d", ret);
			break;
		}

		if (msg_type & WEBSOCKET_FLAG_CLOSE) {
			LOG_INF("[C0] WS close received");
			break;
		}

		if (msg_type & WEBSOCKET_FLAG_TEXT) {
			buf[ret] = '\0';
			LOG_INF("[C0] WS rx text: %s", (char *)buf);
		} else if (msg_type & WEBSOCKET_FLAG_BINARY) {
			LOG_INF("[C0] WS rx binary: %d bytes "
				"(remaining: %lld)", ret, remaining);
			/* TODO (Milestone 4): queue WAV frame for I2S playback */
		}
	}

	ws_fd     = -1;
	connected = false;
	websocket_disconnect(wfd);
	return -EIO;
}

/* ── net_thread ──────────────────────────────────────────────────────────── */

static void net_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("[C0] net_thread started — waiting for WiFi + DHCP");

	while (1) {
		if (!wifi_is_connected()) {
			LOG_INF("[C0] Waiting for DHCP "
				"(connect via shell: wifi connect -s <SSID> "
				"-p <PSK> -k 1)");
			wifi_wait_for_dhcp();
		}

		int ret = ws_connect_and_run();

		LOG_WRN("[C0] WS session ended (%d), retrying in 3s...", ret);
		k_sleep(K_SECONDS(3));
	}
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void net_thread_start(void)
{
	k_thread_create(&net_thread_data,
			(k_thread_stack_t *)net_stack_mem, NET_STACK_SIZE,
			net_thread_fn, NULL, NULL, NULL,
			NET_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&net_thread_data, "net_thread");
}

bool ws_is_connected(void)
{
	return connected;
}

int ws_send_text(const char *msg)
{
	int fd = ws_fd;

	if (fd < 0) {
		return -ENOTCONN;
	}

	int ret = websocket_send_msg(fd,
				     (const uint8_t *)msg, strlen(msg),
				     WEBSOCKET_OPCODE_DATA_TEXT,
				     true, true, 5000);

	return ret < 0 ? ret : 0;
}

int ws_send_binary(const uint8_t *data, size_t len)
{
	int fd = ws_fd;

	if (fd < 0) {
		return -ENOTCONN;
	}

	int ret = websocket_send_msg(fd,
				     data, len,
				     WEBSOCKET_OPCODE_DATA_BINARY,
				     true, true, 5000);

	return ret < 0 ? ret : 0;
}
