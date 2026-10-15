#include "ui_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "i2c_bus.h"
#include "lcd1602_i2c.h"
#include "state_manager.h"

static const char *TAG = "ui_task";

#define UI_TASK_STACK 4096
#define UI_TASK_PRIORITY 3
#define UI_QUEUE_LEN 4
#define UI_LOOP_MS 50

#define BLINK_HALF_PERIOD_MS 500
#define FEEDBACK_LED_MS 1000
#define FEEDBACK_LCD_MS 2000
#define SPLASH_MS 1500

#define BEEP_SUCCESS_COUNT 1
#define BEEP_SUCCESS_ON_MS 120
#define BEEP_SUCCESS_OFF_MS 0
#define BEEP_FAILURE_COUNT 3
#define BEEP_FAILURE_ON_MS 80
#define BEEP_FAILURE_OFF_MS 80

typedef struct
{
    ui_feedback_t type;
    char line1[LCD1602_COLS + 1];
    char line2[LCD1602_COLS + 1];
} ui_msg_t;

typedef struct
{
    lcd1602_handle_t lcd;

    bool blue_on;
    TickType_t blue_next;

    bool green_active;
    bool yellow_active;
    TickType_t green_off;
    TickType_t yellow_off;

    int beeps_left;
    bool buzzer_on;
    TickType_t buzzer_next;
    uint32_t beep_on_ms;
    uint32_t beep_off_ms;

    bool msg_active;
    TickType_t msg_until;
    char msg[LCD1602_ROWS][LCD1602_COLS + 1];
    char shown[LCD1602_ROWS][LCD1602_COLS + 1];
} ui_state_t;

static QueueHandle_t s_queue = NULL;

/* is tick overflow? */
static inline bool reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void copy_line(char dst[LCD1602_COLS + 1], const char *src)
{
    size_t n = src ? strnlen(src, LCD1602_COLS) : 0;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

/**
 * lcd 16x2 handler start from this line
 */
static lcd1602_handle_t lcd_setup(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (bus == NULL)
    {
        ESP_LOGW(TAG, "Bus I2C tidak tersedia; UI berjalan tanpa LCD");
        return NULL;
    }

    static const uint8_t candidates[] = {0x27, 0x3F}; // compatible with PCF8574 or PCF8574A
    for (size_t i = 0; i < sizeof(candidates); i++)
    {
        if (i2c_master_probe(bus, candidates[i], 50) != ESP_OK)
        {
            continue;
        }
        lcd1602_handle_t lcd = NULL;
        if (lcd1602_init(bus, candidates[i], &lcd) == ESP_OK)
        {
            return lcd;
        }
    }
    ESP_LOGW(TAG, "LCD tidak terdeteksi (0x27/0x3F); UI berjalan tanpa LCD");
    return NULL;
}

static void status_lines(EventBits_t bits, const char **l1, const char **l2)
{
    if (bits & MODE_CONFIG_BIT)
    {
        *l1 = "Mode Konfigurasi";
        *l2 = "Buka 192.168.4.1";
    }
    else if (bits & DEVICE_BUSY_BIT)
    {
        *l1 = "Memproses...";
        *l2 = "Mohon tunggu";
    }
    else if (!(bits & WIFI_CONNECTED_BIT))
    {
        *l1 = "Menunggu WiFi...";
        *l2 = "";
    }
    else if (!(bits & MQTT_CONNECTED_BIT))
    {
        *l1 = "Menunggu MQTT...";
        *l2 = "";
    }
    else
    {
        *l1 = "Siap Absen";
        *l2 = "";
    }
}

static void update_lcd(ui_state_t *st, EventBits_t bits, TickType_t now)
{
    if (st->lcd == NULL)
    {
        return;
    }

    const char *lines[LCD1602_ROWS];
    if (st->msg_active && !reached(now, st->msg_until))
    {
        lines[0] = st->msg[0];
        lines[1] = st->msg[1];
    }
    else
    {
        st->msg_active = false;
        status_lines(bits, &lines[0], &lines[1]);
    }

    for (uint8_t row = 0; row < LCD1602_ROWS; row++)
    {
        // is row value changed?
        if (strcmp(st->shown[row], lines[row]) == 0)
        {
            continue;
        }
        copy_line(st->shown[row], lines[row]);
        esp_err_t err = lcd1602_print_line(st->lcd, row, lines[row]);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Gagal menulis LCD baris %u: %s", row, esp_err_to_name(err));
        }
    }
}

static void show_message(ui_state_t *st, const char *l1, const char *l2,
                         uint32_t ms, TickType_t now)
{
    copy_line(st->msg[0], l1);
    copy_line(st->msg[1], l2);
    st->msg_active = true;
    st->msg_until = now + pdMS_TO_TICKS(ms);
}

/**
 * led & buzzer handler start from this line
 */
