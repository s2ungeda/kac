#include <iostream>
#include "arbitrage/exchange/order_base.hpp"
#include "arbitrage/common/logger.hpp"

using namespace arbitrage;

int main() {
    // Logger 초기화
    Logger::init("order_example");
    
    // API 키 (실제로는 환경변수나 설정 파일에서 읽어야 함)
    std::string upbit_access = "YOUR_UPBIT_ACCESS_KEY";
    std::string upbit_secret = "YOUR_UPBIT_SECRET_KEY";
    
    std::string binance_key = "YOUR_BINANCE_API_KEY";
    std::string binance_secret = "YOUR_BINANCE_SECRET_KEY";
    
    // Order 클라이언트 생성
    auto upbit_client = create_order_client(Exchange::Upbit, upbit_access, upbit_secret);
    auto binance_client = create_order_client(Exchange::Binance, binance_key, binance_secret);
    
    if (!upbit_client || !binance_client) {
        std::cerr << "Failed to create order clients\n";
        return 1;
    }
    
    // 잔고 조회 예제
    std::cout << "\n=== Balance Check ===\n";
    
    auto upbit_balance = upbit_client->get_balance("KRW");
    if (upbit_balance) {
        std::cout << "Upbit KRW Balance: " << upbit_balance.value().total() << "\n";
    } else {
        std::cout << "Failed to get Upbit balance: " << upbit_balance.error().message << "\n";
    }
    
    auto binance_balance = binance_client->get_balance("USDT");
    if (binance_balance) {
        std::cout << "Binance USDT Balance: " << binance_balance.value().total() << "\n";
    } else {
        std::cout << "Failed to get Binance balance: " << binance_balance.error().message << "\n";
    }
    
    // 주문 예제 (실제로 실행하지 않도록 주의!)
    std::cout << "\n=== Order Example (DRY RUN) ===\n";
    
    OrderRequest req;
    req.symbol = "XRP";
    req.side = OrderSide::Buy;
    req.type = OrderType::Limit;
    req.quantity = 10;  // 10 XRP
    req.price = 850;    // 850 KRW
    req.client_order_id = "test_order_001";
    
    std::cout << "Order Request:\n";
    std::cout << "  Symbol: " << req.symbol << "\n";
    std::cout << "  Side: " << (req.side == OrderSide::Buy ? "Buy" : "Sell") << "\n";
    std::cout << "  Type: " << (req.type == OrderType::Limit ? "Limit" : "Market") << "\n";
    std::cout << "  Quantity: " << req.quantity << "\n";
    std::cout << "  Price: " << (req.price.has_value() ? std::to_string(req.price.value()) : "N/A") << "\n";
    
    // 실제로 주문하려면 아래 주석 해제 (주의!)
    // auto result = upbit_client->place_order(req);
    // if (result) {
    //     std::cout << "Order placed successfully: " << result.value().order_id << "\n";
    // } else {
    //     std::cout << "Order failed: " << result.error().message << "\n";
    // }
    
    // 김프 계산 예제
    std::cout << "\n=== Kimchi Premium Example ===\n";
    
    double krw_price = 850;      // Upbit price in KRW
    double usdt_price = 0.65;    // Binance price in USDT
    double usd_krw_rate = 1300;  // USD/KRW exchange rate
    
    double binance_krw_price = usdt_price * usd_krw_rate;
    double premium = ((krw_price / binance_krw_price) - 1) * 100;
    
    std::cout << "Upbit XRP/KRW: " << krw_price << " KRW\n";
    std::cout << "Binance XRP/USDT: " << usdt_price << " USDT\n";
    std::cout << "Binance in KRW: " << binance_krw_price << " KRW\n";
    std::cout << "Kimchi Premium: " << premium << "%\n";
    
    return 0;
}