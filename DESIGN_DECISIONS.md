# Design Decisions

A structured record of architectural choices made across the Home AI Assistant project — what was decided, what alternatives existed, and why.

---

## 1. System Architecture

### Local-only inference — no cloud APIs

**Decision:** All STT (Whisper), LLM (Llama 4 Scout via Ollama), TTS (Piper), and embeddings run on the local server. No calls to OpenAI, Google, or any external API.

**Alternatives:** Cloud APIs for any/all inference; hybrid (local with cloud fallback).

**Why:** Privacy is a hard requirement — audio, documents, and conversation history never leave the LAN. Secondary benefits: no per-inference cost, no external availability dependency, lower round-trip latency than cloud APIs for a single-digit household.

---

### Onyx as the RAG platform (pre-built Docker images, not forked)

**Decision:** Use `onyxdotapp/onyx-*:latest` images from Docker Hub for the full RAG stack (web, API, background workers, PostgreSQL, Vespa, Redis). No fork, no custom build.

**Alternatives:** Build Onyx from source; use a different RAG platform (LangChain, LlamaIndex); build indexing from scratch.

**Why:** Onyx is production-grade, actively maintained, and provides vector indexing, connector infrastructure, access control, and a usable chat UI out of the box. Pulling pre-built images means upstream security patches and feature updates apply automatically. The custom value in this project is the voice layer, the embedded device, and the integrations — not reinventing document retrieval.

---

### Separate services for webui, voice, and nginx (not a monolith)

**Decision:** Three custom services (`webui`, `voice`, `nginx`) alongside the Onyx containers. Each has a single responsibility and is independently deployable.

**Alternatives:** Single FastAPI service serving UI + API; embed voice logic in Onyx; single nginx serving static + proxying.

**Why:** The web UI (Vite + TypeScript) and the voice backend (Python + WebSocket + ML) have completely different build toolchains, runtimes, and failure modes. Keeping them separate means a voice service crash doesn't take down the UI, and rebuilding the frontend doesn't restart the WebSocket server. Any client — browser, PWA, or embedded device — can speak the same `/voice/converse` API.

---

### Docker Compose as the unifying deployment model

**Decision:** Every service — Onyx, Ollama, PostgreSQL, Vespa, Redis, `webui`, `voice`, and `nginx` — runs as a Docker container orchestrated by a single `docker-compose.yml`. Each service has a single responsibility and is independently startable, stoppable, and replaceable.

**Alternatives:** Kubernetes; Nomad; some services in Docker + others as systemd units; monolith.

**Why:** The primary driver was **separation of concerns at the deployment boundary**, not just development convenience. Onyx ships as Docker images with no native install path — accepting that deployment model and extending it consistently means every service speaks the same lifecycle language. A voice service crash doesn't take down the RAG layer. The Vite frontend can be rebuilt and redeployed without restarting the WebSocket server. Swapping the TTS model is a single container rebuild.

The secondary driver is the **inter-service dependency graph**: Onyx requires Vespa, Redis, and Postgres to be healthy before its API is ready. Compose's `depends_on: condition: service_healthy` enforces this declaratively — without containers, you'd manage startup ordering with fragile shell scripts or systemd unit dependencies.

The third constraint is **GPU sharing**: `runtime: nvidia` in compose is the standard mechanism to give Whisper, Ollama, and the embedding model access to the RTX 5090 without CUDA library version conflicts between services. Managing that on bare metal across Python environments is error-prone.

Kubernetes would add scheduler, etcd, and control plane overhead that is disproportionate for a single-machine homelab. Compose is the right scope.

---

## 2. Frontend

### Static build (Vite + TypeScript → nginx:alpine)

**Decision:** Vite compiles TypeScript + CSS to content-hashed static assets. The production container is `nginx:alpine` with only `/dist/`. No Node.js at runtime.

**Alternatives:** Server-side rendering; React SPA with Node.js server; plain HTML/JS.

**Why:** The Web Audio + WebSocket code benefits from TypeScript's type safety (correct buffer types, correct event shapes). Vite produces content-hashed filenames (`main.a3f2c1.js`) so assets can be cached for one year — repeat visitors pay zero download cost. The production image is ~30MB because Node.js is only in the build stage (multi-stage Dockerfile).

