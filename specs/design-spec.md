# Design Specification — Home AI Assistant

**Status:** Draft
**Version:** 0.2
**Derives from:** [functional-spec.md](functional-spec.md)

---

## 1. System Architecture

### 1.1 Component Overview

```mermaid
graph TB
    subgraph Clients["Client Layer"]
        Browser["Desktop Browser"]
        PWA["Mobile PWA"]
        Pi["Raspberry Pi\nAmbient Mic + Speaker"]
    end

    subgraph Gateway["Gateway"]
        Nginx["Nginx\nReverse Proxy :80"]
    end

    subgraph Core["Core Services (Docker — Host Machine)"]
        OnyxWeb["Onyx Web\nNext.js UI"]
        OnyxAPI["Onyx API\nFastAPI + RAG"]
        OnyxBG["Onyx Background\nIndexing Workers"]
        VoiceSvc["Voice Service\nSTT · TTS · Audio"]
        HASvc["HA Bridge\nHome Assistant API"]
    end

    subgraph AI["AI Layer"]
        Ollama["Ollama\nLlama 4 Scout"]
        IMS["Inference Model Server\nEmbeddings"]
        IDXMS["Indexing Model Server\nEmbeddings"]
        GPU["RTX 5090"]
    end

    subgraph Data["Data Layer"]
        PG[("PostgreSQL\nState · Files · Users")]
        Vespa[("Vespa\nVector Index")]
        Redis[("Redis\nTask Queue")]
    end

    subgraph External["External (optional)"]
        HA["Home Assistant\n(separate host)"]
        SearchAPI["Web Search API\nBing · Tavily"]
    end

    subgraph Indexed["Indexed Sources"]
        LocalFS["Local Filesystem\n(host mounts)"]
        NetShare["Network Shares\nSMB · NFS"]
        PhoneFiles["Phone File Uploads\n(via PWA)"]
    end

    Browser -->|HTTP| Nginx
    PWA -->|HTTP| Nginx
    Pi -->|WebSocket| Nginx

    Nginx --> OnyxWeb
    Nginx -->|/api/*| OnyxAPI
    Nginx -->|/voice/*| VoiceSvc

    OnyxAPI --> PG
    OnyxAPI --> Vespa
    OnyxAPI --> Redis
    OnyxAPI --> Ollama
    OnyxAPI --> IMS
    OnyxAPI --> HASvc

    OnyxBG --> PG
    OnyxBG --> Vespa
    OnyxBG --> IDXMS
    OnyxBG -.->|reads| LocalFS
    OnyxBG -.->|reads| NetShare
    OnyxBG -.->|reads| PhoneFiles

    VoiceSvc --> OnyxAPI
    VoiceSvc -->|WebSocket| Pi

    HASvc --> HA

    OnyxAPI -.->|optional| SearchAPI

    Ollama --> GPU
    IMS --> GPU
    IDXMS --> GPU
```

### 1.2 New Services vs Existing

| Service | Source | Purpose |
|---|---|---|
| Onyx Web, API, Background, Model Servers | Existing (Onyx) | RAG, chat, indexing |
| Ollama | Existing | LLM inference |
| PostgreSQL, Vespa, Redis, Nginx | Existing | Infra |
| **Voice Service** | **New — custom** | STT (Whisper), TTS (Piper), WebSocket relay for Pi |
| **HA Bridge** | **New — custom** | Translates assistant intent → Home Assistant REST API calls |

---

## 1.3 Design Constraints

1. **Frontend/Backend Separation** — User-facing services must separate frontend (UI) and backend (API/logic) into distinct layers. Frontend code lives in its own directory with its own build toolchain (e.g., `frontend/` with Vite + TypeScript), and backend code is never coupled to a specific UI. This promotes modularity: any client (web, mobile, Pi agent) can consume the same backend API without reimplementing server-side logic. Build artifacts (HTML, JS, CSS) are generated at container build time and served as static assets.

---

## 2. User Experience Design

### 2.1 Desktop Browser UI

Provided by Onyx out of the box (unmodified — the Onyx web container is a pre-built image). Key interactions:

```
┌─────────────────────────────────────────┐
│  🏠 Home Assistant    [Admin] [Profile]  │
├──────────┬──────────────────────────────┤
│          │                              │
│  Chats   │   Hello! How can I help?    │
│  ──────  │                              │
│  Today   │   [user message]            │
│  > Chat1 │   [assistant response]      │
│  > Chat2 │   Sources: doc1.pdf ↗       │
│          │                              │
│  Sources │                              │
│  ──────  │                              │
│  My Docs │   ┌─────────────────────┐   │
│  Photos  │   │ Type a message...   │   │
│  Videos  │   └─────────────────────┘   │
│          │                              │
└──────────┴──────────────────────────────┘
```

- Source citations are clickable, showing the originating file or document
- **Voice Mode A** is not embedded in the Onyx UI (the container cannot be modified). Instead, it lives at a separate URL: `http://<host>/voice/` — see §2.1a below.

### 2.1a Voice Chat UI (`/voice/`)

