#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "AlternateSelf.h"
#include "Log.h"
#include "r2_diary.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *as_db;
static pthread_mutex_t as_lock = PTHREAD_MUTEX_INITIALIZER;
static int as_ready;

static int as_exec(const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(as_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Alternate-Self Lab] SQLite error: %s\n",
                err ? err : (as_db ? sqlite3_errmsg(as_db) : "no database"));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int r2_altself_init(void)
{
    pthread_mutex_lock(&as_lock);
    if (as_ready) { pthread_mutex_unlock(&as_lock); return 0; }
    if (sqlite3_open_v2(R2_DIARY_DATABASE, &as_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            NULL) != SQLITE_OK) {
        if (as_db) sqlite3_close(as_db);
        as_db = NULL;
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    sqlite3_busy_timeout(as_db, 5000);
    sqlite3_exec(as_db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;",
                 NULL, NULL, NULL);
    const char *schema =
        "CREATE TABLE IF NOT EXISTS r2_alternate_self_branches ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL,"
        " scenario TEXT NOT NULL,"
        " assumptions TEXT,"
        " predicted_outcome TEXT,"
        " conclusion TEXT,"
        " status TEXT NOT NULL DEFAULT 'active',"
        " evidence_event_id INTEGER,"
        " log_event_id INTEGER NOT NULL,"
        " created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " updated_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " FOREIGN KEY(evidence_event_id) REFERENCES r2_log_events(id),"
        " FOREIGN KEY(log_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS r2_alternate_self_status_idx "
        "ON r2_alternate_self_branches(status,id);"
        "DELETE FROM r2_alternate_self_branches WHERE id NOT IN "
        "(SELECT MIN(id) FROM r2_alternate_self_branches GROUP BY log_event_id);"
        "CREATE UNIQUE INDEX IF NOT EXISTS r2_alternate_self_log_event_idx "
        "ON r2_alternate_self_branches(log_event_id);";
    if (as_exec(schema) != 0) {
        sqlite3_close(as_db); as_db = NULL;
        pthread_mutex_unlock(&as_lock); return -1;
    }
    as_ready = 1;
    pthread_mutex_unlock(&as_lock);
    fprintf(stderr, "[R2 Alternate-Self Lab] Initialized.\n");
    return 0;
}

void r2_altself_shutdown(void)
{
    pthread_mutex_lock(&as_lock);
    if (as_db) sqlite3_close(as_db);
    as_db = NULL; as_ready = 0;
    pthread_mutex_unlock(&as_lock);
}

int64_t r2_altself_create(const char *name, const char *scenario,
                          const char *assumptions, const char *predicted_outcome,
                          const char *conclusion, int64_t evidence_event_id)
{
    if (!name || !*name || !scenario || !*scenario) return -1;
    pthread_mutex_lock(&as_lock);
    int ready = as_ready && as_db != NULL;
    pthread_mutex_unlock(&as_lock);
    if (!ready) return -1;
    char detail[8192];
    snprintf(detail, sizeof(detail),
        "ALTERNATE-SELF HYPOTHESIS — NOT A REAL EVENT OR MEMORY\n"
        "Branch: %s\nScenario: %s\nAssumptions: %s\n"
        "Predicted outcome: %s\nTentative conclusion: %s\n"
        "Evidence event ID: %lld\n"
        "This branch is isolated from factual memory; its outcome has not been observed.",
        name, scenario, assumptions && *assumptions ? assumptions : "(none supplied)",
        predicted_outcome && *predicted_outcome ? predicted_outcome : "(not specified)",
        conclusion && *conclusion ? conclusion : "(not specified)",
        (long long)evidence_event_id);
    int64_t event_id = r2_log_hypothetical(scenario, assumptions,
        predicted_outcome, conclusion, "Alternate-Self Lab; hypothetical only");
    if (event_id < 0) return -1;
    /* Keep the explicit label and branch identity in the event record. */
    r2_log_event(R2_LOG_HYPOTHETICAL, "alternate_self_branch_created",
                 "Created an isolated Alternate-Self hypothetical branch.",
                 detail, "AlternateSelf.c");

    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) { pthread_mutex_unlock(&as_lock); return -1; }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "INSERT INTO r2_alternate_self_branches"
        "(name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,log_event_id)"
        " VALUES(?,?,?,?,?,'active',?,?);", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st,1,name,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,scenario,-1,SQLITE_TRANSIENT);
        if (assumptions) sqlite3_bind_text(st,3,assumptions,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,3);
        if (predicted_outcome) sqlite3_bind_text(st,4,predicted_outcome,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,4);
        if (conclusion) sqlite3_bind_text(st,5,conclusion,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(st,5);
        if (evidence_event_id > 0) sqlite3_bind_int64(st,6,evidence_event_id); else sqlite3_bind_null(st,6);
        sqlite3_bind_int64(st,7,event_id);
        rc = sqlite3_step(st);
    }
    int64_t id = rc == SQLITE_DONE ? sqlite3_last_insert_rowid(as_db) : -1;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    if (id > 0 && evidence_event_id > 0)
        r2_log_link(evidence_event_id, event_id, "evidence_for_hypothesis",
                    "Evidence is referenced for comparison; it is not altered by this branch.");
    return id;
}

