#include "db.h"
#include "sqlite3.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "db_config";

/* Declared in db.c, shared across all db_*.c files */
extern void     db_lock(void);
extern void     db_unlock(void);
extern sqlite3 *db_handle(void);

char *db_config_get(const char *key)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    char *result = NULL;

    if (sqlite3_prepare_v2(db,
            "SELECT value FROM config WHERE key=? LIMIT 1",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *val = (const char *)sqlite3_column_text(s, 0);
            if (val) result = strdup(val);
        }
        sqlite3_finalize(s);
    }

    db_unlock();
    return result;   /* caller must free() */
}

void db_config_set(const char *key, const char *value)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;

    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO config(key,value) VALUES(?,?)",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key,   -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, value, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    db_unlock();
}

/* ── Classes list JSON ───────────────────────────────────────── */
char *db_classes_list_json(void)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;

    /* First pass: count */
    int count = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM classes", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) count = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }

    /* Build JSON array: [{class_num,student_count,record_count},...] */
    /* Estimate size: each entry ~60 chars */
    size_t cap = (size_t)(count * 80 + 8);
    char *out = malloc(cap);
    if (!out) { db_unlock(); return NULL; }
    size_t pos = 0;
    out[pos++] = '[';

    if (sqlite3_prepare_v2(db,
            "SELECT c.class_num,"
            " (SELECT COUNT(*) FROM students s WHERE s.class_num=c.class_num AND s.is_active=1) AS stu,"
            " (SELECT COUNT(*) FROM attendance a WHERE a.class_num=c.class_num) AS att"
            " FROM classes c ORDER BY c.class_num",
            -1, &s, NULL) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(s) == SQLITE_ROW) {
            int cn  = sqlite3_column_int(s, 0);
            int stu = sqlite3_column_int(s, 1);
            int att = sqlite3_column_int(s, 2);
            int n = snprintf(out + pos, cap - pos,
                             "%s{\"class_num\":%d,\"student_count\":%d,\"record_count\":%d}",
                             first ? "" : ",", cn, stu, att);
            pos += (size_t)n;
            first = false;
        }
        sqlite3_finalize(s);
    }

    if (pos + 2 < cap) { out[pos++] = ']'; out[pos] = '\0'; }
    db_unlock();
    return out;
}

bool db_class_exists(int class_num)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    bool found = false;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM classes WHERE class_num=? LIMIT 1",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, class_num);
        found = (sqlite3_step(s) == SQLITE_ROW);
        sqlite3_finalize(s);
    }
    db_unlock();
    return found;
}

int db_class_add(int class_num)
{
    db_lock();
    sqlite3 *db = db_handle();
    bool ok = false;

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO classes(class_num) VALUES(?)",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, class_num);
        ok = (sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }
    /* Default batches */
    if (ok) {
        const char *times[] = {"08:00","10:00","12:00","14:00","16:00"};
        for (int i = 0; i < 5; i++) {
            if (sqlite3_prepare_v2(db,
                    "INSERT OR IGNORE INTO batches(class_num,time_str) VALUES(?,?)",
                    -1, &s, NULL) == SQLITE_OK) {
                sqlite3_bind_int(s, 1, class_num);
                sqlite3_bind_text(s, 2, times[i], -1, SQLITE_STATIC);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }
    }
    db_unlock();
    return ok ? 0 : -1;
}

int db_class_delete(int class_num)
{
    /* FIX-29: Wrap all four DELETEs in a single transaction.  Without this,
     * a power loss between any two statements leaves orphaned rows in the DB. */
    db_lock();
    char sql[120];
    db_exec_raw("BEGIN;");
    snprintf(sql,sizeof(sql),"DELETE FROM students   WHERE class_num=%d", class_num);
    db_exec_raw(sql);
    snprintf(sql,sizeof(sql),"DELETE FROM attendance WHERE class_num=%d", class_num);
    db_exec_raw(sql);
    snprintf(sql,sizeof(sql),"DELETE FROM batches    WHERE class_num=%d", class_num);
    db_exec_raw(sql);
    snprintf(sql,sizeof(sql),"DELETE FROM classes    WHERE class_num=%d", class_num);
    db_exec_raw(sql);
    db_exec_raw("COMMIT;");
    db_unlock();
    return 0;
}

char *db_batches_list_json(int class_num)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;

    char sql[128];
    snprintf(sql, sizeof(sql),
             "SELECT time_str FROM batches WHERE class_num=%d ORDER BY time_str",
             class_num);

    /* Collect results */
    char *out = malloc(256);
    if (!out) { db_unlock(); return NULL; }
    size_t pos = 0, cap = 256;
    out[pos++] = '[';
    bool first = true;

    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            if (!t) continue;
            /* Grow buffer if needed */
            size_t needed = pos + strlen(t) + 6;
            if (needed > cap) {
                cap = needed * 2;
                char *tmp = realloc(out, cap);
                if (!tmp) break;
                out = tmp;
            }
            int n = snprintf(out + pos, cap - pos,
                             "%s\"%s\"", first ? "" : ",", t);
            pos += (size_t)n;
            first = false;
        }
        sqlite3_finalize(s);
    }

    /* Fallback defaults if no rows */
    if (first) {
        const char *def[] = {"08:00","10:00","12:00","14:00","16:00"};
        for (int i = 0; i < 5; i++) {
            int n = snprintf(out + pos, cap - pos,
                             "%s\"%s\"", i == 0 ? "" : ",", def[i]);
            pos += (size_t)n;
        }
    }

    out[pos++] = ']';
    out[pos]   = '\0';
    db_unlock();
    return out;
}

int db_batch_add(int class_num, const char *time_str)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO batches(class_num,time_str) VALUES(?,?)",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s,  1, class_num);
        sqlite3_bind_text(s, 2, time_str, -1, SQLITE_STATIC);
        rc = (sqlite3_step(s) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(s);
    }
    db_unlock();
    return rc;
}

int db_batch_delete(int class_num, const char *time_str)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM batches WHERE class_num=? AND time_str=?",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s,  1, class_num);
        sqlite3_bind_text(s, 2, time_str, -1, SQLITE_STATIC);
        rc = (sqlite3_step(s) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(s);
    }
    db_unlock();
    return rc;
}
