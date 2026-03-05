import io
import os
import wave
from piper.voice import PiperVoice

_voice: PiperVoice | None = None


def load_voice() -> PiperVoice:
    global _voice
    if _voice is None:
        voice_name = os.getenv("PIPER_VOICE", "en_US-lessac-medium")
        model_path = os.path.join(
            os.getenv("CACHE_DIR", "/app/.cache"), "piper", f"{voice_name}.onnx"
        )
        _voice = PiperVoice.load(model_path)
    return _voice


def synthesize(text: str) -> bytes:
    voice = load_voice()
    buf = io.BytesIO()

    with wave.open(buf, "wb") as wav_file:
        voice.synthesize(text, wav_file)

    return buf.getvalue()
