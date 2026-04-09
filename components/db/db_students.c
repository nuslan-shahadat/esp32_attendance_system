#include "db.h"
#include "sqlite3.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "db_students";
/* FIX: Release DB lock during network I/O to prevent ECONNRESET on other endpoints.
 * See db_attendance.c STREAM_CB comment for full rationale. */
#define STREAM_CB(buf, len) do {           \
    db_unlock();                           \
    ret = cb((buf), (size_t)(len), ctx);  \
    db_lock();                            \
} while (0)

extern void     db_lock(void);
extern void     db_unlock(void);
extern sqlite3 *db_handle(void);
extern bool     db_exec_raw(const char *sql);

/* ── JSON string escape helper ───────────────────────────────── */
/* Writes escaped version of src into dst (max dst_len bytes).
   Returns number of bytes written.
   FIX-34: Added optional truncation flag — caller can detect when the
   source string was too long for the destination buffer and log a warning
   rather than silently producing a truncated (potentially malformed) value. */
static size_t json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0;
    while (*src && i + 2 < dst_len) {
        unsigned char c = (unsigned char)*src++;
        switch (c) {
            case '"':  dst[i++]='\\'; dst[i++]='"';  break;
            case '\\': dst[i++]='\\'; dst[i++]='\\'; break;
            case '\n': dst[i++]='\\'; dst[i++]='n';  break;
            case '\r': dst[i++]='\\'; dst[i++]='r';  break;
            case '\t': dst[i++]='\\'; dst[i++]='t';  break;
            default:   dst[i++]=(char)c;               break;
        }
    }
    dst[i] = '\0';
    /* FIX-34: If src is not exhausted, truncation occurred — log a warning */
    if (*src != '\0') {
        ESP_LOGW(TAG, "json_escape: value truncated (dst_len=%zu)", dst_len);
    }
    return i;
}

/* ── Dynamic string builder ──────────────────────────────────── */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} StrBuf;

static bool sb_init(StrBuf *sb, size_t initial)
{
    sb->buf = malloc(initial);
    if (!sb->buf) return false;
    sb->buf[0] = '\0';
    sb->len = 0;
    sb->cap = initial;
    return true;
}

static bool sb_append(StrBuf *sb, const char *s)
{
    size_t slen = strlen(s);
    if (sb->len + slen + 1 > sb->cap) {
        size_t new_cap = (sb->len + slen + 1) * 2;
        char *tmp = realloc(sb->buf, new_cap);
        if (!tmp) return false;
        sb->buf = tmp;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, s, slen + 1);
    sb->len += slen;
    return true;
}

static bool sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    return sb_append(sb, tmp);
}

/* ── Append a JSON-escaped string field ──────────────────────── */
static bool sb_append_jstr(StrBuf *sb, const char *val)
{
    if (!sb_append(sb, "\"")) return false;
    if (val) {
        char esc[256];
        json_escape(val, esc, sizeof(esc));
        if (!sb_append(sb, esc)) return false;
    }
    return sb_append(sb, "\"");
}

