#include "db.h"
#include "sqlite3.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static const char *TAG = "db_att";

/*
 * FIX: The DB mutex must NOT be held during cb() calls (httpd_resp_send_chunk).
 * Holding the lock while doing network I/O blocks every other DB operation
 * (classes API, RFID saves, status checks) for the entire response duration —
 * up to 30 s on slow WiFi — causing ECONNRESET (error 104) on other requests.
 *
 * STREAM_CB: release the lock, send the chunk, reacquire the lock.
 * SQLite WAL mode + SQLITE_THREADSAFE=1 make it safe to briefly release the
 * external mutex between sqlite3_step() calls on the same prepared statement.
 */
#define STREAM_CB(buf, len) do {            \
    db_unlock();                            \
    ret = cb((buf), (size_t)(len), ctx);   \
    db_lock();                             \
} while (0)

extern void     db_lock(void);
extern void     db_unlock(void);
extern sqlite3 *db_handle(void);
extern bool     db_exec_raw(const char *sql);

/* Reuse the StrBuf from db_students — declared locally here too */
typedef struct { char *buf; size_t len, cap; } StrBuf;
static bool sb_init(StrBuf *sb, size_t n)
    { sb->buf=malloc(n); if(!sb->buf)return false; sb->buf[0]='\0'; sb->len=0; sb->cap=n; return true; }
static bool sb_append(StrBuf *sb, const char *s)
    { size_t sl=strlen(s); if(sb->len+sl+1>sb->cap){size_t nc=(sb->len+sl+1)*2;char*t=realloc(sb->buf,nc);if(!t)return false;sb->buf=t;sb->cap=nc;} memcpy(sb->buf+sb->len,s,sl+1); sb->len+=sl; return true; }
static bool sb_appendf(StrBuf *sb, const char *fmt, ...)
    /* BUG FIX (Bug 5): The original 512-byte tmp buffer could silently truncate
     * a student record with a heavily-escaped long name (~460 bytes).  Increased
     * to 1024 to give a comfortable margin for the longest realistic record. */
    { char tmp[1024]; va_list ap; va_start(ap,fmt); vsnprintf(tmp,sizeof(tmp),fmt,ap); va_end(ap); return sb_append(sb,tmp); }
static bool sb_append_jstr(StrBuf *sb, const char *v)
    { sb_append(sb,"\""); if(v){char e[256];size_t i=0;while(*v&&i+2<sizeof(e)){unsigned char c=(unsigned char)*v++;if(c=='"'||c=='\\'){e[i++]='\\';e[i++]=c;}else if(c=='\n'){e[i++]='\\';e[i++]='n';}else e[i++]=c;} e[i]='\0';sb_append(sb,e);} return sb_append(sb,"\""); }

/* ── Stack-safe JSON string escape (no heap) ─────────────────── */
static void att_json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0;
    if (!src) { if (dst_len) dst[0] = '\0'; return; }
    while (*src && i + 3 < dst_len) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') { dst[i++] = '\\'; dst[i++] = c; }
        else if (c == '\n')        { dst[i++] = '\\'; dst[i++] = 'n'; }
        else if (c == '\r')        { dst[i++] = '\\'; dst[i++] = 'r'; }
        else                       { dst[i++] = c; }
    }
    dst[i] = '\0';
}

/* ── Attendance page — chunked streaming (DMA-safe) ──────────────
 *
 * Replaces db_attendance_json() which built a 300 KB+ heap buffer.
 * Each cb() call emits ≤ ~350 bytes so httpd_resp_send_chunk() only
 * needs tiny DMA-capable DRAM allocations (one student / one record
 * at a time).
 * ─────────────────────────────────────────────────────────────── */
/* FIX-33: The original implementation called cb() (TCP send) while holding
 * db_lock(), which could stall for up to 30 s on a slow client, freezing
 * RFID scanning and all other DB access.  Fixed by collecting the full JSON
 * into a StrBuf under a short lock, then releasing the lock before sending. */
/* db_attendance_stream_json — two-phase streaming with JOIN-based today query
 *
 * PERF FIX: The previous implementation used an N+1 query pattern for today's
 * data — one prepared-statement lookup per student (500 lookups for 500 students)
 * causing 1-2 s of pure SQLite CPU before any byte reached the browser.
 *
 * New approach:
 *   Phase A — short DB lock:
 *     3 COUNT queries (stats) + 1 LEFT JOIN (all students × today's attendance)
 *     → collect into sb_today, unlock, send immediately.
 *     Browser renders stats + today tab with no wait.
 *   Phase B — second short DB lock:
 *     Log queries (grouped by date) → collect into sb_log, unlock, send.
 *     Log tab streams in while user is already viewing today's data.
 *
 * Neither lock is held during TCP I/O (cb calls), preserving FIX-33's
 * guarantee that a slow client never blocks RFID scanning or other DB access.
 */
/* ── Attendance page — TRUE per-row streaming (DMA-safe) ────────
 * Now emits one small chunk per student (exactly like db_students_stream_json).
 * No large StrBuf is built in memory. Lock is held only while reading SQLite. */
