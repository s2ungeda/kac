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
            // return std::make_unique<BithumbOrderClient>(api_key, secret_key);
            return nullptr;  // TODO
            
        case Exchange::MEXC:
            // return std::make_unique<MEXCOrderClient>(api_key, secret_key);
            return nullptr;  // TODO
            
        default:
            return nullptr;
    }
}

}  // namespace arbitrage