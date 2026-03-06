# Technical Specification — Home AI Assistant

**Status:** Draft
**Version:** 0.2
**Derives from:** [design-spec.md](design-spec.md)

---

## 1. Technology Stack

### 1.1 Existing Stack (Phases 1–2)

| Component | Technology | Version |
|---|---|---|
| LLM | Llama 4 Scout via Ollama | `ollama/ollama:latest` |
| RAG Platform | Onyx | `onyxdotapp/onyx-backend:latest` |
| Embeddings | Onyx Model Server (2x) | `onyxdotapp/onyx-model-server:latest` |
| Vector Store | Vespa | `8.609.39` |
| Database | PostgreSQL | `15.2-alpine` |
| Cache / Queue | Redis | `7.4-alpine` |
| Web UI | Onyx Next.js | `onyxdotapp/onyx-web-server:latest` |
| Reverse Proxy | Nginx | `1.25.5-alpine` (custom build — adds openssl, self-signed TLS entrypoint) |
| GPU Runtime | NVIDIA Container Toolkit | host-installed |

### 1.2 New Services

| Service | Technology | Rationale |
|---|---|---|
| **Web UI Service** | Vite + TypeScript + nginx:alpine | Static frontend for voice assistant; built at image build time, served with long-lived cache headers |
| **Voice Service** | Python + FastAPI | WebSocket orchestrator for STT → LLM → TTS pipeline; REST endpoints for direct STT/TTS access |
| **STT** | OpenAI Whisper (local) | Best local accuracy; already used by Onyx model server base image |
| **TTS** | Piper TTS | Fast, lightweight, fully local, good voice quality |
| **HA Bridge** | Python + FastAPI | Thin proxy between Onyx and Home Assistant REST API |
| **Pi Agent** | Python | Runs on Raspberry Pi; OpenWakeWord + WebSocket client |

### 1.3 Mobile PWA

- Built as a standard web app (React or plain HTML/JS) served by Nginx
- Uses **Web Speech API** for mic capture in browser
- Uses **Web Push API** for notifications
- Installable via browser "Add to Home Screen"
- No app store submission required

---

## 2. New Services — Detailed Design

### 2.1 Voice Service

**Container:** `voice-service`
**Base image:** `nvidia/cuda:12.8.0-cudnn-runtime-ubuntu22.04`
**Ports:** `8765` (internal only, proxied by Nginx)

**Dependencies (`requirements.txt`):**
```
faster-whisper==1.1.1    # GPU-accelerated Whisper inference
requests==2.32.3         # unlisted faster-whisper dependency
piper-tts==1.2.0         # Local TTS
fastapi==0.115.0
uvicorn[standard]==0.30.6
python-multipart==0.0.12 # multipart form upload for audio
httpx==0.27.2
soundfile==0.12.1
numpy==1.26.4
```

**Environment variables:**
```
ONYX_API_KEY=<onyx-api-key>   # basic-role key; authenticates voice service to Onyx API
WHISPER_MODEL=medium
PIPER_VOICE=en_US-lessac-medium
CACHE_DIR=/app/.cache
```

**Endpoints (implemented — Phase 4):**

```
WS  /voice/converse
  Bidirectional WebSocket — full-duplex voice chat protocol:
  → Client sends: TEXT  { "type": "config", "tts": true }       optional on connect
  → Client sends: TEXT  { "type": "text_input", "message": "…" } text query
  → Client sends: BINARY raw audio chunks (webm/opus)            voice query
  → Client sends: TEXT  { "type": "end_audio" }                  signals recording done
  ← Server sends: TEXT  { "type": "transcript", "text": "…" }   STT result
  ← Server sends: TEXT  { "type": "token", "content": "…" }     LLM streaming token
  ← Server sends: BINARY WAV audio bytes                         TTS per sentence
  ← Server sends: TEXT  { "type": "done" }                       turn complete
  ← Server sends: TEXT  { "type": "error", "detail": "…" }      on failure

GET  /voice/health
  Response: { "status": "ok" }

POST /voice/transcribe
  Body: multipart/form-data — audio file (webm/wav)
  Response: { "text": "transcribed string" }

POST /voice/synthesize
  Body: { "text": "string to speak" }
  Response: audio/wav binary
```