esp_err_t db_attendance_stream_json(int class_num, db_stream_cb_t cb, void *ctx)
{
    /* FIX-BUG1: Signal that a streaming response is in progress.  The watchdog
     * in main.c checks db_is_streaming() and defers db_sd_remount() until the
     * response completes, preventing sqlite3_close_v2() from being called while
     * our prepared statements below are live (STREAM_CB releases db_lock()
     * between steps, opening a window for the watchdog to fire).             */
    db_streaming_begin();
    db_lock();
    sqlite3 *db = db_handle();
    esp_err_t ret = ESP_OK;

    char today[12];
    db_today_string(today, sizeof(today));

    /* ── Stats (fast indexed counts) ── */
    int total = 0, present_td = 0, total_rec = 0;
    {
        sqlite3_stmt *s;
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM students WHERE class_num=%d AND is_active=1",
                 class_num);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) total = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM attendance WHERE class_num=%d AND entry_date='%s' AND status='present'",
                 class_num, today);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) present_td = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM attendance WHERE class_num=%d", class_num);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) total_rec = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    int absent_td = (present_td > total) ? 0 : (total - present_td);

    /* ── Open JSON + send header immediately ── */
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "{\"today_date\":\"%s\","
        "\"stats\":{\"total\":%d,\"present\":%d,\"absent\":%d,\"records\":%d},"
        "\"today\":[",
        today, total, present_td, absent_td, total_rec);

    STREAM_CB(hdr, n);
    if (ret != ESP_OK) { db_unlock(); db_streaming_end(); return ret; }

    /* ── Today tab: one student per chunk (true streaming) ── */
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT s.card_uid, s.name, s.roll, s.batchtime,"
                "       a.entry_time, a.status"
                " FROM students s"
                " LEFT JOIN attendance a"
                "   ON  a.card_uid = s.card_uid"
                "   AND a.class_num = s.class_num"
                "   AND a.entry_date = ?"
                " WHERE s.class_num = ? AND s.is_active = 1"
                " ORDER BY s.name",
                -1, &s, NULL) == SQLITE_OK) {

            sqlite3_bind_text(s, 1, today,     -1, SQLITE_STATIC);
            sqlite3_bind_int (s, 2, class_num);

            bool first = true;
            char chunk[512];
            char esc_uid[64], esc_name[160], esc_roll[64];
            char esc_batch[48], esc_time[32];

            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *uid   = (const char *)sqlite3_column_text(s, 0);
                const char *name  = (const char *)sqlite3_column_text(s, 1);
                const char *roll  = (const char *)sqlite3_column_text(s, 2);
                const char *batch = (const char *)sqlite3_column_text(s, 3);
                const char *etime = (const char *)sqlite3_column_text(s, 4);
                const char *st    = (const char *)sqlite3_column_text(s, 5);
                bool present = (st && strcmp(st, "present") == 0);

                att_json_escape(uid   ? uid   : "", esc_uid,   sizeof(esc_uid));
                att_json_escape(name  ? name  : "", esc_name,  sizeof(esc_name));
                att_json_escape(roll  ? roll  : "", esc_roll,  sizeof(esc_roll));
                att_json_escape(batch ? batch : "", esc_batch, sizeof(esc_batch));
                att_json_escape(etime ? etime : "", esc_time,  sizeof(esc_time));

                int len = snprintf(chunk, sizeof(chunk),
                    "%s{\"uid\":\"%s\",\"name\":\"%s\",\"roll\":\"%s\","
                    "\"batch\":\"%s\",\"present\":%s,\"time\":\"%s\"}",
                    first ? "" : ",",
                    esc_uid, esc_name, esc_roll, esc_batch,
                    present ? "true" : "false", esc_time);

                if (len > 0 && len < (int)sizeof(chunk)) {
                    STREAM_CB(chunk, len);
                    if (ret != ESP_OK) break;
                    first = false;
                }
            }
            sqlite3_finalize(s);
        }
    }

    /* Close today array and open log array */
    if (ret == ESP_OK) { STREAM_CB("],\"log\":[", 9); }

    /* ── Log tab (grouped by date) — also streamed per date group ── */
    if (ret == ESP_OK) {
        sqlite3_stmt *ds;
        if (sqlite3_prepare_v2(db,
                "SELECT DISTINCT entry_date FROM attendance"
                " WHERE class_num=? ORDER BY entry_date DESC",
                -1, &ds, NULL) == SQLITE_OK) {

            sqlite3_bind_int(ds, 1, class_num);
            bool first_date = true;

            while (sqlite3_step(ds) == SQLITE_ROW) {
                const char *date = (const char *)sqlite3_column_text(ds, 0);
                if (!date) continue;

                char esc_date[24];
                att_json_escape(date, esc_date, sizeof(esc_date));

                /* Count present/absent for this date */
                int p_cnt = 0, a_cnt = 0;
                {
                    sqlite3_stmt *cnt;
                    if (sqlite3_prepare_v2(db,
                            "SELECT COUNT(*) FROM attendance"
                            " WHERE class_num=? AND entry_date=? AND status='present'",
                            -1, &cnt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int(cnt, 1, class_num);
                        sqlite3_bind_text(cnt, 2, date, -1, SQLITE_STATIC);
                        if (sqlite3_step(cnt) == SQLITE_ROW) p_cnt = sqlite3_column_int(cnt, 0);
                        sqlite3_finalize(cnt);
                    }
                    /* Use total active students from earlier */
                    a_cnt = (p_cnt > total) ? 0 : (total - p_cnt);
                }

                char date_hdr[128];
                int dh_len = snprintf(date_hdr, sizeof(date_hdr),
                    "%s{\"date\":\"%s\",\"present_count\":%d,\"absent_count\":%d,\"records\":[",
                    first_date ? "" : ",", esc_date, p_cnt, a_cnt);

                STREAM_CB(date_hdr, dh_len);
                if (ret != ESP_OK) break;
                first_date = false;

                /* Records for this date */
                sqlite3_stmt *rs;
                if (sqlite3_prepare_v2(db,
                        "SELECT id,card_uid,name,roll,batchtime,entry_time,status"
                        " FROM attendance WHERE class_num=? AND entry_date=?"
                        " ORDER BY entry_time DESC",
                        -1, &rs, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(rs, 1, class_num);
                    sqlite3_bind_text(rs, 2, date, -1, SQLITE_STATIC);

                    bool first_rec = true;
                    char rec_chunk[400];
                    char esc_uid[64], esc_name[160], esc_roll[64];
                    char esc_batch[48], esc_time[32], esc_st[16];

                    while (sqlite3_step(rs) == SQLITE_ROW) {
                        int rid = sqlite3_column_int(rs, 0);
                        const char *ruid   = (const char *)sqlite3_column_text(rs, 1);
                        const char *rname  = (const char *)sqlite3_column_text(rs, 2);
                        const char *rroll  = (const char *)sqlite3_column_text(rs, 3);
                        const char *rbatch = (const char *)sqlite3_column_text(rs, 4);
                        const char *rtime  = (const char *)sqlite3_column_text(rs, 5);
                        const char *rst    = (const char *)sqlite3_column_text(rs, 6);

                        att_json_escape(ruid   ? ruid   : "", esc_uid,   sizeof(esc_uid));
                        att_json_escape(rname  ? rname  : "", esc_name,  sizeof(esc_name));
                        att_json_escape(rroll  ? rroll  : "", esc_roll,  sizeof(esc_roll));
                        att_json_escape(rbatch ? rbatch : "", esc_batch, sizeof(esc_batch));
                        att_json_escape(rtime  ? rtime  : "", esc_time,  sizeof(esc_time));
                        att_json_escape(rst    ? rst    : "present", esc_st, sizeof(esc_st));

                        int len = snprintf(rec_chunk, sizeof(rec_chunk),
                            "%s{\"id\":%d,\"uid\":\"%s\",\"name\":\"%s\","
                            "\"roll\":\"%s\",\"batch\":\"%s\","
                            "\"time\":\"%s\",\"status\":\"%s\"}",
                            first_rec ? "" : ",",
                            rid, esc_uid, esc_name, esc_roll, esc_batch,
                            esc_time, esc_st);

                        if (len > 0 && len < (int)sizeof(rec_chunk)) {
                            STREAM_CB(rec_chunk, len);
                            if (ret != ESP_OK) break;
                            first_rec = false;
                        }
                    }
                    sqlite3_finalize(rs);
                }

                STREAM_CB("]}", 2);   /* close this date group */
                if (ret != ESP_OK) break;
            }
            sqlite3_finalize(ds);
        }
    }

    /* Close the entire log array and the JSON object */
    if (ret == ESP_OK) { STREAM_CB("]}", 2); }

    db_unlock();
    db_streaming_end();   /* FIX-BUG1: clear streaming guard */
    return ret;
}

/* ── Monthly Report — TRUE per-row streaming (DMA-safe) ─────────
 * Now emits one small JSON row per student instead of building
 * one huge string. Matches the style of db_students_stream_json. */
esp_err_t db_report_stream_json(int class_num, const char *month_prefix,
                                int min_att_pct, db_stream_cb_t cb, void *ctx)
{
    db_streaming_begin();   /* FIX-BUG1 */
    db_lock();
    sqlite3 *db = db_handle();
    esp_err_t ret = ESP_OK;

    /* Count class days in the month */
    int class_days = 0;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(DISTINCT entry_date) FROM attendance"
                " WHERE class_num=? AND entry_date LIKE ?",
                -1, &s, NULL) == SQLITE_OK) {
            char pat[16];
            snprintf(pat, sizeof(pat), "%s%%", month_prefix);
            sqlite3_bind_int(s,  1, class_num);
            sqlite3_bind_text(s, 2, pat, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_ROW)
                class_days = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    /* Send header immediately */
    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
                      "{\"class_days\":%d,\"min_pct\":%d,\"rows\":[",
                      class_days, min_att_pct);
    STREAM_CB(hdr, hn);
    if (ret != ESP_OK) { db_unlock(); db_streaming_end(); return ret; }

    /* Stream each student row one by one */
    if (class_days > 0) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT s.card_uid, s.name, s.roll, s.batchtime,"
                "       COUNT(CASE WHEN a.status='present' THEN 1 END) AS pres"
                " FROM students s"
                " LEFT JOIN attendance a"
                "   ON s.card_uid = a.card_uid"
                "   AND a.class_num = s.class_num"
                "   AND a.entry_date LIKE ?"
                " WHERE s.class_num = ? AND s.is_active = 1"
                " GROUP BY s.card_uid"
                " ORDER BY pres DESC, s.name",
                -1, &s, NULL) == SQLITE_OK) {

            char pat[16];
            snprintf(pat, sizeof(pat), "%s%%", month_prefix);
            sqlite3_bind_text(s, 1, pat,       -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (s, 2, class_num);

            bool first = true;
            char chunk[400];
            char esc_uid[64], esc_name[160], esc_roll[64], esc_batch[48];

            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *uid   = (const char *)sqlite3_column_text(s, 0);
                const char *name  = (const char *)sqlite3_column_text(s, 1);
                const char *roll  = (const char *)sqlite3_column_text(s, 2);
                const char *batch = (const char *)sqlite3_column_text(s, 3);
                int pres = sqlite3_column_int(s, 4);
                int pct  = class_days > 0 ? (pres * 100 / class_days) : 0;

                att_json_escape(uid   ? uid   : "", esc_uid,   sizeof(esc_uid));
                att_json_escape(name  ? name  : "", esc_name,  sizeof(esc_name));
                att_json_escape(roll  ? roll  : "", esc_roll,  sizeof(esc_roll));
                att_json_escape(batch ? batch : "", esc_batch, sizeof(esc_batch));

                int n = snprintf(chunk, sizeof(chunk),
                    "%s{\"uid\":\"%s\",\"name\":\"%s\",\"roll\":\"%s\","
                    "\"batch\":\"%s\",\"present\":%d,\"pct\":%d,\"low\":%s}",
                    first ? "" : ",",
                    esc_uid, esc_name, esc_roll, esc_batch,
                    pres, pct, (pct < min_att_pct) ? "true" : "false");

                if (n > 0 && n < (int)sizeof(chunk)) {
                    STREAM_CB(chunk, n);
                    if (ret != ESP_OK) break;
                    first = false;
                }
            }
            sqlite3_finalize(s);
        }
    }

    /* Close the JSON */
    if (ret == ESP_OK) { STREAM_CB("]}", 2); }

    db_unlock();
    db_streaming_end();   /* FIX-BUG1 */
    return ret;
}

