/**
 * Multi Account Manager Implementation (TASK_17)
 */

#include "arbitrage/common/account_manager.hpp"

#include <fstream>
#include <random>
#include <algorithm>
#include <sstream>

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
namespace { AccountManager* g_set_account_manager_override = nullptr; }
AccountManager& account_manager() {
    if (g_set_account_manager_override) return *g_set_account_manager_override;
    static AccountManager instance;
    return instance;
}
void set_account_manager(AccountManager* p) { g_set_account_manager_override = p; }

// =============================================================================
// 생성자
// =============================================================================
AccountManager::AccountManager() {
    // 라운드 로빈 인덱스 초기화
    for (uint8_t i = 0; i < static_cast<uint8_t>(Exchange::Count); ++i) {
        round_robin_index_[static_cast<Exchange>(i)] = 0;
    }
}

// =============================================================================
// 계정 관리
// =============================================================================
Result<void> AccountManager::add_account(const Account& account) {
    if (account.id.empty()) {
        return Error(ErrorCode::InvalidParameter, "Account ID cannot be empty");
    }

    Account new_account = account;
    new_account.created_at = std::chrono::system_clock::now();
    new_account.last_used_at = new_account.created_at;

    {
        WriteGuard lock(mutex_);

        if (accounts_.find(account.id) != accounts_.end()) {
            return Error(ErrorCode::InvalidParameter, "Account already exists: " + account.id);
        }

        accounts_[account.id] = new_account;
    }

    notify_change(new_account, "added");

    return {};
}

Result<void> AccountManager::remove_account(const std::string& account_id) {
    Account removed;
    {
        WriteGuard lock(mutex_);

        auto it = accounts_.find(account_id);
        if (it == accounts_.end()) {
            return Error(ErrorCode::NotFound, "Account not found: " + account_id);
        }

        removed = it->second;
        accounts_.erase(it);
    }

    notify_change(removed, "removed");

    return {};
}

Result<void> AccountManager::update_account(const Account& account) {
    {
        WriteGuard lock(mutex_);

        auto it = accounts_.find(account.id);
        if (it == accounts_.end()) {
            return Error(ErrorCode::NotFound, "Account not found: " + account.id);
        }

        // 생성 시간은 유지
        auto created_at = it->second.created_at;
        it->second = account;
        it->second.created_at = created_at;
    }

    notify_change(account, "updated");

    return {};
}

