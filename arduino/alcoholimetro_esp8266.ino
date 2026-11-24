#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

// -------- CONFIGURACIÓN DEL USUARIO --------
const char* WIFI_SSID = "ECG";
const char* WIFI_PASSWORD = "12345678";

// Sustituir por el dominio HTTPS que asigne Netlify, sin / al final.
const char* API_BASE_URL = "https://TU-SITIO.netlify.app";
const char* DEVICE_TOKEN = "CAMBIA_ESTA_CLAVE_POR_UNA_LARGA";

// -------- PINES --------
#define MQ3_PIN A0
#define BUZZER_PIN D5
#define OLED_SDA D2
#define OLED_SCL D1
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// -------- CALIBRACIÓN --------
// Mantener false hasta realizar calibración documentada con referencia.
const bool CALIBRATION_VALID = false;
// Reemplazar con valores medidos durante calibración.
// CAL_ZERO_ADC = lectura en aire de referencia.
// CAL_FULL_ADC = lectura en una concentración patrón conocida.
const float CAL_ZERO_ADC = 100.0;
const float CAL_FULL_ADC = 700.0;
const float CAL_FULL_MGL = 2.0;

// -------- PARÁMETROS --------
float warningLevel = 35;
float highLevel = 65;
float criticalLevel = 85;
String alarmMode = "auto"; // auto, silent, test
int warmupHours = 48;
unsigned long warmupStartedAt = 0;
unsigned long warmupDurationMs = 48UL * 60UL * 60UL * 1000UL;
unsigned long lastWarmupCommand = 0;
bool warmupCommandInitialized = false;
bool sensorWarming = true;
float sensorBaseline = -1;

unsigned long lastSample = 0;
unsigned long lastSend = 0;
unsigned long lastConfig = 0;
const unsigned long SAMPLE_INTERVAL = 500;
const unsigned long SEND_INTERVAL = 2000;
const unsigned long CONFIG_INTERVAL = 5000;

int rawValue = 0;
float relativeIndex = 0;
float estimatedMgL = 0;
bool wifiWasConnected = false;

void buzzerOff();

bool isWarming() {
  return (millis() - warmupStartedAt) < warmupDurationMs;
}

unsigned long warmupRemainingSeconds() {
  unsigned long elapsed = millis() - warmupStartedAt;
  if (elapsed >= warmupDurationMs) return 0;
  return (warmupDurationMs - elapsed) / 1000UL;
}

void startWarmup(int hours, unsigned long commandId) {
  warmupHours = constrain(hours, 1, 48);
  warmupDurationMs = (unsigned long)warmupHours * 60UL * 60UL * 1000UL;
  warmupStartedAt = millis();
  lastWarmupCommand = commandId;
  sensorWarming = true;
  sensorBaseline = -1;
  relativeIndex = 0;
  buzzerOff();
  Serial.printf("Precalentamiento iniciado: %d horas\n", warmupHours);
}

int readMQ3() {
  // Promedio de muestras separadas para reducir ruido.
  long sum = 0;
  const int samples = 20;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(MQ3_PIN);
    delay(5);
  }
  return (int)(sum / samples);
}

float calculateIndex(int raw) {
  // Índice relativo experimental; no es una medición certificada.
  if (sensorBaseline < 0) sensorBaseline = raw;
  if (raw < sensorBaseline + 20) sensorBaseline = sensorBaseline * 0.995f + raw * 0.005f;
  // Esta versión asume que AO aumenta ante alcohol. Verificar en tu módulo.
  float delta = (float)raw - sensorBaseline;
  return constrain((delta / 350.0f) * 100.0f, 0.0f, 100.0f);
}

bool calculateMgL(float raw, float &mgL) {
  if (!CALIBRATION_VALID || fabs(CAL_FULL_ADC - CAL_ZERO_ADC) < 1.0) return false;
  float fraction = (raw - CAL_ZERO_ADC) / (CAL_FULL_ADC - CAL_ZERO_ADC);
  fraction = constrain(fraction, 0.0f, 1.0f);
  mgL = fraction * CAL_FULL_MGL;
  return true;
}

