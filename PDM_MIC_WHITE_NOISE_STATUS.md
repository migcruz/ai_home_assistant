# PDM Mic White Noise Debug Status (Voice Node)

## Summary

`testapp` (now renamed to `testapp_mic`) has now produced a clear, intelligible recording to SD card. This is a major update: the microphone hardware is working.

`voice_node` still produces static/white-noise behavior in the outbound WAV/transcript path.

Given the successful `testapp_mic` capture, the issue is now most likely in configuration/path differences between `testapp_mic` and `voice_node` rather than mic hardware failure.

## Latest Confirmed Finding (2026-03-15)

- `testapp_mic` records clear audio to SD WAV (human speech is intelligible, similar to Arduino test quality).
- Therefore:
  - Mic element and board hardware are functional.
  - ESP32-S3 PDM capture path can work correctly under Zephyr on this board.
- The highest-priority suspect shifts to `voice_node` overlay/routing and integration differences.

## What We Know From Logs

- App CPU logs show RX is configured as:
  - `tdm=0`, `pdm=1`, `pdm2pcm=1`, `dsr128=0` (64x downsample)
  - Example: `rx_conf=0x00301004 (tdm=0 pdm=1 pdm2pcm=1 dsr128=0)`
- Pro CPU audio inspection shows:
  - L and R channels have similar energy (neither is near-silent).
  - **L/R correlation ~ 1.000** consistently in the mid-window.
    - This implies both “channels” are duplicates, and they are duplicate noise.
- The generated WAV is mono 16 kHz, 16-bit, and the server (`services/voice/src/transcribe.py`) saves the last received audio to `/tmp/last_audio.wav` for listening.

## Changes Already Made (Firmware + Debugging)

### PDM register forcing (appcpu)

In `embedded/voice_node/appcpu/src/pdm.c`:

- Explicitly forces PDM mode and PDM2PCM enable bits after `i2s_trigger(START)` resets RX mode.
- Explicitly sets PDM downsample to 64x (`dsr128=0`).
- Forces known sample formatting bits (endianness/alignment/bit-order) to avoid inheriting stale state.
- Adds logging after `i2s_ll_rx_start()` so we can see the in-effect RX configuration.
- Adds a `PDM_ACTIVE_MASK` constant (slot gating). We tried:
  - `0x02` (LEFT-only)
  - `0x01` (RIGHT-only)
  - `0x03` (both)
  Slot gating did not resolve the noise.

### WAV creation + DC offset removal (procpu)

In `embedded/voice_node/procpu/src/main.c`:

- Builds a proper WAV header and streams binary WAV frames to the voice service.
- Strips stereo to mono.
- Removes DC offset via a mean subtraction pass.
- Adds auto slot pick (LEFT vs RIGHT) based on mid-buffer amplitude, to avoid stripping the wrong slot if the mic lands in the opposite slot.

In `embedded/voice_node/procpu/src/audio.c`:

- Adds L/R correlation logging over a mid-window to identify duplicate channels vs unrelated noise.

### Overlay experiments (signal matrix)

We tried multiple devicetree overlays under `embedded/voice_node/overlays/` to vary:

- Whether PDM clock is driven on `BCK` vs `WS`.
- Whether the pins are swapped (GPIO41/GPIO42 permutations).
- Using **output** clock signals (`I2S0_O_BCK`, `I2S0_O_WS`) instead of input signals.

None of the overlay variants changed the “full-scale noise” characteristics.

### Build system fix (overlay path)

In `embedded/voice_node/Makefile`:

- Overlay paths are normalized to absolute paths so `make APPCPU_OVERLAY=...` works reliably (CMake resolves relative overlay paths from the build directory).

## Current Interpretation

Previous interpretation assumed likely hardware fault. That is now invalidated by `testapp_mic` success.

Current interpretation:

- `voice_node` and `testapp_mic` are not using the same effective pin/signal routing.
- The strongest concrete mismatch currently identified:
  - `testapp_mic` overlay (`embedded/testapp_mic/testapp_mic.overlay`) uses:
    - `I2S0_I_WS_GPIO42`
    - `I2S0_I_SD_GPIO41` (+ `bias-pull-down`)
  - `voice_node` appcpu overlay (`embedded/voice_node/overlays/appcpu.overlay`) currently uses:
    - `I2S0_O_BCK_GPIO41`
    - `I2S0_I_SD_GPIO42` (+ `bias-pull-down`)
- `voice_node` also adds extra complexity not present in `testapp_mic`:
  - dual-core IPC path (appcpu capture -> procpu processing)
  - PSRAM handoff + cache coherence
  - runtime slot auto-selection + DC removal + WS streaming

## Next Steps (Pending Hardware Verification)

1. Mirror `testapp_mic` pin routing exactly in `voice_node` appcpu overlay as the first change.
2. Keep capture format/clock settings aligned between `testapp_mic` and `voice_node` during comparison.
3. Once routing is aligned, re-test `voice_node` and compare:
   - L/R RMS and correlation logs
   - resulting transcript quality
4. If still noisy after routing alignment, isolate each integration layer:
   - appcpu capture-only dump to file/raw buffer
   - procpu PSRAM readback verification
   - websocket transport verification

## Useful Debug Artifacts

- On the voice service container:
  - `/tmp/last_audio.wav` is written on each transcription attempt (see `services/voice/src/transcribe.py`).
- On-device comparison baseline:
  - `testapp_mic` SD output WAV is now a known-good local reference.
