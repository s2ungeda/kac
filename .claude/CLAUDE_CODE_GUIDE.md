# Claude Code 사용 가이드 (C++ 버전)

## 🎯 개요

이 프로젝트는 Claude Code를 활용하여 C++ 김프 아비트라지 시스템을 구현합니다.
Go 버전과 달리 C++의 특성(메모리 관리, 컴파일 등)을 고려한 가이드입니다.

> ⚠️ **필독 문서**
> - [CLAUDE_CODE_RULES.md](./CLAUDE_CODE_RULES.md) - 코드 품질 규칙 및 작업 연속성
> - [PROGRESS.md](./PROGRESS.md) - 현재 진행 상황

---

## 🚀 빠른 시작

### 세션 시작 시
```bash
# 이어서 작업
claude "/resume" 또는 "이어서 작업해줘"

# 상태 확인
claude "/status" 또는 "현재 상태 알려줘"

# 처음부터
claude "/start" 또는 "처음부터 시작해줘"
```

### 세션 종료 시
```bash
# 상태 저장
claude "/save" 또는 "진행상황 저장해줘"
```

---

## 📋 태스크 실행 방법

### 1. 태스크 파일 읽기

```bash
# Claude Code에 태스크 전달
claude "TASK_01_project_setup.md 파일을 읽고 구현해줘"
```

### 2. 구현 진행

```bash
# 단계별 진행
claude "TASK_01의 1단계: CMakeLists.txt 생성"
claude "TASK_01의 2단계: 디렉토리 구조 생성"
```

### 3. 테스트 및 검증

```bash
# 빌드 테스트
claude "빌드가 되는지 확인해줘"

# 단위 테스트
claude "TASK_01 완료 조건 체크리스트 검증해줘"
```

---

## 🔧 C++ 특화 지시사항

### 컴파일러 설정

```bash
# Claude에게 컴파일러 지정
claude "GCC 11 기준으로 구현해줘"
claude "C++20 기능 사용해도 돼"
```

### 빌드 시스템

```bash
# CMake 관련
claude "CMakeLists.txt에 Boost.Beast 의존성 추가해줘"
claude "vcpkg 툴체인 사용하도록 설정해줘"
```

### 메모리 관리

```bash
# 스마트 포인터 사용 지시
claude "raw pointer 대신 unique_ptr 사용해줘"
claude "shared_ptr은 정말 필요한 곳만 사용해줘"
```

---

## 📝 코드 스타일 가이드

### 네이밍 컨벤션

```cpp
// 클래스: PascalCase
class WebSocketClient {};

// 함수/메서드: snake_case
void connect_async();
Result<void> place_order();

// 변수: snake_case
int retry_count;
std::string api_key_;  // 멤버 변수는 _로 끝남

// 상수: SCREAMING_SNAKE_CASE
constexpr int MAX_RETRY_COUNT = 10;

// 네임스페이스: snake_case
namespace arbitrage::exchange::upbit {}
```

### 파일 구조

```cpp
// include/arbitrage/exchange/upbit/websocket.hpp
#pragma once

#include <memory>
#include <string>

namespace arbitrage::upbit {

class WebSocketClient {
public:
    // 생성자/소멸자
    WebSocketClient();
    ~WebSocketClient();
    
    // 복사/이동 금지 또는 명시
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    
    // 공개 메서드
    void connect(const std::string& url);
    void disconnect();
    
private:
    // 구현 세부사항
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arbitrage::upbit
```

### 에러 처리

```cpp
// std::expected (C++23) 또는 커스텀 Result<T>
template<typename T>
using Result = std::expected<T, Error>;

// 사용
Result<OrderResult> place_order(const OrderRequest& req) {
    if (!connected_) {
        return std::unexpected(Error{-1, "Not connected"});
    }
    // ...
    return order_result;
}
```

---

## 🏗️ 프로젝트 구조 규칙

### 헤더/소스 분리

