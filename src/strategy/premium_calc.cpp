#include "arbitrage/strategy/premium_calc.hpp"
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