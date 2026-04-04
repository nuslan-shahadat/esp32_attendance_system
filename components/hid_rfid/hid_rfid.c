#include "hid_rfid.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"   /* esp_timer_get_time() — needed for RFID debounce */
#include <string.h>
#include <stdlib.h>

static const char *TAG = "hid_rfid";

/* ── Internal types ─────────────────────────────────────────── */
#define UID_MAX_LEN      32
#define MAX_POWER_CYCLES 1000000 /* FIX: was infinite — cap retries */

typedef struct { char uid[UID_MAX_LEN]; } uid_msg_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t  event;
    void                    *arg;
} hid_event_t;

/* ── State ──────────────────────────────────────────────────── */
static QueueHandle_t        s_uid_queue   = NULL;
static QueueHandle_t        s_hid_queue   = NULL;
static volatile bool        s_connected   = false;
static volatile bool        s_shutdown    = false;
static hid_rfid_card_cb_t   s_callback    = NULL;

/* Card accumulator (written from HID task) */
static char    s_card_buf[UID_MAX_LEN];
static uint8_t s_card_len = 0;

/* ── Keycode → ASCII map ─────────────────────────────────────── */
static const uint8_t keycode2ascii[57][2] = {
    {0,0},{0,0},{0,0},{0,0},
    {'a','A'},{'b','B'},{'c','C'},{'d','D'},{'e','E'},{'f','F'},
    {'g','G'},{'h','H'},{'i','I'},{'j','J'},{'k','K'},{'l','L'},
    {'m','M'},{'n','N'},{'o','O'},{'p','P'},{'q','Q'},{'r','R'},
    {'s','S'},{'t','T'},{'u','U'},{'v','V'},{'w','W'},{'x','X'},
    {'y','Y'},{'z','Z'},
    {'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},
    {'6','^'},{'7','&'},{'8','*'},{'9','('},{'0',')'},
    {'\r','\r'},{0,0},{'\b',0},{0,0},{' ',' '},
    {'-','_'},{'=','+'},{'[','{'},
    {']','}'},{'\\','|'},{'\\','|'},{';',':'},
    {'\'','"'},{'`','~'},{',','<'},{'.','>'},{'/','?'}
};

static bool key_get_char(uint8_t modifier, uint8_t kc, unsigned char *out)
{
    if (kc < HID_KEY_A || kc > HID_KEY_SLASH) return false;
    bool shift = (modifier & HID_LEFT_SHIFT) || (modifier & HID_RIGHT_SHIFT);
    *out = keycode2ascii[kc][shift ? 1 : 0];
    return true;
}

static bool key_in_array(const uint8_t *arr, uint8_t key, int len)
{
    for (int i = 0; i < len; i++) if (arr[i] == key) return true;
    return false;
}

/* ── Called on each decoded character from scanner ───────────── */
static void on_char(char c)
{
    if (c == '\r' || c == '\n') {
        if (s_card_len > 0) {
            s_card_buf[s_card_len] = '\0';
            uid_msg_t msg;
            strncpy(msg.uid, s_card_buf, UID_MAX_LEN - 1);
            msg.uid[UID_MAX_LEN - 1] = '\0';
            xQueueSend(s_uid_queue, &msg, 0);
            s_card_len = 0;
        }
        return;
    }
    if (s_card_len < UID_MAX_LEN - 1) s_card_buf[s_card_len++] = c;
}

/* ── HID keyboard report callback ───────────────────────────── */
static void keyboard_report_cb(const uint8_t *data, int len)
{
    if (len < (int)sizeof(hid_keyboard_input_report_boot_t)) return;
    const hid_keyboard_input_report_boot_t *kb =
        (const hid_keyboard_input_report_boot_t *)data;

    static uint8_t prev[HID_KEYBOARD_KEY_MAX] = {0};

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        /* Key pressed (new key not in previous report) */
        if (kb->key[i] > HID_KEY_ERROR_UNDEFINED &&
            !key_in_array(prev, kb->key[i], HID_KEYBOARD_KEY_MAX)) {
            unsigned char ch = 0;
            if (key_get_char(kb->modifier.val, kb->key[i], &ch) && ch)
                on_char((char)ch);
        }
    }
    memcpy(prev, kb->key, HID_KEYBOARD_KEY_MAX);
}

