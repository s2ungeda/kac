#!/usr/bin/env python3
"""
FX Rate Service - 별도 프로세스로 실행되는 환율 크롤링 서비스
"""
import json
import socket
import threading
import time
import logging
from datetime import datetime
from dataclasses import dataclass
from typing import Optional
import signal
import sys
import os

# Selenium 관련 임포트는 나중에
try:
    from selenium import webdriver
    from selenium.webdriver.common.by import By
    from selenium.webdriver.support.ui import WebDriverWait
    from selenium.webdriver.support import expected_conditions as EC
    import undetected_chromedriver as uc
    SELENIUM_AVAILABLE = True
except ImportError:
    SELENIUM_AVAILABLE = False

@dataclass
class FXRate:
    rate: float
    timestamp: str
    source: str
    error: Optional[str] = None

class FXRateService:
    def __init__(self, port=9516):
        self.port = port
        self.running = False
        self.server_socket = None
        self.driver = None
        
        # 캐시
        self.cached_rate = None
        self.cache_lock = threading.Lock()
        self.last_update = 0
        self.update_interval = 10  # 10초
        
        # 로깅 설정
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )
        self.logger = logging.getLogger('FXService')
        
        # 크롤링 통계
        self.request_count = 0
        self.success_count = 0
        self.error_count = 0
        
    def setup_driver(self):
        """Chrome 드라이버 설정"""
        if not SELENIUM_AVAILABLE:
            return None
            
        options = uc.ChromeOptions()
        options.add_argument('--headless')
        options.add_argument('--no-sandbox')
        options.add_argument('--disable-dev-shm-usage')
        options.add_argument('--disable-blink-features=AutomationControlled')
        options.add_argument('--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36')
        
        try:
            self.driver = uc.Chrome(options=options)
            self.logger.info("Chrome driver initialized")
            return True
        except Exception as e:
            self.logger.error(f"Failed to init Chrome: {e}")
            return False
    
    def crawl_investing(self) -> Optional[float]:
        """investing.com에서 환율 크롤링"""
        if not self.driver:
            return None
            
        try:
            self.driver.get('https://www.investing.com/currencies/usd-krw')
            
            # 여러 선택자 시도
            selectors = [
                '[data-test="instrument-price-last"]',
                '.instrument-price_last__KQzyA',
                '.pid-650-last'
            ]
            
            rate_element = None
            for selector in selectors:
                try:
                    rate_element = WebDriverWait(self.driver, 5).until(
                        EC.presence_of_element_located((By.CSS_SELECTOR, selector))
                    )
                    break
                except:
                    continue
                    
            if rate_element:
                rate_text = rate_element.text.replace(',', '')
                return float(rate_text)
                
        except Exception as e:
            self.logger.error(f"Crawling error: {e}")
            
        return None
    
    def fetch_fallback(self) -> Optional[float]:
        """Fallback API 사용"""
        try:
            import requests
            response = requests.get(
                'https://api.exchangerate-api.com/v4/latest/USD',
                timeout=5
            )
            data = response.json()
            return data.get('rates', {}).get('KRW')
        except:
            return None
    
    def update_rate(self):
        """환율 업데이트 (자동 갱신용)"""
        self.request_count += 1
        
        # Selenium 크롤링 시도
        rate = None
        source = "unknown"
        
        if SELENIUM_AVAILABLE and self.driver:
            rate = self.crawl_investing()
            source = "investing.com"
        
        # Fallback
        if not rate:
            rate = self.fetch_fallback()
            source = "exchangerate-api"
        
        if rate:
            self.success_count += 1
            with self.cache_lock:
                self.cached_rate = FXRate(
                    rate=rate,
                    timestamp=datetime.now().isoformat(),
                    source=source
                )
                self.last_update = time.time()
            self.logger.info(f"Rate updated: {rate} from {source}")
        else:
            self.error_count += 1
            self.logger.error("Failed to fetch rate from all sources")
    
    def handle_client(self, client_socket, address):
        """클라이언트 요청 처리"""
        try:
            # 요청 수신
            data = client_socket.recv(1024).decode('utf-8').strip()
            
            if data == "GET_RATE":
                # 캐시 확인
                current_time = time.time()
                with self.cache_lock:
                    if (not self.cached_rate or 
                        current_time - self.last_update > self.update_interval):
                        # 캐시 만료, 업데이트
                        self.update_rate()
                    
                    if self.cached_rate:
                        response = {
                            "status": "success",
                            "rate": self.cached_rate.rate,
                            "source": self.cached_rate.source,
                            "timestamp": self.cached_rate.timestamp
                        }
                    else:
                        response = {
                            "status": "error",
                            "message": "No rate available"
                        }
                        
            elif data == "STATS":
                response = {
                    "status": "success",
                    "stats": {
                        "requests": self.request_count,
                        "success": self.success_count,
                        "errors": self.error_count,
                        "uptime": int(time.time() - self.start_time)
                    }
                }
                
            elif data == "SHUTDOWN":
                self.running = False
                response = {"status": "success", "message": "Shutting down"}
                
            else:
                response = {"status": "error", "message": f"Unknown command: {data}"}
            
            # 응답 전송
            client_socket.send(json.dumps(response).encode('utf-8'))
            
        except Exception as e:
            self.logger.error(f"Client handler error: {e}")
            error_response = {"status": "error", "message": str(e)}
            client_socket.send(json.dumps(error_response).encode('utf-8'))
        finally:
            client_socket.close()
    
    def auto_update_thread(self):
        """자동 업데이트 스레드"""
        while self.running:
            try:
                self.update_rate()
                # 8-13초 랜덤 대기
                import random
                time.sleep(random.uniform(8, 13))
            except Exception as e:
                self.logger.error(f"Auto update error: {e}")
                time.sleep(10)
    
    def start(self):
        """서비스 시작"""
        self.running = True
        self.start_time = time.time()
        
        # Chrome 드라이버 초기화
        if SELENIUM_AVAILABLE:
            self.setup_driver()
        else:
            self.logger.warning("Selenium not available, using fallback APIs only")
        
        # TCP 서버 소켓 생성
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.bind(('localhost', self.port))
        self.server_socket.listen(5)
        
        self.logger.info(f"FX Service listening on port {self.port}")
        
        # 자동 업데이트 스레드 시작
        update_thread = threading.Thread(target=self.auto_update_thread)
        update_thread.daemon = True
        update_thread.start()
        
        # 클라이언트 요청 처리
        while self.running:
            try:
                self.server_socket.settimeout(1.0)
                client_socket, address = self.server_socket.accept()
                
                # 각 클라이언트를 별도 스레드로 처리
                client_thread = threading.Thread(
                    target=self.handle_client,
                    args=(client_socket, address)
                )
                client_thread.daemon = True
                client_thread.start()
                
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    self.logger.error(f"Server error: {e}")
    
    def stop(self):
        """서비스 종료"""
        self.running = False
        
        if self.server_socket:
            self.server_socket.close()
            
        if self.driver:
            self.driver.quit()
            
        self.logger.info("FX Service stopped")

def signal_handler(sig, frame):
    """시그널 핸들러"""
    print("\nShutting down FX Service...")
    sys.exit(0)

def main():
    """메인 함수"""
    # 시그널 핸들러 등록
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # 포트 설정 (환경변수 또는 기본값)
    port = int(os.environ.get('FX_SERVICE_PORT', '9516'))
    
    # 서비스 시작
    service = FXRateService(port=port)
    
    try:
        service.start()
    except KeyboardInterrupt:
        pass
    finally:
        service.stop()

if __name__ == '__main__':
    main()