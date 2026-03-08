# Technical Specification — Ambient Voice Node

**Status:** Draft
**Version:** 0.1
**Derives from:** [design-spec.md](design-spec.md)

---

## 1. Hardware

### 1.1 Bill of Materials

| Component | Purpose | Est. cost |
|---|---|---|
| Seeed XIAO ESP32S3 Sense | MCU, 8MB OPI PSRAM, PDM mic, WiFi, BLE | ~$14 |
| MAX98357A I2S amplifier module | I2S DAC + Class D amp, 3.3W into 4Ω | ~$3 |
| Speaker (4Ω, 2W) | Audio output | ~$3 |
| USB-C power supply | 5V mains power | ~$5 |
| **Total** | | **~$25** |

### 1.2 MCU Specifications

| Spec | Value |
|---|---|
| SoC | ESP32-S3R8 |
| CPU | Xtensa LX7 dual-core @ 240MHz |
| On-chip SRAM | 512KB |
| PSRAM | 8MB OPI (Octal SPI, on-die) |
| Flash | 8MB |
| WiFi | 802.11 b/g/n (2.4GHz) |
| Bluetooth | 5.0 LE |
| Built-in mic | PDM — MSM261S4030H0R |

The 8MB OPI PSRAM is the critical enabler. The combined TLS session, networking stack, audio buffers, and wake word model would not fit in the 512KB on-chip SRAM alone.

### 1.3 I2S Amplifier Wiring

```
XIAO ESP32S3       MAX98357A
  GPIO 2  ──────── BCLK
  GPIO 3  ──────── LRCLK  ────── Speaker (4Ω or 8Ω, 1–3W)
  GPIO 4  ──────── DIN
  3.3V    ──────── VIN
  GND     ──────── GND
```

---

## 2. Software Stack

### 2.1 Overview

| Layer | Technology |
|---|---|
| RTOS | Zephyr 4.3.0 (AMP — two independent images, one per core) |
| Multi-image build | west sysbuild (`west build --sysbuild`) |
| Inter-core IPC | `espressif,esp32-ipm` — 1KB shared memory, one in-flight message at a time |
| WiFi driver | Espressif binary blob (hal_espressif) — procpu only |
| TLS | mbedTLS (bundled with Zephyr) — procpu only |
| WebSocket | Zephyr WebSocket client (`CONFIG_WEBSOCKET_CLIENT`) — procpu only |
| PDM microphone | Zephyr I2S driver + PDM config — appcpu |
| I2S playback | Zephyr I2S driver — procpu |
| Wake word | TensorFlow Lite Micro (`CONFIG_TENSORFLOW_LITE_MICRO`) — appcpu |
| BLE provisioning | Zephyr BLE stack + GATT (`CONFIG_BT`, `CONFIG_BT_PERIPHERAL`) — procpu |
| Credential storage | Zephyr NVS + settings subsystem — procpu |
| Console / shell | USB Serial/JTAG owned by procpu; appcpu forwards logs via IPM |
| Build system | west + CMake + sysbuild |
| Toolchain | Zephyr SDK 0.17.4 (xtensa-espressif_esp32s3_zephyr-elf) |
| Dev environment | Dev Container (Docker) — see `embedded/.devcontainer/` |

### 2.2 Key Kconfig Settings

**procpu/prj.conf** (Core 0 — networking, audio I/O, shell):

```kconfig
# ── IPC ───────────────────────────────────────────────────────────────────────
CONFIG_IPM=y                      # inter-processor mailbox (receives from appcpu)

# ── PSRAM ─────────────────────────────────────────────────────────────────────
CONFIG_ESP_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y          # XIAO ESP32S3 has OPI PSRAM — quad mode crashes
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y

# ── WiFi ──────────────────────────────────────────────────────────────────────
CONFIG_WIFI=y
CONFIG_NET_L2_WIFI_MGMT=y

# ── Networking ────────────────────────────────────────────────────────────────
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_DNS_RESOLVER=y

# ── TLS ───────────────────────────────────────────────────────────────────────
CONFIG_MBEDTLS=y
CONFIG_NET_SOCKETS_SOCKOPT_TLS=y
CONFIG_NET_SOCKETS_TLS_MAX_CONTEXTS=4

# ── WebSocket ─────────────────────────────────────────────────────────────────
CONFIG_WEBSOCKET_CLIENT=y
CONFIG_HTTP_CLIENT=y

# ── I2S playback ──────────────────────────────────────────────────────────────
CONFIG_I2S=y

# ── BLE provisioning ──────────────────────────────────────────────────────────
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_GATT=y

# ── Credential storage ────────────────────────────────────────────────────────
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
CONFIG_NVS=y

# ── Shell (development) ───────────────────────────────────────────────────────
CONFIG_SHELL=y
CONFIG_SHELL_BACKEND_SERIAL=y
CONFIG_NET_SHELL=y
CONFIG_NET_L2_WIFI_SHELL=y
CONFIG_GPIO_SHELL=y
```

