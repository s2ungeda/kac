#!/usr/bin/env python3
"""
독립 실행형 환율 크롤러 - 파일로 결과 저장
"""
import json
import time
import logging
import os
from datetime import datetime
from pathlib import Path
import fcntl
import random

# 환율 데이터 파일 경로
FX_DATA_FILE = "/tmp/usdkrw_rate.json"
FX_LOCK_FILE = "/tmp/usdkrw_rate.lock"

class StandaloneFXCrawler:
    def __init__(self):
        self.logger = logging.getLogger('FXCrawler')
        self.setup_logging()
        
    def setup_logging(self):
        """로깅 설정"""
        log_dir = Path.home() / "kimchi-arbitrage-cpp" / "logs"
        log_dir.mkdir(exist_ok=True)
        
        handler = logging.FileHandler(log_dir / "fx_crawler.log")
        formatter = logging.Formatter('%(asctime)s - %(levelname)s - %(message)s')
        handler.setFormatter(formatter)
        
        self.logger.addHandler(handler)
        self.logger.setLevel(logging.INFO)
        
    def fetch_from_investing(self):
        """investing.com에서 실시간 환율 크롤링 - 정규식만 사용"""
        try:
            import requests
            import re
            
            headers = {
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
                'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8',
                'Accept-Language': 'ko-KR,ko;q=0.9,en-US;q=0.8,en;q=0.7',
                'Accept-Encoding': 'gzip, deflate, br',
                'DNT': '1',
                'Connection': 'keep-alive',
                'Upgrade-Insecure-Requests': '1'
            }
            
            response = requests.get('https://www.investing.com/currencies/usd-krw', 
                                  headers=headers, timeout=10)
            
            if response.status_code == 200:
                # HTML에서 직접 패턴 매칭
                patterns = [
                    r'"last":([0-9,]+\.?[0-9]*)',
                    r'data-test="instrument-price-last"[^>]*>([0-9,]+\.?[0-9]*)<',
                    r'data-value="([0-9,]+\.?[0-9]*)"',
                    r'class="[^"]*instrument-price[^"]*"[^>]*>([0-9,]+\.?[0-9]*)<',
                    r'>([0-9]{1,3}(?:,[0-9]{3})*(?:\.[0-9]+)?)</span>'
                ]
                
                for pattern in patterns:
                    match = re.search(pattern, response.text)
                    if match:
                        rate_text = match.group(1).replace(',', '')
                        try:
                            rate = float(rate_text)
                            if 1000 < rate < 2000:  # 합리적 범위
                                self.logger.info(f"Got rate from investing.com: {rate}")
                                return rate, "investing.com"
                        except:
                            continue
                            
        except Exception as e:
            self.logger.warning(f"Failed to fetch from investing.com: {e}")
            
        return None, None
    
    def fetch_from_api(self):
        """API에서 환율 가져오기 (백업)"""
        sources = [
            {
                "name": "exchangerate-api",
                "url": "https://api.exchangerate-api.com/v4/latest/USD",
                "parser": lambda data: data.get('rates', {}).get('KRW')
            },
            {
                "name": "fixer.io (demo)",
                "url": "http://data.fixer.io/api/latest?access_key=demo&symbols=KRW&base=USD",
                "parser": lambda data: data.get('rates', {}).get('KRW')
            }
        ]
        
        import requests
        for source in sources:
            try:
                response = requests.get(source['url'], timeout=5)
                if response.status_code == 200:
                    data = response.json()
                    rate = source['parser'](data)
                    if rate:
                        return rate, source['name']
            except Exception as e:
                self.logger.warning(f"Failed to fetch from {source['name']}: {e}")
                
        return None, None
        
    def write_rate_data(self, rate, source):
        """환율 데이터를 파일에 저장 (원자적 쓰기)"""
        data = {
            "rate": rate,
            "source": source,
            "timestamp": datetime.now().isoformat(),
            "timestamp_unix": time.time()
        }
        
        # 임시 파일에 먼저 쓰기
        tmp_file = f"{FX_DATA_FILE}.tmp"
        try:
            with open(tmp_file, 'w') as f:
                json.dump(data, f)
            
            # 원자적으로 이동
            os.rename(tmp_file, FX_DATA_FILE)
            
            self.logger.info(f"Saved rate: {rate} from {source}")
            
        except Exception as e:
            self.logger.error(f"Failed to save rate: {e}")
            if os.path.exists(tmp_file):
                os.remove(tmp_file)
                
    def run(self):
        """메인 루프"""
        self.logger.info("FX Crawler started")
        
        while True:
            try:
                # 먼저 investing.com 시도
                rate, source = self.fetch_from_investing()
                
                # investing.com 실패시 API 백업
                if not rate:
                    rate, source = self.fetch_from_api()
                
                if rate:
                    self.write_rate_data(rate, source)
                else:
                    self.logger.error("Failed to fetch rate from all sources")
                
                # 8-13초 랜덤 대기
                sleep_time = random.uniform(8, 13)
                time.sleep(sleep_time)
                
            except KeyboardInterrupt:
                self.logger.info("Crawler stopped by user")
                break
            except Exception as e:
                self.logger.error(f"Unexpected error: {e}")
                time.sleep(10)

if __name__ == "__main__":
    # 데몬으로 실행하려면:
    # nohup python3 fx_crawler_standalone.py > /dev/null 2>&1 &
    
    crawler = StandaloneFXCrawler()
    crawler.run()