# Embedded Ambient Voice Node — XIAO ESP32S3 Sense Proposal

**Status:** Pre-implementation scoping
**Target device:** Seeed XIAO ESP32S3 Sense
**RTOS:** Zephyr
**Role:** Always-on ambient voice node — wake word detection, mic capture, speaker playback

---

## Overview

The ambient voice node is a small embedded device placed in a room to give hands-free access to the AI butler. It listens for a wake word locally, then streams recorded audio to the voice service over a secure WebSocket. The server handles all speech recognition, LLM inference, and text-to-speech. The device only streams audio in and plays WAV audio out.

Because all heavy processing (Whisper STT, Ollama LLM, Piper TTS) runs on the server GPU, the device requirements are minimal: WiFi, a microphone, a speaker, and enough compute to run a small wake word model.

---

## Hardware

### Core Board

**Seeed XIAO ESP32S3 Sense**

| Spec | Value |
|---|---|
| CPU | Xtensa LX7 dual-core @ 240MHz |
| On-chip SRAM | 512KB |
| PSRAM | 8MB (external, on-board) |
| Flash | 8MB |
| WiFi | 802.11 b/g/n |
| Bluetooth | 5.0 LE |
| Built-in mic | PDM (MSM261S4030H0R) |
| Dimensions | 21 × 17.5mm |

The 8MB PSRAM is the critical enabler. Without it, the memory budget for TLS + WebSocket + audio buffers would be too tight.

### Speaker Output

The ESP32S3 has no built-in DAC. Audio output uses I2S to an external Class D amplifier module.

**MAX98357A** (recommended)

| Spec | Value |
|---|---|
| Interface | I2S |
| Output power | 3.3W into 4Ω |
| Supply | 2.7–5.5V (3.3V from board) |
| Cost | ~$3 module |

Wiring:

```
XIAO ESP32S3          MAX98357A
  GPIO 2  ──── BCLK
  GPIO 3  ──── LRCLK  ──────── Speaker (4Ω or 8Ω, 1–3W)
  GPIO 4  ──── DIN
  3.3V    ──── VIN
  GND     ──── GND
```

### Full BOM

| Component | Purpose | Est. cost |
|---|---|---|
| Seeed XIAO ESP32S3 Sense | Main board, PDM mic, WiFi | ~$14 |
| MAX98357A module | I2S DAC + Class D amp | ~$3 |
| Small speaker (4Ω, 2W) | Audio output | ~$3 |
| USB-C power supply | Power | ~$5 |
| Enclosure (optional) | Housing | varies |
| **Total** | | **~$25** |

---

## Software Stack

### Zephyr RTOS

| Requirement | Zephyr Config | Status |
|---|---|---|
| WiFi | `CONFIG_ESP32_WIFI` | Supported via Espressif binary blob |
| TLS 1.2/1.3 | `CONFIG_MBEDTLS` | Full support |
| WebSocket client | `CONFIG_WEBSOCKET_CLIENT` | Binary + text frames |
| PDM microphone | `CONFIG_I2S` + PDM driver | Supported |
| I2S playback | `CONFIG_I2S` | Supported |
| Wake word inference | `CONFIG_TENSORFLOW_LITE_MICRO` | ESP32S3 vector extensions help |

### Memory Budget (PSRAM)

| Component | Approx RAM |
|---|---|
| mbedTLS + TLS session | ~150KB |
| LwIP networking stack | ~80KB |
| WebSocket + frame buffers | ~40KB |
| Audio record buffer (5s @ 16kHz 16-bit mono) | ~160KB |
| WAV playback buffer | ~64KB |
| Wake word TFLite model | ~40KB |
| Zephyr kernel + misc | ~50KB |
| **Total** | **~580KB** — fits in 8MB PSRAM comfortably |

---

## Device State Machine

