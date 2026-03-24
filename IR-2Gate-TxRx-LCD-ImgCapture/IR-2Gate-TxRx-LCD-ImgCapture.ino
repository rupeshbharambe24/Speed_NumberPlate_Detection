#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================== WIFI / HUB ==================
const char* WIFI_SSID = "REDACTED";
const char* WIFI_PASS = "REDACTED";

const char* LAPTOP_IP = "192.168.10.5";
const int   LAPTOP_PORT = 5000;

// ================== LCD ==================
const uint8_t LCD_ADDR = 0x27;   // change if needed
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ================== GATE PINS ==================
const int TX1_PIN = 25;
const int TX2_PIN = 26;
const int RX1_PIN = 32;
const int RX2_PIN = 33;

// ================== IR PWM ==================
const uint32_t PWM_FREQ = 38000;
const uint8_t  PWM_RES  = 8;
const uint32_t DUTY_ON  = 200;    // 0..255

// Time-multiplex bursts: TX1 ON -> gap -> TX2 ON -> gap
const uint32_t BURST_ON_US  = 600;
const uint32_t BURST_OFF_US = 650;

enum Phase { TX1_ON, GAP1, TX2_ON, GAP2 };
Phase phase = TX1_ON;
uint32_t nextToggleUs = 0;

// Pairing window: only accept RX edges from the currently active TX window
volatile uint8_t activeGate = 1;

// ================== RX edge timestamps ==================
volatile uint32_t lastEdge1 = 0;
volatile uint32_t lastEdge2 = 0;

void IRAM_ATTR isrRx1() { if (activeGate == 1) lastEdge1 = micros(); }
void IRAM_ATTR isrRx2() { if (activeGate == 2) lastEdge2 = micros(); }

// Beam detection thresholds
const uint32_t PRESENT_US = 60000;        // 60ms
const uint32_t BREAK_CONFIRM_US = 25000;  // 25ms
const uint32_t ARM_STABLE_MS = 300;

// ================== SPEED SETTINGS ==================
const float GATE_DISTANCE_M = 2.0;
const float SPEED_LIMIT_KMH = 20.0;
const float MAX_VALID_SPEED_KMH = 80.0;

const float GATE2_TIMEOUT_S = 2.0;
const uint32_t GATE2_DEADTIME_US = 80000; // ignore gate2 break for first 80ms

// Capture cooldown
const uint32_t CAPTURE_COOLDOWN_MS = 1500;
uint32_t lastCaptureMs = 0;

// ================== LCD helper ==================
String lastL0="", lastL1="";
void lcdShow(const String& l0, const String& l1) {
  String a=l0, b=l1;
  if (a.length()>16) a=a.substring(0,16);
  if (b.length()>16) b=b.substring(0,16);
  if (a==lastL0 && b==lastL1) return;
  lastL0=a; lastL1=b;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(a);
  lcd.setCursor(0,1); lcd.print(b);
}

// ================== WiFi ensure/reconnect ==================
bool ensureWiFi(uint32_t timeoutMs = 20000) {
  // Already connected with valid IP?
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0)) return true;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);    // auto reconnect support :contentReference[oaicite:2]{index=2}

  // Clean restart of STA connection (do NOT erase credentials)
  WiFi.disconnect(true);
  delay(200);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0)) {
      return true;
    }
    delay(250);
  }
  return false;
}

// ================== IR burst engine ==================
void setTxDuty(uint32_t d1, uint32_t d2) {
  ledcWrite(TX1_PIN, d1);
  ledcWrite(TX2_PIN, d2);
}

void updateBursts() {
  uint32_t now = micros();
  if ((int32_t)(now - nextToggleUs) < 0) return;

  switch (phase) {
    case TX1_ON:
      activeGate = 1;
      setTxDuty(DUTY_ON, 0);
      nextToggleUs = now + BURST_ON_US;
      phase = GAP1;
      break;

    case GAP1:
      activeGate = 1;
      setTxDuty(0, 0);
      nextToggleUs = now + BURST_OFF_US;
      phase = TX2_ON;
      break;

    case TX2_ON:
      activeGate = 2;
      setTxDuty(0, DUTY_ON);
      nextToggleUs = now + BURST_ON_US;
      phase = GAP2;
      break;

    case GAP2:
      activeGate = 2;
      setTxDuty(0, 0);
      nextToggleUs = now + BURST_OFF_US;
      phase = TX1_ON;
      break;
  }
}

inline bool present1() { return (micros() - lastEdge1) < PRESENT_US; }
inline bool present2() { return (micros() - lastEdge2) < PRESENT_US; }

// ================== Laptop trigger (with WiFi ensure) ==================
bool triggerLaptop(float speedKmh) {
  if (millis() - lastCaptureMs < CAPTURE_COOLDOWN_MS) return true;
  lastCaptureMs = millis();

  // Make sure WiFi is up right before capture
  if (!ensureWiFi(7000)) {
    Serial.println("WiFi down -> cannot capture");
    return false;
  }

  HTTPClient http;
  String url = String("http://") + LAPTOP_IP + ":" + String(LAPTOP_PORT) +
               "/trigger?speed=" + String(speedKmh, 2);

  Serial.print("GET "); Serial.println(url);

  http.begin(url);
  int code = http.GET();
  http.end();

  Serial.print("HTTP code="); Serial.println(code);
  return (code == 200);
}

// ================== STATE MACHINE ==================
enum State {
  WAIT_BOTH_STABLE,
  READY_WAIT_GATE1,
  TIMING_WAIT_GATE2
};

State state = WAIT_BOTH_STABLE;

