# Claude Code 규칙 및 작업 가이드

> ⚠️ **이 문서는 모든 태스크 수행 전 필독**

---

## 📌 세션 시작 명령어

### 🔄 이어서 작업하기 (RESUME)

새 세션에서 이전 작업을 이어서 할 때 아래 명령어 사용:

```
/resume 또는 "이어서 작업해줘"
```

**Claude Code가 수행할 동작:**
```
1. PROGRESS.md 파일 확인
2. 마지막 완료된 태스크 확인
3. 현재 진행 중인 태스크 상태 확인
4. 다음 작업 제안
```

### 📊 현재 상태 확인 (STATUS)

```
/status 또는 "현재 상태 알려줘"
```

**Claude Code가 수행할 동작:**
```
1. 완료된 태스크 목록
2. 진행 중인 태스크와 진행률
3. 남은 태스크 목록
4. 예상 남은 시간
```

### 🆕 새로 시작하기 (START)

```
/start 또는 "처음부터 시작해줘"
```

---

## 📋 PROGRESS.md 파일 형식

Claude Code는 반드시 이 파일을 유지/업데이트해야 함:

```markdown
# 프로젝트 진행 상황

## 📅 마지막 업데이트
- 날짜: 2024-11-27 15:30 KST
- 세션: #3

## ✅ 완료된 태스크
| # | 태스크 | 완료일 | 비고 |
|---|--------|--------|------|
| 01 | Project Setup | 2024-11-25 | CMake, vcpkg 설정 완료 |
| 02 | Upbit WebSocket | 2024-11-26 | 테스트 통과 |
| 03 | Binance WebSocket | 2024-11-27 | 테스트 통과 |

## 🔄 진행 중인 태스크
### TASK_04: Bithumb WebSocket
- 상태: 70% 완료
- 완료 항목:
  - [x] 기본 연결
  - [x] 시세 구독
  - [ ] 자동 재연결
  - [ ] 단위 테스트
- 마지막 작업: `src/exchange/bithumb/websocket.cpp` 재연결 로직 구현 중
- 다음 작업: 재연결 타임아웃 처리

## 📝 현재 세션 작업 로그
- 15:00 - TASK_04 시작
- 15:20 - WebSocket 연결 성공
- 15:30 - 시세 수신 확인

## ⚠️ 알려진 이슈
- Bithumb API rate limit 주의 (분당 100회)
- OpenSSL 3.0에서 deprecation warning 있음

## 📌 다음 세션에서 할 일
1. TASK_04 재연결 로직 완료
2. 단위 테스트 작성
3. TASK_05로 이동

## 🔧 개발 환경 상태
- 빌드 상태: ✅ 성공
- 테스트 상태: ⚠️ 3/5 통과
- 마지막 커밋: `abc123` - "feat: add bithumb ticker subscription"
```

---

## 🚀 저지연(Low-Latency) 설계 원칙

> ⚠️ **모든 새 코드는 이 원칙을 준수해야 함**

### 1. Zero-Copy (복사 금지)
```cpp
// ❌ 금지 - 값 복사
void process(Ticker ticker) { ... }
queue.push(ticker);  // 복사 발생

// ✅ 권장 - 포인터 전달
void process(Ticker* ticker) { ... }
queue.push(ticker);  // 포인터만 전달

// Zero-Copy Queue 사용
ZeroCopyQueue<Ticker> queue(1024);
Ticker* t = ticker_pool().create();
queue.push(t);  // 포인터만 저장
```

### 2. Lock-Free 통신
```cpp
// ❌ 금지 - std::mutex
std::mutex mtx;
{
    std::lock_guard<std::mutex> lock(mtx);
    data = value;  // 락 오버헤드: 50~500ns
}

// ✅ 권장 - SPSC Queue
SPSCQueue<Ticker*> queue(1024);
queue.push(ticker);  // Lock-free: 5~20ns
```

### 3. Cache-Line Awareness
```cpp
// ❌ 금지 - 정렬 없음
struct Ticker {
    double price;
    double bid;
    // ... false sharing 위험
};

// ✅ 필수 - alignas(64)
struct alignas(64) Ticker {
    double price;
    double bid;
    // ... 캐시 라인 정렬
};
static_assert(sizeof(Ticker) == 64, "Must be cache-line sized");
```