String getStatus(float idx) {
  if (idx >= criticalLevel) return "Critico";
  if (idx >= highLevel) return "Alto";
  if (idx >= warningLevel) return "Advertencia";
  return "Normal";
}

void buzzerOff() { digitalWrite(BUZZER_PIN, LOW); }

void updateBuzzer(float idx) {
  if (sensorWarming || isWarming()) { buzzerOff(); return; }
  if (alarmMode == "silent") { buzzerOff(); return; }

  unsigned long t = millis();
  if (alarmMode == "test") {
    // Patrón de prueba: 150 ms encendido / 850 ms apagado.
    digitalWrite(BUZZER_PIN, (t % 1000 < 150) ? HIGH : LOW);
    return;
  }

  if (idx >= criticalLevel) {
    // Crítico: doble pulso rápido repetitivo.
    unsigned long phase = t % 700;
    digitalWrite(BUZZER_PIN, (phase < 120 || (phase >= 220 && phase < 340)) ? HIGH : LOW);
  } else if (idx >= highLevel) {
    // Alto: pulso rápido.
    digitalWrite(BUZZER_PIN, (t % 500 < 160) ? HIGH : LOW);
  } else if (idx >= warningLevel) {
    // Advertencia: pulso lento.
    digitalWrite(BUZZER_PIN, (t % 1400 < 130) ? HIGH : LOW);
  } else {
    buzzerOff();
  }
}

void drawOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("VITASENSE  |  MQ-3");
  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  if (sensorWarming || isWarming()) {
    unsigned long rem = warmupRemainingSeconds();
    unsigned int hh = rem / 3600UL;
    unsigned int mm = (rem % 3600UL) / 60UL;
    unsigned int ss = rem % 60UL;
    unsigned long elapsed = warmupDurationMs - rem * 1000UL;
    int pct = (int)((elapsed * 100UL) / warmupDurationMs);
    display.setCursor(0, 17); display.print("PRECALENTANDO MQ-3");
    display.setTextSize(2); display.setCursor(0, 29);
    if (hh < 10) display.print("0"); display.print(hh); display.print(":");
    if (mm < 10) display.print("0"); display.print(mm); display.print(":");
    if (ss < 10) display.print("0"); display.print(ss);
    display.setTextSize(1); display.setCursor(0, 49);
    display.print("Progreso: "); display.print(pct); display.print("%");
    display.drawRect(0, 58, 128, 5, SSD1306_WHITE);
    display.fillRect(1, 59, (126 * pct) / 100, 3, SSD1306_WHITE);
  } else {
    display.setCursor(0, 17); display.print("Indice relativo");
    display.setTextSize(2); display.setCursor(0, 28);
    display.print(relativeIndex, 1); display.setTextSize(1); display.print(" /100");
    display.setCursor(0, 49); display.print("ADC:"); display.print(rawValue);
    display.print("  "); display.print(getStatus(relativeIndex));
    display.setCursor(0, 58); display.print(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "WiFi OFF");
    display.print(" | "); display.print(CALIBRATION_VALID ? "CAL OK" : "SIN CAL");
  }
  display.display();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("No se pudo conectar. Se reintentara.");
  }
}

String deviceTime() {
  time_t now = time(nullptr);
  if (now < 100000) return "";
  struct tm *tmInfo = gmtime(&now);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tmInfo);
  return String(buf);
}

