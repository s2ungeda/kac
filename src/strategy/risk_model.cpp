/**
 * Risk Model Implementation (TASK_12)
 */

#include "arbitrage/strategy/risk_model.hpp"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
RiskModel& risk_model() {
    static RiskModel instance;
    return instance;
}

// =============================================================================
// 생성자
// =============================================================================
RiskModel::RiskModel(const RiskModelConfig& config)
    : config_(config)
{
}

// =============================================================================
// 종합 리스크 평가
// =============================================================================
RiskAssessment RiskModel::evaluate(
    const PremiumInfo& opportunity,
    double order_qty,
    std::chrono::seconds estimated_transfer_time
) {
    RiskAssessment result;
    ++stats_.evaluations;

    if (!opportunity.is_valid() || order_qty <= 0) {
        result.score = 100.0;
        result.level = RiskLevel::Critical;
        result.add_warning(RiskWarning::NegativeExpectedProfit);
        ++stats_.rejected;
        return result;
    }

    // 개별 리스크 계산
    result.transfer_risk = calculate_transfer_risk(
        opportunity.buy_exchange,
        opportunity.sell_exchange,
        estimated_transfer_time
    );

    result.market_risk = calculate_market_risk(opportunity);

    // 슬리피지 리스크 (기본값, 오더북 없음)
    result.slippage_risk = 30.0;  // 중간 수준 가정

    // 유동성 리스크 (기본값, 오더북 없음)
    result.liquidity_risk = 30.0;  // 중간 수준 가정

    // 종합 점수 계산 (가중 평균)
    result.score =
        config_.weight_transfer * result.transfer_risk +
        config_.weight_market * result.market_risk +
        config_.weight_liquidity * result.liquidity_risk +
        config_.weight_slippage * result.slippage_risk;

    result.update_level();

    // 예상 수익 계산
    double position_krw = order_qty * opportunity.sell_price;

    // 수수료 차감 (fee_calc 있으면 사용)
    double total_fee_pct = 0.15;  // 기본값 0.15%
    if (fee_calc_) {
        ArbitrageCost cost = fee_calc_->calculate_arbitrage_cost(
            opportunity.buy_exchange,
            opportunity.sell_exchange,
            order_qty,
            opportunity.buy_price / opportunity.fx_rate,  // USDT 가격
            opportunity.sell_price,
            opportunity.fx_rate,
            OrderRole::Maker,
            OrderRole::Taker
        );
        total_fee_pct = cost.total_fee_pct;
    }

    result.expected_profit_pct = opportunity.premium_pct - total_fee_pct;
    result.expected_profit_krw = position_krw * result.expected_profit_pct / 100.0;

    // 최대 손실 계산 (김프 역전 시나리오)
    PremiumStats stats = get_premium_stats(
        opportunity.buy_exchange, opportunity.sell_exchange);

    double worst_case_premium = stats.mean - 2.0 * stats.std_dev;
    if (worst_case_premium > opportunity.premium_pct) {
        worst_case_premium = opportunity.premium_pct * 0.3;  // 최소 70% 감소 가정
    }
    result.max_loss_pct = worst_case_premium - total_fee_pct;
    if (result.max_loss_pct > 0) result.max_loss_pct = 0;
    result.max_loss_krw = position_krw * result.max_loss_pct / 100.0;

    // VaR 계산
    int transfer_min = static_cast<int>(estimated_transfer_time.count() / 60) + 1;
    result.var_95_krw = calculate_var(position_krw, 0.95, transfer_min);
    result.var_99_krw = calculate_var(position_krw, 0.99, transfer_min);
    result.var_95_pct = (position_krw > 0) ? (result.var_95_krw / position_krw * 100.0) : 0;

    // 확률 추정
    if (stats.std_dev > 0) {
        // 현재 김프가 평균 대비 얼마나 높은지
        double z = calc_z_score(opportunity.premium_pct, stats.mean, stats.std_dev);
        // Z > 0이면 평균보다 높음 (좋은 기회)
        // 수익 확률 = 김프가 수수료 이상 유지될 확률
        double break_even_z = calc_z_score(total_fee_pct, stats.mean, stats.std_dev);
        result.profit_probability = 0.5 + 0.5 * std::erf((z - break_even_z) / std::sqrt(2.0));
    } else {
        result.profit_probability = opportunity.premium_pct > total_fee_pct ? 0.7 : 0.3;
    }

    result.full_fill_probability = 0.9;  // 오더북 없이는 기본값

    // 경고 생성
    if (result.expected_profit_pct < config_.min_expected_profit_pct) {
        result.add_warning(RiskWarning::LowPremium);
    }
    if (result.expected_profit_pct < 0) {
        result.add_warning(RiskWarning::NegativeExpectedProfit);
    }
    if (result.transfer_risk > 70) {
        result.add_warning(RiskWarning::LongTransferTime);
    }
    if (result.market_risk > 70) {
        result.add_warning(RiskWarning::HighVolatility);
    }
    if (result.var_95_pct > config_.max_var_95_pct) {
        result.add_warning(RiskWarning::MarginTooThin);
    }

    // 최종 결정
    result.is_acceptable = (result.score <= config_.max_acceptable_score);
    result.should_execute = result.is_acceptable &&
                            result.expected_profit_pct >= config_.min_expected_profit_pct &&
                            result.profit_probability >= 0.5;

    if (result.is_acceptable) {
        ++stats_.accepted;
    } else {
        ++stats_.rejected;
    }

    return result;
}

