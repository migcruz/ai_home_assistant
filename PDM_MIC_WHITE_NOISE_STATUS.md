# PDM Mic White Noise Debug Status (Voice Node)

## Summary

**RESOLVED.** The white noise issue has been fixed. Both `testapp_mic` and `voice_node` now produce clear, intelligible audio.

## Root Cause

**`AUDIO_PSRAM_BASE` was set to `0x3C000000`, which is flash-mapped rodata, not PSRAM.**

On ESP32-S3, flash rodata and PSRAM share the same data cache bus starting at `0x3C000000`. The Zephyr linker places a `.ext_ram.dummy` section first to skip past flash rodata pages before the actual PSRAM region begins. In our build:

```
.ext_ram.dummy   0x3C000000   0xB0000   <- flash rodata (704 KB)
.ext_ram.data    0x3C0B0000             <- actual PSRAM starts here
```

Writes to `0x3C000000` entered the CPU's write-back data cache but could never persist to physical storage (the backing memory is read-only flash). On the same core, short-lived reads could hit the cached values, which is why `log_block_stats()` showed valid-looking audio from DMA blocks during recording. But after cache eviction or flush, reads returned flash content — random-looking data that sounds like white noise.

This explains every symptom:
- **testapp_mic worked**: it wrote mono PCM to a DRAM stack buffer (`int16_t mono_buf[BLOCK_SAMPLES]`), then directly to SD. PSRAM was never involved.
- **voice_node per-block SD streaming worked**: the old approach wrote DMA block data to SD immediately from DRAM slab buffers, before PSRAM was involved.
- **voice_node PSRAM path produced white noise**: mono PCM was written to `0x3C000000` (flash, not PSRAM), then read back for SD/WebSocket — returning flash content.
- **L/R correlation ~ 1.000**: the data was mono (consecutive samples from the same mic), not stereo. The correlation metric was measuring adjacent mono samples, not L/R channels.

## Fix Applied (2026-03-16)

1. **procpu `audio.c`**: Allocated `audio_psram_buf[512KB]` in `.ext_ram.bss` section, which the linker places in actual PSRAM at `0x3C0B0000`.
2. **procpu `audio.h`**: Removed hardcoded `AUDIO_PSRAM_BASE`; exports `audio_psram_buf[]`.
3. **appcpu `pdm.h`**: Updated `AUDIO_PSRAM_BASE` from `0x3C000000` to `0x3C0B0000` to match the linker-allocated address.

After rebuilding, `APPREC.WAV` on SD card has clear audio.

~~**Important**: If procpu's flash rodata size changes significantly, `_ext_ram_start` may shift — this is now handled dynamically via IPM (see below).~~

## ESP32-S3 PSRAM Address Map (for reference)

```
0x3C000000 +-----------------------+
           | .ext_ram.dummy        |  Flash rodata pages (size varies by build)
           | (flash-mapped, R/O)   |
0x3C0B0000 +-----------------------+  <- _ext_ram_start (actual PSRAM)
           | .ext_ram.bss          |  audio_psram_buf (512KB), net_thread stack, etc.
0x3C0C3A60 +-----------------------+
           | SPIRAM heap           |  CONFIG_ESP_SPIRAM_HEAP_SIZE
0x3C1C3A60 +-----------------------+  <- _ext_ram_end
           | (unmapped)            |
0x3C800000 +-----------------------+  End of 8MB PSRAM address window
```

## Runtime Address Negotiation via IPM (2026-03-16)

The hardcoded `AUDIO_PSRAM_BASE` address in `appcpu/src/pdm.h` is fragile — if flash rodata grows enough to cross a page boundary, `_ext_ram_start` shifts and the two images would disagree on the buffer location.

**Fix**: procpu sends the actual runtime address of `audio_psram_buf` to appcpu at startup via IPM ID=3.

Sequence:
1. appcpu registers its IPM callback **before** its 1500ms startup sleep, so it's ready to receive early messages.
2. procpu (~500ms after boot) sends `(uint32_t)audio_psram_buf` on IPM ID=3 right after IPM is initialized.
3. appcpu waits up to 5s for the address, then calls `pdm_set_audio_buf(addr)` before `pdm_init`.
4. If the message never arrives (e.g. during early bringup), appcpu falls back to `AUDIO_PSRAM_BASE` in `pdm.h` with a warning log.

Log confirmation:
```
[C0] sent audio buf addr 0x3C0B0000 to appcpu
[C1] audio buf addr: 0x3C0B0000
```

Files changed:
- `appcpu/src/pdm.h` — added `pdm_set_audio_buf(uint32_t addr)`
- `appcpu/src/pdm.c` — `audio_buf` is now a non-const pointer, set via `pdm_set_audio_buf()`
- `appcpu/src/main.c` — early callback registration, wait loop, `pdm_set_audio_buf()` call
- `procpu/src/main.c` — sends `IPM_ID_BUFADDR` (ID=3) after IPM init
- Both `main.c` headers updated with ID=3 in the IPM message table

## Changes Already Made (Firmware + Debugging)

### PDM register forcing (appcpu)

In `embedded/voice_node/appcpu/src/pdm.c`:

- Explicitly forces PDM mode and PDM2PCM enable bits after `i2s_trigger(START)` resets RX mode.
- Explicitly sets PDM downsample to 64x (`dsr128=0`).
- Forces known sample formatting bits (endianness/alignment/bit-order).
- Per-block mono conversion in recording loop: extracts left channel from stereo DMA blocks, writes to PSRAM.
- Post-loop SD WAV write via DRAM bounce buffer (SD SPI DMA may not read correctly from PSRAM cache-mapped addresses).

### WAV creation + DC offset removal (procpu)

In `embedded/voice_node/procpu/src/main.c`:

- Builds WAV header, streams mono PCM to voice service via WebSocket.
- Auto-selects L/R slot based on mid-buffer amplitude.
- DC offset removal via mean subtraction.

### Overlay alignment

`voice_node` appcpu overlay now matches `testapp_mic`:
- `I2S0_I_WS_GPIO42` (PDM clock output)
- `I2S0_I_SD_GPIO41` (PDM data input, with `bias-pull-down`)

## Remaining Work

- **procpu stereo/mono mismatch**: `send_audio_as_wav()` in `procpu/src/main.c` still treats PSRAM data as stereo-interleaved (stride-2 access, `mono_bytes = byte_count / 2`). The data is already mono from appcpu. This needs to be fixed for the WebSocket path to work correctly.
- Re-enable `vad_filter=True` in `services/voice/src/transcribe.py` once end-to-end path is verified.
- Clean up diagnostic logging (`log_block_stats`, PSRAM readback, register dumps).
