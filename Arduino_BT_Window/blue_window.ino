// 창문 아두이노 코드
#include <SoftwareSerial.h>
SoftwareSerial BT(2,3); // HC-05: TX->D2, RX<-D3(분압)

const int IN1=5, IN2=6; // 모터 방향 제어 핀
byte st=0; // 현재 동작상태(0=IDLE,1=OPENING,2=CLOSING)
const unsigned long RUN_MS = 1000; // 모터 회전 시간
unsigned long tStart = 0; // 모터 동작 시간
String state = "CLOSED"; // 현재 상태 저장

void setup(){
  Serial.begin(9600);
  BT.begin(9600);
  
  pinMode(IN1,OUTPUT); 
  pinMode(IN2,OUTPUT);
  
  stopM(); // 처음엔 모터 정지
  Serial.println("READY"); BT.println("READY");
}

void loop(){
  handle(Serial);
  handle(BT);

  // 1초 지나면 동작 완료 → 상태 확정
  if(st!=0 && millis()-tStart >= RUN_MS){
    if(st==1) state="OPENED";
    else      state="CLOSED";
    stopM();

    Serial.print("EVT "); 
    Serial.println(state);
    BT.print("EVT ");    
    BT.println(state);
  }
}

void handle(Stream& io){
  if(!io.available()) return;
  String s=io.readStringUntil('\n'); 
  s.trim();

  if(s.equalsIgnoreCase("OPEN"))  { openM();  io.println("ACK OPEN"); }
  else if(s.equalsIgnoreCase("CLOSE")) { closeM(); io.println("ACK CLOSE"); }
  else if(s.equalsIgnoreCase("STATUS")) printStatus(io);
}

void openM(){  digitalWrite(IN1,HIGH); digitalWrite(IN2,LOW);  st=1; tStart=millis(); }
void closeM(){ digitalWrite(IN1,LOW);  digitalWrite(IN2,HIGH); st=2; tStart=millis(); }
void stopM(){  digitalWrite(IN1,LOW);  digitalWrite(IN2,LOW);  st=0; }

void printStatus(Stream& io){
  io.print("STATE : ");
  io.println(state);
}
