# API 키 관리 가이드

> SOPS + age 기반 암호화 키 관리. 평문은 디스크에 저장되지 않으며,
> 메모리에서도 AES-256-GCM으로 암호화 보관. API 호출 시에만 순간 복호화 후 즉시 삭제.

---

## 사전 준비 (최초 1회)

### 1. 도구 설치

```bash
# age (암호화)
sudo apt install age
# 또는: https://github.com/FiloSottile/age/releases

# sops (시크릿 관리)
# https://github.com/getsops/sops/releases 에서 최신 바이너리 다운로드
sudo mv sops-v3.*.linux.amd64 /usr/local/bin/sops && sudo chmod +x /usr/local/bin/sops

# yq (YAML 병합)
sudo apt install yq
# 또는: sudo snap install yq
```

### 2. 개인 키 생성

```bash
mkdir -p ~/.config/sops/age
age-keygen -o ~/.config/sops/age/keys.txt
```

출력 예시:
```
Public key: age1abc123def456...
```

이 **공개키**를 `.sops.yaml`에 등록해야 복호화 가능.

### 3. 공개키 등록

```bash
# CLI로 등록 (권장)
./scripts/secrets.sh add-user "내이름" "age1abc123def456..."

# 또는 수동 편집
vi .sops.yaml
```

---

## secrets.sh CLI

`scripts/secrets.sh`로 모든 키 관리 작업을 수행할 수 있음.

```bash
./scripts/secrets.sh help
```

### 명령어 요약

| 명령어 | 설명 | 예시 |
|--------|------|------|
| `list` | 등록된 전체 계정 목록 | `secrets.sh list` |
| `get` | 특정 거래소 키 조회 (마스킹) | `secrets.sh get upbit` |
| `set` | 특정 필드 값 변경 | `secrets.sh set upbit api_key "NEW_KEY"` |
| `edit` | 에디터로 전체 편집 | `secrets.sh edit` |
| `add-account` | 계정 추가 | `secrets.sh add-account upbit upbit_sub "보조" "KEY" "SECRET" 0.5` |
| `remove-account` | 계정 삭제 | `secrets.sh remove-account upbit upbit_sub` |
| `add-user` | 팀원 공개키 등록 | `secrets.sh add-user "홍길동" "age1abc..."` |
| `remove-user` | 팀원 제거 | `secrets.sh remove-user "홍길동"` |
| `verify` | 복호화 테스트 | `secrets.sh verify` |

---

## API 키 등록

### 방법 A: CLI로 계정 추가 (권장)

```bash
# 업비트
./scripts/secrets.sh add-account upbit upbit_main "메인 계정" "ACCESS_KEY" "SECRET_KEY" 1.0

# 빗썸
./scripts/secrets.sh add-account bithumb bithumb_main "메인 계정" "API_KEY" "SECRET_KEY" 1.0

# 바이낸스
./scripts/secrets.sh add-account binance binance_main "선물 메인" "API_KEY" "SECRET_KEY" 1.0

# MEXC
./scripts/secrets.sh add-account mexc mexc_main "메인 계정" "API_KEY" "SECRET_KEY" 1.0
```

### 방법 B: 템플릿에서 최초 생성

```bash
# 1. 템플릿 복사 → 실제 키 입력
cp secrets.yaml.template secrets.yaml
vi secrets.yaml

# 2. 암호화 후 평문 삭제
sops -e secrets.yaml > secrets.enc.yaml
rm secrets.yaml
```

### 방법 C: 에디터로 직접 편집

```bash
./scripts/secrets.sh edit
# 또는: sops secrets.enc.yaml
```

에디터가 열리면 평문 YAML이 표시됨. 편집 후 저장하면 자동 재암호화.

---

## API 키 조회

```bash
# 전체 계정 목록 (키 값 마스킹됨)
./scripts/secrets.sh list

# 특정 거래소 상세 조회 (키 앞4자리...뒤4자리로 마스킹)
./scripts/secrets.sh get upbit
./scripts/secrets.sh get binance

# 특정 필드만 조회 (평문 노출)
./scripts/secrets.sh get upbit api_key
```

### sops 직접 사용

```bash
# 전체 평문 출력 (주의: 화면에 노출)
sops -d secrets.enc.yaml

# 특정 거래소만
sops -d secrets.enc.yaml | yq '.exchanges.upbit'

# 계정 ID만 (값 숨김)
sops -d secrets.enc.yaml | yq '.exchanges[].accounts[].id'
```

---

## API 키 변경

```bash
# CLI로 특정 필드 변경
./scripts/secrets.sh set upbit api_key "NEW_ACCESS_KEY"
./scripts/secrets.sh set upbit api_secret "NEW_SECRET_KEY"

# 두 번째 계정(index 1) 변경
./scripts/secrets.sh set binance api_key "NEW_KEY" 1

# 에디터로 변경
./scripts/secrets.sh edit
```

---

## API 키 삭제

```bash
# 특정 계정 삭제
./scripts/secrets.sh remove-account upbit upbit_sub

# 에디터로 삭제
./scripts/secrets.sh edit
# → 해당 거래소/계정 블록 삭제 → 저장
```

---

## 다중 계정

거래소당 여러 계정 등록 가능. `weight`로 주문 분배 비율 설정.

```bash
# 메인 계정 (weight 1.0)
./scripts/secrets.sh add-account upbit upbit_main "메인" "KEY1" "SECRET1" 1.0

# 보조 계정 (weight 0.5 → 메인의 절반 비율로 주문)
./scripts/secrets.sh add-account upbit upbit_sub "보조" "KEY2" "SECRET2" 0.5
```

---

## 팀원 관리

```bash
# 팀원 추가 (공개키 등록 + 재암호화)
./scripts/secrets.sh add-user "홍길동" "age1abc123..."

# 팀원 제거 (공개키 삭제 + 재암호화)
./scripts/secrets.sh remove-user "홍길동"
# ⚠️ 제거 후 반드시 모든 API 키 로테이션 필요!
```

---

## 실행

```bash
# 단일 프로세스
./scripts/start.sh --standalone

# 8 프로세스 (Watchdog)
./scripts/start.sh --engine

# Dry Run (실제 주문 없음)
./scripts/start.sh --standalone --dry-run
```

`start.sh`가 자동으로:
1. `secrets.enc.yaml` 복호화 (메모리에서만)
2. `config/config.yaml`과 병합
3. 프로세스에 stdin으로 전달
4. 평문 즉시 해제

---

## 검증 & 문제 해결

```bash
# 복호화 테스트
./scripts/secrets.sh verify
```

### "Failed to decrypt"
```bash
# age 키 존재 확인
ls -la ~/.config/sops/age/keys.txt

# 공개키가 .sops.yaml에 등록되어 있는지 확인
cat ~/.config/sops/age/keys.txt | grep "public key"
cat .sops.yaml
```

### "sops not found"
```bash
which sops && sops --version
```

---

## 요약

| 작업 | CLI 명령어 |
|------|-----------|
| **목록** | `secrets.sh list` |
| **조회** | `secrets.sh get <거래소>` |
| **등록** | `secrets.sh add-account <거래소> <id> <label> <key> <secret> [weight]` |
| **변경** | `secrets.sh set <거래소> <필드> <값>` |
| **삭제** | `secrets.sh remove-account <거래소> <id>` |
| **편집** | `secrets.sh edit` |
| **검증** | `secrets.sh verify` |
| **실행** | `start.sh --standalone` |
