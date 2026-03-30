#!/usr/bin/env bash
#
# Kimchi Arbitrage 기동 스크립트
# SOPS + age로 시크릿 복호화 후 프로세스에 stdin 파이프
#
# 사용법:
#   ./scripts/start.sh --standalone          # 단일 프로세스
#   ./scripts/start.sh --engine              # 8 프로세스 (Watchdog)
#   ./scripts/start.sh --standalone --dry-run
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="${PROJECT_DIR}/build"

# 기본값
MODE="--standalone"
EXTRA_ARGS=()

# 인자 파싱
while [[ $# -gt 0 ]]; do
    case "$1" in
        --standalone|--engine)
            MODE="$1"; shift ;;
        *)
            EXTRA_ARGS+=("$1"); shift ;;
    esac
done

CONFIG_FILE="${PROJECT_DIR}/config/config.yaml"
SECRETS_FILE="${PROJECT_DIR}/secrets.enc.yaml"
AGE_KEY_FILE="${SOPS_AGE_KEY_FILE:-${HOME}/.config/sops/age/keys.txt}"

# =============================================================================
# 사전 검증
# =============================================================================
check_prereq() {
    local cmd="$1"
    local install_hint="$2"
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: '$cmd' not found."
        echo "Install: $install_hint"
        exit 1
    fi
}

check_prereq sops "https://github.com/getsops/sops/releases"
check_prereq yq   "sudo apt install yq  or  snap install yq"

if [[ ! -f "$AGE_KEY_FILE" ]]; then
    echo "ERROR: age key not found at $AGE_KEY_FILE"
    echo ""
    echo "Generate your age keypair:"
    echo "  mkdir -p ~/.config/sops/age"
    echo "  age-keygen -o ~/.config/sops/age/keys.txt"
    echo ""
    echo "Then share your public key with the team admin."
    exit 1
fi

if [[ ! -f "$SECRETS_FILE" ]]; then
    echo "ERROR: $SECRETS_FILE not found."
    echo "Create it with: sops -e secrets.yaml > secrets.enc.yaml"
    exit 1
fi

if [[ ! -f "$CONFIG_FILE" ]]; then
    echo "ERROR: $CONFIG_FILE not found."
    exit 1
fi

# =============================================================================
# 복호화 + 병합
# =============================================================================
echo "[*] Decrypting secrets..."
DECRYPTED=$(SOPS_AGE_KEY_FILE="$AGE_KEY_FILE" sops -d "$SECRETS_FILE" 2>&1) || {
    echo "ERROR: Failed to decrypt secrets."
    echo "Check your age key: $AGE_KEY_FILE"
    echo "Detail: $DECRYPTED"
    exit 1
}

echo "[*] Merging config + secrets..."
MERGED=$(yq eval-all 'select(fileIndex == 0) *d select(fileIndex == 1)' \
    "$CONFIG_FILE" <(echo "$DECRYPTED"))

# 복호화 원본 즉시 해제
unset DECRYPTED

echo "[*] Mode: $MODE"

# =============================================================================
# 프로세스 기동
# =============================================================================
if [[ "$MODE" == "--engine" ]]; then
    echo "[*] Starting 8-process architecture..."

    # Priority 0: Feeders (동시)
    for EXCHANGE in upbit bithumb binance mexc; do
        echo "    Starting ${EXCHANGE}-feeder..."
        echo "$MERGED" | "${BIN_DIR}/${EXCHANGE}-feeder" \
            --config-stdin "${EXTRA_ARGS[@]}" &
    done
    sleep 1

    # Priority 1: Engine
    echo "    Starting arb-engine..."
    echo "$MERGED" | "${BIN_DIR}/arbitrage" \
        --engine --config-stdin "${EXTRA_ARGS[@]}" &
    sleep 0.5

    # Priority 2: Cold Path
    echo "    Starting order-manager..."
    echo "$MERGED" | "${BIN_DIR}/order-manager" \
        --config-stdin "${EXTRA_ARGS[@]}" &
    echo "    Starting risk-manager..."
    echo "$MERGED" | "${BIN_DIR}/risk-manager" \
        --config-stdin "${EXTRA_ARGS[@]}" &
    sleep 0.5

    # Priority 3: Monitor
    echo "    Starting monitor..."
    echo "$MERGED" | "${BIN_DIR}/monitor" \
        --config-stdin "${EXTRA_ARGS[@]}" &

    echo "[*] All processes started. Waiting..."

    # 병합 데이터 해제
    unset MERGED

    wait
else
    echo "[*] Starting standalone mode..."

    # 병합 데이터를 파이프 후 해제
    echo "$MERGED" | "${BIN_DIR}/arbitrage" \
        --standalone --config-stdin "${EXTRA_ARGS[@]}"
fi