/* ── HID interface event handler ─────────────────────────────── */
static void hid_interface_cb(hid_host_device_handle_t handle,
                              hid_host_interface_event_t event, void *arg)
{
    uint8_t data[64] = {0};
    size_t  data_len = 0;
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) return;

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            if (hid_host_device_get_raw_input_report_data(
                    handle, data, sizeof(data), &data_len) == ESP_OK &&
                params.proto == HID_PROTOCOL_KEYBOARD)
                keyboard_report_cb(data, (int)data_len);
            break;

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "HID device disconnected");
            hid_host_device_close(handle);
            s_connected = false;
            break;

        default: break;
    }
}

/* ── HID driver event handler ────────────────────────────────── */
static void hid_device_event(hid_host_device_handle_t handle,
                              hid_host_driver_event_t event, void *arg)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) return;

    ESP_LOGI(TAG, "HID device connected (proto=%d)", params.proto);
    s_connected = true;

    hid_host_device_config_t dcfg = {
        .callback     = hid_interface_cb,
        .callback_arg = NULL,
    };
    if (hid_host_device_open(handle, &dcfg) != ESP_OK) return;

    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
        if (params.proto == HID_PROTOCOL_KEYBOARD)
            hid_class_request_set_idle(handle, 0, 0);
    }

    if (hid_host_device_start(handle) != ESP_OK)
        hid_host_device_close(handle);
}

/* ── HID event queue callback (from USB stack) ───────────────── */
static void hid_event_cb(hid_host_device_handle_t handle,
                          hid_host_driver_event_t event, void *arg)
{
    hid_event_t ev = { handle, event, arg };
    xQueueSend(s_hid_queue, &ev, 0);
}

/* ── HID host task ───────────────────────────────────────────── */
static void hid_host_task(void *arg)
{
    hid_event_t ev;
    while (!s_shutdown) {
        if (xQueueReceive(s_hid_queue, &ev, pdMS_TO_TICKS(50)) == pdTRUE)
            hid_device_event(ev.handle, ev.event, ev.arg);
    }
    vQueueDelete(s_hid_queue);
    s_hid_queue = NULL;
    vTaskDelete(NULL);
}

/* ── USB lib task ────────────────────────────────────────────── */
static void usb_lib_task(void *arg)
{
    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    if (usb_host_install(&host_cfg) != ESP_OK) { vTaskDelete(NULL); return; }
    xTaskNotifyGive((TaskHandle_t)arg);

    while (!s_shutdown) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
    usb_host_uninstall();
    vTaskDelete(NULL);
}

