# Kimchi Arbitrage C++ Project

## 📋 프로젝트 개요

XRP 김치 프리미엄 아비트라지 트레이딩 시스템 (C++ 구현)

- **거래소**: 업비트, 빗썸 (국내) ↔ 바이낸스, MEXC (해외)
- **목표**: 저지연 자동 매매 시스템

---

## ⚠️ 작업 전 필독

1. **규칙**: `.claude/CLAUDE_CODE_RULES.md` - 반드시 먼저 읽을 것
2. **순서**: `.claude/TASK_ORDER.md` - 태스크 실행 순서
3. **진행**: `.claude/PROGRESS.md` - 현재 진행 상황

---

## 🔧 작업 방법

### 태스크 진행
```
1. .claude/TASK_ORDER.md 확인
2. 해당 태스크 파일 읽기: .claude/tasks/TASK_XX_*.md
3. 구현 후 PROGRESS.md 업데이트
4. Git 커밋
```

### 현재 태스크 확인
`.claude/PROGRESS.md`에서 `[ ]` 표시된 첫 번째 태스크가 현재 작업 대상

---

## 📁 프로젝트 구조

```
kimchi-arbitrage-cpp/
├── CLAUDE.md              ← 이 파일
├── .claude/               ← Claude Code 전용
│   ├── CLAUDE_CODE_RULES.md
│   ├── CLAUDE_CODE_GUIDE.md
│   ├── TASK_ORDER.md
│   ├── PROGRESS.md
│   └── tasks/             ← 29개 태스크 문서
├── docs/                  ← 참고 문서
├── include/               ← 헤더 파일
├── src/                   ← 소스 파일
├── tests/                 ← 테스트
└── CMakeLists.txt
```

---

## 🛠️ 기술 스택

| 용도 | 라이브러리 |
|------|-----------|
| WebSocket | Boost.Beast |
| HTTP | libcurl |
| JSON | simdjson |
| 로깅 | spdlog |
| 빌드 | CMake + vcpkg |

---

## 📌 핵심 원칙

1. **Lock-Free**: `std::mutex` 사용 금지, SPSC Queue 사용
2. **Memory Pool**: `new/delete` 최소화, Object Pool 사용
3. **Spin Wait**: `sleep` 금지, Busy Polling 사용
4. **Thread Pinning**: 코어 고정으로 캐시 최적화

---

## 🚀 시작하기

```bash
# 첫 번째 태스크부터 시작
# .claude/tasks/TASK_01_project_setup.md 참조
```
