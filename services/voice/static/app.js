// ── State ────────────────────────────────────────────────────────────────────
let ws = null;
let mediaRecorder = null;
let audioChunks = [];
let isRecording = false;
let isBusy = false;
let currentAssistantBubble = null;
let currentAudio = null;

// Active turn state — set when a response is being received.
let activeBubble = null;
let activeCursor = null;
let activeTextNode = null;
let fullText = '';
// Audio playback queue for the current turn.
let audioQueue = [];      // Blob[]
let audioDone = false;
let notifyPlayer = null;
let playerPromise = null;

// ── Audio device warmup ───────────────────────────────────────────────────────
let _audioCtx = null;
function ensureAudioWarm() {
  if (_audioCtx) return;
  _audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  const osc = _audioCtx.createOscillator();
  const gain = _audioCtx.createGain();
  gain.gain.value = 0;
  osc.connect(gain);
  gain.connect(_audioCtx.destination);
  osc.start();
  _audioCtx.resume().catch(() => {});
}

// ── DOM ──────────────────────────────────────────────────────────────────────
const conversation = document.getElementById('conversation');
const emptyState = document.getElementById('empty-state');
const statusBadge = document.getElementById('status-badge');
const micBtn = document.getElementById('mic-btn');
const textInput = document.getElementById('text-input');
const sendBtn = document.getElementById('send-btn');
const visualizer = document.getElementById('visualizer');
const errorToast = document.getElementById('error-toast');

// ── Helpers ──────────────────────────────────────────────────────────────────
function setStatus(label, color) {
  statusBadge.textContent = label;
  statusBadge.style.color = color || '';
  statusBadge.style.borderColor = color || '';
}

function showError(msg) {
  errorToast.textContent = msg;
  errorToast.classList.add('show');
  setTimeout(() => errorToast.classList.remove('show'), 4000);
}

function hideEmpty() {
  if (emptyState) emptyState.remove();
}

function addMessage(role, text) {
  hideEmpty();
  const wrap = document.createElement('div');
  wrap.className = `message ${role}`;

  const avatar = document.createElement('div');
  avatar.className = 'avatar';
  avatar.textContent = role === 'user' ? 'U' : 'AI';

  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  bubble.textContent = text;

  wrap.appendChild(avatar);
  wrap.appendChild(bubble);
  conversation.appendChild(wrap);
  conversation.scrollTop = conversation.scrollHeight;
  return bubble;
}

function startStreamingMessage() {
  hideEmpty();
  const wrap = document.createElement('div');
  wrap.className = 'message assistant';

  const avatar = document.createElement('div');
  avatar.className = 'avatar';
  avatar.textContent = 'AI';

  const bubble = document.createElement('div');
  bubble.className = 'bubble';

  const textNode = document.createTextNode('');
  bubble.appendChild(textNode);

  const cursor = document.createElement('span');
  cursor.className = 'cursor';
  bubble.appendChild(cursor);

  wrap.appendChild(avatar);
  wrap.appendChild(bubble);
  conversation.appendChild(wrap);
  conversation.scrollTop = conversation.scrollHeight;
  currentAssistantBubble = bubble;
  return { bubble, cursor, textNode };
}

// ── Audio playback ──────────────────────────────────────────────────────────

async function playBlob(blob) {
  ensureAudioWarm();
  if (_audioCtx.state === 'suspended') await _audioCtx.resume();
  const arrayBuffer = await blob.arrayBuffer();
  const audioBuffer = await _audioCtx.decodeAudioData(arrayBuffer);
  await new Promise((resolve, reject) => {
    const source = _audioCtx.createBufferSource();
    source.buffer = audioBuffer;
    source.connect(_audioCtx.destination);
    source.onended = () => { currentAudio = null; resolve(); };
    currentAudio = source;
    try { source.start(); } catch (e) { reject(e); }
  });
}

// Plays queued audio blobs in order. Runs concurrently with incoming messages.
async function runPlayer() {
  let idx = 0;
  while (true) {
    if (idx < audioQueue.length) {
      setStatus('speaking…', '#5be07a');
      try { await playBlob(audioQueue[idx++]); } catch (e) { console.error('Playback error:', e); }
    } else if (audioDone) {
      break;
    } else {
      await new Promise(r => { notifyPlayer = r; });
    }
  }
}

function enqueueAudio(blob) {
  audioQueue.push(blob);
  if (notifyPlayer) { notifyPlayer(); notifyPlayer = null; }
}

function finishAudio() {
  audioDone = true;
  if (notifyPlayer) { notifyPlayer(); notifyPlayer = null; }
}

// ── WebSocket connection ─────────────────────────────────────────────────────

let reconnectDelay = 1000;