---

### Content-hashed assets + `Cache-Control: max-age=31536000` for JS/CSS; `no-cache` for `index.html`

**Decision:** JS and CSS filenames include a content hash and are cached aggressively. `index.html` is never cached.

**Why:** Content hash = the filename changes only when the file changes. Users always get the correct version without needing a versioned path or cache-busting query string. `index.html` is small and must always be fresh so clients pick up new asset filenames on deploy.

---

### AudioContext keepalive (silent oscillator)

**Decision:** The browser maintains a persistent Web Audio context via a zero-gain oscillator, even when no audio is playing.

**Why:** Some operating systems gate audio device power until the first `play()` call. When TTS arrives, the OS takes 200-400ms to reopen the audio device, clipping the first syllable. The silent oscillator signals to the OS that audio is active, keeping the device warm. CPU cost is negligible.

---

## 3. Voice Pipeline

### WebSocket for voice communication (not HTTP polling or SSE)

**Decision:** `WS /voice/converse` handles the full voice turn — audio upload, token streaming, and WAV playback — in a single persistent connection.

**Alternatives:** HTTP POST for audio upload + SSE for streaming response; gRPC bidirectional; polling.

**Why:** Voice needs both directions simultaneously: the client streams audio chunks while the server streams tokens back. SSE is unidirectional. HTTP POST requires the client to accumulate all audio before uploading, adding latency. WebSocket binary frames carry audio with no base64 overhead, and the same connection is reused for text tokens and WAV frames.

---

### Sentence-boundary TTS with concurrent synthesis

**Decision:** During LLM token streaming, the server detects sentence boundaries (`.`, `?`, `!`). Each completed sentence is synthesized by Piper in `asyncio.to_thread()` while the LLM continues generating. The browser queues and plays WAV chunks as they arrive.

**Alternatives:** TTS the full response after LLM completes; TTS every N tokens; TTS per word.

**Why:** Latency. If TTS waits for the full LLM response, the user waits 3-8 seconds after speaking before hearing anything. With sentence-boundary streaming, the first sentence plays within ~2 seconds of the query completing. Sentence-level chunks give the TTS model enough context for correct prosody. Piper's ~0.5s synthesis time per sentence overlaps with the LLM generating the next sentence — the pipeline is parallel, not sequential.

---

### Whisper (faster-whisper, medium, CUDA float16)

**Decision:** Speech-to-text uses the `faster-whisper` library with the `medium` model, CUDA compute type `float16`.

**Alternatives:** Google Speech-to-Text API; Vosk; Whisper large (higher accuracy, slower); Whisper base (faster, lower accuracy).

**Why:** `faster-whisper` is a CTranslate2 re-implementation of Whisper — 2-10x faster than the original. `float16` on the RTX 5090 halves memory bandwidth vs `float32`. The `medium` model is the practical balance point: transcription in ~0.5s for a 5-second utterance, with accuracy high enough for natural household speech. The large model adds marginal accuracy for 3x the latency.

---

### Piper for TTS (local, ONNX-based)

**Decision:** Text-to-speech uses Piper with a pre-downloaded ONNX voice model.

**Alternatives:** ElevenLabs API; Google TTS API; espeak; Coqui TTS.

**Why:** Piper is fully local (no cloud dependency), fast enough on CPU (~0.5s per sentence), and produces neural-quality voice (not robotic). ONNX runtime is portable across CPU/GPU. Model files (~80MB) bake into the voice service image at build time — no startup download delay.

---

### Server-side STT/LLM/TTS orchestration (not client-side)

**Decision:** The voice service orchestrates the full pipeline. Clients stream audio in and receive WAV frames out.

**Why:** Any client — browser, PWA, or a $14 embedded microcontroller — gets identical behavior without implementing the pipeline themselves. The embedded device cannot run Whisper. The browser could theoretically run Whisper.wasm, but that wastes the RTX 5090 sitting on the LAN. Centralising orchestration also means updating the pipeline (swapping models, adding RAG context) without touching any client.