bool postReading() {
  if (WiFi.status() != WL_CONNECTED) return false;
  BearSSL::WiFiClientSecure client;
  client.setInsecure(); // Para prototipo. En producción, validar certificado raíz.
  HTTPClient https;
  String url = String(API_BASE_URL) + "/.netlify/functions/reading";
  if (!https.begin(client, url)) return false;
  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-Device-Token", DEVICE_TOKEN);

  bool warming = sensorWarming || isWarming();
  bool calibrated = !warming && calculateMgL(rawValue, estimatedMgL);
  String body = "{";
  body += "\"raw\":" + String(rawValue) + ",";
  body += "\"index\":" + String(relativeIndex, 1) + ",";
  body += "\"calibrated\":" + String(calibrated ? "true" : "false") + ",";
  body += "\"mgL\":" + String(calibrated ? estimatedMgL : 0, 4) + ",";
  body += "\"warming\":" + String(warming ? "true" : "false") + ",";
  body += "\"warmupHours\":" + String(warmupHours) + ",";
  body += "\"remainingSec\":" + String(warming ? warmupRemainingSeconds() : 0) + ",";
  String statusText = warming ? String("Precalentando") : getStatus(relativeIndex);
  body += "\"status\":\"" + statusText + "\",";
  body += "\"deviceTime\":\"" + deviceTime() + "\"}";
  int code = https.POST(body);
  String response = https.getString();
  Serial.printf("POST %d: %s\n", code, response.c_str());
  https.end();
  return code >= 200 && code < 300;
}

void getRemoteConfig() {
  if (WiFi.status() != WL_CONNECTED) return;
  BearSSL::WiFiClientSecure client;
  client.setInsecure(); // Para prototipo. En producción, validar certificado raíz.
  HTTPClient https;
  String url = String(API_BASE_URL) + "/.netlify/functions/reading";
  if (!https.begin(client, url)) return;
  int code = https.GET();
  if (code == HTTP_CODE_OK) {
    String payload = https.getString();
    DynamicJsonDocument doc(1536);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["config"].is<JsonObject>()) {
      JsonObject cfg = doc["config"];
      warningLevel = cfg["warning"] | 35.0;
      highLevel = cfg["high"] | 65.0;
      criticalLevel = cfg["critical"] | 85.0;
      alarmMode = String((const char*)(cfg["mode"] | "auto"));
      int remoteHours = cfg["warmupHours"] | 48;
      unsigned long commandId = cfg["warmupCommand"] | 0UL;
      if (!warmupCommandInitialized || commandId != lastWarmupCommand) {
        startWarmup(remoteHours, commandId);
        warmupCommandInitialized = true;
      }
      Serial.printf("Config: %s %.0f/%.0f/%.0f | warmup %d h\n", alarmMode.c_str(), warningLevel, highLevel, criticalLevel, warmupHours);
    }
  }
  https.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerOff();

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("No se detecto OLED. Verifique direccion/cableado.");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.println("VitaSense iniciando...");
    display.display();
  }

  warmupStartedAt = millis(); // Si no hay orden remota, comienza con el valor inicial de 48 h.
  warmupDurationMs = 48UL * 60UL * 60UL * 1000UL;
  sensorWarming = true;
  connectWiFi();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED && now % 10000 < 50) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    rawValue = readMQ3();
    sensorWarming = isWarming();
    if (sensorWarming) relativeIndex = 0;
    else relativeIndex = calculateIndex(rawValue);
    drawOLED();
    if (sensorWarming) {
      Serial.printf("ADC=%d | PRECALENTANDO | quedan %lu s\n", rawValue, warmupRemainingSeconds());
    } else {
      Serial.printf("ADC=%d | indice=%.1f | estado=%s\n",
                    rawValue, relativeIndex, getStatus(relativeIndex).c_str());
    }
  }

  if (sensorWarming && !isWarming()) {
    sensorWarming = false;
    sensorBaseline = rawValue; // establecer referencia al terminar el calentamiento
    relativeIndex = 0;
    Serial.println("Precalentamiento terminado. Sensor habilitado.");
  }
  updateBuzzer(relativeIndex);

  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;
    postReading();
  }

  if (now - lastConfig >= CONFIG_INTERVAL) {
    lastConfig = now;
    getRemoteConfig();
  }

  delay(10);
}