/* ── RFID scan → mark present ────────────────────────────────── */
/* Returns: 1=newly marked, 0=already marked today, -1=unknown card */
int db_save_attendance(const char *uid, char *out_name, size_t name_len,
                       char *out_time, size_t time_len)
{
    db_lock();
    sqlite3 *db = db_handle();

    /* Look up student */
    char name[128]="", roll[64]="", batch[16]="";
    int  class_num = 0;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT name,roll,batchtime,class_num FROM students"
                " WHERE card_uid=? AND is_active=1 LIMIT 1",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, uid, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *v;
                if ((v=(const char*)sqlite3_column_text(s,0))) strncpy(name,  v,127);
                if ((v=(const char*)sqlite3_column_text(s,1))) strncpy(roll,  v, 63);
                if ((v=(const char*)sqlite3_column_text(s,2))) strncpy(batch, v, 15);
                class_num = sqlite3_column_int(s, 3);
            }
            sqlite3_finalize(s);
        }
    }

    if (!class_num) {
        ESP_LOGW(TAG, "Unknown card: %s", uid);
        db_unlock();
        return -1;
    }

    char date_buf[12], time_buf[10];
    db_today_string(date_buf,  sizeof(date_buf));
    db_now_time_string(time_buf, sizeof(time_buf));

    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db,
            /*
             * BUG FIX: Was INSERT OR REPLACE — that deleted the existing row
             * and inserted a fresh one, so every subsequent scan of the same
             * card on the same day updated entry_time (the last scan always
             * won).  INSERT OR IGNORE preserves the first scan and silently
             * skips all duplicates, which is the correct behaviour.
             */
            "INSERT OR IGNORE INTO attendance"
            "(class_num,card_uid,name,roll,batchtime,entry_date,entry_time,status)"
            " VALUES(?,?,?,?,?,?,?,'present')",
            -1, &ins, NULL) == SQLITE_OK) {
        sqlite3_bind_int(ins,  1, class_num);
        sqlite3_bind_text(ins, 2, uid,       -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, name,      -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, roll,      -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 5, batch,     -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 6, date_buf,  -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 7, time_buf,  -1, SQLITE_STATIC);
        sqlite3_step(ins);
        int changes = sqlite3_changes(db);
        sqlite3_finalize(ins);
        ESP_LOGI(TAG, "%s: %s %s — %s",
                 changes > 0 ? "Marked present" : "Already marked", name, batch, time_buf);
        if (out_name) strncpy(out_name, name,     name_len - 1);
        if (out_time) strncpy(out_time, time_buf,  time_len - 1);
        db_unlock();
        return changes > 0 ? 1 : 0;
    }
    db_unlock();
    return 0;
}

