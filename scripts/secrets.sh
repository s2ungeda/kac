#!/usr/bin/env bash
#
# Kimchi Arbitrage 시크릿 관리 CLI
#
# 사용법:
#   secrets.sh set <exchange> <field> <value>   # 키 설정
#   secrets.sh get <exchange> [field]           # 키 조회
#   secrets.sh list                             # 전체 목록
#   secrets.sh edit                             # 에디터로 편집
#   secrets.sh add-account <exchange> <id> <label> <api_key> <api_secret> [weight]
#   secrets.sh remove-account <exchange> <id>   # 계정 삭제
#   secrets.sh add-user <name> <pubkey>         # 팀원 추가
#   secrets.sh remove-user <name>               # 팀원 제거
#   secrets.sh verify                           # 복호화 테스트
#
set -euo pipefail

# 프로젝트 루트 찾기
find_project_root() {
    local dir="$1"
    while [[ "$dir" != "/" ]]; do
        if [[ -f "$dir/secrets.enc.yaml" ]]; then
            echo "$dir"
            return 0
        fi
        dir="$(dirname "$dir")"
    done
    # 기본 경로
    if [[ -f "$HOME/kimchi-arbitrage-cpp/secrets.enc.yaml" ]]; then
        echo "$HOME/kimchi-arbitrage-cpp"
        return 0
    fi
    echo "ERROR: secrets.enc.yaml not found" >&2
    return 1
}

PROJECT_DIR=$(find_project_root "$(pwd)")
SECRETS_FILE="$PROJECT_DIR/secrets.enc.yaml"
SOPS_CONFIG="$PROJECT_DIR/.sops.yaml"
AGE_KEY_FILE="${SOPS_AGE_KEY_FILE:-${HOME}/.config/sops/age/keys.txt}"

export SOPS_AGE_KEY_FILE="$AGE_KEY_FILE"

# 색상
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# =============================================================================
# 사전 검증
# =============================================================================
check_prereqs() {
    for cmd in sops yq; do
        command -v "$cmd" &>/dev/null || {
            echo -e "${RED}ERROR: '$cmd' not found${NC}"; exit 1;
        }
    done
    [[ -f "$AGE_KEY_FILE" ]] || {
        echo -e "${RED}ERROR: age key not found at $AGE_KEY_FILE${NC}"
        echo "Generate: age-keygen -o $AGE_KEY_FILE"
        exit 1
    }
    [[ -f "$SECRETS_FILE" ]] || {
        echo -e "${RED}ERROR: $SECRETS_FILE not found${NC}"
        exit 1
    }
}

# =============================================================================
# 거래소 이름 검증
# =============================================================================
validate_exchange() {
    local ex="$1"
    case "$ex" in
        upbit|bithumb|binance|mexc) return 0 ;;
        *) echo -e "${RED}ERROR: Unknown exchange '$ex' (upbit|bithumb|binance|mexc)${NC}"; exit 1 ;;
    esac
}

# =============================================================================
# 계정 인덱스 찾기 (id 기반)
# =============================================================================
find_account_index() {
    local ex="$1"
    local id="$2"
    local decrypted
    decrypted=$(sops -d "$SECRETS_FILE" 2>/dev/null)
    echo "$decrypted" | yq ".exchanges.$ex.accounts // [] | to_entries | .[] | select(.value.id == \"$id\") | .key" 2>/dev/null
}

# =============================================================================
# 명령어: set
# =============================================================================
cmd_set() {
    local exchange="${1:?Usage: secrets.sh set <exchange> <field> <value>}"
    local field="${2:?Usage: secrets.sh set <exchange> <field> <value>}"
    local value="${3:?Usage: secrets.sh set <exchange> <field> <value>}"
    local account_idx="${4:-0}"

    validate_exchange "$exchange"

    sops set "$SECRETS_FILE" \
        "[\"exchanges\"][\"$exchange\"][\"accounts\"][$account_idx][\"$field\"]" \
        "\"$value\""

    echo -e "${GREEN}OK${NC}: $exchange/account[$account_idx]/$field updated"
}

