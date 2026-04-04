#include "wifi_mgr.h"
#include "db.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


static const char *TAG = "wifi_mgr";

/* ── Event group bits ───────────────────────────────────────── */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static EventGroupHandle_t s_wifi_events = NULL;

/* ── State ──────────────────────────────────────────────────── */
static wifi_mgr_config_t s_cfg         = {0};
static bool              s_is_ap_mode  = false;
static int64_t           s_disconnect_us = 0;    /* time of first disconnect */
#define FAILOVER_TIMEOUT_US  (30LL * 1000000LL)  /* 30 seconds */

/* ── Event handler ──────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *d =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Disconnected (reason %d)", d->reason);
                if (s_disconnect_us == 0)
                    s_disconnect_us = esp_timer_get_time();
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *ev =
                    (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Client connected to AP: %02x:%02x:%02x:%02x:%02x:%02x",
                    (unsigned)ev->mac[0], (unsigned)ev->mac[1], (unsigned)ev->mac[2],
                    (unsigned)ev->mac[3], (unsigned)ev->mac[4], (unsigned)ev->mac[5]);

                break;
            }

            default: break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_disconnect_us = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* ── Start AP mode ──────────────────────────────────────────── */
void wifi_mgr_start_ap(void)
{
    ESP_LOGI(TAG, "Starting AP: %s", s_cfg.ap_ssid);

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_cfg.ap_ssid, 31);
    strncpy((char *)ap_cfg.ap.password, s_cfg.ap_pass, 63);
    ap_cfg.ap.ssid_len      = (uint8_t)strlen(s_cfg.ap_ssid);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = strlen(s_cfg.ap_pass) >= 8
                                 ? WIFI_AUTH_WPA2_PSK
                                 : WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    s_is_ap_mode = true;

    esp_netif_ip_info_t ip;
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK)
        ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ip.ip));
}

/* ── Try STA connect ────────────────────────────────────────── */
static bool try_sta_connect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid, 31);
    strncpy((char *)sta_cfg.sta.password, pass, 63);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    /* Static IP */
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif && s_cfg.sta_ip) {
        esp_netif_dhcpc_stop(sta_netif);
        esp_netif_ip_info_t ip_info = {0};
        /* esp_ip4addr_aton is the ESP-IDF v5 wrapper — same as ip4addr_aton */
        ip_info.ip.addr      = esp_ip4addr_aton(s_cfg.sta_ip);
        ip_info.gw.addr      = esp_ip4addr_aton(s_cfg.sta_gateway);
        ip_info.netmask.addr = esp_ip4addr_aton(s_cfg.sta_netmask);
        esp_netif_set_ip_info(sta_netif, &ip_info);

        /*
         * BUG FIX: When DHCP is stopped and a static IP is set, the DNS
         * server is NOT populated automatically.  Without a DNS server,
         * pool.ntp.org (and any other hostname) cannot be resolved, which
         * causes SNTP to always time out.
         * Use the gateway as primary DNS (common for home routers) and
         * Google 8.8.8.8 as secondary fallback.
         */
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;

        /* Primary DNS = gateway (most home routers double as DNS resolvers) */
        dns.ip.u_addr.ip4.addr = ip_info.gw.addr;
        esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);

        /* Secondary DNS = Google public DNS */
        dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
        esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
    }

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();

    /* Wait for connect or failure (max 20 s) */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        s_is_ap_mode = false;
        return true;
    }
    return false;
}

/* ── Public init ────────────────────────────────────────────── */
void wifi_mgr_init(const wifi_mgr_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* Read credentials from DB config */
    char *ssid = db_config_get("wifi_ssid");
    char *pass = db_config_get("wifi_pass");

    bool connected = false;
    if (ssid && strlen(ssid) > 0) {
        connected = try_sta_connect(ssid, pass ? pass : "");
    }

    if (ssid) free(ssid);
    if (pass) free(pass);

    if (!connected) {
        ESP_LOGW(TAG, "WiFi connection failed — starting AP mode");
        wifi_mgr_start_ap();
    }
}

/* ── Poll (call from main loop) ─────────────────────────────── */
void wifi_mgr_poll(void)
{
    if (s_is_ap_mode) return;

    wifi_ap_record_t ap;
    bool connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    if (!connected) {
        if (s_disconnect_us == 0)
            s_disconnect_us = esp_timer_get_time();

        if (esp_timer_get_time() - s_disconnect_us > FAILOVER_TIMEOUT_US) {
            ESP_LOGW(TAG, "STA unreachable >30s — switching to AP mode");
            wifi_mgr_start_ap();
            s_disconnect_us = 0;
        } else {
            ESP_LOGI(TAG, "WiFi lost — reconnecting...");
            esp_wifi_connect();
        }
    } else {
        s_disconnect_us = 0;
    }
}