A custom standalone single-page app served by the Voice Service at `http://<host>/voice/`.

```
┌───────────────────────────────────────────────┐
│  ← Chat                         ● IDLE        │
├───────────────────────────────────────────────┤
│                                               │
│              (empty state prompt)             │
│                                               │
│  ┌──────────────────────────────────────────┐ │
│  │ AI  Barack Obama won the 2008 election…  │ │
│  └──────────────────────────────────────────┘ │
│                                               │
├───────────────────────────────────────────────┤
│  ┌───────────────────────────┐  [Send]        │
│  │ Or type here…             │                │
│  └───────────────────────────┘                │
│                    [  🎤  ]                   │
└───────────────────────────────────────────────┘
```

- Dark theme; conversation history with user / AI message bubbles
- Large mic button: click to start recording, click again (or silence timeout) to stop
- Status badge: IDLE / LISTENING / TRANSCRIBING / GENERATING / SPEAKING
- Audio visualizer bar while recording
- Text input fallback — can type instead of speak
- "← Chat" link returns to the main Onyx UI
- Reuses the active Onyx login session (`credentials: include`) — user must be logged into Onyx first
- TTS plays back sentence-by-sentence for low latency; uses a persistent Web Audio `AudioContext` (zero-gain oscillator) to keep the OS audio device warm and prevent the first syllable from being clipped

### 2.2 Mobile PWA UI

Mobile-optimized single-column layout:

```
┌─────────────────────┐
│ 🏠 Butler    ☰  👤  │
├─────────────────────┤
│                     │
│  Hello! How can     │
│  I help you today?  │
│                     │
│  [user message]     │
│                     │
│  [streaming         │
│   response...]      │
│                     │
│  Sources: doc1 ↗    │
│                     │
├─────────────────────┤
│ ┌─────────────┐ 🎤  │
│ │ Message...  │ 📎  │
│ └─────────────┘     │
└─────────────────────┘
```

- 🎤 activates **Voice Mode B** (foreground mic)
- 📎 opens file picker for **phone file upload**
- Installable as a PWA from the browser share menu
- Push notifications for long-running indexing jobs or assistant proactive alerts

### 2.3 Voice Interaction Flows

#### Mode A — Browser Voice Chat (`/voice/`)

```
User navigates to http://<host>/voice/ (must be logged into Onyx at http://<host>/)
    → Page creates a new Onyx chat session on load
    → User clicks 🎤
        → Web Audio AudioContext created (keeps audio device warm)
        → Browser requests mic permission
        → UI shows "Listening…" + audio visualizer
        → MediaRecorder captures audio (webm/ogg)
    → User clicks 🎤 again to stop (or silence timeout — future)
        → Audio blob POSTed to POST /voice/transcribe
        → Whisper transcribes on GPU → { "text": "..." }
        → Transcript shown as user message bubble
    → Text sent to Onyx API POST /api/chat/send-message (SSE/NDJSON stream)
        → Streaming response tokens rendered in AI bubble in real time
    → On stream complete → full response text POSTed sentence-by-sentence to POST /voice/synthesize
        → Piper TTS returns WAV audio per sentence
        → Browser plays each WAV sequentially via Audio element
        → AudioContext prevents OS audio device warmup clipping on first sentence
```

#### Mode B — PWA Microphone (Mobile) *(planned)*

```
Same as Mode A but from the mobile PWA
    → Initiated by tapping a mic button in the PWA
    → Optimized for quick voice queries while on home WiFi
```

#### Mode C — Ambient (Raspberry Pi)

```
Pi mic always listening for wake word
    → "hey computer" detected locally (OpenWakeWord)
    → Pi plays a short audio chime (acknowledgement)
    → Pi streams audio to Voice Service via WebSocket
    → Silence/timeout → stream ends
    → Whisper transcribes on server
    → Sent to Onyx API as owner's message
    → Response generated by LLM
    → TTS converts response to audio on server
    → Audio streamed back to Pi via WebSocket
    → Pi plays response through speaker
```

### 2.4 Smart Home Control Flow

```
User: "hey computer, turn the bedroom lights to 50%"
    → Voice pipeline transcribes
    → Onyx API receives text, detects intent is a smart home action
    → Routes to HA Bridge instead of (or in addition to) RAG retrieval
    → HA Bridge calls Home Assistant REST API:
        POST /api/services/light/turn_on
        { entity_id: "light.bedroom", brightness_pct: 50 }
    → Home Assistant executes via Matter
    → HA Bridge returns confirmation
    → LLM generates natural language confirmation response
    → "Done — bedroom lights set to 50%"
```

---

## 3. Security & Permissions Model

### 3.1 User Roles

```
Owner
├── Full access to all connectors, sources, settings
├── Can view all users' conversation history
└── Can manage all accounts

Household Member
├── Access to sources assigned by owner
├── Private conversation history (owner can view)
└── No admin access

Child (extends Household Member)
├── Subset of sources assigned by owner
├── Content filter applied to all responses
├── Time-of-day access window enforced
└── No admin access
```

### 3.2 Data Access Profiles

