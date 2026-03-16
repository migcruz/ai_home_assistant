import io
import os
import tempfile
import numpy as np
import soundfile as sf
from faster_whisper import WhisperModel

try:
    import torch as _torch
    _has_torch = True
except ImportError:
    _has_torch = False

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

    # Auto-detect format from magic bytes: WAV files start with "RIFF".
    suffix = ".wav" if audio_bytes[:4] == b"RIFF" else ".webm"

    # DEBUG: save a copy so it can be pulled from the container for listening.
    # Remove once mic quality is confirmed.
    # with open("/tmp/last_audio" + suffix, "wb") as dbg:
    #     dbg.write(audio_bytes)

    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
        tmp.write(audio_bytes)
        tmp_path = tmp.name

    try:
        if _has_torch:
            _torch.cuda.empty_cache()
        segments, _ = model.transcribe(
            tmp_path,
            language="en",
            beam_size=5,
            vad_filter=True, # skip silent segments
            vad_parameters={"min_silence_duration_ms": 500},
        )
        return " ".join(seg.text.strip() for seg in segments).strip()
    finally:
        os.unlink(tmp_path)
