#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

const char* ssid        = "JY24";
const char* password    = "juanfe24";
const char* mqtt_server = "10.106.70.64";
const int   mqtt_port   = 1883;
const char* mqtt_user   = "iot_device";
const char* mqtt_pass   = "SecurePass123!";
const char* TOPIC_CLIMA  = "casa/clima";
const char* TOPIC_ACCESO = "casa/acceso";

#define DHTPIN      16
#define DHTTYPE     DHT22
#define RST_PIN     27
#define SS_PIN      5
#define SERVO_PIN   14
#define MOTOR_ENA   15
#define MOTOR_IN3   32
#define MOTOR_IN4   33
#define BUZZER_PIN  17
#define SCK_PIN     18
#define MISO_PIN    19
#define MOSI_PIN    23
#define PWM_FREQ    5000
#define PWM_RES     8

DHT               dht(DHTPIN, DHTTYPE);
MFRC522           mfrc522(SS_PIN, RST_PIN);
Servo             miPuerta;
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient        espClient;
PubSubClient      mqttClient(espClient);

SemaphoreHandle_t xMutexLCD;
SemaphoreHandle_t xMutexDHT;
QueueHandle_t     xQueueMQTT;
TaskHandle_t      hBuzzer  = NULL;
TaskHandle_t      hServo   = NULL; // ✅ Task dedicada al servo

#define NUM_USUARIOS 2
String usuariosAutorizados[NUM_USUARIOS] = {
  "a3f2bc1d",
  "73896ba7"
};

struct MQTTMessage {
  char topic[50];
  char payload[256];
};

void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Conectando");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    if (++intentos > 40) ESP.restart();
  }
  Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());
}

void publicarMQTT(const char* topic, const char* payload) {
  MQTTMessage msg;
  strncpy(msg.topic,   topic,   sizeof(msg.topic)   - 1);
  strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
  msg.topic[sizeof(msg.topic)   - 1] = '\0';
  msg.payload[sizeof(msg.payload) - 1] = '\0';
  xQueueSend(xQueueMQTT, &msg, portMAX_DELAY);
}

bool esAutorizado(const String& uid) {
  for (int i = 0; i < NUM_USUARIOS; i++)
    if (uid.equalsIgnoreCase(usuariosAutorizados[i])) return true;
  return false;
}

