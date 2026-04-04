/*  db_schema.c — dynamic column schema for student / attendance tables
    Schema table: schema_cols(col_type, key, label, ask, required)
    col_type is "student" or "attendance".                              */

#include "db.h"
#include "sqlite3.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "db_schema";

/* ── Seed default schema if table is empty ───────────────────── */
void db_ensure_default_schema(void)
{
    /* Table already created by db.c create_tables() */
    /* If any rows exist, skip seeding */
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *st = NULL;
    int count = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM schema_cols;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    db_unlock();

    if (count > 0) return;

    /* Default student columns (col_type,col_key,label,required,ask,col_order) */
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('student','name','Name',1,1,0);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('student','roll','Roll No',0,1,1);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('student','card_uid','Card UID',1,1,2);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('student','batchtime','Batch',1,1,3);");

    /* Default attendance columns */
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('attendance','name','Name',0,0,0);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('attendance','roll','Roll No',0,0,1);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('attendance','UID','Card UID',1,0,2);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('attendance','batchtime','Batch',0,0,3);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('attendance','entrytime','Entry Time',1,0,4);");
    db_exec("INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,required,ask,col_order)"
            " VALUES('attendance','status','Status',0,0,5);");

    ESP_LOGI(TAG, "Default schema seeded");
}

/* ── GET /api/schema → JSON ──────────────────────────────────── */
char *db_schema_get_json(void)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT col_type,col_key,label,ask,required FROM schema_cols ORDER BY col_type,col_order,id;";

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        db_unlock();
        ESP_LOGE(TAG, "schema_get_json prepare failed");
        return strdup("{\"student\":[],\"attendance\":[]}");
    }

    /* Build JSON manually — two arrays */
    char *stu_buf = malloc(2048);
    char *att_buf = malloc(2048);
    if (!stu_buf || !att_buf) {
        free(stu_buf); free(att_buf);
        sqlite3_finalize(st);
        db_unlock();
        return strdup("{\"student\":[],\"attendance\":[]}");
    }
    stu_buf[0] = '['; stu_buf[1] = '\0';
    att_buf[0] = '['; att_buf[1] = '\0';
    int stu_first = 1, att_first = 1;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *ctype  = (const char *)sqlite3_column_text(st, 0);
        const char *key    = (const char *)sqlite3_column_text(st, 1);
        const char *label  = (const char *)sqlite3_column_text(st, 2);
        int ask    = sqlite3_column_int(st, 3);
        int req    = sqlite3_column_int(st, 4);

        char entry[256];
        snprintf(entry, sizeof(entry),
                 "%s{\"key\":\"%s\",\"label\":\"%s\",\"ask\":%s,\"required\":%s}",
                 (strcmp(ctype, "student") == 0 ? (stu_first ? "" : ",")
                                                : (att_first ? "" : ",")),
                 key, label,
                 ask ? "true" : "false",
                 req ? "true" : "false");

        if (strcmp(ctype, "student") == 0) {
            strncat(stu_buf, entry, 2047 - strlen(stu_buf));
            stu_first = 0;
        } else {
            strncat(att_buf, entry, 2047 - strlen(att_buf));
            att_first = 0;
        }
    }
    sqlite3_finalize(st);
    db_unlock();

    strncat(stu_buf, "]", 2047 - strlen(stu_buf));
    strncat(att_buf, "]", 2047 - strlen(att_buf));

    /* Combine */
    size_t total = strlen(stu_buf) + strlen(att_buf) + 32;
    char *out = malloc(total);
    if (out)
        snprintf(out, total, "{\"student\":%s,\"attendance\":%s}", stu_buf, att_buf);

    free(stu_buf);
    free(att_buf);
    return out ? out : strdup("{\"student\":[],\"attendance\":[]}");
}

/* ── Add a schema column ─────────────────────────────────────── */
int db_schema_add(const char *col_type, const char *key, const char *label, int ask)
{
    if (!col_type || !key || !label) return -1;
    if (strcmp(col_type, "student") != 0 && strcmp(col_type, "attendance") != 0)
        return -1;

    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *st = NULL;
    int rc = -1;

    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO schema_cols(col_type,col_key,label,ask,required) "
            "VALUES(?,?,?,?,0);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, col_type, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, key,      -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, label,    -1, SQLITE_STATIC);
        sqlite3_bind_int (st, 4, ask);
        rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(st);
    }
    db_unlock();
    return rc;
}

/* ── Delete a schema column ──────────────────────────────────── */
int db_schema_delete(const char *col_type, const char *key)
{
    if (!col_type || !key) return -1;

    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *st = NULL;
    int rc = -1;

    if (sqlite3_prepare_v2(db,
            "DELETE FROM schema_cols WHERE col_type=? AND col_key=?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, col_type, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, key,      -1, SQLITE_STATIC);
        rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(st);
    }
    db_unlock();
    return rc;
}

/* ── Edit a column's label ───────────────────────────────────── */
int db_schema_edit_label(const char *col_type, const char *key, const char *new_label)
{
    if (!col_type || !key || !new_label) return -1;

    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *st = NULL;
    int rc = -1;

    if (sqlite3_prepare_v2(db,
            "UPDATE schema_cols SET label=? WHERE col_type=? AND col_key=?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, new_label, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, col_type,  -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, key,       -1, SQLITE_STATIC);
        rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(st);
    }
    db_unlock();
    return rc;
}

/* ── Toggle ask or required flag ─────────────────────────────── */
int db_schema_toggle_flag(const char *col_type, const char *key,
                           const char *flag, int value)
{
    if (!col_type || !key || !flag) return -1;
    /* Only allow "ask" or "required" column names */
    if (strcmp(flag, "ask") != 0 && strcmp(flag, "required") != 0) return -1;

    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *st = NULL;
    int rc = -1;

    char sql[128];
    /* Safe because flag is validated above */
    snprintf(sql, sizeof(sql),
             "UPDATE schema_cols SET %s=? WHERE col_type=? AND col_key=?;", flag);

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int (st, 1, value);
        sqlite3_bind_text(st, 2, col_type, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, key,      -1, SQLITE_STATIC);
        rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(st);
    }
    db_unlock();
    return rc;
}
