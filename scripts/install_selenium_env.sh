#!/bin/bash

# Selenium 환경 설치 스크립트

echo "=== Selenium 환경 설치 시작 ==="
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 1. pip3 설치 확인
echo "1. pip3 확인..."
if ! command -v pip3 &> /dev/null; then
    echo -e "${YELLOW}pip3가 없습니다. 설치를 시도합니다...${NC}"
    echo "sudo apt-get install python3-pip"
    echo -e "${RED}수동으로 설치해주세요: sudo apt-get install python3-pip${NC}"
    exit 1
fi
echo -e "${GREEN}✓ pip3 확인됨${NC}"

# 2. Selenium 설치
echo ""
echo "2. Selenium 패키지 설치..."
pip3 install --user selenium
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Selenium 설치 완료${NC}"
else
    echo -e "${RED}✗ Selenium 설치 실패${NC}"
fi

# 3. Chrome/Chromium 설치 안내
echo ""
echo "3. Chrome/Chromium 브라우저"
if ! command -v chromium-browser &> /dev/null && ! command -v google-chrome &> /dev/null; then
    echo -e "${YELLOW}Chrome이 설치되지 않았습니다.${NC}"
    echo ""
    echo "다음 명령어로 설치하세요:"
    echo -e "${GREEN}sudo apt update${NC}"
    echo -e "${GREEN}sudo apt install -y chromium-browser${NC}"
else
    echo -e "${GREEN}✓ Chrome/Chromium 확인됨${NC}"
fi

# 4. ChromeDriver 자동 다운로드 및 설치
echo ""
echo "4. ChromeDriver 다운로드 및 설치..."

# 임시 디렉토리 생성
TEMP_DIR=$(mktemp -d)
cd $TEMP_DIR

# ChromeDriver 다운로드 (최신 안정 버전)
echo "ChromeDriver 다운로드 중..."
CHROME_DRIVER_VERSION="114.0.5735.90"
wget -q -O chromedriver.zip "https://chromedriver.storage.googleapis.com/${CHROME_DRIVER_VERSION}/chromedriver_linux64.zip"

if [ $? -eq 0 ]; then
    # 압축 해제
    unzip -q chromedriver.zip
    
    # 실행 권한 부여
    chmod +x chromedriver
    
    # 사용자 로컬 bin으로 이동
    mkdir -p $HOME/.local/bin
    mv chromedriver $HOME/.local/bin/
    
    echo -e "${GREEN}✓ ChromeDriver 설치 완료: $HOME/.local/bin/chromedriver${NC}"
    
    # PATH에 추가 안내
    if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
        echo ""
        echo -e "${YELLOW}PATH에 추가가 필요합니다:${NC}"
        echo 'export PATH="$HOME/.local/bin:$PATH"'
        echo ""
        echo "~/.bashrc에 추가하려면:"
        echo 'echo '\''export PATH="$HOME/.local/bin:$PATH"'\'' >> ~/.bashrc'
        echo 'source ~/.bashrc'
    fi
else
    echo -e "${RED}✗ ChromeDriver 다운로드 실패${NC}"
    echo "수동으로 다운로드: https://chromedriver.chromium.org/downloads"
fi

# 임시 디렉토리 정리
cd - > /dev/null
rm -rf $TEMP_DIR

# 5. 설치 확인
echo ""
echo "=== 설치 확인 ==="

echo -n "Selenium: "
if python3 -c "import selenium" 2>/dev/null; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}FAILED${NC}"
fi

echo -n "ChromeDriver: "
if [ -f "$HOME/.local/bin/chromedriver" ]; then
    echo -e "${GREEN}OK${NC} ($HOME/.local/bin/chromedriver)"
elif command -v chromedriver &> /dev/null; then
    echo -e "${GREEN}OK${NC} ($(which chromedriver))"
else
    echo -e "${RED}NOT FOUND${NC}"
fi

echo ""
echo "=== 설치 완료 ==="
echo ""
echo "다음 단계:"
echo "1. Chrome 설치 (필요시): sudo apt install chromium-browser"
echo "2. PATH 설정 (필요시): source ~/.bashrc"
echo "3. Selenium 크롤러 시작: ./scripts/start_selenium_crawler.sh"