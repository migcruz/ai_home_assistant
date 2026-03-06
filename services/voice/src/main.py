from contextlib import asynccontextmanager

from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.responses import Response
from pydantic import BaseModel

from converse import router as converse_router
from synthesize import load_voice, synthesize
from transcribe import load_model, transcribe


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Pre-load models at startup so first request is fast
    load_model()
    load_voice()
    yield


app = FastAPI(lifespan=lifespan)
app.include_router(converse_router)


class SynthesizeRequest(BaseModel):
    text: str


@app.get("/voice/health")
def health():
    return {"status": "ok"}


@app.post("/voice/transcribe")
async def transcribe_audio(audio: UploadFile = File(...)):
    audio_bytes = await audio.read()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="Empty audio file")
    try:
        text = transcribe(audio_bytes)
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    return {"text": text}


@app.post("/voice/synthesize")
def synthesize_text(req: SynthesizeRequest):
    if not req.text.strip():
        raise HTTPException(status_code=400, detail="Empty text")
    try:
        wav_bytes = synthesize(req.text)
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    return Response(content=wav_bytes, media_type="audio/wav")
