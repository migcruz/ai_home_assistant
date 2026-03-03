#!/usr/bin/env bash
# test.sh — Smoke tests for Home AI Assistant stack
# Usage: bash scripts/test.sh [--verbose]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ENV_FILE="$PROJECT_DIR/.env"

VERBOSE=false
[[ "${1:-}" == "--verbose" ]] && VERBOSE=true

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
PASS=0; FAIL=0

pass() { echo -e "${GREEN}  ✓${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}  ✗${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${BLUE}──${NC} $*"; }

# Load .env if it exists
[ -f "$ENV_FILE" ] && { set -a; source "$ENV_FILE"; set +a; }
MODEL="${LLM_MODEL:-llama4:scout}"

# ── Helper ──────────────────────────────────────────────────────────────────
http_ok() {
    local url="$1" label="$2"
    local code
    code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 10 "$url" 2>/dev/null || echo "000")
    if [[ "$code" =~ ^[23] ]]; then
        pass "$label (HTTP $code)"
    else
        fail "$label (HTTP $code — expected 2xx/3xx)"
    fi
}

json_field() {
    # Check that curl response contains a JSON field/value
    local url="$1" field="$2" label="$3"
    local body
    body=$(curl -sf --max-time 10 "$url" 2>/dev/null || echo "{}")
    if echo "$body" | grep -q "$field"; then
        pass "$label"
        $VERBOSE && echo "      response: $body" | head -c 300
    else
        fail "$label (field '$field' not found)"
        $VERBOSE && echo "      response: $body" | head -c 300
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

# ── Tests ───────────────────────────────────────────────────────────────────

info "1. Container health"
for container in onyx-ollama onyx-postgres onyx-vespa onyx-redis \
                 onyx-inference-model-server onyx-indexing-model-server \
                 onyx-background onyx-api onyx-web onyx-nginx; do
    container_running "$container"
done

echo ""
info "2. Service endpoints"
http_ok "http://localhost:11434/api/tags"        "Ollama API reachable"
http_ok "http://localhost:8080/health"           "Onyx API health"
http_ok "http://localhost:3000"                  "Onyx Web UI reachable"
http_ok "http://localhost"                       "Nginx proxy reachable"
http_ok "http://localhost:19071/state/v1/health" "Vespa config server"
http_ok "http://localhost:9000/api/health"       "Inference model server health"

echo ""
info "3. Ollama — model availability"
json_field "http://localhost:11434/api/tags" "$MODEL" "Model '$MODEL' is loaded in Ollama"

echo ""
info "4. Ollama — GPU inference test (single token)"
PAYLOAD="{\"model\":\"${MODEL}\",\"prompt\":\"Say: OK\",\"stream\":false,\"options\":{\"num_predict\":5}}"
RESPONSE=$(curl -sf --max-time 60 -X POST \
    -H "Content-Type: application/json" \
    -d "$PAYLOAD" \
    http://localhost:11434/api/generate 2>/dev/null || echo "")
if echo "$RESPONSE" | grep -q '"response"'; then
    pass "Ollama inference returned a response"
    if echo "$RESPONSE" | grep -q '"eval_duration"'; then
        TOKENS=$(echo "$RESPONSE" | grep -o '"eval_count":[0-9]*' | grep -o '[0-9]*' || echo "?")
        DURATION=$(echo "$RESPONSE" | grep -o '"eval_duration":[0-9]*' | grep -o '[0-9]*' || echo "?")
        if [[ "$DURATION" != "?" && "$TOKENS" != "?" && "$DURATION" -gt 0 ]]; then
            TPS=$(( TOKENS * 1000000000 / DURATION ))
            pass "Inference speed: ~${TPS} tokens/sec"
        fi
    fi
    $VERBOSE && echo "      $(echo "$RESPONSE" | grep -o '"response":"[^"]*"')"
else
    fail "Ollama inference failed or timed out"
    $VERBOSE && echo "      response: $RESPONSE"
fi

echo ""
info "5. Onyx API — basic connectivity"
json_field "http://localhost:8080/health" "status" "Onyx API returns status field"

echo ""
info "6. Embedding model servers"
json_field "http://localhost:9000/api/health" "status" "Inference model server returns status"

echo ""
info "7. GPU passthrough in Ollama container"
if docker exec onyx-ollama nvidia-smi &>/dev/null; then
    GPU_NAME=$(docker exec onyx-ollama nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
    pass "GPU visible in container: ${GPU_NAME:-unknown}"
else
    fail "nvidia-smi not accessible in Ollama container — check NVIDIA Container Toolkit"
fi

# ── Summary ─────────────────────────────────────────────────────────────────
echo ""
echo "────────────────────────────────────"
TOTAL=$((PASS + FAIL))
echo -e "Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "────────────────────────────────────"

[ $FAIL -gt 0 ] && exit 1
exit 0
