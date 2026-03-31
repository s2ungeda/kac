#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/account_manager.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iterator>
#include <cstring>

#ifdef HAS_YAML_CPP
#include <yaml-cpp/yaml.h>
#endif

namespace arbitrage {

namespace { Config* g_set_config_instance_override = nullptr; }
Config& Config::instance() {
    if (g_set_config_instance_override) return *g_set_config_instance_override;
    static Config instance;
    return instance;
}
void set_config_instance(Config* p) { g_set_config_instance_override = p; }

// =============================================================================
// YAML 문자열 파싱 (load / load_from_stream / reload 공용)
// =============================================================================
bool Config::parse_yaml_string(const std::string& yaml_content) {
#ifdef HAS_YAML_CPP
    try {
        YAML::Node root = YAML::Load(yaml_content);

        // 기존 데이터 클리어
        symbols_.clear();
        primary_symbols_.clear();
        secondary_symbols_.clear();
        exchanges_.clear();

        // 심볼 설정
        if (root["symbols"]) {
            auto symbols = root["symbols"];

            if (symbols["primary"]) {
                for (const auto& sym : symbols["primary"]) {
                    SymbolConfig cfg;
                    cfg.symbol = sym["symbol"].as<std::string>("");
                    cfg.upbit = sym["upbit"].as<std::string>("");
                    cfg.bithumb = sym["bithumb"].as<std::string>("");
                    cfg.binance = sym["binance"].as<std::string>("");
                    cfg.mexc = sym["mexc"].as<std::string>("");
                    cfg.enabled = sym["enabled"].as<bool>(true);

                    if (cfg.enabled) {
                        primary_symbols_.push_back(cfg);
                        symbols_.push_back(cfg);
                    }
                }
            }

            if (symbols["secondary"]) {
                for (const auto& sym : symbols["secondary"]) {
                    SymbolConfig cfg;
                    cfg.symbol = sym["symbol"].as<std::string>("");
                    cfg.upbit = sym["upbit"].as<std::string>("");
                    cfg.bithumb = sym["bithumb"].as<std::string>("");
                    cfg.binance = sym["binance"].as<std::string>("");
                    cfg.mexc = sym["mexc"].as<std::string>("");
                    cfg.enabled = sym["enabled"].as<bool>(false);

                    if (cfg.enabled) {
                        secondary_symbols_.push_back(cfg);
                        symbols_.push_back(cfg);
                    }
                }
            }
        }

        // 거래소 설정
        if (root["exchanges"]) {
            auto exchanges = root["exchanges"];

            auto load_exchange = [&](const std::string& name, Exchange ex) {
                if (!exchanges[name]) return;
                auto node = exchanges[name];

                ExchangeConfig cfg;
                cfg.enabled = node["enabled"].as<bool>(false);
                cfg.ws_url = node["ws_url"].as<std::string>("");
                cfg.rest_url = node["rest_url"].as<std::string>("");
                cfg.rate_limit_per_second = node["rate_limit_per_second"].as<int>(10);

                // 다중 계정 모드: exchanges.<name>.accounts[] 배열
                if (node["accounts"] && node["accounts"].IsSequence()) {
                    auto logger = Logger::get("config");
                    int account_count = 0;

                    for (const auto& acct_node : node["accounts"]) {
                        Account acct;
                        acct.id = acct_node["id"].as<std::string>("");
                        acct.exchange = ex;
                        acct.label = acct_node["label"].as<std::string>("");
                        acct.weight = acct_node["weight"].as<double>(1.0);
                        acct.enabled = acct_node["enabled"].as<bool>(true);

                        acct.api_key_ref = acct_node["api_key"].as<std::string>("");
                        acct.api_secret_ref = acct_node["api_secret"].as<std::string>("");

                        if (!acct.id.empty()) {
                            account_manager().add_account(acct);
                            ++account_count;
                        }
                    }

                    // 첫 번째 계정의 키를 ExchangeConfig에도 설정 (호환성)
                    if (node["accounts"].size() > 0) {
                        auto first = node["accounts"][0];
                        cfg.api_key = first["api_key"].as<std::string>("");
                        cfg.api_secret = first["api_secret"].as<std::string>("");
                        cfg.passphrase = first["passphrase"].as<std::string>("");
                    }

                    if (logger) {
                        logger->info("[AUDIT] Loaded {} accounts for {}", account_count, name);
                    }
                }
                // 단일 계정 모드 (기존 형식)
                else {
                    cfg.api_key = node["api_key"].as<std::string>("");
                    cfg.api_secret = node["api_secret"].as<std::string>("");
                    cfg.passphrase = node["passphrase"].as<std::string>("");
                }

                exchanges_[ex] = cfg;
            };

            load_exchange("upbit", Exchange::Upbit);
            load_exchange("bithumb", Exchange::Bithumb);
            load_exchange("binance", Exchange::Binance);
            load_exchange("mexc", Exchange::MEXC);
        }

        // 전략 설정
        if (root["strategy"]) {
            auto s = root["strategy"];
            strategy_.min_premium_pct = s["min_premium_pct"].as<double>(3.0);
            strategy_.max_premium_pct = s["max_premium_pct"].as<double>(10.0);
            strategy_.min_order_qty = s["min_order_qty"].as<double>(10.0);
            strategy_.max_order_qty = s["max_order_qty"].as<double>(10000.0);
        }

        // 리스크 설정
        if (root["risk"]) {
            auto r = root["risk"];
            risk_.daily_loss_limit_krw = r["daily_loss_limit_krw"].as<double>(500000.0);
            risk_.kill_switch_enabled = r["kill_switch_enabled"].as<bool>(true);
        }

        // 서버 설정
        if (root["server"]) {
            auto srv = root["server"];
            server_.bind_address = srv["bind_address"].as<std::string>("0.0.0.0");
            server_.port = srv["port"].as<int>(9800);
            server_.auth_token = srv["auth_token"].as<std::string>("");
        }

        // 알림 설정
        if (root["alert"]) {
            auto a = root["alert"];
            alert_.telegram_enabled = a["telegram_enabled"].as<bool>(false);
            alert_.telegram_token = a["telegram_token"].as<std::string>("");
            alert_.telegram_chat_id = a["telegram_chat_id"].as<std::string>("");
            alert_.discord_enabled = a["discord_enabled"].as<bool>(false);
            alert_.discord_webhook = a["discord_webhook"].as<std::string>("");
        }

        return true;

    } catch (const std::exception& e) {
        auto logger = Logger::get("config");
        if (logger) {
            logger->error("Config parse error: {}", e.what());
        }
        return false;
    }
#else
    (void)yaml_content;
    std::cerr << "[WARNING] yaml-cpp not found, YAML parsing not supported\n";
    return false;
#endif
}

// =============================================================================
// 파일에서 로드
// =============================================================================
bool Config::load(const std::string& path) {
    WriteGuard lock(mutex_);
    config_path_ = path;
    cached_yaml_.clear();

#ifdef HAS_YAML_CPP
    try {
        // 파일을 문자열로 읽어서 parse_yaml_string에 위임
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            auto logger = Logger::get("config");
            if (logger) logger->error("Cannot open config file: {}", path);
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        return parse_yaml_string(content);
    } catch (const std::exception& e) {
        auto logger = Logger::get("config");
        if (logger) {
            logger->error("Config load error: {}", e.what());
        }
        return false;
    }
#else
    std::cerr << "[WARNING] yaml-cpp not found, using default configuration\n";

    SymbolConfig xrp_cfg;
    xrp_cfg.symbol = "XRP";
    xrp_cfg.upbit = "KRW-XRP";
    xrp_cfg.bithumb = "XRP_KRW";
    xrp_cfg.binance = "XRPUSDT";
    xrp_cfg.mexc = "XRPUSDT";
    xrp_cfg.enabled = true;
    symbols_.push_back(xrp_cfg);
    primary_symbols_.push_back(xrp_cfg);

    SymbolConfig btc_cfg;
    btc_cfg.symbol = "BTC";
    btc_cfg.upbit = "KRW-BTC";
    btc_cfg.bithumb = "BTC_KRW";
    btc_cfg.binance = "BTCUSDT";
    btc_cfg.mexc = "BTCUSDT";
    btc_cfg.enabled = true;
    symbols_.push_back(btc_cfg);
    primary_symbols_.push_back(btc_cfg);

    SymbolConfig eth_cfg;
    eth_cfg.symbol = "ETH";
    eth_cfg.upbit = "KRW-ETH";
    eth_cfg.bithumb = "ETH_KRW";
    eth_cfg.binance = "ETHUSDT";
    eth_cfg.mexc = "ETHUSDT";
    eth_cfg.enabled = true;
    symbols_.push_back(eth_cfg);
    primary_symbols_.push_back(eth_cfg);

    ExchangeConfig upbit_cfg;
    upbit_cfg.enabled = true;
    upbit_cfg.ws_url = "wss://api.upbit.com/websocket/v1";
    upbit_cfg.rest_url = "https://api.upbit.com/v1";
    exchanges_[Exchange::Upbit] = upbit_cfg;

    ExchangeConfig binance_cfg;
    binance_cfg.enabled = true;
    binance_cfg.ws_url = "wss://fstream.binance.com/ws";
    binance_cfg.rest_url = "https://fapi.binance.com";
    exchanges_[Exchange::Binance] = binance_cfg;

    ExchangeConfig bithumb_cfg;
    bithumb_cfg.enabled = true;
    bithumb_cfg.ws_url = "wss://ws-api.bithumb.com/websocket/v1";
    bithumb_cfg.rest_url = "https://api.bithumb.com";
    exchanges_[Exchange::Bithumb] = bithumb_cfg;

    ExchangeConfig mexc_cfg;
    mexc_cfg.enabled = true;
    mexc_cfg.ws_url = "wss://contract.mexc.com/edge";
    mexc_cfg.rest_url = "https://contract.mexc.com";
    exchanges_[Exchange::MEXC] = mexc_cfg;

    return true;
#endif
}

// =============================================================================
// 스트림에서 로드 (SOPS 파이프라인용)
// =============================================================================
bool Config::load_from_stream(std::istream& input) {
    WriteGuard lock(mutex_);

    std::string yaml_content(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());

    if (yaml_content.empty()) {
        auto logger = Logger::get("config");
        if (logger) {
            logger->error("Config stdin is empty");
        }
        return false;
    }

    bool ok = parse_yaml_string(yaml_content);
    if (ok) {
        cached_yaml_ = std::move(yaml_content);
    } else {
        // 실패 시 평문 제로화
        std::memset(yaml_content.data(), 0, yaml_content.size());
    }
    return ok;
}

// =============================================================================
// 리로드
// =============================================================================
bool Config::reload() {
    if (!cached_yaml_.empty()) {
        WriteGuard lock(mutex_);
        return parse_yaml_string(cached_yaml_);
    }
    return load(config_path_);
}

const ExchangeConfig& Config::exchange(Exchange ex) const {
    ReadGuard lock(mutex_);
    static ExchangeConfig empty;
    auto it = exchanges_.find(ex);
    return it != exchanges_.end() ? it->second : empty;
}

std::vector<std::string> Config::get_symbols_for_exchange(Exchange ex) const {
    ReadGuard lock(mutex_);
    std::vector<std::string> result;

    for (const auto& symbol : symbols_) {
        if (!symbol.enabled) continue;

        switch (ex) {
            case Exchange::Upbit:
                if (!symbol.upbit.empty()) result.push_back(symbol.upbit);
                break;
            case Exchange::Bithumb:
                if (!symbol.bithumb.empty()) result.push_back(symbol.bithumb);
                break;
            case Exchange::Binance:
                if (!symbol.binance.empty()) result.push_back(symbol.binance);
                break;
            case Exchange::MEXC:
                if (!symbol.mexc.empty()) result.push_back(symbol.mexc);
                break;
        }
    }

    return result;
}

}  // namespace arbitrage
