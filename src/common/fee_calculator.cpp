/**
 * Fee Calculator Implementation (TASK_11)
 */

#include "arbitrage/common/fee_calculator.hpp"
#include "arbitrage/common/logger.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cmath>

// yaml-cpp (선택적)
#ifdef HAS_YAML_CPP
#include <yaml-cpp/yaml.h>
#endif

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
namespace { FeeCalculator* g_set_fee_calculator_override = nullptr; }
FeeCalculator& fee_calculator() {
    if (g_set_fee_calculator_override) return *g_set_fee_calculator_override;
    static FeeCalculator instance;
    return instance;
}
void set_fee_calculator(FeeCalculator* p) { g_set_fee_calculator_override = p; }

// =============================================================================
// 생성자
// =============================================================================
FeeCalculator::FeeCalculator() {
    init_default_configs();
}

// =============================================================================
// 기본 설정 초기화
// =============================================================================
void FeeCalculator::init_default_configs() {
    // Upbit
    {
        ExchangeFeeConfig config;
        config.exchange = Exchange::Upbit;
        config.maker_fee_pct = 0.05;
        config.taker_fee_pct = 0.05;
        config.token_discount_pct = 0.0;
        config.use_token_for_fee = false;
        config.vip_level = 0;
        config.withdraw_fees["XRP"] = 1.0;
        config.withdraw_fees["BTC"] = 0.0005;
        config.min_withdraw["XRP"] = 21.0;
        config.min_withdraw["BTC"] = 0.001;
        configs_[Exchange::Upbit] = config;
    }

    // Bithumb
    {
        ExchangeFeeConfig config;
        config.exchange = Exchange::Bithumb;
        config.maker_fee_pct = 0.04;
        config.taker_fee_pct = 0.04;
        config.token_discount_pct = 0.0;
        config.use_token_for_fee = false;
        config.vip_level = 0;
        config.withdraw_fees["XRP"] = 1.0;
        config.withdraw_fees["BTC"] = 0.001;
        config.min_withdraw["XRP"] = 20.0;
        config.min_withdraw["BTC"] = 0.001;
        configs_[Exchange::Bithumb] = config;

        // Bithumb VIP 등급
        std::vector<VipFeeLevel> vip;
        vip.push_back({1, 0.035, 0.035});
        vip.push_back({2, 0.030, 0.030});
        vip_tables_[Exchange::Bithumb] = vip;
    }

    // Binance
    {
        ExchangeFeeConfig config;
        config.exchange = Exchange::Binance;
        config.maker_fee_pct = 0.10;
        config.taker_fee_pct = 0.10;
        config.token_discount_pct = 25.0;  // BNB 25% 할인
        config.use_token_for_fee = false;
        config.vip_level = 0;
        config.withdraw_fees["XRP"] = 0.25;
        config.withdraw_fees["BTC"] = 0.0002;
        config.min_withdraw["XRP"] = 25.0;
        config.min_withdraw["BTC"] = 0.001;
        configs_[Exchange::Binance] = config;

        // Binance VIP 등급
        std::vector<VipFeeLevel> vip;
        vip.push_back({1, 0.09, 0.10});
        vip.push_back({2, 0.08, 0.10});
        vip.push_back({3, 0.07, 0.09});
        vip.push_back({4, 0.06, 0.08});
        vip.push_back({5, 0.05, 0.07});
        vip_tables_[Exchange::Binance] = vip;
    }

    // MEXC
    {
        ExchangeFeeConfig config;
        config.exchange = Exchange::MEXC;
        config.maker_fee_pct = 0.00;  // Maker 무료
        config.taker_fee_pct = 0.02;
        config.token_discount_pct = 20.0;  // MX 20% 할인
        config.use_token_for_fee = false;
        config.vip_level = 0;
        config.withdraw_fees["XRP"] = 0.25;
        config.withdraw_fees["BTC"] = 0.0003;
        config.min_withdraw["XRP"] = 20.0;
        config.min_withdraw["BTC"] = 0.001;
        configs_[Exchange::MEXC] = config;
    }
}

