# TASK_38: 4개 Feeder 실행 파일

> Phase 2

---

## 📌 목표

거래소별 독립 Feeder 프로세스 실행 파일 생성

---

## 🔧 실행 파일

| 실행 파일 | 소스 | WebSocket 클래스 |
|-----------|------|-----------------|
| `upbit-feeder` | `src/feeder/upbit_main.cpp` | UpbitWebSocket |
| `bithumb-feeder` | `src/feeder/bithumb_main.cpp` | BithumbWebSocket |
| `binance-feeder` | `src/feeder/binance_main.cpp` | BinanceWebSocket |
| `mexc-feeder` | `src/feeder/mexc_main.cpp` | MEXCWebSocket |

### main() 구조 (모두 동일 패턴)

```cpp
int main(int argc, char* argv[]) {
    auto config = parse_feeder_args(argc, argv);
    FeederProcess feeder(Exchange::Upbit, config);
    feeder.run();
    return 0;
}
```

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `src/feeder/upbit_main.cpp` | Upbit feeder |
| `src/feeder/bithumb_main.cpp` | Bithumb feeder |
| `src/feeder/binance_main.cpp` | Binance feeder |
| `src/feeder/mexc_main.cpp` | MEXC feeder |
| `src/feeder/CMakeLists.txt` | 빌드 설정 |

## 📎 의존성: TASK_37
## ⏱️ 예상: 0.5일
