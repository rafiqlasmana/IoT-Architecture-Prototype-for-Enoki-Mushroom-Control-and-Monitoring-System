/*
 * ============================================================
 * SISTEM MONITORING & KENDALI CHAMBER BUDIDAYA JAMUR ENOKI
 * Universitas Pembangunan Panca Budi — Rafiq Syarif Lasmana
 * NIM: 2424210112
 * ============================================================
 * PERUBAHAN UTAMA (v2):
 *  - Relay AC (GPIO25) DIHAPUS, diganti Peltier/TEC dengan
 *    kendali FUZZY LOGIC (Sugeno-approx, eFLL).
 *  - Fan sirkulasi/heatsink Peltier (baru) juga FUZZY LOGIC,
 *    input dari daya Peltier + delta suhu.
 *  - Fan lama (GPIO14) TETAP relay ON/OFF biasa → fungsinya
 *    exhaust CO2, tidak diubah.
 *  - Output Peltier & Fan Heatsink TIDAK memakai pin PWM fisik.
 *    Sesuai simulasi Wokwi: 74HC595 menggantikan peran driver
 *    MOSFET, dan LED bar graph (10 segmen) menggantikan peran
 *    aktuator fisik (level daya 0-100%, step 10%).
 *      - Peltier   -> bargraph1 via sr1+sr2 (DS=5, STCP=18, SHCP=19)
 *      - FanHeatsink -> bargraph2 via sr3+sr4 (DS=13, STCP=25, SHCP=32)
 *  - Rate limiter ±15%/siklus (siklus = 2 detik) pada output
 *    Peltier & Fan Heatsink, termasuk saat ganti mode dan saat
 *    failsafe NaN aktif (semua turun/naik bertahap, tidak instan).
 *  - Delta suhu dihaluskan pakai moving average 3 sampel.
 *  - Failsafe: 5x gagal baca DHT22 berturut-turut -> target
 *    output Peltier & Fan Heatsink dipaksa 0% (tetap lewat
 *    rate limiter, turun bertahap).
 * ============================================================
 * PIN:
 *   DHT22          → GPIO15
 *   OLED SSD1306   → SDA=GPIO21, SCL=GPIO22 (I2C 0x3C)
 *   Relay Mist     → GPIO26
 *   Relay LED      → GPIO27
 *   Relay Fan(CO2) → GPIO14
 *   Tombol Mode    → GPIO33
 *   SR1(+SR2) bargraph1 (Peltier)     → DS=5,  STCP=18, SHCP=19
 *   SR3(+SR4) bargraph2 (FanHeatsink) → DS=13, STCP=25, SHCP=32
 * ============================================================
 */

#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Fuzzy.h>

const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* MQTT_BROKER   = "test.mosquitto.org";
const int   MQTT_PORT     = 1883;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ------------ PIN ----------------
#define DHTPIN         15
#define DHTTYPE        DHT22
#define PIN_RELAY_MIST 26
#define PIN_RELAY_LED  27
#define PIN_RELAY_FAN  14   // Fan exhaust CO2 (relay ON/OFF biasa, TIDAK berubah)
#define PIN_BUTTON     33

// ------------ SHIFT REGISTER (pengganti driver MOSFET) ------
#define SR1_DS    5   // bargraph1 = Peltier
#define SR1_STCP  18
#define SR1_SHCP  19

#define SR3_DS    13  // bargraph2 = Fan Heatsink
#define SR3_STCP  25
#define SR3_SHCP  32

// ------------ OLED ----------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ------------ SENSOR --------------
DHT dht(DHTPIN, DHTTYPE);
float temperature = 0;
float humidity    = 0;

// ------------ MODE OPERASI --------
enum Mode { M1_INKUBASI, M2A_INDUKSI, M2B_PEMANJANGAN, M2C_FRUITING };
Mode currentMode = M1_INKUBASI;

struct ModeConfig {
  const char* name;
  float tempMax;
  float tempMin;
  float rhMin;
  float rhMax;
  bool useMist;
  bool useFan;
  bool useLED;
  float  durMinDays;   // durasi minimum (hari) -> pemicu notifikasi dashboard
  float  durMaxDays;   // durasi maksimum (hari) -> pemicu pindah fase OTOMATIS
};