---

## 4. Embedded Firmware

### Zephyr RTOS with AMP (two independent images, one per core)

**Decision:** Each ESP32-S3 LX7 core runs a separate Zephyr image (`procpu` on Core 0, `appcpu` on Core 1), built together via `west sysbuild`.

**Alternatives:** SMP (single image, shared scheduler); FreeRTOS; bare metal.

**Why:** Zephyr 4.3.0 does not implement `arch_cpu_start` for the ESP32-S3, making SMP unavailable. Beyond that constraint, AMP is the right model anyway: the two workloads are orthogonal. Core 0 is I/O-bound (WiFi, TLS, WebSocket, I2S). Core 1 is compute-bound (TFLite Micro inference, PDM capture). Running them on separate schedulers avoids shared-scheduler contention, cache-coherency issues, and spinlock complexity. Sysbuild compiles and flashes both images atomically with a single `west build --sysbuild` command.

---

### IPM (espressif,esp32-ipm) for inter-core communication

**Decision:** Cores communicate via Zephyr's `espressif,esp32-ipm` driver — a 1KB shared memory mailbox. One message in-flight at a time. The appcpu sends with `ipm_send(wait=1)`.

**Alternatives:** Shared variables + spinlocks; dedicated UART; SPI; larger message queue.

**Why:** IPM is the hardware-supported IPC primitive on ESP32 — low overhead, no custom protocol. The single-slot constraint (no queue) is a real limitation: sending two messages back-to-back with `wait=0` overwrites the first before the receiver reads it. Using `wait=1` (blocking) ensures each send completes before the next. This was discovered empirically — the appcpu heartbeat tick was clobbering the log string when both were sent without waiting. The fix: one send per loop iteration, `wait=1`.

---

### USB Serial/JTAG owned by procpu; appcpu logs forwarded via IPM

**Decision:** The USB Serial/JTAG peripheral is initialized by the procpu driver. The appcpu cannot write to it independently. Instead, the appcpu formats log strings via `vsnprintf` and sends them to the procpu via IPM, which prints them with a `[C1]` prefix.

**Why:** The USB Serial/JTAG peripheral is a single hardware unit. Its flush mechanism (the register write that triggers TX) is driver-controlled. If both cores' drivers initialize it, the second initialization silently fails — TX writes from that core never flush. This was confirmed by inspecting the compiled DTS (the appcpu overlay correctly set `zephyr,console = &usb_serial`) but observing zero output regardless. The hardware ownership constraint is fundamental. IPM log forwarding is the canonical AMP pattern for shared console output.

---

### TFLite Micro wake word on APP CPU (Core 1), not PRO CPU

**Decision:** TFLite Micro inference runs continuously on Core 1. When a wake word is detected, Core 1 sends a wake event to Core 0 via IPM.

**Alternatives:** Wake word on Core 0; dedicated wake word IC (Syntiant NDP101); cloud-based detection.

**Why:** Isolating inference on Core 1 keeps Core 0 free for WiFi, TLS, and WebSocket operations. An inference spike on Core 0 would stall the network stack, causing WebSocket timeouts or audio jitter. Running inference on Core 1 means inference latency and I/O latency are independent. The ESP32-S3 also includes PIE (vector instruction extensions) that accelerate TFLite Micro operations, reducing Core 1 load.

---

### LittleFS for credential storage (not Zephyr NVS)

**Decision:** WiFi credentials are stored in a LittleFS filesystem mounted at `/lfs` on the `storage_partition` (192KB at flash offset `0x3b0000`). Credentials are written to `/lfs/wifi.conf` as plain text key-value pairs.

**Alternatives:** Zephyr NVS (key-value flash store); hardcoded at build time; Settings subsystem.

**Why:** LittleFS provides a standard POSIX-like file API (`fs_open`, `fs_read`, `fs_write`), which is easier to extend (multiple credential sets, configuration keys, calibration data) than NVS's bitfield model. LittleFS has built-in wear leveling and power-fail-safe writes. The storage partition is separate from the application image partitions, so credentials survive OTA firmware updates. The manual `fs_mount()` call in `storage.c` (rather than DTS `automount`) makes the initialization sequence explicit and error-handling straightforward.

