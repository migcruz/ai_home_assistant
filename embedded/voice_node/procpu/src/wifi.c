/*
 * wifi.c — passive WiFi event listener for procpu
 *
 * Does NOT initiate WiFi connections.  The user connects via the Zephyr shell:
 *   uart:~$ wifi connect -s <SSID> -p <PSK> -k 1
 *
 * On successful DHCP assignment, posts dhcp_sem to unblock net_thread.
 * On disconnect, resets dhcp_sem so net_thread waits again on reconnect.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi.h"

LOG_MODULE_DECLARE(procpu, LOG_LEVEL_DBG);

static K_SEM_DEFINE(dhcp_sem, 0, 1);
static volatile bool wifi_connected;

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback dhcp_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status =
			(const struct wifi_status *)cb->info;

		if (status && status->status == 0) {
			LOG_INF("[C0] WiFi associated — waiting for DHCP");
			wifi_connected = true;
		} else {
			LOG_WRN("[C0] WiFi association failed: %d",
				status ? status->status : -1);
		}
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_INF("[C0] WiFi disconnected");
		wifi_connected = false;
		/* Reset so net_thread blocks on next wifi_wait_for_dhcp() */
		k_sem_reset(&dhcp_sem);
	}
}

static void dhcp_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
		LOG_INF("[C0] DHCP bound — IP assigned, WebSocket will connect");
		k_sem_give(&dhcp_sem);
	}
}

void wifi_init(void)
{
	net_mgmt_init_event_callback(
		&wifi_cb, wifi_event_handler,
		NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(
		&dhcp_cb, dhcp_event_handler,
		NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&dhcp_cb);

	LOG_INF("[C0] WiFi listener ready — connect via shell: "
		"wifi connect -s <SSID> -p <PSK> -k 1");
}

void wifi_wait_for_dhcp(void)
{
	k_sem_take(&dhcp_sem, K_FOREVER);
}

bool wifi_is_connected(void)
{
	return wifi_connected;
}