### 4. Deterministic Performance (런타임 할당 금지)
```cpp
// ❌ 금지 - 런타임 new/malloc
Ticker* t = new Ticker();  // 100~1000ns

// ✅ 권장 - Memory Pool
Ticker* t = ticker_pool().create();  // 10ns
if (!t) {
    // 풀 소진 처리 - fallback 없음!
    logger->error("Ticker pool exhausted!");
    return nullptr;
}
```

### 5. 고정 크기 데이터 구조
```cpp
// ❌ 금지 - 동적 크기
struct Ticker {
    std::string symbol;  // 힙 할당
    std::vector<double> data;  // 힙 할당
};

// ✅ 권장 - 고정 크기
struct Ticker {
    char symbol[16];  // 스택/풀 할당
    double data[10];  // 고정 크기
};
```

### 6. SIMD 가속 (호가 연산)
```cpp
// Phase 4 (TASK_10)에서 구현 예정
// AVX-512를 이용한 호가창 병렬 연산
```

### 핵심 원칙 요약

| 원칙 | 금지 | 권장 |
|------|------|------|
| Zero-Copy | 값 복사 | 포인터 전달 |
| Lock-Free | std::mutex | SPSC/MPSC Queue |
| Cache-Line | 비정렬 구조체 | alignas(64) |
| Deterministic | new/malloc | Memory Pool |
| 고정 크기 | std::string, std::vector | char[], 고정 배열 |

---

## ⚠️ 코드 품질 규칙

### ❌ 절대 금지 (위반 시 태스크 실패 처리)

#### 1. Mock/Stub 데이터 금지
```cpp
// ❌ 금지 - 하드코딩된 응답
return R"({"price": 100.0, "volume": 1000})";

// ❌ 금지 - TODO 주석
// TODO: 나중에 구현
// FIXME: 임시 코드

// ✅ 허용 - 실제 API 호출
auto response = http_client_.get(UPBIT_API_URL + "/ticker");
return response.body();
```

#### 2. 빈 구현 금지
```cpp
// ❌ 금지
void process_message(const std::string& msg) {
    // 구현 예정
}

// ❌ 금지
Result<void> connect() {
    throw std::runtime_error("Not implemented");
}

// ✅ 필수 - 완전한 구현
void process_message(const std::string& msg) {
    auto json = nlohmann::json::parse(msg);
    auto ticker = parse_ticker(json);
    callback_(ticker);
}
```

#### 3. 에러 삼키기 금지
```cpp
// ❌ 금지 - 에러 무시
try {
    connect();
} catch (...) {
    // 무시
}

// ❌ 금지 - 빈 catch
try {
    connect();
} catch (const std::exception& e) {
}

// ✅ 필수 - 에러 처리 + 로깅
try {
    connect();
} catch (const std::exception& e) {
    logger_->error("Connection failed: {}", e.what());
    schedule_reconnect();
    throw;  // 또는 Result<> 반환
}
```

#### 4. 검증 없는 완료 선언 금지
```
// ❌ 금지
"TASK_02 완료했습니다."

// ✅ 필수 - 증거 제시
"TASK_02 완료:
- 빌드: cmake --build . 성공
- 테스트: 5/5 통과
- 실제 연결: Upbit에서 XRP 시세 수신 확인 (830.5 KRW)"
```

---

### ✅ 필수 준수

#### 1. 실제 API 엔드포인트 사용
```cpp
// ✅ 실제 엔드포인트
namespace endpoints {
    constexpr auto UPBIT_WS = "wss://api.upbit.com/websocket/v1";
    constexpr auto UPBIT_REST = "https://api.upbit.com/v1";
    constexpr auto BINANCE_WS = "wss://stream.binance.com:9443/ws";
    constexpr auto BINANCE_REST = "https://api.binance.com/api/v3";
    constexpr auto BITHUMB_WS = "wss://pubwss.bithumb.com/pub/ws";
    constexpr auto BITHUMB_REST = "https://api.bithumb.com/public";
    constexpr auto MEXC_WS = "wss://wbs.mexc.com/ws";
    constexpr auto MEXC_REST = "https://api.mexc.com/api/v3";
}

// ❌ 가짜 엔드포인트 금지
constexpr auto TEST_URL = "ws://localhost:8080/mock";
```

