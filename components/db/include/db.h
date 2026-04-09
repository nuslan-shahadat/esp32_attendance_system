#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "driver/gpio.h"
#include "sqlite3.h"

/* ── Internal helpers (used across db_*.c translation units) ─ */
void     db_lock(void);
void     db_unlock(void);
sqlite3 *db_handle(void);
bool     db_exec_raw(const char *sql);
bool     db_exec(const char *sql);

/* ── SPI config (passed from main) ───────────────────────── */
typedef struct {
    gpio_num_t mosi;
    gpio_num_t miso;
    gpio_num_t sck;
    gpio_num_t cs;
} db_spi_config_t;

/* ── Initialisation ──────────────────────────────────────── */
/* Mount SD card, open SQLite database, create tables.
   Returns ESP_OK on success. */
int db_init(const db_spi_config_t *spi);

/* Seed default classes (6–10) and batch times if tables are empty. */
void db_ensure_default_classes(void);

/* Seed default schema columns if schema_cols is empty. */
void db_ensure_default_schema(void);

/* ── Time helpers ────────────────────────────────────────── */
/* Start SNTP and wait for sync (blocks up to 10 s). */
void db_sntp_sync(void);

/* Write today's date as "YYYY-MM-DD" into buf. */
void db_today_string(char *buf, size_t len);

/* Write current time as "HH:MM:SS" into buf. */
void db_now_time_string(char *buf, size_t len);

/* Fill a struct tm with local time. */
void db_get_localtime(struct tm *out);

/* ── Config key-value store ──────────────────────────────── */
/* Returns value string (caller owns it — free with db_free).
   Returns NULL if key not found. */
char *db_config_get(const char *key);

/* Set a config value. */
void  db_config_set(const char *key, const char *value);

/* Free a string returned by db_config_get / db_students_* etc. */
void  db_free(void *ptr);

/* ── Classes ─────────────────────────────────────────────── */
/* Returns comma-separated class numbers as a malloc'd string.
   e.g. "6,7,8,9,10". Caller frees. */
char *db_classes_list_json(void);

bool  db_class_exists(int class_num);
int   db_class_add(int class_num);       /* 0 = ok, -1 = error */
int   db_class_delete(int class_num);

/* ── Batches ─────────────────────────────────────────────── */
/* Returns JSON array string of batch times for a class.
   e.g. "[\"08:00\",\"10:00\"]". Caller frees. */
char *db_batches_list_json(int class_num);

int   db_batch_add(int class_num, const char *time_str);
int   db_batch_delete(int class_num, const char *time_str);

/* ── Students ────────────────────────────────────────────── */
/* Returns JSON array of students for the given class.
   Include archived if include_archived != 0. Caller frees.
   NOTE: avoided for large lists — use db_students_stream_json instead. */
char *db_students_list_json(int class_num, int include_archived, int min_att_pct);

/* Streaming variant — calls cb(chunk, len, ctx) once per student row plus the
   surrounding '[' and ']' brackets.  Each chunk is ≤ ~300 bytes so the lwIP
   DMA-capable DRAM requirement per httpd_resp_send_chunk() call stays tiny.
   Returns ESP_OK when the full list has been sent, or the first non-ESP_OK
   value returned by the callback (stream is aborted at that point). */
#include "esp_err.h"
typedef esp_err_t (*db_student_stream_cb_t)(const char *chunk, size_t len, void *ctx);
esp_err_t db_students_stream_json(int class_num, int include_archived, int min_att_pct,
                                   db_student_stream_cb_t cb, void *ctx);

/* Returns JSON object for a single student + attendance history. Caller frees. */
char *db_student_detail_json(const char *uid, int class_num);

int   db_student_register(int class_num, const char *uid,
                           const char *name, const char *roll,
                           const char *batchtime, const char *extra_json);

int   db_student_edit(int class_num, const char *uid_orig,
                      const char *uid_new, const char *name,
                      const char *roll, const char *batchtime);

int   db_student_archive(const char *uid, int class_num);
int   db_student_restore(const char *uid, int class_num);
int   db_student_delete(const char *uid, int class_num);

/* ── Streaming callback type (shared by attendance + report) ─ */
/* Same pattern as db_student_stream_cb_t: called once per small chunk.
   Each chunk is ≤ ~512 bytes — safe for httpd_resp_send_chunk() DMA. */
