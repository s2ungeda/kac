#include "arbitrage/exchange/order_base.hpp"
#include "arbitrage/exchange/upbit/order.hpp"
#include "arbitrage/exchange/binance/order.hpp"
// #include "arbitrage/exchange/bithumb/order.hpp"
// #include "arbitrage/exchange/mexc/order.hpp"

namespace arbitrage {

std::unique_ptr<OrderClientBase> create_order_client(
    Exchange exchange,
    const std::string& api_key,
    const std::string& secret_key) 
{
    switch (exchange) {
        case Exchange::Upbit:
            return std::make_unique<UpbitOrderClient>(api_key, secret_key);
            
        case Exchange::Binance:
            return std::make_unique<BinanceOrderClient>(api_key, secret_key);
            
        case Exchange::Bithumb:
            // TODO: Bithumb order client implementation
            throw std::runtime_error("Bithumb order client not implemented");
            
        case Exchange::MEXC:
            // TODO: MEXC order client implementation
            throw std::runtime_error("MEXC order client not implemented");
            
        default:
            throw std::runtime_error("Unknown exchange");
    }
}

}  // namespace arbitrage