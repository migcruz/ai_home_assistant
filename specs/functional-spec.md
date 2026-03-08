# Functional Specification — Home AI Assistant

**Status:** Final
**Version:** 1.0

---

## 1. Purpose

A self-hosted AI butler that lives on the home network, knows the owner's files and data, and is accessible from any device in the house via browser, PWA, or voice. All inference and data storage stays local — nothing is sent to external AI services (except an optional web search API key).

---

## 2. Users

| Role | Description | Access |
|---|---|---|
| **Owner** | Administrator. Full access to all features, connectors, settings, and all user conversation history. | Unrestricted |
| **Household Member** | Other people in the home. Can chat with the assistant but cannot see other members' conversation history. Data access scoped by owner. | Scoped by owner |
| **Child** | Household member with parental controls applied. Subject to content filtering, topic restrictions, time-of-day limits, and restricted data sources. | Restricted |

- Conversation history is **private per user** — only the owner can view all conversations
- The owner assigns each household member a **data access profile** controlling which indexed sources they can query
- Parental controls are configured per child account by the owner

---

## 3. Core Goals

1. **Local-first** — No prompts, files, or conversation data leave the home network (except optional opted-in web search queries)
2. **Always-on** — Available 24/7 from any device on the WiFi *(Phase 2: homelab rack)*
3. **Context-aware** — Knows the owner's files, documents, photos, and videos
4. **Multi-modal access** — Desktop browser, mobile PWA, and ambient voice
5. **Actionable** — Can take actions on smart home devices, not just answer questions about them
6. **Progressively capable** — Each phase adds capability without requiring a rebuild

---

## 4. Features

### 4.1 Chat Interface

- **Desktop browser** — Full chat UI at a LAN URL, accessible from any PC or laptop
- **Mobile PWA** — Mobile-optimized Progressive Web App; installable from the browser, supports file upload, push notifications, and foreground microphone access
- Conversations are persistent — history is saved per user and can be resumed
- The assistant cites its sources when answering from indexed documents
- Responses stream in real time

---

### 4.2 Local File Awareness

Files are indexed in the background on a nightly schedule. The assistant can answer questions about indexed content.

**Supported file types:**

| Type | Formats | Indexing depth |
|---|---|---|
| Documents | PDF, Word (.docx), Markdown (.md) | Full text |
| Photos | JPEG, PNG, HEIC and common formats | Phase 1: filename + metadata. Future: object/face/scene recognition |
| Videos | MP4, MOV and common formats | Phase 1: filename + metadata. Future: audio transcription |

**Always-excluded paths** (never indexed):
- `.ssh/` and private key files
- Browser profile data and password stores
- System directories (`/proc`, `/sys`, `/dev`, etc.)

**Owner-configurable exclusions:** Additional paths excluded via admin UI

**Indexing schedule:** Nightly (daily minimum)

---

### 4.3 Network File Awareness

**Other PCs:** Shared folders mounted read-only over SMB or NFS

**Phones:** A dedicated mobile PWA feature lets the owner or household members upload selected files (photos, documents) on demand to be indexed by the assistant. The assistant never pulls from the phone automatically.

**NAS** *(future)*: TrueNAS, Synology, or similar mounted as a network share

All network access is read-only — the assistant never writes to external shares.

---

### 4.4 Web Search

- Owner opts in by providing an external search API key (Bing, Tavily, or similar)
- Used for current events and information beyond the model's training cutoff
- LLM inference still runs locally even when web search is active
- Can be enabled or disabled per user role in admin settings

---

### 4.5 Voice Interaction

Voice is a first-class feature with three modes. All speech processing runs locally — no audio is sent to external services.

#### Mode A — Browser Microphone (Desktop)
- User clicks a mic button in the desktop browser UI
- Speech is transcribed locally on the server using Whisper
- Transcribed text is sent to the assistant as a normal message
- The assistant's response is read back via local TTS

#### Mode B — PWA Microphone (Mobile)
- Same as Mode A but from the mobile PWA
- Initiated by tapping a mic button in the app
- Optimized for quick voice queries while on home WiFi

