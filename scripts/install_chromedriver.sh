#!/bin/bash

echo "=== ChromeDriver Installation Script ==="

# 시스템 정보 확인
OS=$(uname -s)
ARCH=$(uname -m)

# Chrome/Chromium 설치 확인
echo "1. Checking for Chrome/Chromium..."
if command -v google-chrome &> /dev/null; then
    CHROME_VERSION=$(google-chrome --version | grep -oP '\d+\.\d+\.\d+')
    echo "Found Google Chrome: $CHROME_VERSION"
elif command -v chromium-browser &> /dev/null; then
    CHROME_VERSION=$(chromium-browser --version | grep -oP '\d+\.\d+\.\d+')
    echo "Found Chromium: $CHROME_VERSION"
elif command -v chromium &> /dev/null; then
    CHROME_VERSION=$(chromium --version | grep -oP '\d+\.\d+\.\d+')
    echo "Found Chromium: $CHROME_VERSION"
else
    echo "Chrome/Chromium not found. Installing Chromium..."
    sudo apt-get update
    sudo apt-get install -y chromium-browser
    CHROME_VERSION=$(chromium-browser --version | grep -oP '\d+\.\d+\.\d+')
fi

# Chrome 메이저 버전 추출
CHROME_MAJOR=$(echo $CHROME_VERSION | cut -d. -f1)
echo "Chrome major version: $CHROME_MAJOR"

# ChromeDriver 버전 확인
echo -e "\n2. Getting ChromeDriver version for Chrome $CHROME_MAJOR..."
CHROMEDRIVER_VERSION=$(curl -s "https://chromedriver.storage.googleapis.com/LATEST_RELEASE_$CHROME_MAJOR")

if [ -z "$CHROMEDRIVER_VERSION" ]; then
    echo "Could not determine ChromeDriver version. Using latest stable..."
    CHROMEDRIVER_VERSION=$(curl -s "https://chromedriver.storage.googleapis.com/LATEST_RELEASE")
fi

echo "ChromeDriver version: $CHROMEDRIVER_VERSION"

# 아키텍처 확인 및 다운로드 URL 설정
if [[ "$ARCH" == "x86_64" ]]; then
    CHROMEDRIVER_URL="https://chromedriver.storage.googleapis.com/$CHROMEDRIVER_VERSION/chromedriver_linux64.zip"
elif [[ "$ARCH" == "aarch64" ]]; then
    CHROMEDRIVER_URL="https://chromedriver.storage.googleapis.com/$CHROMEDRIVER_VERSION/chromedriver_linux64.zip"
else
    echo "Unsupported architecture: $ARCH"
    exit 1
fi

# ChromeDriver 다운로드 및 설치
echo -e "\n3. Downloading ChromeDriver..."
cd /tmp
wget -q -O chromedriver.zip "$CHROMEDRIVER_URL"

if [ $? -ne 0 ]; then
    echo "Failed to download ChromeDriver"
    exit 1
fi

# 압축 해제
echo "4. Installing ChromeDriver..."
unzip -q chromedriver.zip
chmod +x chromedriver

# /usr/local/bin으로 이동
sudo mv chromedriver /usr/local/bin/
sudo chown root:root /usr/local/bin/chromedriver
sudo chmod 755 /usr/local/bin/chromedriver

# 정리
rm chromedriver.zip

# 설치 확인
echo -e "\n5. Verifying installation..."
if command -v chromedriver &> /dev/null; then
    INSTALLED_VERSION=$(chromedriver --version | grep -oP '\d+\.\d+\.\d+')
    echo "✓ ChromeDriver $INSTALLED_VERSION installed successfully at: $(which chromedriver)"
else
    echo "✗ ChromeDriver installation failed"
    exit 1
fi

# 필요한 라이브러리 설치
echo -e "\n6. Installing required libraries..."
sudo apt-get install -y \
    libnss3 \
    libnspr4 \
    libatk1.0-0 \
    libatk-bridge2.0-0 \
    libcups2 \
    libdrm2 \
    libxkbcommon0 \
    libxcomposite1 \
    libxdamage1 \
    libxrandr2 \
    libgbm1 \
    libgtk-3-0 \
    libasound2

echo -e "\n=== Installation Complete ==="
echo "Chrome version: $CHROME_VERSION"
echo "ChromeDriver version: $CHROMEDRIVER_VERSION"
echo ""
echo "You can now use ChromeDriver with C++ or any other language."