**Note:** The voice chat HTML/JS/CSS is served by `services/webui` (nginx:alpine), not this service.

**Endpoints (Phase 6 — Pi agent):**

The Pi agent uses the existing `WS /voice/converse` protocol — no new endpoint needed. The wire protocol already supports binary audio in and WAV frames out.

**Whisper configuration:**
```python
model_size = "medium"        # balance of speed vs accuracy
device = "cuda"              # uses RTX 5090 via NVIDIA CTK
compute_type = "float16"     # matches GPU capability
language = "en"              # auto-detect if multilingual needed
```

**Piper configuration:**
- Default voice: `en_US-lessac-medium`
- Voice model files (`.onnx` + `.onnx.json`) downloaded at **image build time** and baked into the image — no startup download delay
- Runs on CPU (GPU not needed for TTS at this scale)

**Nginx routing additions:**
```nginx
upstream webui {
    server webui:80;
}

# Voice UI — HTML page (exact match, highest nginx priority)
location = /voice/ {
    proxy_pass http://webui/voice/;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
}

# Voice UI — hashed JS/CSS assets (1-year immutable cache via webui nginx)
location /voice/static/ {
    proxy_pass http://webui/voice/static/;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
}

# Voice API + WebSocket
# Lazy DNS resolution so nginx starts even if voice-service isn't up yet
location /voice/ {
    resolver 127.0.0.11 valid=10s;
    set $voice_upstream "voice-service:8765";
    proxy_pass http://$voice_upstream;   # no path suffix — nginx passes full URI unchanged
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_buffering off;
    proxy_read_timeout 300s;
    proxy_send_timeout 300s;
}
```

> **Note:** nginx location priority — exact (`= /voice/`) > longest prefix (`/voice/static/`) > general prefix (`/voice/`). This ensures HTML and assets route to `webui` while API and WebSocket traffic routes to `voice-service`. When `proxy_pass` uses a variable, nginx passes the full original URI unchanged — do not append a path suffix to avoid double-path bugs.

---

### 2.2 HA Bridge

**Container:** `ha-bridge`
**Base image:** `python:3.11-slim`
**Ports:** `8766` (internal only)

**Environment variables:**
```
HA_URL=http://<home-assistant-ip>:8123
HA_TOKEN=<long-lived-access-token>
```

**Dependencies:**
```
fastapi
uvicorn
httpx
```

**Endpoints:**

```
GET /ha/entities
  Response: [{ "entity_id": "light.bedroom", "state": "on", "attributes": {...} }]

POST /ha/action
  Body: {
    "entity_id": "light.bedroom",
    "domain": "light",
    "service": "turn_on",
    "params": { "brightness_pct": 50 }
  }
  Response: { "success": true, "result": {...} }

GET /ha/state/{entity_id}
  Response: { "entity_id": "...", "state": "...", "attributes": {...} }
```

**HA Bridge calls Home Assistant REST API:**
```
POST http://<ha-host>:8123/api/services/{domain}/{service}
Authorization: Bearer <HA_TOKEN>
```

**Intent routing in Onyx API:**
The Onyx system prompt is augmented with available entities and a function-calling schema. When the LLM outputs a structured tool call for `ha_action`, the Onyx API layer calls the HA Bridge before composing the final response.

---

### 2.3 Raspberry Pi Agent

**Hardware:** Raspberry Pi 4 (2GB+ RAM) or Pi 5
**OS:** Raspberry Pi OS Lite (64-bit)
**Mic:** ReSpeaker 4-mic array (USB or HAT) or similar
**Speaker:** USB speaker or 3.5mm analog

**Software dependencies:**
```
openWakeWord          # wake word detection (local, lightweight)
pyaudio               # mic capture
websockets            # connection to Voice Service
sounddevice           # audio playback
numpy
```

**Pi agent state machine:**
```
IDLE
  → mic always listening via OpenWakeWord
  → CPU usage: ~5-10% on Pi 4

WAKE_WORD_DETECTED
  → play acknowledgement chime
  → open WebSocket to wss://<host-ip>/voice/converse
  → transition to RECORDING

RECORDING
  → stream mic audio frames to Voice Service
  → listen for silence (VAD) or timeout (8s)
  → send { "event": "end_of_speech" }
  → transition to WAITING

WAITING
  → receive transcript JSON from server (confirmation)
  → receive TTS audio frames
  → transition to PLAYING

PLAYING
  → play received audio through speaker
  → on completion → transition to IDLE
  → on error → play error chime → transition to IDLE
```

