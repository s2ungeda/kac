#include <iostream>
#include <string>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

int main() {
    try {
        std::string host = "wbs-api.mexc.com";
        std::string port = "443";
        std::string target = "/ws";
        
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};
        ctx.set_default_verify_paths();
        
        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};
        
        // DNS 조회
        auto const results = resolver.resolve(host, port);
        
        // TCP 연결
        auto ep = net::connect(get_lowest_layer(ws), results);
        std::cout << "Connected to " << ep << std::endl;
        
        // SSL handshake with SNI
        if(!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category()),
                "Failed to set SNI Hostname");
        }
        
        ws.next_layer().handshake(ssl::stream_base::client);
        std::cout << "SSL handshake completed" << std::endl;
        
        // WebSocket handshake - 최소한의 헤더만
        ws.handshake(host, target);
        std::cout << "WebSocket handshake completed" << std::endl;
        
        // 델파이 테스트와 동일한 구독 메시지
        std::string subscribe_msg = R"({"method": "SUBSCRIPTION","params": ["spot@public.aggre.deals.v3.api.pb@100ms@BTCUSDT"]})";
        
        ws.write(net::buffer(subscribe_msg));
        std::cout << "Sent: " << subscribe_msg << std::endl;
        
        // 응답 읽기
        beast::flat_buffer buffer;
        ws.read(buffer);
        std::string response = beast::buffers_to_string(buffer.data());
        std::cout << "Received: " << response << std::endl;
        
        // 몇 개 더 읽어보기
        for (int i = 0; i < 5; ++i) {
            buffer.consume(buffer.size());
            ws.read(buffer);
            
            // 바이너리 체크
            std::string data = beast::buffers_to_string(buffer.data());
            bool is_binary = false;
            for (unsigned char c : data) {
                if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
                    is_binary = true;
                    break;
                }
            }
            
            if (is_binary) {
                std::cout << "Received binary data (length: " << data.length() << ")" << std::endl;
            } else {
                std::cout << "Received: " << data.substr(0, 100) << std::endl;
            }
        }
        
        // 정리
        ws.close(websocket::close_code::normal);
        
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}