```
IDLE
  │  PDM mic sampled continuously at low duty cycle
  │  TFLite Micro wake word model running on LX7
  │  WiFi associated but WebSocket closed
  │
  ▼ wake word detected
WAKE
  │  Play acknowledgement chime via I2S
  │  Open WSS /voice/converse to server
  │  Send: { "type": "config", "tts": true, "audio_format": "wav", "sample_rate": 16000 }
  │
  ▼ connection established
RECORDING
  │  PDM mic → raw PCM samples
  │  Buffer audio in PSRAM
  │  Prepend 44-byte WAV header to accumulated buffer
  │  Send binary WebSocket frames (audio chunks)
  │  Energy-based VAD: monitor RMS amplitude
  │
  ▼ silence detected (VAD) or timeout (8s)
  │  Send: { "type": "end_audio" }
  │
WAITING
  │  Receive: { "type": "transcript", "text": "..." }  — ignored (no display)
  │  Receive: { "type": "token", "content": "..." }    — ignored
  │  Receive: binary WAV frames (TTS per sentence)     — enqueue for playback
  │  Receive: { "type": "done" }                       — signal playback end
  │
PLAYING
  │  Dequeue WAV frames from PSRAM buffer
  │  Strip 44-byte WAV header
  │  Feed raw PCM to I2S at 22050Hz → MAX98357A → speaker
  │
  ▼ playback complete
IDLE
```

Error path: on WebSocket close or server error at any state → play error chime → return to IDLE.

---

## Protocol

The device uses the existing `WS /voice/converse` wire protocol without modification on the server's text frame side. One small server-side change is needed to handle WAV audio input instead of webm.

### Config Message (sent on connect)

```json
{ "type": "config", "tts": true, "audio_format": "wav", "sample_rate": 16000 }
```

### Audio Transmission

The device sends raw 16kHz 16-bit mono PCM with a standard WAV header prepended:

```
Bytes 0–3:   "RIFF"
Bytes 4–7:   file size - 8 (little-endian uint32)
Bytes 8–11:  "WAVE"
Bytes 12–35: fmt chunk (PCM, 1 channel, 16000Hz, 16-bit)
Bytes 36–43: data chunk header + data size
Bytes 44+:   raw PCM samples
```

This is 44 bytes of header + PCM data. The device constructs this in PSRAM before sending.

### TTS Playback

The server sends binary WebSocket frames, each containing one sentence's WAV audio (Piper TTS medium voice outputs at 22050Hz, 16-bit mono). The device:

1. Receives binary frame into PSRAM buffer
2. Parses sample rate from WAV header bytes 24–27
3. Configures I2S clock for parsed sample rate
4. Feeds raw PCM to I2S output

---

## Wake Word

### Approach: TFLite Micro on ESP32S3

Zephyr's `CONFIG_TENSORFLOW_LITE_MICRO` enables TFLite Micro on the LX7. The ESP32S3 includes a vector instruction extension (PIE) that accelerates inference.

Suitable models:
- Google Microcontroller Speech Commands (keyword spotting) — ~18KB, runs at ~10ms/inference
- Custom "hey butler" or "hey computer" model trained via Edge Impulse or Google's micro_speech toolchain

Accuracy trade-off: TFLite Micro wake word models have higher false positive rates than larger models. Acceptable for home use; tune the detection threshold to preference.

### Alternative: Dedicated Wake Word IC

For lower false-positive rates and near-zero idle power draw, a dedicated wake word processor can be wired to the ESP32S3 over SPI/UART:

| IC | Notes |
|---|---|
| Syntiant NDP101 | 140µW always-on, I2S mic input, SPI to host |
| Knowles IA8201 | Multi-stage DSP + neural net, enterprise-grade accuracy |

Worth considering if TFLite Micro accuracy proves insufficient in a noisy room.

---

## Server-Side Changes Required

### 1. `services/voice/src/transcribe.py` — accept WAV format

```python
def transcribe(audio_bytes: bytes, fmt: str = "webm") -> str:
    suffix = ".wav" if fmt == "wav" else ".webm"
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
        tmp.write(audio_bytes)
        tmp_path = tmp.name
    # rest unchanged
```

### 2. `services/voice/src/converse.py` — pass format from config to transcribe

Parse `audio_format` from the config message and thread it through to `transcribe_audio()`. The config message is already parsed in the WebSocket handler; `audio_format` defaults to `"webm"` for browser clients.

### 3. TLS certificate trust

The server uses a self-signed certificate. The device configures mbedTLS with `MBEDTLS_SSL_VERIFY_NONE` for LAN-only use. Alternatively, burn `selfsigned.crt` into device flash and configure mbedTLS to trust it explicitly.

