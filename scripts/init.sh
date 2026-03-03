#!/usr/bin/env bash
# init.sh — One-time setup for Home AI Assistant
# Run once before docker compose up

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ENV_FILE="$PROJECT_DIR/.env"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()    { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

# ── Preflight checks ────────────────────────────────────────────────────────
check_deps() {
    for cmd in docker openssl curl; do
        command -v "$cmd" &>/dev/null || error "'$cmd' is required but not installed."
    done
    docker info &>/dev/null || error "Docker daemon is not running."
    # Check NVIDIA Container Toolkit
    if ! docker run --rm --gpus all nvidia/cuda:12.8.0-base-ubuntu22.04 nvidia-smi &>/dev/null; then
        warn "GPU passthrough test failed. Ensure NVIDIA Container Toolkit is installed."
        warn "See: https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html"
    else
        info "GPU passthrough verified."
    fi
}

# ── .env setup ──────────────────────────────────────────────────────────────
setup_env() {
    if [ -f "$ENV_FILE" ]; then
        warn ".env already exists — skipping. Delete it and re-run to regenerate."
        return
    fi

    info "Creating .env from template..."
    cp "$PROJECT_DIR/.env.example" "$ENV_FILE"

    # Auto-generate secrets
    # SECRET_KEY: 64 hex chars = 64 bytes (fine for JWT signing)
    SECRET_KEY=$(openssl rand -hex 32)
    # ENCRYPTION_KEY_SECRET: must be exactly 32 bytes for AES-256
    # openssl rand -hex 16 → 32 hex chars → 32 bytes when used as string
    ENCRYPTION_KEY=$(openssl rand -hex 16)
    DB_PASSWORD=$(openssl rand -hex 16)

    sed -i "s/^SECRET_KEY=$/SECRET_KEY=$SECRET_KEY/" "$ENV_FILE"
    sed -i "s/^ENCRYPTION_KEY_SECRET=$/ENCRYPTION_KEY_SECRET=$ENCRYPTION_KEY/" "$ENV_FILE"
    sed -i "s/^POSTGRES_PASSWORD=$/POSTGRES_PASSWORD=$DB_PASSWORD/" "$ENV_FILE"

    info ".env created with auto-generated secrets."
    warn "Review $ENV_FILE before starting (especially AUTH_TYPE for production)."
}

# ── Pull Llama 4 Scout ──────────────────────────────────────────────────────
pull_model() {
    local model="${LLM_MODEL:-llama4:scout}"
    info "Starting Ollama to pull model: $model"

    # Start only ollama service first
    docker compose -f "$PROJECT_DIR/docker-compose.yml" up -d ollama

    info "Waiting for Ollama to be ready..."
    local retries=0
    until curl -sf http://localhost:11434/api/tags &>/dev/null; do
        sleep 3
        retries=$((retries + 1))
        [ $retries -ge 20 ] && error "Ollama did not start in time."
    done

    info "Pulling $model (this may take a while on first run)..."
    docker exec onyx-ollama ollama pull "$model"
    info "Model pull complete."
}

# ── Main ────────────────────────────────────────────────────────────────────
main() {
    info "=== Home AI Assistant — Init ==="
    check_deps
    setup_env

    # Source the .env so we can read LLM_MODEL
    # shellcheck disable=SC1090
    set -a; source "$ENV_FILE"; set +a

    pull_model

    info ""
    info "=== Setup complete ==="
    info "Start the full stack with:"
    info "  docker compose up -d"
    info ""
    info "Then open: http://localhost"
}

main "$@"
