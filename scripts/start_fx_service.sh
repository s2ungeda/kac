#!/bin/bash
# FX 환율 서비스 시작 스크립트

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="${SCRIPT_DIR}/../logs"

echo "=== FX 환율 서비스 시작 ==="

# 1. 기존 프로세스 정리
echo "기존 프로세스 정리 중..."
pkill -f fx_selenium_crawler.py 2>/dev/null
pkill -f fx_watchdog.py 2>/dev/null
sleep 2

# 2. 크롬 프로세스 정리
echo "크롬 프로세스 정리 중..."
pkill chrome 2>/dev/null
pkill chromedriver 2>/dev/null
sleep 1

# 3. 크롤러 시작
echo "크롤러 시작 중..."
nohup python3 "${SCRIPT_DIR}/fx_selenium_crawler.py" > "${LOG_DIR}/fx_selenium.out" 2>&1 &
CRAWLER_PID=$!
echo "크롤러 PID: $CRAWLER_PID"

# 4. 크롤러 시작 대기
sleep 5

# 5. 워치독 시작
echo "워치독 시작 중..."
nohup python3 "${SCRIPT_DIR}/fx_watchdog.py" > "${LOG_DIR}/fx_watchdog.out" 2>&1 &
WATCHDOG_PID=$!
echo "워치독 PID: $WATCHDOG_PID"

# 6. 상태 확인
sleep 3
echo ""
echo "=== 서비스 상태 ==="

if ps -p $CRAWLER_PID > /dev/null; then
    echo "✅ 크롤러: 실행 중 (PID: $CRAWLER_PID)"
else
    echo "❌ 크롤러: 실행 실패"
fi

if ps -p $WATCHDOG_PID > /dev/null; then
    echo "✅ 워치독: 실행 중 (PID: $WATCHDOG_PID)"
else
    echo "❌ 워치독: 실행 실패"
fi

# 7. 환율 파일 확인
if [ -f "/tmp/usdkrw_rate.json" ]; then
    echo ""
    echo "=== 현재 환율 ==="
    cat /tmp/usdkrw_rate.json | python3 -m json.tool
fi

echo ""
echo "로그 확인:"
echo "  tail -f ${LOG_DIR}/fx_selenium_*.log"
echo "  tail -f ${LOG_DIR}/fx_watchdog_*.log"