#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <string>

int main() {
    std::cout << "=== 간단한 환율 테스트 ===" << std::endl;
    
    while (true) {
        std::ifstream file("/tmp/usdkrw_rate.json");
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
            file.close();
            
            // 간단한 파싱
            size_t rate_pos = content.find("\"rate\":");
            size_t source_pos = content.find("\"source\":");
            
            if (rate_pos != std::string::npos && source_pos != std::string::npos) {
                // rate 추출
                size_t rate_start = rate_pos + 8;  // "rate": 다음
                while (rate_start < content.length() && content[rate_start] == ' ') rate_start++;
                size_t rate_end = content.find(",", rate_start);
                std::string rate_str = content.substr(rate_start, rate_end - rate_start);
                
                // source 추출  
                size_t source_start = source_pos + 10;  // "source": " 다음
                while (source_start < content.length() && content[source_start] == ' ') source_start++;
                if (content[source_start] == '"') source_start++;
                size_t source_end = content.find("\"", source_start);
                std::string source = content.substr(source_start, source_end - source_start);
                
                std::cout << "환율: " << rate_str << " KRW/USD | 소스: " << source << std::endl;
            } else {
                std::cout << "파싱 실패: " << content << std::endl;
            }
        } else {
            std::cout << "파일 열기 실패" << std::endl;
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    
    return 0;
}