🍄 IoT Arsitecture of Enoki Mushroom Cultivation Monitoring and Control System


ESP32 · DHT22 · MQTT · Fuzzy Logic · Hysteresis · Web Dashboard ·
Wokwi


IoT Arsitecture of Enoki Mushroom Cultivation Monitoring and Control System
adalah prototipe sistem monitoring dan kendali chamber budidaya jamur enoki 
yang menggabungkan mikrokontroler ESP32,sensor DHT22, komunikasi MQTT, fuzzy 
logic, hysteresis control, virtual phase management, dan cultivation-aware 
web dashboard.

Proyek ini dirancang untuk merepresentasikan kebutuhan kontrol
lingkungan berdasarkan fase budidaya, bukan hanya menampilkan data
sensor secara real-time. Setiap fase memiliki rentang suhu, kelembapan,
konfigurasi aktuator, dan durasi yang berbeda.

⚠️ Important: Versi repository yang menjadi dasar README ini masih
berorientasi pada simulasi Wokwi. Peltier dan fan heatsink
direpresentasikan menggunakan LED bargraph melalui 74HC595 untuk
menunjukkan level output 0--100%, sehingga implementasi ini tidak
boleh dianggap sebagai validasi performa aktuator fisik.



✨ Features

### Monitoring

🌡️ Monitoring suhu menggunakan DHT22.

💧 Monitoring kelembapan relatif.

🖥️ Tampilan lokal melalui OLED SSD1306.

📊 Grafik suhu dan kelembapan pada dashboard.

📋 Tabel histori data dan filter berdasarkan fase.



### Phase-Aware Cultivation

🌱 M1 --- Inkubasi

❄️ M2a --- Induksi

🌿 M2b --- Pemanjangan

🍄 M2c --- Fruiting

Setpoint berubah mengikuti fase aktif.

Durasi minimum dan maksimum fase dikelola oleh sistem.

Notifikasi diberikan ketika durasi minimum tercapai.

Pergantian fase otomatis dilakukan ketika durasi maksimum tercapai.

Hybrid Control

🧠 Fuzzy logic untuk kontrol daya Peltier.

🔄 Fuzzy cascade untuk fan heatsink Peltier.

💨 Hysteresis untuk mist maker.

⏱️ Rate limiter pada output Peltier dan fan heatsink.

🛡️ Sensor failsafe ketika pembacaan DHT22 gagal berturut-turut.

💡 Kontrol LED grow berdasarkan fase dan jam virtual.

🌬️ Kontrol exhaust fan berdasarkan konfigurasi fase.


### IoT & Dashboard

📡 Komunikasi MQTT dua arah.

🎛️ Pergantian fase melalui tombol fisik maupun dashboard.

📈 Monitoring status aktuator.

🗃️ Logging data selama runtime.

📤 Export data ke CSV dan XLSX.

🧪 Fallback ke simulasi sensor lokal ketika ESP32 belum terhubung.



🏗️ System Architecture

                         ┌───────────────────────┐
                         │        DHT22          │
                         │    Temperature / RH    │
                         └───────────┬───────────┘
                                     │
                                     ▼
┌───────────────┐              ┌───────────────┐
│ Mode Button   │─────────────▶│     ESP32     │
└───────────────┘              │               │
                               │ Phase Engine  │
                               │ Fuzzy Control │
                               │ Hysteresis    │
                               │ Rate Limiter  │
                               └───────┬───────┘
                                       │
                 ┌─────────────────────┼─────────────────────┐
                 │                     │                     │
                 ▼                     ▼                     ▼
          ┌─────────────┐       ┌─────────────┐       ┌─────────────┐
          │ OLED SSD1306│       │   Actuators │       │ MQTT Client │
          └─────────────┘       └──────┬──────┘       └──────┬──────┘
                                       │                     │
                          ┌────────────┼────────────┐        │
                          │            │            │        │
                          ▼            ▼            ▼        ▼
                     Peltier/FHS    Mist Maker  LED/Fan  MQTT Broker
                     (Bargraph)      (Relay)    (Relay)      │
                                                            │
                                                            ▼
                                                   ┌──────────────────┐
                                                   │  Web Dashboard   │
                                                   │ Monitoring / Log │
                                                   │ Control / Export │
                                                   └──────────────────┘



