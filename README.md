# Autosteel Customs Fingerprint

Repositori ini menampung kode dan ketentuan dalam pengembangan dan implementasi firmware untuk board dengan chip `ESP WROOM 32-U` (ESP32 DevKitC V4). Dikembangkan dengan framework `ESP IDF`. Templat ini diperoleh dari repository [Espressif esp-idf-template](https://github.com/espressif/esp-idf-template) dan telah dimodifikasi pada beberapa bagian untuk menyesuaikan dengan kebutuhan dari firmware yang akan dikembangkan.

## Komponen

### Board

![ESP WROOM 32U](assets/esp32-devkitC-v4-pinout.png "esp_wroom_32_u.jpeg")

| Spesifikasi | Keterangan                                   |
| ----------- | -------------------------------------------- |
| Board       | DevKit V4 ESP WROOM 32-U (External antenna ) |
| Jumlah Pin  | 38                                           |
| WiFi        | Ya                                           |
| Bluetooth   | Ya                                           |
| Chipset     | Xtensa® 32-bit LX6 CPU Dual Core 240 MHz     |
| Flash       | 4MB                                          |

### Sensor

| Sensor                                  | Jumlah |
| --------------------------------------- | ------ |
| AS608/FPM10A Optical Fingerprint Module | 1      |

### Komponen Lainnya (Aktuator & Lainnya)

| Komponen           | Jumlah |
| ------------------ | ------ |
| LED Blue           | 1      |
| LED Yellow         | 1      |
| LED Green          | 1      |
| LCD 16x2 + I2C     | 1      |
| Active Buzzer 3.3V | 1      |
| RTC DS3231         | 1      |

## Rancangan Perangkat

### Skematik

![Diagram Skematik](assets/autosteel%20presensi_schem.png "Diagram Skematik")

### Konfigurasi Pin

| Komponen                                | Pin Komponen | Pin Board |
| --------------------------------------- | ------------ | --------- |
| RTC DS3231                              | VCC          | 3.3V      |
|                                         | GND          | GND       |
|                                         | SCL          | GPIO 22   |
|                                         | SDA          | GPIO 21   |
|                                         | SQW          | GPIO 4    |
| LCD 16x2 + I2C                          | VCC          | 5V        |
|                                         | GND          | GND       |
|                                         | SCL          | GPIO 22   |
|                                         | SDA          | GPIO 21   |
| LED Blue                                | VCC          | GPIO 33   |
|                                         | GND          | GND       |
| LED Green                               | VCC          | GPIO 25   |
|                                         | GND          | GND       |
| LED Yellow                              | VCC          | GPIO 26   |
|                                         | GND          | GND       |
| Active Buzzer 3.3V                      | VCC          | GPIO 27   |
|                                         | GND          | GND       |
| AS608/FPM10A Optical Fingerprint Module | VCC          | 5V        |
|                                         | GND          | GND       |
|                                         | TX           | GPIO 16   |
|                                         | RX           | GPIO 17   |
|                                         | WAK          | GPIO 18   |

## Fungsi Perangkat

1. Enrollment
2. Template store
3. Validation
4. Access point (Managed by Event Group)
5. Web server (Managed by Event Group, related to point 4)
6. Attendance sync

> Repositori akan di perbarui secara berkala.
