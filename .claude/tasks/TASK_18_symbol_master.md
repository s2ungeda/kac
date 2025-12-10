# TASK 22: 심볼 마스터 (C++)

## 🎯 목표
거래소별 심볼 매핑 관리

---

## 📁 생성할 파일

```
include/arbitrage/common/
└── symbol_master.hpp
src/common/
└── symbol_master.cpp
```

---

## 📝 핵심 구현

```cpp
struct SymbolInfo {
    std::string base;      // XRP
    std::string quote;     // KRW, USDT
    std::string native;    // 거래소 네이티브 심볼
    double min_qty;
    double max_qty;
    double qty_step;
    double min_notional;
};

class SymbolMaster {
public:
    // 심볼 변환
    std::string to_native(Exchange ex, const std::string& unified);
    std::string to_unified(Exchange ex, const std::string& native);
    
    // 심볼 정보
    std::optional<SymbolInfo> get_info(Exchange ex, const std::string& symbol);
    
    // 수량 정규화
    double normalize_qty(Exchange ex, const std::string& symbol, double qty);
};

// 변환 예시
// unified: "XRP/KRW"
// Upbit:   "KRW-XRP"
// Bithumb: "XRP_KRW"
// Binance: "XRPUSDT" (KRW 없음)
// MEXC:    "XRPUSDT"
```

---

## ✅ 완료 조건

```
□ 심볼 변환
□ 정보 조회
□ 수량 정규화
□ 동적 업데이트
```

---

## 📎 다음 태스크

완료 후: TASK_23_event_bus.md
