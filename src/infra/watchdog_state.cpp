#include "arbitrage/infra/watchdog_state.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace arbitrage {

// =============================================================================
// 상태 저장/로드
// =============================================================================

void WatchdogState::save_state(const PersistedState& state) {
    auto filename = generate_state_filename();
    auto data = serialize_state(state);

    std::ofstream file(filename, std::ios::binary);
    if (file) {
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
}

std::optional<PersistedState> WatchdogState::load_latest_state() {
    auto snapshots = list_state_snapshots(1);
    if (snapshots.empty()) {
        return std::nullopt;
    }

    std::ifstream file(snapshots[0], std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());

    if (data.empty()) {
        return std::nullopt;
    }

    return deserialize_state(data);
}

// =============================================================================
// 스냅샷 관리
// =============================================================================

std::vector<std::string> WatchdogState::list_state_snapshots(int max_count) {
    std::vector<std::string> result;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(state_directory_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                result.push_back(entry.path().string());
            }
        }
    } catch (...) {
        return result;
    }

    // 최신 순으로 정렬
    std::sort(result.begin(), result.end(), std::greater<>());

    if (static_cast<int>(result.size()) > max_count) {
        result.resize(max_count);
    }

    return result;
}

void WatchdogState::cleanup_old_snapshots(int keep_count) {
    auto snapshots = list_state_snapshots(10000);

    for (size_t i = keep_count; i < snapshots.size(); ++i) {
        try {
            std::filesystem::remove(snapshots[i]);
        } catch (const std::exception& e) {
            // 스냅샷 삭제 실패 (무시하고 계속)
        }
    }
}

// =============================================================================
// 프로세스 상태 조회
// =============================================================================

ProcessStatus WatchdogState::get_process_status(int pid) {
    ProcessStatus status;
    status.pid = pid;

#ifndef _WIN32
    // /proc/[pid]/stat 읽기
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream stat_file(stat_path);

    if (stat_file) {
        status.is_running = true;

        // 간단히 메모리만 읽기
        std::string statm_path = "/proc/" + std::to_string(pid) + "/statm";
        std::ifstream statm_file(statm_path);

        if (statm_file) {
            size_t size, resident;
            statm_file >> size >> resident;
            status.memory_bytes = resident * 4096;  // 페이지 크기
        }
    } else {
        status.is_running = false;
    }
#endif

    return status;
}

// =============================================================================
// 상태 파일 경로 생성
// =============================================================================

std::string WatchdogState::generate_state_filename() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm_now);

    static std::atomic<int> counter{0};
    int seq = counter.fetch_add(1, std::memory_order_relaxed) % 1000;

    std::ostringstream oss;
    oss << state_directory_ << "/state_" << buffer << "_"
        << std::setfill('0') << std::setw(3) << seq << ".dat";

    return oss.str();
}

// =============================================================================
// 직렬화/역직렬화
// =============================================================================

std::vector<uint8_t> WatchdogState::serialize_state(const PersistedState& state) {
    std::vector<uint8_t> data;

    // 간단한 직렬화 (실제로는 MessagePack 또는 Protobuf 사용 권장)

    // 버전 (8바이트)
    for (int i = 0; i < 8; ++i) {
        data.push_back((state.version >> (i * 8)) & 0xFF);
    }

    // 타임스탬프 (8바이트)
    auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
        state.saved_at.time_since_epoch()).count();
    for (int i = 0; i < 8; ++i) {
        data.push_back((ts >> (i * 8)) & 0xFF);
    }

    // total_pnl_today (8바이트)
    uint64_t pnl_bits;
    memcpy(&pnl_bits, &state.total_pnl_today, sizeof(pnl_bits));
    for (int i = 0; i < 8; ++i) {
        data.push_back((pnl_bits >> (i * 8)) & 0xFF);
    }

    // total_trades_today (4바이트)
    for (int i = 0; i < 4; ++i) {
        data.push_back((state.total_trades_today >> (i * 8)) & 0xFF);
    }

    // daily_loss_used (8바이트)
    uint64_t loss_bits;
    memcpy(&loss_bits, &state.daily_loss_used, sizeof(loss_bits));
    for (int i = 0; i < 8; ++i) {
        data.push_back((loss_bits >> (i * 8)) & 0xFF);
    }

    // kill_switch_active (1바이트)
    data.push_back(state.kill_switch_active ? 1 : 0);

    // last_error 길이 + 데이터
    uint32_t error_len = static_cast<uint32_t>(state.last_error.size());
    for (int i = 0; i < 4; ++i) {
        data.push_back((error_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), state.last_error.begin(), state.last_error.end());

    return data;
}

PersistedState WatchdogState::deserialize_state(const std::vector<uint8_t>& data) {
    PersistedState state;

    if (data.size() < 37) {  // 최소 크기
        return state;
    }

    size_t offset = 0;

    // 버전
    state.version = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        state.version |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }

    // 타임스탬프
    uint64_t ts = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        ts |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }
    state.saved_at = std::chrono::system_clock::time_point(
        std::chrono::microseconds(ts));

    // total_pnl_today
    uint64_t pnl_bits = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        pnl_bits |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }
    memcpy(&state.total_pnl_today, &pnl_bits, sizeof(state.total_pnl_today));

    // total_trades_today
    state.total_trades_today = 0;
    for (int i = 0; i < 4 && offset < data.size(); ++i, ++offset) {
        state.total_trades_today |= static_cast<int>(data[offset]) << (i * 8);
    }

    // daily_loss_used
    uint64_t loss_bits = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        loss_bits |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }
    memcpy(&state.daily_loss_used, &loss_bits, sizeof(state.daily_loss_used));

    // kill_switch_active
    if (offset < data.size()) {
        state.kill_switch_active = data[offset++] != 0;
    }

    // last_error
    if (offset + 4 <= data.size()) {
        uint32_t error_len = 0;
        for (int i = 0; i < 4; ++i, ++offset) {
            error_len |= static_cast<uint32_t>(data[offset]) << (i * 8);
        }

        if (offset + error_len <= data.size()) {
            state.last_error.assign(data.begin() + offset, data.begin() + offset + error_len);
        }
    }

    return state;
}

}  // namespace arbitrage