/* ── Manual mark present ─────────────────────────────────────── */
int db_attendance_mark(int class_num, const char *uid,
                       const char *date, const char *time_str)
{
    db_lock();
    sqlite3 *db = db_handle();

    char name[128]="", roll[64]="", batch[16]="";
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
                if ((v=(const char*)sqlite3_column_text(s,0))) strncpy(name,  v,127);
                if ((v=(const char*)sqlite3_column_text(s,1))) strncpy(roll,  v, 63);
                if ((v=(const char*)sqlite3_column_text(s,2))) strncpy(batch, v, 15);
            }
            sqlite3_finalize(s);
        }
    }

    sqlite3_stmt *ins;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO attendance"
            "(class_num,card_uid,name,roll,batchtime,entry_date,entry_time,status)"
            " VALUES(?,?,?,?,?,?,?,'present')",
            -1, &ins, NULL) == SQLITE_OK) {
        sqlite3_bind_int(ins,  1, class_num);
        sqlite3_bind_text(ins, 2, uid,       -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, name,      -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, roll,      -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 5, batch,     -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 6, date,      -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 7, time_str,  -1, SQLITE_STATIC);
        rc = (sqlite3_step(ins) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(ins);
    }
    db_unlock();
    return rc;
}

/* ── Delete by id ────────────────────────────────────────────── */
int db_attendance_delete_by_id(int id, int class_num)
{
    db_lock();
    sqlite3 *db = db_handle();
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM attendance WHERE id=? AND class_num=?",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, id);
        sqlite3_bind_int(s, 2, class_num);
        rc = (sqlite3_step(s) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(s);
    }
    db_unlock();
    return rc;
}

/* ── Attendance page JSON ─────────────────────────────────────── */
/*  {
      "today_date": "2025-01-15",
      "stats": {"total":30,"present":18,"absent":12,"records":420},
      "today": [{"uid","name","roll","batch","present":bool,"time":""},...],
      "log":   [{"date","present_count","absent_count","records":[...]}]
    }                                                                      */