/* ── Student list JSON ───────────────────────────────────────── */
char *db_students_list_json(int class_num, int include_archived, int min_att_pct)
{
    db_lock();
    sqlite3 *db = db_handle();

    /* Total class days (for att%) */
    int total_days = 0;
    {
        sqlite3_stmt *s;
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(DISTINCT entry_date) FROM attendance WHERE class_num=%d",
                 class_num);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) total_days = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    StrBuf sb;
    sb_init(&sb, 2048);
    sb_append(&sb, "[");

    sqlite3_stmt *s;
    /* When include_archived=1 return ALL students (active + archived).
       When include_archived=0 return only active students.
       Also select s.is_active so we can emit the "archived" flag in JSON.
       Use batchtime field name (not aliased) to match frontend expectation.
       NOTE: No GROUP BY / no LEFT JOIN here — SQLITE_OMIT_TEMPDB means SQLite
       cannot allocate a temp B-tree to sort aggregated results, which silently
       returns 0 rows or hangs.  We fetch per-student counts with a separate
       scalar subquery that uses the idx_att_uid_class covering index instead. */
    const char *qsql_all =
        "SELECT card_uid, name, roll, batchtime, is_active"
        " FROM students WHERE class_num=? ORDER BY name";

    const char *qsql_active =
        "SELECT card_uid, name, roll, batchtime, is_active"
        " FROM students WHERE class_num=? AND is_active=1 ORDER BY name";

    const char *qsql = include_archived ? qsql_all : qsql_active;

    /* Prepare count stmt once; reset & rebind per row */
    sqlite3_stmt *cnt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM attendance WHERE card_uid=? AND class_num=?",
        -1, &cnt, NULL);

    if (sqlite3_prepare_v2(db, qsql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, class_num);

        bool first = true;
        while (sqlite3_step(s) == SQLITE_ROW) {
            const char *uid       = (const char *)sqlite3_column_text(s, 0);
            const char *name      = (const char *)sqlite3_column_text(s, 1);
            const char *roll      = (const char *)sqlite3_column_text(s, 2);
            const char *batchtime = (const char *)sqlite3_column_text(s, 3);
            int is_active         = sqlite3_column_int(s, 4);
            bool archived         = (is_active == 0);

            /* Per-student attendance count via covering index — no temp B-tree */
            int sess = 0;
            if (cnt) {
                sqlite3_reset(cnt);
                sqlite3_bind_text(cnt, 1, uid ? uid : "", -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(cnt, 2, class_num);
                if (sqlite3_step(cnt) == SQLITE_ROW)
                    sess = sqlite3_column_int(cnt, 0);
            }

            int att_pct = (total_days > 0) ? (sess * 100 / total_days) : 100;
            bool low_att = (total_days > 0 && att_pct < min_att_pct);

            if (!first) sb_append(&sb, ",");
            first = false;

            sb_append(&sb, "{");
            sb_append(&sb, "\"uid\":"); sb_append_jstr(&sb, uid ? uid : "");
            sb_append(&sb, ",\"name\":"); sb_append_jstr(&sb, name ? name : "");
            sb_append(&sb, ",\"roll\":"); sb_append_jstr(&sb, roll ? roll : "");
            sb_append(&sb, ",\"batchtime\":"); sb_append_jstr(&sb, batchtime ? batchtime : "");
            sb_appendf(&sb, ",\"archived\":%s", archived ? "true" : "false");
            sb_appendf(&sb, ",\"sessions\":%d", sess);
            sb_appendf(&sb, ",\"att_pct\":%d", att_pct);
            sb_appendf(&sb, ",\"low_att\":%s", low_att ? "true" : "false");
            sb_append(&sb, "}");
        }
        sqlite3_finalize(s);
    }
    if (cnt) sqlite3_finalize(cnt);

    sb_append(&sb, "]");
    db_unlock();
    return sb.buf;
}

/* ── Student detail JSON ─────────────────────────────────────── */
char *db_student_detail_json(const char *uid, int class_num)
{
    db_lock();
    sqlite3 *db = db_handle();

    /* Student row */
    char name[128]="", roll[64]="", batch[16]="";
    bool found = false;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT name,roll,batchtime FROM students"
                " WHERE card_uid=? AND class_num=? LIMIT 1",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, uid,       -1, SQLITE_STATIC);
            sqlite3_bind_int(s,  2, class_num);
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *v;
                if ((v = (const char *)sqlite3_column_text(s,0))) strncpy(name,  v, 127);
                if ((v = (const char *)sqlite3_column_text(s,1))) strncpy(roll,  v,  63);
                if ((v = (const char *)sqlite3_column_text(s,2))) strncpy(batch, v,  15);
                found = true;
            }
            sqlite3_finalize(s);
        }
    }
    if (!found) { db_unlock(); return strdup("{\"error\":\"not_found\"}"); }

    /* Total class days */
    int total_days = 0;
    {
        sqlite3_stmt *s;
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(DISTINCT entry_date) FROM attendance WHERE class_num=%d",
                 class_num);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) total_days = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    /* Attendance records */
    StrBuf sb;
    sb_init(&sb, 1024);

    /* Build student object */
    sb_append(&sb, "{\"student\":{");
    sb_append(&sb, "\"uid\":"); sb_append_jstr(&sb, uid);
    sb_append(&sb, ",\"name\":"); sb_append_jstr(&sb, name);
    sb_append(&sb, ",\"roll\":"); sb_append_jstr(&sb, roll);
    sb_append(&sb, ",\"batch\":"); sb_append_jstr(&sb, batch);
    sb_append(&sb, "},\"total_days\":");
    sb_appendf(&sb, "%d", total_days);
    sb_append(&sb, ",\"records\":[");

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT id,entry_date,entry_time,batchtime,status"
            " FROM attendance WHERE card_uid=? AND class_num=?"
            " ORDER BY entry_date DESC,entry_time DESC",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, uid,       -1, SQLITE_STATIC);
        sqlite3_bind_int(s,  2, class_num);

        bool first = true;
        while (sqlite3_step(s) == SQLITE_ROW) {
            int         rid    = sqlite3_column_int(s,  0);
            const char *date   = (const char *)sqlite3_column_text(s, 1);
            const char *ttime  = (const char *)sqlite3_column_text(s, 2);
            const char *rbatch = (const char *)sqlite3_column_text(s, 3);
            const char *status = (const char *)sqlite3_column_text(s, 4);

            if (!first) sb_append(&sb, ",");
            first = false;

            sb_appendf(&sb, "{\"id\":%d", rid);
            sb_append(&sb, ",\"date\":"); sb_append_jstr(&sb, date   ? date   : "");
            sb_append(&sb, ",\"time\":"); sb_append_jstr(&sb, ttime  ? ttime  : "");
            sb_append(&sb, ",\"batch\":"); sb_append_jstr(&sb, rbatch ? rbatch : "");
            sb_append(&sb, ",\"status\":"); sb_append_jstr(&sb, status ? status : "");
            sb_append(&sb, "}");
        }
        sqlite3_finalize(s);
    }

    sb_append(&sb, "]}");
    db_unlock();
    return sb.buf;
}

