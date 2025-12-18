#!/usr/bin/env python3
"""
Selenium 기반 investing.com 환율 크롤러
Python4Delphi 코드를 기반으로 수정
"""
import time
import logging
import os
import json
import random
from pathlib import Path
from datetime import datetime

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.chrome.service import Service
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.chrome.options import Options
from selenium.common.exceptions import TimeoutException, WebDriverException

# --- 설정부 ---
TARGET_URL = 'https://kr.investing.com/currencies/usd-krw-chart'
FX_DATA_FILE = "/tmp/usdkrw_rate.json"
LOG_PATH = Path.home() / "kimchi-arbitrage-cpp" / "logs"
WAIT_TIMEOUT = 10      # 요소 대기 시간 (줄여서 빠른 실패 감지)

# XPath 패턴들
RATE_XPATHS = [
    '//*[@data-test="instrument-price-last"]',
    '//*[contains(@class, "text-5xl")]',
    '//span[contains(@class, "instrument-price")]',
    '//*[@class="pid-650-last"]'
]

class SeleniumFXCrawler:
    def __init__(self):
        self.logger = self.setup_logging()
        self.driver = None
        self.current_xpath = RATE_XPATHS[0]
        
    def setup_logging(self):
        """로깅 설정"""
        LOG_PATH.mkdir(exist_ok=True)
        
        logger = logging.getLogger("FXSeleniumCrawler")
        logger.setLevel(logging.INFO)
        
        # 날짜별 로그 파일
        today = time.strftime("%Y-%m-%d")
        log_file = LOG_PATH / f'fx_selenium_{today}.log'
        
        handler = logging.FileHandler(log_file, encoding='utf-8')
        formatter = logging.Formatter("%(asctime)s - %(levelname)s - %(message)s")
        handler.setFormatter(formatter)
        logger.addHandler(handler)
        
        return logger
    
    def create_driver(self):
        """크롬 드라이버 초기화"""
        options = Options()
        options.add_argument('--headless')
        options.add_argument('--no-sandbox')
        options.add_argument('--disable-dev-shm-usage')
        options.add_argument('--disable-gpu')
        options.add_argument('--incognito')
        
        # Snap Chromium 관련 추가 옵션
        options.add_argument('--disable-setuid-sandbox')
        options.add_argument('--remote-debugging-port=9222')
        options.add_argument('--disable-extensions')
        options.add_argument('--disable-background-timer-throttling')
        options.add_argument('--disable-backgrounding-occluded-windows')
        options.add_argument('--disable-renderer-backgrounding')
        
        # 봇 탐지 방지
        options.add_argument('--disable-blink-features=AutomationControlled')
        options.add_experimental_option("excludeSwitches", ["enable-automation"])
        options.add_experimental_option('useAutomationExtension', False)
        
        # User-Agent 설정
        user_agents = [
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36"
        ]
        options.add_argument(f"user-agent={random.choice(user_agents)}")
        
        try:
            # ChromeDriver 자동 설치는 webdriver-manager 필요
            # 수동 설치된 경우 직접 경로 지정
            # Snap으로 설치된 Chromium의 경우
            from selenium.webdriver.chrome.service import Service
            try:
                # webdriver-manager 사용 (설치되어 있다면)
                from webdriver_manager.chrome import ChromeDriverManager
                service = Service(ChromeDriverManager().install())
                self.driver = webdriver.Chrome(service=service, options=options)
            except:
                # webdriver-manager가 없으면 시스템 ChromeDriver 사용
                self.driver = webdriver.Chrome(options=options)
            
            # JavaScript로 webdriver 속성 숨기기
            self.driver.execute_cdp_cmd('Page.addScriptToEvaluateOnNewDocument', {
                'source': '''
                    Object.defineProperty(navigator, 'webdriver', {
                        get: () => undefined
                    });
                '''
            })
            
            self.logger.info("Chrome driver created successfully")
            return True
            
        except Exception as e:
            self.logger.error(f"Failed to create driver: {e}")
            return False
    
    def extract_rate(self):
        """환율 추출"""
        for xpath in RATE_XPATHS:
            try:
                # 더 짧은 타임아웃으로 시도
                element = WebDriverWait(self.driver, WAIT_TIMEOUT).until(
                    EC.presence_of_element_located((By.XPATH, xpath))
                )
                
                # 텍스트 정제
                rate_text = element.text.replace(',', '').strip()
                rate = float(rate_text)
                
                if 1000 < rate < 2000:  # 합리적 범위 체크
                    self.current_xpath = xpath  # 성공한 XPath 기억
                    return rate
                    
            except TimeoutException:
                self.logger.debug(f"Timeout with XPath {xpath}")
                continue
            except Exception as e:
                self.logger.warning(f"Error with XPath {xpath}: {e}")
                
        return None
    
    def write_rate_data(self, rate):
        """환율 데이터를 파일에 저장"""
        data = {
            "rate": rate,
            "source": "investing.com (selenium)",
            "timestamp": datetime.now().isoformat(),
            "timestamp_unix": time.time()
        }
        
        # 원자적 쓰기
        tmp_file = f"{FX_DATA_FILE}.tmp"
        try:
            with open(tmp_file, 'w') as f:
                json.dump(data, f)
            
            os.rename(tmp_file, FX_DATA_FILE)
            self.logger.info(f"Saved rate: {rate} from investing.com")
            
        except Exception as e:
            self.logger.error(f"Failed to save rate: {e}")
            if os.path.exists(tmp_file):
                os.remove(tmp_file)
    
    def run(self):
        """메인 실행 루프"""
        self.logger.info("Selenium FX Crawler started")
        
        if not self.create_driver():
            return
        
        try:
            # 페이지 로드
            self.driver.get(TARGET_URL)
            self.logger.info(f"Loaded page: {TARGET_URL}")
            
            # 초기 대기 (페이지 완전 로드)
            time.sleep(3)
            
            iteration = 0
            consecutive_failures = 0
            
            while True:
                try:
                    # 환율 추출
                    rate = self.extract_rate()
                    
                    if rate:
                        self.write_rate_data(rate)
                        consecutive_failures = 0
                        
                        timestamp = time.strftime('%H:%M:%S')
                        print(f"[{iteration:04d}] {timestamp} | Rate: {rate}")
                    else:
                        consecutive_failures += 1
                        self.logger.warning("Failed to extract rate")
                        
                        # 5번 연속 실패시 페이지 새로고침
                        if consecutive_failures >= 5:
                            self.logger.info("Refreshing page due to consecutive failures")
                            self.driver.refresh()
                            time.sleep(5)
                            consecutive_failures = 0
                    
                    # 10초 고정 대기
                    sleep_time = 10
                    time.sleep(sleep_time)
                    
                    iteration += 1
                    
                    # 100회마다 페이지 새로고침 (메모리 누수 방지)
                    if iteration % 100 == 0:
                        self.logger.info("Periodic page refresh")
                        self.driver.refresh()
                        time.sleep(5)
                        
                except KeyboardInterrupt:
                    self.logger.info("Crawler stopped by user")
                    break
                except Exception as e:
                    self.logger.error(f"Loop error: {e}")
                    time.sleep(10)
                    
        except Exception as e:
            self.logger.error(f"Critical error: {e}")
        finally:
            if self.driver:
                self.driver.quit()
                self.logger.info("Driver closed")

def main():
    crawler = SeleniumFXCrawler()
    crawler.run()

if __name__ == "__main__":
    main()