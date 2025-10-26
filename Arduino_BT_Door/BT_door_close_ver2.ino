#include <SoftwareSerial.h>

// ===== 핀 맵 =====
const int EN  = 5;   // L298N ENA (PWM 제어)
const int IN1 = 2;   // L298N IN1 (방향 제어 1)
const int IN2 = 4;   // L298N IN2 (방향 제어 2)
const int DOOR = 9;  // 리드 스위치 (LOW=자석 감지=문 닫힘)
SoftwareSerial bt(7, 6); // BT-04: TX->D7, RX<-D6

// ===== 파라미터(환경에 맞게 미세조정) =====
// PWM 세기(모터 회전 세기)
const int OPEN_PWM  = 210;
const int CLOSE_PWM = 210;
// 모터 회전 시간(도어락 작동 시간)
const int OPEN_MS   = 850;
const int CLOSE_MS  = 950;

const int  BRAKE_MS                 = 200; // 모터 제동 시간(브레이크 유지 시간)
const unsigned long SENSOR_DEBOUNCE_MS          = 150; (리드 스위치 안정화 시간)
const unsigned long OPEN_SIGNAL_TIMEOUT_MS      = 3000; // 마지막 OPEN 이후 3초 후 닫힘
const unsigned long AFTER_REED_CLOSE_DELAY_MS   = 1000; // 리드 HIGH->LOW 전환 후 1초 후 닫힘
const unsigned long MOVE_LOCKOUT_MS             = 1500; // 모션 종료 후 명령 무시

// ===== 상태 변수 =====
volatile bool motorBusy = false;           // 모터의 현재 상태(회전 중인지)
unsigned long lastMoveEndAt = 0;           // 마지막 모션 종료 시각
unsigned long lastOpenAt    = 0;           // 마지막 OPEN 수신 시각(0이면 아직 없음)

bool reedLowStable = false;                // 안정화된 리드 상태(LOW=자석 감지=닫힘)
unsigned long reedChangedAt = 0;           // 리드 상태 변화 시각(디바운스)

bool pendingCloseUntilReedLow = false;     // 닫힘 대기 모드인지(OPEN 이후 3초 -> 리드 HIGH)
unsigned long reedLowDetectedAt = 0;       // 닫힘 대기 중(리드 LOW로 바뀐 순간 시각 : 1초 지연)