**Pi systemd service:**
```ini
[Unit]
Description=Home Assistant Pi Agent
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 /home/pi/agent/main.py
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

---

## 3. Data Models

### 3.1 User & Permissions (new tables in PostgreSQL)

```sql
-- Extends Onyx's existing users table
CREATE TABLE user_profiles (
    user_id     UUID PRIMARY KEY REFERENCES users(id),
    role        VARCHAR(20) NOT NULL DEFAULT 'member',
                -- 'owner' | 'member' | 'child'
    profile_id  UUID REFERENCES access_profiles(id)
);

CREATE TABLE access_profiles (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            VARCHAR(100) NOT NULL,
    web_search      BOOLEAN DEFAULT false,
    ha_access       VARCHAR(20) DEFAULT 'none',
                    -- 'none' | 'read' | 'full'
    created_by      UUID REFERENCES users(id)
);

CREATE TABLE profile_connectors (
    profile_id      UUID REFERENCES access_profiles(id),
    connector_id    INT REFERENCES connector(id),
    PRIMARY KEY (profile_id, connector_id)
);

CREATE TABLE parental_controls (
    user_id             UUID PRIMARY KEY REFERENCES users(id),
    blocked_topics      TEXT[],
    allowed_start_time  TIME,   -- e.g. 07:00
    allowed_end_time    TIME    -- e.g. 21:00
);
```

### 3.2 Voice Session (in-memory / Redis)

```
voice_session:{session_id}
  user_id:      string
  mode:         'browser' | 'pwa' | 'ambient'
  started_at:   timestamp
  transcript:   string (built up as chunks arrive)
  state:        'recording' | 'transcribing' | 'generating' | 'speaking'
  ttl:          60s
```

### 3.3 HA Entity Cache (Redis)

```
ha:entities           → JSON array of all entities (cached 30s)
ha:state:{entity_id}  → JSON state object (cached 10s)
```

---

## 4. Infrastructure Changes by Phase

### Phase 3 — File Indexing

Add volume mounts to `background` and `api_server` in `docker-compose.yml`:

```yaml
volumes:
  - /home/${USER}/Documents:/mnt/indexed/documents:ro
  - /home/${USER}/Pictures:/mnt/indexed/photos:ro
  - /home/${USER}/Videos:/mnt/indexed/videos:ro
```

Configure connectors in Onyx admin UI pointing to `/mnt/indexed/`.

---

### Phase 4 — Voice Service + Web UI Service *(implemented)*

Add to `docker-compose.yml`:

```yaml
webui:
  build: ./services/webui
  container_name: onyx-webui
  restart: unless-stopped
  healthcheck:
    test: ["CMD", "wget", "-qO-", "http://localhost/voice/"]
    interval: 30s
    timeout: 5s
    retries: 3

voice-service:
  build: ./services/voice
  container_name: onyx-voice
  restart: unless-stopped
  runtime: nvidia              # Whisper uses GPU
  environment:
    - NVIDIA_VISIBLE_DEVICES=all
    - WHISPER_MODEL=medium
    - PIPER_VOICE=en_US-lessac-medium
    - CACHE_DIR=/app/.cache
    - ONYX_API_KEY=${ONYX_API_KEY}
  volumes:
    - voice_model_cache:/app/.cache
  # No exposed ports — internal only, accessed via nginx proxy
  healthcheck:
    test: ["CMD", "python", "-c", "import urllib.request; urllib.request.urlopen('http://localhost:8765/voice/health')"]
    interval: 30s
    timeout: 10s
    retries: 3
    start_period: 60s
```

Also add `voice_model_cache:` to the top-level `volumes:` section, and add `webui` and `voice-service` to nginx's `depends_on`.

Add to `.env`:
```
ONYX_API_KEY=<basic-role key from Onyx UI → Settings → API Keys>
```

Add to `services/nginx/nginx.conf` (see routing note in §2.1):
```nginx
upstream webui { server webui:80; }

