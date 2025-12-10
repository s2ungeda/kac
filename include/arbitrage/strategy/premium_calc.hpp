#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/logger.hpp"
#include <array>
#include <shared_mutex>
#include <functional>
#include <atomic>
#include <optional>
#include <vector>
#include <algorithm>
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
    // 김프(%) = (매도가 - 매수가) / 매수가 × 100
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
    
    std::shared_ptr<SimpleLogger> logger_;
};

}  // namespace arbitrage