**appcpu/prj.conf** (Core 1 — wake word, VAD, mic capture):

```kconfig
# ── IPC ───────────────────────────────────────────────────────────────────────
CONFIG_IPM=y                      # sends events + log strings to procpu

# ── PDM microphone ────────────────────────────────────────────────────────────
CONFIG_I2S=y

# ── Wake word ─────────────────────────────────────────────────────────────────
CONFIG_TENSORFLOW_LITE_MICRO=y

# No shell, no log backend UART — appcpu has no console.
# Log strings are forwarded to procpu via IPM and printed with [C1] prefix.
```

---

## 3. Memory Budget

All large allocations go in PSRAM. On-chip SRAM holds only the Zephyr kernel, interrupt handlers, and time-critical audio paths.

| Component | Approx size | Location |
|---|---|---|
| mbedTLS + TLS session | ~150KB | PSRAM |
| LwIP networking stack | ~80KB | PSRAM |
| WebSocket + frame buffers | ~40KB | PSRAM |
| Audio record buffer (5s @ 16kHz 16-bit mono) | ~160KB | PSRAM |
| WAV playback buffer | ~64KB | PSRAM |
| TFLite Micro wake word model | ~40KB | PSRAM |
| Zephyr kernel + misc | ~50KB | On-chip SRAM |
| **Total PSRAM used** | **~580KB** | fits in 8MB comfortably |

---

## 4. Audio Configuration

### 4.1 Microphone (PDM → PCM)

| Parameter | Value |
|---|---|
| Interface | PDM via I2S driver |
| Sample rate | 16kHz |
| Bit depth | 16-bit |
| Channels | Mono |
| Buffer duration | 5 seconds max recording |

### 4.2 Speaker (PCM → I2S)

| Parameter | Value |
|---|---|
| Interface | I2S output |
| Sample rate | 22050Hz (Piper TTS output — parsed from WAV header) |
| Bit depth | 16-bit |
| Channels | Mono |
| Amplifier | MAX98357A Class D, 3.3W into 4Ω |

The I2S clock is reconfigured at the start of each playback frame to match the sample rate from the WAV header, allowing the server's TTS output rate to change without firmware updates.

### 4.3 WAV Header Format (outbound audio)

Outbound audio is sent as raw 16kHz 16-bit mono PCM with a standard WAV header prepended:

```
Bytes 0–3:   "RIFF"
Bytes 4–7:   file size - 8 (little-endian uint32)
Bytes 8–11:  "WAVE"
Bytes 12–35: fmt chunk (PCM, 1ch, 16000Hz, 16-bit)
Bytes 36–43: data chunk header + size
Bytes 44+:   raw PCM samples
```

---

## 5. Server Wire Protocol

The device uses the existing `WS /voice/converse` protocol on the Voice Service. No server-side changes to the WebSocket protocol are needed. Two small server-side changes are required to handle WAV input (see §5.3).

### 5.1 Config Message (sent on connect)

```json
{ "type": "config", "tts": true, "audio_format": "wav", "sample_rate": 16000 }
```

### 5.2 Full Protocol Summary

| Direction | Frame type | Content |
|---|---|---|
| Device → Server | TEXT | `{ "type": "config", ... }` |
| Device → Server | BINARY | Raw WAV audio chunks |
| Device → Server | TEXT | `{ "type": "end_audio" }` |
| Server → Device | TEXT | `{ "type": "transcript", "text": "..." }` — ignored |
| Server → Device | TEXT | `{ "type": "token", "content": "..." }` — ignored |
| Server → Device | BINARY | WAV audio frame (one sentence) |
| Server → Device | TEXT | `{ "type": "done" }` |
| Server → Device | TEXT | `{ "type": "error", "detail": "..." }` |

### 5.3 Required Server-Side Changes

**`services/voice/src/transcribe.py`** — accept WAV format:

```python
def transcribe(audio_bytes: bytes, fmt: str = "webm") -> str:
    suffix = ".wav" if fmt == "wav" else ".webm"
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
        tmp.write(audio_bytes)
        tmp_path = tmp.name
    # rest unchanged
```

**`services/voice/src/converse.py`** — parse `audio_format` from the config message and thread it through to `transcribe_audio()`. Browser clients default to `"webm"` if no config message is sent.

### 5.4 TLS Certificate

