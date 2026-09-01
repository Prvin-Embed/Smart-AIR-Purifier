# 🌬️ Smart Air Purifier — by Parveen

An ESP8266-based intelligent air purifier system with real-time particulate matter sensing, temperature/humidity monitoring, automatic fan speed control, RGB LED air-quality feedback, OLED display, and a responsive Wi-Fi web dashboard.

---

## ✨ Features

- **Real-time AQI Monitoring** — PMS7003 laser particle sensor reads PM1.0, PM2.5, and PM10 every second
- **Climate Sensing** — DHT22 measures ambient temperature (°C) and relative humidity (%)
- **Auto Fan Speed Control** — Fan speed adapts automatically based on PM2.5 levels (30% → 100%)
- **RGB LED Feedback** — 16x NeoPixel ring changes color (Green / Orange / Red) based on air quality
- **Breathing Effect** — Sinusoidal LED pulse animation for aesthetic indication
- **Rainbow Mode** — Cycling RGB animation for decoration mode
- **OLED Display (128×64)** — 3 swipeable pages: Environment, Pollution data, Network info
- **Touch Button** — Capacitive touch (A0) for page cycling and factory reset
- **Wi-Fi Web Dashboard** — Live JSON API (`/get`) with fan and LED controls
- **Captive Portal Setup** — First-time WiFi configuration via AP mode
- **EEPROM Persistence** — Saves Wi-Fi credentials across reboots
- **mDNS** — Accessible via `http://Parveen-Purifier.local`

---

## 🔧 Hardware Components

| Component | Description |
|-----------|-------------|
| ESP8266 (NodeMCU) | Main microcontroller with Wi-Fi |
| PMS7003 | Laser particle sensor (PM1.0, PM2.5, PM10) |
| DHT22 (AM2302) | Temperature & humidity sensor |
| SSD1306 OLED | 128×64 I2C display |
| WS2812B NeoPixel Ring | 16-LED RGB ring for visual feedback |
| DC Fan (PWM) | L293D/H-bridge controlled fan |
| Capacitive Touch Pad | A0 analog input for UI interaction |

---

## 📌 Pin Configuration

| Pin | GPIO | Function |
|-----|------|----------|
| D7 | GPIO 13 | NeoPixel LED Data |
| D8 | GPIO 15 | Fan ENA (PWM) |
| D0 | GPIO 16 | Fan IN1 |
| D3 | GPIO 0 | Fan IN2 |
| D4 | GPIO 2 | DHT22 Data |
| D5 | GPIO 14 | PMS7003 RX (SoftSerial) |
| D6 | GPIO 12 | PMS7003 TX (SoftSerial) |
| A0 | ADC | Touch input / Factory Reset |
| SDA/SCL | I2C | SSD1306 OLED |

---

## 📡 Sensor Specifications

| Sensor | Parameter | Range | Accuracy | Interface |
|--------|-----------|-------|----------|-----------|
| PMS7003 | PM1.0 | 0–500 µg/m³ | ±10 µg/m³ (0–100), ±10% (100–500) | UART 9600 |
| PMS7003 | PM2.5 | 0–500 µg/m³ | ±10 µg/m³ (0–100), ±10% (100–500) | UART 9600 |
| PMS7003 | PM10 | 0–500 µg/m³ | ±10 µg/m³ (0–100), ±10% (100–500) | UART 9600 |
| DHT22 | Temperature | -40°C to +80°C | ±0.5°C | Single-wire GPIO |
| DHT22 | Humidity | 0–100% RH | ±2–5% RH | Single-wire GPIO |

> Full specs with resolution, voltage, and notes are in [`sensor_specifications.csv`](./sensor_specifications.csv)

---

## 🚦 Air Quality Thresholds (PM2.5)

| PM2.5 Level | Status | LED Color | Fan Speed |
|-------------|--------|-----------|----------|
| 0–12 µg/m³ | 🟢 GOOD | Green | 30% |
| 13–35 µg/m³ | 🟡 MODERATE | Orange | 60% |
| 36–55 µg/m³ | 🔴 HAZARDOUS | Red | 80% |
| >55 µg/m³ | 🔴 HAZARDOUS | Red | 100% |

---

## 🌐 Web API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Responsive dashboard UI |
| `/get` | GET | JSON sensor data |
| `/set?m=` | GET | Set LED mode (0=Manual, 1=Rainbow, 3=Auto) |
| `/set?fa=` | GET | Fan auto mode (1=on, 0=off) |
| `/set?fs=` | GET | Set fan speed % (0–100) |
| `/set?r=&g=&b=` | GET | Set LED color (manual mode) |
| `/set?br=` | GET | Set LED brightness (0–255) |
| `/set?bm=` | GET | Breathing effect (1=on, 0=off) |

**JSON Response Example (`/get`):**
```json
{"pm1":5,"pm25":12,"pm10":18,"t":27.3,"h":55.2,"m":3,"br":128,"bm":0,"fa":1,"fs":30}
```

---

## 🔌 Libraries Required

Install via Arduino Library Manager:

```
ESP8266WiFi         (built-in ESP8266 core)
ESP8266WebServer    (built-in ESP8266 core)
ESP8266mDNS         (built-in ESP8266 core)
DNSServer           (built-in ESP8266 core)
Adafruit NeoPixel
Adafruit GFX Library
Adafruit SSD1306
DHT sensor library  (by Adafruit)
EEPROM              (built-in)
SoftwareSerial      (built-in)
```

---

## 🚀 Getting Started

1. **Clone / Download** this repository
2. Open `Parveen_Smart_Air_Purifier_1.ino` in Arduino IDE
3. Install all required libraries (see above)
4. Select **NodeMCU 1.0 (ESP-12E Module)** as board
5. Upload the firmware
6. On first boot, connect to AP: `PARVEEN AIR PURIFIER`
7. Open browser → `192.168.4.1` → Enter your Wi-Fi credentials
8. Device reboots and connects. Access dashboard at `http://Parveen-Purifier.local`

---

## 🏭 Factory Reset

Hold the **Touch Pin (A0)** HIGH (>500 ADC) during power-on to erase stored Wi-Fi credentials and restart in AP mode.

---

## 📁 Repository Structure

```
Smart-AIR-Purifier/
├── Parveen_Smart_Air_Purifier_1.ino   # Main firmware
├── sensor_specifications.csv           # Detailed sensor specs table
└── README.md                           # This file
```

---

## 👨‍💻 Author

**Parveen** — Embedded Systems & IoT Developer  
ESP8266 · Sensors · MQTT · Telemetry · Aerospace Systems

---

## 📄 License

MIT License — Free to use, modify, and distribute with attribution.
