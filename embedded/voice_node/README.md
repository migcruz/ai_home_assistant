# Voice Node

The Voice Node is a standalone ambient voice device built on the Seeed XIAO ESP32S3 Sense. It runs Zephyr RTOS 4.3.0 across both LX7 cores in an AMP (Asymmetric Multi-Processing) configuration, connects to the home network over WiFi, and speaks with the [Voice Service](../../services/voice/) via a TLS WebSocket.

The device listens continuously for a wake word (TFLite Micro on Core 1), then streams PCM audio to the server for transcription, sends it through the LLM, and plays back the synthesised TTS response through a MAX98357A Class D amplifier. No cloud dependencies — everything runs on the local network.

---

## System Design

```mermaid
graph TB
    subgraph C0["PRO CPU — Core 0 (procpu)"]
        WIFI["WiFi WPA2"]
        TLS["TLS 1.2/1.3 mbedTLS"]
        WS["WebSocket Client"]
        ABUF["Audio Buffer PSRAM"]
        I2S["I2S Output"]
        AMP["MAX98357A Class D Amp"]
        SPK["Speaker"]
        BLE["BLE Peripheral provisioning only"]
        LFS["LittleFS credentials"]
        SHELL["Shell USB Serial/JTAG"]
    end

    subgraph C1["APP CPU — Core 1 (appcpu)"]
        MIC["PDM Microphone"]
        WW["Wake Word TFLite Micro"]
        VAD["Voice Activity Detector"]
    end

    subgraph IPM["Inter-Core IPC"]
        ipm["espressif,esp32-ipm 1KB shared memory"]
    end

    subgraph Server["Home Network"]
        VSVC["Voice Service WS /voice/converse"]
    end

    MIC --> WW
    WW -->|"wake event"| ipm
    VAD -->|"end-of-speech"| ipm
    ipm -->|"events + log strings"| C0

    ABUF -->|"WAV frames"| WS
    WS --> TLS --> WIFI --> VSVC
    VSVC -->|"WAV frames"| WS
    WS --> ABUF --> I2S --> AMP --> SPK

    BLE -->|"SSID + PSK"| LFS
    LFS --> WIFI
```

---

## Architecture

### AMP — Two Independent Images

The ESP32-S3 is a dual-core LX7. Zephyr 4.3.0 does not implement SMP on this chip, so each core runs a separate Zephyr image built together by `west sysbuild` and flashed to separate flash partitions.

| Core | Image | Responsibilities |
|---|---|---|
| PRO CPU — Core 0 | `procpu` | WiFi, TLS, WebSocket, I2S playback, BLE provisioning, shell |
| APP CPU — Core 1 | `appcpu` | PDM mic capture, wake word inference, VAD |

### Inter-Core Communication (IPM)

Cores communicate via `espressif,esp32-ipm` — a shared memory mailbox with a 1KB payload slot. The driver does not queue messages; each send must complete before the next.

Current IPM message IDs:

| ID | Direction | Payload | Purpose |
|---|---|---|---|
| 0 | appcpu → procpu | `char[]` log string | Forward appcpu log lines; procpu prints with `[C1]` prefix |
| 1 | procpu → appcpu | `uint8_t` (1=start, 0=stop) | BOOT button press/release → start/stop PDM capture |
| 2 | appcpu → procpu | `uint8_t` done signal | PDM capture complete; byte count lives in `struct audio_shared` in PSRAM |
| 3 | procpu → appcpu | `uint32_t` address | PSRAM buffer address (`audio_psram_buf`) — sent at startup so both cores agree |

Future IDs will carry wake word events and end-of-speech signals once TFLite Micro and VAD are added.

### WebSocket Wire Protocol

The device speaks the same `WS /voice/converse` protocol as the web client. See [Voice Service](../../services/voice/README.md) for the full protocol definition. Device-specific behaviour:

```
Device → Server:
  TEXT    { "type": "config", "tts": true, "audio_format": "wav", "sample_rate": 16000 }
  BINARY  WAV audio chunks (16kHz, 16-bit, mono)
  TEXT    { "type": "end_audio" }

Server → Device:
  TEXT    { "type": "transcript", ... }   ignored — no display
  TEXT    { "type": "token", ... }        ignored — no display
  BINARY  WAV audio frame (one sentence)  queued for I2S playback
  TEXT    { "type": "done" }
  TEXT    { "type": "error", ... }        plays error chime, returns to IDLE
```

