/*  Attendance System v5 — Pure ESP-IDF
    Stack: esp_http_server · SPIFFS · SD+SQLite · USB HID · esp_wifi */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

#include "auth.h"
#include "db.h"
#include "wifi_mgr.h"
#include "hid_rfid.h"
#include "http_server.h"

static const char *TAG = "main";

/* ── Pin definitions ─────────────────────────────────────────── */
#define SD_CS_PIN       GPIO_NUM_10
#define SD_MOSI_PIN     GPIO_NUM_11
#define SD_MISO_PIN     GPIO_NUM_13
#define SD_SCK_PIN      GPIO_NUM_12
#define RFID_POWER_PIN  GPIO_NUM_14

/* ── SPIFFS mount ────────────────────────────────────────────── */
static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "spiffs",
        .max_files              = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL)
            ESP_LOGE(TAG, "Failed to mount SPIFFS");
        else if (ret == ESP_ERR_NOT_FOUND)
            ESP_LOGE(TAG, "No SPIFFS partition — run 'pio run -t uploadfs'");
        else
            ESP_LOGE(TAG, "SPIFFS init error: %s", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %zu KB used / %zu KB total", used / 1024, total / 1024);
    return ESP_OK;
}

/* ── NVS init ────────────────────────────────────────────────── */
static void nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS truncated — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/* ── RFID card callback ──────────────────────────────────────── */
static void on_card_scanned(const char *uid)
{
    ESP_LOGI(TAG, "Card scanned: %s", uid);
    char name[64]    = {0};
    char time_buf[10] = {0};
    int result = db_save_attendance(uid, name, sizeof(name),
                                    time_buf, sizeof(time_buf));
    const char *status = (result == 1) ? "registered"
                       : (result == 0) ? "already"
                       : "unknown";
    hid_rfid_push_event(uid, name, time_buf, status);
}

/* ── app_main ────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "=== Attendance System v5 (ESP-IDF) ===");

    /* 0. RFID event store mutex */
    hid_rfid_event_init();

    /* 1. NVS */
    nvs_init();

    /* 2. SPIFFS — HTML/CSS/JS frontend */
    ESP_ERROR_CHECK(spiffs_init());

    /* 3. SD card + SQLite */
    db_spi_config_t spi_cfg = {
        .mosi = SD_MOSI_PIN,
        .miso = SD_MISO_PIN,
        .sck  = SD_SCK_PIN,
        .cs   = SD_CS_PIN,
    };
    ESP_ERROR_CHECK(db_init(&spi_cfg));
    db_ensure_default_classes();
    db_ensure_default_schema();

    /* 4. WiFi
     * FIX-35: Load network credentials from the config DB instead of using
     * compile-time constants.  The hardcoded values are kept as fallback
     * defaults so the device still boots on first flash without a DB record.
     * Exposed fields can be edited through the WiFi config web page and
     * persisted via db_config_set() so re-flashing is not needed. */
    static char ap_pass[64]  = "12345678";
    static char sta_ip[16]   = "";
    static char sta_gw[16]   = "192.168.68.1";
    static char sta_nm[16]   = "255.255.255.0";

    #define LOAD_CFG(key, buf) do { \
        char *_v = db_config_get(key); \
        if (_v && _v[0]) { strncpy(buf, _v, sizeof(buf) - 1); buf[sizeof(buf)-1] = '\0'; } \
        free(_v); \
    } while (0)

    LOAD_CFG("ap_pass",     ap_pass);
    LOAD_CFG("sta_ip",      sta_ip);
    LOAD_CFG("sta_gateway", sta_gw);
    LOAD_CFG("sta_netmask", sta_nm);

    wifi_mgr_config_t wifi_cfg = {
        .ap_ssid     = "Attendance_AP",
        .ap_pass     = ap_pass,
        .sta_ip      = sta_ip[0] ? sta_ip : NULL,   /* NULL = use DHCP */
        .sta_gateway = sta_gw,
        .sta_netmask = sta_nm,
    };
    wifi_mgr_init(&wifi_cfg);

    /*
     * BUG FIX: The old loop used wifi_mgr_is_connected() which always
     * returns false in AP mode.  On first boot (no saved WiFi credentials)
     * the device falls back to AP mode and the loop never exits — so the
     * HTTP server was never started and the webpage was blank.
     *
     * Use wifi_mgr_is_ready() instead: it returns true as soon as either
     * STA is connected OR the AP is active.
     */
    while (!wifi_mgr_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Waiting for WiFi...");
    }

    /* 5. SNTP — only meaningful when we have internet (STA mode).
     * FIX-36: Persist the NTP sync result to the config DB so the UI can
     * show a warning banner when the clock is unsynced (attendance timestamps
     * would be stuck at year 2000 without a successful sync). */
    if (wifi_mgr_is_connected()) {
        db_sntp_sync();
        /* db_sntp_sync() now writes "ntp_synced"="1" or "0" to the config DB */
    } else {
        ESP_LOGW(TAG, "AP mode — NTP skipped (no internet). "
                      "Configure WiFi via the web UI to enable time sync.");
        /* FIX-36: Mark as unsynced so the UI banner fires immediately */
        db_config_set("ntp_synced", "0");
    }

    /* 6. HTTP server */
    httpd_handle_t server = http_server_start();
    if (!server) {
        ESP_LOGE(TAG, "HTTP server failed — halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* 7. USB HID / RFID */
    hid_rfid_config_t rfid_cfg = {
        .power_pin   = RFID_POWER_PIN,
        .on_card_uid = on_card_scanned,
    };
    hid_rfid_init(&rfid_cfg);

    ESP_LOGI(TAG, "=== Setup complete. Free heap: %lu bytes ===",
             (unsigned long)esp_get_free_heap_size());

    /* 8. Main loop */
    char last_backup_date[12] = {0};
    while (1) {
        /* ── CRITICAL: drain the RFID UID queue every 100 ms ── */
        hid_rfid_poll();

        vTaskDelay(pdMS_TO_TICKS(100));

        /* WiFi + SD watchdog every ~10 s (100 × 100 ms) */
        static uint32_t tick = 0;
        if (++tick % 100 == 0) {
            wifi_mgr_poll();

            /* SD health watchdog — auto-remount if the card goes silent.
             * This is the primary fix for the "data gone, only format helps"
             * failure mode: we detect the silent failure within ~10 s and
             * recover transparently without a reboot or format.            */
            if (!db_sd_health_check()) {
                ESP_LOGW(TAG, "SD card unresponsive — attempting soft remount");
                if (db_sd_remount() == ESP_OK)
                    ESP_LOGI(TAG, "SD remount successful");
                else
                    ESP_LOGE(TAG, "SD remount failed — data unavailable until card is re-seated");
            }

            /* Nightly SD backup at 00:00–00:04 */
            char today[12];
            db_today_string(today, sizeof(today));
            struct tm ti = {0};
            db_get_localtime(&ti);
            if (ti.tm_hour == 0 && ti.tm_min <= 4 &&
                strcmp(today, last_backup_date) != 0) {
                strncpy(last_backup_date, today, sizeof(last_backup_date) - 1);
                db_sd_backup();
            }
        }
    }
}