/* ── Public init ─────────────────────────────────────────────── */
void hid_rfid_init(const hid_rfid_config_t *cfg)
{
    s_callback  = cfg->on_card_uid;
    s_uid_queue = xQueueCreate(20, sizeof(uid_msg_t));
    s_hid_queue = xQueueCreate(10, sizeof(hid_event_t));

    /* Configure power pin */
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << cfg->power_pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);

    /* FIX: Ensure reader starts powered OFF before first cycle */
    gpio_set_level(cfg->power_pin, 1);

    /* Start USB lib task — notify us when it is installed */
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                             xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    /* FIX: use pdFALSE instead of bare false for FreeRTOS API */
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));

    /* Start HID host */
    hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority          = 5,
        .stack_size             = 4096,
        .core_id                = 0,
        .callback               = hid_event_cb,
        .callback_arg           = NULL,
    };
    if (hid_host_install(&hid_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed");
        return;
    }

    xTaskCreate(hid_host_task, "hid_task", 4096, NULL, 2, NULL);

    /*
     * FIX: Power cycle sequence was inverted (HIGH→LOW = powered off during
     * wait).  Correct sequence: assert LOW (off) → short delay → assert HIGH
     * (on) → wait for USB enumeration.
     * FIX: Capped at MAX_POWER_CYCLES to prevent an infinite block if the
     * reader is absent.
     */
    for (int attempt = 1; attempt <= MAX_POWER_CYCLES; attempt++) {
        ESP_LOGI(TAG, "RFID power cycle attempt %d/%d…",
                 attempt, MAX_POWER_CYCLES);
        s_connected = false;

        gpio_set_level(cfg->power_pin, 1);          /* power OFF  */
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(cfg->power_pin, 0);          /* power ON   */

        /* Wait up to 4 s for USB enumeration */
        TickType_t t0 = xTaskGetTickCount();
        while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(4000)) {
            if (s_connected) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (s_connected) {
            ESP_LOGI(TAG, "RFID ready after %d attempt(s)", attempt);
            gpio_set_level(cfg->power_pin, 0);  /* ensure power is ON after successful enumeration */
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "RFID reader did not enumerate after %d attempts — "
                  "continuing without RFID", MAX_POWER_CYCLES);
}

/* ── Poll (drain UID queue) ──────────────────────────────────── */
void hid_rfid_poll(void)
{
    /*
     * BUG FIX: The USB HID scanner emits the UID repeatedly as long as the
     * card is held near the reader (it behaves like a held-down key on a
     * keyboard).  Without debouncing, every repeated report fires the
     * callback and db_save_attendance() is hammered on each poll cycle.
     * Combined with the old INSERT OR REPLACE in the DB layer, the last
     * report's timestamp would overwrite the first, so the recorded time
     * was always the moment the card was pulled away.
     *
     * Fix: track the last accepted UID and its timestamp.  If the same UID
     * arrives again within RFID_DEBOUNCE_US, drop it silently.
     */
#define RFID_DEBOUNCE_US  (5LL * 1000000LL)   /* 5 s cooldown per card */

    static char    s_last_uid[UID_MAX_LEN] = {0};
    static int64_t s_last_uid_us           = 0;

    uid_msg_t msg;
    while (xQueueReceive(s_uid_queue, &msg, 0) == pdTRUE) {
        int64_t now = esp_timer_get_time();

        if (strcmp(msg.uid, s_last_uid) == 0 &&
            (now - s_last_uid_us) < RFID_DEBOUNCE_US) {
            ESP_LOGD(TAG, "Debounce: ignoring duplicate UID %s", msg.uid);
            continue;   /* drop — same card still in range */
        }

        /* New card (or same card after cooldown) — accept it */
        strncpy(s_last_uid, msg.uid, UID_MAX_LEN - 1);
        s_last_uid[UID_MAX_LEN - 1] = '\0';
        s_last_uid_us = now;

        ESP_LOGI(TAG, "Card: %s", msg.uid);
        if (s_callback) s_callback(msg.uid);
    }
}

/* ═══════════════════════════════════════════════════════════════
   Last-event store — thread-safe, used by /api/rfid-events
   ═══════════════════════════════════════════════════════════════ */
#include "freertos/semphr.h"

static hid_rfid_event_t  s_last_event  = {0};
static SemaphoreHandle_t s_event_mutex = NULL;

void hid_rfid_event_init(void)
{
    s_event_mutex = xSemaphoreCreateMutex();
}

void hid_rfid_push_event(const char *uid,  const char *name,
                         const char *time, const char *status)
{
    if (!s_event_mutex) return;
    xSemaphoreTake(s_event_mutex, portMAX_DELAY);
    strncpy(s_last_event.uid,    uid    ? uid    : "", 31);
    strncpy(s_last_event.name,   name   ? name   : "", 63);
    strncpy(s_last_event.time,   time   ? time   : "",  9);
    strncpy(s_last_event.status, status ? status : "", 15);
    s_last_event.seq++;
    xSemaphoreGive(s_event_mutex);
}

void hid_rfid_get_last_event(hid_rfid_event_t *out)
{
    if (!out) return;
    if (s_event_mutex) xSemaphoreTake(s_event_mutex, portMAX_DELAY);
    *out = s_last_event;
    if (s_event_mutex) xSemaphoreGive(s_event_mutex);
}