### Device State Machine

```
IDLE  ──(wake word)──▶  WAKE  ──(WS connected)──▶  RECORDING
                                                         │
                                              silence / button / timeout
                                                         │
                                                         ▼
                                                      WAITING
                                                         │
                                                  first WAV frame
                                                         │
                                                         ▼
                                                      PLAYING ──(done + drained)──▶ IDLE

Any state: WS close / error → error chime → IDLE
```

---

## Directory Structure

```
embedded/voice_node/
├── Makefile                     # sysbuild wrappers: make / make flash / make menuconfig
├── README.md
├── specs/
│   ├── functional-spec.md       # what the device does
│   ├── design-spec.md           # architecture, state machine, audio flows
│   └── technical-spec.md        # BOM, Kconfig, flash layout, build commands
├── overlays/                    # DTS overlays passed explicitly via -DDTC_OVERLAY_FILE
│   ├── procpu.overlay           # ipm0, BOOT button, SD card (SPI2, GPIO7-9/21)
│   └── appcpu.overlay           # ipm0, USB serial, DMA, I2S0 PDM pinctrl (GPIO42 CLK, GPIO41 DATA)
├── common/                      # shared headers and drivers — included by both images
│   ├── audio_shared.h           # struct audio_shared, AUDIO_BUF_MAX, AUDIO_SAMPLE_RATE, etc.
│   ├── ipm_ids.h                # IPM message ID constants (single source of truth)
│   ├── sd_wav.h                 # sd_wav_write() API
│   └── sd_wav.c                 # FAT/SD WAV writer; FATFS placed in PSRAM (.ext_ram.bss)
├── procpu/                      # PRO CPU image (Core 0)
│   ├── CMakeLists.txt           # includes ../common/sd_wav.c
│   ├── sysbuild.cmake           # registers appcpu as remote image
│   ├── prj.conf                 # Kconfig: IPM, WiFi, TLS, WebSocket, SD/FAT, shell
│   └── src/
│       ├── main.c               # send_audio_as_wav (reads audio_shared), LED blink loop
│       ├── storage.c            # LittleFS mount + credential read/write
│       ├── button.c/h           # BOOT button: GPIO interrupt → k_work → IPM CMD
│       ├── audio.c/h            # audio_psram_buf in .ext_ram.bss, PSRAM address export
│       ├── wifi.c/h             # WiFi connect + reconnect
│       └── websocket.c/h        # TLS WebSocket client, ws_send_binary/text
└── appcpu/                      # APP CPU image (Core 1)
    ├── CMakeLists.txt
    ├── prj.conf                 # Kconfig: IPM, DMA, I2S, PSRAM (OPI) — no SD/shell
    └── src/
        ├── main.c               # IPM cmd handler, PSRAM address negotiation (IPM ID=3)
        └── pdm.c/h              # I2S PDM init, per-block mono strip, cache flush
```

---

## Hardware

| Component | Purpose | Est. cost |
|---|---|---|
| Seeed XIAO ESP32S3 Sense | MCU, 8MB OPI PSRAM, PDM mic, WiFi, BLE | ~$14 |
| MAX98357A I2S amplifier | I2S DAC + Class D amp, 3.3W into 4Ω | ~$3 |
| Speaker (4Ω, 2W) | Audio output | ~$3 |
| USB-C power supply | 5V mains | ~$5 |

I2S amplifier wiring:

```
XIAO ESP32S3    MAX98357A
  GPIO 2  ───── BCLK
  GPIO 3  ───── LRCLK
  GPIO 4  ───── DIN
  3.3V    ───── VIN
  GND     ───── GND
```

---

## Build and Flash

All commands run inside the Dev Container (`embedded/.devcontainer/`).

```bash
cd /zephyr-ws/embedded/voice_node

make                    # sysbuild: procpu + appcpu + MCUboot
make clean && make      # full rebuild
make menuconfig         # interactive Kconfig (procpu image)
make flash              # west flash -d build
```

If the PDM mic is only producing static, try the alternate pin mapping overlay:

```bash
make APPCPU_OVERLAY=overlays/appcpu_gpio42_clk_gpio41_data.overlay
```

