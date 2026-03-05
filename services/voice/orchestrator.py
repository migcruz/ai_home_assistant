"""Server-side voice chat orchestration.

Handles the STT → Onyx LLM → sentence detection → TTS pipeline so that
any client (browser, PWA, Pi agent) only needs to speak the WebSocket
wire protocol defined in converse.py.
"""

import asyncio
import json
import re
from typing import Callable, Awaitable

import httpx

from transcribe import transcribe
from synthesize import synthesize

ONYX_BASE = "http://api_server:8080"

# Sentence boundary: split after .!? followed by whitespace then an uppercase
# letter or quote.  Avoids false splits on abbreviations like "Dr. Smith".
_SENTENCE_RE = re.compile(r"(?<=[.!?])\s+(?=[A-Z\"'])")


def extract_token(line: str) -> str | None:
    """Parse one NDJSON/SSE line from the Onyx stream.  Returns token text or None."""
    raw = line.strip()
    if raw.startswith("data: "):
        raw = raw[6:].strip()
    if not raw or raw == "[DONE]":
        return None
    try:
        packet = json.loads(raw)
        obj = packet.get("obj")
        if obj and obj.get("type") == "message_delta" and obj.get("content"):
            return obj["content"]
    except (json.JSONDecodeError, TypeError, KeyError):
        pass
    return None


def extract_sentences(text: str, is_end: bool = False) -> tuple[list[str], str]:
    """Split *text* into complete sentences and a trailing fragment.

    Returns ``(sentences, remaining)``.  When *is_end* is True the trailing
    fragment is also emitted as the final sentence.
    """
    parts = _SENTENCE_RE.split(text)
    sentences = [p.strip() for p in parts[:-1] if p.strip()]
    remaining = parts[-1].strip() if parts else ""
    if is_end and remaining:
        sentences.append(remaining)
        remaining = ""
    return sentences, remaining


async def create_chat_session(
    client: httpx.AsyncClient,
    cookies: str,
    persona_id: int = 0,
) -> int:
    """Create an Onyx chat session, forwarding the browser's cookies."""
    resp = await client.post(
        f"{ONYX_BASE}/chat/create-chat-session",
        json={"persona_id": persona_id},
        headers={"Cookie": cookies},
    )
    resp.raise_for_status()
    return resp.json()["chat_session_id"]


async def stream_chat_response(
    client: httpx.AsyncClient,
    cookies: str,
    chat_session_id: int,
    message: str,
    on_token: Callable[[str], Awaitable[None]],
    on_sentence: Callable[[str, int], Awaitable[None]],
) -> None:
    """Stream an Onyx chat response, calling back for tokens and sentences.

    *on_token(token)* is called for every LLM token (for real-time UI).
    *on_sentence(text, index)* is called for each complete sentence (triggers TTS).
    """
    idx = 0
    pending = ""

    async with client.stream(
        "POST",
        f"{ONYX_BASE}/chat/send-message",
        json={
            "chat_session_id": chat_session_id,
            "message": message,
            "parent_message_id": None,
            "prompt_id": None,
            "search_doc_ids": [],
            "retrieval_options": {"run_search": "auto", "real_time": True},
            "stream_response": True,
        },
        headers={"Cookie": cookies},
        timeout=300.0,
    ) as response:
        response.raise_for_status()
        async for line in response.aiter_lines():
            token = extract_token(line)
            if token:
                await on_token(token)
                pending += token
                sentences, pending = extract_sentences(pending)
                for s in sentences:
                    await on_sentence(s, idx)
                    idx += 1

    # Flush any trailing text that didn't end with sentence punctuation.
    if pending.strip():
        await on_sentence(pending.strip(), idx)


async def transcribe_audio(audio_bytes: bytes) -> str:
    """Run Whisper STT in a thread (CPU/GPU-bound)."""
    return await asyncio.to_thread(transcribe, audio_bytes)


async def synthesize_sentence(text: str) -> bytes:
    """Run Piper TTS in a thread (CPU-bound)."""
    return await asyncio.to_thread(synthesize, text)
