/*  db.c — core database init, time helpers, backup, status
    All SQLite access goes through db_exec() / db_prepare() which
    hold the mutex for the lifetime of the statement.              */

#include "db.h"
#include "sqlite3.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_spiffs.h"
/* FIX: esp_sntp.h is deprecated in ESP-IDF v5 — use esp_netif_sntp.h */
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>

static const char *TAG = "db";

/* ── Globals ─────────────────────────────────────────────────── */
static sqlite3          *s_db    = NULL;
static SemaphoreHandle_t s_mutex = NULL;
#define DB_PATH  "/sdcard/attendance.db"
#define SD_MOUNT "/sdcard"

/* Stored at db_init() time so db_sd_remount() can re-use them */
static db_spi_config_t   s_spi_cfg = {0};
static sdmmc_card_t     *s_card    = NULL;  /* kept for esp_vfs_fat_sdcard_unmount */

/* FIX-BUG1: Streaming guard — tracks how many HTTP chunked-stream responses
 * are currently in progress.  db_sd_remount() checks this before proceeding
 * so it never calls sqlite3_close_v2() while a prepared statement is live
 * inside a STREAM_CB cycle (where db_lock() has been temporarily released
 * for network I/O).  Using a simple volatile int is sufficient here because
 * it is only ever incremented/decremented under the FreeRTOS scheduler, and
 * the watchdog reads it from the same core/priority context.               */
static volatile int s_streaming_count = 0;

void db_streaming_begin(void) { s_streaming_count++; }
void db_streaming_end(void)   { if (s_streaming_count > 0) s_streaming_count--; }
bool db_is_streaming(void)    { return s_streaming_count > 0; }

/* ── Mutex helpers (used by all db_*.c files via extern) ──────── */
void db_lock(void)   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void db_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }
sqlite3 *db_handle(void) { return s_db; }

/* ── Raw exec (must be called under lock) ─────────────────────── */
bool db_exec_raw(const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(s_db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQL error [%.80s]: %s", sql, errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

/* ── Safe exec (takes lock itself) ───────────────────────────── */
bool db_exec(const char *sql)
{
    if (!s_db) return false;
    db_lock();
    bool ok = db_exec_raw(sql);
    db_unlock();
    return ok;
}

/* ── SD card mount ───────────────────────────────────────────── */
static esp_err_t sd_mount(const db_spi_config_t *spi)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = spi->mosi,
        .miso_io_num     = spi->miso,
        .sclk_io_num     = spi->sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = spi->cs;
    slot_cfg.host_id   = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    /* Retry loop — each attempt does a full SPI bus teardown + reinit.
     *
     * Why: after a firmware upload the chip resets but the SPI peripheral
     * is not power-cycled.  The bus may be left mid-transaction, and the
     * SD card's internal state machine may be waiting for a CS de-assert
     * or a clock edge that never comes.  A single spi_bus_initialize()
     * call before the loop cannot fix this because the card is already in
     * an unexpected state.  Calling spi_bus_free() first tears down the
     * ESP-IDF driver completely; the subsequent spi_bus_initialize() then
     * pulls CS high, resets the clock, and lets the card time-out its own
     * state machine — giving it a clean SPI negotiation on the next mount
     * attempt.  spi_bus_free() is safe to call even when the bus was
     * never initialised (it returns ESP_ERR_INVALID_STATE which we ignore). */
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 10; attempt++) {
        /* Tear down whatever state the bus is in before each attempt */
        spi_bus_free(SPI2_HOST);
        vTaskDelay(pdMS_TO_TICKS(50));   /* brief CS-high settle */

        ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SD mount: SPI bus init failed (attempt %d): %s",
                     attempt + 1, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot_cfg,
                                       &mount_cfg, &s_card);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD mounted (attempt %d) — %.0f MB",
                     attempt + 1,
                     (double)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size)
                     / (1024.0 * 1024.0));
            return ESP_OK;
        }

        ESP_LOGW(TAG, "SD mount failed (attempt %d): %s",
                 attempt + 1, esp_err_to_name(ret));

        /* Clean up any partial mount state before next attempt */
        if (s_card) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
            s_card = NULL;
        }
        spi_bus_free(SPI2_HOST);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return ret;
}

