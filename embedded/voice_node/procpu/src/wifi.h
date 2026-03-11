#pragma once

#include <stdbool.h>

/*
 * Register net_mgmt event listeners for WiFi connect/disconnect and DHCP bound.
 * Must be called before the network thread starts.
 */
void wifi_init(void);

/*
 * Block until NET_EVENT_IPV4_DHCP_BOUND fires (i.e. the user has run
 * "wifi connect" from the shell and DHCP has assigned an IP).  Returns
 * immediately if already bound.
 */
void wifi_wait_for_dhcp(void);

/*
 * Returns true while the WiFi interface is associated and has an IP.
 * Becomes false on NET_EVENT_WIFI_DISCONNECT_RESULT.
 */
bool wifi_is_connected(void);