#### 2. 에러 처리 패턴
```cpp
// 모든 외부 호출은 Result<T> 패턴 사용
template<typename T>
class Result {
    std::variant<T, Error> data_;
public:
    bool has_value() const;
    T& value();
    Error& error();
};

// 사용 예
Result<Ticker> get_ticker(const std::string& symbol) {
    auto response = http_client_.get(url);
    
    if (!response.ok()) {
        logger_->error("API error: {} - {}", response.status(), response.body());
        return Err(ErrorCode::ApiError, response.body());
    }
    
    try {
        auto json = nlohmann::json::parse(response.body());
        return Ok(parse_ticker(json));
    } catch (const nlohmann::json::exception& e) {
        logger_->error("Parse error: {}", e.what());
        return Err(ErrorCode::ParseError, e.what());
    }
}
```

#### 3. 로깅 필수 포인트
```cpp
// 반드시 로깅해야 하는 상황:
logger_->info("WebSocket connected to {}", url);           // 연결 성공
logger_->warn("Reconnecting in {}ms", delay);              // 재연결 시도
logger_->error("Order failed: {}", error_msg);             // 주문 실패
logger_->info("Order filled: {} {} @ {}", qty, sym, price); // 주문 체결
logger_->debug("Received ticker: {}", ticker.to_string()); // 시세 수신 (debug)
```

#### 4. 테스트 필수 항목
```cpp
// 각 클래스/모듈에 필수 테스트:

// 1. 정상 케이스
TEST(UpbitWebSocket, ConnectAndReceiveTicker) { ... }

// 2. 에러 케이스
TEST(UpbitWebSocket, HandleInvalidMessage) { ... }
TEST(UpbitWebSocket, HandleDisconnection) { ... }

// 3. 경계값
TEST(PremiumCalculator, HandleZeroPrice) { ... }
TEST(PremiumCalculator, HandleNegativePremium) { ... }

// 4. 타임아웃 (네트워크 테스트)
TEST(UpbitWebSocket, ConnectionTimeout) {
    // 5초 타임아웃 설정 필수
    ASSERT_TIMEOUT(5s, client.connect());
}
```

---

## 🔍 태스크 완료 체크리스트

각 태스크 완료 시 반드시 확인:

```markdown
## TASK_XX 완료 체크리스트

### 빌드 검증
- [ ] `cmake --build build/` 성공
- [ ] 경고(warning) 0개 (또는 알려진 것만)
- [ ] 링크 에러 없음

### 코드 품질
- [ ] TODO/FIXME 주석 없음
- [ ] 모든 함수에 에러 처리 있음
- [ ] 모든 에러에 로깅 있음
- [ ] 하드코딩된 테스트 데이터 없음

### 테스트
- [ ] 단위 테스트 작성
- [ ] 테스트 전체 통과
- [ ] 정상 케이스 테스트 있음
- [ ] 에러 케이스 테스트 있음

### 기능 검증
- [ ] 실제 API 호출 확인
- [ ] 예상 동작 확인
- [ ] 태스크 완료 조건 충족

### 문서
- [ ] PROGRESS.md 업데이트
- [ ] 필요시 주석 추가
```

---

## 📋 태스크 완료 보고 양식

```markdown
## TASK_XX: [태스크명] 완료 보고

### 📅 완료 일시
YYYY-MM-DD HH:MM KST

### 🔨 빌드 결과
```
[빌드 로그 - 최소 마지막 10줄]
```

### 🧪 테스트 결과
```
[테스트 실행 결과]
[==========] N tests from M test suites ran.
[  PASSED  ] N tests.
```

### ✅ 체크리스트
- [x] 빌드 성공
- [x] 테스트 통과 (N/N)
- [x] TODO 없음
- [x] 에러 처리 완료
- [x] 로깅 완료
- [x] 실제 API 확인

### 📝 구현 내용 요약
- 파일1.cpp: 주요 구현 내용
- 파일2.hpp: 인터페이스 정의

### ⚠️ 알려진 제한사항
- (있다면 기술)

### 📎 다음 태스크
TASK_YY: [다음 태스크명]
```

---

## 🔄 세션 종료 시 필수 작업

Claude Code는 세션 종료 전 반드시:

```markdown
## 세션 종료 체크리스트

1. [ ] PROGRESS.md 업데이트
   - 완료된 태스크 추가
   - 진행 중인 태스크 상태 기록
   - 다음 할 일 명시

2. [ ] 현재 작업 상태 저장
   - 마지막 수정 파일 목록
   - 미완료 항목 명시

3. [ ] 빌드 가능 상태 확인
   - 컴파일 에러 없음
   - (가능하면) 테스트 통과

4. [ ] 다음 세션 시작점 명시
   예: "다음 세션에서 TASK_04의 reconnect() 함수부터 이어서 작업"
```