The server uses a self-signed certificate. For LAN-only use, the device sets `MBEDTLS_SSL_VERIFY_NONE`. Alternatively, the self-signed certificate can be burned into device flash and trusted explicitly.

---

## 6. Wake Word

### 6.1 Approach

TensorFlow Lite Micro runs on the **APP CPU (Core 1)**, keeping the PRO CPU free for networking and audio I/O. The ESP32-S3 includes vector instruction extensions (PIE) that accelerate inference. Expected inference time: ~10ms per frame at low CPU load.

When a wake word is detected, the appcpu sends a wake event to the procpu via IPM. The procpu then opens the WebSocket and begins streaming audio.

**Candidate models:**

| Model | Size | Notes |
|---|---|---|
| Google Micro Speech Commands (keyword spotting) | ~18KB | Good starting point; supports custom keywords |
| Edge Impulse custom model | ~20–40KB | Train on "hey computer" recordings; higher accuracy |

### 6.2 Wake Word Threshold Tuning

TFLite Micro wake word models have higher false positive rates than server-side models. The detection threshold is tunable at runtime. Start conservatively (high threshold) and lower if the wake word is missed too often.

### 6.3 Alternative: Dedicated Wake Word IC

If false positive rates are unacceptable in a noisy room, a dedicated IC can be wired to the MCU over SPI/UART:

| IC | Notes |
|---|---|
| Syntiant NDP101 | 140µW always-on, I2S mic input, SPI to host |
| Knowles IA8201 | Multi-stage DSP + neural net, enterprise-grade accuracy |

---

## 7. WiFi Provisioning

### 7.1 BLE GATT Service

The device advertises a custom BLE peripheral service with two writable characteristics:

| Characteristic | UUID (custom) | Purpose |
|---|---|---|
| SSID | `0x0001` | WiFi network name (UTF-8, max 32 bytes) |
| Password | `0x0002` | WiFi password (UTF-8, max 64 bytes) |

On successful write to both characteristics, the device:
1. Calls `settings_save()` to persist to NVS
2. Stops BLE advertisement
3. Connects to WiFi using the new credentials

### 7.2 Provisioning Clients

| Client | Notes |
|---|---|
| nRF Connect (Android/iOS) | Free dev app, write characteristics manually — good for testing |
| Web Bluetooth page | Host at `https://vulcan.local/provision/` — Chrome on Android only |
| Custom app | React Native / Swift / Kotlin for polished UX |

### 7.3 Improv WiFi (future option)