/* ── Table creation ──────────────────────────────────────────── */
static void create_tables(void)
{
    db_lock();
    db_exec_raw("BEGIN;");

    db_exec_raw(
        "CREATE TABLE IF NOT EXISTS config ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL DEFAULT ''"
        ");");

    db_exec_raw(
        "CREATE TABLE IF NOT EXISTS classes ("
        "  class_num INTEGER PRIMARY KEY"
        ");");

    db_exec_raw(
        "CREATE TABLE IF NOT EXISTS batches ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  class_num INTEGER NOT NULL,"
        "  time_str  TEXT NOT NULL,"
        "  UNIQUE(class_num, time_str)"
        ");");

    db_exec_raw(
        "CREATE TABLE IF NOT EXISTS schema_cols ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  col_type  TEXT NOT NULL,"
        "  col_key   TEXT NOT NULL,"
        "  label     TEXT NOT NULL,"
        "  required  INTEGER DEFAULT 0,"
        "  ask       INTEGER DEFAULT 1,"
        "  col_order INTEGER DEFAULT 0,"
        "  UNIQUE(col_type, col_key)"
        ");");

    db_exec_raw(
        "CREATE TABLE IF NOT EXISTS students ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  class_num  INTEGER NOT NULL,"
        "  card_uid   TEXT NOT NULL,"
        "  name       TEXT NOT NULL DEFAULT '',"
        "  roll       TEXT DEFAULT '',"
        "  batchtime  TEXT NOT NULL DEFAULT '',"
        "  extra_data TEXT DEFAULT '{}',"
        "  is_active  INTEGER DEFAULT 1,"
        "  created_at TEXT DEFAULT (datetime('now','localtime')),"
        "  UNIQUE(class_num, card_uid)"
        ");");

    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_stu_uid   ON students(card_uid);");
    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_stu_class ON students(class_num);");

    db_exec_raw(
        "CREATE TABLE IF NOT EXISTS attendance ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  class_num  INTEGER NOT NULL,"
        "  card_uid   TEXT NOT NULL,"
        "  name       TEXT NOT NULL DEFAULT '',"
        "  roll       TEXT DEFAULT '',"
        "  batchtime  TEXT DEFAULT '',"
        "  entry_date TEXT NOT NULL,"
        "  entry_time TEXT NOT NULL,"
        "  status     TEXT DEFAULT 'present',"
        "  extra_data TEXT DEFAULT '{}',"
        "  UNIQUE(class_num, card_uid, entry_date)"
        ");");

    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_uid       ON attendance(card_uid);");
    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_date      ON attendance(entry_date);");
    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_class     ON attendance(class_num);");
    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_classdate ON attendance(class_num,entry_date);");
    /* Covering index for per-student COUNT in db_students_stream_json — avoids
       a full table scan and eliminates any need for a temp B-tree sort.        */
    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_uid_class ON attendance(card_uid,class_num);\n");
    db_exec_raw("CREATE INDEX IF NOT EXISTS idx_stu_class_active_name ON students(class_num,is_active,name);");

    db_exec_raw("COMMIT;");
    db_unlock();
}

/* ── Schema migrations ───────────────────────────────────────── *
 * Bug D: CREATE TABLE IF NOT EXISTS never alters an existing table,
 * so the UNIQUE constraint fixes in create_tables() only apply to a
 * fresh DB.  run_migrations() uses PRAGMA user_version to detect
 * old schemas and rebuilds both tables with the correct constraints
 * using the rename-dance: create_new -> copy -> drop_old -> rename. */
