● 프로세스 아키텍처 (8개 프로세스)                                                   
                                                                                       
  프로세스 구조                                                                        
                                                                                       
  Watchdog (메인 프로세스, 전체 관리)                                                  
  ├── Priority 0: Feeders (동시 시작)                                                  
  │   ├── upbit-feeder                                                                 
  │   ├── bithumb-feeder                                                               
  │   ├── binance-feeder                                                               
  │   └── mexc-feeder                                                                  
  ├── Priority 1: arb-engine (feeder 후 1초 대기)                                      
  ├── Priority 2: Cold Path (engine 후 500ms 대기)                                     
  │   ├── order-manager                                                                
  │   └── risk-manager                                                                 
  └── Priority 3: monitor (cold 후 500ms 대기)                                         
                                                                                       
  ---
  각 프로세스 역할                                                                     
                                                            
  ┌─────┬───────────────┬────────────────────────────────────────────────┬──────────┐
  │  #  │   프로세스    │                      역할                      │ Critical │  
  ├─────┼───────────────┼────────────────────────────────────────────────┼──────────┤
  │ 1-4 │ feeder (x4)   │ 거래소 WebSocket 연결 → 시세 수신 → SHM 큐에   │ Yes      │  
  │     │               │ Ticker 기록                                    │          │
  ├─────┼───────────────┼────────────────────────────────────────────────┼──────────┤  
  │ 5   │ arb-engine    │ Hot Thread(Core1)에서 4개 SHM 폴링 → 김프 계산 │ Yes      │
  │     │               │  → 기회 감지 → 주문 큐잉                       │          │  
  ├─────┼───────────────┼────────────────────────────────────────────────┼──────────┤
  │ 6   │ order-manager │ SHM에서 주문 요청 수신 → REST API로 양방향     │ Yes      │  
  │     │               │ 매매 실행 → 결과 전송                          │          │  
  ├─────┼───────────────┼────────────────────────────────────────────────┼──────────┤
  │ 7   │ risk-manager  │ 일일 손익 추적, 손실 한도 초과 시 킬스위치     │ No       │  
  │     │               │ 발동                                           │          │  
  ├─────┼───────────────┼────────────────────────────────────────────────┼──────────┤
  │ 8   │ monitor       │ 텔레메트리 수집, CLI(TCP 8080) 제공, 통계/알림 │ No       │  
  │     │               │  관리                                          │          │  
  └─────┴───────────────┴────────────────────────────────────────────────┴──────────┘
                                                                                       
  IPC 채널                                                  

  [Feeders] ──SHM SPSC──> [Engine] ──SHM OrderChannel──> [Order Manager]
                                                                │                      
                                                       Unix Socket                     
                                                                ↓                      
                                                         [Risk Manager]                
                                                                                       
  [All Processes] ──Unix Socket──> [Monitor] ──TCP 8080──> [CLI Tool]
                                                                                       
  - SHM (공유메모리): /kimchi_feed_{exchange}, /kimchi_order_request,                  
  /kimchi_order_result
  - Unix Socket: /tmp/kimchi_risk.sock, /tmp/kimchi_monitor.sock                       
  - TCP: port 8080 (CLI 접속용)                             
                                                                                       
  ---
  서버 시작                                                                            
                                                            
  main() (src/main.cpp)이 Watchdog 모드로 실행되면 자동으로 8개 프로세스를 순서대로
  기동합니다.                                                                          
  
  # 빌드                                                                               
  cd build && cmake .. && make -j$(nproc)                   

  # 실행 (Watchdog가 8개 프로세스 자동 기동)                                           
  ./arbitrage                # standalone 모드 (단일 프로세스)
  ./arbitrage --engine       # engine 모드 (Watchdog가 관리하는 자식으로 실행)         
                                                                                       
  기동 타임라인:                                                                       
  T=0ms     4개 feeder fork+exec (동시)                                                
  T=1000ms  arb-engine fork+exec                                                       
  T=1500ms  order-manager + risk-manager fork+exec (동시)                              
  T=2000ms  monitor fork+exec                                                          
                                                                                       
  Watchdog는 이후 monitor_loop()에서 1초마다 모든 자식 프로세스를 waitpid(WNOHANG)로   
  감시하고, 비정상 종료 시 자동 재시작합니다 (최대 10회/1시간).                        
  
  ---                                                                                  
  서버 종료                                                 

  Graceful Shutdown (정상 종료):

  # 방법 1: Ctrl+C (SIGINT)                                                            
  # 방법 2: kill 명령
  kill <watchdog_pid>        # SIGTERM 전송                                            
                                                            
  종료 순서 (기동의 역순):                                                             
  1. monitor        (start_order 3) → SIGTERM 전송, 5초 대기
  2. order-manager  (start_order 2) → SIGTERM 전송, 5초 대기                           
     risk-manager   (start_order 2)                                                    
  3. arb-engine     (start_order 1) → SIGTERM 전송, 5초 대기
  4. 4개 feeder     (start_order 0) → SIGTERM 전송, 5초 대기                           
                                                            
  각 프로세스는 SIGTERM 수신 시 running_ = false 설정 → 메인 루프 탈출 → 리소스 정리 → 
  종료.                                                                                
                                                                                       
  강제 종료: SIGTERM 후 타임아웃 내 미종료 시 SIGKILL 전송. Ctrl+C 두 번 누르면 즉시   
  _Exit().                   