function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/voice/converse`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    setStatus('idle');
    reconnectDelay = 1000;
    // Send initial config
    ws.send(JSON.stringify({ type: 'config', tts: true }));
  };

  ws.onclose = () => {
    ws = null;
    setStatus('reconnecting…', '#f0a840');
    setTimeout(connectWS, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 10000);
  };

  ws.onerror = () => {
    ws?.close();
  };

  ws.onmessage = (event) => {
    // Binary frame = TTS audio
    if (event.data instanceof ArrayBuffer) {
      const blob = new Blob([event.data], { type: 'audio/wav' });
      enqueueAudio(blob);
      return;
    }

    // Text frame = JSON message
    let msg;
    try { msg = JSON.parse(event.data); } catch (_) { return; }

    switch (msg.type) {
      case 'transcript':
        // STT result — show as user message
        addMessage('user', msg.text);
        setStatus('generating…', '#5b7cf6');
        beginResponseBubble();
        break;

      case 'token':
        if (!activeTextNode) beginResponseBubble();
        fullText += msg.content;
        activeTextNode.textContent = fullText;
        conversation.scrollTop = conversation.scrollHeight;
        break;

      case 'done':
        finishTurn();
        break;

      case 'error':
        showError(msg.detail || 'Server error');
        finishTurn();
        break;
    }
  };
}

function beginResponseBubble() {
  if (activeTextNode) return;  // already started
  const { bubble, cursor, textNode } = startStreamingMessage();
  activeBubble = bubble;
  activeCursor = cursor;
  activeTextNode = textNode;
  fullText = '';

  // Start audio player for this turn.
  audioQueue = [];
  audioDone = false;
  notifyPlayer = null;
  playerPromise = runPlayer().catch(() => {});
}

async function finishTurn() {
  if (activeCursor) activeCursor.remove();
  if (activeBubble && !fullText.trim()) activeBubble.textContent = '[No response]';
  activeBubble = null;
  activeCursor = null;
  activeTextNode = null;
  fullText = '';

  // Wait for all audio to finish playing.
  finishAudio();
  if (playerPromise) await playerPromise;
  playerPromise = null;

  setStatus('idle');
  isBusy = false;
  micBtn.disabled = false;
}

// ── Send message (text input) ───────────────────────────────────────────────

function sendMessage(userText) {
  if (!userText.trim() || isBusy || !ws || ws.readyState !== WebSocket.OPEN) return;
  isBusy = true;
  micBtn.disabled = true;

  addMessage('user', userText);
  setStatus('generating…', '#5b7cf6');
  beginResponseBubble();

  ws.send(JSON.stringify({ type: 'text_input', message: userText }));
}

// ── Microphone recording ──────────────────────────────────────────────────────

async function startRecording() {
  if (isBusy || !ws || ws.readyState !== WebSocket.OPEN) return;
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({ audio: true });
  } catch (e) {
    showError('Microphone access denied. Allow mic in browser settings.');
    return;
  }

  audioChunks = [];
  const mimeType = MediaRecorder.isTypeSupported('audio/webm;codecs=opus')
    ? 'audio/webm;codecs=opus'
    : 'audio/webm';

  mediaRecorder = new MediaRecorder(stream, { mimeType });

  // Send audio chunks to server as they arrive.
  mediaRecorder.ondataavailable = async (e) => {
    if (e.data.size > 0 && ws && ws.readyState === WebSocket.OPEN) {
      ws.send(await e.data.arrayBuffer());
    }
  };

  mediaRecorder.onstop = () => {
    stream.getTracks().forEach(t => t.stop());
    if (ws && ws.readyState === WebSocket.OPEN) {
      isBusy = true;
      micBtn.disabled = true;
      ws.send(JSON.stringify({ type: 'end_audio' }));
      setStatus('transcribing…', '#f0a840');
    }
  };

  mediaRecorder.start(250);  // send chunks every 250ms
  isRecording = true;
  micBtn.classList.add('listening');
  visualizer.classList.add('active');
  setStatus('listening…', '#e05b5b');
}

function stopRecording() {
  if (!mediaRecorder || !isRecording) return;
  mediaRecorder.stop();
  isRecording = false;
  micBtn.classList.remove('listening');
  visualizer.classList.remove('active');
}

// ── Event listeners ───────────────────────────────────────────────────────────

micBtn.addEventListener('click', () => {
  ensureAudioWarm();
  if (isBusy && !isRecording) return;
  if (isRecording) {
    stopRecording();
  } else {
    if (currentAudio) {
      currentAudio.stop();
      currentAudio = null;
      isBusy = false;
      micBtn.disabled = false;
    }
    startRecording();
  }
});

sendBtn.addEventListener('click', () => {
  ensureAudioWarm();
  const text = textInput.value.trim();
  if (text) {
    textInput.value = '';
    sendMessage(text);
  }
});

textInput.addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    const text = textInput.value.trim();
    if (text) {
      textInput.value = '';
      sendMessage(text);
    }
  }
});

textInput.addEventListener('input', () => {
  textInput.style.height = 'auto';
  textInput.style.height = Math.min(textInput.scrollHeight, 120) + 'px';
});

// ── Boot ──────────────────────────────────────────────────────────────────────
setStatus('connecting…', '#f0a840');
connectWS();
