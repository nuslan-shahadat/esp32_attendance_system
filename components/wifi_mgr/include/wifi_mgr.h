#pragma once
#include <stdbool.h>

typedef struct {
    const char *ap_ssid;       /* Fallback AP SSID */
    const char *ap_pass;       /* Fallback AP password */
    const char *sta_ip;        /* Static IP for STA mode (e.g. "192.168.68.104") */
    const char *sta_gateway;
    const char *sta_netmask;
} wifi_mgr_config_t;

/* Initialise WiFi.
   Reads ssid/pass from DB config; falls back to AP mode if connection fails.
   Blocks until connected or AP is active. */
void wifi_mgr_init(const wifi_mgr_config_t *cfg);

/* Call from main loop — reconnects STA if dropped, activates AP after 30 s. */
void wifi_mgr_poll(void);

/* Switch to AP mode immediately. */
void wifi_mgr_start_ap(void);

/* Returns true if the network interface is ready:
   either connected in STA mode OR running as an AP.
   Use this — NOT wifi_mgr_is_connected() — to gate code that only
   needs *any* network (e.g. the HTTP server, which must start in both
   STA and AP modes so the user can reach the web UI to configure WiFi). */
bool wifi_mgr_is_ready(void);

/* Returns true if currently in STA mode and connected. */
bool wifi_mgr_is_connected(void);

/* Scan for nearby networks. Returns a malloc'd JSON array string.
   Caller must free. */
char *wifi_mgr_scan_json(void);

/* Save new WiFi credentials to DB and restart. */
void wifi_mgr_save_and_restart(const char *ssid, const char *pass);

/* Returns a malloc'd JSON status string. Caller frees. */
char *wifi_mgr_status_json(void);
