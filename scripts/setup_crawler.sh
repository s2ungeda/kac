#!/bin/bash
# Selenium 크롤러 설정 스크립트

echo "=== Investing.com Crawler Setup ==="

# Python 패키지 설치
echo "1. Installing Python packages..."
pip3 install -r scripts/requirements.txt

# Chrome/Chromium 확인
echo "2. Checking Chrome installation..."
if ! command -v google-chrome &> /dev/null && ! command -v chromium &> /dev/null; then
    echo "Chrome/Chromium not found. Please install:"
    echo "  sudo apt-get install chromium-browser"
    exit 1
fi

# 권한 설정
echo "3. Setting permissions..."
chmod +x scripts/crawl_investing.py
chmod +x scripts/anti_detect.py

# 테스트 실행
echo "4. Testing crawler..."
cd scripts
python3 crawl_investing.py --single

echo "Setup complete!"