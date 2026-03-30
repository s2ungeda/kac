# SOPS + age 시크릿 관리 설계 문서

> 작성일: 2026-03-30
> 상태: 설계 완료, 구현 대기

---

## 1. 배경

### 현재 상태
- `config/config.yaml`에 API 키가 `${ENV_VAR}` 플레이스홀더로 저장
- `SecretsManager` (AES-256-GCM) 구현되어 있으나 Config와 미연동
- 단일 마스터 비밀번호 → 3~4명 팀 운영에 부적합 (개별 접근 제어 불가)

### 목표
- **가벼운 구조**: 외부 서버 없음 (Vault 불필요)
- **다중 API 키**: 거래소당 복수 계정
- **팀 보안**: 사용자별 age 키, 개별 접근 해제 가능
- **디스크에 평문 없음**: SOPS 암호화 + stdin 파이프라인

---

## 2. 아키텍처

### 데이터 흐름

```
config.yaml (비밀 없음, git 추적)
    +
secrets.enc.yaml (SOPS 암호화, git 추적)
    │
    ▼
[scripts/start.sh: sops decrypt + yq merge]
    │
    ▼ (병합된 YAML, 파이프)
[arbitrage --config-stdin]
    │
    ▼
Config::load_from_stream() → ExchangeConfig, AccountManager
```

### 핵심 원칙
1. **설정/시크릿 분리**: `config.yaml` (git) + `secrets.enc.yaml` (git, 암호화)
2. **평문 파일 없음**: 복호화 결과는 쉘 변수 → 파이프로만 전달
3. **개인 키 로컬 보관**: 각 팀원의 age 개인키는 본인 머신에만 존재

---

## 3. 외부 도구

### 설치 목록

| 도구 | 용도 | 설치 |
|------|------|------|
| **age** | X25519 암호화/복호화 | `sudo apt install age` |
| **sops** | 시크릿 파일 관리 (암호화/복호화/편집) | GitHub releases에서 바이너리 |
| **yq** | YAML 병합 (config + secrets) | `sudo apt install yq` 또는 snap |

총 3개 바이너리. 서버/데몬 없음.

---

## 4. 파일 구조

### 새로 생성하는 파일

```
kimchi-arbitrage-cpp/
├── .sops.yaml                    # SOPS 설정 (수신자 공개키 목록)
├── secrets.enc.yaml              # 암호화된 시크릿 (git 추적)
├── secrets.yaml.template         # 시크릿 템플릿 (예시, git 추적)
├── scripts/
│   └── start.sh                  # 복호화 + 병합 + 기동 스크립트
└── docs/
    └── SOPS_AGE_DESIGN.md        # 이 문서
```

### 수정하는 파일

```
include/arbitrage/common/config.hpp    # load_from_stream() 추가
src/common/config.cpp                  # load_from_stream() 구현, 다중 계정 파싱
src/main.cpp                           # --config-stdin 옵션 추가
src/feeder/feeder_process.cpp          # --config-stdin 옵션 추가
src/cold/order_manager_process.cpp     # --config-stdin 옵션 추가
src/cold/risk_manager_process.cpp      # --config-stdin 옵션 추가
src/cold/monitor_process.cpp           # --config-stdin 옵션 추가
.gitignore                             # secrets.yaml, *.age 제외 추가
```

---

## 5. SOPS 설정

### `.sops.yaml` (프로젝트 루트)

```yaml
creation_rules:
  - path_regex: secrets\.enc\.yaml$
    age: >-
      age1user1publickey...,
      age1user2publickey...,
      age1user3publickey...
```

각 팀원의 age 공개키를 쉼표로 나열. 이 파일에 등록된 공개키 소유자만 복호화 가능.

---

## 6. 시크릿 파일 구조

### `secrets.yaml.template` (예시, 실제 값 없음)

```yaml
exchanges:
  upbit:
    accounts:
      - id: "upbit_main"
        label: "메인 계정"
        api_key: "YOUR_UPBIT_API_KEY"
        api_secret: "YOUR_UPBIT_API_SECRET"
        weight: 1.0

      - id: "upbit_sub"
        label: "보조 계정"
        api_key: "YOUR_UPBIT_SUB_API_KEY"
        api_secret: "YOUR_UPBIT_SUB_API_SECRET"
        weight: 0.5

  bithumb:
    accounts:
      - id: "bithumb_main"
        label: "메인 계정"
        api_key: "YOUR_BITHUMB_API_KEY"
        api_secret: "YOUR_BITHUMB_API_SECRET"
        passphrase: ""
        weight: 1.0

  binance:
    accounts:
      - id: "binance_main"
        label: "선물 메인"
        api_key: "YOUR_BINANCE_API_KEY"
        api_secret: "YOUR_BINANCE_API_SECRET"
        weight: 1.0

  mexc:
    accounts:
      - id: "mexc_main"
        label: "메인 계정"
        api_key: "YOUR_MEXC_API_KEY"
        api_secret: "YOUR_MEXC_API_SECRET"
        weight: 1.0

alert:
  telegram_token: "YOUR_TELEGRAM_BOT_TOKEN"
  telegram_chat_id: "YOUR_TELEGRAM_CHAT_ID"
  discord_webhook: "YOUR_DISCORD_WEBHOOK_URL"

server:
  auth_token: "YOUR_TCP_AUTH_TOKEN"
```