static void run_migrations(void)
{
    db_lock();

    sqlite3_stmt *vstmt;
    int version = 0;
    if (sqlite3_prepare_v2(s_db, "PRAGMA user_version;", -1, &vstmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(vstmt) == SQLITE_ROW)
            version = sqlite3_column_int(vstmt, 0);
        sqlite3_finalize(vstmt);
    }

    if (version < 1) {
        ESP_LOGI(TAG, "run_migrations: upgrading schema to v1 (per-class UNIQUE)");
        db_exec_raw("BEGIN;");

        /* ---- Rebuild students ---- */
        db_exec_raw(
            "CREATE TABLE IF NOT EXISTS students_v2 ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  class_num  INTEGER NOT NULL,"
            "  card_uid   TEXT NOT NULL,"
            "  name       TEXT NOT NULL DEFAULT '',"
            "  roll       TEXT DEFAULT '',"
            "  batchtime  TEXT NOT NULL DEFAULT '',"
            "  extra_data TEXT DEFAULT '{}',"
            "  is_active  INTEGER DEFAULT 1,"
            "  created_at TEXT DEFAULT (datetime('now','localtime')),"
            "  UNIQUE(class_num, card_uid)"
            ");");
        db_exec_raw(
            "INSERT OR IGNORE INTO students_v2"
            "  (id,class_num,card_uid,name,roll,batchtime,extra_data,is_active,created_at)"
            " SELECT id,class_num,card_uid,name,roll,batchtime,extra_data,is_active,created_at"
            " FROM students;");
        db_exec_raw("DROP TABLE students;");
        db_exec_raw("ALTER TABLE students_v2 RENAME TO students;");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_stu_uid   ON students(card_uid);");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_stu_class ON students(class_num);");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_stu_class_active_name ON students(class_num,is_active,name);");

        /* ---- Rebuild attendance ---- */
        db_exec_raw(
            "CREATE TABLE IF NOT EXISTS attendance_v2 ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  class_num  INTEGER NOT NULL,"
            "  card_uid   TEXT NOT NULL,"
            "  name       TEXT NOT NULL DEFAULT '',"
            "  roll       TEXT DEFAULT '',"
            "  batchtime  TEXT DEFAULT '',"
            "  entry_date TEXT NOT NULL,"
            "  entry_time TEXT NOT NULL,"
            "  status     TEXT DEFAULT 'present',"
            "  extra_data TEXT DEFAULT '{}',"
            "  UNIQUE(class_num, card_uid, entry_date)"
            ");");
        db_exec_raw(
            "INSERT OR IGNORE INTO attendance_v2"
            "  (id,class_num,card_uid,name,roll,batchtime,entry_date,entry_time,status,extra_data)"
            " SELECT id,class_num,card_uid,name,roll,batchtime,entry_date,entry_time,status,extra_data"
            " FROM attendance;");
        db_exec_raw("DROP TABLE attendance;");
        db_exec_raw("ALTER TABLE attendance_v2 RENAME TO attendance;");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_uid       ON attendance(card_uid);");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_date      ON attendance(entry_date);");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_class     ON attendance(class_num);");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_classdate ON attendance(class_num,entry_date);");
        db_exec_raw("CREATE INDEX IF NOT EXISTS idx_att_uid_class ON attendance(card_uid,class_num);");

        db_exec_raw("PRAGMA user_version = 1;");
        db_exec_raw("COMMIT;");
        ESP_LOGI(TAG, "run_migrations: v1 migration complete");
    }

    db_unlock();
}

