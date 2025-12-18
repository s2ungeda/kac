#!/bin/bash

# Selenium FX Crawler 시작 스크립트

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$HOME/kimchi-arbitrage-cpp/logs"
PID_FILE="/tmp/fx_selenium_crawler.pid"

# 로그 디렉토리 생성
mkdir -p "$LOG_DIR"

# 이미 실행 중인지 확인
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if ps -p "$OLD_PID" > /dev/null 2>&1; then
        echo "Selenium FX Crawler is already running (PID: $OLD_PID)"
        exit 0
    fi
fi

echo "Starting Selenium FX Crawler..."
echo "NOTE: Make sure Chrome/Chromium and ChromeDriver are installed!"

# Python 크롤러 백그라운드 실행
cd "$SCRIPT_DIR/.."
nohup python3 scripts/fx_selenium_crawler.py > "$LOG_DIR/fx_selenium.out" 2>&1 &
PID=$!

# PID 저장
echo $PID > "$PID_FILE"

echo "Selenium FX Crawler started (PID: $PID)"
echo "Logs: $LOG_DIR/fx_selenium_*.log"
echo ""
echo "To stop: kill $PID"
echo "To check logs: tail -f $LOG_DIR/fx_selenium_$(date +%Y-%m-%d).log"