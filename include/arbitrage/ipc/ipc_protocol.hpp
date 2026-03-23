#pragma once

/**
 * TASK_42: IPC Protocol — Unix Domain Socket 메시지 프레이밍
 *
 * Wire format: [4-byte length (big-endian)][1-byte type][payload]
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>

#ifdef __linux__
#include <arpa/inet.h>
#endif

namespace arbitrage {

// =============================================================================
// IPC 메시지 타입
// =============================================================================

enum class IpcMessageType : uint8_t {
    Heartbeat       = 0x01,
    HeartbeatAck    = 0x02,

    // 주문 관련 (Engine ↔ OrderManager)
    OrderRequest    = 0x10,
    OrderResult     = 0x11,
    OrderCancel     = 0x12,

    // 리스크 관련 (Engine ↔ RiskManager)
    RiskUpdate      = 0x20,
    RiskQuery       = 0x21,
    RiskResponse    = 0x22,

    // 킬스위치
    KillSwitch      = 0x30,
    KillSwitchAck   = 0x31,

    // 모니터링
    MonitorEvent    = 0x40,
    SystemStatus    = 0x41,
    HealthPing      = 0x50,
    HealthPong      = 0x51,

    // 설정
    ConfigReload    = 0x60,
    ConfigAck       = 0x61,
};

// =============================================================================
// IPC 소켓 경로
// =============================================================================

namespace ipc_paths {
    constexpr const char* RISK_SOCKET    = "/tmp/kimchi_risk.sock";
    constexpr const char* MONITOR_SOCKET = "/tmp/kimchi_monitor.sock";
    constexpr const char* ORDER_SOCKET   = "/tmp/kimchi_order.sock";
}

// =============================================================================
// 프레임 헤더 (5 bytes)
// =============================================================================

constexpr size_t IPC_HEADER_SIZE = 5;
constexpr size_t IPC_MAX_PAYLOAD = 65536;

#pragma pack(push, 1)
struct IpcFrameHeader {
    uint32_t payload_length;   // big-endian
    uint8_t  msg_type;
};
#pragma pack(pop)

static_assert(sizeof(IpcFrameHeader) == IPC_HEADER_SIZE,
              "IpcFrameHeader must be 5 bytes");

// =============================================================================
// 헤더 인코딩/디코딩
// =============================================================================

inline std::array<uint8_t, IPC_HEADER_SIZE> encode_ipc_header(
    uint8_t type, uint32_t payload_len)
{
    std::array<uint8_t, IPC_HEADER_SIZE> buf{};
    uint32_t net_len = htonl(payload_len);
    std::memcpy(buf.data(), &net_len, 4);
    buf[4] = type;
    return buf;
}

inline IpcFrameHeader decode_ipc_header(const uint8_t* data) {
    IpcFrameHeader hdr{};
    uint32_t net_len;
    std::memcpy(&net_len, data, 4);
    hdr.payload_length = ntohl(net_len);
    hdr.msg_type = data[4];
    return hdr;
}

}  // namespace arbitrage
