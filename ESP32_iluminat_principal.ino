#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

const char* WIFI_SSID     = "Licenta";
const char* WIFI_PASSWORD = "12345678";
const char* MQTT_SERVER   = "10.225.79.220";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";
const char* MQTT_PASS     = "";
const char* MQTT_CLIENT   = "ESP32_Lighting";

const char* TOPIC_PUB_POWER    = "home/power/ina219";
const char* TOPIC_PUB_CAM1     = "home/cam1/status";
const char* TOPIC_PUB_CAM2     = "home/cam2/status";
const char* TOPIC_SUB_CMD_CAM1 = "home/cam1/cmd";
const char* TOPIC_SUB_CMD_CAM2 = "home/cam2/cmd";

#define PIN_PIR_CAM1   18
#define PIN_PIR_CAM2   19
#define PIN_LED_CAM1   26
#define PIN_LED_CAM2   27
#define PIN_LDR_CAM1   33
#define PIN_LDR_CAM2   32

#define PWM_FRECVENTA        5000
#define PWM_BITI             8
#define PWM_MINIM_PROCENT    20
#define PWM_BLOCAT_PROCENT    2

#define TIMP_RETINERE_MISCARE_MS   15000
#define INTERVAL_PUBLICARE_MS       2000

#define VR_CMD_RECOGNIZED  0x0D
#define VR_RECORD_PORNIRE  0
#define VR_RECORD_OPRIRE   1

#define VR_BUF_MAX  64
uint8_t vrBuf[VR_BUF_MAX];
int     vrBufLen = 0;

enum VrState { VR_WAIT_AA, VR_READ_LEN, VR_READ_DATA, VR_READ_END };
VrState vrState   = VR_WAIT_AA;
int     vrToRead  = 0;
int     vrIdx     = 0;

Adafruit_INA219 ina219;
WiFiClient      wifiClient;
PubSubClient    mqtt(wifiClient);

bool          cam1Pornita     = false;
bool          cam2Pornita     = false;
bool          cam1Blocata     = false;
bool          cam2Blocata     = false;
unsigned long cam1TimpMiscare = 0;
unsigned long cam2TimpMiscare = 0;
unsigned long ultimaPublicare = 0;

void conecteazaWiFi();
void conecteazaMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int lungime);
void gestioneazaCamera(int nrCamera, int pinPIR, int pinLED, int pinLDR,
                       bool& pornita, bool& blocata, unsigned long& timpMiscare);
int  calculeazaPWM(int pinLDR);
void seteazaPWM(int pinLED, int procent);
void publicaStatus();
void publicaConsum();
void proceseazaVoce();
void proceseazaFrameVocalComplet();
void vrTrimiteLoad();

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n========================================");
  Serial.println(" ESP32 Iluminat Inteligent - Pornire...");
  Serial.println("========================================");

  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  while (Serial2.available()) Serial2.read();
  Serial.println("[VOCAL] Serial2: RX=GPIO16, TX=GPIO17, 9600 baud");

  pinMode(PIN_PIR_CAM1, INPUT);
  pinMode(PIN_PIR_CAM2, INPUT);
  Serial.println("[PIR] Camera1=GPIO18, Camera2=GPIO19");

  ledcAttach(PIN_LED_CAM1, PWM_FRECVENTA, PWM_BITI);
  ledcAttach(PIN_LED_CAM2, PWM_FRECVENTA, PWM_BITI);
  ledcWrite(PIN_LED_CAM1, 0);
  ledcWrite(PIN_LED_CAM2, 0);
  Serial.println("[LED] PWM: Camera1=GPIO26, Camera2=GPIO27");

  analogReadResolution(12);
  Serial.println("[LDR] ADC 12 biti: Camera1=GPIO33, Camera2=GPIO32");

  Wire.begin();
  if (!ina219.begin()) {
    Serial.println("[INA219] EROARE: Nu gasit! Verifica I2C SDA=21, SCL=22");
  } else {
    Serial.println("[INA219] OK");
  }

  conecteazaWiFi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  conecteazaMQTT();

  vrTrimiteLoad();

  Serial.println("[SETUP] Gata. Record 0=Pornire, Record 1=Oprire (ambele camere)");
  Serial.println("----------------------------------------");
}