char *db_attendance_json(int class_num)
{
    db_lock();
    sqlite3 *db = db_handle();

    char today[12];
    db_today_string(today, sizeof(today));

    /* Stats */
    int total=0, present_td=0, total_rec=0;
    {
        sqlite3_stmt *s;
        char sql[128];
        snprintf(sql,sizeof(sql),
                 "SELECT COUNT(*) FROM students WHERE class_num=%d AND is_active=1",
                 class_num);
        if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK){
            if(sqlite3_step(s)==SQLITE_ROW) total=sqlite3_column_int(s,0);
            sqlite3_finalize(s);
        }
        snprintf(sql,sizeof(sql),
                 "SELECT COUNT(*) FROM attendance"
                 " WHERE class_num=%d AND entry_date='%s' AND status='present'",
                 class_num, today);
        if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK){
            if(sqlite3_step(s)==SQLITE_ROW) present_td=sqlite3_column_int(s,0);
            sqlite3_finalize(s);
        }
        snprintf(sql,sizeof(sql),
                 "SELECT COUNT(*) FROM attendance WHERE class_num=%d", class_num);
        if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK){
            if(sqlite3_step(s)==SQLITE_ROW) total_rec=sqlite3_column_int(s,0);
            sqlite3_finalize(s);
        }
    }

    StrBuf sb;
    sb_init(&sb, 4096);

    sb_appendf(&sb, "{\"today_date\":\"%s\"", today);
    sb_appendf(&sb, ",\"stats\":{\"total\":%d,\"present\":%d,\"absent\":%d,\"records\":%d}",
               total, present_td, (present_td > total ? 0 : total - present_td), total_rec);

    /* Today tab: all students with present/absent status */
    sb_append(&sb, ",\"today\":[");
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT s.card_uid,s.name,s.roll,s.batchtime,"
                "       a.entry_time, a.status"
                " FROM students s"
                " LEFT JOIN attendance a"
                "   ON s.card_uid=a.card_uid AND a.class_num=s.class_num AND a.entry_date=?"
                " WHERE s.class_num=? AND s.is_active=1"
                " ORDER BY s.name",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, today,     -1, SQLITE_STATIC);
            sqlite3_bind_int(s,  2, class_num);

            bool first = true;
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *uid    = (const char *)sqlite3_column_text(s,0);
                const char *name   = (const char *)sqlite3_column_text(s,1);
                const char *roll   = (const char *)sqlite3_column_text(s,2);
                const char *batch  = (const char *)sqlite3_column_text(s,3);
                const char *etime  = (const char *)sqlite3_column_text(s,4);
                const char *status = (const char *)sqlite3_column_text(s,5);
                bool present = (status && strcmp(status,"present")==0);

                if (!first) sb_append(&sb, ",");
                first = false;
                sb_append(&sb, "{");
                sb_append(&sb, "\"uid\":"); sb_append_jstr(&sb, uid   ?uid  :"");
                sb_append(&sb, ",\"name\":"); sb_append_jstr(&sb, name ?name :"");
                sb_append(&sb, ",\"roll\":"); sb_append_jstr(&sb, roll ?roll :"");
                sb_append(&sb, ",\"batch\":"); sb_append_jstr(&sb, batch?batch:"");
                sb_appendf(&sb, ",\"present\":%s", present?"true":"false");
                sb_append(&sb, ",\"time\":"); sb_append_jstr(&sb, etime ?etime:"");
                sb_append(&sb, "}");
            }
            sqlite3_finalize(s);
        }
    }
    sb_append(&sb, "]");

    /* Log tab: grouped by date, descending */
    sb_append(&sb, ",\"log\":[");
    {
        /* Get distinct dates */
        sqlite3_stmt *ds;
        if (sqlite3_prepare_v2(db,
                "SELECT DISTINCT entry_date FROM attendance"
                " WHERE class_num=? ORDER BY entry_date DESC",
                -1, &ds, NULL) == SQLITE_OK) {
            sqlite3_bind_int(ds, 1, class_num);

            bool first_date = true;
            while (sqlite3_step(ds) == SQLITE_ROW) {
                const char *date = (const char *)sqlite3_column_text(ds, 0);
                if (!date) continue;

                if (!first_date) sb_append(&sb, ",");
                first_date = false;

                sb_append(&sb, "{");
                sb_append(&sb, "\"date\":"); sb_append_jstr(&sb, date);

                /* Records for this date */
                sqlite3_stmt *rs;
                if (sqlite3_prepare_v2(db,
                        "SELECT id,card_uid,name,roll,batchtime,entry_time,status"
                        " FROM attendance WHERE class_num=? AND entry_date=?"
                        " ORDER BY entry_time DESC",
                        -1, &rs, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(rs,  1, class_num);
                    sqlite3_bind_text(rs, 2, date, -1, SQLITE_STATIC);

                    int p_cnt=0, a_cnt=0;
                    StrBuf recs;
                    sb_init(&recs, 512);
                    sb_append(&recs, "[");
                    bool first_rec = true;

                    while (sqlite3_step(rs) == SQLITE_ROW) {
                        int         rid    = sqlite3_column_int(rs, 0);
                        const char *ruid   = (const char*)sqlite3_column_text(rs,1);
                        const char *rname  = (const char*)sqlite3_column_text(rs,2);
                        const char *rroll  = (const char*)sqlite3_column_text(rs,3);
                        const char *rbatch = (const char*)sqlite3_column_text(rs,4);
                        const char *rtime  = (const char*)sqlite3_column_text(rs,5);
                        const char *rst    = (const char*)sqlite3_column_text(rs,6);
                        bool rp = (rst && strcmp(rst,"present")==0);
                        if (rp) p_cnt++; else a_cnt++;

                        if (!first_rec) sb_append(&recs, ",");
                        first_rec = false;
                        sb_appendf(&recs, "{\"id\":%d", rid);
                        sb_append(&recs, ",\"uid\":"); sb_append_jstr(&recs, ruid  ?ruid  :"");
                        sb_append(&recs, ",\"name\":"); sb_append_jstr(&recs, rname ?rname :"");
                        sb_append(&recs, ",\"roll\":"); sb_append_jstr(&recs, rroll ?rroll :"");
                        sb_append(&recs, ",\"batch\":"); sb_append_jstr(&recs, rbatch?rbatch:"");
                        sb_append(&recs, ",\"time\":"); sb_append_jstr(&recs, rtime ?rtime :"");
                        sb_append(&recs, ",\"status\":"); sb_append_jstr(&recs, rst  ?rst   :"");
                        sb_append(&recs, "}");
                    }
                    sb_append(&recs, "]");
                    sqlite3_finalize(rs);

                    sb_appendf(&sb, ",\"present_count\":%d,\"absent_count\":%d",
                               p_cnt, a_cnt);
                    sb_append(&sb, ",\"records\":");
                    sb_append(&sb, recs.buf);
                    free(recs.buf);
                }
                sb_append(&sb, "}");
            }
            sqlite3_finalize(ds);
        }
    }
    sb_append(&sb, "]}");

    db_unlock();
    return sb.buf;
}