/* ── Public: init ────────────────────────────────────────────── */
int db_init(const db_spi_config_t *spi)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_spi_cfg = *spi;   /* store for db_sd_remount() */

    int rc = sqlite3_initialize();
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "sqlite3_initialize: %d", rc);
        return ESP_FAIL;
    }

    /* ── Outer loop: full SD unmount → remount → sqlite open cycle ──────
     *
     * A single attempt is not enough after a firmware upload.  The flash
     * tool resets the chip without power-cycling the SD card.  Two things
     * can go wrong independently:
     *
     *  (a) The SPI bus is in a dirty state → sd_mount() now handles this
     *      with per-attempt bus teardown + reinit (see sd_mount() above).
     *
     *  (b) The WAL file is partially written from the moment the firmware
     *      upload began (the upload tool asserts RST while a write was
     *      in flight) → sqlite3_open() tries to recover the WAL, fails,
     *      and returns SQLITE_CORRUPT or SQLITE_NOTADB.  We clear the WAL
     *      after two failed open attempts and retry.
     *
     * The outer loop lets us do a full SD unmount + remount if even the
     * WAL-cleared open fails, giving the card a second electrical reset. */
    esp_err_t mount_ret = ESP_FAIL;
    for (int cycle = 0; cycle < 3; cycle++) {
        if (cycle > 0) {
            /* Full teardown before retry cycle */
            if (s_db)   { sqlite3_close_v2(s_db); s_db = NULL; }
            if (s_card) {
                esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
                s_card = NULL;
            }
            spi_bus_free(SPI2_HOST);
            ESP_LOGW(TAG, "db_init: full remount cycle %d/3 ...", cycle + 1);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        mount_ret = sd_mount(spi);
        if (mount_ret != ESP_OK) {
            ESP_LOGW(TAG, "db_init: sd_mount failed on cycle %d", cycle + 1);
            continue;   /* try full remount next cycle */
        }

        /* FIX-31: Check mkdir return */
        if (mkdir(SD_MOUNT "/sd", 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "db_init: failed to create backup dir: errno=%d", errno);
        }

        FILE *touch = fopen(DB_PATH, "ab");
        if (touch) fclose(touch);
        else ESP_LOGW(TAG, "Could not pre-create %s — sqlite3_open may fail", DB_PATH);

        /* Inner loop: sqlite3_open with mid-loop WAL clear.
         * Attempts 0-1: normal open — the WAL may contain valid committed
         *               data; do NOT touch it on a mere I/O error.
         * Attempt 2+  : if the error is a real corruption error
         *               (SQLITE_CORRUPT / SQLITE_NOTADB) clear the WAL and
         *               retry.  A plain SQLITE_IOERR just means the card is
         *               still settling; deleting the WAL in that case loses
         *               all committed-but-uncheckpointed data.             */
        bool wal_cleared = false;
        for (int attempt = 0; attempt < 5; attempt++) {
            if (attempt >= 2 && !wal_cleared &&
                (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)) {
                ESP_LOGW(TAG, "db_init: WAL corrupt (rc=%d) — clearing WAL", rc);
                remove(DB_PATH "-wal");
                remove(DB_PATH "-shm");
                wal_cleared = true;
            }
            rc = sqlite3_open(DB_PATH, &s_db);
            if (rc == SQLITE_OK) break;
            ESP_LOGW(TAG, "db_init: sqlite3_open attempt %d (rc=%d): %s",
                     attempt + 1, rc, sqlite3_errmsg(s_db));
            sqlite3_close(s_db);
            s_db = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (s_db && rc == SQLITE_OK) break;   /* success — exit outer loop */

        ESP_LOGW(TAG, "db_init: sqlite3_open failed all attempts on cycle %d", cycle + 1);
    }

    if (!s_db || rc != SQLITE_OK) {
        ESP_LOGE(TAG, "db_init: could not open database after all retry cycles");
        return ESP_FAIL;
    }

    /* Pragmas
     * FIX-28: synchronous=NORMAL is safe with WAL and faster than FULL. */
    db_lock();
    db_exec_raw("PRAGMA journal_mode=WAL;");
    db_exec_raw("PRAGMA synchronous=NORMAL;");
    db_exec_raw("PRAGMA cache_size=64;");
    db_exec_raw("PRAGMA temp_store=MEMORY;");
    db_exec_raw("PRAGMA foreign_keys=ON;");
    /* FIX-BUG3: Keep WAL small so little data is at risk between checkpoints */
    db_exec_raw("PRAGMA wal_autocheckpoint=100;");
    db_unlock();

    create_tables();
    run_migrations();
    ESP_LOGI(TAG, "Database ready: %s", DB_PATH);
    return ESP_OK;
}

/* ── Default seeds ───────────────────────────────────────────── */
void db_ensure_default_classes(void)
{
    db_lock();
    sqlite3_stmt *chk;
    sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM classes", -1, &chk, NULL);
    int count = (sqlite3_step(chk) == SQLITE_ROW) ? sqlite3_column_int(chk, 0) : 0;
    sqlite3_finalize(chk);

    if (count > 0) { db_unlock(); return; }

    db_exec_raw("BEGIN;");
    const char *times[] = {"08:00","10:00","12:00","14:00","16:00"};
    for (int c = 6; c <= 10; c++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT OR IGNORE INTO classes(class_num) VALUES(%d);", c);
        db_exec_raw(sql);
        for (int t = 0; t < 5; t++) {
            snprintf(sql, sizeof(sql),
                     "INSERT OR IGNORE INTO batches(class_num,time_str)"
                     " VALUES(%d,'%s');", c, times[t]);
            db_exec_raw(sql);
        }
    }
    db_exec_raw("COMMIT;");
    db_unlock();
    ESP_LOGI(TAG, "Default classes 6-10 seeded");
}


/* ── Time helpers ────────────────────────────────────────────── */
void db_sntp_sync(void)
{
    /*
     * ESP-IDF v5: use esp_netif_sntp_* API exclusively (esp_sntp.h is
     * deprecated).  CONFIG_LWIP_SNTP_MAX_SERVERS is 1 in sdkconfig, so
     * we configure a single server via ESP_NETIF_SNTP_DEFAULT_CONFIG().
     * If you need a fallback server, raise CONFIG_LWIP_SNTP_MAX_SERVERS
     * to 2 in sdkconfig.defaults and use ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE().
     */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.start       = true;
    sntp_cfg.smooth_sync = false;
    esp_netif_sntp_init(&sntp_cfg);

    /* UTC+6 (Bangladesh Standard Time) */
    setenv("TZ", "BDT-6", 1);
    tzset();

    ESP_LOGI(TAG, "Waiting for NTP sync...");
    /* BUG FIX: 10 s was too short — DNS resolution alone can take several
     * seconds on a busy network.  30 s gives enough headroom for the full
     * resolve → connect → response round-trip. */
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NTP synced");
        /* FIX-36: Persist success so the UI can confirm the clock is good */
        db_config_set("ntp_synced", "1");
    } else {
        ESP_LOGW(TAG, "NTP sync timed out — time may be inaccurate");
        /* FIX-36: Persist failure — UI will show a warning banner alerting
         * the operator that attendance timestamps may be year-2000 garbage */
        db_config_set("ntp_synced", "0");
    }
}