The owner creates named **access profiles** and assigns them to users:

| Profile field | Description |
|---|---|
| Name | e.g. "Kids", "Adults", "Guests" |
| Allowed connectors | Which indexed sources are queryable |
| Web search enabled | Yes / No |
| Smart home access | None / Read-only / Full control |

### 3.3 Parental Controls

Applied on top of a data access profile for child accounts:

| Control | Implementation |
|---|---|
| Content filtering | System prompt injection that instructs the LLM to decline or rephrase responses on blocked topics |
| Time restrictions | API-level enforcement — requests outside the allowed window receive a "not available right now" response |
| Source restrictions | Child profile's allowed connectors is a strict subset |

---

## 4. File Indexing Pipeline

### 4.1 Data Flow

```
Source (filesystem / network share / phone upload)
    → Onyx Background Worker picks up new/modified files
    → File parsed into text chunks (by type: PDF, DOCX, MD, image metadata, video metadata)
    → Chunks embedded by Indexing Model Server
    → Embeddings stored in Vespa (vector index)
    → File metadata stored in PostgreSQL
    → At query time: user query embedded by Inference Model Server
    → Vespa returns top-k semantically similar chunks
    → Chunks injected into LLM context
    → LLM generates response citing sources
```

### 4.2 Phase 1 Indexing (filename + metadata)

| File type | What is indexed |
|---|---|
| PDF, DOCX, MD | Full extracted text |
| Photos | Filename, EXIF data (date, GPS, camera, dimensions) |
| Videos | Filename, duration, creation date, container metadata |

### 4.3 Future Deep Indexing

| File type | Future capability | Model needed |
|---|---|---|
| Photos | Object/face/scene recognition | CLIP or similar vision model |
| Videos | Audio transcription → searchable transcript | Whisper (already in stack) |

---

## 5. Voice Service Design

The Voice Service is a new lightweight service added to the Docker stack.

### 5.1 Responsibilities

- Accept audio from browser/PWA clients via HTTP POST
- Accept audio streams from Raspberry Pi via WebSocket
- Run Whisper (STT) to transcribe audio → text
- Forward text to Onyx API
- Receive text response from Onyx API
- Run Piper TTS to synthesize speech → audio
- Return audio to client (HTTP response or WebSocket stream)

### 5.2 Interfaces

**Implemented (Phase 4):**

| Interface | Protocol | Used by |
|---|---|---|
| `GET /voice/` | HTTP | Browser — serves standalone voice chat UI |
| `GET /voice/health` | HTTP | Smoke tests, healthcheck |
| `POST /voice/transcribe` | HTTP multipart | Voice UI — uploads recorded audio, returns transcript |
| `POST /voice/synthesize` | HTTP JSON | Voice UI — sends text, returns audio/wav |

**Planned (Phase 6 — Pi agent):**

| Interface | Protocol | Used by |
|---|---|---|
| `WS /voice/stream` | WebSocket | Raspberry Pi — bidirectional audio stream (mic in, TTS out) |

### 5.3 Raspberry Pi Agent

A lightweight Python process running on the Pi:

- Runs **OpenWakeWord** locally for always-on wake word detection (minimal CPU)
- On wake word: plays chime, opens WebSocket to Voice Service, streams mic audio
- Receives TTS audio stream back, plays through speaker
- Reconnects automatically if the WebSocket drops

---

## 6. Home Assistant Bridge Design

A new lightweight service added to the Docker stack.

### 6.1 Responsibilities

- Expose a simple REST API that the Onyx API can call when a smart home intent is detected
- Translate structured intent → Home Assistant REST API calls
- Return confirmation or current device state

### 6.2 Intent Detection

The LLM is prompted with available Home Assistant entities and their capabilities. When a message is classified as a smart home intent (by the LLM or a classifier), the HA Bridge is called instead of (or alongside) RAG retrieval.

### 6.3 Interfaces

| Endpoint | Description |
|---|---|
| `GET /ha/entities` | List all known HA entities and their current state |
| `POST /ha/action` | Execute an action `{ entity_id, service, params }` |
| `GET /ha/state/{entity_id}` | Get current state of a single entity |

---

## 7. Deployment Architecture by Phase

| Phase | New components added | Status |
|---|---|---|
| 1 | Onyx + Ollama + infra | **Done** |
| 2 | Static LAN IP, `WEB_DOMAIN` update, homelab migration | — |
| 3 | Host filesystem mounts, Onyx file connectors configured | — |
| 4 | Voice Service container (Whisper + Piper); standalone `/voice/` UI | **In progress** — text chat + TTS working; STT UI flow pending |
| 5 | HA Bridge container, Home Assistant connection | — |
| 6 | Raspberry Pi agent deployed, `WS /voice/stream` endpoint added | — |
| 7 | Auth system expanded with household/child accounts and profiles | — |
| 8 | Phone upload endpoint in Voice Service or dedicated upload handler | — |
| 9 | SMB/NFS mounts added to docker-compose | — |
| 10 | Vision model service added for photo indexing | — |
| 11 | Web search connector configured in Onyx admin | — |