// ----------- PARAMETER -------------
// durMin/durMax hasil sintesis studi literatur (Chang & Miles, 2004;
// DMR/ICAR leaflet; Dong, 2023) -> lihat Bab 2 skripsi untuk justifikasi.
const ModeConfig modes[4] = {
  {"M1 Inkubasi",     23.0, 20.0, 60.0, 70.0, true, false, false, 20, 25},
  {"M2a Induksi",     12.0, 10.0, 80.0, 85.0, true, false, false, 10, 14},
  {"M2b Pemanjangan", 15.0, 12.0, 75.0, 80.0, true, true,  false,  3,  5},
  {"M2c Fruiting",    12.0,  8.0, 80.0, 90.0, true, false, true,   8, 12},
};

// ---------- STATUS AKTUATOR RELAY (tetap) --------
bool mistOn = false;
bool ledOn  = false;
bool fanOn  = false;   // fan exhaust CO2

// ---------- MINIMUM ON TIMER (mist, tetap) -------
unsigned long mistOnSince = 0;
const unsigned long MIST_MIN_ON_MS = 20000; // 20 detik

// ---------- FUZZY: PELTIER & FAN HEATSINK --------
Fuzzy *fuzzyPeltier = new Fuzzy();
Fuzzy *fuzzyFanHS   = new Fuzzy();

float peltierOutput   = 0;   // % aktual (setelah rate limiter)
float fanHSOutput     = 0;   // % aktual (setelah rate limiter)
const float RATE_LIMIT_PER_CYCLE = 15.0;  // max perubahan %/siklus (2 detik)

// ---------- SMOOTHING DELTA SUHU (moving average 3 sampel) ----
float tempHistory[4] = {0, 0, 0, 0};
int   tempHistCount   = 0;   // jumlah sampel yang sudah masuk (maks 4)
float deltaSmooth     = 0;

// ---------- FAILSAFE NaN ----------
int nanCounter = 0;
const int NAN_FAILSAFE_THRESHOLD = 5;

// ----------- BUTTON ---------------
// State: IDLE → PRESSED → HELD → RELEASED → IDLE
// Tidak ada delay() sama sekali di button handler
enum BtnState { BTN_IDLE, BTN_PRESSED, BTN_HELD, BTN_WAIT_RELEASE };
BtnState btnState = BTN_IDLE;
unsigned long btnTimer = 0;
const unsigned long DEBOUNCE_MS = 50;

// ── TIMER ─────────────────────────────────────────────────
unsigned long lastSensor    = 0;
unsigned long lastOLED      = 0;
unsigned long lastSerial    = 0;
unsigned long modeNotifyEnd = 0;
bool showModeNotif = false;

// ── VIRTUAL JAM (simulasi jam tangan) ────────────────────
unsigned long simStart = 0;
int startHour = 6;

// ── PEMINDAHAN FASE OTOMATIS (man-in-the-middle) ─────────
// Skala waktu virtual sama dengan getVirtualHour(): 1 menit riil = 1 jam
// virtual, sehingga 1 hari virtual = 1440 detik riil (24 menit riil).
unsigned long phaseStartMillis = 0;
bool minNotifSent = false;
const float VIRTUAL_SEC_PER_DAY = 60.0; // 24 jam virtual x 60 detik

// ===== Function Prototypes =====
void allRelayOff();
void printModeInfo();
void handleButton(unsigned long now);
void controlActuators();
void updateOLED(unsigned long now);
void renderModeNotif();
int getVirtualHour();
void printStatus();
void reconnectMQTT();
void publishMQTT();
void setupFuzzyPeltier();
void setupFuzzyFanHS();
float getSetpoint();
void updateDeltaSmooth(float t);
void sendBarLevel(int dsPin, int stcpPin, int shcpPin, int level);
void changeMode(Mode newMode);
float getPhaseElapsedDays();
void checkPhaseTransition();
void mqttCallback(char* topic, byte* payload, unsigned int length);


