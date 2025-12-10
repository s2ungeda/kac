#pragma once

// Include real nlohmann JSON
#include "../../../third_party/nlohmann/json.hpp"

// Export to arbitrage namespace
namespace arbitrage {
    using json = nlohmann::json;
}