#pragma once

#include "arbitrage/common/types.hpp"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>

namespace arbitrage {

// 심볼(코인) 설정
struct SymbolConfig {
    std::string symbol;      // 공통 심볼 (예: XRP, BTC)
    std::string upbit;       // 업비트 심볼 (예: KRW-XRP)
    std::string bithumb;     // 빗썸 심볼 (예: XRP_KRW)
    std::string binance;     // 바이낸스 심볼 (예: XRPUSDT)
    std::string mexc;        // MEXC 심볼 (예: XRPUSDT)
    bool enabled{true};      // 활성화 여부
};

// 거래소별 설정
struct ExchangeConfig {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;  // 일부 거래소용
    
    std::string ws_url;
    std::string rest_url;
    
    int rate_limit_per_second{10};
    int rate_limit_per_minute{600};
    
    bool enabled{false};
};

// 전략 설정
struct StrategyConfig {
    double min_premium_pct{3.0};      // 최소 진입 김프
    double max_premium_pct{10.0};     // 최대 진입 김프
    double stop_loss_pct{1.0};        // 손절 기준
    
    double min_order_qty{10.0};       // 최소 주문량 (XRP)
    double max_order_qty{10000.0};    // 최대 주문량
    double max_position{50000.0};     // 최대 포지션
    
    double max_slippage_pct{0.5};     // 최대 슬리피지
    int order_timeout_ms{5000};       // 주문 타임아웃
};

// 리스크 설정
struct RiskConfig {
    double daily_loss_limit_krw{500000.0};  // 일일 손실 한도
    double max_transfer_amount{50000.0};     // 최대 송금량
    int max_concurrent_orders{4};            // 동시 주문 수
    bool kill_switch_enabled{true};          // 킬스위치 활성화
};

// TCP 서버 설정
struct ServerConfig {
    std::string bind_address{"0.0.0.0"};
    int port{9800};
    int max_connections{10};
    std::string auth_token;
};

// 알림 설정
struct AlertConfig {
    bool telegram_enabled{false};
    std::string telegram_token;
    std::string telegram_chat_id;
    
    bool discord_enabled{false};
    std::string discord_webhook;
};

// 전체 설정
class Config {
public:
    // 싱글톤 접근
    static Config& instance();
    
    // 설정 파일 로드
    bool load(const std::string& path);
    
    // 설정 파일 리로드
    bool reload();
    
    // Getter
    const ExchangeConfig& exchange(Exchange ex) const;
    const StrategyConfig& strategy() const { return strategy_; }
    const RiskConfig& risk() const { return risk_; }
    const ServerConfig& server() const { return server_; }
    const AlertConfig& alert() const { return alert_; }
    
    // 심볼 관련
    const std::vector<SymbolConfig>& symbols() const { return symbols_; }
    const std::vector<SymbolConfig>& primary_symbols() const { return primary_symbols_; }
    const std::vector<SymbolConfig>& secondary_symbols() const { return secondary_symbols_; }
    
    // 특정 거래소의 심볼 목록 가져오기
    std::vector<std::string> get_symbols_for_exchange(Exchange ex) const;
    
    // 설정 파일 경로
    const std::string& config_path() const { return config_path_; }
    
private:
    Config() = default;
    
    mutable std::mutex mutex_;
    std::string config_path_;
    
    std::map<Exchange, ExchangeConfig> exchanges_;
    StrategyConfig strategy_;
    RiskConfig risk_;
    ServerConfig server_;
    AlertConfig alert_;
    
    // 심볼 목록
    std::vector<SymbolConfig> symbols_;          // 전체 심볼
    std::vector<SymbolConfig> primary_symbols_;   // 주요 거래 심볼
    std::vector<SymbolConfig> secondary_symbols_; // 보조 모니터링 심볼
};

}  // namespace arbitrage