// 조건 요약:
// 1) "OPEN" 수신 → 즉시 열림
// 2) 마지막 OPEN 이후 3초 동안 OPEN이 더 안 오면 닫힘 로직 가동:
//    - 리드 LOW(자석 감지=닫힘)면 즉시 닫힘
//    - 리드 HIGH(열림)면 OPEN을 계속 무시하면서, 리드가 LOW로 바뀐 뒤 1초 지난 시점에 닫힘
//    - 닫힌 뒤에는 다시 OPEN 신호를 기다리는 상태로 복귀
//
// 안정화 장치:
// - motorBusy + 동작 후 LOCKOUT 1.5s (왕복 방지)
// - 액티브 브레이크(EN=HIGH + IN1=IN2=HIGH)
// - 리드 스위치 디바운스
// - PWM 적정치로 낮추고(과격 반동 억제) 시간 약간 증가

#include <SoftwareSerial.h>

// ===== 핀 맵 =====
const int EN  = 5;   // L298N ENA (PWM)
const int IN1 = 2;   // L298N IN1
const int IN2 = 4;   // L298N IN2
const int DOOR = 9;  // 리드 스위치 (INPUT_PULLUP: LOW=자석 감지=문 닫힘)

SoftwareSerial bt(7, 6); // HC-06: TX->D7, RX<-D6(분압 권장)

// ===== 파라미터(환경에 맞게 미세조정) =====
const int OPEN_PWM  = 210;    // 180~230 권장
const int OPEN_MS   = 850;    // 기구에 맞춰 700~1200
const int CLOSE_PWM = 210;
const int CLOSE_MS  = 950;

const int  BRAKE_MS                 = 200;
const unsigned long SENSOR_DEBOUNCE_MS          = 150;
const unsigned long OPEN_SIGNAL_TIMEOUT_MS      = 3000; // 마지막 OPEN 이후 3초
const unsigned long AFTER_REED_CLOSE_DELAY_MS   = 1000; // 리드 HIGH->LOW 전환 후 1초 뒤 닫힘
const unsigned long MOVE_LOCKOUT_MS             = 1500; // 모션 종료 후 명령 무시

// ===== 상태 변수 =====
volatile bool motorBusy = false;
unsigned long lastMoveEndAt = 0;           // 마지막 모션 종료 시각
unsigned long lastOpenAt    = 0;           // 마지막 OPEN 수신 시각(0이면 아직 없음)

bool reedLowStable = false;                // 안정화된 리드 상태(LOW=자석 감지=닫힘)
unsigned long reedChangedAt = 0;           // 리드 상태 변화 시각(디바운스)

bool pendingCloseUntilReedLow = false;     // 리드가 LOW로 바뀔 때까지 OPEN 무시/대기
unsigned long reedLowDetectedAt = 0;       // (pending 중) LOW 안정 감지 시각

const int DEADTIME_MS = 25;

// ===== 모터 제어 유틸 =====
static inline void motorCoast() {
  analogWrite(EN, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

static inline void motorBrake() {
  // 액티브 브레이크: EN=HIGH + IN1=IN2=HIGH
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, HIGH);
  analogWrite(EN, 255);
  delay(BRAKE_MS);
  analogWrite(EN, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

static inline void openLock() {
  if (motorBusy) { Serial.println("OPEN IGNORED: busy"); return; }
  if (millis() - lastMoveEndAt < MOVE_LOCKOUT_MS) { Serial.println("OPEN IGNORED: lockout"); return; }

  motorBusy = true;
  Serial.println("ACTION: OPEN");
  digitalWrite(IN1, HIGH);  // 정방향
  digitalWrite(IN2, LOW);
  analogWrite(EN, OPEN_PWM);
  delay(OPEN_MS);
  motorBrake();
  lastMoveEndAt = millis();
  motorBusy = false;
  Serial.println("DONE: OPEN");
}

static inline void closeLock() {
  if (motorBusy) { Serial.println("CLOSE IGNORED: busy"); return; }
  if (millis() - lastMoveEndAt < MOVE_LOCKOUT_MS) { Serial.println("CLOSE IGNORED: lockout"); return; }

  motorBusy = true;
  Serial.println("ACTION: CLOSE");
  digitalWrite(IN1, LOW);   // 역방향
  digitalWrite(IN2, HIGH);
  analogWrite(EN, CLOSE_PWM);
  delay(CLOSE_MS);
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

  if (raw != lastRaw) {
    lastRaw = raw;
    reedChangedAt = now;
  }

  if (now - reedChangedAt >= SENSOR_DEBOUNCE_MS) {
    bool st = (raw == LOW);
    reedLowStable = st;
  }
}

// ===== 명령 파서: CR/LF 구분, OPEN만 처리 =====
void parseStream(Stream& s) {
  static String buf;
  while (s.available()) {
    char c = s.read();
    if (c == '\r' || c == '\n') {
      buf.trim();
      if (buf.length()) {
        String cmd = buf; cmd.toUpperCase();
        if (cmd == "OPEN") {
          // pending 상태(리드 LOW 대기) 동안에는 OPEN 무시
          if (!pendingCloseUntilReedLow) {
            openLock();
            lastOpenAt = millis();       // 마지막 OPEN 시각 갱신
          } else {
            Serial.println("OPEN IGNORED: pending (waiting reed LOW)");
          }
        }
      }
      buf = "";
    } else {
      buf += c;
      if (buf.length() > 32) buf.remove(0, buf.length()-32);
    }
  }
}

// ===== 기본 Arduino 루틴 =====
void setup() {
  Serial.begin(9600);
  bt.begin(9600);

  pinMode(EN, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  motorCoast();

  pinMode(DOOR, INPUT_PULLUP);
  updateReedStable();

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
      // 3-2) 리드 HIGH(열림) → LOW로 바뀔 때까지 OPEN 무시, LOW 전환 + 1초 후 닫기
      if (!pendingCloseUntilReedLow) {
        Serial.println("AUTO: enter pending; ignore OPEN until reed LOW");
      }
      pendingCloseUntilReedLow = true;

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
        // 아직 HIGH면 타이머 리셋
        reedLowDetectedAt = 0;
      }
    }
  }

  // 4) 안전 유휴 (드라이버를 불필요하게 켜두지 않기)
  if (!motorBusy) motorCoast();
}