void db_get_localtime(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
}

void db_today_string(char *buf, size_t len)
{
    struct tm ti;
    db_get_localtime(&ti);
    strftime(buf, len, "%Y-%m-%d", &ti);
}

void db_now_time_string(char *buf, size_t len)
{
    struct tm ti;
    db_get_localtime(&ti);
    strftime(buf, len, "%H:%M:%S", &ti);
}

/* ── Free ────────────────────────────────────────────────────── */
void db_free(void *ptr) { free(ptr); }

/* ── Backup ──────────────────────────────────────────────────── */
/* FIX-30: Use SQLite Online Backup API instead of closing the DB, doing a
 * raw file copy, then reopening.  The old approach held the mutex for the
 * entire multi-megabyte copy, freezing the web UI and RFID scanner.
 * The online backup API copies pages incrementally, releasing the mutex
 * between steps so other tasks can proceed. */
void db_sd_backup(void)
{
    char today[12];
    db_today_string(today, sizeof(today));

    char bak_path[64];
    snprintf(bak_path, sizeof(bak_path), SD_MOUNT "/sd/attendance_%s.bak", today);

    struct stat st;
    if (stat(bak_path, &st) == 0) {
        ESP_LOGI(TAG, "Backup already exists: %s", bak_path);
        return;
    }

    sqlite3 *dst_db = NULL;
    if (sqlite3_open(bak_path, &dst_db) != SQLITE_OK) {
        ESP_LOGW(TAG, "Backup: failed to open destination: %s", bak_path);
        if (dst_db) sqlite3_close(dst_db);
        return;
    }

    db_lock();
    sqlite3_backup *bkp = sqlite3_backup_init(dst_db, "main", s_db, "main");
    db_unlock();

    if (!bkp) {
        ESP_LOGW(TAG, "Backup: sqlite3_backup_init failed");
        sqlite3_close(dst_db);
        return;
    }

    int rc;
    do {
        db_lock();
        rc = sqlite3_backup_step(bkp, 32);   /* copy 32 pages per step */
        db_unlock();
        if (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
            vTaskDelay(pdMS_TO_TICKS(5));    /* yield between steps */
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    sqlite3_backup_finish(bkp);
    sqlite3_close(dst_db);

    if (rc == SQLITE_DONE)
        ESP_LOGI(TAG, "Backup written: %s", bak_path);
    else
        ESP_LOGW(TAG, "Backup incomplete, rc=%d", rc);
}

/* ── SD card health check ────────────────────────────────────── */
/* Returns true when the SD mount-point is accessible.
 * A simple stat() on SD_MOUNT catches the "card is physically present
 * but VFS has gone silent" failure mode that resets don't fix. */
bool db_sd_health_check(void)
{
    /* stat() is NOT reliable here: the ESP-IDF FAT VFS keeps its directory
     * entry cache alive even after the physical card disappears, so stat()
     * on "/sdcard" or on a known file returns ESP_OK even when the card is
     * completely gone.
     *
     * Reliable check: actually write one byte to a probe file and read it
     * back.  Any real I/O failure surfaces immediately with a NULL return
     * from fopen() or a short read.                                        */
    static const char *PROBE = SD_MOUNT "/.hprobe";

    FILE *f = fopen(PROBE, "wb");
    if (!f) {
        ESP_LOGW(TAG, "SD health: fopen write failed (errno=%d)", errno);
        return false;
    }
    fputc(0xA5, f);
    fclose(f);

    f = fopen(PROBE, "rb");
    if (!f) {
        ESP_LOGW(TAG, "SD health: fopen read failed (errno=%d)", errno);
        return false;
    }
    int ch = fgetc(f);
    fclose(f);
    remove(PROBE);

    if (ch != 0xA5) {
        ESP_LOGW(TAG, "SD health: probe byte mismatch (got %d)", ch);
        return false;
    }
    return true;
}

/* ── SD card soft re-mount ───────────────────────────────────── */
/* Closes the live DB, unmounts the card, re-mounts it, and reopens
 * the database — all without a reboot or a format.
 * Should be called when db_sd_health_check() returns false.        */
int db_sd_remount(void)
{
    /* FIX-BUG1: If an HTTP streaming response is in progress, STREAM_CB has
     * released db_lock() to send a chunk over the network.  Proceeding with
     * remount now would call sqlite3_close_v2() while that response's prepared
     * statement is still live, and then unmount the FAT volume under it.
     * Abort and let the watchdog retry on the next 10-second cycle instead.  */
    if (db_is_streaming()) {
        ESP_LOGW(TAG, "SD remount deferred — streaming response in progress");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "SD remount: starting...");

    /* 1. Close the live database (must hold lock) */
    db_lock();
    if (s_db) {
        /* FIX-BUG3: Force a full WAL checkpoint before closing so that all
         * committed pages are flushed into the main .db file.  Without this,
         * any pages that were written to the WAL file but not yet merged into
         * the main database are lost when the FAT volume is unmounted — this
         * was the primary cause of "data not coming to server" intermittently.
         * TRUNCATE resets the WAL file to zero length after the checkpoint so
         * the card holds a clean, self-contained .db with no pending WAL.    */
        int ck_rc = sqlite3_exec(s_db,
            "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
        if (ck_rc != SQLITE_OK)
            ESP_LOGW(TAG, "SD remount: wal_checkpoint returned %d — proceeding anyway", ck_rc);

        /* FIX-BUG2: Use sqlite3_close_v2() instead of sqlite3_close().
         * sqlite3_close() returns SQLITE_BUSY and does nothing if any
         * prepared statement is still open (e.g. a streaming HTTP response
         * that released db_lock() mid-flight via STREAM_CB).  Ignoring that
         * return value and setting s_db = NULL anyway leaves SQLite in an
         * inconsistent state and causes the subsequent FAT unmount to
         * corrupt the VFS layer.
         * sqlite3_close_v2() defers the actual teardown until the last
         * prepared statement is finalized — safe to call even when stmts
         * are still alive.                                                    */
        sqlite3_close_v2(s_db);
        s_db = NULL;
    }
    db_unlock();

    /* 2. Unmount the FAT volume (also removes the SDSPI device internally) */
    if (s_card) {
        esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
        if (ret != ESP_OK)
            ESP_LOGW(TAG, "SD remount: unmount returned %s", esp_err_to_name(ret));
        s_card = NULL;
    }

    /* 3. Free the SPI bus so spi_bus_initialize() in sd_mount() can
     *    reinitialise it cleanly.  Without this the second call returns
     *    ESP_ERR_INVALID_STATE and the SDSPI device is never re-created,
     *    making every subsequent mount attempt silently fail.             */
    spi_bus_free(SPI2_HOST);
    vTaskDelay(pdMS_TO_TICKS(200));   /* brief settle time */

    /* 4. Re-mount */
    esp_err_t ret = sd_mount(&s_spi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD remount: mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. Recreate backup dir if needed */
    if (mkdir(SD_MOUNT "/sd", 0775) != 0 && errno != EEXIST)
        ESP_LOGW(TAG, "SD remount: mkdir /sd errno=%d", errno);

    /* 5. Re-open / create the database file */
    FILE *touch = fopen(DB_PATH, "ab");
    if (touch) fclose(touch);

    /* Open SQLite — try without touching the WAL first so that any
     * committed-but-not-yet-checkpointed data survives.  Only delete
     * the WAL/SHM if the open fails with an actual corruption error
     * (SQLITE_CORRUPT / SQLITE_NOTADB).  A plain SQLITE_IOERR just
     * means the card is still settling; the WAL is intact and must not
     * be destroyed.
     *
     * FIX-BUG-B: Release the mutex between retry attempts so that the
     * RFID handler and HTTP tasks are not blocked for the full delay. */
    int rc = SQLITE_OK;
    bool wal_cleared = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        db_lock();
        rc = sqlite3_open(DB_PATH, &s_db);
        if (rc == SQLITE_OK) {
            db_exec_raw("PRAGMA journal_mode=WAL;");
            db_exec_raw("PRAGMA synchronous=NORMAL;");
            db_exec_raw("PRAGMA cache_size=64;");
            db_exec_raw("PRAGMA temp_store=MEMORY;");
            db_exec_raw("PRAGMA foreign_keys=ON;");
            /* Re-apply low auto-checkpoint threshold after remount */
            db_exec_raw("PRAGMA wal_autocheckpoint=100;");
            db_unlock();
            break;
        }
        ESP_LOGW(TAG, "SD remount: sqlite3_open attempt %d (rc=%d): %s",
                 attempt + 1, rc, sqlite3_errmsg(s_db));
        sqlite3_close(s_db);
        s_db = NULL;
        db_unlock();

        /* Only clear the WAL when the error is a real corruption error,
         * not a transient I/O error while the card is settling.
         * Clearing on SQLITE_IOERR would destroy valid committed data. */
        if (!wal_cleared &&
            (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)) {
            ESP_LOGW(TAG, "SD remount: WAL corrupt — clearing WAL/SHM");
            remove(DB_PATH "-wal");
            remove(DB_PATH "-shm");
            wal_cleared = true;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));    /* yield — other tasks can run */
    }

    if (rc == SQLITE_OK) {
        create_tables();
        ESP_LOGI(TAG, "SD remount: database re-opened OK");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "SD remount: failed to reopen database");
    return ESP_FAIL;
}

/* ── SQLite-only reopen (no SD remount) ─────────────────────── */
/* Called by the watchdog when db_sd_health_check() passes (FAT is fine)
 * but db_handle() == NULL — meaning sqlite3_open() failed during a
 * previous remount attempt (e.g. WAL was corrupt or the card was still
 * settling at that moment).  We do NOT unmount/remount the SD; we just
 * clear the stale WAL files and retry open() on the already-mounted FS.
 *
 * This closes the "permanently NULL handle" gap described as Bug C:
 * without this function, a single failed sqlite3_open() inside
 * db_sd_remount() left s_db == NULL for the entire session with no
 * recovery path, because the watchdog only checked health_check() and
 * never inspected db_handle().                                           */
esp_err_t db_sqlite_reopen(void)
{
    ESP_LOGI(TAG, "db_sqlite_reopen: attempting to recover NULL handle...");

    /* Pre-create the DB file in case the previous remount left none */
    FILE *touch = fopen(DB_PATH, "ab");
    if (touch) fclose(touch);
    else ESP_LOGW(TAG, "db_sqlite_reopen: fopen pre-create failed");

    int rc = SQLITE_OK;
    bool wal_cleared = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        /* Only clear the WAL if a previous attempt returned a real corruption
         * error.  Do not delete on SQLITE_IOERR — the WAL may be healthy and
         * contain committed data; the card may just still be settling.       */
        if (!wal_cleared && attempt >= 1 &&
            (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)) {
            ESP_LOGW(TAG, "db_sqlite_reopen: WAL corrupt (rc=%d) — clearing WAL/SHM", rc);
            remove(DB_PATH "-wal");
            remove(DB_PATH "-shm");
            wal_cleared = true;
        }
        db_lock();
        rc = sqlite3_open(DB_PATH, &s_db);
        if (rc == SQLITE_OK) {
            db_exec_raw("PRAGMA journal_mode=WAL;");
            db_exec_raw("PRAGMA synchronous=NORMAL;");
            db_exec_raw("PRAGMA cache_size=64;");
            db_exec_raw("PRAGMA temp_store=MEMORY;");
            db_exec_raw("PRAGMA foreign_keys=ON;");
            db_exec_raw("PRAGMA wal_autocheckpoint=100;");
            db_unlock();
            break;
        }
        ESP_LOGW(TAG, "db_sqlite_reopen: attempt %d (rc=%d): %s",
                 attempt + 1, rc, sqlite3_errmsg(s_db));
        sqlite3_close(s_db);
        s_db = NULL;
        db_unlock();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (rc == SQLITE_OK) {
        create_tables();
        ESP_LOGI(TAG, "db_sqlite_reopen: handle recovered OK");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "db_sqlite_reopen: all attempts failed — handle still NULL");
    return ESP_FAIL;
}

/* ── List backup files ───────────────────────────────────────── */
/* Scans /sdcard/sd/ for attendance_*.bak files and returns a
 * malloc'd JSON array of filenames sorted newest-first.
 * Caller must free().                                              */
char *db_sd_list_backups(void)
{
    DIR *d = opendir(SD_MOUNT "/sd");
    if (!d) {
        /* dir may not exist yet */
        return strdup("[]");
    }

    /* Collect matching names into a simple fixed-size array */
    #define MAX_BAKS 64
    char names[MAX_BAKS][48];
    int  n = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < MAX_BAKS) {
        if (strncmp(ent->d_name, "attendance_", 11) == 0 &&
            strstr(ent->d_name, ".bak") != NULL) {
            strncpy(names[n], ent->d_name, sizeof(names[n]) - 1);
            names[n][sizeof(names[n]) - 1] = '\0';
            n++;
        }
    }
    closedir(d);

    /* Sort descending (newest date string first via strcmp reversed) */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) < 0) {
                char tmp[48];
                strncpy(tmp,      names[i], sizeof(tmp));
                strncpy(names[i], names[j], sizeof(names[i]));
                strncpy(names[j], tmp,      sizeof(names[j]));
            }

    /* Build JSON array */
    size_t cap = (size_t)(n * 56 + 8);
    char  *out = malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;
    out[pos++] = '[';
    for (int i = 0; i < n; i++) {
        int written = snprintf(out + pos, cap - pos,
                               "%s\"%s\"", i == 0 ? "" : ",", names[i]);
        pos += (size_t)written;
    }
    out[pos++] = ']';
    out[pos]   = '\0';
    return out;
}

