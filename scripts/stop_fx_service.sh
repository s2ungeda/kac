#!/bin/bash
# FX 환율 서비스 종료 스크립트

echo "=== FX 환율 서비스 종료 ==="

# 프로세스 종료
echo "워치독 종료 중..."
pkill -f fx_watchdog.py

echo "크롤러 종료 중..."
pkill -f fx_selenium_crawler.py

echo "크롬 프로세스 정리 중..."
pkill chrome
pkill chromedriver

sleep 2

# 상태 확인
if pgrep -f fx_selenium_crawler.py > /dev/null; then
    echo "⚠️  크롤러가 아직 실행 중입니다. 강제 종료..."
    pkill -9 -f fx_selenium_crawler.py
fi

if pgrep -f fx_watchdog.py > /dev/null; then
    echo "⚠️  워치독이 아직 실행 중입니다. 강제 종료..."
    pkill -9 -f fx_watchdog.py
fi

echo ""
echo "✅ FX 환율 서비스가 종료되었습니다."