```
include/arbitrage/       # 공개 헤더
├── common/
│   ├── types.hpp       # 공통 타입
│   └── config.hpp      # 설정
└── exchange/
    └── interface.hpp   # 인터페이스

src/                    # 구현
├── common/
│   ├── types.cpp
│   └── config.cpp
└── exchange/
    └── upbit/
        └── websocket.cpp
```

### CMake 모듈화

```cmake
# src/exchange/CMakeLists.txt
add_library(exchange
    upbit/websocket.cpp
    upbit/order.cpp
    binance/websocket.cpp
    binance/order.cpp
)

target_link_libraries(exchange
    PRIVATE
        common
        websockets
        curl
        OpenSSL::SSL
)
```

---

## 🔄 Boost.Beast 콜백 패턴

### Claude에게 지시할 때

```bash
claude "Boost.Beast 콜백 패턴으로 WebSocket 클라이언트 만들어줘"
claude "ICS OnDataAvailable처럼 메시지 수신 콜백 구현해줘"
```

### 예시 구조

```cpp
// Claude가 생성할 코드 패턴
static int callback_protocol(
    struct lws *wsi,
    enum beast_callback_reasons reason,
    void *user,
    void *in,
    size_t len
) {
    auto* client = static_cast<WebSocketClient*>(
        beast_context_user(beast_get_context(wsi))
    );
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            client->on_connected();
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
            client->on_message(static_cast<char*>(in), len);
            break;
            
        case LWS_CALLBACK_CLIENT_CLOSED:
            client->on_disconnected();
            break;
    }
    return 0;
}
```

---

## 📊 의존성 관리

### vcpkg 사용

```bash
# Claude에게 의존성 추가 지시
claude "vcpkg.json에 spdlog 추가해줘"
```

### vcpkg.json 예시

```json
{
    "name": "kimchi-arbitrage",
    "version": "1.0.0",
    "dependencies": [
        "Boost.Beast",
        "curl",
        "openssl",
        "nlohmann-json",
        "spdlog",
        "yaml-cpp",
        "sqlite3",
        "gtest",
        "msgpack-cxx"
    ]
}
```

---

## 🧪 테스트 가이드

### Google Test 사용

```bash
claude "Google Test로 WebSocketClient 단위 테스트 만들어줘"
```

### 테스트 구조

```cpp
// tests/unit/exchange/upbit/websocket_test.cpp
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "arbitrage/exchange/upbit/websocket.hpp"

class WebSocketClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        client_ = std::make_unique<arbitrage::upbit::WebSocketClient>();
    }
    
    std::unique_ptr<arbitrage::upbit::WebSocketClient> client_;
};

TEST_F(WebSocketClientTest, ConnectSuccess) {
    // 테스트 구현
}
```

---

## ⚠️ 주의사항

### 1. 메모리 누수 방지

```bash
claude "valgrind로 메모리 누수 체크할 수 있게 해줘"
claude "모든 raw pointer는 RAII로 감싸줘"
```

### 2. 스레드 안전성

```bash
claude "이 클래스를 스레드 안전하게 만들어줘"
claude "std::shared_mutex 사용해서 읽기/쓰기 락 분리해줘"
```

### 3. 예외 처리

```bash
claude "noexcept 붙일 수 있는 함수는 붙여줘"
claude "예외 대신 Result<T> 패턴 사용해줘"
```

---

## 📎 유용한 Claude 프롬프트

### 코드 리뷰

```bash
claude "이 코드 메모리 안전한지 검토해줘"
claude "성능 개선할 부분 있어?"
claude "C++ 베스트 프랙티스 따르고 있어?"
```

### 리팩토링

```bash
claude "이 클래스 PIMPL 패턴으로 바꿔줘"
claude "중복 코드 템플릿으로 통합해줘"
```

### 문서화

```bash
claude "Doxygen 주석 추가해줘"
claude "이 함수 사용법 예시 코드 만들어줘"
```