// -------------- SETUP -------------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_RELAY_MIST, OUTPUT);
  pinMode(PIN_RELAY_LED,  OUTPUT);
  pinMode(PIN_RELAY_FAN,  OUTPUT);
  pinMode(PIN_BUTTON,     INPUT_PULLUP);

  pinMode(SR1_DS,   OUTPUT);
  pinMode(SR1_STCP, OUTPUT);
  pinMode(SR1_SHCP, OUTPUT);
  pinMode(SR3_DS,   OUTPUT);
  pinMode(SR3_STCP, OUTPUT);
  pinMode(SR3_SHCP, OUTPUT);

  allRelayOff();
  sendBarLevel(SR1_DS, SR1_STCP, SR1_SHCP, 0);
  sendBarLevel(SR3_DS, SR3_STCP, SR3_SHCP, 0);

  Wire.begin(21, 22);
  dht.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED tidak terdeteksi!");
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Enoki Chamber IoT");
  display.println("Booting...");
  display.display();
  delay(1000);

  simStart = millis();
  phaseStartMillis = millis();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(" OK");

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  reconnectMQTT();

  setupFuzzyPeltier();
  setupFuzzyFanHS();

  printModeInfo();
  Serial.println("[INFO] Siap. Tekan tombol untuk ganti mode.");
}

void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("MQTT...");
    // Client ID di-uniquify pakai MAC efuse -> hindari broker publik
    // mengusir koneksi lama kalau ada 2 sesi jalan dgn ID sama persis.
    String clientId = "enoki-esp32-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
    if (mqtt.connect(clientId.c_str())) {
      Serial.print("OK sbg "); Serial.println(clientId);
      bool subOk = mqtt.subscribe("enoki/cmd/mode");
      Serial.print("[MQTT] Subscribe enoki/cmd/mode -> ");
      Serial.println(subOk ? "OK" : "GAGAL (cek koneksi/broker)");
    }
    else { Serial.print("fail rc="); Serial.println(mqtt.state()); delay(3000); }
  }
}

// ═════════════════════════════════════════════════════════
// Terima perintah dari dashboard lewat MQTT.
// Payload "next" -> maju satu fase (siklik), sama seperti tombol fisik.
// Payload angka 0-3 -> lompat langsung ke fase tsb (dipakai tombol mode-tab).
// ═════════════════════════════════════════════════════════
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  // Log SEMUA pesan masuk (topic apapun) -> alat diagnosa utama.
  // Kalau baris ini TIDAK PERNAH muncul di Serial Monitor saat tombol
  // dashboard diklik, berarti pesan tidak sampai ke ESP32 sama sekali
  // (masalah broker/koneksi/topic), BUKAN bug di changeMode().
  Serial.print("[MQTT IN] topic="); Serial.print(topic);
  Serial.print(" payload="); Serial.println(msg);

  if (String(topic) != "enoki/cmd/mode") return;

  if (msg == "next") {
    changeMode((Mode)((currentMode + 1) % 4));
  } else {
    int idx = msg.toInt();
    if (idx >= 0 && idx <= 3) changeMode((Mode)idx);
  }
}

//========================================================

void publishMQTT() {
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.publish("enoki/temperature", String(temperature, 1).c_str());
  mqtt.publish("enoki/humidity",    String(humidity, 1).c_str());
  mqtt.publish("enoki/mode",        String((int)currentMode).c_str());
  mqtt.publish("enoki/peltier",     String(peltierOutput, 0).c_str());
  mqtt.publish("enoki/fanheatsink", String(fanHSOutput, 0).c_str());
  mqtt.publish("enoki/mist",        mistOn ? "1" : "0");
  mqtt.publish("enoki/led",         ledOn  ? "1" : "0");
  mqtt.publish("enoki/fan",         fanOn  ? "1" : "0");

  // Info durasi fase, dipakai dashboard untuk progress bar & notifikasi
  mqtt.publish("enoki/phase_elapsed_days", String(getPhaseElapsedDays(), 2).c_str());
  mqtt.publish("enoki/phase_min_days",     String(modes[currentMode].durMinDays).c_str());
  mqtt.publish("enoki/phase_max_days",     String(modes[currentMode].durMaxDays).c_str());
}