---

## WiFi Provisioning

The device needs WiFi credentials (SSID + password) at boot. Three approaches, in order of complexity:

### Option 1: Hardcoded (prototype)

Credentials compiled directly into `prj.conf`:

```kconfig
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PSK="YourPassword"
```

Fastest to get running. Fine for a single fixed device you own. Requires recompile to change networks.

### Option 2: BLE Provisioning (recommended for production)

Device advertises as a BLE peripheral on first boot (or when BOOT button is held at startup). A phone connects and writes credentials to GATT characteristics. The device saves to NVS flash via Zephyr's settings subsystem and connects to WiFi automatically. All subsequent boots skip BLE and read from NVS directly.

**Flow:**

```
Hold BOOT at startup
  → device advertises: "Butler-Node"
  → phone connects via BLE
  → phone writes SSID to characteristic 1
  → phone writes password to characteristic 2
  → device calls settings_save() → stores to NVS
  → device stops BLE, connects WiFi
  → (optional) device notifies phone "WiFi connected OK"

Subsequent boots
  → NVS has credentials → skip BLE → connect WiFi directly
```

**Zephyr config required:**

```kconfig
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_GATT=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
CONFIG_NVS=y
CONFIG_WIFI_CREDENTIALS=y
```

**Security:** BLE GATT writes are unencrypted by default. For a home device, limiting advertisement to "BOOT button held" is sufficient — the provisioning window is only open when you deliberately hold the button. Pairing with a passkey (`CONFIG_BT_SMP=y`) can be added if desired.

**Companion app options:**

| Option | Notes |
|---|---|
| nRF Connect (Android/iOS) | Free dev app, write characteristics manually. Good for testing. |
| Web Bluetooth page | Host a provisioning page at `https://vulcan.local/provision/`. Chrome on Android only — iOS not supported. No app install needed. |
| Custom app | React Native / Swift / Kotlin for polished UX. |

### Option 3: Improv WiFi over BLE

[Improv WiFi](https://www.improv-wifi.com/) is an open standard for BLE provisioning that Home Assistant supports natively. If HA integration is desired, implementing Improv WiFi means HA can provision the device directly from its dashboard — no companion app needed.

Same BLE hardware path as Option 2, but uses a standardised GATT service UUID and packet format instead of a custom one. Adds HA autodiscovery for free.

**Use this if:** you want the node to appear in Home Assistant as a managed device.
**Use Option 2 if:** you want full control without HA dependency.

---

## Open Questions

| Question | Notes |
|---|---|
| Zephyr ESP32S3 WiFi stability | Zephyr WiFi uses Espressif binary blobs — less battle-tested than ESP-IDF. Worth prototyping the WiFi + TLS + WebSocket path first before building audio on top. |
| I2S @ 22050Hz | Non-power-of-two sample rate. Supported on ESP32S3 but verify Zephyr I2S driver handles it correctly with the specific clock configuration. |
| Wake word false positive rate | In a noisy home environment TFLite Micro models may trigger occasionally. Tune threshold during testing; consider dedicated IC if unacceptable. |
| VAD sensitivity | Energy-based VAD is simple but may cut off trailing words in a quiet room or fail to detect end-of-speech in a noisy one. May need tuning or a lightweight VAD model. |
| Multi-room | Each room gets one node. Nodes are independent WebSocket clients; the server handles them as separate sessions. No cross-node coordination needed. |

---

## Prototype Milestones

1. **WiFi + TLS + WebSocket** — connect to `wss://vulcan.local/voice/converse`, send a text message, receive a response. Validate the networking stack before touching audio.
2. **PDM mic → PCM → WAV → WebSocket send** — record audio, construct WAV header, send to server, confirm server transcribes correctly.
3. **Binary WebSocket receive → I2S → speaker** — receive a binary WAV frame from the server, play through MAX98357A. Validate 22050Hz I2S clock.
4. **Full round-trip** — wake by button press (not wake word), speak, hear response end-to-end.
5. **Wake word** — integrate TFLite Micro model, replace button press with keyword detection.
6. **VAD** — replace fixed timeout with energy-based voice activity detection.