// ===== 모터 제어 유틸 =====
static inline void motorCoast() { // 모터 정지
  analogWrite(EN, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

static inline void motorBrake() { // 모터 제동 -> 위치 고정
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, HIGH);
  analogWrite(EN, 255);
  
  delay(BRAKE_MS);
  
  analogWrite(EN, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}
// 모터 구동(open/close)
static inline void openLock() {
  // 모터 회전 중 또는 마지막 동작 후 1.5초가 안지났다면 무시
  if (motorBusy) { Serial.println("OPEN IGNORED: busy"); return; }
  if (millis() - lastMoveEndAt < MOVE_LOCKOUT_MS) { Serial.println("OPEN IGNORED: lockout"); return; }

  // 모터 회전 중...
  motorBusy = true;
  Serial.println("ACTION: OPEN");
  // 정방향
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  // PWM 속도 제어
  analogWrite(EN, OPEN_PWM);
  // 일정 시간동안 회전
  delay(OPEN_MS);
  // 모터 회전 종료... 상태 기록
  motorBrake();
  lastMoveEndAt = millis();
  motorBusy = false;
  
  Serial.println("DONE: OPEN");
}
static inline void closeLock() {
  // 모터 회전 중 또는 마지막 동작 후 1.5초가 안지났다면 무시
  if (motorBusy) { Serial.println("CLOSE IGNORED: busy"); return; }
  if (millis() - lastMoveEndAt < MOVE_LOCKOUT_MS) { Serial.println("CLOSE IGNORED: lockout"); return; }

  // 모터 회전 중...
  motorBusy = true;
  Serial.println("ACTION: CLOSE");
  // 역방향
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  // PWM 속도 제어
  analogWrite(EN, CLOSE_PWM);
  // 일정 시간동안 회전
  delay(CLOSE_MS);
  // 모터 회전 종료... 상태 기록
  motorBrake();
  lastMoveEndAt = millis();
  motorBusy = false;
  
  Serial.println("DONE: CLOSE");
}

// ===== 리드 스위치 디바운스 갱신 =====
void updateReedStable() {
  static int lastRaw = HIGH;
  int raw = digitalRead(DOOR); // LOW=자석 감지(닫힘), HIGH=열림
  unsigned long now = millis();

  // 이전 상태와 다르다면 시간 저장
  if (raw != lastRaw) {
    lastRaw = raw;
    reedChangedAt = now;
  }
  // 변화 후 일정시간(0.5초)이 경과했는지 확인
  if (now - reedChangedAt >= SENSOR_DEBOUNCE_MS) {
    reedLowStable = (raw == LOW); // 안정된 상태(LOW) 업데이트
  }
}

// ===== 명령 파서: CR/LF 구분, OPEN만 처리 =====
void parseStream(Stream& s) {
  static String buf;
  while (s.available()) { // 시리얼 버퍼에 데이터가 있는 경우
    char c = s.read();
    if (c == '\r' || c == '\n') { // 엔터(줄바꿈)시 명령 확정
      buf.trim();
      if (buf.length()) {
        String cmd = buf; cmd.toUpperCase();
        if (cmd == "OPEN") { // OPEN 이면 도어락 열기
          // 리드 LOW(도어락은 열렸지만 실제 문을 열지 않은 경우) 대기 동안에는 OPEN 무시
          if (!pendingCloseUntilReedLow) {
            openLock();
            lastOpenAt = millis();       // 마지막 OPEN 시각 갱신
          } else {
            Serial.println("OPEN IGNORED: pending (waiting reed LOW)");
          }
        }
      }
      buf = ""; // 버퍼 초기화
    } else { // 버퍼에 명령 누적(32자까지)
      buf += c;
      if (buf.length() > 32) buf.remove(0, buf.length()-32);
    }
  }
}

// ===== 기본 Arduino 루틴 =====
void setup() {
  Serial.begin(9600); // USB 시리얼 초기화
  bt.begin(9600); // 블루투스 시리얼 초기화

  // 제어 핀들 출력 모드 설정
  pinMode(EN, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  // 일단 모터 정지
  motorCoast();
  // 리드 스위치 입력 핀 설정 -> 초기 리드 스위치 상태(LOW)로 초기화
  pinMode(DOOR, INPUT_PULLUP);
  updateReedStable();
  // 나머지 변수 초기화
  lastOpenAt = 0;
  pendingCloseUntilReedLow = false;
  reedLowDetectedAt = 0;

  Serial.println("READY");
}

void loop() {
  // 1) 입력 파싱 (USB/BT)
  parseStream(Serial);
  parseStream(bt);

  // 2) 리드 스위치 안정 상태 갱신
  updateReedStable();

  // 3) 닫힘 로직: "마지막 OPEN 이후 3초 동안 OPEN이 더 안 온 경우"에만 동작
  const unsigned long now = millis();
  const bool openTimedOut = (lastOpenAt > 0) && (now - lastOpenAt > OPEN_SIGNAL_TIMEOUT_MS);

  if (openTimedOut) {
    if (reedLowStable) {
      // 3-1) 이미 리드 LOW(닫힘) → 즉시 닫기
      Serial.println("AUTO: CLOSE immediately (timeout & reed LOW)");
      closeLock();
      // 모든 상태 초기화 → 다음 OPEN 대기
      pendingCloseUntilReedLow = false;
      reedLowDetectedAt = 0;
      lastOpenAt = 0;
    } else {
      // 3-2) 리드 HIGH(열림) → LOW로 바뀔 때까지 OPEN 무시
      if (!pendingCloseUntilReedLow) {
        Serial.println("AUTO: enter pending; ignore OPEN until reed LOW");
      }
      pendingCloseUntilReedLow = true;
      // 3-3) LOW 안정화 -> 1초 후 닫기(closeLock)
      if (reedLowStable) {
        if (reedLowDetectedAt == 0) {
          reedLowDetectedAt = now;
          Serial.println("AUTO: reed LOW detected, start 1s timer");
        }
        if (now - reedLowDetectedAt >= AFTER_REED_CLOSE_DELAY_MS) {
          Serial.println("AUTO: CLOSE after 1s (reed HIGH->LOW)");
          closeLock();
          // 초기화 → 다음 OPEN 대기
          pendingCloseUntilReedLow = false;
          reedLowDetectedAt = 0;
          lastOpenAt = 0;
        }
      } else {
        // 3-4) 아직 HIGH면 타이머 리셋
        reedLowDetectedAt = 0;
      }
    }
  }

  // 4) 안전 유휴 (드라이버를 불필요하게 켜두지 않기)
  if (!motorBusy) motorCoast();
}