void loop() {
  if (!mqtt.connected()) conecteazaMQTT();
  mqtt.loop();

  proceseazaVoce();

  gestioneazaCamera(1, PIN_PIR_CAM1, PIN_LED_CAM1, PIN_LDR_CAM1,
                    cam1Pornita, cam1Blocata, cam1TimpMiscare);
  gestioneazaCamera(2, PIN_PIR_CAM2, PIN_LED_CAM2, PIN_LDR_CAM2,
                    cam2Pornita, cam2Blocata, cam2TimpMiscare);

  unsigned long acum = millis();
  if (acum - ultimaPublicare >= INTERVAL_PUBLICARE_MS) {
    ultimaPublicare = acum;
    publicaStatus();
    publicaConsum();
  }
}

void vrTrimiteLoad() {
  Serial.println("[VOCAL] Trimit Load recorduri 0,1...");
  uint8_t frame[] = {0xAA, 0x04, 0x30, 0x00, 0x01, 0x0A};
  for (int i = 0; i < sizeof(frame); i++) {
    Serial2.write(frame[i]);
    delayMicroseconds(800);
  }
  Serial2.flush();
  delay(200);
  unsigned long t = millis();
  while (millis() - t < 500) {
    if (Serial2.available()) Serial2.read();
    delay(1);
  }
  Serial.println("[VOCAL] Load trimis. Modulul asculta comenzile.");
}

void proceseazaVoce() {
  while (Serial2.available()) {
    uint8_t b = Serial2.read();

    switch (vrState) {
      case VR_WAIT_AA:
        if (b == 0xAA) {
          memset(vrBuf, 0, VR_BUF_MAX);
          vrBuf[0] = 0xAA;
          vrIdx   = 1;
          vrState = VR_READ_LEN;
        }
        break;

      case VR_READ_LEN:
        vrBuf[1] = b;
        if (b < 2 || b > VR_BUF_MAX - 3) {
          Serial.printf("[VOCAL] LEN invalid=0x%02X, resetez\n", b);
          vrState = VR_WAIT_AA;
        } else {
          vrIdx    = 2;
          vrToRead = b - 1;
          vrState  = VR_READ_DATA;
        }
        break;

      case VR_READ_DATA:
        vrBuf[vrIdx++] = b;
        vrToRead--;
        if (vrToRead == 0) {
          vrState = VR_READ_END;
        }
        break;

      case VR_READ_END:
        vrBuf[vrIdx++] = b;
        vrBufLen = vrIdx;
        vrState  = VR_WAIT_AA;
        proceseazaFrameVocalComplet();
        break;
    }
  }
}

void proceseazaFrameVocalComplet() {
  Serial.print("[VOCAL DEBUG] Frame:");
  for (int i = 0; i < vrBufLen; i++) Serial.printf(" %02X", vrBuf[i]);

  String text = "";
  for (int i = 3; i < vrBufLen; i++) {
    uint8_t b = vrBuf[i];
    if (b == 0x0D || b == 0x0A) break;
    if (b >= 0x20 && b <= 0x7E) text += (char)b;
  }
  if (text.length() > 0) Serial.printf("  [\"%s\"]", text.c_str());
  Serial.println();

  uint8_t cmd = vrBuf[2];
  if (cmd != VR_CMD_RECOGNIZED) {
    Serial.printf("[VOCAL] CMD=0x%02X ignorat\n", cmd);
    return;
  }

  if (vrBufLen < 7) {
    Serial.println("[VOCAL] Frame recunoastere prea scurt, ignorat");
    return;
  }

  uint8_t record = vrBuf[5];
  Serial.printf("[VOCAL] Comanda recunoscuta: Record %d\n", record);

  switch (record) {
    case VR_RECORD_PORNIRE:
      cam1Pornita = true;
      cam2Pornita = true;
      cam1TimpMiscare = 0;
      cam2TimpMiscare = 0;
      seteazaPWM(PIN_LED_CAM1, PWM_MINIM_PROCENT);
      seteazaPWM(PIN_LED_CAM2, PWM_MINIM_PROCENT);
      Serial.println("[VOCAL] RECORD 0 -> AMBELE camere PORNITE (20%)");
      publicaStatus();
      break;

    case VR_RECORD_OPRIRE:
      cam1Pornita = false;
      cam2Pornita = false;
      cam1TimpMiscare = 0;
      cam2TimpMiscare = 0;
      seteazaPWM(PIN_LED_CAM1, 0);
      seteazaPWM(PIN_LED_CAM2, 0);
      Serial.println("[VOCAL] RECORD 1 -> AMBELE camere OPRITE");
      publicaStatus();
      break;

    default:
      Serial.printf("[VOCAL] Record %d - neasignat\n", record);
      break;
  }
}