typedef esp_err_t (*db_stream_cb_t)(const char *chunk, size_t len, void *ctx);

/* ── Attendance ──────────────────────────────────────────── */
/* Called by RFID scan. Looks up the student, marks present.
   Uses current local time. */
/* Returns: 1 = newly marked present, 0 = already marked today, -1 = unknown card */
int   db_save_attendance(const char *uid, char *out_name, size_t name_len,
                         char *out_time, size_t time_len);

/* DEPRECATED — builds entire JSON in heap (up to 300 KB+); crashes with
   large classes.  Kept for reference only; use db_attendance_stream_json(). */
char *db_attendance_json(int class_num);

/* Streaming variant — emits one student row / one attendance record at a
   time via cb().  No allocation larger than ~512 bytes ever exists.
   Returns ESP_OK on success or the first non-ESP_OK value from cb(). */
esp_err_t db_attendance_stream_json(int class_num, db_stream_cb_t cb, void *ctx);

/* Manually mark a student present for a specific date/time. */
int   db_attendance_mark(int class_num, const char *uid,
                         const char *date, const char *time_str);

int   db_attendance_delete_by_id(int id, int class_num);

/* ── Report ──────────────────────────────────────────────── */
/* DEPRECATED — builds entire JSON in heap; use db_report_stream_json(). */
char *db_report_json(int class_num, const char *month_prefix, int min_att_pct);

/* Streaming variant — same pattern as db_attendance_stream_json(). */
esp_err_t db_report_stream_json(int class_num, const char *month_prefix,
                                int min_att_pct, db_stream_cb_t cb, void *ctx);

/* ── Schema ──────────────────────────────────────────────── */
char *db_schema_get_json(void);
int   db_schema_add(const char *col_type, const char *key,
                    const char *label, int ask);
int   db_schema_delete(const char *col_type, const char *key);
int   db_schema_edit_label(const char *col_type, const char *key,
                           const char *new_label);
int   db_schema_toggle_flag(const char *col_type, const char *key,
                            const char *flag, int value);

/* ── Admin ───────────────────────────────────────────────── */
int   db_reset_class(int class_num);
int   db_reset_all(void);
int   db_factory_reset(const char *classes_csv, const char *batches_csv);

/* Export CSV into a malloc'd string. type = "students" or "attendance". */
char *db_export_csv(int class_num, const char *type);
char *db_export_all_csv(void);
char *db_attendance_summary_json(int class_num, const char *date);

/* Import CSV from a buffer. Returns {added, skipped}. */
typedef struct { int added; int skipped; } db_import_result_t;
db_import_result_t db_import_csv(int class_num, const char *type,
                                 const char *csv_buf, size_t csv_len);

/* ── Streaming CSV import (low-memory, no large buffer needed) ──
 * Usage:
 *   db_import_begin();
 *   for each data row: db_import_process_line(cn, type, fields, nf, &result);
 *   db_import_end();
 * Must NOT be nested or called from multiple tasks simultaneously. */
void db_import_begin(void);
void db_import_process_line(int class_num, const char *type,
                            char **fields, int nfields,
                            db_import_result_t *result);
void db_import_end(void);

/* ── Backup ──────────────────────────────────────────────── */
void  db_sd_backup(void);

/* ── SD card health & recovery ──────────────────────────────────
 * db_sd_health_check() probes the mount-point with a simple stat().
 *   Returns true  if the SD card is readable.
 *   Returns false if the mount-point has gone silent (data disappeared).
 *
 * db_sd_remount() closes the live DB, unmounts the SD card, re-mounts
 *   it and reopens the database — all without a reboot or a format.
 *   Returns ESP_OK on success.
 *
 * db_sd_list_backups() returns a malloc'd JSON array of backup filenames
 *   found in /sdcard/sd/ (newest first).  Caller must free().
 *   Example: ["attendance_2026-04-09.bak","attendance_2026-04-08.bak"]
 *
 * db_sd_restore() replaces the live database with the named backup file
 *   (filename only, e.g. "attendance_2026-04-09.bak").
 *   Returns true on success.
 */
bool  db_sd_health_check(void);
int   db_sd_remount(void);
char *db_sd_list_backups(void);
bool  db_sd_restore(const char *filename);

/* ── System status ───────────────────────────────────────── */
/* Returns JSON status blob. Caller frees. */
char *db_status_json(int class_num);