// ═════════════════════════════════════════════════════════
void loop() {
  mqtt.loop();
  unsigned long now = millis();

  handleButton(now);

  // Baca sensor setiap 2 detik (= 1 siklus kontrol)
  if (now - lastSensor >= 2000) {
    lastSensor = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      temperature = t;
      humidity    = h;
      nanCounter  = 0;
      updateDeltaSmooth(temperature);
    } else {
      Serial.println("[WARN] Gagal baca DHT22");
      nanCounter++;
    }
    controlActuators();
    checkPhaseTransition();
  }

  // Update OLED setiap 500ms
  if (now - lastOLED >= 500) {
    lastOLED = now;
    updateOLED(now);
  }

  // Serial log setiap 3 detik
  if (now - lastSerial >= 3000) {
    lastSerial = now;
    printStatus();
    publishMQTT();
  }
}

// ═════════════════════════════════════════════════════════
// BUTTON STATE MACHINE — tanpa delay() sama sekali
// ─────────────────────────────────────────────────────────
// M1 ──[tekan]──> M2a ──[tekan]──> M2b ──[tekan]──> M2c ──[tekan]──> M1
// Catatan: peltierOutput & fanHSOutput TIDAK direset saat ganti
// mode -> rate limiter yang menangani transisi secara bertahap.
// ═════════════════════════════════════════════════════════
void handleButton(unsigned long now) {
  bool reading = (digitalRead(PIN_BUTTON) == LOW); // true = ditekan

  switch (btnState) {

    case BTN_IDLE:
      if (reading) {
        btnState = BTN_PRESSED;
        btnTimer = now;
      }
      break;

    case BTN_PRESSED:
      if (!reading) {
        // false trigger (bounce)
        btnState = BTN_IDLE;
      }
      else if (now - btnTimer >= DEBOUNCE_MS) {
        // valid press
        btnState = BTN_WAIT_RELEASE;
        changeMode((Mode)((currentMode + 1) % 4));   // manual, sama seperti tombol dashboard
      }
      break;

    case BTN_WAIT_RELEASE:
      // tunggu sampai tombol benar-benar dilepas
      if (!reading) {
        btnState = BTN_IDLE;
      }
      break;
  }
}

// ═════════════════════════════════════════════════════════
// Satu-satunya jalur resmi untuk pindah fase — dipanggil oleh:
//  (1) tombol fisik (handleButton)
//  (2) tombol dashboard / mode-tab (lewat mqttCallback)
//  (3) auto-transition saat durMaxDays tercapai (checkPhaseTransition)
// Peltier & Fan Heatsink SENGAJA tidak direset di sini — transisinya
// tetap ditangani rate limiter di controlActuators().
// ═════════════════════════════════════════════════════════
void changeMode(Mode newMode) {
  currentMode      = newMode;
  phaseStartMillis = millis();   // reset hitung mundur durasi fase baru
  minNotifSent     = false;      // izinkan notifikasi min-days terkirim lagi

  allRelayOff();
  printModeInfo();

  showModeNotif = true;
  modeNotifyEnd = millis() + 800;
  renderModeNotif();

  Serial.print("[MODE] -> ");
  Serial.println(modes[currentMode].name);
}

// ═════════════════════════════════════════════════════════
// Umur fase berjalan saat ini, dalam satuan hari VIRTUAL (float).
// ═════════════════════════════════════════════════════════
float getPhaseElapsedDays() {
  return (millis() - phaseStartMillis) / 1000.0 / VIRTUAL_SEC_PER_DAY;
}

// ═════════════════════════════════════════════════════════
// Dipanggil tiap siklus kontrol (2 detik):
//  - Saat elapsed >= durMinDays  -> kirim notifikasi ke dashboard
//    (fase SUDAH BOLEH dipindah manual oleh user, tapi belum wajib).
//  - Saat elapsed >= durMaxDays  -> paksa pindah fase OTOMATIS,
//    user tetap jadi pengawas (man-in-the-middle) lewat notifikasi
//    di atas, tapi sistem tidak menunggu keputusan manusia selamanya.
// ═════════════════════════════════════════════════════════
void checkPhaseTransition() {
  const ModeConfig& cfg = modes[currentMode];
  float elapsed = getPhaseElapsedDays();

  if (!minNotifSent && elapsed >= cfg.durMinDays) {
    minNotifSent = true;
    mqtt.publish("enoki/notif", "MIN_REACHED");
    Serial.println("[INFO] Durasi minimum fase tercapai -> siap dipindah manual.");
  }

  if (elapsed >= cfg.durMaxDays) {
    Serial.println("[AUTO] Durasi maksimum fase tercapai -> pindah otomatis.");
    changeMode((Mode)((currentMode + 1) % 4));
  }
}

