# 🏠 Sistema Domótico de Control de Acceso y Climatización con ESP32

> Sistemas Embebidos 2026-01 — Universidad Autónoma de Manizales  
> Docente: Ernesto Guevara  
> Grupo 3: Juan Felipe Yepes Riobo | Isabela Silva Londoño | Juan Pablo Garcia Varela

---

## 📋 Descripción

Sistema domótico basado en el microcontrolador **ESP32** que integra:
- Control de acceso mediante tecnología **RFID (RC522)**
- Regulación automática de climatización por **PWM**
- Arquitectura de software multitarea sobre **FreeRTOS**
- Monitoreo remoto en tiempo real vía **MQTT** y dashboard en **Node-RED**
- Persistencia de datos en la nube con **Supabase**

---

## 🛠️ Hardware utilizado

| Componente | Función | Pin ESP32 |
|---|---|---|
| ESP32 | Microcontrolador principal | — |
| RFID RC522 | Lector de tarjetas de acceso | SDA=5, SCK=18, MISO=19, MOSI=23, RST=27 |
| DHT22 | Sensor de temperatura y humedad | GPIO 16 |
| Servomotor SG90 | Accionamiento de puerta | GPIO 14 |
| Motor DC | Ventilador controlado por PWM | ENA=15, IN3=32, IN4=33 |
| LCD 16x2 I2C | Visualización local | SDA=21, SCL=22 |
| Buzzer | Retroalimentación sonora | GPIO 17 |

---

## 💻 Software y herramientas

| Herramienta | Versión | Uso |
|---|---|---|
| ESP-IDF | v5.x | Framework de desarrollo |
| FreeRTOS | Incluido en ESP-IDF | Gestión multitarea |
| MQTT (Mosquitto) | 2.x | Comunicación IoT |
| Node-RED | 3.x | Dashboard de monitoreo |
| Supabase | — | Base de datos en la nube |
| Python 3 | 3.10+ | Script suscriptor MQTT |
| Wokwi | — | Simulación del circuito |

---

## 🧩 Arquitectura de tareas FreeRTOS
Core 0                          Core 1
┌─────────────────┐             ┌─────────────────┐
│  taskServo      │             │  taskRFID       │
│  Prioridad: 3   │             │  Prioridad: 1   │
│  Stack: 2048B   │             │  Stack: 4096B   │
└────────┬────────┘             └────────┬────────┘
│                               │
┌─────────────────┐             ┌─────────────────┐
│  taskMQTT       │             │  taskClima      │
│  Prioridad: 2   │◄────────────│  Prioridad: 1   │
│  Stack: 4096B   │ xQueueMQTT  │  Stack: 4096B   │
└─────────────────┘             └────────┬────────┘
│
┌─────────────────┐
│  taskBuzzer     │
│  Prioridad: 1   │
│  Stack: 2048B   │
└─────────────────┘

**Mecanismos de comunicación:**
- `xQueueMQTT` — taskRFID y taskClima → taskMQTT
- `xTaskNotify` — taskRFID → taskServo y taskBuzzer
- `xMutexLCD` — acceso exclusivo al LCD entre taskRFID y taskClima
- `xMutexDHT` — acceso exclusivo al DHT22

---

## 📡 Tópicos MQTT

| Tópico | Dirección | Publicado por | Frecuencia |
|---|---|---|---|
| `casa/clima` | ESP32 → Broker | taskClima | Cada 5s |
| `casa/acceso` | ESP32 → Broker | taskRFID | Por evento |

### Estructura de mensajes JSON

**casa/clima:**
```json{
"temperatura": 28.5,
"humedad": 65.2,
"pwm_motor": 120
}

**casa/acceso — permitido:**
```json{
"uid": "a3f2bc1d",
"acceso": "permitido",
"temperatura": 28.5,
"humedad": 65.2
}

**casa/acceso — denegado:**
```json{
"uid": "xxxxxxxx",
"acceso": "denegado"
}

---

# 🗄️ Base de datos (Supabase)

Tablas utilizadas:

**`climate_data`**
```sqlCREATE TABLE climate_data (
id          BIGSERIAL PRIMARY KEY,
temperatura FLOAT NOT NULL,
humedad     FLOAT NOT NULL,
pwm_motor   INT,
created_at  TIMESTAMPTZ DEFAULT NOW()
);

**`access_logs`**
```sqlCREATE TABLE access_logs (
id         BIGSERIAL PRIMARY KEY,
uid        TEXT NOT NULL,
acceso     TEXT NOT NULL,
created_at TIMESTAMPTZ DEFAULT NOW()
);

---
