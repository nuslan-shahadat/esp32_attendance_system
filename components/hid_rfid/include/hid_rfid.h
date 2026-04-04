#pragma once
#include "driver/gpio.h"
#include <stddef.h>
#include <stdint.h>

/* Callback type: called when a complete card UID is scanned. */
typedef void (*hid_rfid_card_cb_t)(const char *uid);

typedef struct {
    gpio_num_t         power_pin;
    hid_rfid_card_cb_t on_card_uid;
} hid_rfid_config_t;

/* Initialise USB HID host and power-cycle the RFID reader. */
void hid_rfid_init(const hid_rfid_config_t *cfg);

/* Call from main loop — drains the UID queue and invokes the callback. */
void hid_rfid_poll(void);

/* ── Last-event store ───────────────────────────────────────────
   push_event() is called from main after a card scan result.
   get_last_event() is called from the HTTP handler.              */
typedef struct {
    char     uid[32];
    char     name[64];
    char     time[10];
    char     status[16]; /* "registered" | "already" | "unknown" */
    uint32_t seq;        /* 0 = no events yet; increments each scan */
} hid_rfid_event_t;

void hid_rfid_event_init(void);
void hid_rfid_push_event(const char *uid, const char *name,
                         const char *time,  const char *status);
void hid_rfid_get_last_event(hid_rfid_event_t *out);