// ═════════════════════════════════════════════════════════
// Setpoint suhu = titik tengah rentang mode aktif
// ═════════════════════════════════════════════════════════
float getSetpoint() {
  return (modes[currentMode].tempMax + modes[currentMode].tempMin) / 2.0;
}

// ═════════════════════════════════════════════════════════
// Moving average 3 sampel untuk delta suhu (haluskan noise DHT22)
// ═════════════════════════════════════════════════════════
void updateDeltaSmooth(float t) {
  // geser buffer
  tempHistory[0] = tempHistory[1];
  tempHistory[1] = tempHistory[2];
  tempHistory[2] = tempHistory[3];
  tempHistory[3] = t;

  if (tempHistCount < 4) tempHistCount++;

  if (tempHistCount >= 4) {
    // rata-rata laju perubahan per sampel, dari 3 sampel terakhir
    deltaSmooth = (tempHistory[3] - tempHistory[0]) / 3.0;
  } else {
    deltaSmooth = 0; // buffer belum penuh, anggap stabil
  }
}

// ═════════════════════════════════════════════════════════
// FUZZY #1 — PELTIER
// Input1: errorSuhu (suhu - setpoint), Input2: deltaSmooth
// Output: daya peltier 0-100% (emulasi Sugeno via trapesium sempit)
// ═════════════════════════════════════════════════════════
void setupFuzzyPeltier() {
  FuzzyInput *errorSuhu = new FuzzyInput(1);
  FuzzySet *errNB = new FuzzySet(-10, -10, -4, -2);
  FuzzySet *errNS = new FuzzySet(-4, -2, -1, 0);
  FuzzySet *errZ  = new FuzzySet(-1, 0, 0, 1);
  FuzzySet *errPS = new FuzzySet(0, 1, 2, 4);
  FuzzySet *errPB = new FuzzySet(2, 4, 10, 10);
  errorSuhu->addFuzzySet(errNB);
  errorSuhu->addFuzzySet(errNS);
  errorSuhu->addFuzzySet(errZ);
  errorSuhu->addFuzzySet(errPS);
  errorSuhu->addFuzzySet(errPB);
  fuzzyPeltier->addFuzzyInput(errorSuhu);

  FuzzyInput *delta = new FuzzyInput(2);
  FuzzySet *dN = new FuzzySet(-2, -2, -0.3, 0);
  FuzzySet *dZ = new FuzzySet(-0.2, 0, 0, 0.2);
  FuzzySet *dP = new FuzzySet(0, 0.3, 2, 2);
  delta->addFuzzySet(dN);
  delta->addFuzzySet(dZ);
  delta->addFuzzySet(dP);
  fuzzyPeltier->addFuzzyInput(delta);

  FuzzyOutput *daya = new FuzzyOutput(1);
  FuzzySet *pOFF  = new FuzzySet(0, 0, 0, 5);
  FuzzySet *pLOW  = new FuzzySet(20, 25, 25, 30);
  FuzzySet *pMED  = new FuzzySet(45, 50, 50, 55);
  FuzzySet *pHIGH = new FuzzySet(70, 75, 75, 80);
  FuzzySet *pMAX  = new FuzzySet(95, 100, 100, 100);
  daya->addFuzzySet(pOFF);
  daya->addFuzzySet(pLOW);
  daya->addFuzzySet(pMED);
  daya->addFuzzySet(pHIGH);
  daya->addFuzzySet(pMAX);
  fuzzyPeltier->addFuzzyOutput(daya);

  // Rule base 5x3 = 15 rule
  struct { FuzzySet* e; FuzzySet* d; FuzzySet* out; } R[] = {
    {errNB, dN,  pOFF}, {errNB, dZ,  pOFF}, {errNB, dP,  pOFF},
    {errNS, dN,  pOFF}, {errNS, dZ,  pOFF}, {errNS, dP,  pLOW},
    {errZ,  dN,  pLOW}, {errZ,  dZ,  pLOW}, {errZ,  dP,  pMED},
    {errPS, dN,  pMED}, {errPS, dZ,  pHIGH},{errPS, dP,  pHIGH},
    {errPB, dN,  pHIGH},{errPB, dZ,  pMAX}, {errPB, dP,  pMAX},
  };

  for (int i = 0; i < 15; i++) {
    FuzzyRuleAntecedent *ant = new FuzzyRuleAntecedent();
    ant->joinWithAND(R[i].e, R[i].d);
    FuzzyRuleConsequent *cons = new FuzzyRuleConsequent();
    cons->addOutput(R[i].out);
    fuzzyPeltier->addFuzzyRule(new FuzzyRule(i + 1, ant, cons));
  }
}

