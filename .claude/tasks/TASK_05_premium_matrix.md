# TASK 07: 김프 매트릭스 (C++)

## 🎯 목표
4개 거래소 간 실시간 김프 계산 및 매트릭스 관리

---

## ⚠️ 주의사항

```
필수:
- 스레드 안전 (shared_mutex)
- NaN 처리
- 환율 반영
- 변경 시 콜백
```

---

## 📁 생성할 파일

```
include/arbitrage/strategy/
└── premium_calc.hpp
src/strategy/
└── premium_calc.cpp
```

---

## 📝 상세 구현

### 1. include/arbitrage/strategy/premium_calc.hpp

```cpp
#pragma once

#include "arbitrage/common/types.hpp"
#include <array>
#include <shared_mutex>
#include <functional>
#include <atomic>
#include <optional>
#include <cmath>

namespace arbitrage {

// 김프 정보
struct PremiumInfo {
    Exchange buy_exchange;
    Exchange sell_exchange;
    double premium_pct;
    double buy_price;        // KRW 환산
    double sell_price;       // KRW 환산
    double fx_rate;
    std::chrono::system_clock::time_point timestamp;
    
    bool is_valid() const { return !std::isnan(premium_pct); }
};

// 콜백 타입
using PremiumCallback = std::function<void(const PremiumInfo&)>;

// 김프 계산기
class PremiumCalculator {
public:
    PremiumCalculator();
    
    // 가격 업데이트
    void update_price(Exchange ex, double price);
    
    // 환율 업데이트
    void update_fx_rate(double rate);
    
    // 김프 조회 (buy -> sell)
    double get_premium(Exchange buy, Exchange sell) const;
    
    // 전체 매트릭스 조회
    PremiumMatrix get_matrix() const;
    
    // 최고 김프 기회 조회
    std::optional<PremiumInfo> get_best_opportunity() const;
    
    // 특정 임계값 이상 기회 조회
    std::vector<PremiumInfo> get_opportunities(double min_premium_pct) const;
    
    // 콜백 설정
    void on_premium_changed(PremiumCallback cb) { callback_ = std::move(cb); }
    
    // 임계값 설정 (이 이상일 때만 콜백)
    void set_threshold(double threshold_pct) { threshold_ = threshold_pct; }
    
private:
    // 매트릭스 재계산
    void recalculate();
    
    // KRW 가격으로 변환
    double to_krw(Exchange ex, double price) const;
    
    // 김프 계산 공식
    // 김프(%) = (국내가 - 해외가×환율) / (해외가×환율) × 100
    double calc_premium(double buy_krw, double sell_krw) const;
    
private:
    mutable std::shared_mutex mutex_;
    
    // 거래소별 가격 (원시 값)
    std::array<std::atomic<double>, 4> prices_{};
    
    // 환율
    std::atomic<double> fx_rate_{1350.0};  // 기본값
    
    // 김프 매트릭스 [buy][sell]
    PremiumMatrix matrix_;
    
    // 콜백
    PremiumCallback callback_;
    double threshold_{0.0};
    
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace arbitrage
```

### 2. src/strategy/premium_calc.cpp