uint32_t bothPresentSinceMs = 0;
uint32_t g1CandidateUs = 0;
uint32_t g2CandidateUs = 0;
uint32_t tGate1Us = 0;

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  lcdShow("BOOTING...", "WiFi connect");

  // Try connect WiFi (but do not block forever)
  bool wifiOk = ensureWiFi(20000);
  if (wifiOk) {
    lcdShow("WiFi OK", WiFi.localIP().toString());
    Serial.print("WiFi OK IP="); Serial.println(WiFi.localIP());
    delay(800);
  } else {
    lcdShow("WiFi FAIL", "Speed only");
    Serial.println("WiFi FAIL (will try again on capture)");
    delay(800);
  }

  pinMode(RX1_PIN, INPUT_PULLUP);
  pinMode(RX2_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(RX1_PIN), isrRx1, FALLING);
  attachInterrupt(digitalPinToInterrupt(RX2_PIN), isrRx2, FALLING);

  bool ok1 = ledcAttach(TX1_PIN, PWM_FREQ, PWM_RES);
  bool ok2 = ledcAttach(TX2_PIN, PWM_FREQ, PWM_RES);
  Serial.printf("ledcAttach TX1=%d TX2=%d\n", ok1, ok2);

  setTxDuty(0, 0);
  nextToggleUs = micros();

  lcdShow("SYSTEM READY", "Align beams");
}

void loop() {
  updateBursts();

  bool g1p = present1();
  bool g2p = present2();

  // Debug every 500ms (remove later)
  static uint32_t tPrint = 0;
  if (millis() - tPrint > 500) {
    tPrint = millis();
    Serial.printf("STATE=%d G1p=%d G2p=%d age1=%luus age2=%luus\n",
      (int)state, g1p, g2p,
      (unsigned long)(micros()-lastEdge1),
      (unsigned long)(micros()-lastEdge2));
  }

  switch (state) {

    case WAIT_BOTH_STABLE: {
      if (g1p && g2p) {
        if (bothPresentSinceMs == 0) bothPresentSinceMs = millis();
        if (millis() - bothPresentSinceMs >= ARM_STABLE_MS) {
          state = READY_WAIT_GATE1;
          g1CandidateUs = 0;
          lcdShow("READY", "No vehicle");
          Serial.println("READY (armed)");
        }
      } else {
        bothPresentSinceMs = 0;
        if (!g1p && !g2p) lcdShow("NO VEHICLE", "Fix Gate1+2");
        else if (!g1p)    lcdShow("NO VEHICLE", "Fix Gate1");
        else              lcdShow("NO VEHICLE", "Fix Gate2");
      }
    } break;

    case READY_WAIT_GATE1: {
      // If Gate2 is lost, we can’t measure (go back)
      if (!g2p) {
        state = WAIT_BOTH_STABLE;
        bothPresentSinceMs = 0;
        break;
      }

      // Gate1 break detection (confirm)
      if (!g1p) {
        if (g1CandidateUs == 0) g1CandidateUs = micros();
        if (micros() - g1CandidateUs >= BREAK_CONFIRM_US) {
          tGate1Us = micros();
          state = TIMING_WAIT_GATE2;
          g2CandidateUs = 0;
          lcdShow("VEHICLE...", "Gate1 broken");
          Serial.println("Gate1 BROKEN -> start");
        }
      } else {
        g1CandidateUs = 0;
      }
    } break;

    case TIMING_WAIT_GATE2: {
      float waited = (micros() - tGate1Us) / 1e6f;
      if (waited > GATE2_TIMEOUT_S) {
        lcdShow("TIMEOUT", "Try again");
        Serial.println("TIMEOUT (no Gate2)");
        state = WAIT_BOTH_STABLE;
        bothPresentSinceMs = 0;
        break;
      }

      if ((micros() - tGate1Us) < GATE2_DEADTIME_US) break;

      // Gate2 break detection (confirm)
      if (!g2p) {
        if (g2CandidateUs == 0) g2CandidateUs = micros();
        if (micros() - g2CandidateUs >= BREAK_CONFIRM_US) {
          float dt = (micros() - tGate1Us) / 1e6f;
          float speedKmh = (GATE_DISTANCE_M / dt) * 3.6f;

          float dt_min = GATE_DISTANCE_M / (MAX_VALID_SPEED_KMH / 3.6f);
          if (dt < dt_min || speedKmh > MAX_VALID_SPEED_KMH) {
            Serial.printf("GLITCH dt=%.4f speed=%.2f\n", dt, speedKmh);
            lcdShow("GLITCH", "Ignored");
          } else {
            Serial.printf("dt=%.4f s speed=%.2f km/h\n", dt, speedKmh);
            lcdShow("SPEED:", String(speedKmh,1) + " km/h");

            if (speedKmh > SPEED_LIMIT_KMH) {
              Serial.println("OVERSPEED branch entered");
              Serial.print("WiFi.status="); Serial.println(WiFi.status());
              Serial.print("ESP32 IP="); Serial.println(WiFi.localIP());

              lcdShow("OVERSPEED!", "Capturing...");
              bool ok = triggerLaptop(speedKmh);

              if (ok) lcdShow("OVERSPEED!", "LOGGED");
              else    lcdShow("OVERSPEED!", "WiFi/CAP FAIL");
            } else {
              lcdShow("OK", "No capture");
            }
          }

          state = WAIT_BOTH_STABLE;
          bothPresentSinceMs = 0;
          Serial.println("DONE -> wait stable");
        }
      } else {
        g2CandidateUs = 0;
      }
    } break;
  }

  delay(2);
}