// =============================================================================
// 오더북 기반 리스크 평가
// =============================================================================
RiskAssessment RiskModel::evaluate_with_orderbook(
    const PremiumInfo& opportunity,
    const OrderBook& buy_ob,
    const OrderBook& sell_ob,
    double order_qty,
    std::chrono::seconds estimated_transfer_time
) {
    // 기본 평가 수행
    RiskAssessment result = evaluate(opportunity, order_qty, estimated_transfer_time);

    // 슬리피지 모델로 정밀 계산
    if (slippage_model_) {
        // 매수 슬리피지 (ask 쪽)
        SlippageEstimate buy_slip = slippage_model_->estimate_taker_slippage(
            buy_ob, OrderSide::Buy, order_qty);

        // 매도 슬리피지 (bid 쪽)
        SlippageEstimate sell_slip = slippage_model_->estimate_taker_slippage(
            sell_ob, OrderSide::Sell, order_qty);

        // 슬리피지 리스크 업데이트
        double avg_slippage_bps = (buy_slip.slippage_bps + sell_slip.slippage_bps) / 2.0;
        result.slippage_risk = calculate_slippage_risk(buy_slip);

        // 체결 확률 업데이트
        result.full_fill_probability = std::min(buy_slip.fill_ratio, sell_slip.fill_ratio);

        // 부분 체결 경고
        if (result.full_fill_probability < 0.95) {
            result.add_warning(RiskWarning::PartialFillRisk);
        }

        // 슬리피지 경고
        if (avg_slippage_bps > config_.max_slippage_bps) {
            result.add_warning(RiskWarning::HighSlippage);
        }

        // 유동성 계산
        double buy_depth = 0, sell_depth = 0;
        for (int i = 0; i < buy_ob.ask_count && i < 10; ++i) {
            buy_depth += buy_ob.asks[i].price * buy_ob.asks[i].quantity;
        }
        for (int i = 0; i < sell_ob.bid_count && i < 10; ++i) {
            sell_depth += sell_ob.bids[i].price * sell_ob.bids[i].quantity;
        }
        double min_depth = std::min(buy_depth, sell_depth);
        double position_krw = order_qty * opportunity.sell_price;

        // 유동성 리스크
        if (min_depth < position_krw * 3) {
            result.liquidity_risk = 80.0;  // 유동성 부족
            result.add_warning(RiskWarning::LowLiquidity);
        } else if (min_depth < position_krw * 10) {
            result.liquidity_risk = 50.0;  // 유동성 주의
        } else {
            result.liquidity_risk = 20.0;  // 유동성 충분
        }

        // 점수 재계산
        result.score =
            config_.weight_transfer * result.transfer_risk +
            config_.weight_market * result.market_risk +
            config_.weight_liquidity * result.liquidity_risk +
            config_.weight_slippage * result.slippage_risk;

        result.update_level();

        // 결정 재계산
        result.is_acceptable = (result.score <= config_.max_acceptable_score);
        result.should_execute = result.is_acceptable &&
                                result.expected_profit_pct >= config_.min_expected_profit_pct &&
                                result.profit_probability >= 0.5 &&
                                result.full_fill_probability >= 0.9;
    }

    return result;
}

