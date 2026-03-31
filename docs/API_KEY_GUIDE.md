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
# .sops.yaml 편집 — age: 줄에 공개키 추가
vi .sops.yaml
```

```yaml
creation_rules:
  - path_regex: secrets.*\.yaml$
    age: >-
      age1abc123def456...
```

---

## API 키 등록 (신규)

### 방법 A: 템플릿에서 생성 (최초)

```bash
# 1. 템플릿 복사
cp secrets.yaml.template secrets.yaml

# 2. 실제 키 입력
vi secrets.yaml
```

```yaml
exchanges:
  upbit:
    accounts:
      - id: "upbit_main"
        label: "메인 계정"
        api_key: "실제_업비트_ACCESS_KEY"
        api_secret: "실제_업비트_SECRET_KEY"
        weight: 1.0

  bithumb:
    accounts:
      - id: "bithumb_main"
        label: "메인 계정"
        api_key: "실제_빗썸_API_KEY"
        api_secret: "실제_빗썸_SECRET_KEY"
        weight: 1.0

  binance:
    accounts:
      - id: "binance_main"
        label: "선물 메인"
        api_key: "실제_바이낸스_API_KEY"
        api_secret: "실제_바이낸스_SECRET_KEY"
        weight: 1.0

  mexc:
    accounts:
      - id: "mexc_main"
        label: "메인 계정"
        api_key: "실제_MEXC_API_KEY"
        api_secret: "실제_MEXC_SECRET_KEY"
        weight: 1.0

alert:
  telegram_token: "텔레그램_봇_토큰"
  telegram_chat_id: "텔레그램_채팅_ID"

server:
  auth_token: "TCP_서버_인증_토큰"
```

```bash
# 3. 암호화
sops -e secrets.yaml > secrets.enc.yaml

# 4. 평문 즉시 삭제
rm secrets.yaml

# 5. 확인
git diff secrets.enc.yaml  # 암호화된 내용 확인
```

### 방법 B: sops 에디터로 직접 편집 (권장)

```bash
# sops가 자동으로 복호화 → 에디터 → 재암호화
sops secrets.enc.yaml
```

`$EDITOR` (기본: vim)가 열리면 평문 YAML이 표시됨. 편집 후 저장하면 자동 재암호화.

---

## API 키 조회

### 전체 키 확인 (복호화)

```bash
# 터미널에 평문 출력 (주의: 화면에 노출됨)
sops -d secrets.enc.yaml
```

### 특정 거래소만 확인

```bash
# 업비트 키만
sops -d secrets.enc.yaml | yq '.exchanges.upbit'

# 바이낸스 키만
sops -d secrets.enc.yaml | yq '.exchanges.binance'

# 알림 설정만
sops -d secrets.enc.yaml | yq '.alert'
```

### 키 이름만 확인 (값 숨김)

```bash
# 어떤 거래소가 등록되어 있는지만 확인
sops -d secrets.enc.yaml | yq '.exchanges | keys'

# 각 거래소 계정 ID만 확인
sops -d secrets.enc.yaml | yq '.exchanges[].accounts[].id'
```

---

## API 키 변경

### 방법 A: sops 에디터 (권장)

```bash
sops secrets.enc.yaml
# → 에디터에서 해당 키 값 수정 → 저장 → 자동 재암호화
```

### 방법 B: yq로 특정 값만 변경

```bash
# 1. 복호화
sops -d secrets.enc.yaml > secrets.yaml

# 2. yq로 특정 값 변경
yq -i '.exchanges.upbit.accounts[0].api_key = "NEW_KEY_HERE"' secrets.yaml
yq -i '.exchanges.upbit.accounts[0].api_secret = "NEW_SECRET_HERE"' secrets.yaml

# 3. 재암호화
sops -e secrets.yaml > secrets.enc.yaml

# 4. 평문 삭제
rm secrets.yaml
```

---

## API 키 삭제

### 특정 거래소 삭제

```bash
sops secrets.enc.yaml
# → 에디터에서 해당 거래소 블록 전체 삭제 → 저장
```

### 특정 계정만 삭제 (다중 계정 중 하나)

```bash
sops secrets.enc.yaml
# → 에디터에서 해당 accounts 항목만 삭제 → 저장
```

### yq로 삭제

```bash
sops -d secrets.enc.yaml > secrets.yaml
yq -i 'del(.exchanges.mexc)' secrets.yaml
sops -e secrets.yaml > secrets.enc.yaml
rm secrets.yaml
```

---

## 계정 추가 (다중 계정)

거래소당 여러 계정을 등록할 수 있음. `weight`로 주문 분배 비율 설정.

```bash
sops secrets.enc.yaml
```

```yaml
exchanges:
  upbit:
    accounts:
      - id: "upbit_main"
        label: "메인 계정"
        api_key: "MAIN_KEY"
        api_secret: "MAIN_SECRET"
        weight: 1.0

      - id: "upbit_sub"        # 추가
        label: "보조 계정"
        api_key: "SUB_KEY"
        api_secret: "SUB_SECRET"
        weight: 0.5
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

## 문제 해결

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
# sops 설치 확인
which sops
sops --version
```

### 키 로테이션 (팀원 변경 시)
```bash
# 1. .sops.yaml에서 공개키 추가/삭제
vi .sops.yaml

# 2. 기존 파일 재암호화
sops updatekeys secrets.enc.yaml

# 3. 탈퇴한 팀원이 있으면 API 키 자체도 로테이션
sops secrets.enc.yaml  # 모든 API 키 변경
```

---

## 요약

| 작업 | 명령어 |
|------|--------|
| **등록** | `sops secrets.enc.yaml` (에디터에서 입력) |
| **조회** | `sops -d secrets.enc.yaml` |
| **변경** | `sops secrets.enc.yaml` (에디터에서 수정) |
| **삭제** | `sops secrets.enc.yaml` (에디터에서 삭제) |
| **실행** | `./scripts/start.sh --standalone` |
