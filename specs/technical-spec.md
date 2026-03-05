# Technical Specification — Home AI Assistant

**Status:** Draft
**Version:** 0.1
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
| Reverse Proxy | Nginx | `1.25.5-alpine` |
| GPU Runtime | NVIDIA Container Toolkit | host-installed |

### 1.2 New Services

| Service | Technology | Rationale |
|---|---|---|
| **Voice Service** | Python + FastAPI | Thin wrapper around Whisper + Piper, serves HTTP + WebSocket |
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
**Base image:** `python:3.11-slim`
**Ports:** `8765` (internal only, proxied by Nginx)

**Dependencies:**
```
faster-whisper       # GPU-accelerated Whisper inference
piper-tts            # Local TTS
fastapi
uvicorn
websockets
soundfile
numpy
```

**Endpoints:**

```
POST /voice/transcribe
  Body: audio/wav or audio/webm binary
  Response: { "text": "transcribed string" }

POST /voice/synthesize
  Body: { "text": "string to speak", "voice": "en_US-lessac-medium" }
  Response: audio/wav binary

WS /voice/stream
  Bidirectional WebSocket:
  → Client sends: binary audio frames (16kHz mono PCM)
  → Client sends: JSON { "event": "end_of_speech" }
  ← Server sends: JSON { "transcript": "..." }
  ← Server sends: binary audio frames (TTS response)
```

**Whisper configuration:**
```python
model_size = "medium"        # balance of speed vs accuracy
device = "cuda"              # uses RTX 5090 via NVIDIA CTK
compute_type = "float16"     # matches GPU capability
language = "en"              # auto-detect if multilingual needed
```

**Piper configuration:**
- Default voice: `en_US-lessac-medium`
- Voice files downloaded at container startup if not cached
- Runs on CPU (GPU not needed for TTS at this scale)

**Nginx routing additions:**
```nginx
location /voice/ {
    proxy_pass http://voice-service:8765/;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_buffering off;
    proxy_read_timeout 120s;
}
```

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
  → open WebSocket to wss://<host-ip>/voice/stream
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

### Phase 4 — Voice Service

Add to `docker-compose.yml`:

```yaml
voice-service:
  build: ./services/voice
  container_name: onyx-voice
  restart: unless-stopped
  runtime: nvidia              # Whisper uses GPU
  environment:
    - NVIDIA_VISIBLE_DEVICES=all
    - ONYX_API_URL=http://api_server:8080
    - WHISPER_MODEL=medium
    - PIPER_VOICE=en_US-lessac-medium
  volumes:
    - voice_model_cache:/app/.cache
  ports:
    - "8765:8765"
```

Add to `nginx/nginx.conf`:
```nginx
location /voice/ {
    proxy_pass http://voice-service:8765/;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_buffering off;
    proxy_read_timeout 120s;
}
```

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
  ws_path: /voice/stream

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

| Phase | Key tasks | New files |
|---|---|---|
| **2 — Homelab** | Set static IP, update `WEB_DOMAIN`, `docker compose up` on rack | `docker-compose.yml` env update |
| **3 — File Indexing** | Add volume mounts, configure connectors in Onyx UI | `docker-compose.yml` update |
| **4 — Voice** | Build voice service, add Nginx route, add mic button to UI | `services/voice/`, Nginx update |
| **5 — Smart Home** | Build HA Bridge, extend Onyx system prompt, add HA env vars | `services/ha-bridge/` |
| **6 — Pi Agent** | Build Pi agent, deploy to Pi, add WebSocket Nginx route | `pi-agent/` |
| **7 — Multi-user** | Add DB tables, build admin UI for user/profile management | DB migration, UI additions |
| **8 — Phone Upload** | Add upload endpoint, connector for uploaded files | `services/voice/` or new service |
| **9 — Network Shares** | Mount SMB/NFS, add connectors in Onyx | `docker-compose.yml` update |
| **10 — Deep Media** | Add vision model service, update indexing pipeline | `services/vision/` |
| **11 — Web Search** | Configure search API connector in Onyx admin | `.env` update |

---

## 6. Security Considerations

| Area | Approach |
|---|---|
| **Secrets** | All credentials in `.env` (git-ignored). HA token stored server-side only, never exposed to client. |
| **Network exposure** | Only port 80 exposed to LAN. All internal service ports are container-internal. |
| **Pi WebSocket** | Authenticated via a shared pre-configured token in `config.yaml`. Rotate on compromise. |
| **File mounts** | All host mounts are `:ro` (read-only). Containers cannot write to host filesystem. |
| **Audio privacy** | Audio streams processed entirely on LAN. No audio sent outside the network. |
| **Parental controls** | Enforced server-side — cannot be bypassed by the client. Time restrictions enforced at API layer. |
| **Future: TLS** | Add Nginx SSL (Let's Encrypt or self-signed) when exposing beyond LAN (Tailscale handles this for remote access). |