🧩 Project Components

Component       Role

ESP32           Main controller
DHT22           Temperature and humidity sensor
OLED SSD1306    Local status display
74HC595         Shift-register based actuator representation in Wokwi
LED Bargraph    Peltier & fan heatsink output visualization
Relay           Mist maker, LED grow, and exhaust fan switching
MQTT Broker     Communication between ESP32 and dashboard
Web Dashboard   Monitoring, phase control, logging, and export
Wokwi           Embedded-system simulation environment



🌱 Cultivation Modes

Parameter configuration is defined in both the ESP32 firmware and
dashboard.

Mode              Temperature   Humidity    Mist     Exhaust    LED Grow        Min.       Max.
Fan                  Duration   Duration

M1 Inkubasi     20--23 °C    60--70%     ✅         ❌         ❌        20 days    25 days

M2a Induksi     10--12 °C    80--85%     ✅         ❌         ❌        10 days    14 days

M2b               12--15 °C    75--80%     ✅         ✅         ❌         3 days     5 days
Pemanjangan

Temperature setpoint used by the fuzzy controller is calculated from the
midpoint of the active phase range.



🧠 Control Strategy

1. Fuzzy Peltier Controller

The first fuzzy controller determines the target Peltier power.

Inputs

errorSuhu = temperature - setpoint
deltaSmooth = smoothed temperature change

Temperature error membership

NB  NS  Z  PS  PB

Temperature delta membership

N  Z  P

Output

Peltier power = 0–100%

The firmware contains 15 fuzzy rules generated from a 5 × 3 rule
base.

2. Cascade Fuzzy Fan Heatsink

The second fuzzy controller is a cascade controller.

Temperature Error
        │
        ▼
 Fuzzy Peltier
        │
        ▼
Peltier Target
        │
        ├──────────────┐
        │              │
        ▼              ▼
   Fan Heatsink    Delta Temperature
       Fuzzy              │
        │                 │
        └───────┬─────────┘
                ▼
        Fan Heatsink Output

Inputs:

Peltier target power: 0--100%

Smoothed temperature delta

Output:

Fan heatsink speed: 0--100%

The firmware contains 9 fuzzy rules for this controller.

3. Rate Limiter

To prevent abrupt output changes, the firmware limits Peltier and fan
heatsink changes to:

±15% / control cycle

The control cycle is:

2 seconds

Therefore, a target change from 0% to 100% is approached progressively
rather than applied instantaneously.

4. Humidity Hysteresis

Mist maker control uses the phase-specific humidity range.

RH < RHmin  → Mist ON

RH > RHmax  → Mist OFF

The mist maker also has a minimum ON duration:

20 seconds

This prevents very frequent ON/OFF switching.

5. Sensor Failsafe

The firmware monitors failed DHT22 readings.

5 consecutive failed readings
             │
             ▼
      FAILSAFE ACTIVE
             │
       ┌─────┴─────┐
       ▼           ▼
  Peltier = 0%  Fan HS = 0%

Output reduction still passes through the rate limiter.



⏱️ Phase Management

A phase can be changed through three mechanisms:

                     ┌─────────────────┐
                     │ Phase Change    │
                     └────────┬────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
        Physical Button   Dashboard MQTT   Auto Transition

Minimum duration reached

The ESP32 publishes:

Topic:
enoki/notif

Payload:
MIN_REACHED

This indicates that the current phase has reached its minimum duration
and may be changed manually.

Maximum duration reached

When the maximum phase duration is reached, the ESP32 automatically
advances to the next phase.



📡 MQTT

Broker

The current firmware uses the public Mosquitto test broker:

Broker : test.mosquitto.org
Port   : 1883

The dashboard connects through WebSocket:

wss://test.mosquitto.org:8081

Security note: A public test broker should not be used for
production deployment.

ESP32 → Dashboard Topics

Topic                        Payload   Description