void gestioneazaCamera(int nrCamera, int pinPIR, int pinLED, int pinLDR,
                       bool& pornita, bool& blocata, unsigned long& timpMiscare) {
  bool miscare = digitalRead(pinPIR);
  unsigned long acum = millis();

  // MOD BLOCAT: PIR ignorat complet, lumina ramane la 20% daca e pornita
  if (blocata) {
    seteazaPWM(pinLED, pornita ? PWM_MINIM_PROCENT : 0);
    return;
  }

  if (!pornita) {
    seteazaPWM(pinLED, 0);
    return;
  }

  if (miscare) {
    bool eraLiniste = (timpMiscare == 0) || ((acum - timpMiscare) >= TIMP_RETINERE_MISCARE_MS);
    if (eraLiniste) {
      Serial.printf("[CAMERA %d] Miscare detectata! Cresc luminozitatea.\n", nrCamera);
    }
    timpMiscare = acum;
  }

  bool esteInMiscare = (timpMiscare > 0) && ((acum - timpMiscare) < TIMP_RETINERE_MISCARE_MS);

  if (esteInMiscare) {
    int pwmVal = calculeazaPWM(pinLDR);
    ledcWrite(pinLED, pwmVal);
  } else {
    seteazaPWM(pinLED, PWM_MINIM_PROCENT);
  }
}

int calculeazaPWM(int pinLDR) {
  int valLDR = analogRead(pinLDR);
  float intuneric = (float)valLDR / 4095.0f;
  float curba = intuneric * intuneric;
  int pwmVal = (int)(51.0f + curba * 204.0f);
  return constrain(pwmVal, 51, 255);
}

void seteazaPWM(int pinLED, int procent) {
  int valPWM = constrain((int)(procent / 100.0f * 255), 0, 255);
  ledcWrite(pinLED, valPWM);
}

void publicaStatus() {
  unsigned long acum = millis();
  bool pir1 = digitalRead(PIN_PIR_CAM1);
  bool pir2 = digitalRead(PIN_PIR_CAM2);
  int  ldr1 = analogRead(PIN_LDR_CAM1);
  int  ldr2 = analogRead(PIN_LDR_CAM2);

  bool inMiscare1 = cam1TimpMiscare > 0 && (acum - cam1TimpMiscare) < TIMP_RETINERE_MISCARE_MS;
  bool inMiscare2 = cam2TimpMiscare > 0 && (acum - cam2TimpMiscare) < TIMP_RETINERE_MISCARE_MS;

  int pct1 = (ledcRead(PIN_LED_CAM1) * 100) / 255;
  int pct2 = (ledcRead(PIN_LED_CAM2) * 100) / 255;

  char buf[256];

  snprintf(buf, sizeof(buf),
    "{\"on\":%s,\"locked\":%s,\"motion\":%s,\"pir_raw\":%s,\"ldr\":%d,\"pwm_pct\":%d}",
    cam1Pornita ? "true" : "false",
    cam1Blocata ? "true" : "false",
    (pir1 || inMiscare1) ? "true" : "false",
    pir1 ? "true" : "false",
    ldr1, pct1);
  mqtt.publish(TOPIC_PUB_CAM1, buf);
  Serial.printf("[MQTT PUB] %s -> %s\n", TOPIC_PUB_CAM1, buf);

  snprintf(buf, sizeof(buf),
    "{\"on\":%s,\"locked\":%s,\"motion\":%s,\"pir_raw\":%s,\"ldr\":%d,\"pwm_pct\":%d}",
    cam2Pornita ? "true" : "false",
    cam2Blocata ? "true" : "false",
    (pir2 || inMiscare2) ? "true" : "false",
    pir2 ? "true" : "false",
    ldr2, pct2);
  mqtt.publish(TOPIC_PUB_CAM2, buf);
  Serial.printf("[MQTT PUB] %s -> %s\n", TOPIC_PUB_CAM2, buf);
}