If it is still static, try driving the PDM clock on `WS` (ESP-IDF-style PDM):

```bash
make APPCPU_OVERLAY=overlays/appcpu_gpio42_ws_clk_gpio41_data.overlay
```

For prototype builds with hardcoded WiFi credentials, create `procpu/credentials.conf` (gitignored):

```kconfig
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PSK="YourPassword"
```

Pass it at build time:

```bash
west build --sysbuild -d build -b xiao_esp32s3/esp32s3/procpu/sense procpu \
  -- -Dprocpu_EXTRA_CONF_FILE=credentials.conf
```

### Serial Monitor

```bash
picocom /dev/ttyACM0    # baud rate ignored — USB Serial/JTAG
```

Shell prompt: `uart:~$`. Both cores' log output appears here — procpu logs are prefixed `[C0]`, appcpu logs forwarded via IPM are prefixed `[C1]`.

---

## Key Design Decisions

- **AMP over SMP** — Zephyr 4.3.0 does not implement `arch_cpu_start` for the ESP32-S3, making SMP unavailable. AMP with sysbuild gives full independent control of each core, avoids the shared-scheduler complexity of SMP, and maps cleanly to the workload split: Core 0 owns I/O, Core 1 owns inference.
- **IPM for logging** — The USB Serial/JTAG peripheral is owned by the procpu driver. The appcpu cannot write to it independently (flush mechanism is driver-controlled). Instead, the appcpu formats log strings and sends them to the procpu via IPM, which prints them. One send per log call with `wait=1` prevents the single shared-memory slot from being overwritten before the procpu reads it.
- **Server-side STT/LLM/TTS** — Whisper, Ollama, and Piper run on the server. The device only streams raw audio and plays back WAV. This keeps firmware complexity low and lets the server be upgraded independently.
- **No persistent WebSocket** — The connection opens on wake and closes after playback drains. Idle state carries no open socket, which simplifies reconnection and reduces power draw.
- **OPI PSRAM** — The XIAO ESP32S3 uses Octal PSRAM. `CONFIG_SPIRAM_MODE_OCT=y` is required; the default Quad mode causes a boot crash.
- **I2S DMA always stereo** — The ESP32-S3 I2S DMA controller only operates in stereo (`channels=2`); `channels=1` is rejected with `EINVAL`. The mono PDM mic outputs on the left channel; the right channel carries zeros. Mono conversion (keep every even sample) happens per-block inside `pdm_record()` on appcpu — the PSRAM buffer contains mono PCM by the time procpu reads it.
- **PSRAM cache coherency** — Both cores access PSRAM through independent L1 data caches with no hardware coherency. appcpu writes `struct audio_shared` (header + mono PCM) to PSRAM and calls `sys_cache_data_flush_range()` before sending the IPM done signal. procpu calls `sys_cache_data_invd_range()` on the header, reads `shdr->byte_count`, then invalidates the PCM range before streaming.
- **Single source of truth via `struct audio_shared`** — The PSRAM buffer starts with a self-describing header (`magic`, `byte_count`, `sample_rate`, `channels`, `bits`) followed by a flexible array member `pcm[]`. The IPM done signal is a 1-byte notification only; procpu reads all metadata from the struct, eliminating the need to pass `byte_count` over IPM.

---

## Milestones

| # | Scope | Status |
|---|---|---|
| 0 — Scaffold | Both cores boot (AMP); procpu blinks LED + shell; appcpu heartbeat via IPM | **Done** |
| 1 — PDM capture | BOOT button (interrupt-driven) → appcpu captures PDM to shared PSRAM → procpu reads + validates via cache-safe IPM handshake | **Done** |
| 2 — Network | WiFi + TLS handshake + WebSocket open + text round-trip | **Done** |
| 3 — Mic → Server | WAV framing from PSRAM, WebSocket streaming, server transcribes correctly | — |
| 4 — Server → Speaker | Receive WAV frames, I2S playback through MAX98357A | — |
| 5 — Full round-trip | End-to-end: speak → hear response | — |
| 6 — Wake word | TFLite Micro on appcpu replaces BOOT button trigger | — |
| 7 — VAD | RMS silence detection replaces fixed timeout | — |
| 8 — Provisioning | BLE GATT provisioning, NVS credential storage | — |
