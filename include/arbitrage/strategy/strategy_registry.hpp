#pragma once

/**
 * Strategy Registry (TASK_14)
 *
 * 전략 팩토리 패턴 레지스트리
 * - 전략 타입 등록
 * - 전략 인스턴스 생성
 * - 자동 등록 매크로
 */

#include "arbitrage/strategy/strategy_interface.hpp"

#include <map>
#include <vector>
#include <string>
#include <memory>
#include <shared_mutex>

namespace arbitrage {

// =============================================================================
// 전략 레지스트리 (싱글톤)
// =============================================================================
class StrategyRegistry {
public:
    // 싱글톤 인스턴스
    static StrategyRegistry& instance() {
        static StrategyRegistry registry;
        return registry;
    }

    // 복사/이동 금지
    StrategyRegistry(const StrategyRegistry&) = delete;
    StrategyRegistry& operator=(const StrategyRegistry&) = delete;

    // =========================================================================
    // 전략 타입 등록
    // =========================================================================

    /**
     * 전략 팩토리 등록
     * @param type_name 전략 타입 이름 (예: "basic_arb")
     * @param factory 전략 생성 함수
     * @return 등록 성공 여부
     */
    bool register_type(const std::string& type_name, StrategyFactory factory) {
        std::unique_lock lock(mutex_);
        if (factories_.find(type_name) != factories_.end()) {
            return false;  // 이미 등록됨
        }
        factories_[type_name] = std::move(factory);
        return true;
    }

    /**
     * 전략 타입 등록 해제
     */
    bool unregister_type(const std::string& type_name) {
        std::unique_lock lock(mutex_);
        return factories_.erase(type_name) > 0;
    }

    // =========================================================================
    // 전략 인스턴스 생성
    // =========================================================================

    /**
     * 전략 인스턴스 생성
     * @param type_name 전략 타입 이름
     * @return 생성된 전략 (실패 시 nullptr)
     */
    std::unique_ptr<IStrategy> create(const std::string& type_name) {
        std::shared_lock lock(mutex_);
        auto it = factories_.find(type_name);
        if (it == factories_.end()) {
            return nullptr;
        }
        return it->second();
    }

    /**
     * 전략 인스턴스 생성 및 초기화
     * @param config 전략 설정
     * @return 초기화된 전략 (실패 시 nullptr)
     */
    std::unique_ptr<IStrategy> create_and_init(const StrategyConfig& config) {
        auto strategy = create(config.type);
        if (strategy) {
            strategy->initialize(config);
        }
        return strategy;
    }

    // =========================================================================
    // 조회
    // =========================================================================

    /**
     * 등록된 전략 타입 목록
     */
    std::vector<std::string> registered_types() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> types;
        types.reserve(factories_.size());
        for (const auto& [name, _] : factories_) {
            types.push_back(name);
        }
        return types;
    }

    /**
     * 전략 타입 등록 여부 확인
     */
    bool has_type(const std::string& type_name) const {
        std::shared_lock lock(mutex_);
        return factories_.find(type_name) != factories_.end();
    }

    /**
     * 등록된 타입 수
     */
    size_t type_count() const {
        std::shared_lock lock(mutex_);
        return factories_.size();
    }

private:
    StrategyRegistry() = default;

    mutable std::shared_mutex mutex_;
    std::map<std::string, StrategyFactory> factories_;
};

// =============================================================================
// 자동 등록 헬퍼
// =============================================================================

/**
 * 전략 자동 등록 클래스
 * 정적 초기화 시점에 전략을 등록
 */
class StrategyAutoRegister {
public:
    StrategyAutoRegister(const char* type_name, StrategyFactory factory) {
        StrategyRegistry::instance().register_type(type_name, std::move(factory));
    }
};

// =============================================================================
// 자동 등록 매크로
// =============================================================================

/**
 * 전략 클래스를 레지스트리에 자동 등록
 *
 * 사용법:
 *   class MyStrategy : public StrategyBase { ... };
 *   REGISTER_STRATEGY("my_strategy", MyStrategy);
 */
#define REGISTER_STRATEGY(TypeName, ClassName) \
    static ::arbitrage::StrategyAutoRegister \
        _strategy_registrar_##ClassName( \
            TypeName, \
            []() -> std::unique_ptr<::arbitrage::IStrategy> { \
                return std::make_unique<ClassName>(); \
            } \
        )

}  // namespace arbitrage
