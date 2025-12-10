# Delphi 클라이언트 TCP 통신 가이드

## 프로토콜 개요

### 메시지 구조

```
+----------------+----------------+----------------+
|  Length (4B)   |   Type (1B)    |  Sequence (4B) |
+----------------+----------------+----------------+
|                 Payload (MessagePack)            |
+--------------------------------------------------+
```

### 메시지 타입

```pascal
// Delphi 상수
const
  MSG_SUBSCRIBE     = $01;
  MSG_UNSUBSCRIBE   = $02;
  MSG_PLACE_ORDER   = $03;
  MSG_CANCEL_ORDER  = $04;
  MSG_GET_BALANCE   = $05;
  
  MSG_TICKER_UPDATE = $81;
  MSG_ORDER_UPDATE  = $82;
  MSG_BALANCE_UPDATE = $83;
  MSG_ERROR         = $FF;
```

---

## Delphi 클라이언트 예시

### ICS 사용

```pascal
type
  TArbClient = class
  private
    FSocket: TWSocket;
    FRecvBuffer: TBytes;
    procedure SocketDataAvailable(Sender: TObject; ErrCode: Word);
    procedure SocketSessionConnected(Sender: TObject; ErrCode: Word);
  public
    procedure Connect(const Host: string; Port: Integer);
    procedure Subscribe(const Symbol: string);
    procedure PlaceOrder(const Order: TOrderRequest);
  end;

procedure TArbClient.SocketDataAvailable(Sender: TObject; ErrCode: Word);
var
  Buf: TBytes;
  Len: Integer;
begin
  Len := FSocket.RcvdCount;
  SetLength(Buf, Len);
  FSocket.Receive(@Buf[0], Len);
  
  // 버퍼에 추가
  FRecvBuffer := FRecvBuffer + Buf;
  
  // 완전한 메시지 처리
  ProcessMessages;
end;

procedure TArbClient.Subscribe(const Symbol: string);
var
  Msg: TBytes;
begin
  Msg := BuildMessage(MSG_SUBSCRIBE, PackSymbol(Symbol));
  FSocket.Send(@Msg[0], Length(Msg));
end;
```

---

## 메시지 처리 흐름

```
[C++ Server]                    [Delphi Client]
     |                                |
     |<------- TCP Connect -----------|
     |                                |
     |<------- Subscribe (XRP) -------|
     |                                |
     |-------- Ticker Update -------->|
     |-------- Ticker Update -------->|
     |                                |
     |<------- Place Order -----------|
     |-------- Order Update --------->|
     |                                |
```
