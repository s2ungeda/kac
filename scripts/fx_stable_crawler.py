#!/usr/bin/env python3
"""
안정적인 USD/KRW 환율 크롤러
- 메모리 누수 방지를 위해 주기적으로 드라이버 재시작
- 더 나은 에러 처리
"""

import time
import json
import os
import random
from datetime import datetime
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC
from selenium.common.exceptions import TimeoutException, WebDriverException
from selenium.webdriver.chrome.options import Options
import logging
from pathlib import Path

# 설정
TARGET_URL = 'https://kr.investing.com/currencies/usd-krw-chart'
FX_DATA_FILE = "/tmp/usdkrw_rate.json"
LOG_PATH = Path.home() / "kimchi-arbitrage-cpp" / "logs"
WAIT_TIMEOUT = 5  # 더 짧은 타임아웃
UPDATE_INTERVAL = 10  # 10초마다 업데이트
RESTART_INTERVAL = 20  # 20회 업데이트마다 드라이버 재시작

class StableFXCrawler:
    def __init__(self):
        self.logger = self.setup_logging()
        self.driver = None
        
    def setup_logging(self):
        """로깅 설정"""
        LOG_PATH.mkdir(exist_ok=True)
        log_file = LOG_PATH / f"fx_stable_{datetime.now().strftime('%Y-%m-%d')}.log"
        
        logger = logging.getLogger('FXStableCrawler')
        logger.setLevel(logging.INFO)
        
        handler = logging.FileHandler(log_file)
        handler.setFormatter(logging.Formatter('%(asctime)s - %(levelname)s - %(message)s'))
        logger.addHandler(handler)
        
        return logger
    
    def create_driver(self):
        """크롬 드라이버 생성"""
        try:
            options = Options()
            options.add_argument('--headless')
            options.add_argument('--no-sandbox')
            options.add_argument('--disable-dev-shm-usage')
            options.add_argument('--disable-gpu')
            options.add_argument('--disable-web-security')
            options.add_argument('--disable-features=VizDisplayCompositor')
            options.add_argument('--disable-blink-features=AutomationControlled')
            
            # 메모리 사용 최적화
            options.add_argument('--memory-pressure-off')
            options.add_argument('--disable-background-timer-throttling')
            options.add_argument('--disable-backgrounding-occluded-windows')
            options.add_argument('--disable-renderer-backgrounding')
            
            # User-Agent 설정
            user_agents = [
                'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
                'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
                'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'
            ]
            options.add_argument(f'user-agent={random.choice(user_agents)}')
            
            self.driver = webdriver.Chrome(options=options)
            self.logger.info("Driver created successfully")
            return True
            
        except Exception as e:
            self.logger.error(f"Failed to create driver: {e}")
            return False
    
    def extract_rate(self):
        """여러 방법으로 환율 추출 시도"""
        methods = [
            # 방법 1: data-test 속성
            lambda: self.driver.find_element(By.XPATH, '//*[@data-test="instrument-price-last"]').text,
            # 방법 2: CSS 선택자
            lambda: self.driver.find_element(By.CSS_SELECTOR, 'span[data-test="instrument-price-last"]').text,
            # 방법 3: 클래스 이름으로
            lambda: self.driver.find_element(By.CLASS_NAME, 'text-5xl').text,
            # 방법 4: JavaScript 실행
            lambda: self.driver.execute_script(
                'return document.querySelector(\'[data-test="instrument-price-last"]\')?.innerText || '
                'document.querySelector(".text-5xl")?.innerText'
            )
        ]
        
        for i, method in enumerate(methods):
            try:
                rate_text = method()
                if rate_text:
                    # 쉼표 제거 및 숫자 변환
                    rate = float(rate_text.replace(',', '').strip())
                    if 1000 < rate < 2000:  # 합리적인 범위 확인
                        self.logger.debug(f"Rate extracted using method {i+1}: {rate}")
                        return rate
            except Exception as e:
                self.logger.debug(f"Method {i+1} failed: {str(e)}")
                continue
        
        return None
    
    def save_rate(self, rate):
        """환율 데이터 저장"""
        data = {
            "rate": rate,
            "source": "investing.com (selenium)",
            "timestamp": datetime.now().isoformat(),
            "timestamp_unix": time.time()
        }
        
        # 원자적 쓰기
        tmp_file = f"{FX_DATA_FILE}.tmp"
        with open(tmp_file, 'w') as f:
            json.dump(data, f)
        os.replace(tmp_file, FX_DATA_FILE)
        
        self.logger.info(f"Saved rate: {rate}")
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Rate: {rate} KRW/USD")
    
    def run_session(self):
        """하나의 세션 실행 (드라이버 생성부터 종료까지)"""
        if not self.create_driver():
            return False
        
        try:
            # 페이지 로드
            self.driver.get(TARGET_URL)
            time.sleep(3)  # 페이지 로드 대기
            self.logger.info(f"Loaded page: {TARGET_URL}")
            
            # 지정된 횟수만큼 업데이트 실행
            for i in range(RESTART_INTERVAL):
                try:
                    rate = self.extract_rate()
                    if rate:
                        self.save_rate(rate)
                    else:
                        self.logger.warning("Failed to extract rate")
                        # 실패시 페이지 새로고침
                        self.driver.refresh()
                        time.sleep(3)
                    
                    # 대기
                    time.sleep(UPDATE_INTERVAL)
                    
                except Exception as e:
                    self.logger.error(f"Error in update loop: {e}")
                    time.sleep(5)
            
            return True
            
        except Exception as e:
            self.logger.error(f"Session error: {e}")
            return False
            
        finally:
            # 드라이버 정리
            if self.driver:
                try:
                    self.driver.quit()
                    self.logger.info("Driver closed")
                except:
                    pass
                self.driver = None
    
    def run(self):
        """메인 실행 루프"""
        self.logger.info("Stable FX Crawler started")
        print("=== Stable FX Crawler Started ===")
        
        while True:
            try:
                # 세션 실행
                self.run_session()
                
                # 재시작 전 대기
                self.logger.info("Restarting driver for next session")
                time.sleep(5)
                
            except KeyboardInterrupt:
                self.logger.info("Crawler stopped by user")
                break
            except Exception as e:
                self.logger.error(f"Critical error: {e}")
                time.sleep(10)

def main():
    crawler = StableFXCrawler()
    crawler.run()

if __name__ == "__main__":
    main()