#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <ctime>
#include <string>
#include <iomanip>

// ANSI color codes
#define GREEN "\033[32m"
#define RED "\033[31m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define RESET "\033[0m"

struct RateData {
    double rate;
    std::string source;
    std::string timestamp;
    bool valid;
};

RateData read_fx_rate() {
    RateData data = {0.0, "", "", false};
    
    std::ifstream file("/tmp/usdkrw_rate.json");
    if (!file.is_open()) {
        return data;
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();
    
    // Simple JSON parsing
    size_t rate_pos = content.find("\"rate\":");
    size_t source_pos = content.find("\"source\":");
    size_t timestamp_pos = content.find("\"timestamp\":");
    
    if (rate_pos != std::string::npos && source_pos != std::string::npos) {
        // Extract rate
        size_t rate_start = rate_pos + 8;  // "rate": 다음
        while (rate_start < content.length() && content[rate_start] == ' ') rate_start++;
        size_t rate_end = content.find_first_of(",}", rate_start);
        std::string rate_str = content.substr(rate_start, rate_end - rate_start);
        data.rate = std::stod(rate_str);
        
        // Extract source
        size_t source_start = source_pos + 10;  // "source": " 다음
        while (source_start < content.length() && content[source_start] == ' ') source_start++;
        if (content[source_start] == '"') source_start++;
        size_t source_end = content.find("\"", source_start);
        data.source = content.substr(source_start, source_end - source_start);
        
        // Extract timestamp
        if (timestamp_pos != std::string::npos) {
            size_t ts_start = timestamp_pos + 13;  // "timestamp": " 다음
            while (ts_start < content.length() && content[ts_start] == ' ') ts_start++;
            if (content[ts_start] == '"') ts_start++;
            size_t ts_end = content.find("\"", ts_start);
            data.timestamp = content.substr(ts_start, ts_end - ts_start);
        }
        
        data.valid = true;
    }
    
    return data;
}

int main() {
    std::cout << BLUE << "=== USD/KRW 환율 테스트 ===" << RESET << std::endl;
    std::cout << "파일 위치: /tmp/usdkrw_rate.json\n" << std::endl;
    
    double last_rate = 0.0;
    int count = 0;
    
    while (true) {
        auto data = read_fx_rate();
        
        // 현재 시간
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto* tm = std::localtime(&time_t);
        
        std::cout << "[" << std::put_time(tm, "%H:%M:%S") << "] ";
        
        if (data.valid) {
            // 환율 변화 확인
            if (last_rate > 0 && data.rate != last_rate) {
                double change = data.rate - last_rate;
                if (change > 0) {
                    std::cout << GREEN << "▲ ";
                } else {
                    std::cout << RED << "▼ ";
                }
                std::cout << "변화: " << std::abs(change) << " " << RESET;
            }
            
            std::cout << "환율: " << YELLOW << data.rate << RESET 
                      << " KRW/USD | 소스: " << data.source;
            
            if (data.source.find("investing.com") != std::string::npos) {
                std::cout << GREEN << " [실시간]" << RESET;
            } else {
                std::cout << RED << " [고정]" << RESET;
            }
            
            std::cout << std::endl;
            
            last_rate = data.rate;
        } else {
            std::cout << RED << "파일 읽기 실패 또는 크롤러가 실행중이 아님" << RESET << std::endl;
        }
        
        count++;
        if (count % 6 == 0) {
            std::cout << "----------------------------------------" << std::endl;
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    
    return 0;
}