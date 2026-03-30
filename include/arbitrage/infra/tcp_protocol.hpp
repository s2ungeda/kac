#pragma once

/**
 * TCP Protocol Definitions
 *
 * Binary protocol for Delphi client communication:
 * - Message types (enum)
 * - Message header (packed struct)
 * - Message (header + payload)
 * - JSON payload helpers
 */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace arbitrage {

// =============================================================================
// Message Types
// =============================================================================

/**
 * Message type codes
 */
enum class MessageType : uint8_t {
    // Connection / Auth
    Ping = 0x01,
    Pong = 0x02,
    AuthRequest = 0x10,
    AuthResponse = 0x11,
    Disconnect = 0x1F,

    // Market data
    TickerUpdate = 0x20,
    OrderBookUpdate = 0x21,
    PremiumUpdate = 0x22,
    OpportunityAlert = 0x23,

    // Order / Trade
    OrderStatus = 0x30,
    TradeResult = 0x31,
    BalanceUpdate = 0x32,

    // System
    SystemStatus = 0x40,
    HealthStatus = 0x41,
    KillSwitch = 0x42,
    ConfigUpdate = 0x43,

    // Commands (client -> server)
    CmdStartStrategy = 0x80,
    CmdStopStrategy = 0x81,
    CmdSetKillSwitch = 0x82,
    CmdGetStatus = 0x83,

    // Responses
    CmdResponse = 0xF0,
    Error = 0xFF
};

/**
 * Human-readable message type name
 */
inline const char* message_type_name(MessageType type) {
    switch (type) {
        case MessageType::Ping: return "Ping";
        case MessageType::Pong: return "Pong";
        case MessageType::AuthRequest: return "AuthRequest";
        case MessageType::AuthResponse: return "AuthResponse";
        case MessageType::Disconnect: return "Disconnect";
        case MessageType::TickerUpdate: return "TickerUpdate";
        case MessageType::OrderBookUpdate: return "OrderBookUpdate";
        case MessageType::PremiumUpdate: return "PremiumUpdate";
        case MessageType::OpportunityAlert: return "OpportunityAlert";
        case MessageType::OrderStatus: return "OrderStatus";
        case MessageType::TradeResult: return "TradeResult";
        case MessageType::BalanceUpdate: return "BalanceUpdate";
        case MessageType::SystemStatus: return "SystemStatus";
        case MessageType::HealthStatus: return "HealthStatus";
        case MessageType::KillSwitch: return "KillSwitch";
        case MessageType::ConfigUpdate: return "ConfigUpdate";
        case MessageType::CmdStartStrategy: return "CmdStartStrategy";
        case MessageType::CmdStopStrategy: return "CmdStopStrategy";
        case MessageType::CmdSetKillSwitch: return "CmdSetKillSwitch";
        case MessageType::CmdGetStatus: return "CmdGetStatus";
        case MessageType::CmdResponse: return "CmdResponse";
        case MessageType::Error: return "Error";
        default: return "Unknown";
    }
}

// =============================================================================
// Message Header
// =============================================================================

/**
 * Wire-format message header (8 bytes, packed)
 *
 * | Magic (2) | Version (1) | Type (1) | Payload Length (4) |
 * | 0x4B 0x41 |     0x01    |   type   |     length         |
 */
#pragma pack(push, 1)
struct MessageHeader {
    uint8_t magic[2]{0x4B, 0x41};  // "KA" (Kimchi Arbitrage)
    uint8_t version{0x01};
    uint8_t msg_type{0};
    uint32_t payload_length{0};

    bool is_valid() const {
        return magic[0] == 0x4B && magic[1] == 0x41 && version == 0x01;
    }

    MessageType type() const {
        return static_cast<MessageType>(msg_type);
    }

    void set_type(MessageType t) {
        msg_type = static_cast<uint8_t>(t);
    }
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 8, "MessageHeader must be 8 bytes");

// =============================================================================
// Message
// =============================================================================

/**
 * Complete message (header + payload)
 */
struct Message {
    MessageHeader header;
    std::vector<uint8_t> payload;

    Message() = default;
    Message(MessageType type, const std::vector<uint8_t>& data = {})
        : payload(data)
    {
        header.set_type(type);
        header.payload_length = static_cast<uint32_t>(data.size());
    }

    Message(MessageType type, const std::string& data)
        : payload(data.begin(), data.end())
    {
        header.set_type(type);
        header.payload_length = static_cast<uint32_t>(data.size());
    }

    std::string payload_str() const {
        return std::string(payload.begin(), payload.end());
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> result;
        result.reserve(sizeof(MessageHeader) + payload.size());

        // Header
        const uint8_t* hdr = reinterpret_cast<const uint8_t*>(&header);
        result.insert(result.end(), hdr, hdr + sizeof(MessageHeader));

        // Payload
        result.insert(result.end(), payload.begin(), payload.end());

        return result;
    }
};

// =============================================================================
// JSON Payload Helpers
// =============================================================================

/**
 * Simple JSON payload from string key-value pairs
 */
inline std::string make_json_payload(
    const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string json = "{";
    bool first = true;
    for (const auto& [key, value] : fields) {
        if (!first) json += ",";
        json += "\"" + key + "\":\"" + value + "\"";
        first = false;
    }
    json += "}";
    return json;
}

/**
 * JSON payload with both string and numeric fields
 */
inline std::string make_json_payload_num(
    const std::vector<std::pair<std::string, std::string>>& str_fields,
    const std::vector<std::pair<std::string, double>>& num_fields)
{
    std::string json = "{";
    bool first = true;

    for (const auto& [key, value] : str_fields) {
        if (!first) json += ",";
        json += "\"" + key + "\":\"" + value + "\"";
        first = false;
    }

    for (const auto& [key, value] : num_fields) {
        if (!first) json += ",";
        json += "\"" + key + "\":" + std::to_string(value);
        first = false;
    }

    json += "}";
    return json;
}

}  // namespace arbitrage
