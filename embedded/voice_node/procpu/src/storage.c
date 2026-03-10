#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "storage.h"

LOG_MODULE_DECLARE(procpu, LOG_LEVEL_DBG);

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>

#define LFS_MOUNT_POINT "/lfs"
#define SSID_PATH       "/lfs/ssid"
#define PASS_PATH       "/lfs/pass"
#define MAX_SSID_LEN    64
#define MAX_PASS_LEN    64

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);

static struct fs_mount_t lfs_mnt = {
	.type        = FS_LITTLEFS,
	.fs_data     = &lfs_data,
	.mnt_point   = LFS_MOUNT_POINT,
	.storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
};

/* ── internal helpers ───────────────────────────────────────────────────── */

static int write_file(const char *path, const char *data)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	int ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (ret < 0) {
		return ret;
	}
	ssize_t n = fs_write(&f, data, strlen(data));

	fs_close(&f);
	return n < 0 ? (int)n : 0;
}

static int read_file(const char *path, char *buf, size_t len)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	int ret = fs_open(&f, path, FS_O_READ);

	if (ret < 0) {
		return ret;
	}
	ssize_t n = fs_read(&f, buf, len - 1);

	fs_close(&f);
	if (n < 0) {
		return (int)n;
	}
	buf[n] = '\0';
	return 0;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int storage_init(void)
{
	int ret = fs_mount(&lfs_mnt);

	if (ret < 0) {
		LOG_ERR("[C0] LittleFS mount failed: %d", ret);
		return ret;
	}
	LOG_INF("[C0] LittleFS mounted at %s", LFS_MOUNT_POINT);
	return 0;
}

int storage_wifi_creds_read(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
	int ret = read_file(SSID_PATH, ssid, ssid_len);

	if (ret < 0) {
		return ret;
	}
	return read_file(PASS_PATH, pass, pass_len);
}

/* ── shell commands ─────────────────────────────────────────────────────── */

static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	int ret = write_file(SSID_PATH, argv[1]);

	if (ret < 0) {
		shell_error(sh, "failed to write SSID: %d", ret);
		return ret;
	}
	ret = write_file(PASS_PATH, argv[2]);
	if (ret < 0) {
		shell_error(sh, "failed to write password: %d", ret);
		return ret;
	}
	shell_print(sh, "credentials saved");
	return 0;
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char ssid[MAX_SSID_LEN] = {0};
	char pass[MAX_PASS_LEN] = {0};
	int ret;

	ret = read_file(SSID_PATH, ssid, sizeof(ssid));
	if (ret < 0) {
		shell_error(sh, "no SSID stored (%d)", ret);
		return ret;
	}
	ret = read_file(PASS_PATH, pass, sizeof(pass));
	if (ret < 0) {
		shell_error(sh, "no password stored (%d)", ret);
		return ret;
	}
	shell_print(sh, "ssid: %s", ssid);
	shell_print(sh, "pass: %.*s***", (int)(strlen(pass) > 2 ? 2 : 0), pass);
	return 0;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	fs_unlink(SSID_PATH);
	fs_unlink(PASS_PATH);
	shell_print(sh, "credentials cleared");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wifi_creds_cmds,
	SHELL_CMD_ARG(set,   NULL, "<ssid> <pass>", cmd_set,   3, 0),
	SHELL_CMD_ARG(show,  NULL, "",              cmd_show,  1, 0),
	SHELL_CMD_ARG(clear, NULL, "",              cmd_clear, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(wifi_creds, &wifi_creds_cmds, "Manage WiFi credentials in LittleFS", NULL);

#else /* CONFIG_FILE_SYSTEM_LITTLEFS */

int storage_init(void)
{
	LOG_WRN("[C0] LittleFS not enabled — storage unavailable");
	return -ENOTSUP;
}

int storage_wifi_creds_read(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
	ARG_UNUSED(ssid); ARG_UNUSED(ssid_len);
	ARG_UNUSED(pass); ARG_UNUSED(pass_len);
	return -ENOTSUP;
}

#endif /* CONFIG_FILE_SYSTEM_LITTLEFS */
