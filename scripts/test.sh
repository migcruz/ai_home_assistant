#!/usr/bin/env bash
# test.sh — Smoke tests for Home AI Assistant stack
# Usage: bash scripts/test.sh [--verbose]
#
# Adding a new test: write a function below, then add its name to TESTS in main().

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ENV_FILE="$PROJECT_DIR/.env"

VERBOSE=false
[[ "${1:-}" == "--verbose" ]] && VERBOSE=true

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
PASS=0; FAIL=0

# Load .env if it exists
[ -f "$ENV_FILE" ] && { set -a; source "$ENV_FILE"; set +a; }
MODEL="${LLM_MODEL:-llama4:scout}"

# ── Primitives ────────────────────────────────────────────────────────────────

pass()    { echo -e "${GREEN}  ✓${NC} $*"; PASS=$((PASS+1)); }
fail()    { echo -e "${RED}  ✗${NC} $*"; FAIL=$((FAIL+1)); }
section() { echo ""; echo -e "${BLUE}──${NC} $*"; }

# ── Helpers ───────────────────────────────────────────────────────────────────

http_ok() {
    # http_ok <url> <label> [extra-curl-flags]
    local url="$1" label="$2" extra="${3:-}"
    local code
    code=$(curl -s $extra -o /dev/null -w "%{http_code}" --max-time 10 "$url" 2>/dev/null || echo "000")
    if [[ "$code" =~ ^[23] ]]; then
        pass "$label (HTTP $code)"
    else
        fail "$label (HTTP $code — expected 2xx/3xx)"
    fi
}

json_field() {
    # json_field <url> <grep-pattern> <label> [extra-curl-flags]
    # Passes if the response body contains the pattern.
    # Use extra-curl-flags for e.g. "-k" (skip TLS verify on self-signed certs).
    local url="$1" field="$2" label="$3" extra="${4:-}"
    local body
    body=$(curl -sf $extra --max-time 10 "$url" 2>/dev/null || echo "{}")
    if echo "$body" | grep -q "$field"; then
        pass "$label"
        $VERBOSE && echo "      response: $(echo "$body" | head -c 300)" || true
    else
        fail "$label (field '$field' not found)"
        $VERBOSE && echo "      response: $(echo "$body" | head -c 300)" || true
    fi
}

container_running() {
    local name="$1"
    if docker ps --format '{{.Names}}' | grep -q "^${name}$"; then
        pass "Container running: $name"
    else
        fail "Container NOT running: $name"
    fi
}

# ── Tests ─────────────────────────────────────────────────────────────────────

test_container_health() {
    section "1. Container health"
    for name in onyx-ollama onyx-postgres onyx-vespa onyx-redis \
                onyx-inference-model-server onyx-indexing-model-server \
                onyx-background onyx-api onyx-web onyx-webui onyx-nginx onyx-voice; do
        container_running "$name"
    done
}

test_service_endpoints() {
    section "2. Service endpoints"
    http_ok "http://localhost:11434/api/tags"        "Ollama API reachable"
    http_ok "http://localhost:8080/health"           "Onyx API health"
    http_ok "http://localhost:3000"                  "Onyx Web UI reachable"
    http_ok "http://localhost"                       "Nginx proxy reachable"
    http_ok "http://localhost:19071/state/v1/health" "Vespa config server"
    http_ok "http://localhost:9000/api/health"       "Inference model server health"
}

test_ollama_models() {
    section "3. Ollama — model availability"
    json_field "http://localhost:11434/api/tags" "$MODEL" "Model '$MODEL' is loaded in Ollama"
}

test_ollama_inference() {
    section "4. Ollama — GPU inference test (single token)"
    local payload response
    payload="{\"model\":\"${MODEL}\",\"prompt\":\"Say: OK\",\"stream\":false,\"options\":{\"num_predict\":5}}"
    response=$(curl -sf --max-time 60 -X POST \
        -H "Content-Type: application/json" \
        -d "$payload" \
        http://localhost:11434/api/generate 2>/dev/null || echo "")
    if echo "$response" | grep -q '"response"'; then
        pass "Ollama inference returned a response"
        if echo "$response" | grep -q '"eval_duration"'; then
            local tokens duration
            tokens=$(echo "$response" | grep -o '"eval_count":[0-9]*' | grep -o '[0-9]*' || echo "?")
            duration=$(echo "$response" | grep -o '"eval_duration":[0-9]*' | grep -o '[0-9]*' || echo "?")
            if [[ "$duration" != "?" && "$tokens" != "?" && "$duration" -gt 0 ]]; then
                pass "Inference speed: ~$(( tokens * 1000000000 / duration )) tokens/sec"
            fi
        fi
        $VERBOSE && echo "      $(echo "$response" | grep -o '"response":"[^"]*"')" || true
    else
        fail "Ollama inference failed or timed out"
        $VERBOSE && echo "      response: $response" || true
    fi
}

test_onyx_api() {
    section "5. Onyx API — basic connectivity"
    json_field "http://localhost:8080/health" "success" "Onyx API returns success field"
}

test_embedding_servers() {
    section "6. Embedding model servers"
    http_ok "http://localhost:9000/api/health" "Inference model server health"
}

test_voice_service() {
    section "7. Voice service"
    json_field "https://localhost/voice/health" '"status"' "Voice service health endpoint" "-k"
}

test_webui() {
    section "8. Web UI"
    # Check the page loads and contains the expected HTML
    json_field "https://localhost/voice/" "Voice Assistant" "Voice UI serves correct HTML" "-k"
    # Extract the hashed JS asset URL from the built HTML and verify it loads
    local html asset_path
    html=$(curl -sk --max-time 10 "https://localhost/voice/" 2>/dev/null || echo "")
    asset_path=$(echo "$html" | grep -o 'src="[^"]*assets[^"]*\.js"' | head -1 | sed 's/src="//;s/"//')
    if [[ -n "$asset_path" ]]; then
        http_ok "https://localhost${asset_path}" "Voice UI JS asset reachable" "-k"
    else
        fail "Voice UI JS asset not found in HTML"
    fi
}

test_gpu_passthrough() {
    section "9. GPU passthrough"
    if docker exec onyx-ollama nvidia-smi &>/dev/null; then
        local gpu_name
        gpu_name=$(docker exec onyx-ollama nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
        pass "GPU visible in Ollama container: ${gpu_name:-unknown}"
    else
        fail "nvidia-smi not accessible in Ollama container — check NVIDIA Container Toolkit"
    fi
    if docker exec onyx-voice nvidia-smi &>/dev/null; then
        pass "GPU visible in Voice container (Whisper can use GPU)"
    else
        fail "nvidia-smi not accessible in Voice container — check NVIDIA Container Toolkit"
    fi
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    local TESTS=(
        test_container_health
        test_service_endpoints
        test_ollama_models
        test_ollama_inference
        test_onyx_api
        test_embedding_servers
        test_voice_service
        test_webui
        test_gpu_passthrough
    )

    for fn in "${TESTS[@]}"; do
        "$fn"
    done

    echo ""
    echo "────────────────────────────────────"
    echo -e "Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / $((PASS + FAIL)) total"
    echo "────────────────────────────────────"

    [ $FAIL -gt 0 ] && exit 1
    exit 0
}

main