static void update_blue(ui_state_t *st, EventBits_t bits, TickType_t now)
{
    if (bits & MODE_CONFIG_BIT)
    {
        if (reached(now, st->blue_next))
        {
            st->blue_on = !st->blue_on;
            gpio_set_level(PIN_LED_BLUE, st->blue_on);
            st->blue_next = now + pdMS_TO_TICKS(BLINK_HALF_PERIOD_MS);
        }
    }
    else
    {
        // is connected to broker?
        bool on = (bits & MQTT_CONNECTED_BIT) != 0;
        if (on != st->blue_on)
        {
            st->blue_on = on;
            gpio_set_level(PIN_LED_BLUE, on);
        }
        st->blue_next = now;
    }
}

static void start_feedback(ui_state_t *st, const ui_msg_t *m, TickType_t now)
{
    const bool ok = (m->type == UI_FEEDBACK_SUCCESS);

    /**
     * led rule
     *
     * only one led are HIGH at a time
     */
    gpio_set_level(PIN_LED_GREEN, ok);
    gpio_set_level(PIN_LED_YELLOW, !ok);
    st->green_active = ok;
    st->yellow_active = !ok;
    TickType_t off = now + pdMS_TO_TICKS(FEEDBACK_LED_MS);
    st->green_off = off;
    st->yellow_off = off;

    /**
     * buzzer rule
     *
     * new beep for each event, change current buzzer beep state
     */
    st->beeps_left = ok ? BEEP_SUCCESS_COUNT : BEEP_FAILURE_COUNT;
    st->beep_on_ms = ok ? BEEP_SUCCESS_ON_MS : BEEP_FAILURE_ON_MS;
    st->beep_off_ms = ok ? BEEP_SUCCESS_OFF_MS : BEEP_FAILURE_OFF_MS;
    st->buzzer_on = true;
    gpio_set_level(PIN_BUZZER, 1);
    st->buzzer_next = now + pdMS_TO_TICKS(st->beep_on_ms);

    // lcd
    const char *l1 = m->line1;
    const char *l2 = m->line2;
    if (m->line1[0] == '\0' && m->line2[0] == '\0')
    {
        l1 = ok ? "Berhasil" : "Gagal / Ditolak";
        l2 = "";
    }
    show_message(st, l1, l2, FEEDBACK_LCD_MS, now);
}

/**
 * update feedback
 */
static void update_feedback_outputs(ui_state_t *st, TickType_t now)
{
    if (st->green_active && reached(now, st->green_off))
    {
        gpio_set_level(PIN_LED_GREEN, 0);
        st->green_active = false;
    }
    if (st->yellow_active && reached(now, st->yellow_off))
    {
        gpio_set_level(PIN_LED_YELLOW, 0);
        st->yellow_active = false;
    }

    if ((st->buzzer_on || st->beeps_left > 0) && reached(now, st->buzzer_next))
    {
        if (st->buzzer_on)
        {
            gpio_set_level(PIN_BUZZER, 0);
            st->buzzer_on = false;
            st->beeps_left--;
            if (st->beeps_left > 0)
            {
                st->buzzer_next = now + pdMS_TO_TICKS(st->beep_off_ms);
            }
        }
        else if (st->beeps_left > 0)
        {
            gpio_set_level(PIN_BUZZER, 1);
            st->buzzer_on = true;
            st->buzzer_next = now + pdMS_TO_TICKS(st->beep_on_ms);
        }
    }
}

/**
 * ui task start from this line
 */
static void ui_task(void *arg)
{
    (void)arg;

    ui_state_t st;
    memset(&st, 0, sizeof(st));
    st.lcd = lcd_setup();

    TickType_t now = xTaskGetTickCount();
    st.blue_next = now;
    show_message(&st, "Alat Presensi", "Memulai...", SPLASH_MS, now);

    for (;;)
    {
        ui_msg_t m;
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(UI_LOOP_MS)) == pdTRUE)
        {
            start_feedback(&st, &m, xTaskGetTickCount());
        }

        now = xTaskGetTickCount();
        EventBits_t bits = xEventGroupGetBits(sys_state_group);

        update_blue(&st, bits, now);
        update_feedback_outputs(&st, now);
        update_lcd(&st, bits, now);
    }
}

esp_err_t ui_task_start(void)
{
    if (s_queue != NULL)
    {
        return ESP_OK;
    }
    if (sys_state_group == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED_BLUE) | (1ULL << PIN_LED_GREEN) |
                        (1ULL << PIN_LED_YELLOW) | (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK)
    {
        return err;
    }
    gpio_set_level(PIN_LED_BLUE, 0);
    gpio_set_level(PIN_LED_GREEN, 0);
    gpio_set_level(PIN_LED_YELLOW, 0);
    gpio_set_level(PIN_BUZZER, 0);

    // declare bus for lcd I2C
    if (i2c_bus_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "Bus I2C gagal; lanjut tanpa LCD");
    }

    s_queue = xQueueCreate(UI_QUEUE_LEN, sizeof(ui_msg_t));
    if (s_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ui_task, "ui_task", UI_TASK_STACK, NULL, UI_TASK_PRIORITY, NULL) != pdPASS)
    {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ui_feedback(ui_feedback_t type, const char *line1, const char *line2)
{
    if (s_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ui_msg_t m = {.type = type};
    copy_line(m.line1, line1);
    copy_line(m.line2, line2);
    return (xQueueSend(s_queue, &m, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