void publicaConsum() {
  float tensiuneBus   = ina219.getBusVoltage_V()*1;
  float tensiuneShunt = ina219.getShuntVoltage_mV()*1;
  float curent        = ina219.getCurrent_mA()*-1;
  float putere        = ina219.getPower_mW()*1;

  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"voltage_V\":%.3f,\"shunt_mV\":%.3f,\"current_mA\":%.3f,\"power_mW\":%.3f}",
    tensiuneBus, tensiuneShunt, curent, putere);
  mqtt.publish(TOPIC_PUB_POWER, buf);
  Serial.printf("[MQTT PUB] %s -> %s\n", TOPIC_PUB_POWER, buf);
}

void callbackMQTT(char* topic, byte* payload, unsigned int lungime) {
  char mesaj[16];
  if (lungime == 0 || lungime >= sizeof(mesaj)) {
    Serial.printf("[MQTT SUB] Payload invalid pe %s\n", topic);
    return;
  }
  memcpy(mesaj, payload, lungime);
  mesaj[lungime] = '\0';
  Serial.printf("[MQTT SUB] %s -> \"%s\"\n", topic, mesaj);

  if (strcmp(topic, TOPIC_SUB_CMD_CAM1) == 0) {
    cam1Blocata = (strcmp(mesaj, "1") == 0);
    Serial.printf("[MQTT] Camera 1: %s\n",
      cam1Blocata ? "BLOCAT (PIR ignorat, 20%)" : "NORMAL (miscare activa)");
  }
  else if (strcmp(topic, TOPIC_SUB_CMD_CAM2) == 0) {
    cam2Blocata = (strcmp(mesaj, "1") == 0);
    Serial.printf("[MQTT] Camera 2: %s\n",
      cam2Blocata ? "BLOCAT (PIR ignorat, 20%)" : "NORMAL (miscare activa)");
  }
}

void conecteazaWiFi() {
  Serial.printf("[WiFi] Conectare la: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tentative = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++tentative > 40) {
      Serial.println("\n[WiFi] EROARE: Timeout!");
      return;
    }
  }
  Serial.printf("\n[WiFi] Conectat! IP: %s\n", WiFi.localIP().toString().c_str());
}

void conecteazaMQTT() {
  int tentative = 0;
  while (!mqtt.connected()) {
    Serial.printf("[MQTT] Tentativa %d -> %s:%d ... ", ++tentative, MQTT_SERVER, MQTT_PORT);

    bool ok = (strlen(MQTT_USER) > 0)
      ? mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)
      : mqtt.connect(MQTT_CLIENT);

    if (ok) {
      Serial.println("CONECTAT!");
      mqtt.subscribe(TOPIC_SUB_CMD_CAM1);
      mqtt.subscribe(TOPIC_SUB_CMD_CAM2);
      Serial.printf("[MQTT] Subscris: %s, %s\n", TOPIC_SUB_CMD_CAM1, TOPIC_SUB_CMD_CAM2);
      mqtt.publish("home/esp32/status", "{\"status\":\"online\",\"device\":\"ESP32_Lighting\"}");
    } else {
      Serial.printf("ESEC (cod=%d). Reincerc in 3s...\n", mqtt.state());
      delay(3000);
    }

    if (tentative > 5) {
      Serial.println("[MQTT] Prea multe esecuri. Verifica IP broker.");
      break;
    }
  }
}
