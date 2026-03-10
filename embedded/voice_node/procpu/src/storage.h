#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

int storage_init(void);
int storage_wifi_creds_read(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

#endif /* STORAGE_H */