---

### No persistent WebSocket — open on wake, close after playback

**Decision:** The WebSocket connection opens when the wake word fires and closes after the audio response drains. Idle state carries no open socket.

**Alternatives:** Persistent WebSocket (always connected in idle); reconnect only on failure.

**Why:** A persistent connection requires keepalive heartbeats, reconnection logic, and holds TLS session memory (~150KB) continuously. Since the device is idle most of the time (voice interactions are seconds out of hours), the overhead is disproportionate. A fresh connection on each wake is clean state — no stale half-open sockets, no session expiry edge cases. The TLS handshake adds ~300ms latency to wake, which is acceptable given the acknowledgement chime plays during that window.

---

### PSRAM (8MB OPI) for audio buffers and networking stack

**Decision:** All large allocations — TLS session, networking stack, audio record/playback buffers, TFLite model — live in the 8MB Octal PSRAM. On-chip SRAM holds only the Zephyr kernel, interrupt handlers, and time-critical paths.

**Alternatives:** On-chip SRAM only (limits buffer sizes severely); external SPI DRAM.

**Why:** The on-chip SRAM is 512KB, and after the kernel and WiFi driver reserve their portions, ~300KB is available for the application. A 5-second audio record buffer alone is 160KB. The TLS session is ~150KB. The networking stack is ~80KB. These cannot coexist in on-chip SRAM. The XIAO ESP32S3 Sense includes 8MB OPI PSRAM on-die — using it resolves the memory budget without any additional hardware. The key constraint: `CONFIG_SPIRAM_MODE_OCT=y` is required for this specific board. Quad mode (the Zephyr default) causes a boot crash because the hardware is wired for Octal.

---

### WAV format for device ↔ server audio

**Decision:** The voice node streams audio to the server as WAV (standard RIFF header + raw 16kHz 16-bit mono PCM). The server responds with WAV frames.

**Alternatives:** Raw PCM (no header); opus; MP3; WebM (used by browser clients).

**Why:** WAV is the simplest framed audio format — 44-byte header, then raw PCM. No codec complexity on an embedded device. The server can parse the sample rate from header bytes 24-27, which means the device can report the actual PDM capture rate without a separate signalling message. The response path uses WAV for the same reason: the device reads the sample rate from the WAV header to configure the I2S clock correctly for playback without any out-of-band coordination.

---

## 5. Networking and Security

### HTTPS with self-signed certificate (acceptable for LAN)

**Decision:** nginx auto-generates a self-signed TLS certificate at first start. The embedded device connects with `MBEDTLS_SSL_VERIFY_NONE`. Browsers receive a one-time trust prompt.

**Alternatives:** Let's Encrypt; manually issued certificate; mTLS; plain HTTP.

**Why:** `navigator.mediaDevices.getUserMedia()` (microphone access) is blocked by browsers on non-localhost HTTP. HTTPS is required. Let's Encrypt requires a public domain and ACME challenge, which is impractical for a LAN-only service without a registered hostname. A self-signed cert is the right trade-off: traffic is encrypted against passive eavesdropping; the threat model (LAN-only, trusted home network) doesn't include certificate authority attacks. The certificate is auto-generated in the nginx Dockerfile and stored in a named volume — it persists across restarts without manual renewal.

---

### Lazy DNS resolution in nginx (`resolver 127.0.0.11`)

**Decision:** The nginx voice proxy route uses Docker's embedded DNS resolver (`127.0.0.11`) with a variable upstream, not a hardcoded `proxy_pass`.

```nginx
resolver 127.0.0.11;
set $voice_upstream "voice-service:8765";
proxy_pass http://$voice_upstream;
```

**Why:** If `proxy_pass` uses a hardcoded hostname, nginx resolves it once at startup and caches it. If the `voice-service` container hasn't started yet or restarts and gets a new IP, nginx uses the stale address and 502s. The variable + runtime resolver pattern forces nginx to re-resolve on each request via Docker's DNS, which always returns the current container IP. This means nginx starts successfully even if `voice-service` isn't up yet.

