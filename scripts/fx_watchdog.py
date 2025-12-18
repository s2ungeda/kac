#!/usr/bin/env python3
"""
FX 환율 크롤러 워치독
- 크롤러가 종료되면 자동으로 재시작
- 데이터가 오래되면 알림 및 재시작
- 시스템 모니터링
"""

import time
import json
import os
import subprocess
import sys
import logging
from datetime import datetime, timedelta
from pathlib import Path

# 설정
FX_DATA_FILE = "/tmp/usdkrw_rate.json"
CRAWLER_SCRIPT = str(Path.home() / "kimchi-arbitrage-cpp" / "scripts" / "fx_selenium_crawler.py")
LOG_PATH = Path.home() / "kimchi-arbitrage-cpp" / "logs"
MAX_DATA_AGE_SECONDS = 30  # 30초 이상 오래된 데이터는 문제로 간주
CHECK_INTERVAL = 10  # 10초마다 체크

class FXWatchdog:
    def __init__(self):
        self.logger = self.setup_logging()
        self.crawler_pid = None
        
    def setup_logging(self):
        """로깅 설정"""
        LOG_PATH.mkdir(exist_ok=True)
        log_file = LOG_PATH / f"fx_watchdog_{datetime.now().strftime('%Y-%m-%d')}.log"
        
        logger = logging.getLogger('FXWatchdog')
        logger.setLevel(logging.INFO)
        
        handler = logging.FileHandler(log_file)
        handler.setFormatter(logging.Formatter('%(asctime)s - %(levelname)s - %(message)s'))
        logger.addHandler(handler)
        
        # 콘솔 출력도 추가
        console_handler = logging.StreamHandler()
        console_handler.setFormatter(logging.Formatter('[%(asctime)s] %(message)s'))
        logger.addHandler(console_handler)
        
        return logger
    
    def is_crawler_running(self):
        """크롤러가 실행 중인지 확인"""
        try:
            result = subprocess.run(
                ['pgrep', '-f', 'fx_selenium_crawler.py'],
                capture_output=True,
                text=True
            )
            if result.stdout.strip():
                pids = result.stdout.strip().split('\n')
                self.crawler_pid = int(pids[0])
                return True
            return False
        except Exception as e:
            self.logger.error(f"Failed to check crawler status: {e}")
            return False
    
    def check_data_freshness(self):
        """데이터의 최신성 확인"""
        try:
            if not os.path.exists(FX_DATA_FILE):
                self.logger.warning("FX data file not found")
                return False, None
            
            with open(FX_DATA_FILE, 'r') as f:
                data = json.load(f)
            
            timestamp_unix = data.get('timestamp_unix', 0)
            current_time = time.time()
            age = current_time - timestamp_unix
            
            if age > MAX_DATA_AGE_SECONDS:
                self.logger.warning(f"Data is {age:.1f} seconds old (limit: {MAX_DATA_AGE_SECONDS}s)")
                return False, age
            
            return True, age
            
        except Exception as e:
            self.logger.error(f"Failed to check data: {e}")
            return False, None
    
    def start_crawler(self):
        """크롤러 시작"""
        try:
            # 기존 프로세스 종료
            subprocess.run(['pkill', '-f', 'fx_selenium_crawler.py'], stderr=subprocess.DEVNULL)
            time.sleep(2)
            
            # 크롬 프로세스 정리
            subprocess.run(['pkill', 'chrome'], stderr=subprocess.DEVNULL)
            time.sleep(1)
            
            # 크롤러 시작
            cmd = [
                'nohup',
                'python3',
                CRAWLER_SCRIPT,
                '>', str(LOG_PATH / 'fx_selenium.out'),
                '2>&1',
                '&'
            ]
            
            # 쉘 명령으로 실행
            shell_cmd = f"nohup python3 {CRAWLER_SCRIPT} > {LOG_PATH}/fx_selenium.out 2>&1 &"
            subprocess.Popen(shell_cmd, shell=True)
            
            time.sleep(5)  # 시작 대기
            
            if self.is_crawler_running():
                self.logger.info(f"Crawler started successfully (PID: {self.crawler_pid})")
                return True
            else:
                self.logger.error("Failed to start crawler")
                return False
                
        except Exception as e:
            self.logger.error(f"Failed to start crawler: {e}")
            return False
    
    def run(self):
        """메인 실행 루프"""
        self.logger.info("FX Watchdog started")
        print("=== FX 환율 크롤러 워치독 시작 ===")
        print(f"- 데이터 파일: {FX_DATA_FILE}")
        print(f"- 최대 데이터 나이: {MAX_DATA_AGE_SECONDS}초")
        print(f"- 체크 간격: {CHECK_INTERVAL}초")
        print("=====================================\n")
        
        consecutive_failures = 0
        
        while True:
            try:
                # 크롤러 상태 확인
                crawler_running = self.is_crawler_running()
                
                # 데이터 최신성 확인
                data_fresh, data_age = self.check_data_freshness()
                
                # 상태 출력
                status = []
                if crawler_running:
                    status.append(f"크롤러 실행중(PID:{self.crawler_pid})")
                else:
                    status.append("크롤러 중지됨")
                    
                if data_fresh:
                    status.append(f"데이터 최신({data_age:.1f}초)")
                else:
                    status.append(f"데이터 오래됨({data_age:.1f}초)" if data_age else "데이터 없음")
                
                print(f"[{datetime.now().strftime('%H:%M:%S')}] {' | '.join(status)}")
                
                # 문제가 있으면 재시작
                need_restart = False
                
                if not crawler_running:
                    self.logger.warning("Crawler not running, will restart")
                    need_restart = True
                elif not data_fresh:
                    consecutive_failures += 1
                    if consecutive_failures >= 3:  # 3회 연속 실패시
                        self.logger.warning(f"Data stale for {consecutive_failures} checks, will restart")
                        need_restart = True
                else:
                    consecutive_failures = 0
                
                if need_restart:
                    self.logger.info("Restarting crawler...")
                    if self.start_crawler():
                        consecutive_failures = 0
                        print("✅ 크롤러 재시작 성공")
                    else:
                        print("❌ 크롤러 재시작 실패")
                
                # 대기
                time.sleep(CHECK_INTERVAL)
                
            except KeyboardInterrupt:
                self.logger.info("Watchdog stopped by user")
                print("\n워치독 종료")
                break
            except Exception as e:
                self.logger.error(f"Watchdog error: {e}")
                time.sleep(CHECK_INTERVAL)

def main():
    watchdog = FXWatchdog()
    watchdog.run()

if __name__ == "__main__":
    main()