bool leerDHT(float& t, float& h) {
  if (xSemaphoreTake(xMutexDHT, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
  for (int i = 0; i < 3; i++) {
    h = dht.readHumidity();
    t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
      xSemaphoreGive(xMutexDHT);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  xSemaphoreGive(xMutexDHT);
  return false;
}

// ── Task: Servo dedicada en Core 0 ───────────────────────
// Recibe notificación: 1 = abrir, 0 = nada
void taskServo(void* pvParameters) {
  Serial.println("[Servo] Task lista");

  while (true) {
    // Espera señal de abrir
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Serial.println("[Servo] Abriendo...");
    miPuerta.write(90);
    vTaskDelay(pdMS_TO_TICKS(4000));
    miPuerta.write(0);
    Serial.println("[Servo] Cerrado");
  }
}

// ── Task: Buzzer ─────────────────────────────────────────
void taskBuzzer(void* pvParameters) {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  while (true) {
    uint32_t tipo = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (tipo == 1) {
      digitalWrite(BUZZER_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(200));
      digitalWrite(BUZZER_PIN, LOW);
    } else if (tipo == 3) {
      for (int i = 0; i < 3; i++) {
        digitalWrite(BUZZER_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(150));
        digitalWrite(BUZZER_PIN, LOW);  vTaskDelay(pdMS_TO_TICKS(150));
      }
    }
  }
}

// ── Task: MQTT ───────────────────────────────────────────
void taskMQTT(void* pvParameters) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) setup_wifi();
    if (!mqttClient.connected()) {
      Serial.print("[MQTT] Conectando...");
      if (mqttClient.connect("ESP32_Casa", mqtt_user, mqtt_pass)) {
        Serial.println(" OK");
      } else {
        Serial.printf(" FALLO rc=%d\n", mqttClient.state());
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }
    mqttClient.loop();
    MQTTMessage msg;
    if (xQueueReceive(xQueueMQTT, &msg, 0) == pdTRUE) {
      mqttClient.publish(msg.topic, msg.payload);
      Serial.printf("[MQTT→] %s : %s\n", msg.topic, msg.payload);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── Task: RFID ───────────────────────────────────────────
void taskRFID(void* pvParameters) {
  while (true) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {

      String uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(mfrc522.uid.uidByte[i], HEX);
      }
      Serial.println("[RFID] UID: " + uid);

      StaticJsonDocument<200> doc;
      doc["uid"] = uid;

      if (esAutorizado(uid)) {
        Serial.println("[ACCESO] Permitido");

        // ✅ Notifica a la task dedicada del servo
        if (hServo) xTaskNotify(hServo, 1, eSetValueWithOverwrite);
        if (hBuzzer) xTaskNotify(hBuzzer, 1, eSetValueWithOverwrite);

        float t = 0, h = 0;
        leerDHT(t, h);

        doc["acceso"]      = "permitido";
        doc["temperatura"] = t;
        doc["humedad"]     = h;

        if (xSemaphoreTake(xMutexLCD, pdMS_TO_TICKS(200))) {
          lcd.clear();
          lcd.setCursor(0, 0); lcd.print("Bienvenido!");
          lcd.setCursor(0, 1); lcd.printf("T:%.1fC H:%.0f%%", t, h);
          xSemaphoreGive(xMutexLCD);
        }

        vTaskDelay(pdMS_TO_TICKS(4500)); // Espera a que el servo cierre

      } else {
        Serial.println("[ACCESO] Denegado");
        doc["acceso"] = "denegado";

        if (hBuzzer) xTaskNotify(hBuzzer, 3, eSetValueWithOverwrite);

        if (xSemaphoreTake(xMutexLCD, pdMS_TO_TICKS(200))) {
          lcd.clear();
          lcd.setCursor(0, 0); lcd.print("Acceso denegado");
          lcd.setCursor(0, 1); lcd.print(uid);
          xSemaphoreGive(xMutexLCD);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
      }

      if (xSemaphoreTake(xMutexLCD, pdMS_TO_TICKS(200))) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Casa Inteligente");
        lcd.setCursor(0, 1); lcd.print("Esperando...");
        xSemaphoreGive(xMutexLCD);
      }

      String payload;
      serializeJson(doc, payload);
      publicarMQTT(TOPIC_ACCESO, payload.c_str());

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ── Task: Clima ──────────────────────────────────────────
void taskClima(void* pvParameters) {
  float t = 0, h = 0;
  while (true) {
    if (leerDHT(t, h)) {
      int pwm = 0;
      if (t >= 20) {
        pwm = map((int)t, 20, 40, 60, 255);
        pwm = constrain(pwm, 60, 255);
      }

      if (pwm > 0) {
        digitalWrite(MOTOR_IN3, HIGH);
        digitalWrite(MOTOR_IN4, LOW);
        ledcWrite(MOTOR_ENA, pwm);
      } else {
        digitalWrite(MOTOR_IN3, LOW);
        digitalWrite(MOTOR_IN4, LOW);
        ledcWrite(MOTOR_ENA, 0);
      }

      Serial.printf("[CLIMA] T:%.1fC  H:%.0f%%  PWM:%d\n", t, h, pwm);

      StaticJsonDocument<128> doc;
      doc["temperatura"] = t;
      doc["humedad"]     = h;
      doc["pwm_motor"]   = pwm;
      String payload;
      serializeJson(doc, payload);
      publicarMQTT(TOPIC_CLIMA, payload.c_str());

    } else {
      Serial.println("[CLIMA ERROR] DHT no responde");
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ── Setup ────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(MOTOR_IN3, OUTPUT);
  pinMode(MOTOR_IN4, OUTPUT);
  digitalWrite(MOTOR_IN3, LOW);
  digitalWrite(MOTOR_IN4, LOW);
  // En setup(), ANTES de ledcAttach del motor:
  ESP32PWM::allocateTimer(0); // Timer 0 → reservado para servos
  ESP32PWM::allocateTimer(1); // Timer 1 → reservado para servos

  miPuerta.setPeriodHertz(50); // Frecuencia estándar de servo
  miPuerta.attach(SERVO_PIN, 500, 2400);
  miPuerta.write(0);

  // Motor usa timer 2 en adelante, sin conflicto
  ledcAttach(MOTOR_ENA, PWM_FREQ, PWM_RES);
  ledcWrite(MOTOR_ENA, 0);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  Serial.println("[RFID] Listo");

  dht.begin();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Casa Inteligente");
  lcd.setCursor(0, 1); lcd.print("Iniciando...");

  setup_wifi();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(512);

  xMutexLCD  = xSemaphoreCreateMutex();
  xMutexDHT  = xSemaphoreCreateMutex();
  xQueueMQTT = xQueueCreate(10, sizeof(MQTTMessage));

  miPuerta.attach(SERVO_PIN, 500, 2400);
  miPuerta.write(0);
  // ✅ Servo en Core 0 junto con MQTT, lejos del RFID y Clima
  xTaskCreatePinnedToCore(taskServo,  "Servo",  2048, NULL, 3, &hServo,  0);
  xTaskCreatePinnedToCore(taskMQTT,   "MQTT",   4096, NULL, 2, NULL,     0);
  xTaskCreatePinnedToCore(taskRFID,   "RFID",   4096, NULL, 1, NULL,     1);
  xTaskCreatePinnedToCore(taskClima,  "Clima",  4096, NULL, 1, NULL,     1);
  xTaskCreatePinnedToCore(taskBuzzer, "Buzzer", 2048, NULL, 1, &hBuzzer, 1);

  lcd.setCursor(0, 1); lcd.print("Listo!          ");
  Serial.println("[SETUP] OK");
}

void loop() {}