/* ── Student register ────────────────────────────────────────── */
int db_student_register(int class_num, const char *uid,
                        const char *name, const char *roll,
                        const char *batchtime, const char *extra_json)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO students"
            "(class_num,card_uid,name,roll,batchtime,extra_data)"
            " VALUES(?,?,?,?,?,?)",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s,  1, class_num);
        sqlite3_bind_text(s, 2, uid,        -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, name,       -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, roll,       -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 5, batchtime,  -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 6, extra_json ? extra_json : "{}", -1, SQLITE_STATIC);
        rc = (sqlite3_step(s) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(s);
    }
    db_unlock();
    return rc;
}

/* ── Student edit ────────────────────────────────────────────── */
int db_student_edit(int class_num, const char *uid_orig,
                    const char *uid_new, const char *name,
                    const char *roll, const char *batchtime)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;

    if (sqlite3_prepare_v2(db,
            "UPDATE students SET name=?,roll=?,card_uid=?,batchtime=?"
            " WHERE card_uid=? AND class_num=?",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, name,      -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, roll,      -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, uid_new,   -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, batchtime, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 5, uid_orig,  -1, SQLITE_STATIC);
        sqlite3_bind_int(s,  6, class_num);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    /* If UID changed, update attendance records too */
    if (strcmp(uid_orig, uid_new) != 0) {
        if (sqlite3_prepare_v2(db,
                "UPDATE attendance SET card_uid=?,name=?,roll=?"
                " WHERE card_uid=? AND class_num=?",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, uid_new,  -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 2, name,     -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 3, roll,     -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 4, uid_orig, -1, SQLITE_STATIC);
            sqlite3_bind_int(s,  5, class_num);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }
    db_unlock();
    return 0;
}

/* ── Archive / restore / delete ──────────────────────────────── */
static int set_active(const char *uid, int class_num, int active)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "UPDATE students SET is_active=? WHERE card_uid=? AND class_num=?",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s,  1, active);
        sqlite3_bind_text(s, 2, uid,       -1, SQLITE_STATIC);
        sqlite3_bind_int(s,  3, class_num);
        rc = (sqlite3_step(s) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(s);
    }
    db_unlock();
    return rc;
}

int db_student_archive(const char *uid, int class_num) { return set_active(uid, class_num, 0); }
int db_student_restore(const char *uid, int class_num) { return set_active(uid, class_num, 1); }

int db_student_delete(const char *uid, int class_num)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM students WHERE card_uid=? AND class_num=?",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, uid,       -1, SQLITE_STATIC);
        sqlite3_bind_int(s,  2, class_num);
        rc = (sqlite3_step(s) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(s);
    }
    db_unlock();
    return rc;
}

/* ── Student list — chunked streaming (DMA-safe) ─────────────
 *
 * Instead of building one giant heap string (which forces lwIP to allocate
 * a matching DMA-capable DRAM buffer that doesn't exist when >~75 students
 * are enrolled), we iterate SQLite row-by-row and emit each student as a
 * small chunk (~200-300 bytes).  The caller's callback typically wraps
 * httpd_resp_send_chunk(), so each DMA allocation stays tiny.
 * ──────────────────────────────────────────────────────────── */
