#pragma once

/**
 * Service Locator — Singleton Override Setters
 *
 * Application이 소유한 인스턴스를 글로벌 싱글톤 accessor에 설정.
 * nullptr 전달 시 기본 static local 인스턴스로 복귀.
 */

namespace arbitrage {

// Forward declarations
class Config;
class SymbolMaster;
class FeeCalculator;
class AccountManager;
class RuntimeKeyStore;
class ShutdownManager;
class HealthChecker;
class WatchdogClient;
class DailyLossLimiter;
class TradingStatsTracker;
class AlertService;
class DecisionEngine;
class RiskModel;
class StrategyExecutor;
class ThreadManager;

// Singleton override setters
void set_config_instance(Config* p);
void set_symbol_master(SymbolMaster* p);
void set_fee_calculator(FeeCalculator* p);
void set_account_manager(AccountManager* p);
void set_runtime_keystore(RuntimeKeyStore* p);
void set_shutdown_manager(ShutdownManager* p);
void set_health_checker(HealthChecker* p);
void set_watchdog_client(WatchdogClient* p);
void set_daily_limiter(DailyLossLimiter* p);
void set_trading_stats(TradingStatsTracker* p);
void set_alert_service(AlertService* p);
void set_decision_engine(DecisionEngine* p);
void set_risk_model(RiskModel* p);
void set_strategy_executor(StrategyExecutor* p);
void set_thread_manager(ThreadManager* p);

}  // namespace arbitrage