// =============================================================================
// YAML 설정 로드
// =============================================================================
bool FeeCalculator::load_config(const std::string& config_path) {
#ifdef HAS_YAML_CPP
    auto logger = Logger::default_logger();
    try {
        YAML::Node root = YAML::LoadFile(config_path);

        if (!root["exchanges"]) {
            if (logger) logger->error("fees.yaml: 'exchanges' 섹션 없음");
            return false;
        }

        WriteGuard lock(mutex_);

        for (const auto& pair : root["exchanges"]) {
            std::string name = pair.first.as<std::string>();
            const auto& node = pair.second;

            Exchange ex;
            if (name == "upbit") ex = Exchange::Upbit;
            else if (name == "bithumb") ex = Exchange::Bithumb;
            else if (name == "binance") ex = Exchange::Binance;
            else if (name == "mexc") ex = Exchange::MEXC;
            else continue;

            ExchangeFeeConfig config;
            config.exchange = ex;

            if (node["maker_fee_pct"])
                config.maker_fee_pct = node["maker_fee_pct"].as<double>();
            if (node["taker_fee_pct"])
                config.taker_fee_pct = node["taker_fee_pct"].as<double>();
            if (node["bnb_discount_pct"])
                config.token_discount_pct = node["bnb_discount_pct"].as<double>();
            if (node["mx_discount_pct"])
                config.token_discount_pct = node["mx_discount_pct"].as<double>();

            // 출금 수수료
            if (node["withdraw_fees"]) {
                for (const auto& fee_pair : node["withdraw_fees"]) {
                    std::string coin = fee_pair.first.as<std::string>();
                    double fee = fee_pair.second.as<double>();
                    config.withdraw_fees[coin] = fee;
                }
            }

            // 최소 출금
            if (node["min_withdraw"]) {
                for (const auto& min_pair : node["min_withdraw"]) {
                    std::string coin = min_pair.first.as<std::string>();
                    double min_amt = min_pair.second.as<double>();
                    config.min_withdraw[coin] = min_amt;
                }
            }

            // VIP 등급
            if (node["vip_levels"] && node["vip_levels"].IsSequence()) {
                std::vector<VipFeeLevel> levels;
                for (const auto& vip_node : node["vip_levels"]) {
                    VipFeeLevel level;
                    level.level = vip_node["level"].as<int>();
                    level.maker_fee_pct = vip_node["maker"].as<double>();
                    level.taker_fee_pct = vip_node["taker"].as<double>();
                    levels.push_back(level);
                }
                vip_tables_[ex] = levels;
            }

            configs_[ex] = config;
        }

        if (logger) logger->info("FeeCalculator: 설정 로드 완료 - {}", config_path);
        return true;

    } catch (const std::exception& e) {
        if (logger) logger->error("fees.yaml 로드 실패: {}", e.what());
        return false;
    }
#else
    // yaml-cpp 없으면 기본 설정 사용
    auto logger = Logger::default_logger();
    if (logger) logger->warn("yaml-cpp 미설치: 기본 수수료 설정 사용");
    return false;
#endif
}

// =============================================================================
// 거래소 설정
// =============================================================================
void FeeCalculator::set_exchange_config(const ExchangeFeeConfig& config) {
    WriteGuard lock(mutex_);
    configs_[config.exchange] = config;
}

const ExchangeFeeConfig& FeeCalculator::get_exchange_config(Exchange ex) const {
    ReadGuard lock(mutex_);
    auto it = configs_.find(ex);
    if (it != configs_.end()) {
        return it->second;
    }
    // 없으면 Upbit 기본값 반환 (안전장치)
    static ExchangeFeeConfig default_config;
    return default_config;
}

// =============================================================================
// 수수료율 조회
// =============================================================================
double FeeCalculator::get_fee_rate(Exchange ex, OrderRole role) const {
    ReadGuard lock(mutex_);
    auto it = configs_.find(ex);
    if (it != configs_.end()) {
        return it->second.get_fee_rate(role);
    }
    // 기본값 (constexpr 상수 사용)
    return (role == OrderRole::Maker)
        ? fee::maker_fee(ex)
        : fee::taker_fee(ex);
}

double FeeCalculator::get_fee_rate_pct(Exchange ex, OrderRole role) const {
    return get_fee_rate(ex, role) * 100.0;
}

double FeeCalculator::get_withdraw_fee(Exchange ex, const std::string& coin) const {
    ReadGuard lock(mutex_);
    auto it = configs_.find(ex);
    if (it != configs_.end()) {
        auto fee_it = it->second.withdraw_fees.find(coin);
        if (fee_it != it->second.withdraw_fees.end()) {
            return fee_it->second;
        }
    }
    // XRP 기본값
    if (coin == "XRP") {
        return fee::withdraw_fee_xrp(ex);
    }
    return 0.0;
}

double FeeCalculator::get_min_withdraw(Exchange ex, const std::string& coin) const {
    ReadGuard lock(mutex_);
    auto it = configs_.find(ex);
    if (it != configs_.end()) {
        auto min_it = it->second.min_withdraw.find(coin);
        if (min_it != it->second.min_withdraw.end()) {
            return min_it->second;
        }
    }
    // XRP 기본값
    if (coin == "XRP") {
        return 20.0;
    }
    return 0.0;
}