enoki/temperature          float     Current temperature
enoki/humidity             float     Current humidity
enoki/mode                 0–3     Current phase index
enoki/peltier              0–100   Peltier output
enoki/fanheatsink          0–100   Fan heatsink output
enoki/mist                 0/1     Mist maker state
enoki/led                  0/1     Grow LED state
enoki/fan                  0/1     Exhaust fan state
enoki/phase_elapsed_days   float     Current phase age
enoki/phase_min_days       float     Minimum phase duration
enoki/phase_max_days       float     Maximum phase duration
enoki/notif                string    Phase notification

Dashboard → ESP32

Topic              Payload   Description

enoki/cmd/mode   next    Advance to next phase
enoki/cmd/mode   0–3     Select phase directly



🖥️ Dashboard

The dashboard is titled Enoki Chamber IoT Dashboard and is
implemented using HTML, CSS, and JavaScript.

Dashboard sections

Current temperature.

Current humidity.

Active mode.

Virtual clock.

Total logged data.

Peltier status.

Fan heatsink status.

Mist maker status.

LED grow status.

CO₂ exhaust fan status.

Temperature chart.

Humidity chart.

Combined temperature/humidity history.

Phase progress and notification.

Data filtering.

Time search.

Pagination.

CSV export.

Excel export.

The dashboard also supports local sensor simulation.

ESP32 unavailable
       │
       ▼
Local browser simulation
       │
       ▼
Dashboard remains usable

When real ESP32 data arrives through MQTT, local sensor simulation is
stopped and the device data becomes the primary source.



📊 Data Logging

Each log record contains:

Timestamp
Virtual hour
Mode
Temperature
Humidity
Peltier %
Fan heatsink %
Mist Maker
LED Grow
Exhaust Fan

Export formats

.csv
.xlsx

The XLSX export creates:

M1 Inkubasi
M2a Induksi
M2b Pemanjangan
M2c Fruiting
Semua Fase

Only phases that contain recorded data are added as individual sheets.

🔌 Pin Mapping

ESP32

Device              GPIO / Interface

DHT22               GPIO 15
OLED SDA            GPIO 21
OLED SCL            GPIO 22
Mist Maker Relay    GPIO 26
LED Grow Relay      GPIO 27
Exhaust Fan Relay   GPIO 14
Mode Button         GPIO 33

Peltier Bargraph

DS   = GPIO 5
STCP = GPIO 18
SHCP = GPIO 19

Fan Heatsink Bargraph

DS   = GPIO 13
STCP = GPIO 25
SHCP = GPIO 32

Peltier dan fan heatsink menggunakan dua 74HC595 secara daisy-chain
untuk merepresentasikan 0--100% output dalam 10 segmen LED.



🛠️ Software Stack

Firmware

C++
Arduino Framework
ESP32
PlatformIO

Libraries

Wire
DHT
Adafruit GFX
Adafruit SSD1306
PubSubClient
WiFi
Fuzzy

Dashboard

HTML5
CSS3
JavaScript
Chart.js
MQTT.js
SheetJS / XLSX
Tabler Icons

External dashboard dependencies are currently loaded through CDN.




🚀 Getting Started

Prerequisites

Install:

VS Code

PlatformIO

Wokwi Simulator

Live Server

Browser modern yang mendukung JavaScript dan WebSocket.

1. Build Firmware

Pastikan environment ESP32 tersedia pada PlatformIO.

Build project hingga menghasilkan:

.pio/build/esp32/firmware.elf
.pio/build/esp32/firmware.bin

wokwi.toml sudah mengarah ke kedua hasil build tersebut.

2. Run in Wokwi

File wokwi.toml:

[wokwi]
version = 1
elf = ".pio/build/esp32/firmware.elf"
firmware = ".pio/build/esp32/firmware.bin"

Jalankan simulasi menggunakan rangkaian Wokwi dengan pemetaan pin sesuai
firmware.

3. Start Dashboard

Dashboard merupakan web frontend statis.

Struktur minimal:

index.html
style.css
script.js

Dashboard memuat Chart.js, SheetJS, MQTT.js, dan Tabler Icons melalui
CDN.

