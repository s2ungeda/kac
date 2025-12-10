#!/bin/bash
# Git 설정 스크립트

# 프로젝트별 Git 설정
git config user.email "kimchi-arbitrage@example.com"
git config user.name "Kimchi Arbitrage Developer"

echo "Git configuration set for this repository:"
echo "Email: $(git config user.email)"
echo "Name: $(git config user.name)"