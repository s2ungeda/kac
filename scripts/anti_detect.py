#!/usr/bin/env python3
"""
Anti-detection utilities for web scraping
"""
import random
import time
from selenium.webdriver.common.action_chains import ActionChains
from selenium.webdriver.common.by import By
import pyautogui

class AntiDetect:
    def __init__(self):
        self.user_agents = [
            'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
            'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36',
            'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
            'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'
        ]
    
    def get_random_user_agent(self):
        """랜덤 User-Agent 반환"""
        return random.choice(self.user_agents)
    
    def random_delay(self, min_seconds=0.5, max_seconds=2.0):
        """랜덤 지연"""
        time.sleep(random.uniform(min_seconds, max_seconds))
    
    def human_like_scroll(self, driver):
        """인간처럼 스크롤"""
        scroll_pause = random.uniform(0.5, 1.5)
        
        # 작은 단위로 여러 번 스크롤
        for _ in range(random.randint(2, 5)):
            scroll_amount = random.randint(100, 300)
            driver.execute_script(f"window.scrollBy(0, {scroll_amount})")
            time.sleep(scroll_pause)
    
    def random_mouse_movement(self, driver):
        """랜덤 마우스 움직임"""
        action = ActionChains(driver)
        
        # 화면 내 랜덤 위치로 마우스 이동
        width = driver.execute_script("return window.innerWidth")
        height = driver.execute_script("return window.innerHeight")
        
        for _ in range(random.randint(2, 4)):
            x = random.randint(100, width - 100)
            y = random.randint(100, height - 100)
            
            # 부드러운 곡선 움직임
            action.move_by_offset(x, y)
            self.random_delay(0.1, 0.3)
        
        action.perform()
    
    def add_stealth_scripts(self, driver):
        """JavaScript를 통한 봇 감지 회피"""
        # webdriver 속성 숨기기
        driver.execute_cdp_cmd('Page.addScriptToEvaluateOnNewDocument', {
            'source': '''
                Object.defineProperty(navigator, 'webdriver', {
                    get: () => undefined
                });
                
                // Chrome 관련 속성 숨기기
                window.chrome = {
                    runtime: {}
                };
                
                // Permission 관련 수정
                const originalQuery = window.navigator.permissions.query;
                window.navigator.permissions.query = (parameters) => (
                    parameters.name === 'notifications' ?
                        Promise.resolve({ state: Notification.permission }) :
                        originalQuery(parameters)
                );
                
                // Plugin 배열 수정
                Object.defineProperty(navigator, 'plugins', {
                    get: () => [1, 2, 3, 4, 5]
                });
                
                // Language 설정
                Object.defineProperty(navigator, 'languages', {
                    get: () => ['ko-KR', 'ko', 'en-US', 'en']
                });
            '''
        })
    
    def get_random_viewport_size(self):
        """일반적인 화면 크기 중 랜덤 선택"""
        sizes = [
            (1920, 1080),
            (1366, 768),
            (1440, 900),
            (1536, 864),
            (1280, 720)
        ]
        return random.choice(sizes)
    
    def random_request_interval(self):
        """10초 기준으로 랜덤하게 변동"""
        base_interval = 10
        variation = random.uniform(-2, 3)  # 8초~13초
        return base_interval + variation