location = /voice/      { proxy_pass http://webui/voice/; ... }
location /voice/static/ { proxy_pass http://webui/voice/static/; ... }
location /voice/        { resolver 127.0.0.11 valid=10s; set $u "voice-service:8765"; proxy_pass http://$u; ... }
```

See `services/nginx/nginx.conf` for the full block (WebSocket upgrade headers, timeouts).

---

### Phase 5 — HA Bridge

Add to `docker-compose.yml`:

```yaml
ha-bridge:
  build: ./services/ha-bridge
  container_name: onyx-ha-bridge
  restart: unless-stopped
  environment:
    - HA_URL=${HA_URL}
    - HA_TOKEN=${HA_TOKEN}
  ports:
    - "8766:8766"
```

Add to `.env.example`:
```
# ── Home Assistant ────────────────────────────────────────────────────────────
HA_URL=http://192.168.1.x:8123
HA_TOKEN=
```

---

### Phase 6 — Raspberry Pi

**On the Pi (one-time setup):**
```bash
git clone <repo> && cd home_ai_assistant/pi-agent
pip install -r requirements.txt
sudo cp pi-agent.service /etc/systemd/system/
sudo systemctl enable --now pi-agent
```

**Pi agent config (`pi-agent/config.yaml`):**
```yaml
server:
  host: 192.168.1.x      # assistant host static IP
  ws_path: /voice/converse

audio:
  sample_rate: 16000
  channels: 1
  chunk_size: 1024
  silence_threshold: 500
  silence_timeout_ms: 1500

wake_word:
  model: hey_computer     # OpenWakeWord model
  threshold: 0.5

chime:
  wake: sounds/wake.wav
  error: sounds/error.wav
```

---

## 5. Implementation Order

Each phase is self-contained and can be stopped/started independently.

| Phase | Key tasks | New files | Status |
|---|---|---|---|
| **2 — Homelab** | Set static IP, update `WEB_DOMAIN`, `docker compose up` on rack | `docker-compose.yml` env update | — |
| **3 — File Indexing** | Add volume mounts, configure connectors in Onyx UI | `docker-compose.yml` update | — |
| **4 — Voice** | Voice Service (WS orchestration + STT/TTS); Web UI Service (Vite + nginx); standalone `/voice/` UI; ONYX_API_KEY auth; HTTPS (self-signed TLS, cert auto-generated in nginx container) | `services/voice/`, `services/webui/`, `services/nginx/` | **Done** |
| **5 — Smart Home** | Build HA Bridge, extend Onyx system prompt, add HA env vars | `services/ha-bridge/` | — |
| **6 — Pi Agent** | Build Pi agent, deploy to Pi — uses existing `WS /voice/converse` | `pi-agent/` | — |
| **7 — Multi-user** | Add DB tables, build admin UI for user/profile management | DB migration, UI additions | — |
| **8 — Phone Upload** | Add upload endpoint, connector for uploaded files | `services/voice/` or new service | — |
| **9 — Network Shares** | Mount SMB/NFS, add connectors in Onyx | `docker-compose.yml` update | — |
| **10 — Deep Media** | Add vision model service, update indexing pipeline | `services/vision/` | — |
| **11 — Web Search** | Configure search API connector in Onyx admin | `.env` update | — |

---

## 6. Security Considerations

| Area | Approach |
|---|---|
| **Secrets** | All credentials in `.env` (git-ignored). HA token stored server-side only, never exposed to client. |
| **Network exposure** | Ports 80 (HTTP→HTTPS redirect) and 443 (HTTPS/TLS) exposed to LAN. All internal service ports are container-internal. |
| **Pi WebSocket** | Authenticated via a shared pre-configured token in `config.yaml`. Rotate on compromise. |
| **File mounts** | All host mounts are `:ro` (read-only). Containers cannot write to host filesystem. |
| **Audio privacy** | Audio streams processed entirely on LAN. No audio sent outside the network. |
| **Parental controls** | Enforced server-side — cannot be bypassed by the client. Time restrictions enforced at API layer. |
| **TLS** | Self-signed cert auto-generated inside the nginx container on first start, stored in a named Docker volume (`nginx_certs`). Required for `navigator.mediaDevices` mic access (browsers block on plain HTTP). Set `CERT_HOSTNAME` in `.env` (default: `vulcan.local`). |