int64_t r2_altself_import_hypothesis(const char *scenario,
                                    const char *assumptions,
                                    const char *predicted_outcome,
                                    const char *conclusion,
                                    int64_t hypothetical_event_id,
                                    int64_t evidence_event_id)
{
    if (!scenario || !*scenario || hypothetical_event_id <= 0) return -1;
    char name[128];
    snprintf(name, sizeof(name), "R2 hypothesis from event %lld",
             (long long)hypothetical_event_id);
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) {
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "SELECT id FROM r2_alternate_self_branches "
        "WHERE scenario=? AND COALESCE(assumptions,'')=COALESCE(?,'') "
        "AND COALESCE(predicted_outcome,'')=COALESCE(?,'') "
        "AND COALESCE(conclusion,'')=COALESCE(?,'') AND status!='discarded' "
        "ORDER BY id LIMIT 1;", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, scenario, -1, SQLITE_TRANSIENT);
        if (assumptions) sqlite3_bind_text(st, 2, assumptions, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 2);
        if (predicted_outcome) sqlite3_bind_text(st, 3, predicted_outcome, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
        if (conclusion) sqlite3_bind_text(st, 4, conclusion, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int64_t existing = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            pthread_mutex_unlock(&as_lock);
            return existing;
        }
    }
    sqlite3_finalize(st);
    st = NULL;
    rc = sqlite3_prepare_v2(as_db,
        "INSERT OR IGNORE INTO r2_alternate_self_branches"
        "(name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,log_event_id)"
        " VALUES(?,?,?,?,?,'active',?,?);", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, scenario, -1, SQLITE_TRANSIENT);
        if (assumptions) sqlite3_bind_text(st, 3, assumptions, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
        if (predicted_outcome) sqlite3_bind_text(st, 4, predicted_outcome, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
        if (conclusion) sqlite3_bind_text(st, 5, conclusion, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
        if (evidence_event_id > 0) sqlite3_bind_int64(st, 6, evidence_event_id); else sqlite3_bind_null(st, 6);
        sqlite3_bind_int64(st, 7, hypothetical_event_id);
        rc = sqlite3_step(st);
    }
    int64_t id = rc == SQLITE_DONE ? sqlite3_last_insert_rowid(as_db) : -1;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    return id;
}

static int as_set_status(int64_t id, const char *status)
{
    if (id <= 0) return -1;
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) { pthread_mutex_unlock(&as_lock); return -1; }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "UPDATE r2_alternate_self_branches SET status=?,updated_utc=CURRENT_TIMESTAMP "
        "WHERE id=? AND status!='discarded';", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st,1,status,-1,SQLITE_STATIC);
        sqlite3_bind_int64(st,2,id);
        rc = sqlite3_step(st);
    }
    int changed = rc == SQLITE_DONE ? sqlite3_changes(as_db) : 0;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    if (changed) {
        char details[256];
        snprintf(details,sizeof(details),"Branch ID: %lld; new status: %s. The branch remains hypothetical.",
                 (long long)id,status);
        r2_log_event(R2_LOG_HYPOTHETICAL,"alternate_self_branch_status",
                     "Alternate-Self branch status changed.",details,"AlternateSelf.c");
        return 0;
    }
    return -1;
}
int r2_altself_discard(int64_t id) { return as_set_status(id,"discarded"); }
int r2_altself_retain_hypothesis(int64_t id) { return as_set_status(id,"retained_hypothesis"); }