// =============================================================================
// 거래 비용 계산
// =============================================================================
TradeCost FeeCalculator::calculate_trade_cost(
    Exchange ex,
    OrderRole role,
    double quantity,
    double price,
    double fx_rate
) const {
    TradeCost cost;
    cost.exchange = ex;
    cost.role = role;
    cost.quantity = quantity;
    cost.price = price;

    // 거래 금액 (원 통화)
    cost.notional = quantity * price;

    // KRW 환산 (국내 거래소면 fx_rate = 1.0)
    cost.notional_krw = cost.notional * fx_rate;

    // 수수료율
    cost.fee_rate = get_fee_rate(ex, role);
    cost.fee_rate_pct = cost.fee_rate * 100.0;

    // 수수료 금액
    cost.fee = cost.notional * cost.fee_rate;
    cost.fee_krw = cost.fee * fx_rate;

    // 토큰 지불 여부 확인
    {
        ReadGuard lock(mutex_);
        auto it = configs_.find(ex);
        if (it != configs_.end()) {
            cost.use_token_fee = it->second.use_token_for_fee;
        }
    }

    // 수수료 차감 후 수량
    // 매수: 수량은 그대로, 실효가격 상승
    // 매도: 수량 감소 또는 실효가격 하락
    if (role == OrderRole::Maker || role == OrderRole::Taker) {
        // 일반적으로 거래소는 거래 금액에서 수수료를 차감
        // 매수 시: 실제 받는 코인 = quantity
        // 매도 시: 실제 받는 금액 = notional - fee
        cost.net_quantity = quantity;
        cost.effective_price = price * (1.0 + cost.fee_rate);  // 매수 기준
    }

    return cost;
}

// =============================================================================
// 송금 비용 계산
// =============================================================================
TransferCost FeeCalculator::calculate_transfer_cost(
    const std::string& coin,
    double amount,
    Exchange from,
    Exchange to,
    double coin_price_krw
) const {
    TransferCost cost;
    cost.coin = coin;
    cost.amount = amount;
    cost.from = from;
    cost.to = to;

    // 출금 수수료 (출발 거래소에서 차감)
    cost.withdraw_fee = get_withdraw_fee(from, coin);
    cost.withdraw_fee_krw = cost.withdraw_fee * coin_price_krw;

    // 네트워크 수수료 (XRP는 거의 없음)
    cost.network_fee = 0.0;

    // 실수령 수량
    cost.net_amount = amount - cost.withdraw_fee;
    if (cost.net_amount < 0) {
        cost.net_amount = 0;
    }

    // 총 비용
    cost.total_cost_krw = cost.withdraw_fee_krw;

    return cost;
}

// =============================================================================
// 아비트라지 총 비용 계산
// =============================================================================
ArbitrageCost FeeCalculator::calculate_arbitrage_cost(
    Exchange buy_ex,
    Exchange sell_ex,
    double quantity,
    double buy_price,
    double sell_price,
    double fx_rate,
    OrderRole buy_role,
    OrderRole sell_role
) const {
    ArbitrageCost result;
    result.quantity = quantity;

    // 매수 비용 (해외 거래소)
    result.buy_cost = calculate_trade_cost(
        buy_ex, buy_role, quantity, buy_price, fx_rate);

    // 매도 비용 (국내 거래소, fx_rate = 1.0)
    result.sell_cost = calculate_trade_cost(
        sell_ex, sell_role, quantity, sell_price, 1.0);

    // 송금 비용 (해외 → 국내)
    result.transfer_cost = calculate_transfer_cost(
        "XRP", quantity, buy_ex, sell_ex, sell_price);

    // 가격 (KRW 기준)
    result.buy_price_krw = buy_price * fx_rate;
    result.sell_price_krw = sell_price;

    // 세전 프리미엄
    result.gross_premium = (sell_price - buy_price * fx_rate) * quantity;
    if (result.buy_price_krw > 0) {
        result.gross_premium_pct = (result.sell_price_krw - result.buy_price_krw)
                                   / result.buy_price_krw * 100.0;
    }

    // 총 수수료
    result.total_fee_krw = result.buy_cost.fee_krw
                         + result.sell_cost.fee_krw
                         + result.transfer_cost.total_cost_krw;

    // 수수료율 (매수 금액 기준)
    if (result.buy_cost.notional_krw > 0) {
        result.total_fee_pct = result.total_fee_krw / result.buy_cost.notional_krw * 100.0;
    }

    // 순수익
    result.net_profit_krw = result.gross_premium - result.total_fee_krw;
    if (result.buy_cost.notional_krw > 0) {
        result.net_profit_pct = result.net_profit_krw / result.buy_cost.notional_krw * 100.0;
    }

    return result;
}