### 병합 결과 (config.yaml + secrets.yaml)

`config.yaml`의 일반 설정과 `secrets.yaml`의 인증 정보가 병합되어 하나의 YAML로 stdin에 전달됨. `exchanges.<name>.accounts[]` 배열이 존재하면 다중 계정 모드, `exchanges.<name>.api_key` (flat)이면 단일 계정 모드로 동작.

---

## 7. C++ 변경 사항

### 7.1 Config 클래스 확장

**`config.hpp`** — 새 메서드 추가:

```cpp
class Config {
public:
    // 기존
    bool load(const std::string& path);
    bool reload();

    // 신규
    bool load_from_stream(std::istream& input);

private:
    // 기존 load()에서 YAML 파싱 로직 분리
    bool parse_yaml_node(const YAML::Node& root);

    // 다중 계정 파싱
    void parse_exchange_accounts(const YAML::Node& exchange_node,
                                  Exchange ex);

    // stdin 재로드용 캐시
    std::string cached_yaml_;
};
```

**`config.cpp`** — 리팩터링:

1. `load()`: `YAML::LoadFile(path)` → `parse_yaml_node(root)`
2. `load_from_stream()`: `std::istream` → `std::string` → `YAML::Load(str)` → `parse_yaml_node(root)` + `cached_yaml_`에 저장
3. `reload()`: `cached_yaml_`이 있으면 그걸로 재파싱, 없으면 기존 파일 재로드
4. `parse_exchange_accounts()`: `exchanges.<name>.accounts[]` 배열을 `AccountManager::add_account()`로 등록
5. 감사 로그: `[AUDIT] Loaded N accounts for exchange X` (값은 절대 로깅하지 않음)

### 7.2 커맨드라인 옵션

**`src/main.cpp`**:

```cpp
struct AppOptions {
    std::string config_path = "config/config.yaml";
    RunMode mode = RunMode::Standalone;
    bool dry_run = false;
    bool verbose = false;
    bool config_from_stdin = false;  // 신규
};

// parse_args()
else if (arg == "--config-stdin") opts.config_from_stdin = true;

// main()
if (opts.config_from_stdin) {
    if (!Config::instance().load_from_stream(std::cin)) {
        logger->error("Failed to load config from stdin");
        return 1;
    }
} else {
    Config::instance().load(opts.config_path);
}
```

동일한 `--config-stdin` 옵션을 모든 프로세스에 추가:
- `src/feeder/feeder_process.cpp`
- `src/cold/order_manager_process.cpp`
- `src/cold/risk_manager_process.cpp`
- `src/cold/monitor_process.cpp`

### 7.3 메모리 보안

stdin에서 읽은 YAML 문자열에 평문 API 키가 포함됨. Config 파싱 완료 후:

```cpp
// cached_yaml_은 reload용으로 유지 (SecureString 사용)
// ExchangeConfig의 api_key/api_secret도 사용 후 SecureString 고려
```

---

## 8. 기동 스크립트

### `scripts/start.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="${PROJECT_DIR}/build"

MODE="${1:---standalone}"
shift || true

CONFIG_FILE="${PROJECT_DIR}/config/config.yaml"
SECRETS_FILE="${PROJECT_DIR}/secrets.enc.yaml"
AGE_KEY_FILE="${SOPS_AGE_KEY_FILE:-${HOME}/.config/sops/age/keys.txt}"

# 사전 검증
for cmd in sops yq; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "ERROR: $cmd not found"; exit 1;
    }
done

[[ -f "$AGE_KEY_FILE" ]] || {
    echo "ERROR: age key not found at $AGE_KEY_FILE"
    echo "Generate: age-keygen -o $AGE_KEY_FILE"
    exit 1
}

[[ -f "$SECRETS_FILE" ]] || {
    echo "ERROR: $SECRETS_FILE not found"
    exit 1
}

# 복호화 (메모리에만 존재)
echo "[*] Decrypting secrets..."
DECRYPTED=$(SOPS_AGE_KEY_FILE="$AGE_KEY_FILE" sops -d "$SECRETS_FILE")

# 병합 (config + secrets)
MERGED=$(yq eval-all 'select(fileIndex == 0) *d select(fileIndex == 1)' \
    "$CONFIG_FILE" <(echo "$DECRYPTED"))

# 복호화 원본 즉시 제거
unset DECRYPTED

echo "[*] Starting ($MODE)..."