// =============================================================================
// 송금 리스크 계산
// =============================================================================
double RiskModel::calculate_transfer_risk(
    Exchange from,
    Exchange to,
    std::chrono::seconds transfer_time
) const {
    // 기본 리스크: 시간에 비례
    double time_sec = static_cast<double>(transfer_time.count());
    double time_risk = (time_sec / 60.0) * config_.transfer_risk_per_min;

    // 최대 100으로 제한
    time_risk = std::min(time_risk, 100.0);

    // 거래소별 추가 리스크
    double exchange_risk = 0;

    // 해외 거래소에서 출금 시 추가 리스크
    if (!is_krw_exchange(from)) {
        exchange_risk += 10.0;
    }

    // 과거 통계 기반 조정
    TransferTimeStats stats = get_transfer_stats(from, to);
    if (stats.sample_count >= 10) {
        // 예상 시간이 P95를 초과하면 추가 리스크
        if (time_sec > stats.p95_seconds) {
            exchange_risk += 20.0;
        }
    }

    return std::min(time_risk + exchange_risk, 100.0);
}

// =============================================================================
// 시장 리스크 계산
// =============================================================================
double RiskModel::calculate_market_risk(const PremiumInfo& opportunity) const {
    PremiumStats stats = get_premium_stats(
        opportunity.buy_exchange, opportunity.sell_exchange);

    if (stats.sample_count < 10) {
        // 데이터 부족 시 중간 리스크
        return 50.0;
    }

    // 변동성 기반 리스크
    double volatility_risk = (stats.std_dev / config_.high_volatility_threshold) * 50.0;
    volatility_risk = std::min(volatility_risk, 50.0);

    // Z-점수 기반 리스크 (평균에서 멀수록 회귀 위험)
    double z = std::abs(stats.z_score);
    double mean_reversion_risk = 0;
    if (z > 2.0) {
        mean_reversion_risk = (z - 2.0) * 15.0;  // Z > 2이면 회귀 위험
    }

    return std::min(volatility_risk + mean_reversion_risk, 100.0);
}

// =============================================================================
// 슬리피지 리스크 계산
// =============================================================================
double RiskModel::calculate_slippage_risk(const SlippageEstimate& estimate) const {
    if (!estimate.is_valid()) {
        return 50.0;  // 데이터 없음
    }

    // 슬리피지 bps 기반 리스크
    double slip_risk = (estimate.slippage_bps / config_.max_slippage_bps) * 50.0;

    // 체결 비율 기반 리스크
    double fill_risk = (1.0 - estimate.fill_ratio) * 50.0;

    return std::min(slip_risk + fill_risk, 100.0);
}

// =============================================================================
// 김프 데이터 기록
// =============================================================================
void RiskModel::record_premium(Exchange buy_ex, Exchange sell_ex, double premium_pct) {
    std::unique_lock lock(mutex_);

    int key = premium_key(buy_ex, sell_ex);
    auto& history = premium_history_[key];

    history.push_back(premium_pct);

    // 크기 제한
    while (history.size() > config_.premium_history_size) {
        history.pop_front();
    }
}