std::optional<Account> AccountManager::get_account(const std::string& account_id) const {
    ReadGuard lock(mutex_);

    auto it = accounts_.find(account_id);
    if (it == accounts_.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::vector<Account> AccountManager::get_accounts(Exchange exchange) const {
    ReadGuard lock(mutex_);

    std::vector<Account> result;
    for (const auto& [id, account] : accounts_) {
        if (account.exchange == exchange) {
            result.push_back(account);
        }
    }

    return result;
}

std::vector<Account> AccountManager::get_active_accounts(Exchange exchange) const {
    ReadGuard lock(mutex_);

    std::vector<Account> result;
    for (const auto& [id, account] : accounts_) {
        if (account.exchange == exchange && account.is_active()) {
            result.push_back(account);
        }
    }

    return result;
}

std::vector<Account> AccountManager::get_all_accounts() const {
    ReadGuard lock(mutex_);

    std::vector<Account> result;
    result.reserve(accounts_.size());
    for (const auto& [id, account] : accounts_) {
        result.push_back(account);
    }

    return result;
}

size_t AccountManager::count() const {
    ReadGuard lock(mutex_);
    return accounts_.size();
}

size_t AccountManager::count(Exchange exchange) const {
    ReadGuard lock(mutex_);

    size_t cnt = 0;
    for (const auto& [id, account] : accounts_) {
        if (account.exchange == exchange) {
            ++cnt;
        }
    }

    return cnt;
}

// =============================================================================
// 계정 선택
// =============================================================================
std::optional<Account> AccountManager::select_account(
    Exchange exchange,
    double required_balance,
    const std::string& symbol
) {
    WriteGuard lock(mutex_);

    // 활성 계정 필터링
    std::vector<Account> candidates;
    for (const auto& [id, account] : accounts_) {
        if (account.exchange != exchange || !account.is_active()) {
            continue;
        }

        // 잔고 확인
        if (required_balance > 0.0 && !symbol.empty()) {
            auto it = account.balances.find(symbol);
            if (it == account.balances.end() || it->second < required_balance) {
                continue;
            }
        }

        candidates.push_back(account);
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    ++stats_.selection_count;

    // 전략에 따라 선택
    Account* selected = nullptr;

    switch (strategy_) {
        case AccountSelectionStrategy::RoundRobin: {
            size_t idx = next_round_robin_index(exchange);
            selected = &candidates[idx % candidates.size()];
            break;
        }

        case AccountSelectionStrategy::WeightedRandom: {
            auto result = select_weighted_random(candidates);
            if (result) {
                // 임시로 포인터 처리
                for (auto& c : candidates) {
                    if (c.id == result->id) {
                        selected = &c;
                        break;
                    }
                }
            }
            break;
        }

        case AccountSelectionStrategy::LeastUsed: {
            selected = &candidates[0];
            for (auto& c : candidates) {
                if (c.order_count < selected->order_count) {
                    selected = &c;
                }
            }
            break;
        }

        case AccountSelectionStrategy::HighestBalance: {
            if (symbol.empty()) {
                selected = &candidates[0];
            } else {
                double max_balance = 0.0;
                for (auto& c : candidates) {
                    auto it = c.balances.find(symbol);
                    if (it != c.balances.end() && it->second > max_balance) {
                        max_balance = it->second;
                        selected = &c;
                    }
                }
                if (!selected) {
                    selected = &candidates[0];
                }
            }
            break;
        }

        case AccountSelectionStrategy::LowestLatency:
        default:
            selected = &candidates[0];
            break;
    }

    if (selected) {
        // 사용 시간 업데이트
        auto it = accounts_.find(selected->id);
        if (it != accounts_.end()) {
            it->second.last_used_at = std::chrono::system_clock::now();
        }
        return *selected;
    }

    return std::nullopt;
}

size_t AccountManager::next_round_robin_index(Exchange exchange) {
    auto& idx = round_robin_index_[exchange];
    return idx++;
}

std::optional<Account> AccountManager::select_weighted_random(
    const std::vector<Account>& candidates
) {
    if (candidates.empty()) {
        return std::nullopt;
    }

    // 총 가중치 계산
    double total_weight = 0.0;
    for (const auto& c : candidates) {
        total_weight += c.weight;
    }

    if (total_weight <= 0.0) {
        // 모든 가중치가 0이면 첫 번째 선택
        return candidates[0];
    }

    // 랜덤 선택
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, total_weight);
    double r = dis(gen);

    double cumulative = 0.0;
    for (const auto& c : candidates) {
        cumulative += c.weight;
        if (r <= cumulative) {
            return c;
        }
    }

    return candidates.back();
}

// =============================================================================
// 잔고 관리
// =============================================================================
void AccountManager::update_balance(
    const std::string& account_id,
    const std::map<std::string, double>& balances
) {
    WriteGuard lock(mutex_);

    auto it = accounts_.find(account_id);
    if (it == accounts_.end()) {
        return;
    }

    it->second.balances = balances;
    it->second.balance_updated_at = std::chrono::system_clock::now();
}

std::map<std::string, double> AccountManager::get_total_balance(Exchange exchange) const {
    ReadGuard lock(mutex_);

    std::map<std::string, double> total;

    for (const auto& [id, account] : accounts_) {
        if (account.exchange == exchange && account.is_active()) {
            for (const auto& [symbol, balance] : account.balances) {
                total[symbol] += balance;
            }
        }
    }

    return total;
}

double AccountManager::get_total_balance(Exchange exchange, const std::string& symbol) const {
    ReadGuard lock(mutex_);

    double total = 0.0;

    for (const auto& [id, account] : accounts_) {
        if (account.exchange == exchange && account.is_active()) {
            auto it = account.balances.find(symbol);
            if (it != account.balances.end()) {
                total += it->second;
            }
        }
    }

    return total;
}

void AccountManager::refresh_balances(Exchange exchange) {
    if (!balance_callback_) {
        return;
    }

    std::vector<std::string> account_ids;
    {
        ReadGuard lock(mutex_);
        for (const auto& [id, account] : accounts_) {
            if (account.exchange == exchange && account.is_active()) {
                account_ids.push_back(id);
            }
        }
    }

    for (const auto& id : account_ids) {
        std::optional<Account> account;
        {
            ReadGuard lock(mutex_);
            auto it = accounts_.find(id);
            if (it != accounts_.end()) {
                account = it->second;
            }
        }

        if (account) {
            auto balances = balance_callback_(*account);
            update_balance(id, balances);
        }
    }
}

void AccountManager::refresh_all_balances() {
    for (uint8_t i = 0; i < static_cast<uint8_t>(Exchange::Count); ++i) {
        refresh_balances(static_cast<Exchange>(i));
    }
}

// =============================================================================
// 상태 관리
// =============================================================================
void AccountManager::enable_account(const std::string& account_id) {
    Account copy;
    bool found = false;
    {
        WriteGuard lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end()) {
            it->second.enabled = true;
            it->second.status = AccountStatus::Active;
            it->second.status_message.clear();
            copy = it->second;
            found = true;
        }
    }
    if (found) {
        notify_change(copy, "enabled");
    }
}

void AccountManager::disable_account(const std::string& account_id) {
    Account copy;
    bool found = false;
    {
        WriteGuard lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end()) {
            it->second.enabled = false;
            copy = it->second;
            found = true;
        }
    }
    if (found) {
        notify_change(copy, "disabled");
    }
}