/* ── Monthly report JSON ─────────────────────────────────────── */
char *db_report_json(int class_num, const char *month_prefix, int min_att_pct)
{
    db_lock();
    sqlite3 *db = db_handle();

    /* Count distinct class days in month */
    int class_days = 0;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(DISTINCT entry_date) FROM attendance"
                " WHERE class_num=? AND entry_date LIKE ?",
                -1, &s, NULL) == SQLITE_OK) {
            char pat[16]; snprintf(pat, sizeof(pat), "%s%%", month_prefix);
            sqlite3_bind_int(s,  1, class_num);
            sqlite3_bind_text(s, 2, pat, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_ROW) class_days = sqlite3_column_int(s,0);
            sqlite3_finalize(s);
        }
    }

    StrBuf sb;
    sb_init(&sb, 2048);
    sb_appendf(&sb, "{\"class_days\":%d,\"min_pct\":%d,\"rows\":[",
               class_days, min_att_pct);

    if (class_days > 0) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT s.card_uid,s.name,s.roll,s.batchtime,"
                "       COUNT(CASE WHEN a.status='present' THEN 1 END) AS pres"
                " FROM students s"
                " LEFT JOIN attendance a"
                "   ON s.card_uid=a.card_uid AND a.class_num=s.class_num AND a.entry_date LIKE ?"
                " WHERE s.class_num=? AND s.is_active=1"
                " GROUP BY s.card_uid ORDER BY pres DESC, s.name",
                -1, &s, NULL) == SQLITE_OK) {
            char pat[16]; snprintf(pat, sizeof(pat), "%s%%", month_prefix);
            sqlite3_bind_text(s, 1, pat,       -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s,  2, class_num);

            bool first = true;
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *uid   = (const char*)sqlite3_column_text(s,0);
                const char *name  = (const char*)sqlite3_column_text(s,1);
                const char *roll  = (const char*)sqlite3_column_text(s,2);
                const char *batch = (const char*)sqlite3_column_text(s,3);
                int pres = sqlite3_column_int(s,4);
                int pct  = class_days > 0 ? pres*100/class_days : 0;

                if (!first) sb_append(&sb, ",");
                first = false;
                sb_append(&sb, "{");
                sb_append(&sb, "\"uid\":"); sb_append_jstr(&sb, uid  ?uid  :"");
                sb_append(&sb, ",\"name\":"); sb_append_jstr(&sb, name ?name :"");
                sb_append(&sb, ",\"roll\":"); sb_append_jstr(&sb, roll ?roll :"");
                sb_append(&sb, ",\"batch\":"); sb_append_jstr(&sb, batch?batch:"");
                sb_appendf(&sb, ",\"present\":%d,\"pct\":%d,\"low\":%s",
                           pres, pct, pct < min_att_pct ? "true":"false");
                sb_append(&sb, "}");
            }
            sqlite3_finalize(s);
        }
    }

    sb_append(&sb, "]}");
    db_unlock();
    return sb.buf;
}

/* ── Admin resets ────────────────────────────────────────────── */
int db_reset_class(int class_num)
{
    db_lock();
    char sql[120];
    snprintf(sql,sizeof(sql),"DELETE FROM students   WHERE class_num=%d",class_num);
    db_exec_raw(sql);
    snprintf(sql,sizeof(sql),"DELETE FROM attendance WHERE class_num=%d",class_num);
    db_exec_raw(sql);
    db_unlock();
    return 0;
}

int db_reset_all(void)
{
    db_exec("DELETE FROM students");
    db_exec("DELETE FROM attendance");
    return 0;
}