Buka index.html menggunakan browser atau jalankan melalui local web
server.


Kemudian buka:

http://localhost:8000



🧪 Simulation Notes

Virtual Time

ESP32 menggunakan konsep waktu virtual:

1 menit nyata = 1 jam virtual

Jam virtual dimulai dari:

06:00

Hal ini digunakan untuk mempercepat simulasi perubahan fase dan jadwal
LED grow.

Important implementation detail

Pada source yang tersedia terdapat dua konstanta waktu fase:

// ESP32
VIRTUAL_SEC_PER_DAY = 60.0

dan pada dashboard:

// Local dashboard simulation
VIRTUAL_SEC_PER_DAY = 1440

Keduanya memiliki skala yang berbeda.

Untuk eksperimen yang membutuhkan sinkronisasi umur fase antara firmware
dan dashboard, nilai tersebut perlu diseragamkan.



⚠️ Limitations

Simulation vs Physical System

Proyek saat ini belum merupakan implementasi penuh pada chamber fisik.

Khususnya:

Peltier belum dikendalikan melalui driver daya fisik.

Fan heatsink belum menggunakan PWM hardware untuk aktuator nyata.

LED bargraph hanya digunakan sebagai representasi level output pada
Wokwi.

Nilai sensor pada simulasi tidak dapat dijadikan bukti akurasi
sensor fisik.

Pengujian MAE/RMSE terhadap sensor memerlukan perangkat pembanding
pada lingkungan nyata.

Data Persistence

Logging dashboard saat ini menggunakan array runtime JavaScript:

logData

Artinya data histori akan hilang ketika halaman di-refresh atau ditutup.

Belum terdapat:

Database
Backend API
Persistent cloud storage

pada source yang tersedia.



🔐 Production Considerations

Untuk deployment nyata, beberapa bagian perlu ditingkatkan:

Public MQTT Broker
        ↓
Authenticated MQTT Broker
        ↓
TLS / Secure WebSocket
        ↓
Persistent Database
        ↓
Backend API

Hal ini penting agar:

komunikasi tidak bergantung pada broker publik;

command MQTT dapat diautentikasi;

histori data tersimpan permanen;

sistem lebih sesuai untuk penggunaan produksi.



🗺️ Future Development

Pengembangan lanjutan yang relevan dengan arsitektur saat ini:

Sinkronisasi virtual-time firmware dan dashboard.

Implementasi aktuator fisik Peltier + MOSFET/driver.

PWM fan heatsink pada hardware nyata.

Database untuk histori jangka panjang.

User authentication pada dashboard.

MQTT TLS dan credential management.

Alarm ketika sensor gagal atau kondisi lingkungan di luar batas.

Analisis performa kontrol secara kuantitatif.

Deployment dashboard ke server/cloud.

Integrasi data multi-chamber.



🎓 Research Context

Proyek ini dikembangkan sebagai prototipe penelitian monitoring dan
kendali chamber budidaya jamur enoki dengan pendekatan:

Phase-Aware IoT Architecture
            +
Hybrid Logic Control
            +
Cultivation-Aware Dashboard

Konsep tersebut diwujudkan melalui:

parameter lingkungan yang berubah mengikuti fase;

kontrol fuzzy untuk Peltier dan fan heatsink;

hysteresis untuk pengelolaan kelembapan;

pengelolaan fase manual dan otomatis;

MQTT untuk komunikasi perangkat-dashboard;

dashboard yang menampilkan informasi berdasarkan konteks budidaya.



👤 Author

Rafiq Syarif Lasmana
Universitas Pembangunan Panca Budi
NIM: 2424210112


📄 License

Belum ditentukan.

Tambahkan lisensi repository sesuai kebutuhan penelitian atau distribusi
project, misalnya:

MIT
Apache-2.0
GPL-3.0



⭐ Acknowledgement

Project ini memanfaatkan beberapa library dan platform open-source,
termasuk:

Arduino Framework

ESP32

PlatformIO

Wokwi

PubSubClient

Adafruit GFX

Adafruit SSD1306

Chart.js

MQTT.js

SheetJS

Tabler Icons