void AccountManager::set_account_status(
    const std::string& account_id,
    AccountStatus status,
    const std::string& message
) {
    Account copy;
    bool found = false;
    {
        WriteGuard lock(mutex_);
        auto it = accounts_.find(account_id);
        if (it != accounts_.end()) {
            it->second.status = status;
            it->second.status_message = message;
            copy = it->second;
            found = true;
        }
    }
    if (found) {
        notify_change(copy, "status_changed");
    }
}

void AccountManager::record_order(const std::string& account_id) {
    WriteGuard lock(mutex_);

    auto it = accounts_.find(account_id);
    if (it != accounts_.end()) {
        ++it->second.order_count;
        it->second.last_used_at = std::chrono::system_clock::now();
        ++stats_.total_orders;
    }
}

void AccountManager::record_error(const std::string& account_id) {
    WriteGuard lock(mutex_);

    auto it = accounts_.find(account_id);
    if (it != accounts_.end()) {
        ++it->second.error_count;
        ++stats_.total_errors;

        // 연속 오류 시 상태 변경 (예: 5회 연속)
        // 여기서는 단순히 카운트만 증가
    }
}

// =============================================================================
// 이벤트
// =============================================================================
void AccountManager::on_account_change(AccountChangeCallback callback) {
    WriteGuard lock(mutex_);
    change_callbacks_.push_back(std::move(callback));
}

void AccountManager::notify_change(const Account& account, const std::string& event) {
    ReadGuard lock(mutex_);
    for (const auto& cb : change_callbacks_) {
        cb(account, event);
    }
}

// =============================================================================
// 설정 파일 저장/로드
// =============================================================================
Result<void> AccountManager::save_to_file(const std::string& path) const {
    ReadGuard lock(mutex_);

    std::ofstream file(path);
    if (!file) {
        return Error(ErrorCode::FileError, "Failed to open file for writing: " + path);
    }

    // 간단한 텍스트 포맷 (YAML/JSON 대신)
    file << "# Account Manager Configuration\n";
    file << "# Generated by AccountManager\n\n";

    file << "accounts:\n";
    for (const auto& [id, account] : accounts_) {
        file << "  - id: " << account.id << "\n";
        file << "    exchange: " << exchange_name(account.exchange) << "\n";
        file << "    label: " << account.label << "\n";
        file << "    api_key_ref: " << account.api_key_ref << "\n";
        file << "    api_secret_ref: " << account.api_secret_ref << "\n";
        file << "    enabled: " << (account.enabled ? "true" : "false") << "\n";
        file << "    weight: " << account.weight << "\n";
        file << "    max_order_amount: " << account.max_order_amount << "\n";
        file << "\n";
    }

    if (!file) {
        return Error(ErrorCode::FileError, "Failed to write to file: " + path);
    }

    return {};
}

Result<void> AccountManager::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return Error(ErrorCode::FileError, "Failed to open file for reading: " + path);
    }

    // 간단한 파서 (실제로는 YAML 라이브러리 사용 권장)
    std::string line;
    Account current_account;
    bool in_account = false;

    auto parse_exchange = [](const std::string& s) -> Exchange {
        if (s == "upbit") return Exchange::Upbit;
        if (s == "bithumb") return Exchange::Bithumb;
        if (s == "binance") return Exchange::Binance;
        if (s == "mexc") return Exchange::MEXC;
        return Exchange::Upbit;
    };

    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
    };

    WriteGuard lock(mutex_);

    while (std::getline(file, line)) {
        // 주석과 빈 줄 건너뛰기
        if (line.empty() || line[0] == '#') continue;

        if (line.find("  - id:") != std::string::npos) {
            // 이전 계정 저장
            if (in_account && !current_account.id.empty()) {
                current_account.created_at = std::chrono::system_clock::now();
                accounts_[current_account.id] = current_account;
            }

            // 새 계정 시작
            current_account = Account();
            in_account = true;
            current_account.id = line.substr(line.find(":") + 1);
            trim(current_account.id);
        }
        else if (in_account) {
            size_t colon_pos = line.find(":");
            if (colon_pos != std::string::npos) {
                std::string key = line.substr(0, colon_pos);
                std::string value = line.substr(colon_pos + 1);
                trim(key);
                trim(value);

                if (key.find("exchange") != std::string::npos) {
                    current_account.exchange = parse_exchange(value);
                }
                else if (key.find("label") != std::string::npos) {
                    current_account.label = value;
                }
                else if (key.find("api_key_ref") != std::string::npos) {
                    current_account.api_key_ref = value;
                }
                else if (key.find("api_secret_ref") != std::string::npos) {
                    current_account.api_secret_ref = value;
                }
                else if (key.find("enabled") != std::string::npos) {
                    current_account.enabled = (value == "true");
                }
                else if (key.find("weight") != std::string::npos) {
                    current_account.weight = std::stod(value);
                }
                else if (key.find("max_order_amount") != std::string::npos) {
                    current_account.max_order_amount = std::stod(value);
                }
            }
        }
    }

    // 마지막 계정 저장
    if (in_account && !current_account.id.empty()) {
        current_account.created_at = std::chrono::system_clock::now();
        accounts_[current_account.id] = current_account;
    }

    return {};
}

}  // namespace arbitrage