int db_factory_reset(const char *classes_csv, const char *batches_csv)
{
    db_lock();
    db_exec_raw("BEGIN;");
    db_exec_raw("DELETE FROM attendance");
    db_exec_raw("DELETE FROM students");
    db_exec_raw("DELETE FROM batches");
    db_exec_raw("DELETE FROM classes");
    db_exec_raw("DELETE FROM schema_cols");
    db_exec_raw("COMMIT;");
    db_unlock();

    /* Re-seed classes */
    /* Parse classes_csv e.g. "6,7,8,9,10"
     *
     * FIX: The original code used strtok() for both the outer (class) and
     * inner (batch) loops.  strtok() keeps global internal state, so the
     * inner strtok(bbuf, ",") call corrupted the outer loop's position,
     * causing all class tokens after the first to be silently skipped.
     * Replace both with strtok_r() (re-entrant / thread-safe variant).
     */
    char buf[64];
    strncpy(buf, classes_csv ? classes_csv : "6,7,8,9,10", sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    char *outer_save = NULL;
    char *tok = strtok_r(buf, ",", &outer_save);
    while (tok) {
        int c = atoi(tok);
        if (c >= 1 && c <= 12) {
            db_lock();
            char sql[128]; /* FIX: 64 was too small for the batches INSERT template */
            snprintf(sql, sizeof(sql),
                     "INSERT OR IGNORE INTO classes(class_num) VALUES(%d);", c);
            db_exec_raw(sql);

            /* Parse batches — use a separate save pointer (strtok_r) */
            char bbuf[128];
            strncpy(bbuf, batches_csv ? batches_csv : "08:00,10:00,12:00,14:00,16:00",
                    sizeof(bbuf)-1);
            bbuf[sizeof(bbuf)-1] = '\0';

            char *inner_save = NULL;
            char *bt = strtok_r(bbuf, ",", &inner_save);
            while (bt) {
                char *e = bt; while (*e == ' ') e++;
                snprintf(sql, sizeof(sql),
                         "INSERT OR IGNORE INTO batches(class_num,time_str)"
                         " VALUES(%d,'%s');", c, e);
                db_exec_raw(sql);
                bt = strtok_r(NULL, ",", &inner_save);
            }
            db_unlock();
        }
        tok = strtok_r(NULL, ",", &outer_save);
    }

    db_ensure_default_schema();
    return 0;
}


/* ── CSV export ──────────────────────────────────────────────── */
char *db_export_csv(int class_num, const char *type)
{
    db_lock();
    sqlite3 *db = db_handle();

    StrBuf sb;
    sb_init(&sb, 4096);

    if (strcmp(type, "students") == 0) {
        sb_append(&sb, "Card UID,Name,Roll No,Batch\r\n");
        sqlite3_stmt *s;
        char sql[128];
        snprintf(sql,sizeof(sql),
                 "SELECT card_uid,name,roll,batchtime FROM students"
                 " WHERE class_num=%d AND is_active=1 ORDER BY name", class_num);
        if (sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK) {
            while (sqlite3_step(s)==SQLITE_ROW) {
                const char *uid   = (const char*)sqlite3_column_text(s,0);
                const char *name  = (const char*)sqlite3_column_text(s,1);
                const char *roll  = (const char*)sqlite3_column_text(s,2);
                const char *batch = (const char*)sqlite3_column_text(s,3);
                sb_appendf(&sb, "%s,%s,%s,%s\r\n",
                           uid?uid:"", name?name:"", roll?roll:"", batch?batch:"");
            }
            sqlite3_finalize(s);
        }
    } else {
        sb_append(&sb, "Card UID,Name,Roll No,Batch,Date,Time,Status\r\n");
        sqlite3_stmt *s;
        char sql[160];
        snprintf(sql,sizeof(sql),
                 "SELECT card_uid,name,roll,batchtime,entry_date,entry_time,status"
                 " FROM attendance WHERE class_num=%d"
                 " ORDER BY entry_date DESC,entry_time DESC", class_num);
        if (sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK) {
            while (sqlite3_step(s)==SQLITE_ROW) {
                const char *uid    = (const char*)sqlite3_column_text(s,0);
                const char *name   = (const char*)sqlite3_column_text(s,1);
                const char *roll   = (const char*)sqlite3_column_text(s,2);
                const char *batch  = (const char*)sqlite3_column_text(s,3);
                const char *date   = (const char*)sqlite3_column_text(s,4);
                const char *ttime  = (const char*)sqlite3_column_text(s,5);
                const char *status = (const char*)sqlite3_column_text(s,6);
                sb_appendf(&sb, "%s,%s,%s,%s,%s,%s,%s\r\n",
                           uid?uid:"", name?name:"", roll?roll:"",
                           batch?batch:"", date?date:"", ttime?ttime:"",
                           status?status:"");
            }
            sqlite3_finalize(s);
        }
    }

    db_unlock();
    return sb.buf;
}

/* ── CSV import ──────────────────────────────────────────────── */
db_import_result_t db_import_csv(int class_num, const char *type,
                                 const char *csv_buf, size_t csv_len)
{
    db_import_result_t result = {0, 0};
    if (!csv_buf || csv_len == 0) return result;

    /* Simple line-by-line parser — skip header row */
    db_lock();
    db_exec_raw("BEGIN;");

    const char *p = csv_buf;
    bool first_line = true;

    while (p < csv_buf + csv_len) {
        /* Find end of line */
        const char *eol = p;
        while (eol < csv_buf + csv_len && *eol != '\n') eol++;

        /* Copy line */
        size_t llen = (size_t)(eol - p);
        if (llen == 0 || (llen == 1 && *p == '\r')) { p = eol + 1; continue; }

        char line[512] = {0};
        size_t copy = llen < sizeof(line)-1 ? llen : sizeof(line)-1;
        memcpy(line, p, copy);
        /* Strip \r */
        if (copy > 0 && line[copy-1] == '\r') line[copy-1] = '\0';

        p = eol + 1;

        /* Skip header */
        if (first_line) { first_line = false; continue; }

        /* Parse CSV fields (simple, no quoted commas) */
        char *fields[8] = {0};
        int  nf = 0;
        char *tok = strtok(line, ",");
        while (tok && nf < 8) {
            /* Trim leading space */
            while (*tok == ' ') tok++;
            fields[nf++] = tok;
            tok = strtok(NULL, ",");
        }

        if (strcmp(type, "students") == 0) {
            /* uid,name,roll,batch */
            char *uid   = nf > 0 ? fields[0] : "";
            char *name  = nf > 1 ? fields[1] : "";
            char *roll  = nf > 2 ? fields[2] : "";
            char *batch = nf > 3 ? fields[3] : "";
            if (!uid || strlen(uid) == 0) { result.skipped++; continue; }

            sqlite3_stmt *s;
            if (sqlite3_prepare_v2(db_handle(),
                    "INSERT OR IGNORE INTO students"
                    "(class_num,card_uid,name,roll,batchtime)"
                    " VALUES(?,?,?,?,?)",
                    -1, &s, NULL) == SQLITE_OK) {
                sqlite3_bind_int(s,  1, class_num);
                sqlite3_bind_text(s, 2, uid,   -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 3, name,  -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 4, roll,  -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 5, batch, -1, SQLITE_STATIC);
                sqlite3_step(s);
                if (sqlite3_changes(db_handle()) > 0) result.added++;
                else result.skipped++;
                sqlite3_finalize(s);
            }
        } else {
            /* uid,name,roll,batch,date,time,status */
            char *uid    = nf > 0 ? fields[0] : "";
            char *name   = nf > 1 ? fields[1] : "";
            char *roll   = nf > 2 ? fields[2] : "";
            char *batch  = nf > 3 ? fields[3] : "";
            char *date   = nf > 4 ? fields[4] : "";
            char *ttime  = nf > 5 ? fields[5] : "";
            char *status = nf > 6 ? fields[6] : "present";
            if (!uid||strlen(uid)==0||!date||strlen(date)==0){result.skipped++;continue;}
            if (!status||strlen(status)==0) status = "present";

            sqlite3_stmt *s;
            if (sqlite3_prepare_v2(db_handle(),
                    "INSERT OR IGNORE INTO attendance"
                    "(class_num,card_uid,name,roll,batchtime,entry_date,entry_time,status)"
                    " VALUES(?,?,?,?,?,?,?,?)",
                    -1, &s, NULL) == SQLITE_OK) {
                sqlite3_bind_int(s,  1, class_num);
                sqlite3_bind_text(s, 2, uid,    -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 3, name,   -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 4, roll,   -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 5, batch,  -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 6, date,   -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 7, ttime,  -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 8, status, -1, SQLITE_STATIC);
                sqlite3_step(s);
                if (sqlite3_changes(db_handle()) > 0) result.added++;
                else result.skipped++;
                sqlite3_finalize(s);
            }
        }
    }

    db_exec_raw("COMMIT;");
    db_unlock();
    return result;
}

/* ── Attendance summary for a class on a date ─────────────────── */
char *db_attendance_summary_json(int class_num, const char *date)
{
    db_lock();
    sqlite3 *db = db_handle();

    /* Count distinct students in this class */
    int total = 0;
    {
        sqlite3_stmt *s;
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM students WHERE class_num=%d AND is_active=1",
                 class_num);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) total = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    /* Count present students for the date */
    int present = 0;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(DISTINCT card_uid) FROM attendance"
                " WHERE class_num=? AND entry_date=? AND status='present'",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_int(s,  1, class_num);
            sqlite3_bind_text(s, 2, date, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) present = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    int absent = (total > present) ? (total - present) : 0;

    db_unlock();

    char *out = malloc(80);
    if (out)
        snprintf(out, 80,
                 "{\"present\":%d,\"absent\":%d,\"total\":%d}",
                 present, absent, total);
    return out;
}