/* ── Status ─────────────────────────────────────────────────── */
bool wifi_mgr_is_connected(void)
{
    if (s_is_ap_mode) return false;
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

/*
 * BUG FIX: wifi_mgr_is_connected() always returned false in AP mode,
 * so main.c's startup loop — while(!wifi_mgr_is_connected()) — spun
 * forever when there were no saved WiFi credentials (first boot or after
 * factory reset).  The HTTP server was never started, giving users a blank
 * page even though the ESP32 was broadcasting its own AP.
 *
 * wifi_mgr_is_ready() returns true as soon as *any* usable network
 * interface is up — STA connected OR AP active.  Gate the HTTP server
 * start on this, not on is_connected().
 */
bool wifi_mgr_is_ready(void)
{
    if (s_is_ap_mode) return true;   /* AP is up — clients can connect */
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

char *wifi_mgr_status_json(void)
{
    char *out = malloc(256);
    if (!out) return NULL;

    /*
     * FIX: Replace manual byte-lane extraction of IP addresses with the
     * standard ESP-IDF IPSTR / IP2STR macros.  The manual shifts assumed
     * little-endian byte order which matched lwIP's in_addr_t layout, but
     * the macro form is clearer, consistent with the rest of the driver
     * (e.g. the AP_STACONNECTED handler already uses MAC2STR / MACSTR).
     */
    if (s_is_ap_mode) {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip = {0};
        if (ap_netif) esp_netif_get_ip_info(ap_netif, &ip);
        snprintf(out, 256,
                 "{\"mode\":\"ap\",\"ssid\":\"%s\",\"ip\":\"" IPSTR "\"}",
                 s_cfg.ap_ssid, IP2STR(&ip.ip));
    } else {
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);

        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {0};
        if (sta_netif) esp_netif_get_ip_info(sta_netif, &ip);

        snprintf(out, 256,
                 "{\"mode\":\"sta\",\"ssid\":\"%s\","
                 "\"ip\":\"" IPSTR "\",\"rssi\":%d}",
                 (char *)ap.ssid, IP2STR(&ip.ip), ap.rssi);
    }
    return out;
}

/* ── Scan ───────────────────────────────────────────────────── */
char *wifi_mgr_scan_json(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    /* Ensure STA mode for scan */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    esp_wifi_scan_start(&scan_cfg, true);   /* blocking */

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);
        return strdup("[]");
    }

    wifi_ap_record_t *records = malloc(count * sizeof(wifi_ap_record_t));
    if (!records) return strdup("[]");
    esp_wifi_scan_get_ap_records(&count, records);

    /* Build JSON */
    size_t cap = (size_t)count * 100 + 8;
    char *out  = malloc(cap);
    if (!out) { free(records); return strdup("[]"); }

    size_t pos = 0;
    out[pos++] = '[';

    for (int i = 0; i < count; i++) {
        int rssi     = records[i].rssi;
        int strength = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
        bool secured = (records[i].authmode != WIFI_AUTH_OPEN);
        bool primary = (records[i].primary != 0);  /* has a channel */
        (void)primary;

        /* Escape SSID */
        char ssid_esc[70] = {0};
        size_t si = 0;
        for (size_t j = 0; j < sizeof(records[i].ssid) && records[i].ssid[j]; j++) {
            char c = (char)records[i].ssid[j];
            if (c == '"' || c == '\\') { ssid_esc[si++] = '\\'; }
            if (si < sizeof(ssid_esc)-1) ssid_esc[si++] = c;
        }

        int n = snprintf(out + pos, cap - pos,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,"
                         "\"strength\":%d,\"secured\":%s}",
                         i == 0 ? "" : ",",
                         ssid_esc, rssi, strength,
                         secured ? "true" : "false");
        pos += (size_t)n;
    }

    out[pos++] = ']';
    out[pos]   = '\0';

    free(records);
    esp_wifi_clear_ap_list();

    if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);
    return out;
}

/* ── Save + restart ─────────────────────────────────────────── */
void wifi_mgr_save_and_restart(const char *ssid, const char *pass)
{
    db_config_set("wifi_ssid", ssid);
    if (pass && strlen(pass) > 0)
        db_config_set("wifi_pass", pass);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