---

### API key server-side only (never sent to browser clients)

**Decision:** The Onyx API key is stored in `.env`, used internally by the voice service, and never included in WebSocket messages or browser responses.

**Why:** The voice UI has no login page — any device on the LAN can use it. If the API key were sent to the browser, any household member (including children) could extract it from browser devtools and make arbitrary Onyx API calls. Keeping it server-side means the voice service is the only client of the Onyx API. The key is rotatable by changing `.env` and restarting the service.

---

### Read-only host mounts for indexed directories

**Decision:** All host filesystem mounts passed to Onyx indexing workers are `:ro` (read-only).

**Why:** Indexing workers only need to read files. A misconfiguration or compromise in an indexing worker cannot delete, overwrite, or corrupt the host filesystem. Principle of least privilege applied at the Docker layer, not just the application layer.

---

## 6. Development Environment

### Embedded toolchain in a Dev Container (not native host install)

**Decision:** Zephyr SDK 0.17.4, `west`, CMake, and all embedded toolchains live inside a Docker dev container. Host files are mounted as a volume.

**Alternatives:** Native Zephyr SDK install; VM; bare metal.

**Why:** Zephyr's native install involves ~2GB of toolchain downloads, Python virtualenvs, and PATH configuration that varies by OS version. Pinning the SDK version in a Dockerfile (`0.17.4`) ensures every contributor (and every CI run) uses identical tools. Rebuilding the container restores the environment from scratch in minutes. The host is not polluted. Flash access works because `/dev` is bind-mounted into the container.

---

### Makefile wrappers for `west build --sysbuild`

**Decision:** `make`, `make clean`, `make flash`, `make menuconfig` wrap the full west sysbuild command.

**Why:** The raw command is:
```
west build --sysbuild -d build -b xiao_esp32s3/esp32s3/procpu/sense procpu \
  -- -Dprocpu_EXTRA_CONF_FILE=credentials.conf
```
That is not something you type from memory. Makefiles are universally understood, tab-completable, and self-documenting. The wrapper also enforces that the build always targets the correct board qualifier, preventing accidental builds against the wrong target.

---

### `credentials.conf` pattern for dev-time WiFi credentials

**Decision:** WiFi SSID and PSK are never hardcoded in `prj.conf` or source. They live in `procpu/credentials.conf` (gitignored), passed at build time via `-Dprocpu_EXTRA_CONF_FILE=credentials.conf`.

**Why:** Hardcoding credentials in tracked files is a common and serious mistake in embedded projects. The `credentials.conf` pattern makes the right thing the default: the file doesn't exist in the repo, the `.gitignore` entry prevents accidental adds, and the build works without it (for CI or blank-credential testing). Production provisioning uses BLE (Milestone 7), not build-time credentials.

---

## 7. Tradeoffs and Known Limitations

| Decision | Tradeoff accepted |
|---|---|
| AMP instead of SMP | Two independent images to maintain; inter-core communication via IPM adds latency and complexity |
| IPM single-slot, no queue | One message in-flight at a time; back-to-back sends must use `wait=1` (blocking) |
| USB Serial/JTAG owned by procpu | appcpu cannot log directly; must forward via IPM; log strings truncated at 128 bytes |
| Self-signed TLS | Browser shows cert warning on first visit; device uses `VERIFY_NONE` (no cert pinning) |
| HEAP_MEM_POOL_SIZE=49152 | DRAM is tight; 64KB overflows DRAM by ~13KB; 48KB fits with margin but limits WiFi stack headroom |
| LittleFS over NVS | More flexible API, but heavier footprint; NVS would be smaller for simple key-value storage |
| Sentence-boundary TTS | Sentence detection on punctuation is fragile for lists, abbreviations, and decimal numbers |
| No persistent WebSocket | ~300ms TLS handshake added to each wake latency |
| Piper TTS (CPU) | Voice quality is good but below neural cloud APIs; no emotional prosody control |
| Onyx pre-built images | Cannot patch Onyx internals; must wait for upstream to fix bugs in RAG layer |