/* ── Export all classes as combined CSV ───────────────────────── */
char *db_export_all_csv(void)
{
    db_lock();
    sqlite3 *db = db_handle();

    /* Dynamically sized buffer */
    size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf) { db_unlock(); return NULL; }
    size_t len = 0;

#define SBUF_APPEND(str) do { \
    size_t _sl = strlen(str); \
    if (len + _sl + 1 > cap) { \
        cap = (len + _sl + 1) * 2; \
        char *_t = realloc(buf, cap); \
        if (!_t) { free(buf); db_unlock(); return NULL; } \
        buf = _t; \
    } \
    memcpy(buf + len, str, _sl); \
    len += _sl; \
    buf[len] = '\0'; \
} while(0)

    /* STUDENTS section */
    SBUF_APPEND("=== STUDENTS ===\r\nClass,Card UID,Name,Roll No,Batch\r\n");
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT class_num,card_uid,name,roll,batchtime"
                " FROM students WHERE is_active=1 ORDER BY class_num,name",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int cn = sqlite3_column_int(s, 0);
                const char *uid   = (const char*)sqlite3_column_text(s, 1);
                const char *name  = (const char*)sqlite3_column_text(s, 2);
                const char *roll  = (const char*)sqlite3_column_text(s, 3);
                const char *batch = (const char*)sqlite3_column_text(s, 4);
                char row[256];
                snprintf(row, sizeof(row), "%d,%s,%s,%s,%s\r\n",
                         cn,
                         uid?uid:"", name?name:"", roll?roll:"", batch?batch:"");
                SBUF_APPEND(row);
            }
            sqlite3_finalize(s);
        }
    }

    /* ATTENDANCE section */
    SBUF_APPEND("\r\n=== ATTENDANCE ===\r\nClass,Card UID,Name,Roll No,Batch,Date,Time,Status\r\n");
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT class_num,card_uid,name,roll,batchtime,entry_date,entry_time,status"
                " FROM attendance ORDER BY class_num,entry_date DESC,entry_time DESC",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int cn = sqlite3_column_int(s, 0);
                const char *uid    = (const char*)sqlite3_column_text(s, 1);
                const char *name   = (const char*)sqlite3_column_text(s, 2);
                const char *roll   = (const char*)sqlite3_column_text(s, 3);
                const char *batch  = (const char*)sqlite3_column_text(s, 4);
                const char *date   = (const char*)sqlite3_column_text(s, 5);
                const char *ttime  = (const char*)sqlite3_column_text(s, 6);
                const char *status = (const char*)sqlite3_column_text(s, 7);
                char row[256];
                snprintf(row, sizeof(row), "%d,%s,%s,%s,%s,%s,%s,%s\r\n",
                         cn,
                         uid?uid:"", name?name:"", roll?roll:"", batch?batch:"",
                         date?date:"", ttime?ttime:"", status?status:"");
                SBUF_APPEND(row);
            }
            sqlite3_finalize(s);
        }
    }

#undef SBUF_APPEND

    db_unlock();
    return buf;
}

/* ── Streaming CSV import helpers ────────────────────────────────
 *
 * These let the HTTP handler process rows one at a time from a small
 * socket chunk buffer instead of loading the whole CSV into a 128 KB
 * heap block (which fails on targets without PSRAM).
 * ──────────────────────────────────────────────────────────────── */

void db_import_begin(void)
{
    db_lock();
    db_exec_raw("BEGIN;");
}

void db_import_end(void)
{
    db_exec_raw("COMMIT;");
    db_unlock();
}

/* Process one data row.  fields[] are mutable pointers into a
 * stack-allocated line buffer — use SQLITE_TRANSIENT so SQLite copies
 * them before the buffer is reused for the next line. */
void db_import_process_line(int class_num, const char *type,
                            char **fields, int nf,
                            db_import_result_t *result)
{
    if (!fields || nf == 0) { result->skipped++; return; }

    if (strcmp(type, "students") == 0) {
        char *uid   = nf > 0 ? fields[0] : "";
        char *name  = nf > 1 ? fields[1] : "";
        char *roll  = nf > 2 ? fields[2] : "";
        char *batch = nf > 3 ? fields[3] : "";
        if (!uid || strlen(uid) == 0) { result->skipped++; return; }

        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_handle(),
                "INSERT OR IGNORE INTO students"
                "(class_num,card_uid,name,roll,batchtime)"
                " VALUES(?,?,?,?,?)",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_int (s, 1, class_num);
            sqlite3_bind_text(s, 2, uid,   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, name,  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, roll,  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, batch, -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            if (sqlite3_changes(db_handle()) > 0) result->added++;
            else result->skipped++;
            sqlite3_finalize(s);
        } else {
            result->skipped++;
        }
    } else {
        /* attendance: uid,name,roll,batch,date,time,status */
        char *uid    = nf > 0 ? fields[0] : "";
        char *name   = nf > 1 ? fields[1] : "";
        char *roll   = nf > 2 ? fields[2] : "";
        char *batch  = nf > 3 ? fields[3] : "";
        char *date   = nf > 4 ? fields[4] : "";
        char *ttime  = nf > 5 ? fields[5] : "";
        char *status = nf > 6 ? fields[6] : "present";
        if (!uid||strlen(uid)==0||!date||strlen(date)==0) { result->skipped++; return; }
        if (!status || strlen(status) == 0) status = "present";

        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_handle(),
                "INSERT OR IGNORE INTO attendance"
                "(class_num,card_uid,name,roll,batchtime,entry_date,entry_time,status)"
                " VALUES(?,?,?,?,?,?,?,?)",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_int (s, 1, class_num);
            sqlite3_bind_text(s, 2, uid,    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, name,   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, roll,   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, batch,  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 6, date,   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 7, ttime,  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 8, status, -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            if (sqlite3_changes(db_handle()) > 0) result->added++;
            else result->skipped++;
            sqlite3_finalize(s);
        } else {
            result->skipped++;
        }
    }
}