---

## 📌 자주 사용하는 명령어

| 명령어 | 설명 |
|--------|------|
| `/resume` | 이전 작업 이어서 하기 |
| `/status` | 현재 진행 상황 확인 |
| `/start` | 처음부터 시작 |
| `/test` | 전체 테스트 실행 |
| `/build` | 프로젝트 빌드 |
| `/check` | 코드 품질 검사 (TODO, 에러처리 등) |
| `/next` | 다음 태스크로 이동 |
| `/save` | 현재 상태 저장 (PROGRESS.md 업데이트) |
| `/commit` | 변경사항 커밋 |
| `/push` | 원격 저장소에 푸시 |

---

## 🔄 Git 워크플로우

### 커밋 규칙

```bash
# 커밋 메시지 형식
<type>(<scope>): <subject>

# 타입
feat:     새 기능
fix:      버그 수정
refactor: 리팩토링
test:     테스트 추가/수정
docs:     문서 수정
chore:    빌드, 설정 등

# 예시
feat(websocket): add Upbit WebSocket connection
fix(order): handle timeout in dual executor
test(premium): add unit tests for premium calculator
```

### 태스크 완료 시 Git 명령어

```bash
# 1. 변경사항 확인
git status
git diff

# 2. 스테이징
git add -A
# 또는 선택적으로
git add src/exchange/upbit/

# 3. 커밋
git commit -m "feat(upbit): complete TASK_02 WebSocket implementation

- Add real-time ticker subscription
- Implement auto-reconnection
- Add unit tests (5/5 passed)

Closes #2"

# 4. 푸시
git push origin main
# 또는 브랜치 작업 시
git push origin feature/task-02-upbit-websocket
```

### 브랜치 전략 (선택)

```bash
# 태스크별 브랜치 생성
git checkout -b feature/task-XX-description

# 작업 완료 후 main에 머지
git checkout main
git merge feature/task-XX-description
git push origin main

# 브랜치 삭제
git branch -d feature/task-XX-description
```

### Claude Code Git 명령어

```bash
# 커밋만
claude "/commit"
# 또는
claude "현재 변경사항 커밋해줘"

# 커밋 + 푸시
claude "/push"
# 또는
claude "커밋하고 푸시해줘"

# 상세 지시
claude "
TASK_02 완료했으니:
1. git status로 변경사항 확인
2. 커밋 메시지는 'feat(upbit): complete WebSocket implementation'
3. main 브랜치에 푸시
"
```

### 세션 종료 시 Git 체크리스트

```markdown
## 세션 종료 전 Git 체크리스트

1. [ ] 모든 변경사항 커밋됨
2. [ ] 커밋 메시지 규칙 준수
3. [ ] 원격 저장소에 푸시 완료
4. [ ] PROGRESS.md도 커밋에 포함
5. [ ] 빌드 가능한 상태로 커밋 (깨진 코드 푸시 금지)
```

### ⚠️ Git 주의사항

```
1. 깨진 코드 푸시 금지
   - 컴파일 안 되는 코드 커밋하지 않음
   - 푸시 전 반드시 빌드 확인

2. 대용량 파일 주의
   - 바이너리, 빌드 결과물 커밋 금지
   - .gitignore 설정 확인

3. API 키 노출 금지
   - config/*.yaml에 실제 키 넣지 않음
   - config/*.yaml.example 만 커밋
   
4. 자주 커밋
   - 기능 단위로 작은 커밋
   - "WIP" 커밋보다 의미있는 단위로
```

---

## ⚠️ 중요 알림

### Claude Code가 반드시 지켜야 할 것:

1. **거짓말 금지** - 실제로 동작하지 않는 코드를 "완료"라고 하지 않음
2. **증거 제시** - 완료 선언 시 빌드/테스트 결과 첨부
3. **상태 유지** - PROGRESS.md 항상 최신 상태로 유지
4. **투명성** - 모르는 것은 모른다고 말함, 추측하지 않음

### 사용자가 확인해야 할 것:

1. 세션 시작 시 `/status` 또는 `/resume`으로 상태 확인
2. 태스크 완료 보고서의 테스트 결과 확인
3. 의심스러우면 직접 빌드/테스트 실행