esp_err_t db_students_stream_json(int class_num, int include_archived, int min_att_pct,
                                   db_student_stream_cb_t cb, void *ctx)
{
    db_lock();
    sqlite3 *db = db_handle();
    esp_err_t ret = ESP_OK;

    /* ── Total class days (for attendance %) ── */
    int total_days = 0;
    {
        sqlite3_stmt *s;
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(DISTINCT entry_date) FROM attendance WHERE class_num=%d",
                 class_num);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) total_days = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    /* ── Open the JSON array ── */
    STREAM_CB("[", 1);
    if (ret != ESP_OK) { db_unlock(); return ret; }

    /* ── Query — no GROUP BY, no LEFT JOIN, no temp B-tree ──────────
     * SQLITE_OMIT_TEMPDB means SQLite cannot allocate a temp sort buffer,
     * so any query that uses GROUP BY + ORDER BY together hangs or silently
     * returns nothing.  Instead we do a plain scan ordered by name on the
     * students table, then fire a tiny per-student COUNT using the composite
     * idx_att_uid_class covering index (O(log n), no sort needed).        */
    const char *qsql_all =
        "SELECT card_uid, name, roll, batchtime, is_active"
        " FROM students WHERE class_num=? ORDER BY name";

    const char *qsql_active =
        "SELECT card_uid, name, roll, batchtime, is_active"
        " FROM students WHERE class_num=? AND is_active=1 ORDER BY name";

    const char *qsql = include_archived ? qsql_all : qsql_active;

    /* Prepare the per-student count stmt once; reset & rebind each iteration */
    sqlite3_stmt *cnt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM attendance WHERE card_uid=? AND class_num=?",
        -1, &cnt, NULL);

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, qsql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, class_num);

        bool first = true;
        /* 512 bytes: enough for one JSON object even with long names/UIDs,
           and safe on the 20 KB HTTP task stack. */
        char chunk[512];
        char esc_uid[64], esc_name[160], esc_roll[48], esc_batch[32];

        while (sqlite3_step(s) == SQLITE_ROW) {
            const char *uid       = (const char *)sqlite3_column_text(s, 0);
            const char *name      = (const char *)sqlite3_column_text(s, 1);
            const char *roll      = (const char *)sqlite3_column_text(s, 2);
            const char *batchtime = (const char *)sqlite3_column_text(s, 3);
            int is_active         = sqlite3_column_int(s, 4);
            bool archived         = (is_active == 0);

            /* Per-student attendance count via covering index — no temp B-tree */
            int sess = 0;
            if (cnt) {
                sqlite3_reset(cnt);
                sqlite3_bind_text(cnt, 1, uid ? uid : "", -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(cnt, 2, class_num);
                if (sqlite3_step(cnt) == SQLITE_ROW)
                    sess = sqlite3_column_int(cnt, 0);
            }

            int att_pct  = (total_days > 0) ? (sess * 100 / total_days) : 100;
            bool low_att = (total_days > 0 && att_pct < min_att_pct);

            /* JSON-escape each field into its own small fixed buffer */
            json_escape(uid       ? uid       : "", esc_uid,   sizeof(esc_uid));
            json_escape(name      ? name      : "", esc_name,  sizeof(esc_name));
            json_escape(roll      ? roll      : "", esc_roll,  sizeof(esc_roll));
            json_escape(batchtime ? batchtime : "", esc_batch, sizeof(esc_batch));

            int n = snprintf(chunk, sizeof(chunk),
                "%s{\"uid\":\"%s\",\"name\":\"%s\",\"roll\":\"%s\","
                "\"batchtime\":\"%s\",\"archived\":%s,\"sessions\":%d,"
                "\"att_pct\":%d,\"low_att\":%s}",
                first ? "" : ",",
                esc_uid, esc_name, esc_roll, esc_batch,
                archived ? "true" : "false",
                sess, att_pct,
                low_att ? "true" : "false");

            if (n > 0 && n < (int)sizeof(chunk)) {
                STREAM_CB(chunk, n);
                if (ret != ESP_OK) break;
                first = false;   /* only advance past comma after successful send */
            }
        }
        sqlite3_finalize(s);
    }
    if (cnt) sqlite3_finalize(cnt);

    /* ── Close the JSON array ── */
    if (ret == ESP_OK) {
        STREAM_CB("]", 1);
    }

    db_unlock();
    return ret;
}