// =============================================================================
// 김프 변동성 통계
// =============================================================================
PremiumStats RiskModel::get_premium_stats(Exchange buy_ex, Exchange sell_ex) const {
    std::shared_lock lock(mutex_);

    PremiumStats result;
    int key = premium_key(buy_ex, sell_ex);
    const auto& history = premium_history_[key];

    result.sample_count = history.size();
    if (result.sample_count == 0) {
        return result;
    }

    // 평균
    result.mean = std::accumulate(history.begin(), history.end(), 0.0) / history.size();

    // 표준편차
    result.std_dev = calc_std_dev(history, result.mean);

    // 최소/최대
    auto [min_it, max_it] = std::minmax_element(history.begin(), history.end());
    result.min = *min_it;
    result.max = *max_it;

    // 현재값
    result.current = history.back();

    // Z-점수
    result.z_score = calc_z_score(result.current, result.mean, result.std_dev);

    return result;
}

// =============================================================================
// 전체 김프 변동성
// =============================================================================
double RiskModel::calculate_overall_volatility() const {
    std::shared_lock lock(mutex_);

    double total_vol = 0;
    int count = 0;

    for (int i = 0; i < 16; ++i) {
        const auto& history = premium_history_[i];
        if (history.size() >= 10) {
            double mean = std::accumulate(history.begin(), history.end(), 0.0) / history.size();
            double std_dev = calc_std_dev(history, mean);
            total_vol += std_dev;
            ++count;
        }
    }

    return count > 0 ? total_vol / count : 0.0;
}

// =============================================================================
// 송금 시간 기록
// =============================================================================
void RiskModel::record_transfer_time(
    Exchange from,
    Exchange to,
    std::chrono::seconds elapsed
) {
    std::unique_lock lock(mutex_);

    int key = static_cast<int>(from) * 4 + static_cast<int>(to);
    auto& history = transfer_time_history_[key];

    history.push_back(static_cast<double>(elapsed.count()));

    // 크기 제한 (최근 100개)
    while (history.size() > 100) {
        history.pop_front();
    }
}

// =============================================================================
// 송금 시간 통계
// =============================================================================
TransferTimeStats RiskModel::get_transfer_stats(Exchange from, Exchange to) const {
    std::shared_lock lock(mutex_);

    TransferTimeStats result;
    result.from = from;
    result.to = to;

    int key = static_cast<int>(from) * 4 + static_cast<int>(to);
    const auto& history = transfer_time_history_[key];

    result.sample_count = history.size();
    if (result.sample_count == 0) {
        // 기본값 반환
        result.avg_seconds = TransferTimeStats::DEFAULT_AVG_SEC;
        result.p95_seconds = TransferTimeStats::DEFAULT_P95_SEC;
        result.p99_seconds = TransferTimeStats::DEFAULT_P99_SEC;
        return result;
    }

    // 평균
    result.avg_seconds = std::accumulate(history.begin(), history.end(), 0.0) / history.size();

    // 표준편차
    result.std_dev = calc_std_dev(history, result.avg_seconds);

    // 퍼센타일 계산 (정렬 필요)
    std::vector<double> sorted(history.begin(), history.end());
    std::sort(sorted.begin(), sorted.end());

    size_t p95_idx = static_cast<size_t>(sorted.size() * 0.95);
    size_t p99_idx = static_cast<size_t>(sorted.size() * 0.99);

    result.p95_seconds = sorted[std::min(p95_idx, sorted.size() - 1)];
    result.p99_seconds = sorted[std::min(p99_idx, sorted.size() - 1)];

    return result;
}