#### Mode C — Ambient Always-Listening Microphone
- A **custom embedded voice node** (Seeed XIAO ESP32S3 Sense) sits in the home and listens passively
- **Wake word** (e.g. *"hey computer"*) is detected on-device using TensorFlow Lite Micro
- On wake word detection, the device records the query and streams WAV audio to the assistant server over a TLS WebSocket
- The server transcribes (Whisper) and responds via the LLM
- The response is converted to speech (Piper TTS on the server) and streamed back as WAV frames; the device plays it through an onboard Class D amplifier and speaker
- Supports natural home control: *"hey computer, turn off the bedroom lights"*

**All voice modes share:**
- Local STT (Whisper, runs on the assistant server)
- Local TTS (Piper, runs on the assistant server)
- Wake word detection on-device (no audio sent before wake word)
- No audio or transcripts sent outside the LAN

---

### 4.6 Smart Home Control

**Controller:** Home Assistant (self-hosted)
**Protocol:** Matter

The assistant can both read state and take actions:

| Category | Examples |
|---|---|
| Lighting | Turn on/off, set brightness, set color |
| Climate | Set thermostat temperature, read current temp |
| Security | Lock/unlock doors, read sensor state |
| Sensors | Query motion, temperature, humidity readings |
| Scenes & Automations | Trigger a named scene or automation |

Actions can be initiated from chat or any of the three voice modes.

---

### 4.7 Parental Controls

Configured per child account by the owner:

| Control | Description |
|---|---|
| **Content filtering** | Block responses on specified topics or categories |
| **Time-of-day restrictions** | Define hours during which the child account can access the assistant |
| **Data source restrictions** | Limit which indexed sources (file shares, connectors) the child can query |

---

### 4.8 Administration

- Add, remove, and configure data connectors (file paths, network shares, web search)
- View indexing status — sources synced and last-run timestamp
- Create and manage household member and child accounts
- Assign data access profiles and parental control settings per user
- Enable or disable features per user role

---

## 5. Non-Functional Requirements

| Requirement | Target |
|---|---|
| **Privacy** | No conversation, file content, or audio leaves the LAN (except opted-in web search) |
| **Response latency** | First token within a few seconds of message submission |
| **Voice latency** | Wake word → first spoken word within 3–5 seconds |
| **Availability** | Phase 1: when host PC is on. Phase 2+: 24/7 on homelab rack |
| **Client accessibility** | Desktop and mobile work from a standard browser — no install required |
| **Recoverability** | `docker compose up -d` restores the stack without data loss |
| **Indexing freshness** | File index updated at least nightly |

---

## 6. Deployment Phases

| Phase | Scope |
|---|---|
| **1 — Done** | Chat via browser. Llama 4 Scout on RTX 5090. Owner only. |
| **2 — Homelab** | Migrate to server rack. 24/7 availability. LAN access for all devices. |
| **3 — File Indexing** | Local PC files indexed (docs, photos, videos — filename/metadata first). |
| **4 — Voice A/B** | Browser and PWA microphone. Local STT + TTS on server. |
| **5 — Smart Home** | Home Assistant + Matter integration. Chat and voice can control devices. |
| **6 — Ambient Voice (C)** | Embedded voice node (XIAO ESP32S3 Sense) — always-listening, wake word on-device, WAV streaming, speaker playback. |
| **7 — Multi-user** | Household accounts, data scoping, parental controls. |
| **8 — Phone File Sync** | PWA file upload for phone photos and documents. |
| **9 — Network Shares** | Other PCs and NAS indexed over SMB/NFS. |
| **10 — Deep Media Indexing** | Photo object/scene recognition. Video audio transcription. |
| **11 — Web Search** | Opted-in external search API for real-time information. |

---

## 7. Out of Scope

- Cloud hosting or internet-accessible deployment
- Third-party AI API usage (OpenAI, Anthropic, etc.)
- Native iOS/Android app *(PWA covers all stated mobile requirements)*