// =============================================================================
// 손익분기 프리미엄 계산
// =============================================================================
double FeeCalculator::calculate_breakeven_premium(
    Exchange buy_ex,
    Exchange sell_ex,
    OrderRole buy_role,
    OrderRole sell_role
) const {
    // 매수 수수료율
    double buy_fee = get_fee_rate(buy_ex, buy_role) * 100.0;  // %

    // 매도 수수료율
    double sell_fee = get_fee_rate(sell_ex, sell_role) * 100.0;  // %

    // 출금 수수료 (XRP 기준, 약 0.025% @ 1000 XRP)
    // 간단히 고정값으로 계산
    double withdraw_fee_pct = 0.025;  // 0.025%

    // 손익분기 = 매수 수수료 + 매도 수수료 + 출금 수수료
    return buy_fee + sell_fee + withdraw_fee_pct;
}

// =============================================================================
// 설정 변경
// =============================================================================
void FeeCalculator::set_vip_level(Exchange ex, int level) {
    WriteGuard lock(mutex_);

    auto it = configs_.find(ex);
    if (it == configs_.end()) return;

    it->second.vip_level = level;

    // VIP 수수료 적용
    auto vip_it = vip_tables_.find(ex);
    if (vip_it != vip_tables_.end()) {
        for (const auto& vip : vip_it->second) {
            if (vip.level == level) {
                it->second.maker_fee_pct = vip.maker_fee_pct;
                it->second.taker_fee_pct = vip.taker_fee_pct;
                break;
            }
        }
    }
}

void FeeCalculator::set_token_discount(Exchange ex, bool enabled) {
    WriteGuard lock(mutex_);
    auto it = configs_.find(ex);
    if (it != configs_.end()) {
        it->second.use_token_for_fee = enabled;
    }
}

void FeeCalculator::update_withdraw_fee(Exchange ex, const std::string& coin, double fee) {
    WriteGuard lock(mutex_);
    auto it = configs_.find(ex);
    if (it != configs_.end()) {
        it->second.withdraw_fees[coin] = fee;
    }
}

// =============================================================================
// VIP 수수료 적용
// =============================================================================
void FeeCalculator::apply_vip_fees(ExchangeFeeConfig& config) const {
    auto vip_it = vip_tables_.find(config.exchange);
    if (vip_it == vip_tables_.end()) return;

    for (const auto& vip : vip_it->second) {
        if (vip.level == config.vip_level) {
            config.maker_fee_pct = vip.maker_fee_pct;
            config.taker_fee_pct = vip.taker_fee_pct;
            break;
        }
    }
}

// =============================================================================
// 요약 출력
// =============================================================================
void FeeCalculator::print_summary() const {
    ReadGuard lock(mutex_);

    std::cout << "\n========== Fee Calculator Summary ==========\n";

    for (int i = 0; i < static_cast<int>(Exchange::Count); ++i) {
        auto ex = static_cast<Exchange>(i);
        auto it = configs_.find(ex);
        if (it == configs_.end()) continue;

        const auto& cfg = it->second;
        std::cout << "\n[" << exchange_name(ex) << "]\n";
        std::cout << "  Maker: " << std::fixed << std::setprecision(3)
                  << cfg.maker_fee_pct << "%\n";
        std::cout << "  Taker: " << cfg.taker_fee_pct << "%\n";
        std::cout << "  VIP Level: " << cfg.vip_level << "\n";

        if (cfg.token_discount_pct > 0) {
            std::cout << "  Token Discount: " << cfg.token_discount_pct << "%";
            if (cfg.use_token_for_fee) {
                std::cout << " (ENABLED)";
            }
            std::cout << "\n";
        }

        std::cout << "  Withdraw XRP: " << get_withdraw_fee(ex, "XRP") << " XRP\n";
    }

    std::cout << "\n=============================================\n";
}

// =============================================================================
// 유효성 검사
// =============================================================================
bool FeeCalculator::validate() const {
    ReadGuard lock(mutex_);
    auto logger = Logger::default_logger();

    for (int i = 0; i < static_cast<int>(Exchange::Count); ++i) {
        auto ex = static_cast<Exchange>(i);
        auto it = configs_.find(ex);

        if (it == configs_.end()) {
            if (logger) logger->error("FeeCalculator: 거래소 설정 없음 - {}",
                                      exchange_name(ex));
            return false;
        }

        const auto& cfg = it->second;

        // 수수료 범위 검사 (0 ~ 1%)
        if (cfg.maker_fee_pct < 0 || cfg.maker_fee_pct > 1.0) {
            if (logger) logger->error("FeeCalculator: 잘못된 Maker 수수료 - {}",
                                      exchange_name(ex));
            return false;
        }
        if (cfg.taker_fee_pct < 0 || cfg.taker_fee_pct > 1.0) {
            if (logger) logger->error("FeeCalculator: 잘못된 Taker 수수료 - {}",
                                      exchange_name(ex));
            return false;
        }
    }

    return true;
}

}  // namespace arbitrage