// =============================================================================
// VaR 계산
// =============================================================================
double RiskModel::calculate_var(
    double position_krw,
    double confidence,
    int time_horizon_min
) const {
    // 전체 변동성 가져오기
    double volatility = calculate_overall_volatility();
    if (volatility <= 0) {
        volatility = 0.5;  // 기본 변동성 0.5%
    }

    // 시간 조정 (루트 스케일링)
    double time_factor = std::sqrt(static_cast<double>(time_horizon_min) / 5.0);

    // 정규분포 역함수로 Z값 계산
    double z = inverse_normal_cdf(confidence);

    // VaR = Position × Volatility × Z × Time_Factor
    double var = position_krw * (volatility / 100.0) * z * time_factor;

    return std::abs(var);
}

// =============================================================================
// 설정
// =============================================================================
void RiskModel::set_config(const RiskModelConfig& config) {
    std::unique_lock lock(mutex_);
    config_ = config;
}

// =============================================================================
// 리스크 평가 출력
// =============================================================================
void RiskModel::print_assessment(const RiskAssessment& assessment) const {
    std::cout << "\n========== Risk Assessment ==========\n";
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "Score: " << assessment.score << "/100 ("
              << risk_level_name(assessment.level) << ")\n\n";

    std::cout << "[Individual Risks]\n";
    std::cout << "  Transfer:  " << assessment.transfer_risk << "\n";
    std::cout << "  Market:    " << assessment.market_risk << "\n";
    std::cout << "  Liquidity: " << assessment.liquidity_risk << "\n";
    std::cout << "  Slippage:  " << assessment.slippage_risk << "\n\n";

    std::cout << "[Profit/Loss]\n";
    std::cout << "  Expected Profit: " << assessment.expected_profit_krw << " KRW ("
              << assessment.expected_profit_pct << "%)\n";
    std::cout << "  Max Loss:        " << assessment.max_loss_krw << " KRW ("
              << assessment.max_loss_pct << "%)\n";
    std::cout << "  VaR 95%:         " << assessment.var_95_krw << " KRW ("
              << assessment.var_95_pct << "%)\n\n";

    std::cout << "[Probability]\n";
    std::cout << "  Profit:    " << (assessment.profit_probability * 100) << "%\n";
    std::cout << "  Full Fill: " << (assessment.full_fill_probability * 100) << "%\n\n";

    std::cout << "[Decision]\n";
    std::cout << "  Acceptable: " << (assessment.is_acceptable ? "YES" : "NO") << "\n";
    std::cout << "  Execute:    " << (assessment.should_execute ? "YES" : "NO") << "\n";

    if (assessment.warning_count > 0) {
        std::cout << "\n[Warnings]\n";
        for (int i = 0; i < assessment.warning_count; ++i) {
            std::cout << "  - " << risk_warning_name(assessment.warnings[i]) << "\n";
        }
    }

    std::cout << "======================================\n";
}

// =============================================================================
// 유틸리티: 표준편차
// =============================================================================
double RiskModel::calc_std_dev(const std::deque<double>& data, double mean) {
    if (data.size() < 2) return 0.0;

    double sum_sq = 0;
    for (double v : data) {
        double diff = v - mean;
        sum_sq += diff * diff;
    }
    return std::sqrt(sum_sq / (data.size() - 1));
}

// =============================================================================
// 유틸리티: 정규분포 역함수 (근사)
// =============================================================================
double RiskModel::inverse_normal_cdf(double p) {
    // Abramowitz and Stegun approximation
    if (p <= 0.0) return -10.0;
    if (p >= 1.0) return 10.0;

    if (p < 0.5) {
        return -inverse_normal_cdf(1.0 - p);
    }

    // 0.5 <= p < 1.0
    double t = std::sqrt(-2.0 * std::log(1.0 - p));
    double c0 = 2.515517;
    double c1 = 0.802853;
    double c2 = 0.010328;
    double d1 = 1.432788;
    double d2 = 0.189269;
    double d3 = 0.001308;

    return t - (c0 + c1 * t + c2 * t * t) /
               (1.0 + d1 * t + d2 * t * t + d3 * t * t * t);
}

}  // namespace arbitrage