Implementing the [Improv WiFi](https://www.improv-wifi.com/) open standard (same BLE hardware path, standardised GATT service UUID) would allow Home Assistant to provision the device directly from its dashboard.

---

## 8. Project Structure

```
embedded/
├── .devcontainer/
│   ├── Dockerfile          # Zephyr SDK 0.17.4, all toolchains
│   └── devcontainer.json   # mounts, postCreateCommand (west init + update + blobs)
├── west.yml                # pins Zephyr v4.3.0
├── README.md
└── voice_node/
    ├── Makefile             # sysbuild wrappers (make / make flash / make menuconfig)
    ├── specs/
    │   ├── functional-spec.md
    │   ├── design-spec.md
    │   └── technical-spec.md
    ├── procpu/              # PRO CPU image (Core 0) — networking, audio I/O, shell
    │   ├── CMakeLists.txt
    │   ├── prj.conf         # Kconfig: IPM, WiFi, TLS, WebSocket, I2S, BLE, shell
    │   ├── sysbuild.cmake   # registers appcpu as remote image; sets build order
    │   ├── credentials.conf # gitignored — WiFi SSID/PSK for prototype builds
    │   ├── socs/
    │   │   └── esp32s3_procpu_sense.overlay  # enables ipm0 for procpu
    │   └── src/
    │       ├── main.c           # entry point, IPM receive, state machine
    │       ├── wifi.c           # WiFi connect + reconnect
    │       ├── provisioning.c   # BLE GATT provisioning
    │       ├── websocket.c      # WebSocket client + protocol framing
    │       ├── audio_capture.c  # PDM mic → PCM → WAV framing
    │       ├── audio_playback.c # WAV frame receive → I2S output
    │       └── chime.c          # acknowledgement and error chimes
    └── appcpu/              # APP CPU image (Core 1) — wake word, VAD, mic capture
        ├── CMakeLists.txt
        ├── prj.conf         # Kconfig: IPM, I2S, TFLite Micro — no shell/log backend
        ├── boards/
        │   └── xiao_esp32s3_appcpu.overlay  # enables ipm0; console → usb_serial (unused)
        └── src/
            ├── main.c       # entry point; ipm_log() forwards strings to procpu via IPM
            ├── wake_word.c  # TFLite Micro inference loop; sends wake event via IPM
            └── vad.c        # RMS-based voice activity detection; sends end-of-speech via IPM
```

**Flash layout** (4MB, `partitions_0x0_amp.dtsi`):

| Partition | Offset | Size | Contents |
|---|---|---|---|
| mcuboot | `0x000000` | 64 KB | Bootloader |
| sys | `0x010000` | 64 KB | System / NVS |
| image-0 (procpu slot 0) | `0x020000` | 1344 KB | Active PRO CPU image |
| image-1 (procpu slot 1) | `0x170000` | 1344 KB | OTA swap slot |
| image-0-appcpu (slot 0) | `0x2c0000` | 448 KB | Active APP CPU image |
| image-1-appcpu (slot 1) | `0x330000` | 448 KB | OTA swap slot |
| storage | `0x3b0000` | 192 KB | NVS credentials |
| image-scratch | `0x3e0000` | 124 KB | MCUboot swap scratch |
| coredump | `0x3ff000` | 4 KB | Crash dump |

---

## 9. Build and Flash

### 9.1 Build (inside Dev Container)

The build uses sysbuild to compile both images together. The `Makefile` wraps the west commands:

```bash
cd /zephyr-ws/embedded/voice_node
make                    # incremental sysbuild (procpu + appcpu + mcuboot)
make clean && make      # full rebuild
make menuconfig         # interactive Kconfig browser (procpu image)
```

Equivalent raw west command:
```bash
west build --sysbuild -d build -b xiao_esp32s3/esp32s3/procpu/sense procpu
```

For prototype builds with hardcoded WiFi credentials, create `procpu/credentials.conf` (gitignored):

```kconfig
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PSK="YourPassword"
```

Then pass it as an extra config file:

```bash
west build --sysbuild -d build -b xiao_esp32s3/esp32s3/procpu/sense procpu \
  -- -Dprocpu_EXTRA_CONF_FILE=credentials.conf
```

### 9.2 Flash

`west flash` writes all three images in one operation: MCUboot at `0x0`, procpu image at `0x20000`, appcpu image at `0x2c0000`.

```bash
make flash # run inside the container
# equivalent: west flash -d build
```

Some boards require entering bootloader mode: hold BOOT, tap RESET, release BOOT.

### 9.3 Serial Monitor

```bash
picocom /dev/ttyACM0    # baud rate ignored — ESP32S3 USB Serial/JTAG
```

With shell enabled, the prompt is:
```
uart:~$
```

Useful shell commands:
```
wifi scan
wifi connect <SSID> <password>
wifi status
net iface
net ping 192.168.1.x
gpio get gpio0 2
```

---

## 10. Implementation Order

Each milestone is independently testable. Do not proceed to the next until the current one is solid.

| Milestone | Key tasks | Status |
|---|---|---|
| **0 — Scaffold** | Both cores boot (AMP sysbuild); procpu blinks LED + runs shell; appcpu forwards heartbeat logs to procpu via IPM; both visible on `/dev/ttyACM0` | Done |
| **1 — Network** | WiFi connects; TLS handshake; WebSocket opens; text round-trip with server | — |
| **2 — Mic → Server** | PDM capture; WAV framing; stream to server; server transcribes correctly | — |
| **3 — Server → Speaker** | Receive binary WAV frames; I2S playback through MAX98357A at 22050Hz | — |
| **4 — Full round-trip** | Button-triggered (not wake word yet): speak → hear response end-to-end | — |
| **5 — Wake word** | Integrate TFLite Micro model; replace button press with keyword detection | — |
| **6 — VAD** | Replace fixed timeout with RMS-based voice activity detection | — |
| **7 — Provisioning** | BLE GATT provisioning flow; NVS credential storage; replace hardcoded creds | — |

---

## 11. Open Questions

| Question | Notes |
|---|---|
| Zephyr ESP32S3 WiFi stability | Espressif binary blob WiFi — less battle-tested than ESP-IDF. Validate WiFi + TLS + WebSocket path (Milestone 1) before building audio on top. |
| I2S @ 22050Hz | Non-power-of-two sample rate. Supported on ESP32S3 but verify Zephyr I2S driver handles it correctly with the clock configuration for MAX98357A. |
| Wake word false positive rate | TFLite Micro models may trigger occasionally in a noisy room. Tune threshold; consider dedicated IC if unacceptable. |
| VAD sensitivity | Energy-based VAD may cut off trailing words in a quiet room or fail to detect silence in a noisy one. May need tuning or a lightweight VAD model. |
| PSRAM stability at 80MHz | SPIRAM speed defaults to 40MHz. 80MHz is supported and would help with audio buffer throughput, but validate stability before enabling. |
