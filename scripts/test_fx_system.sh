#!/bin/bash

# 환율 시스템 테스트 스크립트

echo "=== 김치 아비트라지 환율 시스템 테스트 ==="
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 1. Python 환경 체크
echo "1. Python 환경 확인..."
echo -n "   Python3: "
if command -v python3 &> /dev/null; then
    echo -e "${GREEN}OK${NC} ($(python3 --version))"
else
    echo -e "${RED}MISSING${NC}"
fi

echo -n "   requests: "
if python3 -c "import requests" 2>/dev/null; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}MISSING${NC} - pip3 install requests"
fi


echo -n "   selenium: "
if python3 -c "import selenium" 2>/dev/null; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${YELLOW}MISSING${NC} - pip3 install selenium (실시간 환율용)"
fi

echo ""

# 2. Chrome/ChromeDriver 체크
echo "2. Chrome/ChromeDriver 확인..."
echo -n "   Chrome/Chromium: "
if command -v google-chrome &> /dev/null || command -v chromium &> /dev/null || command -v chromium-browser &> /dev/null; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${YELLOW}MISSING${NC} - sudo apt-get install chromium-browser (실시간 환율용)"
fi

echo -n "   ChromeDriver: "
if command -v chromedriver &> /dev/null; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${YELLOW}MISSING${NC} - 실시간 환율을 위해 필요"
fi

echo ""

# 3. 크롤러 상태 확인
echo "3. 크롤러 프로세스 확인..."
CRAWLER_PIDS=$(ps aux | grep -E "fx_(crawler|selenium)" | grep -v grep | awk '{print $2}')
if [ -n "$CRAWLER_PIDS" ]; then
    echo -e "   ${GREEN}실행 중${NC} (PID: $CRAWLER_PIDS)"
else
    echo -e "   ${RED}실행 안됨${NC}"
    echo ""
    echo "   크롤러 시작 방법:"
    echo "   ./scripts/start_fx_crawler.sh      # 기본 크롤러"
    echo "   ./scripts/start_selenium_crawler.sh # Selenium 크롤러 (실시간)"
fi

echo ""

# 4. 환율 파일 확인
echo "4. 환율 파일 확인..."
if [ -f "/tmp/usdkrw_rate.json" ]; then
    echo -e "   파일: ${GREEN}존재${NC}"
    
    # 파일 내용 확인
    RATE=$(cat /tmp/usdkrw_rate.json 2>/dev/null | grep -o '"rate":[0-9.]*' | cut -d':' -f2)
    SOURCE=$(cat /tmp/usdkrw_rate.json 2>/dev/null | grep -o '"source":"[^"]*"' | cut -d'"' -f4)
    
    if [ -n "$RATE" ]; then
        echo "   현재 환율: $RATE KRW/USD"
        echo "   데이터 소스: $SOURCE"
        
        # 마지막 업데이트 시간
        LAST_MOD=$(stat -c %y /tmp/usdkrw_rate.json | cut -d'.' -f1)
        echo "   마지막 업데이트: $LAST_MOD"
    fi
else
    echo -e "   파일: ${RED}없음${NC}"
fi

echo ""

# 5. C++ 테스트 프로그램 확인
echo "5. C++ 테스트 프로그램..."
if [ -f "test_fx_rate.cpp" ]; then
    echo "   컴파일 중..."
    if g++ -o test_fx_rate test_fx_rate.cpp -std=c++17 -pthread 2>/dev/null; then
        echo -e "   ${GREEN}컴파일 성공${NC}"
        echo ""
        echo "=== 테스트 시작 방법 ==="
        echo ""
        echo "1) 크롤러가 실행 중이 아니면 시작:"
        echo "   ./scripts/start_fx_crawler.sh"
        echo ""
        echo "2) 별도 터미널에서 C++ 테스트 실행:"
        echo "   ./test_fx_rate"
        echo ""
        echo "3) 로그 확인 (선택사항):"
        echo "   tail -f ~/kimchi-arbitrage-cpp/logs/fx_crawler.log"
    else
        echo -e "   ${RED}컴파일 실패${NC}"
    fi
else
    echo -e "   ${RED}test_fx_rate.cpp 파일 없음${NC}"
fi

echo ""
echo "=== 테스트 끝 ==="