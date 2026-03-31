#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <istream>

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

// 전략 설정 (기본 설정 파일용 - 상세 설정은 decision_engine.hpp의 StrategyConfig)
struct BasicStrategyConfig {
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

// 알림 설정 (기본 설정 파일용 - 상세 설정은 alert.hpp의 AlertConfig)
struct BasicAlertConfig {
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

    // stdin 등 스트림에서 로드 (SOPS 파이프라인용)
    bool load_from_stream(std::istream& input);

    // 설정 파일 리로드
    bool reload();
    
    // Getter
    const ExchangeConfig& exchange(Exchange ex) const;
    const BasicStrategyConfig& strategy() const { return strategy_; }
    const RiskConfig& risk() const { return risk_; }
    const ServerConfig& server() const { return server_; }
    const BasicAlertConfig& alert() const { return alert_; }
    
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

    // YAML 파싱 (config.cpp에서 구현, HAS_YAML_CPP일 때만 사용)
    bool parse_yaml_string(const std::string& yaml_content);

    mutable RWSpinLock mutex_;
    std::string config_path_;
    std::string cached_yaml_;  // stdin 재로드용 캐시
    
    std::map<Exchange, ExchangeConfig> exchanges_;
    BasicStrategyConfig strategy_;
    RiskConfig risk_;
    ServerConfig server_;
    BasicAlertConfig alert_;
    
    // 심볼 목록
    std::vector<SymbolConfig> symbols_;          // 전체 심볼
    std::vector<SymbolConfig> primary_symbols_;   // 주요 거래 심볼
    std::vector<SymbolConfig> secondary_symbols_; // 보조 모니터링 심볼
};

}  // namespace arbitrage