// ═════════════════════════════════════════════════════════
// FUZZY #2 — FAN HEATSINK (cascade dari output Peltier)
// Input1: dayaPeltier (0-100), Input2: deltaSmooth
// Output: kecepatan fan heatsink 0-100%
// ═════════════════════════════════════════════════════════
void setupFuzzyFanHS() {
  FuzzyInput *dayaPeltierIn = new FuzzyInput(1);
  FuzzySet *fLOW  = new FuzzySet(0, 0, 25, 40);
  FuzzySet *fMED  = new FuzzySet(25, 40, 60, 75);
  FuzzySet *fHIGH = new FuzzySet(60, 75, 100, 100);
  dayaPeltierIn->addFuzzySet(fLOW);
  dayaPeltierIn->addFuzzySet(fMED);
  dayaPeltierIn->addFuzzySet(fHIGH);
  fuzzyFanHS->addFuzzyInput(dayaPeltierIn);

  FuzzyInput *delta2 = new FuzzyInput(2);
  FuzzySet *d2N = new FuzzySet(-2, -2, -0.3, 0);
  FuzzySet *d2Z = new FuzzySet(-0.2, 0, 0, 0.2);
  FuzzySet *d2P = new FuzzySet(0, 0.3, 2, 2);
  delta2->addFuzzySet(d2N);
  delta2->addFuzzySet(d2Z);
  delta2->addFuzzySet(d2P);
  fuzzyFanHS->addFuzzyInput(delta2);

  FuzzyOutput *kecepatan = new FuzzyOutput(1);
  FuzzySet *fsOFF  = new FuzzySet(0, 0, 0, 5);
  FuzzySet *fsLOW  = new FuzzySet(25, 30, 30, 35);
  FuzzySet *fsMED  = new FuzzySet(55, 60, 60, 65);
  FuzzySet *fsHIGH = new FuzzySet(85, 90, 90, 95);
  FuzzySet *fsMAX  = new FuzzySet(95, 100, 100, 100);
  kecepatan->addFuzzySet(fsOFF);
  kecepatan->addFuzzySet(fsLOW);
  kecepatan->addFuzzySet(fsMED);
  kecepatan->addFuzzySet(fsHIGH);
  kecepatan->addFuzzySet(fsMAX);
  fuzzyFanHS->addFuzzyOutput(kecepatan);

  // Rule base 3x3 = 9 rule
  struct { FuzzySet* p; FuzzySet* d; FuzzySet* out; } R[] = {
    {fLOW,  d2N, fsLOW},  {fLOW,  d2Z, fsLOW},  {fLOW,  d2P, fsMED},
    {fMED,  d2N, fsMED},  {fMED,  d2Z, fsMED},  {fMED,  d2P, fsHIGH},
    {fHIGH, d2N, fsHIGH}, {fHIGH, d2Z, fsMAX},  {fHIGH, d2P, fsMAX},
  };

  for (int i = 0; i < 9; i++) {
    FuzzyRuleAntecedent *ant = new FuzzyRuleAntecedent();
    ant->joinWithAND(R[i].p, R[i].d);
    FuzzyRuleConsequent *cons = new FuzzyRuleConsequent();
    cons->addOutput(R[i].out);
    fuzzyFanHS->addFuzzyRule(new FuzzyRule(i + 1, ant, cons));
  }
}

