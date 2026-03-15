#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/printk.h>

#include <errno.h>
#include <ff.h>
#include <hal/i2s_ll.h>
#include <hal/i2s_types.h>
#include <soc/i2s_struct.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SAMPLE_RATE   16000
#define SAMPLE_WIDTH  16
#define CHANNELS      2
#define BLOCK_MS      20
#define DMA_BLOCKS    4
#define RECORD_SECONDS 20
#define WAV_PATH      "/SD:/REC.WAV"
#define WAV_PATH_ALT  "/SD:REC.WAV"
#define DISK_NAME     "SD"
#define MOUNT_POINT   "/SD:"

#define BLOCK_SAMPLES (SAMPLE_RATE * BLOCK_MS / 1000)
#define BLOCK_BYTES   (BLOCK_SAMPLES * (SAMPLE_WIDTH / 8) * CHANNELS)

K_MEM_SLAB_DEFINE_STATIC(rx_slab, BLOCK_BYTES, DMA_BLOCKS, 4);
static FATFS fatfs;

static void put_le16(uint8_t *dst, uint16_t v)
{
	dst[0] = (uint8_t)(v & 0xFFU);
	dst[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void put_le32(uint8_t *dst, uint32_t v)
{
	dst[0] = (uint8_t)(v & 0xFFU);
	dst[1] = (uint8_t)((v >> 8) & 0xFFU);
	dst[2] = (uint8_t)((v >> 16) & 0xFFU);
	dst[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static void make_wav_header(uint8_t header[44], uint32_t data_bytes)
{
	const uint16_t channels = 1;
	const uint16_t bits_per_sample = 16;
	const uint16_t block_align = channels * (bits_per_sample / 8U);
	const uint32_t byte_rate = SAMPLE_RATE * (uint32_t)block_align;

	memset(header, 0, 44);
	memcpy(&header[0], "RIFF", 4);
	put_le32(&header[4], 36U + data_bytes);
	memcpy(&header[8], "WAVE", 4);
	memcpy(&header[12], "fmt ", 4);
	put_le32(&header[16], 16U); /* PCM fmt chunk size */
	put_le16(&header[20], 1U);  /* PCM format */
	put_le16(&header[22], channels);
	put_le32(&header[24], SAMPLE_RATE);
	put_le32(&header[28], byte_rate);
	put_le16(&header[32], block_align);
	put_le16(&header[34], bits_per_sample);
	memcpy(&header[36], "data", 4);
	put_le32(&header[40], data_bytes);
}

static void apply_pdm_patch(void)
{
	/* Re-apply RX config after i2s_trigger(START), which resets RX mode. */
	I2S0.rx_conf.rx_start = 0;

	i2s_ll_rx_reset(&I2S0);
	i2s_ll_rx_set_slave_mod(&I2S0, false);
	i2s_ll_rx_set_sample_bit(&I2S0, 16, 16);
	i2s_ll_rx_set_half_sample_bit(&I2S0, 16);
	i2s_ll_rx_enable_mono_mode(&I2S0, false);
	i2s_ll_rx_set_active_chan_mask(&I2S0, 0x03);
	i2s_ll_rx_enable_pdm(&I2S0);

	/* Be explicit about PDM mode bits and PDM2PCM settings. */
	I2S0.rx_conf.rx_tdm_en = 0;
	I2S0.rx_conf.rx_pdm_en = 1;
	I2S0.rx_conf.rx_pdm2pcm_en = 1;
	i2s_ll_rx_set_pdm_dsr(&I2S0, I2S_PDM_DSR_8S); /* 64x downsample */

	/* Keep sample format deterministic. */
	i2s_ll_rx_enable_big_endian(&I2S0, false);
	i2s_ll_rx_enable_left_align(&I2S0, false);
	I2S0.rx_conf.rx_bit_order = 0;
	I2S0.rx_conf.rx_pcm_conf = 0;
	I2S0.rx_conf.rx_pcm_bypass = 1;

	i2s_ll_rx_start(&I2S0);

	printk("rx_conf=0x%08X (tdm=%u pdm=%u pdm2pcm=%u dsr128=%u) rx_conf1=0x%08X clkm=0x%08X\n",
	       I2S0.rx_conf.val,
	       I2S0.rx_conf.rx_tdm_en,
	       I2S0.rx_conf.rx_pdm_en,
	       I2S0.rx_conf.rx_pdm2pcm_en,
	       I2S0.rx_conf.rx_pdm_sinc_dsr_16_en,
	       I2S0.rx_conf1.val,
	       I2S0.rx_clkm_conf.val);
}

static void dump_i2s_regs(void)
{
	printk("===== I2S REGISTER DUMP =====\n");
	printk("int_raw           = 0x%08X\n", I2S0.int_raw.val);
	printk("int_st            = 0x%08X\n", I2S0.int_st.val);
	printk("int_ena           = 0x%08X\n", I2S0.int_ena.val);
	printk("int_clr           = 0x%08X\n", I2S0.int_clr.val);
	printk("rx_conf           = 0x%08X\n", I2S0.rx_conf.val);
	printk("  rx_start        = %u\n", I2S0.rx_conf.rx_start);
	printk("  rx_mono         = %u\n", I2S0.rx_conf.rx_mono);
	printk("  rx_tdm_en       = %u\n", I2S0.rx_conf.rx_tdm_en);
	printk("  rx_pdm_en       = %u\n", I2S0.rx_conf.rx_pdm_en);
	printk("  rx_pdm2pcm_en   = %u\n", I2S0.rx_conf.rx_pdm2pcm_en);
	printk("  rx_update       = %u\n", I2S0.rx_conf.rx_update);
	printk("rx_conf1          = 0x%08X\n", I2S0.rx_conf1.val);
	printk("  rx_bits_mod     = %u\n", I2S0.rx_conf1.rx_bits_mod);
	printk("  rx_msb_shift    = %u\n", I2S0.rx_conf1.rx_msb_shift);
	printk("rx_clkm_conf      = 0x%08X\n", I2S0.rx_clkm_conf.val);
	printk("  rx_clkm_div_num = %u\n", I2S0.rx_clkm_conf.rx_clkm_div_num);
	printk("  rx_clk_active   = %u\n", I2S0.rx_clkm_conf.rx_clk_active);
	printk("  rx_clk_sel      = %u\n", I2S0.rx_clkm_conf.rx_clk_sel);
	printk("rx_clkm_div_conf  = 0x%08X\n", I2S0.rx_clkm_div_conf.val);
	printk("  div_x           = %u\n", I2S0.rx_clkm_div_conf.rx_clkm_div_x);
	printk("  div_y           = %u\n", I2S0.rx_clkm_div_conf.rx_clkm_div_y);
	printk("  div_z           = %u\n", I2S0.rx_clkm_div_conf.rx_clkm_div_z);
	printk("  yn1             = %u\n", I2S0.rx_clkm_div_conf.rx_clkm_div_yn1);
	printk("rx_tdm_ctrl       = 0x%08X\n", I2S0.rx_tdm_ctrl.val);
}

static void list_sd_root(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	int ret;

	fs_dir_t_init(&dir);
	ret = fs_opendir(&dir, MOUNT_POINT);
	if (ret < 0) {
		printk("fs_opendir(%s) failed: %d\n", MOUNT_POINT, ret);
		return;
	}

	printk("SD root listing (%s):\n", MOUNT_POINT);
	while (1) {
		ret = fs_readdir(&dir, &ent);
		if (ret < 0) {
			printk("  fs_readdir error: %d\n", ret);
			break;
		}
		if (ent.name[0] == '\0') {
			break;
		}
		printk("  %s  type=%u size=%zu\n",
		       ent.name, (unsigned)ent.type, ent.size);
	}

	(void)fs_closedir(&dir);
}

int main(void)
{
	const struct device *i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
	static struct fs_mount_t mp = {
		.type = FS_FATFS,
		.mnt_point = MOUNT_POINT,
		.fs_data = &fatfs,
		.storage_dev = (void *)DISK_NAME,
	};
	struct fs_file_t file;
	struct fs_file_t verify_file;
	uint8_t wav_header[44];
	int16_t mono_buf[BLOCK_SAMPLES];
	const char *active_wav_path = WAV_PATH;

	/* Arduino sample equivalent:
	 * setPinsPdmRx(42, 41) comes from testapp_mic.overlay pinctrl.
	 * begin(PDM_RX, 16000, 16-bit, mono) is approximated with stereo DMA,
	 * then we select the left slot for WAV mono.
	 */
	printk("testapp_mic booted\n");
	for (int i = 3; i > 0; i--) {
		printk("Starting capture in %d...\n", i);
		k_sleep(K_SECONDS(1));
	}
	printk("testapp_mic: record %d s WAV to SD (%s)\n", RECORD_SECONDS, WAV_PATH);

	if (!device_is_ready(i2s_dev)) {
		printk("Failed to initialize I2S: i2s0 not ready\n");
		return 0;
	}

	struct i2s_config cfg = {
		.word_size = SAMPLE_WIDTH,
		.channels = CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE * 2,
		.mem_slab = &rx_slab,
		.block_size = BLOCK_BYTES,
		.timeout = 1000,
	};

	int ret = i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);
	if (ret < 0) {
		printk("Failed to initialize I2S: i2s_configure=%d\n", ret);
		return 0;
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		printk("Failed to initialize I2S: trigger start=%d\n", ret);
		return 0;
	}

	apply_pdm_patch();

	ret = disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret != 0) {
		printk("Failed to initialize SD disk (%s): %d\n", DISK_NAME, ret);
		return 0;
	}

	ret = fs_mount(&mp);
	if (ret < 0) {
		printk("Failed to mount SD at %s: %d\n", MOUNT_POINT, ret);
		return 0;
	}
	printk("SD mounted at %s\n", MOUNT_POINT);

	fs_file_t_init(&file);
	ret = fs_open(&file, WAV_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		printk("Open failed for %s: %d, trying %s\n", WAV_PATH, ret, WAV_PATH_ALT);
		ret = fs_open(&file, WAV_PATH_ALT, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
		if (ret < 0) {
			printk("Failed to open WAV file (%s / %s): %d\n",
			       WAV_PATH, WAV_PATH_ALT, ret);
			(void)fs_unmount(&mp);
			return 0;
		}
		active_wav_path = WAV_PATH_ALT;
	}

	make_wav_header(wav_header, 0);
	ret = fs_write(&file, wav_header, sizeof(wav_header));
	if (ret < 0) {
		printk("Failed to write WAV header: %d\n", ret);
		(void)fs_close(&file);
		(void)fs_unmount(&mp);
		return 0;
	}

	const uint32_t target_mono_samples = SAMPLE_RATE * RECORD_SECONDS;
	uint32_t written_mono_samples = 0;
	uint32_t block_idx = 0;
	int64_t next_reg_dump_ms = k_uptime_get() + 2000;

	printk("Recording...\n");
	while (written_mono_samples < target_mono_samples) {
		void *block = NULL;
		size_t block_size = 0;

		ret = i2s_read(i2s_dev, &block, &block_size);
		if (ret == -EAGAIN) {
			continue;
		}
		if (ret < 0) {
			printk("i2s_read error: %d\n", ret);
			k_sleep(K_MSEC(10));
			continue;
		}
		if (block == NULL || block_size == 0) {
			continue;
		}

		/* Convert interleaved stereo DMA block to mono WAV by taking left slot. */
		int16_t *samples = (int16_t *)block;
		size_t count = block_size / sizeof(int16_t);
		size_t stereo_pairs = count / 2U;

		if (stereo_pairs > BLOCK_SAMPLES) {
			stereo_pairs = BLOCK_SAMPLES;
		}
		if (stereo_pairs > (target_mono_samples - written_mono_samples)) {
			stereo_pairs = target_mono_samples - written_mono_samples;
		}

		for (size_t i = 0; i < stereo_pairs; i++) {
			mono_buf[i] = samples[2U * i];
		}

		ssize_t wr = fs_write(&file, mono_buf, stereo_pairs * sizeof(int16_t));
		if (wr < 0) {
			printk("fs_write audio failed: %d\n", (int)wr);
			k_mem_slab_free(&rx_slab, block);
			break;
		}
		if ((size_t)wr != stereo_pairs * sizeof(int16_t)) {
			printk("Short write: %d/%u\n", (int)wr,
			       (unsigned)(stereo_pairs * sizeof(int16_t)));
			k_mem_slab_free(&rx_slab, block);
			break;
		}
		written_mono_samples += (uint32_t)stereo_pairs;

		int64_t l_sum = 0, l_sum_sq = 0;
		int64_t r_sum = 0, r_sum_sq = 0;
		uint32_t n = 0;
		int16_t l_min = 32767, l_max = -32768;
		int16_t r_min = 32767, r_max = -32768;
		int16_t first_non_trivial = 0;
		bool have_first = false;

		for (size_t i = 0; i + 1 < count; i += 2) {
			int32_t l = samples[i];
			int32_t r = samples[i + 1];

			l_sum += l;
			l_sum_sq += (int64_t)l * (int64_t)l;
			r_sum += r;
			r_sum_sq += (int64_t)r * (int64_t)r;
			n++;

			if (l < l_min) {
				l_min = (int16_t)l;
			}
			if (l > l_max) {
				l_max = (int16_t)l;
			}
			if (r < r_min) {
				r_min = (int16_t)r;
			}
			if (r > r_max) {
				r_max = (int16_t)r;
			}

			if (!have_first && l != 0 && l != -1 && l != 1) {
				first_non_trivial = (int16_t)l;
				have_first = true;
			}
		}

		/* Print one line every ~200ms to monitor signal stats while recording. */
		if ((block_idx % 10U) == 0U && n > 0U) {
			int32_t l_mean = (int32_t)(l_sum / (int64_t)n);
			int32_t r_mean = (int32_t)(r_sum / (int64_t)n);
			uint32_t l_rms2 = (uint32_t)(l_sum_sq / (int64_t)n);
			uint32_t r_rms2 = (uint32_t)(r_sum_sq / (int64_t)n);
			int32_t l_p2p = (int32_t)l_max - (int32_t)l_min;
			int32_t r_p2p = (int32_t)r_max - (int32_t)r_min;
			int16_t s16 = have_first ? first_non_trivial : 0;
			uint16_t u16 = (uint16_t)s16;

			printk("blk=%u n=%u L(mean=%d rms2=%u min=%d max=%d p2p=%d) R(mean=%d rms2=%u min=%d max=%d p2p=%d) sample_s16=%d sample_u16=%u sample_hex=0x%04x\n",
			       block_idx, n,
			       l_mean, l_rms2, l_min, l_max, l_p2p,
			       r_mean, r_rms2, r_min, r_max, r_p2p,
			       s16, u16, u16);
		}
		block_idx++;

		int64_t now_ms = k_uptime_get();
		if (now_ms >= next_reg_dump_ms) {
			dump_i2s_regs();
			next_reg_dump_ms = now_ms + 2000;
		}

		k_mem_slab_free(&rx_slab, block);
	}

	(void)i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);

	uint32_t data_bytes = written_mono_samples * sizeof(int16_t);
	make_wav_header(wav_header, data_bytes);
	ret = fs_seek(&file, 0, FS_SEEK_SET);
	if (ret < 0) {
		printk("Failed to seek for WAV finalize: %d\n", ret);
	} else {
		ret = fs_write(&file, wav_header, sizeof(wav_header));
		if (ret < 0) {
			printk("Failed to finalize WAV header: %d\n", ret);
		}
	}
	ret = fs_sync(&file);
	if (ret < 0) {
		printk("fs_sync failed: %d\n", ret);
	}

	(void)fs_close(&file);

	ret = disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);
	if (ret < 0) {
		printk("DISK_IOCTL_CTRL_SYNC failed: %d\n", ret);
	}

	fs_file_t_init(&verify_file);
	ret = fs_open(&verify_file, active_wav_path, FS_O_READ);
	if (ret < 0) {
		printk("Post-write verify open failed for %s: %d\n", active_wav_path, ret);
	} else {
		uint8_t verify_hdr[44];
		ssize_t rd = fs_read(&verify_file, verify_hdr, sizeof(verify_hdr));
		printk("Post-write verify read: %d bytes from %s\n", (int)rd, active_wav_path);
		(void)fs_close(&verify_file);
	}

	list_sd_root();
	(void)fs_unmount(&mp);
	printk("WAV saved: %s (%u bytes PCM, %u samples)\n",
	       active_wav_path, data_bytes, written_mono_samples);

	while (1) {
		printk("idle: done\n");
		k_sleep(K_SECONDS(2));
	}

	return 0;
}
