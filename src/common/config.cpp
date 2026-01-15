#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include <fstream>
#include <iostream>

// yaml-cpp가 있으면 사용, 없으면 더미 구현
#ifdef HAS_YAML_CPP
#include <yaml-cpp/yaml.h>
#endif

namespace arbitrage {

Config& Config::instance() {
    static Config instance;
    return instance;
}

bool Config::load(const std::string& path) {
    std::lock_guard lock(mutex_);
    config_path_ = path;
    
#ifdef HAS_YAML_CPP
    try {
        YAML::Node root = YAML::LoadFile(path);
        
        // 심볼 설정
        if (root["symbols"]) {
            auto symbols = root["symbols"];
            
            // Primary symbols
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
            
            // Secondary symbols
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
                if (exchanges[name]) {
                    auto node = exchanges[name];
                    ExchangeConfig cfg;
                    cfg.enabled = node["enabled"].as<bool>(false);
                    cfg.api_key = node["api_key"].as<std::string>("");
                    cfg.api_secret = node["api_secret"].as<std::string>("");
                    cfg.ws_url = node["ws_url"].as<std::string>("");
                    cfg.rest_url = node["rest_url"].as<std::string>("");
                    cfg.rate_limit_per_second = node["rate_limit_per_second"].as<int>(10);
                    exchanges_[ex] = cfg;
                }
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
        }
        
        return true;
        
    } catch (const std::exception& e) {
        auto logger = Logger::get("config");
        if (logger) {
            logger->error("Config load error: {}", e.what());
        }
        return false;
    }
#else
    // yaml-cpp가 없을 때는 기본값 사용
    std::cerr << "[WARNING] yaml-cpp not found, using default configuration\n";
    
    // 기본 심볼 설정
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
    
    // 기본 거래소 설정값
    ExchangeConfig upbit_cfg;
    upbit_cfg.enabled = true;
    upbit_cfg.ws_url = "wss://api.upbit.com/websocket/v1";
    upbit_cfg.rest_url = "https://api.upbit.com/v1";
    exchanges_[Exchange::Upbit] = upbit_cfg;
    
    ExchangeConfig binance_cfg;
    binance_cfg.enabled = true;
    binance_cfg.ws_url = "wss://stream.binance.com:9443/ws";
    binance_cfg.rest_url = "https://api.binance.com";
    exchanges_[Exchange::Binance] = binance_cfg;
    
    ExchangeConfig bithumb_cfg;
    bithumb_cfg.enabled = true;
    bithumb_cfg.ws_url = "wss://pubwss.bithumb.com/pub/ws";
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

bool Config::reload() {
    return load(config_path_);
}

const ExchangeConfig& Config::exchange(Exchange ex) const {
    std::lock_guard lock(mutex_);
    static ExchangeConfig empty;
    auto it = exchanges_.find(ex);
    return it != exchanges_.end() ? it->second : empty;
}

std::vector<std::string> Config::get_symbols_for_exchange(Exchange ex) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    
    // 활성화된 심볼들만 반환
    for (const auto& symbol : symbols_) {
        if (!symbol.enabled) continue;
        
        switch (ex) {
            case Exchange::Upbit:
                if (!symbol.upbit.empty()) {
                    result.push_back(symbol.upbit);
                }
                break;
            case Exchange::Bithumb:
                if (!symbol.bithumb.empty()) {
                    result.push_back(symbol.bithumb);
                }
                break;
            case Exchange::Binance:
                if (!symbol.binance.empty()) {
                    result.push_back(symbol.binance);
                }
                break;
            case Exchange::MEXC:
                if (!symbol.mexc.empty()) {
                    result.push_back(symbol.mexc);
                }
                break;
        }
    }
    
    return result;
}

}  // namespace arbitrage