# Functional Specification — Ambient Voice Node

**Status:** Draft
**Version:** 0.1

---

## 1. Purpose

A small, always-on device placed in a room that gives hands-free access to the home AI assistant. It listens passively for a wake word, captures the user's spoken query, streams it to the assistant server on the local network, and plays the spoken response back through a speaker. All intelligence (speech recognition, AI inference, text-to-speech) runs on the server — the device only handles audio input/output and wake word detection.

---

## 2. Deployment Context

One node per room. Nodes operate independently — each connects separately to the assistant server and is treated as its own voice session. No coordination between nodes is required.

The device is always plugged in to mains power. It does not need to function off a battery.

---

## 3. Features

### 3.1 Always-On Wake Word Detection

- The device listens continuously for a configurable wake word (e.g. *"hey computer"*)
- Wake word detection runs entirely on the device — no audio is sent to the server during idle listening
- Detection is low-latency: the user should not need to pause after speaking the wake word before beginning their query
- A false positive rate acceptable for home use is required; occasional accidental triggers are tolerable but should be infrequent

---

### 3.2 Audio Capture

- On wake word detection, the device begins recording the user's spoken query
- Recording ends automatically when the user stops speaking (silence detection)
- A maximum recording duration caps queries to prevent runaway recording if silence detection fails
- Captured audio is streamed to the assistant server over the local network in real time

---

### 3.3 Response Playback

- The server returns synthesised speech audio in response to the query
- The device plays the audio through an attached speaker
- Playback begins as soon as the first audio arrives — the device does not wait for the full response before starting to play
- Audio quality must be intelligible and natural-sounding at conversational volume in a typical room

---

### 3.4 Status Indicators

The device gives the user clear feedback at each stage of an interaction:

| State | Indicator |
|---|---|
| Idle | No sound; device is listening silently |
| Wake word detected | A short acknowledgement chime plays before recording begins |
| Recording | — |
| Waiting for response | — |
| Playing response | Speaker plays the AI's spoken reply |
| Error | A short error chime plays; device returns to idle |

The user can tell an interaction succeeded because they hear the response. The user can tell an interaction failed because they hear the error chime.

---

### 3.5 Network Connectivity

- The device connects to the home WiFi network at startup
- If the connection drops, the device reconnects automatically without requiring a power cycle
- If a query is in progress when the connection drops, the device plays the error chime and returns to idle

---

### 3.6 WiFi Provisioning

- On first use, the device needs to be given the home WiFi credentials (network name and password)
- Provisioning is done wirelessly — no serial cable, no firmware recompile required
- Credentials are stored persistently on the device so it connects automatically on every subsequent power-on
- To change the WiFi network, the user can re-enter provisioning mode by holding a physical button at startup

---

### 3.7 Error Handling

- Any failure during an interaction (network error, server unreachable, timeout) results in an error chime and a return to idle listening
- The device never gets stuck in a non-idle state — it always returns to listening
- The device does not store or retry failed queries

---

## 4. Non-Functional Requirements

| Requirement | Target |
|---|---|
| **Wake word latency** | Wake word detection completes within ~1 second of the word being spoken |
| **End-to-end latency** | Wake word → first spoken word of response within 3–5 seconds |
| **Idle audio privacy** | No audio captured or transmitted during idle; only post-wake-word audio leaves the device |
| **Network privacy** | Audio streams only to the local home network — never to the internet |
| **Reliability** | Returns to idle and resumes listening after any error without requiring a restart |
| **Availability** | Ready to respond within a few seconds of being powered on |
| **Multi-room** | Multiple nodes operate independently; no cross-node interference |

---

## 5. Out of Scope

- Local speech recognition (STT runs on the server)
- Local AI inference (LLM runs on the server)
- Local text-to-speech synthesis (TTS runs on the server)
- A display or screen of any kind
- Battery or portable operation
- Internet or cloud connectivity
- Per-user identification (the device does not know who is speaking)
- Conversation history stored on the device
