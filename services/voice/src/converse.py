"""WebSocket endpoint for voice chat orchestration.

Wire protocol (see plan for full spec):

    Client → Server
      TEXT   {"type": "config", "tts": true}
      TEXT   {"type": "text_input", "message": "..."}
      BINARY raw audio bytes
      TEXT   {"type": "end_audio"}

    Server → Client
      TEXT   {"type": "transcript", "text": "..."}
      TEXT   {"type": "token", "content": "..."}
      BINARY WAV audio (one per sentence)
      TEXT   {"type": "done"}
      TEXT   {"type": "error", "detail": "..."}
"""

import asyncio
import json
import logging

import httpx
from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from orchestrator import (
    create_chat_session,
    stream_chat_response,
    synthesize_sentence,
    transcribe_audio,
)

logger = logging.getLogger(__name__)

router = APIRouter()

MAX_AUDIO_BYTES = 10 * 1024 * 1024  # 10 MB — ~10 min of opus audio


@router.websocket("/voice/converse")
async def converse(ws: WebSocket) -> None:
    await ws.accept()

    chat_session_id: int | None = None
    tts_enabled = True
    audio_buffer = bytearray()

    async with httpx.AsyncClient() as client:
        try:
            while True:
                msg = await ws.receive()

                if msg["type"] == "websocket.disconnect":
                    break

                # ── Binary frame: accumulate audio ────────────────────────
                if "bytes" in msg and msg["bytes"]:
                    if len(audio_buffer) + len(msg["bytes"]) > MAX_AUDIO_BYTES:
                        await _send_error(ws, "Audio too long (max ~10 minutes)")
                        audio_buffer.clear()
                        continue
                    audio_buffer.extend(msg["bytes"])
                    continue

                # ── Text frame: JSON command ──────────────────────────────
                if "text" not in msg or not msg["text"]:
                    continue

                try:
                    data = json.loads(msg["text"])
                except json.JSONDecodeError:
                    await _send_error(ws, "Invalid JSON")
                    continue

                msg_type = data.get("type")

                if msg_type == "config":
                    tts_enabled = data.get("tts", True)
                    continue

                user_text: str | None = None

                if msg_type == "text_input":
                    user_text = data.get("message", "").strip()
                    if not user_text:
                        await _send_error(ws, "Empty message")
                        continue

                elif msg_type == "end_audio":
                    if not audio_buffer:
                        await _send_error(ws, "No audio received")
                        continue
                    try:
                        user_text = await transcribe_audio(bytes(audio_buffer))
                    except Exception as exc:
                        logger.exception("STT failed")
                        await _send_error(ws, f"Transcription failed: {exc}")
                        audio_buffer.clear()
                        continue
                    finally:
                        audio_buffer.clear()

                    if not user_text or not user_text.strip():
                        await _send_error(ws, "No speech detected")
                        continue

                    await ws.send_text(json.dumps({
                        "type": "transcript",
                        "text": user_text,
                    }))

                else:
                    continue  # unknown message type — ignore

                # ── Run the pipeline ──────────────────────────────────────
                if chat_session_id is None:
                    try:
                        chat_session_id = await create_chat_session(client)
                    except Exception as exc:
                        logger.exception("Session creation failed")
                        await _send_error(ws, f"Chat session failed: {exc}")
                        continue

                await _run_pipeline(
                    ws, client,
                    chat_session_id, user_text, tts_enabled,
                )

        except WebSocketDisconnect:
            pass
        except Exception:
            logger.exception("WebSocket error")


async def _run_pipeline(
    ws: WebSocket,
    client: httpx.AsyncClient,
    chat_session_id: int,
    user_text: str,
    tts_enabled: bool,
) -> None:
    """Execute one conversation turn: LLM streaming + concurrent TTS."""

    # TTS worker: synthesizes sentences and sends binary WAV frames.
    tts_queue: asyncio.Queue[tuple[str, int] | None] = asyncio.Queue()

    async def tts_worker() -> None:
        while True:
            item = await tts_queue.get()
            if item is None:
                break
            text, idx = item
            try:
                wav = await synthesize_sentence(text)
                await ws.send_bytes(wav)
            except Exception:
                logger.exception("TTS failed for sentence %d", idx)

    tts_task = asyncio.create_task(tts_worker()) if tts_enabled else None

    try:
        async def on_token(token: str) -> None:
            await ws.send_text(json.dumps({
                "type": "token",
                "content": token,
            }))

        async def on_sentence(text: str, idx: int) -> None:
            if tts_enabled:
                await tts_queue.put((text, idx))

        await stream_chat_response(
            client, chat_session_id, user_text,
            on_token, on_sentence,
        )
    except Exception as exc:
        logger.exception("LLM streaming failed")
        await _send_error(ws, f"Chat failed: {exc}")
    finally:
        if tts_task:
            await tts_queue.put(None)  # signal worker to stop
            await tts_task

    await ws.send_text(json.dumps({"type": "done"}))


async def _send_error(ws: WebSocket, detail: str) -> None:
    await ws.send_text(json.dumps({"type": "error", "detail": detail}))