# =============================================================================
# 명령어: get
# =============================================================================
cmd_get() {
    local exchange="${1:?Usage: secrets.sh get <exchange> [field]}"
    local field="${2:-}"

    validate_exchange "$exchange"

    local decrypted
    decrypted=$(sops -d "$SECRETS_FILE" 2>/dev/null)

    if [[ -z "$field" ]]; then
        echo -e "${CYAN}=== $exchange ===${NC}"
        echo "$decrypted" | yq ".exchanges.$exchange.accounts[]" 2>/dev/null | while IFS= read -r line; do
            # api_key/api_secret 마스킹
            if echo "$line" | grep -qE "^(api_key|api_secret):"; then
                local key val masked
                key=$(echo "$line" | cut -d: -f1)
                val=$(echo "$line" | cut -d: -f2- | xargs)
                if [[ ${#val} -gt 8 ]]; then
                    masked="${val:0:4}...${val: -4}"
                else
                    masked="****"
                fi
                echo -e "  $key: ${YELLOW}$masked${NC}"
            else
                echo "  $line"
            fi
        done
    else
        echo "$decrypted" | yq ".exchanges.$exchange.accounts[0].$field" 2>/dev/null
    fi
}

# =============================================================================
# 명령어: list
# =============================================================================
cmd_list() {
    local decrypted
    decrypted=$(sops -d "$SECRETS_FILE" 2>/dev/null)

    echo -e "${CYAN}=== Registered Accounts ===${NC}"
    echo ""

    for ex in upbit bithumb binance mexc; do
        local count
        count=$(echo "$decrypted" | yq ".exchanges.$ex.accounts | length" 2>/dev/null)
        if [[ "$count" == "0" || "$count" == "null" ]]; then
            echo -e "  $ex: ${YELLOW}(no accounts)${NC}"
            continue
        fi

        echo -e "  ${GREEN}$ex${NC}: $count account(s)"
        echo "$decrypted" | yq -r ".exchanges.$ex.accounts[] | \"    - \" + .id + \" (\" + .label + \") weight=\" + (.weight | tostring)" 2>/dev/null
    done

    echo ""

    # alert/server 키 존재 여부
    local tg_token
    tg_token=$(echo "$decrypted" | yq ".alert.telegram_token // \"\"" 2>/dev/null)
    if [[ -n "$tg_token" && "$tg_token" != "null" && "$tg_token" != *"YOUR_"* ]]; then
        echo -e "  telegram: ${GREEN}configured${NC}"
    else
        echo -e "  telegram: ${YELLOW}not set${NC}"
    fi

    local auth
    auth=$(echo "$decrypted" | yq ".server.auth_token // \"\"" 2>/dev/null)
    if [[ -n "$auth" && "$auth" != "null" && "$auth" != *"YOUR_"* ]]; then
        echo -e "  server auth: ${GREEN}configured${NC}"
    else
        echo -e "  server auth: ${YELLOW}not set${NC}"
    fi
}

# =============================================================================
# 명령어: edit
# =============================================================================
cmd_edit() {
    sops "$SECRETS_FILE"
    echo -e "${GREEN}OK${NC}: secrets updated"
}

# =============================================================================
# 명령어: add-account
# =============================================================================
cmd_add_account() {
    local exchange="${1:?Usage: secrets.sh add-account <exchange> <id> <label> <api_key> <api_secret> [weight]}"
    local id="${2:?}"
    local label="${3:?}"
    local api_key="${4:?}"
    local api_secret="${5:?}"
    local weight="${6:-1.0}"

    validate_exchange "$exchange"

    # 현재 계정 수 확인
    local decrypted count
    decrypted=$(sops -d "$SECRETS_FILE" 2>/dev/null)
    count=$(echo "$decrypted" | yq ".exchanges.$exchange.accounts | length" 2>/dev/null)
    count=${count:-0}

    # 새 계정 추가
    sops set "$SECRETS_FILE" \
        "[\"exchanges\"][\"$exchange\"][\"accounts\"][$count][\"id\"]" "\"$id\""
    sops set "$SECRETS_FILE" \
        "[\"exchanges\"][\"$exchange\"][\"accounts\"][$count][\"label\"]" "\"$label\""
    sops set "$SECRETS_FILE" \
        "[\"exchanges\"][\"$exchange\"][\"accounts\"][$count][\"api_key\"]" "\"$api_key\""
    sops set "$SECRETS_FILE" \
        "[\"exchanges\"][\"$exchange\"][\"accounts\"][$count][\"api_secret\"]" "\"$api_secret\""
    sops set "$SECRETS_FILE" \
        "[\"exchanges\"][\"$exchange\"][\"accounts\"][$count][\"weight\"]" "$weight"

    echo -e "${GREEN}OK${NC}: Added $exchange/$id ($label)"
}

# =============================================================================
# 명령어: remove-account
# =============================================================================
cmd_remove_account() {
    local exchange="${1:?Usage: secrets.sh remove-account <exchange> <id>}"
    local id="${2:?}"

    validate_exchange "$exchange"

    local idx
    idx=$(find_account_index "$exchange" "$id")
    if [[ -z "$idx" ]]; then
        echo -e "${RED}ERROR: Account '$id' not found in $exchange${NC}"
        exit 1
    fi

    # 복호화 → 계정 삭제 → 재암호화
    local decrypted
    decrypted=$(sops -d "$SECRETS_FILE")
    decrypted=$(echo "$decrypted" | yq "del(.exchanges.$exchange.accounts[$idx])")
    echo "$decrypted" | sops -e --input-type yaml --output-type yaml /dev/stdin > "${SECRETS_FILE}.tmp"
    mv "${SECRETS_FILE}.tmp" "$SECRETS_FILE"

    echo -e "${GREEN}OK${NC}: Removed $exchange/$id"
}

# =============================================================================
# 명령어: add-user
# =============================================================================
cmd_add_user() {
    local name="${1:?Usage: secrets.sh add-user <name> <age_pubkey>}"
    local pubkey="${2:?}"

    if [[ ! "$pubkey" =~ ^age1 ]]; then
        echo -e "${RED}ERROR: Invalid age public key (must start with age1)${NC}"
        exit 1
    fi

    # .sops.yaml에 공개키 추가
    local current_keys
    current_keys=$(yq '.creation_rules[0].age' "$SOPS_CONFIG")

    if echo "$current_keys" | grep -q "$pubkey"; then
        echo -e "${YELLOW}WARN: Key already registered${NC}"
        return
    fi

    local new_keys="${current_keys},${pubkey}"
    # 앞뒤 공백/줄바꿈 정리
    new_keys=$(echo "$new_keys" | tr -d '\n' | sed 's/  */ /g')

    yq -i ".creation_rules[0].age = \"$new_keys\"" "$SOPS_CONFIG"

    # 재암호화
    sops updatekeys -y "$SECRETS_FILE"

    echo -e "${GREEN}OK${NC}: Added user '$name' ($pubkey)"
    echo "Don't forget to commit .sops.yaml and secrets.enc.yaml"
}

# =============================================================================
# 명령어: remove-user
# =============================================================================
cmd_remove_user() {
    local name="${1:?Usage: secrets.sh remove-user <name>}"

    echo -e "${YELLOW}Current keys in .sops.yaml:${NC}"
    yq '.creation_rules[0].age' "$SOPS_CONFIG" | tr ',' '\n' | sed 's/^ *//' | nl

    echo ""
    read -rp "Enter the key number to remove: " num

    local current_keys key_to_remove new_keys
    current_keys=$(yq '.creation_rules[0].age' "$SOPS_CONFIG")
    key_to_remove=$(echo "$current_keys" | tr ',' '\n' | sed 's/^ *//' | sed -n "${num}p")

    if [[ -z "$key_to_remove" ]]; then
        echo -e "${RED}ERROR: Invalid selection${NC}"
        exit 1
    fi

    echo -e "Removing: ${RED}$key_to_remove${NC}"
    new_keys=$(echo "$current_keys" | tr ',' '\n' | sed 's/^ *//' | sed "${num}d" | paste -sd,)

    yq -i ".creation_rules[0].age = \"$new_keys\"" "$SOPS_CONFIG"
    sops updatekeys -y "$SECRETS_FILE"

    echo -e "${GREEN}OK${NC}: Removed user '$name'"
    echo -e "${RED}IMPORTANT: Rotate all API keys! The removed user had access to plaintext.${NC}"
}

# =============================================================================
# 명령어: verify
# =============================================================================
cmd_verify() {
    echo -n "Decrypting... "
    if sops -d "$SECRETS_FILE" > /dev/null 2>&1; then
        echo -e "${GREEN}OK${NC}"
        cmd_list
    else
        echo -e "${RED}FAILED${NC}"
        echo "Check your age key: $AGE_KEY_FILE"
        exit 1
    fi
}

# =============================================================================
# 도움말
# =============================================================================
cmd_help() {
    cat << 'EOF'
Kimchi Arbitrage Secret Manager

Usage: secrets.sh <command> [args...]

Commands:
  set <exchange> <field> <value> [idx]   Set a field (default: account[0])
  get <exchange> [field]                 Show account info (masked)
  list                                   List all accounts
  edit                                   Open in editor (sops edit)
  add-account <ex> <id> <label> <key> <secret> [weight]
  remove-account <exchange> <id>         Remove an account
  add-user <name> <age_pubkey>           Add team member
  remove-user <name>                     Remove team member
  verify                                 Test decryption

Exchanges: upbit, bithumb, binance, mexc

Examples:
  secrets.sh set upbit api_key "abc123"
  secrets.sh set binance api_secret "xyz789" 1    # account[1]
  secrets.sh get upbit
  secrets.sh get binance api_key
  secrets.sh add-account upbit upbit_sub "보조계정" "key" "secret" 0.5
  secrets.sh remove-account upbit upbit_sub
  secrets.sh list
  secrets.sh edit
EOF
}

# =============================================================================
# 메인
# =============================================================================
check_prereqs

case "${1:-help}" in
    set)            shift; cmd_set "$@" ;;
    get)            shift; cmd_get "$@" ;;
    list)           cmd_list ;;
    edit)           cmd_edit ;;
    add-account)    shift; cmd_add_account "$@" ;;
    remove-account) shift; cmd_remove_account "$@" ;;
    add-user)       shift; cmd_add_user "$@" ;;
    remove-user)    shift; cmd_remove_user "$@" ;;
    verify)         cmd_verify ;;
    help|--help|-h) cmd_help ;;
    *)              echo -e "${RED}Unknown command: $1${NC}"; cmd_help; exit 1 ;;
esac