// ═════════════════════════════════════════════════════════
// Kirim level (0-10 segmen) ke bargraph via 74HC595 (daisy chain 2 IC)
// Pola VU-meter: isi segmen dari bawah (A1) sampai sejumlah `level`.
// Chain: DS -> SR_pertama -> Q7S -> SR_kedua
//   -> bit yang dikirim LEBIH DULU akan menempati SR_kedua (jauh),
//      jadi highByte (untuk SR kedua, segmen A9-A10) dikirim duluan,
//      lalu lowByte (untuk SR pertama, segmen A1-A8).
// ═════════════════════════════════════════════════════════
void sendBarLevel(int dsPin, int stcpPin, int shcpPin, int level) {
  level = constrain(level, 0, 10);

  uint16_t pattern = 0;
  for (int i = 0; i < level; i++) {
    pattern |= (1 << i);   // bit0=A1 ... bit9=A10
  }

  uint8_t lowByte  = pattern & 0xFF;         // -> SR pertama (A1-A8)
  uint8_t highByte = (pattern >> 8) & 0xFF;  // -> SR kedua   (A9-A10)

  digitalWrite(stcpPin, LOW);
  shiftOut(dsPin, shcpPin, MSBFIRST, highByte);
  shiftOut(dsPin, shcpPin, MSBFIRST, lowByte);
  digitalWrite(stcpPin, HIGH);
}

// ═════════════════════════════════════════════════════════
void controlActuators() {
  const ModeConfig& cfg = modes[currentMode];

  // ---------- FUZZY PELTIER + FAN HEATSINK ----------
  float targetPeltier = peltierOutput;
  float targetFanHS   = fanHSOutput;

  if (nanCounter >= NAN_FAILSAFE_THRESHOLD) {
    // Failsafe: sensor gagal berkelanjutan -> target paksa 0%,
    // tetap turun bertahap lewat rate limiter (bukan instan).
    targetPeltier = 0;
    targetFanHS   = 0;
  } else {
    float error = temperature - getSetpoint();

    fuzzyPeltier->setInput(1, error);
    fuzzyPeltier->setInput(2, deltaSmooth);
    fuzzyPeltier->fuzzify();
    targetPeltier = fuzzyPeltier->defuzzify(1);

    fuzzyFanHS->setInput(1, targetPeltier);
    fuzzyFanHS->setInput(2, deltaSmooth);
    fuzzyFanHS->fuzzify();
    targetFanHS = fuzzyFanHS->defuzzify(1);
  }

  // ---------- RATE LIMITER (±15%/siklus) ----------
  float diffP = targetPeltier - peltierOutput;
  diffP = constrain(diffP, -RATE_LIMIT_PER_CYCLE, RATE_LIMIT_PER_CYCLE);
  peltierOutput = constrain(peltierOutput + diffP, 0, 100);

  float diffF = targetFanHS - fanHSOutput;
  diffF = constrain(diffF, -RATE_LIMIT_PER_CYCLE, RATE_LIMIT_PER_CYCLE);
  fanHSOutput = constrain(fanHSOutput + diffF, 0, 100);

  // ---------- KIRIM KE BARGRAPH (pengganti aktuator fisik) ----------
  int levelPeltier = round(peltierOutput / 10.0);
  int levelFanHS   = round(fanHSOutput / 10.0);
  sendBarLevel(SR1_DS, SR1_STCP, SR1_SHCP, levelPeltier);
  sendBarLevel(SR3_DS, SR3_STCP, SR3_SHCP, levelFanHS);

  // ---------- Loop RH → Mist Maker (histeresis, TETAP) ----------
  if (cfg.useMist) {
    if (humidity < cfg.rhMin) {
      if (!mistOn) { mistOn = true; mistOnSince = millis(); }
    }
    else if (humidity > cfg.rhMax) {
      if (mistOn && (millis() - mistOnSince >= MIST_MIN_ON_MS)) {
        mistOn = false;
      }
    }
  } else {
    mistOn = false;
  }
  digitalWrite(PIN_RELAY_MIST, mistOn ? HIGH : LOW);

  // ---------- Fan exhaust CO2 (relay ON/OFF, TETAP) ----------
  fanOn = cfg.useFan;
  digitalWrite(PIN_RELAY_FAN, fanOn ? HIGH : LOW);

  // ---------- LED Grow: M2c saja, ON jam 06:00–18:00 virtual ----------
  if (cfg.useLED) {
    int vh = getVirtualHour();
    ledOn  = (vh >= 6 && vh < 18);
  } else {
    ledOn = false;
  }
  digitalWrite(PIN_RELAY_LED, ledOn ? HIGH : LOW);
}

