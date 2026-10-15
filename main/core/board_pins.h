/**
 * BOARD_PINS_H
 *
 * Pin mapping for ESP-WROOM-32-U (38 pins)/Devkit V4
 *
 * STRAPPING PIN JANGAN DIPAKE YA DEK KECUALI KEPEPET!!!
 * mana aja bang strapping pinnya?
 * ini dek: GPIO 0, 2, 5, 12, 15
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/**
 * for I2C connections
 * devices: LCD 16x2 (PCF8574), RTC DS3231
 */
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

/**
 * SQW pin for RTC DS3231
 */
#define PIN_RTC_SQW 4

/**
 * led & buzzer 3.3V
 */
#define PIN_LED_BLUE 33
#define PIN_LED_GREEN 25
#define PIN_LED_YELLOW 26
#define PIN_BUZZER 27

/* Tombol BOOT bawaan DevKit (GPIO 0, pull-up eksternal). Satu-satunya pemakaian strapping pin:
 * hanya DIBACA setelah boot (tahan 5 detik di mode operasional = reset WiFi ke mode konfigurasi).
 * Jangan menahan tombol saat reset/power-on karena ROM akan masuk mode download. */

/**
 * BOOT button (GPIO 0, external pull up)
 * only used after boot is done, using hold mechanism (maybe 5 or 15 secs) to insert congig mode
 *
 * lohh?? kok pake strapping bang?
 * gapapa, kan kepepet. besok saya cariin artefak prasejarah peninggalan zess dan engs
 */
#define PIN_BTN_CONFIG 0

/**
 * fingerprint AS608, this device using UART (RX, TX)
 *
 * RX device => TX ESP
 * TX device => RX ESP
 *
 * (baru rencana)
 */
#define PIN_FPM_UART_RX 16
#define PIN_FPM_UART_TX 17
#define PIN_FPM_WAKE 18

#endif