if [[ "$MODE" == "--engine" ]]; then
    # 8 프로세스 모드: 각 프로세스에 파이프
    for EXCHANGE in upbit bithumb binance mexc; do
        echo "$MERGED" | "${BIN_DIR}/${EXCHANGE}-feeder" \
            --config-stdin "$@" &
    done
    sleep 1

    echo "$MERGED" | "${BIN_DIR}/arbitrage" \
        --engine --config-stdin "$@" &
    sleep 0.5

    echo "$MERGED" | "${BIN_DIR}/order-manager" \
        --config-stdin "$@" &
    echo "$MERGED" | "${BIN_DIR}/risk-manager" \
        --config-stdin "$@" &
    sleep 0.5

    echo "$MERGED" | "${BIN_DIR}/monitor" \
        --config-stdin "$@" &

    wait
else
    # 단일 프로세스 모드
    echo "$MERGED" | "${BIN_DIR}/arbitrage" \
        --standalone --config-stdin "$@"
fi
```

### 보안 특성
- `DECRYPTED`, `MERGED`는 쉘 변수 → 디스크에 안 남음
- `echo "$MERGED" | process` → 각 프로세스의 stdin으로 파이프
- `DECRYPTED`는 병합 후 `unset`으로 즉시 해제

---

## 9. 팀 운영

### 신규 팀원 온보딩

```bash
# 1. 도구 설치
sudo apt install age
# sops는 GitHub에서 다운로드

# 2. 개인 age 키 생성
age-keygen -o ~/.config/sops/age/keys.txt
# 출력 예: Public key: age1abc123...

# 3. 공개키를 관리자에게 전달 (Slack, 메일 등)

# 4. 관리자가 .sops.yaml에 공개키 추가 후 re-encrypt
sops updatekeys secrets.enc.yaml

# 5. git pull 후 검증
sops -d secrets.enc.yaml > /dev/null && echo "OK"
```

### 팀원 제거

```bash
# 1. .sops.yaml에서 해당 공개키 삭제

# 2. re-encrypt (이제 해당 키로 복호화 불가)
sops updatekeys secrets.enc.yaml

# 3. 반드시: 모든 거래소 API 키 로테이션
#    (이미 평문을 본 적 있으므로)
sops secrets.enc.yaml  # 에디터에서 새 키로 교체

# 4. 커밋 + 서비스 재시작
```

### API 키 로테이션

```bash
# 거래소 웹사이트에서 새 API 키 발급 후:
sops secrets.enc.yaml
# 에디터가 열림 → 해당 키 교체 → 저장 시 자동 재암호화
git commit -am "chore(secrets): rotate binance API keys"
```

---

## 10. 구현 순서

| Phase | 내용 | 파일 | 비고 |
|:-----:|------|------|------|
| **A** | SOPS/age 인프라 | `.sops.yaml`, `secrets.yaml.template`, `scripts/start.sh`, `.gitignore` | C++ 변경 없음 |
| **B** | Config stdin 로딩 | `config.hpp`, `config.cpp` | `load_from_stream()`, `parse_yaml_node()` 리팩터링 |
| **C** | 다중 계정 파싱 | `config.cpp` | `exchanges.<name>.accounts[]` → `AccountManager` |
| **D** | 전 프로세스 적용 | `main.cpp`, feeder, cold 프로세스 | `--config-stdin` 옵션 |
| **E** | 테스트 | `tests/` | stdin 로딩 + 다중 계정 파싱 테스트 |

### 의존성

```
Phase A (인프라)
    ↓
Phase B (Config 리팩터) → Phase C (다중 계정) → Phase D (전 프로세스)
                                                       ↓
                                                  Phase E (테스트)
```

Phase A는 C++ 변경 없이 즉시 시작 가능.

---

## 11. 기존 코드와의 호환성

- `--config-stdin` 없이 실행하면 기존과 동일하게 `config/config.yaml` 파일 로드
- `exchanges.<name>.api_key` (flat) 형식도 계속 지원 → 단일 계정으로 처리
- `secrets.enc.yaml` 없이도 기존 환경변수 방식으로 동작 가능
- `SecretsManager`(TASK_16)는 그대로 유지 — 런타임에 추가 시크릿 관리 시 사용 가능

---

## 12. 보안 요약

| 위협 | 방어 |
|------|------|
| secrets.enc.yaml 유출 | age 개인키 없이 복호화 불가 (X25519) |
| git에 평문 커밋 | `.gitignore` + SOPS는 암호화 상태만 커밋 |
| 디스크에 평문 잔류 | stdin 파이프만 사용, 임시 파일 없음 |
| 팀원 이탈 | 공개키 제거 + API 키 로테이션 |
| 프로세스 메모리 덤프 | 런타임 메모리에는 평문 존재 (Vault도 동일) |
| 쉘 히스토리 노출 | API 키를 CLI 인자로 전달하지 않음 |