```cpp
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/common/logger.hpp"
#include <limits>

namespace arbitrage {

PremiumCalculator::PremiumCalculator()
    : logger_(Logger::create("premium"))
{
    // 초기화: NaN으로
    for (auto& row : matrix_) {
        row.fill(std::numeric_limits<double>::quiet_NaN());
    }
    
    for (auto& p : prices_) {
        p.store(0.0);
    }
}

void PremiumCalculator::update_price(Exchange ex, double price) {
    prices_[static_cast<size_t>(ex)].store(price);
    recalculate();
}

void PremiumCalculator::update_fx_rate(double rate) {
    fx_rate_.store(rate);
    recalculate();
}

double PremiumCalculator::to_krw(Exchange ex, double price) const {
    if (is_krw_exchange(ex)) {
        return price;  // 이미 KRW
    }
    return price * fx_rate_.load();  // USDT -> KRW
}

double PremiumCalculator::calc_premium(double buy_krw, double sell_krw) const {
    if (buy_krw <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // 김프(%) = (매도가 - 매수가) / 매수가 × 100
    return ((sell_krw - buy_krw) / buy_krw) * 100.0;
}

void PremiumCalculator::recalculate() {
    std::unique_lock lock(mutex_);
    
    double fx = fx_rate_.load();
    
    // 매트릭스 계산
    for (int buy = 0; buy < 4; ++buy) {
        for (int sell = 0; sell < 4; ++sell) {
            if (buy == sell) {
                matrix_[buy][sell] = 0.0;
                continue;
            }
            
            double buy_price = prices_[buy].load();
            double sell_price = prices_[sell].load();
            
            if (buy_price <= 0.0 || sell_price <= 0.0) {
                matrix_[buy][sell] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            
            double buy_krw = to_krw(static_cast<Exchange>(buy), buy_price);
            double sell_krw = to_krw(static_cast<Exchange>(sell), sell_price);
            
            matrix_[buy][sell] = calc_premium(buy_krw, sell_krw);
        }
    }
    
    // 콜백 호출 (임계값 이상인 경우)
    if (callback_) {
        for (int buy = 0; buy < 4; ++buy) {
            for (int sell = 0; sell < 4; ++sell) {
                if (buy == sell) continue;
                
                double prem = matrix_[buy][sell];
                if (!std::isnan(prem) && prem >= threshold_) {
                    PremiumInfo info;
                    info.buy_exchange = static_cast<Exchange>(buy);
                    info.sell_exchange = static_cast<Exchange>(sell);
                    info.premium_pct = prem;
                    info.buy_price = to_krw(static_cast<Exchange>(buy), prices_[buy].load());
                    info.sell_price = to_krw(static_cast<Exchange>(sell), prices_[sell].load());
                    info.fx_rate = fx;
                    info.timestamp = std::chrono::system_clock::now();
                    
                    // 락 해제 후 콜백 (데드락 방지)
                    lock.unlock();
                    callback_(info);
                    lock.lock();
                }
            }
        }
    }
}

double PremiumCalculator::get_premium(Exchange buy, Exchange sell) const {
    std::shared_lock lock(mutex_);
    return matrix_[static_cast<size_t>(buy)][static_cast<size_t>(sell)];
}

PremiumMatrix PremiumCalculator::get_matrix() const {
    std::shared_lock lock(mutex_);
    return matrix_;
}

std::optional<PremiumInfo> PremiumCalculator::get_best_opportunity() const {
    std::shared_lock lock(mutex_);
    
    double best_premium = std::numeric_limits<double>::lowest();
    int best_buy = -1, best_sell = -1;
    
    for (int buy = 0; buy < 4; ++buy) {
        for (int sell = 0; sell < 4; ++sell) {
            if (buy == sell) continue;
            
            double prem = matrix_[buy][sell];
            if (!std::isnan(prem) && prem > best_premium) {
                best_premium = prem;
                best_buy = buy;
                best_sell = sell;
            }
        }
    }
    
    if (best_buy < 0) {
        return std::nullopt;
    }
    
    PremiumInfo info;
    info.buy_exchange = static_cast<Exchange>(best_buy);
    info.sell_exchange = static_cast<Exchange>(best_sell);
    info.premium_pct = best_premium;
    info.buy_price = to_krw(info.buy_exchange, prices_[best_buy].load());
    info.sell_price = to_krw(info.sell_exchange, prices_[best_sell].load());
    info.fx_rate = fx_rate_.load();
    info.timestamp = std::chrono::system_clock::now();
    
    return info;
}

std::vector<PremiumInfo> PremiumCalculator::get_opportunities(double min_premium_pct) const {
    std::shared_lock lock(mutex_);
    
    std::vector<PremiumInfo> results;
    
    for (int buy = 0; buy < 4; ++buy) {
        for (int sell = 0; sell < 4; ++sell) {
            if (buy == sell) continue;
            
            double prem = matrix_[buy][sell];
            if (!std::isnan(prem) && prem >= min_premium_pct) {
                PremiumInfo info;
                info.buy_exchange = static_cast<Exchange>(buy);
                info.sell_exchange = static_cast<Exchange>(sell);
                info.premium_pct = prem;
                info.buy_price = to_krw(info.buy_exchange, prices_[buy].load());
                info.sell_price = to_krw(info.sell_exchange, prices_[sell].load());
                info.fx_rate = fx_rate_.load();
                info.timestamp = std::chrono::system_clock::now();
                
                results.push_back(info);
            }
        }
    }
    
    // 김프 높은 순 정렬
    std::sort(results.begin(), results.end(), 
        [](const PremiumInfo& a, const PremiumInfo& b) {
            return a.premium_pct > b.premium_pct;
        });
    
    return results;
}

}  // namespace arbitrage
```

### 3. 사용 예시

```cpp
#include "arbitrage/strategy/premium_calc.hpp"
#include <iostream>

int main() {
    arbitrage::PremiumCalculator calc;
    
    // 임계값 설정 (3% 이상만 콜백)
    calc.set_threshold(3.0);
    
    // 콜백 설정
    calc.on_premium_changed([](const arbitrage::PremiumInfo& info) {
        std::cout << "Opportunity: " 
                  << arbitrage::exchange_name(info.buy_exchange) << " -> "
                  << arbitrage::exchange_name(info.sell_exchange)
                  << " " << info.premium_pct << "%\n";
    });
    
    // 환율 설정
    calc.update_fx_rate(1350.0);
    
    // 가격 업데이트 (시뮬레이션)
    calc.update_price(arbitrage::Exchange::Binance, 0.62);    // USDT
    calc.update_price(arbitrage::Exchange::MEXC, 0.621);      // USDT
    calc.update_price(arbitrage::Exchange::Upbit, 880.0);     // KRW
    calc.update_price(arbitrage::Exchange::Bithumb, 878.0);   // KRW
    
    // 최고 기회 조회
    auto best = calc.get_best_opportunity();
    if (best) {
        std::cout << "Best: " << best->premium_pct << "%\n";
    }
    
    // 매트릭스 출력
    auto matrix = calc.get_matrix();
    // ...
    
    return 0;
}
```

---

## ✅ 완료 조건 체크리스트

```
□ 4x4 김프 매트릭스 계산
□ KRW/USDT 환율 반영
□ NaN 처리
□ 스레드 안전 (shared_mutex)
□ 최고 기회 조회
□ 임계값 기반 콜백
□ 정렬된 기회 목록
```

---

## 📎 다음 태스크

완료 후: TASK_08_rtt_monitor.md
