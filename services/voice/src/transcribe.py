import io
import os
import tempfile
import numpy as np
import soundfile as sf
from faster_whisper import WhisperModel

_model: WhisperModel | None = None


def load_model() -> WhisperModel:
    global _model
    if _model is None:
        model_size = os.getenv("WHISPER_MODEL", "medium")
        cache_dir = os.path.join(os.getenv("CACHE_DIR", "/app/.cache"), "whisper")
        _model = WhisperModel(
            model_size,
            device="cuda",
            compute_type="float16",
            download_root=cache_dir,
        )
    return _model


def transcribe(audio_bytes: bytes) -> str:
    model = load_model()

    # Write to a temp file — faster-whisper needs a file path or numpy array
    with tempfile.NamedTemporaryFile(suffix=".webm", delete=False) as tmp:
        tmp.write(audio_bytes)
        tmp_path = tmp.name

    try:
        segments, _ = model.transcribe(
            tmp_path,
            language="en",
            beam_size=5,
            vad_filter=True,               # skip silent segments
            vad_parameters={"min_silence_duration_ms": 500},
        )
        return " ".join(seg.text.strip() for seg in segments).strip()
    finally:
        os.unlink(tmp_path)