/* ── Restore from backup ─────────────────────────────────────── */
/* Replaces the live database with the named backup file.
 * 'filename' must be a bare filename (e.g. "attendance_2026-04-09.bak"),
 * NOT a full path — the function prepends /sdcard/sd/ automatically.
 * Uses SQLite's Online Backup API so the restore is atomic:
 * if it fails halfway the live DB is unchanged.                    */
bool db_sd_restore(const char *filename)
{
    if (!filename || !*filename) return false;

    /* Block path traversal */
    if (strstr(filename, "/") || strstr(filename, "..")) {
        ESP_LOGW(TAG, "db_sd_restore: invalid filename");
        return false;
    }

    char src_path[64];
    snprintf(src_path, sizeof(src_path), SD_MOUNT "/sd/%s", filename);

    struct stat st;
    if (stat(src_path, &st) != 0) {
        ESP_LOGW(TAG, "db_sd_restore: file not found: %s", src_path);
        return false;
    }

    /* Open the backup file as the source SQLite database */
    sqlite3 *src_db = NULL;
    if (sqlite3_open_v2(src_path, &src_db,
                        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        ESP_LOGW(TAG, "db_sd_restore: cannot open source: %s",
                 sqlite3_errmsg(src_db));
        if (src_db) sqlite3_close(src_db);
        return false;
    }

    /* Use backup API: source=src_db, destination=live s_db */
    db_lock();
    sqlite3_backup *bkp = sqlite3_backup_init(s_db, "main", src_db, "main");
    db_unlock();

    if (!bkp) {
        ESP_LOGW(TAG, "db_sd_restore: backup_init failed");
        sqlite3_close(src_db);
        return false;
    }

    int rc;
    do {
        db_lock();
        rc = sqlite3_backup_step(bkp, 32);
        db_unlock();
        if (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
            vTaskDelay(pdMS_TO_TICKS(5));
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    sqlite3_backup_finish(bkp);
    sqlite3_close(src_db);

    if (rc == SQLITE_DONE) {
        /* Flush the page cache so restored pages are read fresh,
         * then truncate the stale WAL that belongs to the old DB. */
        db_lock();
        db_exec_raw("PRAGMA cache_size=1;");
        db_exec_raw("PRAGMA cache_size=64;");
        int ck_rc = sqlite3_exec(s_db,
                "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
        if (ck_rc != SQLITE_OK)
            ESP_LOGW(TAG, "db_sd_restore: wal_checkpoint returned %d — proceeding anyway", ck_rc);
        db_unlock();
        ESP_LOGI(TAG, "db_sd_restore: restored from %s", src_path);
        return true;
    }
    ESP_LOGW(TAG, "db_sd_restore: incomplete, rc=%d", rc);
    return false;
}

/* ── Delete a backup file ────────────────────────────────────── */
bool db_sd_delete_backup(const char *filename)
{
    if (!filename || !*filename) return false;
    if (strstr(filename, "/") || strstr(filename, "..")) {
        ESP_LOGW(TAG, "db_sd_delete_backup: invalid filename");
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), SD_MOUNT "/sd/%s", filename);
    if (remove(path) != 0) {
        ESP_LOGW(TAG, "db_sd_delete_backup: remove failed errno=%d", errno);
        return false;
    }
    ESP_LOGI(TAG, "Backup deleted: %s", path);
    return true;
}

/* ── Status JSON ─────────────────────────────────────────────── */
char *db_status_json(int class_num)
{
    char today[12];
    db_today_string(today, sizeof(today));

    int total = 0, present = 0, total_rec = 0;

    db_lock();

    sqlite3_stmt *s;
    char sql[200];

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM students WHERE class_num=%d AND is_active=1",
             class_num);
    if (sqlite3_prepare_v2(s_db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) total = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM attendance"
             " WHERE class_num=%d AND entry_date='%s' AND status='present'",
             class_num, today);
    if (sqlite3_prepare_v2(s_db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) present = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM attendance WHERE class_num=%d", class_num);
    if (sqlite3_prepare_v2(s_db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) total_rec = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }

    db_unlock();

    char *out = malloc(256);
    if (!out) return NULL;
    snprintf(out, 256,
             "{\"class\":%d,\"date\":\"%s\","
             "\"total\":%d,\"present\":%d,\"absent\":%d,"
             "\"records\":%d,\"heap\":%lu}",
             class_num, today,
             total, present, total - present,
             total_rec,
             (unsigned long)esp_get_free_heap_size());
    return out;
}