static char *as_query(const char *sql, int64_t first, int64_t second, int limit)
{
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) { pthread_mutex_unlock(&as_lock); return NULL; }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(as_db,sql,-1,&st,NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&as_lock); return NULL;
    }
    int param_count = sqlite3_bind_parameter_count(st);
    if (param_count >= 1) sqlite3_bind_int64(st,1,first);
    if (param_count >= 2) sqlite3_bind_int64(st,2,second);
    if (param_count >= 3) sqlite3_bind_int(st,3,limit);
    size_t cap=4096,len=0;
    char *out=malloc(cap);
    if (!out) { sqlite3_finalize(st); pthread_mutex_unlock(&as_lock); return NULL; }
    out[0]='\0';
    int rc;
    while ((rc=sqlite3_step(st))==SQLITE_ROW) {
        int64_t id=sqlite3_column_int64(st,0);
        const unsigned char *name=sqlite3_column_text(st,1);
        const unsigned char *scenario=sqlite3_column_text(st,2);
        const unsigned char *assumptions=sqlite3_column_text(st,3);
        const unsigned char *outcome=sqlite3_column_text(st,4);
        const unsigned char *conclusion=sqlite3_column_text(st,5);
        const unsigned char *status=sqlite3_column_text(st,6);
        int64_t evidence=sqlite3_column_int64(st,7);
        const unsigned char *created=sqlite3_column_text(st,8);
        int needed=snprintf(NULL,0,
            "Branch #%lld [%s] %s\n  Scenario: %s\n  Assumptions: %s\n"
            "  Predicted outcome: %s\n  Tentative conclusion: %s\n"
            "  Evidence event ID: %lld\n  Created UTC: %s\n\n",
            (long long)id,status?(const char*)status:"unknown",
            name?(const char*)name:"unnamed",
            scenario?(const char*)scenario:"",
            assumptions?(const char*)assumptions:"(none)",
            outcome?(const char*)outcome:"(not specified)",
            conclusion?(const char*)conclusion:"(not specified)",
            (long long)evidence,
            created?(const char*)created:"unknown");
        if (needed<0 || len+(size_t)needed+2>1024*1024) break;
        if (len+(size_t)needed+2>cap) {
            size_t next=cap; while(next<len+(size_t)needed+2) next*=2;
            char *grown=realloc(out,next); if(!grown){free(out);out=NULL;break;}
            out=grown;cap=next;
        }
        snprintf(out+len,cap-len,
            "Branch #%lld [%s] %s\n  Scenario: %s\n  Assumptions: %s\n"
            "  Predicted outcome: %s\n  Tentative conclusion: %s\n"
            "  Evidence event ID: %lld\n  Created UTC: %s\n\n",
            (long long)id,status?(const char*)status:"unknown",
            name?(const char*)name:"unnamed",
            scenario?(const char*)scenario:"",
            assumptions?(const char*)assumptions:"(none)",
            outcome?(const char*)outcome:"(not specified)",
            conclusion?(const char*)conclusion:"(not specified)",
            (long long)evidence,
            created?(const char*)created:"unknown");
        len+=(size_t)needed;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    if (out && !len) snprintf(out,cap,"No Alternate-Self branches found.\n");
    return out;
}
char *r2_altself_list(int limit)
{
    if (limit < 1) limit = 25;
    if (limit > 200) limit = 200;
    return as_query("SELECT id,name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,created_utc "
                    "FROM r2_alternate_self_branches ORDER BY id DESC LIMIT ?;",limit,0,0);
}
char *r2_altself_show(int64_t id)
{
    return as_query("SELECT id,name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,created_utc "
                    "FROM r2_alternate_self_branches WHERE id=?;",id,0,0);
}
char *r2_altself_compare(int64_t a, int64_t b)
{
    return as_query("SELECT id,name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,created_utc "
                    "FROM r2_alternate_self_branches WHERE id IN (?,?) ORDER BY id;",a,b,0);
}