// ═════════════════════════════════════════════════════════
void updateOLED(unsigned long now) {
  // Tampilkan notifikasi mode berganti selama 800ms
  if (showModeNotif) {
    if (now < modeNotifyEnd) {
      renderModeNotif();
      return;
    } else {
      showModeNotif = false;
    }
  }

  // Tampilan normal
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(modes[currentMode].name);

  display.print("T:"); display.print(temperature, 1);
  display.print("C  H:"); display.print(humidity, 1);
  display.println("%");

  display.print("Jam: "); display.print(getVirtualHour());
  display.println(":00");

  display.setCursor(0, 36);
  display.print("PLT:"); display.print(peltierOutput, 0); display.print("% ");
  display.print("FHS:"); display.print(fanHSOutput, 0);   display.println("%");
  display.print("MST:"); display.print(mistOn ? "ON " : "off");
  display.print(" LED:"); display.print(ledOn  ? "ON " : "off");
  display.print(" FAN:"); display.println(fanOn  ? "ON"  : "off");

  display.setCursor(0, 56);
  display.print(modes[currentMode].tempMin, 0);
  display.print("-");
  display.print(modes[currentMode].tempMax, 0);
  display.print("C | ");
  display.print(modes[currentMode].rhMin, 0);
  display.print("-");
  display.print(modes[currentMode].rhMax, 0);
  display.print("%");

  display.display();
}

// ═════════════════════════════════════════════════════════
void renderModeNotif() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(">> Mode berganti:");
  display.setTextSize(2);
  display.setCursor(0, 20);
  display.println(modes[currentMode].name);
  display.display();
}

// ═════════════════════════════════════════════════════════
int getVirtualHour() {
  unsigned long elapsedSec = (millis() - simStart) / 1000UL;
  return (startHour + (int)(elapsedSec / 60UL)) % 24; // 1 menit = 1 jam virtual
}

// ═════════════════════════════════════════════════════════
// Hanya reset relay Mist/LED/Fan-exhaust.
// Peltier & Fan Heatsink SENGAJA tidak disentuh di sini —
// transisinya ditangani rate limiter di controlActuators().
// ═════════════════════════════════════════════════════════
void allRelayOff() {
  mistOn = ledOn = fanOn = false;
  digitalWrite(PIN_RELAY_MIST, LOW);
  digitalWrite(PIN_RELAY_LED,  LOW);
  digitalWrite(PIN_RELAY_FAN,  LOW);
}

// ═════════════════════════════════════════════════════════
void printModeInfo() {
  Serial.println("========================================");
  Serial.print(">> MODE : "); Serial.println(modes[currentMode].name);
  Serial.print("   Suhu : "); Serial.print(modes[currentMode].tempMin);
  Serial.print(" – ");        Serial.print(modes[currentMode].tempMax);
  Serial.println(" C");
  Serial.print("   Setpoint: "); Serial.println(getSetpoint());
  if (modes[currentMode].useMist) {
    Serial.print("   RH   : "); Serial.print(modes[currentMode].rhMin);
    Serial.print(" – ");        Serial.print(modes[currentMode].rhMax);
    Serial.println(" %");
  }
  Serial.println("========================================");
}

// ═════════════════════════════════════════════════════════
void printStatus() {
  Serial.print("[T="); Serial.print(temperature, 1);
  Serial.print("C H="); Serial.print(humidity, 1);
  Serial.print("% | PLT="); Serial.print(peltierOutput, 0); Serial.print("%");
  Serial.print(" FHS=");    Serial.print(fanHSOutput, 0);   Serial.print("%");
  Serial.print(" | dT=");   Serial.print(deltaSmooth, 2);
  Serial.print(" | MST=");  Serial.print(mistOn ? "ON"  : "off");
  Serial.print(" LED=");    Serial.print(ledOn  ? "ON"  : "off");
  Serial.print(" FAN=");    Serial.print(fanOn  ? "ON"  : "off");
  Serial.print(" | Jam=");  Serial.print(getVirtualHour());
  Serial.print(" | Mode="); Serial.print(modes[currentMode].name);
  Serial.println("]");
}