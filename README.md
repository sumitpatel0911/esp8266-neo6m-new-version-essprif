🚀 PILAB GPS + RFID + Firebase + MOSFET Control
🔥 ESP8266 • GPS Tracking • RFID Authentication • Firebase RTDB • Captive Portal UI

This project converts an ESP8266 (NodeMCU) into a smart GPS + RFID-based access controller with Firebase logging, one-time configuration upload, MOSFET control, and a beautiful iOS-style WiFi setup portal.

✨ Features

📡 GPS tracking (NEO-6M)

🏷️ RFID detection & UID matching (RC522)

🔥 Firebase Realtime Database sync

💾 EEPROM configuration storage

🌐 Captive Portal with iOS-style UI

⚡ MOSFET control based on scanned card

🔔 Buzzer + LED feedback

🔁 One-time config push to Firebase

🔧 Factory reset button

🗂️ System Overview
ESP8266
 ├── GPS (NEO-6M)
 ├── RFID (RC522)
 ├── Firebase RTDB
 ├── Captive Portal Config UI
 ├── EEPROM Stored Config
 └── MOSFET Output Control

🪛 Hardware Connections
📡 GPS (NEO-6M)
GPS Pin	ESP8266
TX	D1
RX	D2
🏷️ RFID RC522
RC522	ESP8266
SDA	D8 (GPIO15)
SCK	D5 (GPIO14)
MOSI	D7 (GPIO13)
MISO	D6 (GPIO12)
RST	D3 (GPIO0)
3.3V	3.3V
GND	GND
⚡ MOSFET Output

Gate → GPIO5

Drain → Load

Source → GND

🔔 Indicators
Component	Pin
Red LED	GPIO10
Green LED	GPIO16
Buzzer	GPIO4
Reset Button	GPIO2
🔥 Firebase Structure
On First-Time Config Upload:
/data/<veh>/rtmp1
/data/<veh>/rtmp2
/data/<veh>/rtmp3
/data/<veh>/rtmp4
/data/<veh>/imei
/data/<veh>/vehnum
/data/<veh>/timestampz

RFID Data
/data/<veh>/rfid_data.json


Example:

{
  "uid1": "AB CD EF",
  "uid2": "11 22 33",
  "uid3": "",
  "uid4": "",
  "current": "AB CD EF",
  "status": "1"
}

🔌 MOSFET Logic
UID	Action
uid1	MOSFET ON
uid2	MOSFET OFF
Others	No action
📡 GPS Upload JSON
{
  "UID": "12 AB 34 CD",
  "lat": "22.123456",
  "lon": "72.654321",
  "alt": "55.3",
  "speed": "31.2",
  "sat": "7",
  "time": "13:55:21",
  "date": "03-02-2025"
}

🌐 Captive Portal Setup

When no config exists:

SSID: PILAB-GPS
PASS: pilab123


The portal allows you to set:

Vehicle Number

IMEI

RTMP URLs

WiFi SSID (auto-scanned list)

WiFi Password

After saving → device automatically reboots.

🎛️ LED/Buzzer Behavior
LED/Buzzer	Meaning
🔴 Red Solid	No config (Setup mode)
🟢 Green Solid	Config OK
🟡 Yellow	No GPS fix
🟢 Green Blink	Normal tracking
🟢 + 🔔 Long	RFID matched
🟢 + 🔔 Short	Card detected
🔁 Factory Reset

Hold the reset button (GPIO2) for 3 seconds:

Clears EEPROM

Reboots

Enters Captive Portal mode

📥 Installation

Install libraries:

TinyGPSPlus

MFRC522

ESP8266WiFi

DNSServer

EEPROM

HTTPClient

Upload the sketch to ESP8266

Connect to PILAB-GPS WiFi

Configure in portal

Reboot → system starts working 🎉

❤️ Credits

Built with ❤️ for PILAB.
