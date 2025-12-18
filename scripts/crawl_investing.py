#!/usr/bin/env python3
"""
Investing.com USD/KRW crawler with anti-detection
"""
import json
import sys
import os
import time
import random
from datetime import datetime
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC
import undetected_chromedriver as uc
from anti_detect import AntiDetect

class InvestingCrawler:
    def __init__(self, headless=True):
        self.anti_detect = AntiDetect()
        self.driver = None
        self.headless = headless
        self.last_request_time = 0
        self.request_count = 0
        
    def setup_driver(self):
        """Chrome 드라이버 설정"""
        options = uc.ChromeOptions()
        
        # 기본 옵션
        options.add_argument('--disable-blink-features=AutomationControlled')
        options.add_argument('--disable-dev-shm-usage')
        options.add_argument('--no-sandbox')
        options.add_argument('--disable-gpu')
        
        # 랜덤 User-Agent
        user_agent = self.anti_detect.get_random_user_agent()
        options.add_argument(f'--user-agent={user_agent}')
        
        # 랜덤 뷰포트
        width, height = self.anti_detect.get_random_viewport_size()
        options.add_argument(f'--window-size={width},{height}')
        
        # 언어 설정
        options.add_argument('--lang=ko-KR')
        
        # Headless 모드
        if self.headless:
            options.add_argument('--headless=new')  # Chrome 109+ 새로운 headless
        
        # WebRTC 비활성화
        prefs = {
            'webrtc.ip_handling_policy': 'disable_non_proxied_udp',
            'webrtc.multiple_routes_enabled': False,
            'webrtc.nonproxied_udp_enabled': False
        }
        options.add_experimental_option('prefs', prefs)
        
        # undetected-chromedriver로 생성
        self.driver = uc.Chrome(options=options, version_main=None)
        
        # Stealth 스크립트 추가
        self.anti_detect.add_stealth_scripts(self.driver)
        
        # 타임아웃 설정
        self.driver.set_page_load_timeout(30)
        self.driver.implicitly_wait(10)
        
    def fetch_usdkrw(self):
        """USD/KRW 환율 크롤링"""
        try:
            # 드라이버 초기화 (첫 요청 또는 주기적 재시작)
            if not self.driver or self.request_count % 50 == 0:
                if self.driver:
                    self.driver.quit()
                self.setup_driver()
                time.sleep(2)  # 초기 대기
            
            # 요청 간격 체크
            current_time = time.time()
            elapsed = current_time - self.last_request_time
            if elapsed < 8:  # 최소 8초
                time.sleep(8 - elapsed)
            
            # URL 접속
            url = 'https://www.investing.com/currencies/usd-krw'
            self.driver.get(url)
            
            # 랜덤 대기
            self.anti_detect.random_delay(2, 4)
            
            # 인간같은 행동 시뮬레이션 (30% 확률)
            if random.random() < 0.3:
                self.anti_detect.human_like_scroll(self.driver)
                self.anti_detect.random_delay(0.5, 1.5)
                
                # 가끔 마우스 움직임 (10% 확률)
                if random.random() < 0.1:
                    self.anti_detect.random_mouse_movement(self.driver)
            
            # 환율 데이터 추출 (여러 선택자 시도)
            selectors = [
                '[data-test="instrument-price-last"]',
                '.instrument-price_last__KQzyA',
                'span[class*="instrument-price"]',
                '.pid-650-last'
            ]
            
            rate_text = None
            for selector in selectors:
                try:
                    element = WebDriverWait(self.driver, 5).until(
                        EC.presence_of_element_located((By.CSS_SELECTOR, selector))
                    )
                    rate_text = element.text
                    break
                except:
                    continue
            
            if not rate_text:
                raise Exception("Rate element not found")
            
            # 환율 파싱 (콤마 제거)
            rate = float(rate_text.replace(',', ''))
            
            # 성공 응답
            result = {
                'status': 'success',
                'rate': rate,
                'source': 'investing.com',
                'timestamp': datetime.now().isoformat(),
                'request_count': self.request_count
            }
            
            self.last_request_time = time.time()
            self.request_count += 1
            
            return result
            
        except Exception as e:
            return {
                'status': 'error',
                'message': str(e),
                'timestamp': datetime.now().isoformat()
            }
    
    def cleanup(self):
        """리소스 정리"""
        if self.driver:
            self.driver.quit()

def main():
    """메인 실행 함수"""
    # Headless 모드 설정 (환경변수로 제어 가능)
    headless = os.environ.get('CRAWLER_HEADLESS', 'true').lower() == 'true'
    
    crawler = InvestingCrawler(headless=headless)
    
    try:
        # 단일 요청 모드
        if len(sys.argv) > 1 and sys.argv[1] == '--single':
            result = crawler.fetch_usdkrw()
            print(json.dumps(result))
        
        # 연속 실행 모드 (테스트용)
        else:
            while True:
                result = crawler.fetch_usdkrw()
                print(json.dumps(result), flush=True)
                
                # 다음 요청까지 랜덤 대기
                interval = crawler.anti_detect.random_request_interval()
                time.sleep(interval)
                
    except KeyboardInterrupt:
        pass
    finally:
        crawler.cleanup()

if __name__ == '__main__':
    main()