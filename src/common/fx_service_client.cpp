#include "arbitrage/common/logger.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>

namespace arbitrage {

class FXServiceClient {
private:
    std::shared_ptr<Logger> logger_;
    std::string host_ = "127.0.0.1";
    int port_ = 9516;
    int socket_fd_ = -1;
    
public:
    FXServiceClient(int port = 9516) 
        : logger_(Logger::create("FXClient"))
        , port_(port) {
    }
    
    ~FXServiceClient() {
        disconnect();
    }
    
    bool connect() {
        // 소켓 생성
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            logger_->error("Failed to create socket");
            return false;
        }
        
        // 서버 주소 설정
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);
        server_addr.sin_addr.s_addr = inet_addr(host_.c_str());
        
        // 연결 시도 (타임아웃 5초)
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        if (::connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            logger_->error("Failed to connect to FX service on port {}", port_);
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        
        logger_->info("Connected to FX service");
        return true;
    }
    
    void disconnect() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }
    
    Result<double> get_rate() {
        // 연결 확인
        if (socket_fd_ < 0 && !connect()) {
            return Err<double>(ErrorCode::NetworkError, "Not connected to FX service");
        }
        
        // 요청 전송
        const char* request = "GET_RATE";
        if (send(socket_fd_, request, strlen(request), 0) < 0) {
            disconnect();
            return Err<double>(ErrorCode::NetworkError, "Failed to send request");
        }
        
        // 응답 수신
        char buffer[1024] = {0};
        int bytes_received = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            disconnect();
            return Err<double>(ErrorCode::NetworkError, "Failed to receive response");
        }
        
        // JSON 파싱 (간단한 구현)
        std::string response(buffer);
        
        // status 체크
        if (response.find("\"status\":\"success\"") == std::string::npos) {
            return Err<double>(ErrorCode::ApiError, "FX service returned error");
        }
        
        // rate 추출
        size_t rate_pos = response.find("\"rate\":");
        if (rate_pos == std::string::npos) {
            return Err<double>(ErrorCode::ParseError, "Rate not found in response");
        }
        
        size_t start = rate_pos + 7;
        size_t end = response.find_first_of(",}", start);
        std::string rate_str = response.substr(start, end - start);
        
        try {
            double rate = std::stod(rate_str);
            
            // source 추출 (옵션)
            size_t source_pos = response.find("\"source\":\"");
            if (source_pos != std::string::npos) {
                size_t source_start = source_pos + 10;
                size_t source_end = response.find("\"", source_start);
                std::string source = response.substr(source_start, source_end - source_start);
                logger_->info("Got rate {} from {}", rate, source);
            }
            
            return Ok(rate);
            
        } catch (const std::exception& e) {
            return Err<double>(ErrorCode::ParseError, "Failed to parse rate");
        }
    }
    
    Result<std::string> get_stats() {
        if (socket_fd_ < 0 && !connect()) {
            return Err<std::string>(ErrorCode::NetworkError, "Not connected");
        }
        
        const char* request = "STATS";
        if (send(socket_fd_, request, strlen(request), 0) < 0) {
            disconnect();
            return Err<std::string>(ErrorCode::NetworkError, "Failed to send request");
        }
        
        char buffer[1024] = {0};
        int bytes_received = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            disconnect();
            return Err<std::string>(ErrorCode::NetworkError, "Failed to receive response");
        }
        
        return Ok(std::string(buffer));
    }
    
    bool shutdown_service() {
        if (socket_fd_ < 0 && !connect()) {
            return false;
        }
        
        const char* request = "SHUTDOWN";
        send(socket_fd_, request, strlen(request), 0);
        disconnect();
        return true;
    }
};

// Python 서비스 프로세스 관리
class FXServiceManager {
private:
    pid_t service_pid_ = -1;
    int port_ = 9516;
    std::shared_ptr<Logger> logger_;
    
public:
    FXServiceManager(int port = 9516)
        : port_(port)
        , logger_(Logger::create("FXManager")) {
    }
    
    ~FXServiceManager() {
        stop();
    }
    
    bool start() {
        service_pid_ = fork();
        
        if (service_pid_ == 0) {
            // 자식 프로세스 - Python 서비스 실행
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%d", port_);
            setenv("FX_SERVICE_PORT", port_str, 1);
            
            execl("/usr/bin/python3", "python3", 
                  "scripts/fx_service.py", nullptr);
            
            // exec 실패 시
            exit(1);
        }
        
        if (service_pid_ < 0) {
            logger_->error("Failed to start FX service");
            return false;
        }
        
        // 서비스 시작 대기
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        // 연결 테스트
        FXServiceClient client(port_);
        if (!client.connect()) {
            logger_->error("Failed to connect to FX service after start");
            stop();
            return false;
        }
        
        logger_->info("FX service started on port {}", port_);
        return true;
    }
    
    void stop() {
        if (service_pid_ > 0) {
            // 정상 종료 요청
            FXServiceClient client(port_);
            client.shutdown_service();
            
            // 프로세스 종료 대기
            int status;
            waitpid(service_pid_, &status, WNOHANG);
            
            // 강제 종료
            kill(service_pid_, SIGTERM);
            waitpid(service_pid_, &status, 0);
            
            service_pid_ = -1;
            logger_->info("FX service stopped");
        }
    }
    
    bool is_running() const {
        if (service_pid_ <= 0) return false;
        
        // 프로세스 상태 확인
        int status;
        pid_t result = waitpid(service_pid_, &status, WNOHANG);
        return result == 0;
    }
};

// FXRateService 통합
static std::unique_ptr<FXServiceManager> g_fx_manager;
static std::unique_ptr<FXServiceClient> g_fx_client;

Result<double> fetch_from_fx_service() {
    // 매니저 초기화
    if (!g_fx_manager) {
        g_fx_manager = std::make_unique<FXServiceManager>();
        if (!g_fx_manager->start()) {
            return Err<double>(ErrorCode::SystemError, "Failed to start FX service");
        }
    }
    
    // 클라이언트 초기화
    if (!g_fx_client) {
        g_fx_client = std::make_unique<FXServiceClient>();
    }
    
    return g_fx_client->get_rate();
}

void cleanup_fx_service() {
    g_fx_client.reset();
    g_fx_manager.reset();
}

} // namespace arbitrage