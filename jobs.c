/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#define _DEFAULT_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "cjson/cJSON.h"

#include "default.inc"
#include "main.h"
#include "user.h"
#include "vec.h"

/* The list will access them */
int busy_slots = 0;
int max_slots = 1;
time_t sstmp_skip_sec = DEFAULT_EMAIL_TIME; // skip task smaller than 200 s

char *email_sender;

struct Notify {
    int socket;
    int jobid;
    struct Notify *next;
};

/* Globals — dynamic arrays replacing linked lists */
static vec_t active_jobs;    /* QUEUED, RUNNING, PAUSE, etc. */
static vec_t finished_jobs;  /* FINISHED, SKIPPED */
static int jobids = 1000;
/* This is used for dependencies from jobs
 * already out of the queue */
static int last_errorlevel = 0; /* Before the first job, let's consider
                                   a good previous result */
/* We need this to handle well "-d" after a "-nf" run */
static int last_finished_jobid;

static struct Notify *first_notify = 0;
static char buff[256];
/* server will access them */
int max_jobs;

void init_jobs(void) {
    vec_init(&active_jobs);
    vec_init(&finished_jobs);
}

void destroy_jobs(void) {
    vec_destroy(&active_jobs);
    vec_destroy(&finished_jobs);
}

static struct Job *get_job(int jobid);
static int fork_cmd(int UID, char const *path, char const *cmd);
static int safe_pause_job(struct Job *p);

/* Return index in active_jobs, or -1 */
static int findjob_idx(int jobid) {
    for (size_t i = 0; i < vec_size(&active_jobs); i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->jobid == jobid) return (int)i;
    }
    return -1;
}

/* Return index in finished_jobs, or -1 */
static int find_finished_idx(int jobid) {
    for (size_t i = 0; i < vec_size(&finished_jobs); i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        if (p->jobid == jobid) return (int)i;
    }
    return -1;
}

void notify_errorlevel(struct Job *p);

void s_set_jobids(int i) {
    jobids = i;
    set_jobids_DB(i);
}

void setup_ssmtp() {
    email_sender = getenv("TS_MAIL_FROM");
    if (email_sender == NULL) {
        email_sender = DEFAULT_EMAIL_SENDER;
    }
    char *time_s = getenv("TS_MAIL_TIME");
    if (time_s != NULL) {
        float time_sec;
        int ret = sscanf(time_s, "%f", &time_sec);
        if (ret == 1) {
            sstmp_skip_sec = time_sec;
        }
    }
}

static void send_mail_via_ssmtp(struct Job *p) {
    time_t real_sec = p->result.real_sec; // units in second
    if (real_sec == 0) {
        real_sec = p->info.end_time - p->info.start_time; // TODO add a function
    }
    // skip the short task
    if (real_sec < sstmp_skip_sec || p->email == NULL) {
        return;
    }
    char const *state =
        (p->result.errorlevel || p->result.signal || p->result.died_by_signal)
            ? "failed"
            : "finished";
    time_repr_t r = format_time(real_sec);
    char cmd[2048];
    snprintf(cmd, 2047,
             "echo \"Subject: %s[%d] n_core: %d, Elsp %.3f %c from MSI\nFrom: "
             "TS<%s>\nTo: %s\n\n\n Cmd: %s [%s] Output: %s\" | ssmtp %s",
             p->label, p->jobid, p->num_slots, r.value, r.unit, p->email,
             email_sender, p->command + p->command_strip, state,
             p->output_filename, p->email);
    fork_cmd(root_UID, NULL, cmd);
}

static void sound_notify(struct Job *p) {
#ifdef SOUND
    time_t real_sec = p->result.real_sec;
    if (real_sec == 0.0) {
        real_sec = p->info.end_time - p->info.start_time; // TODO ADD FUNC
    }
    // skip the short task
    if (real_sec < 5) {
        return;
    }
    char cmd[256];
    if (p->result.errorlevel == 0) {
        snprintf(cmd, 255, "paplay -p \"%s\" -s %s", DEFAULT_NOTIFICATION_SOUND,
                 DEFAULT_PULSE_SERVER);
    } else {
        snprintf(cmd, 255, "paplay -p \"%s\" -s %s", DEFAULT_ERROR_SOUND,
                 DEFAULT_PULSE_SERVER);
    }
    printf("%s\n", cmd);
    fork_cmd(user_UID[p->ts_UID], NULL, cmd);
#endif
}

static void destroy_job(struct Job *p) {
    if (p != NULL) {
        free(p->notify_errorlevel_to);
        free(p->command);
        free(p->work_dir);
        free(p->output_filename);
        pinfo_free(&p->info);
        free(p->depend_on);
        free(p->label);
        free(p);
    }
}

static void pause_job_config(struct Job* p) {
    if (p->info.pause_time == 0) {
        p->info.pause_time = get_monotonic_sec();
        // printf("set pause_time at %ld\n", p->info.pause_time);
        update_field_int64("Jobs", p->jobid, "pause_time", p->info.pause_time);
    }
}

static void rerun_job_config(struct Job* p) {
    if (p->info.pause_time != 0) {
        p->info.pause_duration += get_monotonic_sec() - p->info.pause_time;
        update_field_int64("Jobs", p->jobid, "pause_duration", p->info.pause_duration);

        p->info.pause_time = 0;
        update_field_int64("Jobs", p->jobid, "pause_time", p->info.pause_time);
    }
}

static void free_cores(struct Job *p) {
    if (p == NULL || p->num_allocated == 0) {
        return;
    }
    int ts_UID = p->ts_UID;
    user_busy[ts_UID] -= p->num_slots;
    busy_slots -= p->num_slots;
    p->num_allocated = 0;
    // user_queue[ts_UID]--;
    user_jobs[ts_UID]--;
}

static int config_running(struct Job *p) {
    if (p == NULL || (p->state != PAUSE && p->state != QUEUED)) {
        return 1;
    }
    printf("config running %d, is_sleep %d\n", p->jobid, is_sleep(p));
    if (is_sleep(p) == 1) {
        cgroups_thaw_job(p);
    }

    // printf("Start job[%d]: PID: %d\n", p->jobid, p->pid);
    int ts_UID = p->ts_UID;
    user_busy[ts_UID] += p->num_slots;
    busy_slots += p->num_slots;
    p->num_allocated = p->num_slots;
    user_jobs[ts_UID]++;
    p->state = RUNNING;
    rerun_job_config(p);
    return 0;
}

/* Serialize a job and add it to the JSON array. Returns 1 for success, 0 for
 * failure. */
static int add_job_to_json_array(struct Job *p, cJSON *jobs) {
    cJSON *job = cJSON_CreateObject();
    if (job == NULL) {
        error("Error initializing JSON object for job %i.", p->jobid);
        return 0;
    }
    cJSON_AddItemToArray(jobs, job);

    /* Add fields */
    cJSON *field;

    /* ID */
    field = cJSON_CreateNumber(p->jobid);
    if (field == NULL) {
        error("Error initializing JSON object for job %i field ID.", p->jobid);
        return 0;
    }
    cJSON_AddItemToObject(job, "ID", field);

    /* State */
    char const *state_string = jstate2string(p->state);
    field = cJSON_CreateStringReference(state_string);
    if (field == NULL) {
        error("Error initializing JSON object for job %i field State (value "
              "%d/%s).",
              p->jobid, p->state, state_string);
        return 0;
    }
    cJSON_AddItemToObject(job, "State", field);

    /* num_slots */
    field = cJSON_CreateNumber(p->num_slots);
    if (field == NULL) {
        error("Error initializing JSON object for job %i field ID.", p->jobid);
        return 0;
    }
    cJSON_AddItemToObject(job, "Proc.", field);

    /* user */
    field = cJSON_CreateStringReference(user_name[p->ts_UID]);
    if (field == NULL) {
        error("Error initializing JSON object for job %i field State (value "
              "%d/%s).",
              p->jobid, p->state, state_string);
        return 0;
    }
    cJSON_AddItemToObject(job, "User", field);

    /* label */

    if (p->label != NULL) {
        field = cJSON_CreateStringReference(p->label);
    } else {
        field = cJSON_CreateNull();
    }
    if (field == NULL) {
        error("Error initializing JSON object for job %i field State (value "
              "%d/%s).",
              p->jobid, p->state, state_string);
        return 0;
    }
    cJSON_AddItemToObject(job, "Label", field);

    /* Output */
    field = cJSON_CreateStringReference(p->output_filename);
    if (field == NULL) {
        error("Error initializing JSON object for job %i field Output (value "
              "%s).",
              p->jobid, p->output_filename);
        return 0;
    }
    cJSON_AddItemToObject(job, "Output", field);

    /* E-Level */
    if (p->state == FINISHED) {
        field = cJSON_CreateNumber(p->result.errorlevel);
    } else {
        field = cJSON_CreateNull();
    }
    if (field == NULL) {
        error("Error initializing JSON object for job %i field E-Level.",
              p->jobid);
        return 0;
    }
    cJSON_AddItemToObject(job, "E-Level", field);

    /* Time */
    if (p->state == FINISHED) {
        field = cJSON_CreateNumber(p->result.real_sec);
        if (field == NULL) {
            error("Error initializing JSON object for job %i field [real_sec] "
                  "(value %ld).",
                  p->jobid, (long)p->result.real_sec);
            return 0;
        }
    } else {
        field = cJSON_CreateNull();
        if (field == NULL) {
            error("Error initializing JSON object for job %i field [real_sec] (no "
                  "result).", p->jobid);
            return 0;
        }
    }
    cJSON_AddItemToObject(job, "Time_ms", field);

    /* Command */
    field = cJSON_CreateStringReference(p->command + p->command_strip);
    if (field == NULL) {
        error("Error initializing JSON object for job %i field Command (value "
              "%s).",
              p->jobid, p->command);
        return 0;
    }
    cJSON_AddItemToObject(job, "Command", field);

    return 1;
}

void send_list_line(int s, char const *str) {
    struct Msg m = default_msg();

    /* Message */
    m.type = LIST_LINE;
    m.u.size = strlen(str) + 1;

    send_msg(s, &m);

    /* Send the line */
    send_bytes(s, str, m.u.size);
}

static void send_urgent_ok(int s) {
    struct Msg m = default_msg();

    /* Message */
    m.type = URGENT_OK;

    send_msg(s, &m);
}

static void send_swap_jobs_ok(int s) {
    struct Msg m = default_msg();

    /* Message */
    m.type = SWAP_JOBS_OK;

    send_msg(s, &m);
}

void s_sort_jobs() {
    /* Stable partition: RUNNING first, then others, preserving relative order */
    size_t n = vec_size(&active_jobs);
    vec_t running;
    vec_t other;
    vec_init(&running);
    vec_init(&other);

    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == RUNNING)
            vec_push(&running, p);
        else
            vec_push(&other, p);
    }

    vec_clear(&active_jobs);
    for (size_t i = 0; i < vec_size(&running); i++)
        vec_push(&active_jobs, vec_get(&running, i));
    for (size_t i = 0; i < vec_size(&other); i++)
        vec_push(&active_jobs, vec_get(&other, i));

    vec_destroy(&running);
    vec_destroy(&other);
}

struct Job *findjob(int jobid) {
    int idx = findjob_idx(jobid);
    return (idx >= 0) ? (struct Job *)vec_get(&active_jobs, (size_t)idx) : NULL;
}

static int check_timeout(struct Job *p) {
    // printf("check job %d\n", p->jobid);
    if (p->pid == 0) {
        return 0;
    }

    time_t wall_time = i64abs(p->wall_time);
    time_t work_time = get_work_time_by_job(p);
    if (work_time <= wall_time || work_time < 3) {
        return 0;
    }

    printf("Job[%d|pid:%d] is time-out %ld sec (limit %ld)\n", p->jobid, p->pid, work_time, wall_time);
    if (safe_pause_job(p) == 0) {
        p->wall_time = -i64abs(p->wall_time) - 86400; // plus 24 hrs
        update_field_int64("Jobs", p->jobid, "wall_time", p->wall_time);
        p->state = PAUSE;
    }
    return 1; // time-out
}

static int s_check_timeout() {
    /* Collect timed-out jobs: iterate backwards so removals don't shift later indices */
    size_t n = vec_size(&active_jobs);
    vec_t timed_out;
    vec_init(&timed_out);

    for (size_t i = n; i > 0; i--) {
        size_t idx = i - 1;
        struct Job *p = (struct Job *)vec_get(&active_jobs, idx);
        if (p->state == RUNNING && check_timeout(p)) {
            vec_remove(&active_jobs, idx);
            vec_push(&timed_out, p);
        }
    }

    /* Push timed-out jobs to the back */
    size_t tn = vec_size(&timed_out);
    for (size_t i = 0; i < tn; i++)
        vec_push(&active_jobs, vec_get(&timed_out, i));

    int had_timeout = tn > 0;
    vec_destroy(&timed_out);
    return had_timeout;
}

int s_update_slots_usage() {
    int timeout_flag = s_check_timeout();

    int slots_usage = 0;
    for (int i = 0; i < user_number; i++)
        user_busy[i] = user_jobs[i] = user_queue[i] = 0;

    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        int uid = p->ts_UID;
        if (p->state == RUNNING) {
            slots_usage += p->num_slots;
            user_busy[uid] += p->num_slots;
            user_jobs[uid]++;
        } else {
            user_queue[uid]++;
        }
    }

    if (slots_usage != busy_slots) {
        printf("Error: invalid slots: %d vs %d\n", slots_usage, busy_slots);
        busy_slots = slots_usage;
    }
    if (timeout_flag)
        next_run_job();
    return slots_usage;
}

static struct Job *job_by_pid(pid_t pid) {
    if (pid == 0) return NULL;
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->pid == pid) return p;
    }
    return NULL;
}

// return 1 for running, other is dead
int s_check_running_pid(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }

    // kill(pid, 0) 不真正发送信号
    if (kill(pid, 0) == 0) {
        return 1;
    }

    // EPERM 说明进程存在但无权限
    if (errno == EPERM) {
        return 1;
    }
    return 0;
}

// if any error return non-0;
int s_check_relink(int s, pid_t pid, int ts_UID) {
    struct Job *p = job_by_pid(pid);
    if (p != NULL && (p->state != DELINK && p->state != WAIT)) {
        sprintf(buff, "  Error: PID [%i] is already in job as Jobid: %i [%s]\n",
                pid, p->jobid, jstate2string(p->state));
        send_list_line(s, buff);
        return -1;
    }

    char filename[256];
    struct stat t_stat;

    snprintf(filename, 256, "/proc/%d/stat", pid);
    if (stat(filename, &t_stat) == -1) {
        sprintf(buff, "  Error: PID [%i] is not running\n", pid);
        send_list_line(s, buff);
        return -1;
    }

    int job_tsUID = get_tsUID(t_stat.st_uid);
    if (ts_UID == 0) {
        ;
    } else if (ts_UID == job_tsUID) {
        ;
    } else {
        snprintf(buff, 255,
                 "  Error: PID [%i] is owned by [%d] `%150s` not the user [%d] "
                 "`%s`\n",
                 pid, user_UID[job_tsUID], user_name[job_tsUID],
                 user_UID[ts_UID], user_name[ts_UID]);
        send_list_line(s, buff);
        return -1;
    }

    // printf("is delink %d, %d\n", p->jobid, p->state == DELINK);
    check_timeout(p);
    return job_tsUID;
}

static struct Job *findjob_holding_client() {
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == HOLDING_CLIENT) return p;
    }
    return 0;
}

static struct Job *find_finished_job(int jobid) {
    int idx = find_finished_idx(jobid);
    return (idx >= 0) ? (struct Job *)vec_get(&finished_jobs, (size_t)idx) : NULL;
}

static int count_not_finished_jobs() {
    return (int)vec_size(&active_jobs);
}

static void add_notify_errorlevel_to(struct Job *job, int jobid) {
    int *p;
    int newsize = (job->notify_errorlevel_to_size + 1) * sizeof(int);
    p = (int *)realloc(job->notify_errorlevel_to, newsize);

    if (p == 0) {
        error(
            "Cannot allocate more memory for notify_errorlist_to for jobid %i,"
            " having already %i elements",
            job->jobid, job->notify_errorlevel_to_size);
    }

    job->notify_errorlevel_to = p;
    job->notify_errorlevel_to_size += 1;
    job->notify_errorlevel_to[job->notify_errorlevel_to_size - 1] = jobid;
}

void s_kill_all_jobs(int s, int ts_UID) {
    s_count_running_jobs(s, ts_UID);

    /* send running job PIDs */
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == RUNNING && (ts_UID == 0 || p->ts_UID == ts_UID)) {
            send(s, &p->pid, sizeof(int), 0);
        }
    }
}

void s_count_running_jobs(int s, int ts_UID) {
    int count = 0;
    size_t n = vec_size(&active_jobs);
    struct Msg m = default_msg();

    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state == RUNNING && (ts_UID == 0 || p->ts_UID == ts_UID))
            ++count;
    }

    m.type = COUNT_RUNNING;
    m.u.count_running = count;
    send_msg(s, &m);
}

int s_get_job_tsUID(int jobid) {
    struct Job *p = get_job(jobid);
    if (p == NULL) return -1;
    return p->ts_UID;
}

void s_get_label(int s, int jobid) {
    struct Job *p = 0;
    char *label;

    if (jobid == -1) {
        /* Find the last job added */
        size_t n = vec_size(&active_jobs);
        if (n > 0) p = (struct Job *)vec_get(&active_jobs, n - 1);

        /* Look in finished jobs if needed */
        if (p == 0) {
            n = vec_size(&finished_jobs);
            if (n > 0) p = (struct Job *)vec_get(&finished_jobs, n - 1);
        }
    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        snprintf(buff, 255,
                 "[get_label0] Job %i not finished or not running.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    if (p->label) {
        label = (char *)malloc(strlen(p->label) + 1);
        sprintf(label, "%s\n", p->label);
    } else {
        label = "";
    }
    send_list_line(s, label);
    if (p->label) free(label);
}

void s_add_wtime(int s, int jobid, int64_t add_wtime) {
    if (jobid == 0) {
        snprintf(buff, 255, "Error: job ID is not specified. Use --job [jobid] or -J [jobid].\n");
        send_list_line(s, buff);
        return;
    }

    struct Job *p = findjob(jobid);
    if (p == NULL) {
        snprintf(buff, 255, "cannot find Job by id: %d\n", jobid);
        send_list_line(s, buff);
        return;
    }

    int64_t new_wtime = i64abs(p->wall_time) + add_wtime;
    if (new_wtime <= 0) {
        snprintf(buff, 255,
                 "Error: [%d], negative wall-time after change from %ld => %ld\n",
                 jobid, i64abs(p->wall_time), new_wtime);
        send_list_line(s, buff);
        return;
    }
    p->wall_time = (p->wall_time < 0) ? -new_wtime : new_wtime;

    insert_or_replace_DB(p, "Jobs");
    snprintf(buff, 255, "Set [%d] wall-time as %ld hr\n", jobid,
             i64abs(p->wall_time));
    send_list_line(s, buff);
    printf("s_add_wtime(): %s", buff);
}

void s_send_cmd(int s, int jobid) {
    struct Job *p = 0;
    char *cmd;

    if (jobid == -1) {
        size_t n = vec_size(&active_jobs);
        if (n > 0) p = (struct Job *)vec_get(&active_jobs, n - 1);
        if (p == 0) {
            n = vec_size(&finished_jobs);
            if (n > 0) p = (struct Job *)vec_get(&finished_jobs, n - 1);
        }
    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        snprintf(buff, 255,
                 "[get_label1] Job %i not finished or not running.\n", jobid);
        send_list_line(s, buff);
        return;
    }
    cmd = (char *)malloc(strlen(p->command) + 1);
    sprintf(cmd, "%s\n", p->command);
    send_list_line(s, cmd);
    free(cmd);
}

static char *get_ofile_from_FD(int pid) {
    char path[256], buff[256] = "";
    snprintf(path, 255, "/proc/%d/fd/1", pid);
    ssize_t len = readlink(path, buff, sizeof(buff) - 1);
    if (len == -1 || len == 0) {
        return NULL;
    }
    buff[len] = '\0';
    int namesize = (int)len + 1;
    char *f = (char *)malloc(namesize);
    strncpy(f, buff, namesize);
    return f;
}

void s_mark_job_running(int jobid) {
    struct Job *p = findjob(jobid);
    if (!p) {
        error("Cannot mark the jobid %i RUNNING.", jobid);
    }
    if (p->state == RELINK) {
        if (p->output_filename == NULL) {
            p->output_filename = get_ofile_from_FD(p->pid);
        }
        if (is_sleep(p) == 1) {
            p->state = PAUSE;
            return;
        } else {
            p->state = QUEUED;
        }
    }
    if (config_running(p)) {
        error("Err. in s_mark_job_running(): Cannot mark Job %d as RUNNING "
              "from state %i\n",
              jobid, p->state);
    }
}

/* -1 means nothing awaken, otherwise returns the jobid awaken */
int wake_hold_client() {
    struct Job *p;
    p = findjob_holding_client();
    if (p) {
        p->state = QUEUED;
        return p->jobid;
    }
    return -1;
}

char const *jstate2string(enum Jobstate s) {
    char const *jobstate;
    switch (s) {
    case QUEUED:         jobstate = "queued  "; break;
    case RUNNING:        jobstate = "running "; break;
    case FINISHED:       jobstate = "finished"; break;
    case SKIPPED:
    case HOLDING_CLIENT: jobstate = "skipped "; break;
    case RELINK:         jobstate = "relink  "; break;
    case WAIT:           jobstate = "wait    "; break;
    case DELINK:         jobstate = "delink  "; break;
    case LOCKED:         jobstate = "locked  "; break;
    case PAUSE:          jobstate = "pause   "; break;
    default:             jobstate = "UNKNOWN ";
    }
    return jobstate;
}

void s_list(int s, int ts_UID, enum ListFormat listFormat) {
    s_update_slots_usage();

    size_t an = vec_size(&active_jobs);
    size_t fn = vec_size(&finished_jobs);
    char *buffer;
    if (listFormat == DEFAULT) {
        buffer = joblist_headers();
        send_list_line(s, buffer);
        free(buffer);

        for (size_t i = 0; i < an; i++) {
            struct Job *p = (struct Job *)vec_get(&active_jobs, i);
            if (p->state != HOLDING_CLIENT) {
                if (p->ts_UID == ts_UID || ts_UID == 0) {
                    buffer = joblist_line(p);
                    send_list_line(s, buffer);
                    free(buffer);
                }
            }
        }

        if (fn > 0 && an > 0)
            send_list_line(s, "----- Finished -----\n");

        for (size_t i = 0; i < fn; i++) {
            struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
            if (p->ts_UID == ts_UID || ts_UID == 0) {
                buffer = joblist_line(p);
                send_list_line(s, buffer);
                free(buffer);
            }
        }
        if (ts_UID == 0) {
            s_user_status_all(s);
        } else {
            s_user_status(s, ts_UID);
        }
    } else if (listFormat == JSON) {
        cJSON *jobs = cJSON_CreateArray();
        if (jobs == NULL) {
            error("Error initializing JSON array.");
            goto end;
        }
        for (size_t i = 0; i < an; i++) {
            struct Job *p = (struct Job *)vec_get(&active_jobs, i);
            if (p->state != HOLDING_CLIENT) {
                if (add_job_to_json_array(p, jobs) == 0) goto end;
            }
        }
        for (size_t i = 0; i < fn; i++) {
            struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
            if (add_job_to_json_array(p, jobs) == 0) goto end;
        }

        buffer = cJSON_PrintUnformatted(jobs);
        if (buffer == NULL) {
            error("Error converting jobs to JSON.");
            goto end;
        }

        size_t buffer_strlen = strlen(buffer);
        char *newbuf = realloc(buffer, buffer_strlen + 2);
        if (newbuf == NULL) {
            free(buffer);
            buffer = NULL;
            goto end;
        }
        buffer = newbuf;
        strcat(buffer, "\n");

        send_list_line(s, buffer);
        goto end;

    end:
        cJSON_Delete(jobs);
        free(buffer);
    } else if (listFormat == TAB) {
        for (size_t i = 0; i < an; i++) {
            struct Job *p = (struct Job *)vec_get(&active_jobs, i);
            if (p->state != HOLDING_CLIENT) {
                buffer = joblist_line_plain(p);
                send_list_line(s, buffer);
                free(buffer);
            }
        }
        for (size_t i = 0; i < fn; i++) {
            struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
            buffer = joblist_line_plain(p);
            send_list_line(s, buffer);
            free(buffer);
        }
    }
}

void s_list_all(int s, enum ListFormat listFormat) {
    char *buffer;

    buffer = joblist_headers();
    send_list_line(s, buffer);
    free(buffer);

    size_t an = vec_size(&active_jobs);
    size_t fn = vec_size(&finished_jobs);

    for (size_t i = 0; i < an; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->state != HOLDING_CLIENT) {
            buffer = joblist_line(p);
            send_list_line(s, buffer);
            free(buffer);
        }
    }

    if (fn > 0 && an > 0)
        send_list_line(s, "\n ----- Finished -----\n");

    for (size_t i = 0; i < fn; i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        buffer = joblist_line(p);
        send_list_line(s, buffer);
        free(buffer);
    }
}

/*
void s_list_plain(int s) {
  struct Job *p;
  char *buffer;

  / Show Queued or Running jobs /
  p = firstjob.next;
  while (p != 0) {
    if (p->state != HOLDING_CLIENT) {
      buffer = joblist_line_plain(p);
      send_list_line(s, buffer);
      free(buffer);
    }
    p = p->next;
  }

  p = first_finished_job.next;

  / Show Finished jobs /
  while (p != 0) {
    buffer = joblist_line_plain(p);
    send_list_line(s, buffer);
    free(buffer);
    p = p->next;
  }
}
*/

static struct Job *newjobptr() {
    struct Job *p = (struct Job *)calloc(sizeof(struct Job), sizeof(char));
    if (p) vec_push(&active_jobs, p);
    return p;
}

/* Returns -1 if no last job id found */
static int find_last_jobid_in_queue(int neglect_jobid) {
    int last_jobid = -1;
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->jobid != neglect_jobid && p->jobid > last_jobid)
            last_jobid = p->jobid;
    }
    return last_jobid;
}

/* Returns -1 if no last job id found */
static int find_last_stored_jobid_finished() {
    int last_jobid = -1;
    size_t n = vec_size(&finished_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        if (p->jobid > last_jobid) last_jobid = p->jobid;
    }
    return last_jobid;
}

/* Returns job id or -1 on error */
int s_newjob(int s, struct Msg *m, int ts_UID) {
    s_update_slots_usage();

    struct Job *p = NULL;
    int res;
    // int waitjob_flag = 0; // 0 for newjob, 1 for WAIT and 2 for DELINK
    if (m->jobid != 0) {
        p = findjob(m->jobid);
        // if p == NULL => Manual Relink
        if (p != NULL) {
            // WAIT for restore queued tasks
            if (p->state == DELINK) {
                ; // waitjob_flag = 2;
            } else if (p->state == WAIT) {
                // jobDB_wait_num--;
                ; // waitjob_flag = 1;
            } else if (p->state == LOCKED) {
                // jobDB_wait_num--;
                ; // waitjob_flag = 1;
            } else {
                return -1;
            }
        }
    }

    if (p == NULL) {
        p = newjobptr();
        if (m->jobid != 0) {
            p->jobid = m->jobid;
            jobids = jobids > m->jobid ? jobids : m->jobid + 1;
        } else {
            p->jobid = jobids++;
        }
        if (count_not_finished_jobs() < max_jobs) {
            p->state = QUEUED;
        } else {
            p->state = HOLDING_CLIENT;
        }

        // manually relink
        if (m->u.newjob.taskpid != 0) {
            p->state = RELINK;
            printf("relink to pid: %d\n", m->u.newjob.taskpid);
        }
    }
    // save the ts_UID and record the number of waiting jobs
    p->ts_UID = ts_UID; // get_tsUID(m->uid);
    p->client_socket = s;
    p->num_slots = m->u.newjob.num_slots;
    p->store_output = m->u.newjob.store_output;
    p->should_keep_finished = m->u.newjob.should_keep_finished;
    p->notify_errorlevel_to = 0;
    p->notify_errorlevel_to_size = 0;
    p->depend_on_size = m->u.newjob.depend_on_size;
    p->depend_on = 0;

    if (p->state != DELINK) {
        p->wall_time = get_max_wall_time();
        // printf("wall time = %d\n", m->u.newjob.wall_time);
        if (p->wall_time > m->u.newjob.wall_time) {
            p->wall_time = m->u.newjob.wall_time;
        }
    }
    /* this error level here is used internally to decide whether a job should
     * be run or not so it only matters whether the error level is 0 or not.
     * thus, summing the absolute error levels of all dependencies is
     * sufficient.*/
    p->dependency_errorlevel = 0;
    if (m->u.newjob.depend_on_size) {
        int *depend_on;
        int foo;
        depend_on = recv_ints(s, &foo);
        assert(p->depend_on_size == foo);

        /* Depend on the last queued job. */
        int idx = 0;
        for (int i = 0; i < p->depend_on_size; i++) {
            /* filter out dependencies that are current jobs */
            if (depend_on[i] >= p->jobid) {
                continue;
            }

            p->depend_on =
                (int *)realloc(p->depend_on, (idx + 1) * sizeof(int));
            /* As we already have 'p' in the queue,
             * neglect it during the find_last_jobid_in_queue() */
            if (depend_on[i] == -1) {
                p->depend_on[idx] = find_last_jobid_in_queue(p->jobid);

                /* We don't trust the last jobid in the queue (running or
                 * queued) if it's not the last added job. In that case, let the
                 * next control flow handle it as if it could not do_depend on
                 * any still queued job. */
                if (last_finished_jobid > p->depend_on[idx]) {
                    p->depend_on[idx] = -1;
                }

                /* If it's queued still without result, let it know
                 * its result to p when it finishes. */
                if (p->depend_on[idx] != -1) {
                    struct Job *depended_job;
                    depended_job = findjob(p->depend_on[idx]);
                    if (depended_job != 0) {
                        add_notify_errorlevel_to(depended_job, p->jobid);
                    } else {
                        warning("The jobid %i is queued to do_depend on the "
                                "jobid %i"
                                " suddenly non existent in the queue",
                                p->jobid, p->depend_on[idx]);
                    }
                } else /* Otherwise take the finished job, or the
                          last_errorlevel */
                {
                    if (depend_on[i] == -1) {
                        int ljobid = find_last_stored_jobid_finished();
                        p->depend_on[idx] = ljobid;

                        /* If we have a newer result stored, use it */
                        /* NOTE:
                         *   Reading this now, I don't know how ljobid can be
                         *   greater than last_finished_jobid */
                        if (last_finished_jobid < ljobid) {
                            struct Job *parent;
                            parent = find_finished_job(ljobid);
                            if (!parent) {
                                error("jobid %i suddenly disappeared from the "
                                      "finished list",
                                      ljobid);
                            }
                            p->dependency_errorlevel +=
                                abs(parent->result.errorlevel);
                        } else {
                            p->dependency_errorlevel += abs(last_errorlevel);
                        }
                    }
                }
            } else {
                /* The user decided what's the job this new job depends on */
                struct Job *depended_job;
                p->depend_on[idx] = depend_on[i];
                depended_job = findjob(p->depend_on[idx]);

                if (depended_job != 0) {
                    add_notify_errorlevel_to(depended_job, p->jobid);
                } else {
                    struct Job *parent;
                    parent = find_finished_job(p->depend_on[idx]);
                    if (parent) {
                        p->dependency_errorlevel +=
                            abs(parent->result.errorlevel);
                    } else {
                        /* We consider as if the job not found
                           didn't finish well */
                        p->dependency_errorlevel += 1;
                    }
                }
            }
            idx++;
        }
        free(depend_on);
        p->depend_on_size = idx;
    }

    /* if dependency list is empty after removing invalid dependencies, make it
     * independent */
    if (p->depend_on_size == 0) {
        p->depend_on = 0;
    }

    if (p->state != DELINK && p->state != WAIT && p->state != LOCKED) {
        pinfo_init(&p->info);
        pinfo_set_enqueue_time(&p->info);
    }

    /* load the command */

    char *buff = malloc(m->u.newjob.command_size);
    if (buff == 0) {
        error("Cannot allocate memory in s_newjob command_size (%i)",
              m->u.newjob.command_size);
    }
    res = recv_bytes(s, buff, m->u.newjob.command_size);
    if (res == -1) {
        error("wrong bytes received");
    }

    p->command = buff;
    p->command_strip = m->u.newjob.command_size_strip;

    /* load the work dir */
    p->work_dir = 0;
    if (m->u.newjob.path_size > 0) {
        char *ptr;
        ptr = (char *)malloc(m->u.newjob.path_size);
        if (ptr == 0) {
            error("Cannot allocate memory in s_newjob path_size(%i)",
                  m->u.newjob.path_size);
        }
        res = recv_bytes(s, ptr, m->u.newjob.path_size);
        if (res == -1) {
            error("wrong bytes received");
        }
        p->work_dir = ptr;
    }

    /* load the label */
    p->label = NULL;
    if (m->u.newjob.label_size > 0) {
        char *ptr;
        ptr = (char *)malloc(m->u.newjob.label_size);
        if (ptr == 0) {
            error("Cannot allocate memory in s_newjob label_size(%i)",
                  m->u.newjob.label_size);
        }
        res = recv_bytes(s, ptr, m->u.newjob.label_size);
        if (res == -1) {
            error("wrong bytes received");
        }
        p->label = ptr;
    }

    p->email = NULL;
    if (m->u.newjob.email_size > 0) {
        char *ptr;
        ptr = (char *)malloc(m->u.newjob.email_size);
        if (ptr == 0) {
            error("Cannot allocate memory in s_newjob email_size(%i)",
                  m->u.newjob.email_size);
        }
        res = recv_bytes(s, ptr, m->u.newjob.email_size);
        if (res == -1) {
            error("wrong bytes received");
        }
        p->email = ptr;
    }

    /* load the info */
    if (m->u.newjob.env_size > 0) {
        char *ptr;
        ptr = (char *)malloc(m->u.newjob.env_size);
        if (ptr == 0) {
            error("Cannot allocate memory in s_newjob env_size(%i)",
                  m->u.newjob.env_size);
        }
        res = recv_bytes(s, ptr, m->u.newjob.env_size);
        if (res == -1) {
            error("wrong bytes received");
        }
        pinfo_addinfo(&p->info, m->u.newjob.env_size + 100, "Environment:\n%s",
                      ptr);
        free(ptr);
    }

    if (p->state == DELINK) {
        p->state = RELINK;
        // manually insert
    } else if (p->state == WAIT) {
        p->state = QUEUED;
        user_queue[p->ts_UID]++;
    } else if (p->state == RELINK) {
        /* for manually relink running task */
        p->pid = m->u.newjob.taskpid;
        p->info.start_time = m->u.newjob.start_time;
        insert_or_replace_DB(p, "Jobs");
    } else if (p->state == QUEUED) {
        insert_DB(p, "Jobs");
        user_queue[p->ts_UID]++;
    } else if (p->state == LOCKED) {
        ;
    } else {
        insert_DB(p, "Jobs");
        user_queue[p->ts_UID]++;
    }

    set_jobids_DB(jobids);
    return p->jobid;
}

/* This assumes the jobid exists */
void s_delete_job(int jobid) {
    int idx = findjob_idx(jobid);
    if (idx < 0)
        error("Job to be removed not found. jobid=%i", jobid);

    struct Job *p = (struct Job *)vec_remove(&active_jobs, (size_t)idx);
    destroy_job(p);
}

/* -1 if no one should be run. */
/*
    next_run_job()
    in `server.c`
    s_mark_job_running(newjob);
    s_runjob(newjob, conn);
*/
static void wake_all_held_clients(void) {
    int awaken_job;
    while ((awaken_job = wake_hold_client()) != -1) {
        struct Job *p = findjob(awaken_job);
        if (p) s_send_newjob_ok(p->client_socket, awaken_job);
    }
}

int next_run_job(void) {
    static int last_uid = 0;
    int dispatched = 0;

    while (1) {
        size_t n = vec_size(&active_jobs);
        if (n == 0) break;

        /* RELINK jobs get priority — dispatch one, then re-check */
        int got_relink = 0;
        for (size_t i = 0; i < n; i++) {
            struct Job *p = (struct Job *)vec_get(&active_jobs, i);
            if (p->state == RELINK) {
                s_mark_job_running(p->jobid);
                s_send_runjob(p->client_socket, p->jobid);
                got_relink = 1;
                dispatched++;
                break;
            }
        }
        if (got_relink) {
            wake_all_held_clients();
            continue;
        }

        /* ---- Round-robin: one job per user per round (QUEUED or PAUSE-timeout) ---- */
        int found = 0;
        for (int i = 0; i < user_number; i++) {
            int uid = (last_uid + i) % user_number;
            if (user_queue[uid] == 0) continue;

            int free_slots = max_slots - busy_slots;
            if (free_slots <= 0) break;

            for (size_t j = 0; j < n; j++) {
                struct Job *p = (struct Job *)vec_get(&active_jobs, j);
                if (p->ts_UID != uid) continue;

                if (p->state == QUEUED) {
                    if (p->depend_on_size) {
                        int ready = 1;
                        for (int k = 0; k < p->depend_on_size; k++) {
                            struct Job *dep = get_job(p->depend_on[k]);
                            if (dep != NULL &&
                                (dep->state == QUEUED || dep->state == RUNNING)) {
                                ready = 0;
                                break;
                            }
                        }
                        if (!ready) continue;
                    }

                    if (free_slots < p->num_slots) continue;
                    if (user_max_slots[uid] - user_busy[uid] < p->num_slots) continue;

                    user_queue[uid]--;
                    s_mark_job_running(p->jobid);
                    s_send_runjob(p->client_socket, p->jobid);
                    last_uid = (uid + 1) % user_number;
                    found = 1;
                    dispatched++;
                    break;
                }

                if (p->state == PAUSE && p->wall_time < 0) {
                    if (i64abs(p->wall_time) <= get_cpu_time_by_pid(p->pid)) continue;
                    if (free_slots < p->num_slots) continue;
                    if (user_max_slots[uid] - user_busy[uid] < p->num_slots) continue;

                    config_running(p);
                    s_send_runjob(p->client_socket, p->jobid);
                    last_uid = (uid + 1) % user_number;
                    found = 1;
                    dispatched++;
                    break;
                }
            }
        }

        if (!found) break;
        wake_all_held_clients();
    }

    return dispatched;
}

/* Returns 1000 if no limit, The limit otherwise. */
static int get_max_finished_jobs() {
    char *limit;

    limit = getenv("TS_MAXFINISHED");
    if (limit == NULL) {
        return DEFAULT_MAXFINISHED;
    }
    int num = abs(atoi(limit));
    if (num < 1) {
        num = DEFAULT_MAXFINISHED;
    }
    return num;
}

/* Add the job to the finished queue. */
static void new_finished_job(struct Job *j) {
    int max = get_max_finished_jobs();

    /* If too many jobs, wipe out the first (oldest) */
    if ((int)vec_size(&finished_jobs) >= max) {
        struct Job *old = (struct Job *)vec_remove(&finished_jobs, 0);
        destroy_job(old);
    }
    vec_push(&finished_jobs, j);

    int err = insert_DB(j, "Finished");
    if (err == 0) {
        delete_DB(j->jobid, "Jobs");
        cgroups_clean_job(j);
    }
    sound_notify(j);
    send_mail_via_ssmtp(j);
}

static int job_is_in_state(int jobid, enum Jobstate state) {
    struct Job *p;

    p = findjob(jobid);
    if (p == 0) {
        return 0;
    }
    if (p->state == state) {
        return 1;
    }
    return 0;
}

int job_is_running(int jobid) {
    return job_is_in_state(jobid, RUNNING);
}

int job_is_holding_client(int jobid) {
    return job_is_in_state(jobid, HOLDING_CLIENT);
}

static int in_notify_list(int jobid) {
    struct Notify *n, *tmp;

    n = first_notify;
    while (n != 0) {
        tmp = n;
        n = n->next;
        if (tmp->jobid == jobid) {
            return 1;
        }
    }
    return 0;
}

/* job_finished from running to jobid */
void job_finished(const struct Result *result, int jobid) {
    if (busy_slots < 0) {
        error("Wrong state in the server. busy_slots = %i instead of greater "
              "than 0",
              busy_slots);
    }

    int idx = findjob_idx(jobid);
    if (idx < 0) error("on jobid %i finished, it doesn't exist", jobid);

    struct Job *p = (struct Job *)vec_get(&active_jobs, (size_t)idx);

    if (p->num_allocated != 0) free_cores(p);

    p->state = result->skipped ? SKIPPED : FINISHED;
    p->result = *result;
    last_finished_jobid = p->jobid;
    notify_errorlevel(p);

    pinfo_set_end_time(&p->info);
    if (result->real_sec == 0)
        p->info.start_time = p->info.enqueue_time = p->info.end_time;

    if (p->result.died_by_signal) {
        pinfo_addinfo(&p->info, 100, "Exit status: killed by signal %i\n",
                      p->result.signal);
    } else {
        pinfo_addinfo(&p->info, 100, "Exit status: died with exit code %i\n",
                      p->result.errorlevel);
    }

    /* Remove from active, optionally add to finished */
    vec_remove(&active_jobs, (size_t)idx);

    if (p->should_keep_finished || in_notify_list(p->jobid))
        new_finished_job(p);
}

static int fork_cmd(int const UID, char const *path, char const *cmd) {
    int pid = -1;             // 定义一个进程ID变量

    pid = fork();             // 调用fork()函数创建子进程
    if (pid < 0)              // 如果返回值小于0，表示fork失败
    {
        perror("fork error"); // 打印错误信息
        return -1;
    } else if (pid == 0) // 如果返回值等于0，表示子进程正在运行
    {
        setuid(UID);
        if (path != NULL) {
            chdir(path);
        }
        system(cmd);
        exit(EXIT_SUCCESS);
        /*
        int cmd_array_size;
        printf("cmd = %s\n", cmd);
        char** cmd_arry = split_str(cmd, &cmd_array_size);
        if (cmd_array_size > 0) {
          printf("run cmd %s\n", cmd_arry[0]);
          system(cmd);
          exit(EXIT_SUCCESS);
          // execvp(cmd_arry[0], cmd_arry);
        }
        // execlp("ls", "-l", NULL); //执行ls -l命令，替换当前进程
        */
        return -1;
    } else // 如果返回值大于0，表示父进程正在运行
    {
        printf("[Child PID:%d] Add queued job: %s\n", pid,
               cmd); // 打印子进程的ID
    }
    return pid;
}

static void s_add_job(struct Job *j) {
    if (j->state == RUNNING) {
        if (j->pid > 0 && s_check_running_pid(j->pid) == 1) {
            j->state = DELINK;
            vec_push(&active_jobs, j);

            char c[64];
            sprintf(c, " --relink %d -J %d ", j->pid, j->jobid);
            char *str = insert_chars_check(j->command_strip, j->command, c);

            fork_cmd(user_UID[j->ts_UID], j->work_dir, str);
            free(str);
            jobids = jobids > j->jobid ? jobids : j->jobid + 1;
            return;
        } else {
            delete_DB(j->jobid, "Jobs");
        }
    } else if (j->state == QUEUED || j->state == LOCKED) {
        printf("add the queue job %d\n", j->jobid);
        if (j->state == QUEUED) j->state = WAIT;

        vec_push(&active_jobs, j);

        char c[32];
        sprintf(c, " -J %d ", j->jobid);
        char *str = insert_chars_check(j->command_strip, j->command, c);

        fork_cmd(user_UID[j->ts_UID], j->work_dir, str);
        jobids = jobids > j->jobid ? jobids : j->jobid + 1;
        free(str);
        return;
    }

    destroy_job(j);
}

void s_read_sqlite() {
    int num_jobs, *jobs_DB = NULL;
    struct Job *job;
    num_jobs = read_jobid_DB(&(jobs_DB), "Jobs");
    printf("Jobs: #%d\n", num_jobs);
    for (int i = 0; i < num_jobs; i++) {
        job = read_DB(jobs_DB[i], "Jobs");
        if (job == NULL) {
            printf("Error in reading DB %d\n", jobs_DB[i]);
        } else {
            s_add_job(job);
        }
    }

    // finished jobs
    num_jobs = read_jobid_DB(&(jobs_DB), "Finished");
    printf("Finished:\n");
    for (int i = 0; i < num_jobs; i++) {
        job = read_DB(jobs_DB[i], "Finished");
        if (job == NULL) {
            printf("Error in reading DB %d\n", jobs_DB[i]);
        } else {
            printf("add job: %d from %d\n", job->jobid, jobs_DB[i]);
            vec_push(&finished_jobs, job);
        }
    }
    free(jobs_DB);
    set_jobids_DB(jobids);
}

void s_clear_finished(int ts_UID) {
    size_t n = vec_size(&finished_jobs);
    /* Iterate backwards so removals don't shift unvisited indices */
    for (size_t i = n; i > 0; i--) {
        size_t idx = i - 1;
        struct Job *p = (struct Job *)vec_get(&finished_jobs, idx);
        if (p->ts_UID == ts_UID || ts_UID == 0) {
            delete_DB(p->jobid, "Finished");
            vec_remove(&finished_jobs, idx);
            destroy_job(p);
        }
    }
}

void s_check_holdon() {
    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->pid != 0 && p->state == PAUSE) {
            if (is_sleep(p) == 0) cgroups_freeze_job(p);
        }
    }
}

// run the jobs
void s_process_runjob_ok(int jobid, char *oname, int pid) {
    struct Job *p = findjob(jobid);
    if (p == NULL) {
        error("Job %i already run not found on runjob_ok", jobid);
    }
    if (p->state == PAUSE) {
        return;
    }
    if (p->state != RUNNING) {
        error("Job %i not running, but %i on runjob_ok", jobid, p->state);
    }

    p->pid = pid;
    cgroups_create_job(p);
    if (oname != NULL && strlen(oname) != 0) {
        p->output_filename = oname;
    }
    pinfo_set_start_time_check(&p->info);
    if (pid > 0) {
        // printf("s_process_runjob_ok = %d\n", jobid);
        write_logfile(p);
        // if (p->state == PAUSE) {
        //  config_running(p);
        // }
        insert_or_replace_DB(p, "Jobs");
    }
}

void s_send_runjob(int s, int jobid) {
    struct Msg m = default_msg();
    struct Job *p;

    p = findjob(jobid);
    if (p == 0) {
        error("Job %i was expected to run", jobid);
    }

    m.type = RUNJOB;

    /* TODO
     * We should make the dependencies update the jobids they're do_depending
     * on. Then, on finish, these could set the errorlevel to send to its
     * dependency childs. We cannot consider that the jobs will leave traces in
     * the finished job list
     * (-nf?) . */

    m.u.last_errorlevel = p->dependency_errorlevel;
    m.jobid = jobid;
    send_msg(s, &m);
}

void s_job_info(int s, int jobid) {
    struct Job *p = 0;
    struct Msg m = default_msg();

    if (jobid == -1) {
        if (busy_slots > 0) {
            if (vec_size(&active_jobs) == 0)
                error("Internal state WAITING, but no active job.");
            p = (struct Job *)vec_get(&active_jobs, 0);
        } else {
            size_t fn = vec_size(&finished_jobs);
            if (fn == 0) {
                send_list_line(s, "No jobs.\n");
                return;
            }
            p = (struct Job *)vec_get(&finished_jobs, fn - 1);
        }
    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        snprintf(buff, 255,
                 "[s_job_info] Job %i not finished or not running.\n",
                 jobid);
        send_list_line(s, buff);
        return;
    }

    m.type = INFO_DATA;

    send_msg(s, &m);
    pinfo_dump(&p->info, s);
    fd_nprintf(s, 100, "Command: ");
    if (p->depend_on) {
        fd_nprintf(s, 100, "[%i,", p->depend_on[0]);
        for (int i = 1; i < p->depend_on_size; i++) {
            fd_nprintf(s, 100, ",%i", p->depend_on[i]);
        }
        fd_nprintf(s, 100, "]&& ");
    }
    char const *status = "";
    if (p->state != FINISHED) {
        if (p->state != PAUSE && is_sleep(p) == 1) {
            status = " in SLEEP!";
        }
    }
    
    write(s, p->command + p->command_strip,
          strlen(p->command + p->command_strip));
    fd_nprintf(s, 100, "\n");
    fd_nprintf(s, 100, "User: %s [%d]\n", user_name[p->ts_UID],
               user_UID[p->ts_UID]);
    fd_nprintf(s, 100, "State: %9s PID: %-6d%s\n", jstate2string(p->state),
               p->pid, status);

    fd_nprintf(s, 100, "Slots: %-3d\n", p->num_slots);
    if (p->output_filename != NULL) {
        int slen = strlen(p->output_filename) + 30;
        fd_nprintf(s, slen, "Ouput: %s\n", p->output_filename);
    } else {
        int slen = strlen(p->work_dir) + 30;
        fd_nprintf(s, slen, "Workdir: %s\n", p->work_dir);
    }
    if (p->email) {
        fd_nprintf(s, 100, "Email: %s\n", p->email);
    }
    // calc time-stamp
    time_t g_boot_wallclock = time(NULL) - get_monotonic_sec();
    time_t ct = p->info.enqueue_time + g_boot_wallclock;
    fd_nprintf(s, 100, "Enqueue time: %s", ctime(&ct));
    ct = p->info.start_time + g_boot_wallclock;
    fd_nprintf(s, 100, "Start time: %s", ctime(&ct));
    if (p->info.pause_time != 0) {
        ct = p->info.pause_time + g_boot_wallclock;
        fd_nprintf(s, 100, "Pause time: %s", ctime(&ct));
    }

    if (p->state == FINISHED) {
        ct = p->info.end_time + g_boot_wallclock;
        fd_nprintf(s, 100, "End time: %s", ctime(&ct));
    }

    time_t t_wall = i64abs(p->wall_time);
    time_repr_t r = format_time(t_wall);
    fd_nprintf(s, 100, "Wall-time: %.4f %c\n----\n", r.value, r.unit);
    time_t t_work = get_work_time_by_job(p);
    r = format_time(t_work);
    fd_nprintf(s, 100, "Work time: %.4f %c\n", r.value, r.unit);
    
    time_t t_pause = get_pause_time_by_job(p);
    time_t t_real = t_pause + t_work;
    double p_rate = (double)(t_pause) / t_real;
    // more than 5% PAUSE in total work time
    if (p_rate > 0.05 || 1) {
        r = format_time(t_pause);
        fd_nprintf(s, 100, "Pause time: %.4f %c\n", r.value, r.unit);

        r = format_time(t_real);  // totol Elapsed time
        fd_nprintf(s, 100, "Elapsed time: %.4f %c\n", r.value, r.unit);
    }

    if (p->state == FINISHED) {
        struct Result *res = &(p->result);
        fd_nprintf(s, 100, "Error: %d Signal: %d Die: %d\n", res->errorlevel,
                   res->signal, res->died_by_signal);
    }
    // fd_nprintf(s, 100, "\n");
}

void s_send_last_id(int s) {
    struct Msg m = default_msg();

    m.type = LAST_ID;
    m.jobid = jobids - 1;
    send_msg(s, &m);
}

void s_refresh_users(int s) {
    read_user_file(get_user_path());
    send_list_line(s, "refresh the list success!\n");
    s_update_slots_usage();
}

void s_suspend_user_all(int s) {
    for (int i = 1; i < user_number; i++) {
        s_suspend_user(s, i);
    }
}

void s_resume_user_all(int s) {
    for (int i = 1; i < user_number; i++) {
        s_resume_user(s, i);
    }
}

void s_resume_user(int s, int ts_UID) {
    if (ts_UID < 0 || ts_UID >= USER_MAX) return;

    user_max_slots[ts_UID] = abs(user_max_slots[ts_UID]);
    user_locked[ts_UID] = 0;

    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->ts_UID == ts_UID && p->state == PAUSE) {
            if (p->pid != 0) config_running(p);
        }
    }
    snprintf(buff, 255, "Resume user: [%04d] %-20s\n", user_UID[ts_UID],
             user_name[ts_UID]);
    send_list_line(s, buff);
}

void s_suspend_user(int s, int ts_UID) {
    if (ts_UID < 0 || ts_UID >= USER_MAX) return;

    user_max_slots[ts_UID] = -abs(user_max_slots[ts_UID]);
    user_locked[ts_UID] = 1;

    size_t n = vec_size(&active_jobs);
    for (size_t i = 0; i < n; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        if (p->ts_UID == ts_UID && p->state == RUNNING) {
            if (p->pid != 0) {
                safe_pause_job(p);
                p->state = PAUSE;
            } else {
                const char *label = "(...)";
                if (p->label != NULL) label = p->label;
                snprintf(buff, 255, "Error in stop %s [%d] %s | %s\n",
                         user_name[ts_UID], p->jobid, label, p->command);
                send_list_line(s, buff);
            }
        }
    }

    snprintf(buff, 255, "Suspend user: [%04d] %-20s\n", user_UID[ts_UID],
             user_name[ts_UID]);
    send_list_line(s, buff);
    s_update_slots_usage();
}

void s_send_output(int s, int jobid) {
    struct Job *p = 0;
    struct Msg m = default_msg();

    if (jobid == -1) {
        if (busy_slots > 0) {
            if (vec_size(&active_jobs) == 0)
                error("Internal state WAITING, but no active job.");
            p = (struct Job *)vec_get(&active_jobs, 0);
        } else {
            size_t fn = vec_size(&finished_jobs);
            if (fn == 0) {
                send_list_line(s, "No jobs.\n");
                return;
            }
            p = (struct Job *)vec_get(&finished_jobs, fn - 1);
        }
    } else {
        p = get_job(jobid);
        if (p != 0 && p->state != RUNNING && p->state != FINISHED &&
            p->state != SKIPPED)
            p = 0;
    }

    if (p == 0) {
        if (jobid == -1) {
            snprintf(buff, 255,
                     "The last job has not finished or is not running.\n");
        } else {
            snprintf(buff, 255,
                     "[s_send_output] Job %i not finished or not running.\n",
                     jobid);
        }
        send_list_line(s, buff);
        return;
    }

    if (p->state == SKIPPED) {
        if (jobid == -1) {
            snprintf(buff, 255,
                     "The last job was skipped due to a dependency.\n");
        }

        else {
            snprintf(buff, 255, "Job %i was skipped due to a dependency.\n",
                     jobid);
        }
        send_list_line(s, buff);
        return;
    }

    m.type = ANSWER_OUTPUT;
    m.u.output.store_output = p->store_output;
    m.u.output.pid = p->pid;
    if (m.u.output.store_output && p->output_filename) {
        m.u.output.ofilename_size = strlen(p->output_filename) + 1;
    } else {
        m.u.output.ofilename_size = 0;
    }
    send_msg(s, &m);
    if (m.u.output.ofilename_size > 0) {
        send_bytes(s, p->output_filename, m.u.output.ofilename_size);
    }
}

void notify_errorlevel(struct Job *p) {
    int i;

    last_errorlevel = p->result.errorlevel;

    for (i = 0; i < p->notify_errorlevel_to_size; ++i) {
        struct Job *notified;
        notified = get_job(p->notify_errorlevel_to[i]);
        if (notified) {
            notified->dependency_errorlevel += abs(p->result.errorlevel);
        }
    }
}

/* jobid is input/output. If the input is -1, it's changed to the jobid
 * removed */
int s_remove_job(int s, int *jobid, int client_tsUID) {
    struct Job *p = 0;
    struct Msg m = default_msg();
    int in_active = 1; /* 1 = active_jobs, 0 = finished_jobs */
    int remove_idx = -1;

    if (client_tsUID < 0 || client_tsUID > USER_MAX) {
        snprintf(buff, 255, "invalid ts_UID [%d] in job removal.\n", client_tsUID);
        send_list_line(s, buff);
        return 0;
    }

    if (*jobid == -1) {
        /* Find the last job added */
        size_t an = vec_size(&active_jobs);
        if (an > 0) {
            remove_idx = (int)(an - 1);
            p = (struct Job *)vec_get(&active_jobs, (size_t)remove_idx);
            in_active = 1;
        } else {
            size_t fn = vec_size(&finished_jobs);
            if (fn > 0) {
                remove_idx = (int)(fn - 1);
                p = (struct Job *)vec_get(&finished_jobs, (size_t)remove_idx);
                in_active = 0;
            }
        }
    } else {
        remove_idx = findjob_idx(*jobid);
        if (remove_idx >= 0) {
            p = (struct Job *)vec_get(&active_jobs, (size_t)remove_idx);
            in_active = 1;
        } else {
            remove_idx = find_finished_idx(*jobid);
            if (remove_idx >= 0) {
                p = (struct Job *)vec_get(&finished_jobs, (size_t)remove_idx);
                in_active = 0;
            }
        }
    }

    if (p != NULL && client_tsUID == 0)
        client_tsUID = p->ts_UID;

    if (p == NULL || (p->ts_UID != client_tsUID)) {
        if (*jobid == -1) {
            snprintf(buff, 255, "The last job cannot be removed.\n");
        } else {
            snprintf(buff, 255, "The job %i is not in queue.\n", *jobid);
        }
        send_list_line(s, buff);
        return 0;
    }

    if (p->state == RUNNING) {
        if (*jobid == -1)
            snprintf(buff, 255, "Running job of last job is removed.\n");
        else
            snprintf(buff, 255, "Running job [%i] PID: %d by `%s` is removed.\n",
                     *jobid, p->pid, user_name[p->ts_UID]);
        send_list_line(s, buff);
        return 0;
    }

    *jobid = p->jobid;
    delete_DB(p->jobid, "Jobs");
    p->state = FINISHED;
    p->result.errorlevel = -1;
    notify_errorlevel(p);
    check_notify_list(*jobid);

    /* Remove from the appropriate vec */
    if (in_active)
        vec_remove(&active_jobs, (size_t)remove_idx);
    else
        vec_remove(&finished_jobs, (size_t)remove_idx);

    /* PAUSE jobs were running — preserve them in the finished queue.
       QUEUED/LOCKED jobs never ran, just free them. */
    if (p->pid != 0) {
        /* Finalize pause timing so work/pause duration display is correct */
        if (p->info.pause_time != 0) {
            p->info.pause_duration += get_monotonic_sec() - p->info.pause_time;
            p->info.pause_time = 0;
        }
        pinfo_set_end_time(&p->info);
        free_cores(p);
        cgroups_clean_job(p);
        new_finished_job(p);
    } else {
        destroy_job(p);
    }

    m.type = REMOVEJOB_OK;
    send_msg(s, &m);
    return 1;
}

static void add_to_notify_list(int s, int jobid) {
    struct Notify *n;
    struct Notify *new;

    new = (struct Notify *)malloc(sizeof(*new));

    new->socket = s;
    new->jobid = jobid;
    new->next = 0;

    n = first_notify;
    if (n == 0) {
        first_notify = new;
        return;
    }

    while (n->next != 0) {
        n = n->next;
    }

    n->next = new;
}

static void send_waitjob_ok(int s, int errorlevel) {
    struct Msg m = default_msg();
    m.type = WAITJOB_OK;
    m.u.result.errorlevel = errorlevel;
    send_msg(s, &m);
}

void s_send_newjob_ok(int socket, int jobid) {
    struct Msg m = default_msg();
    m.type = NEWJOB_OK;
    m.jobid = jobid;
    send_msg(socket, &m);
}

static struct Job *get_job(int jobid) {
    struct Job *j;

    j = findjob(jobid);
    if (j != NULL) {
        return j;
    }

    j = find_finished_job(jobid);

    if (j != NULL) {
        return j;
    }

    return 0;
}

int s_check_locker(int ts_UID) {
    time_t dt = get_monotonic_sec() - locker_time;
    int res;
    if (user_locker != 0 && dt > 30) {
        user_locker = -1;
    }
    if (user_locker == -1) {
        res = 0; // unlocked
    } else {
        if (user_locker == ts_UID) {
            res = 0; // unlocked 
        } else {
            res = 1; // The service is locker by other user or root
        }
    }
    return res;
}

void s_lock_server(int s, int ts_UID) {
    if (ts_UID == 0) {
        s_update_slots_usage();
        user_locker = 0;
        locker_time = get_monotonic_sec();
        snprintf(buff, 255, "lock the task-spooler server by Root\n");
    } else {
        if (user_locker == -1) {
            user_locker = ts_UID;
            locker_time = get_monotonic_sec();
            snprintf(buff, 255, "lock the task-spooler server by [%d] `%s`\n",
                     user_UID[user_locker], user_name[ts_UID]);
        } else {
            if (user_locker == ts_UID) {
                snprintf(buff, 255,
                         "The task-spooler server has already been locked by "
                         "[%d] `%s`\n",
                         user_UID[user_locker], user_name[user_locker]);
            } else {
                snprintf(
                    buff, 255,
                    "Error: the task-spooler server has already been locked by "
                    "other user [%d] `%s`\n",
                    user_UID[user_locker], user_name[user_locker]);
            }
        }
    }
    send_list_line(s, buff);
}

void s_unlock_server(int s, int ts_UID) {
    if (user_locker == -1) {
        snprintf(buff, 255,
                 "The task-spooler server has already been unlocked\n");
    } else {
        if (ts_UID == 0) {
            user_locker = -1;
            snprintf(buff, 255, "Unlock the task-spooler server by Root\n");
        } else {
            if (user_locker == ts_UID) {
                user_locker = -1;
                snprintf(buff, 255,
                         "Unlock the task-spooler server by [%d] `%s`\n",
                         user_UID[ts_UID], user_name[ts_UID]);
            } else {
                snprintf(buff, 255,
                         "Error: the task-spooler server locked by other user "
                         "cannot "
                         "be unlocked by [%d] `%s`\n",
                         user_UID[ts_UID], user_name[ts_UID]);
            }
        }
    }
    send_list_line(s, buff);
}

static void s_lock_queue(struct Job *p) {
    if (p->state == QUEUED) {
        user_queue[p->ts_UID]--;
        p->state = LOCKED;
        set_state_DB(p->jobid, LOCKED);
    }
}

static void s_unlock_queue(struct Job *p) {
    if (p->state == LOCKED) {
        user_queue[p->ts_UID]++;
        p->state = QUEUED;
        set_state_DB(p->jobid, QUEUED);
    }
}

static int safe_pause_job(struct Job *p) {
    cgroups_freeze_job(p);
    free_cores(p);
    pause_job_config(p);
    return 0;
    /*
    kill(p->pid, SIGSTOP);
    kill_pids(p->pid, SIGSTOP, NULL);
    usleep(10000);
    if (is_sleep(p) == 1) {
        free_cores(p);
        pause_job_config(p);
        return 0;
    } else {
        kill_pids(p->pid, SIGCONT, NULL);
        return 1;
    }
    */
}

void s_hold_job(int s, int jobid, int ts_UID) {
    if (user_max_slots[ts_UID] < 0) {
        snprintf(buff, 255, "Error: The owner `%s` is locked\n",
                 user_name[ts_UID]);
        send_list_line(s, buff);
        return;
    }
    struct Job *p;
    p = findjob(jobid);
    if (p == 0) {
        snprintf(buff, 255, "Error: cannot find job [%d]\n", jobid);
        send_list_line(s, buff);
        return;
    }

    if (p->state == QUEUED) {
        if (p->ts_UID == ts_UID || ts_UID == 0) {
            snprintf(buff, 255, "The queued job [%d] is hold on.\n", jobid);
            s_lock_queue(p);
            send_list_line(s, buff);
            // set_state_DB(jobid, p->state);
            return;
        } else {
            snprintf(buff, 255, "Cannot hold on the queued job [%d].\n", jobid);
            send_list_line(s, buff);
            return;
        }
    }

    if (p->state == LOCKED) {
        snprintf(buff, 255, "The queued job [%d] is already in locked.\n",
                 jobid);
        send_list_line(s, buff);
        return;
    }

    if (p->state == PAUSE) {
        snprintf(buff, 255, "The job [%d] is already in PAUSE.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    int job_tsUID = p->ts_UID;
    if (p->pid != 0 && (job_tsUID == ts_UID || ts_UID == 0)) {
        // kill_pid(p->pid, "kill -s STOP", NULL);
        if (safe_pause_job(p) == 0) {
            p->state = PAUSE;
            snprintf(buff, 255, "To pause job [%d] successfully!\n", jobid);
        } else {
            snprintf(buff, 255,
                     "Error: cannot pause job [%d] using kill SIGSTOP\n",
                     jobid);
        }
    } else {
        snprintf(buff, 255, "Error: cannot pause job [%d]\n", jobid);
    }
    send_list_line(s, buff);
}

void s_cont_job(int s, int jobid, int ts_UID) {
    s_update_slots_usage();
    if (user_max_slots[ts_UID] < 0) {
        snprintf(buff, 255, "Error: The owner `%s` is locked\n",
                 user_name[ts_UID]);
        send_list_line(s, buff);
        return;
    }
    struct Job *p;

    p = findjob(jobid);
    if (p == 0) {
        snprintf(buff, 255, "Error: cannot find job [%d]\n", jobid);
        send_list_line(s, buff);
        return;
    }

    if (p->state == LOCKED) {
        if (p->ts_UID == ts_UID || ts_UID == 0) {
            snprintf(buff, 255, "The locked job [%d] is in queue.\n", jobid);
            s_unlock_queue(p);
            // set_state_DB(jobid, p->state);
            send_list_line(s, buff);
            return;
        } else {
            snprintf(buff, 255, "Cannot unlock the locked job [%d].\n", jobid);
            send_list_line(s, buff);
            return;
        }
    }

    if (p->state == QUEUED) {
        snprintf(buff, 255, "The job [%d] is already in queue.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    p->wall_time = i64abs(p->wall_time);
    if (p->state == RUNNING) {
        if (is_sleep(p) == 0) {
            snprintf(buff, 255, "job [%d] is already in RUNNING.\n", jobid);
        } else {
            cgroups_thaw_job(p);
            snprintf(buff, 255, "job [%d] is continued.\n", jobid);
        }
    } else {
        int job_tsUID = p->ts_UID;
        if (p->pid != 0 && (job_tsUID == ts_UID || ts_UID == 0)) {
            int num_slots = p->num_slots;
            if (user_busy[ts_UID] + num_slots <= user_max_slots[ts_UID] &&
                busy_slots + num_slots <= max_slots) {
                // PAUSE states
                if (config_running(p)) {
                    printf("Cannot set Job %i as RUNNING", p->jobid);
                }
                snprintf(buff, 255, "To rerun job [%d] successfully!\n", jobid);
            } else {
                snprintf(buff, 255, "Not enough slots for job [%d], set as time-out wait\n", jobid);
                p->wall_time = -i64abs(p->wall_time); // set as time-out wait
            }
        } else {
            snprintf(buff, 255, "Error: cannot rerun job [%d]\n", jobid);
        }
    } // p->pid
    send_list_line(s, buff);
}

/* Don't complain, if the socket doesn't exist */
void s_remove_notification(int s) {
    struct Notify *n;
    struct Notify *previous;
    n = first_notify;
    while (n != 0 && n->socket != s) {
        n = n->next;
    }
    if (n == 0 || n->socket != s) {
        return;
    }

    /* Remove the notification */
    previous = first_notify;
    if (n == previous) {
        first_notify = n->next;
        free(n);
        return;
    }

    /* if not the first... */
    while (previous->next != n) {
        previous = previous->next;
    }

    previous->next = n->next;
    free(n);
}

static void destroy_finished_job(struct Job *j) {
    int idx = find_finished_idx(j->jobid);
    if (idx < 0)
        error("Cannot destroy the expected job %i", j->jobid);
    vec_remove(&finished_jobs, (size_t)idx);
    destroy_job(j);
}

/* This is called when a job finishes */
void check_notify_list(int jobid) {
    struct Notify *n, *tmp;
    struct Job *j;

    n = first_notify;
    while (n != 0) {
        tmp = n;
        n = n->next;
        if (tmp->jobid == jobid) {
            j = get_job(jobid);
            /* If the job finishes, notify the waiter */
            if (j->state == FINISHED || j->state == SKIPPED) {
                send_waitjob_ok(tmp->socket, j->result.errorlevel);
                /* We want to get the next Nofity* before we remove
                 * the actual 'n'. As s_remove_notification() simply
                 * removes the element from the linked list, we can
                 * safely follow on the list from n->next. */
                s_remove_notification(tmp->socket);

                /* Remove the jobs that were temporarily in the finished list,
                 * just for their notifiers. */
                if (!in_notify_list(jobid) && !j->should_keep_finished) {
                    destroy_finished_job(j);
                }
            }
        }
    }
}

void s_wait_job(int s, int jobid) {
    struct Job *p = 0;

    if (jobid == -1) {
        size_t an = vec_size(&active_jobs);
        if (an > 0) {
            p = (struct Job *)vec_get(&active_jobs, an - 1);
        } else {
            size_t fn = vec_size(&finished_jobs);
            if (fn > 0) p = (struct Job *)vec_get(&finished_jobs, fn - 1);
        }
    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        snprintf(buff, 255, "The job %i cannot be waited.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    if (p->state == FINISHED || p->state == SKIPPED)
        send_waitjob_ok(s, p->result.errorlevel);
    else
        add_to_notify_list(s, p->jobid);
}

void s_wait_running_job(int s, int jobid) {
    struct Job *p = 0;

    if (jobid == -1) {
        if (busy_slots > 0) {
            if (vec_size(&active_jobs) == 0)
                error("Internal state WAITING, but no active job.");
            p = (struct Job *)vec_get(&active_jobs, 0);
        } else {
            size_t fn = vec_size(&finished_jobs);
            if (fn == 0) {
                send_list_line(s, "No jobs.\n");
                return;
            }
            p = (struct Job *)vec_get(&finished_jobs, fn - 1);
        }
    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        snprintf(buff, 255, "The job %i cannot be waited.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    if (p->state == FINISHED || p->state == SKIPPED)
        send_waitjob_ok(s, p->result.errorlevel);
    else
        add_to_notify_list(s, p->jobid);
}

void s_set_max_slots(int s, int new_max_slots) {
    if (new_max_slots > 0) {
        max_slots = new_max_slots;
    } else {
        warning("Received new_max_slots=%i", new_max_slots);
    }
    if (s > 0) {
        snprintf(buff, 255, "Reset the number of slots: %d\n", max_slots);
        send_list_line(s, buff);
    }
}

void s_get_max_slots(int s) {
    struct Msg m = default_msg();

    /* Message */
    m.type = GET_MAX_SLOTS_OK;
    m.u.max_slots = max_slots;

    send_msg(s, &m);
}

/* move jobid upto the top of list */
void s_move_urgent(int s, int jobid) {
    struct Job *p = 0;

    if (jobid == -1) {
        size_t an = vec_size(&active_jobs);
        if (an > 0) p = (struct Job *)vec_get(&active_jobs, an - 1);
    } else {
        int idx = findjob_idx(jobid);
        if (idx >= 0) p = (struct Job *)vec_get(&active_jobs, (size_t)idx);
    }

    if (p == 0 || vec_size(&active_jobs) == 0) {
        snprintf(buff, 255, "The job %i cannot be urged.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    /* Move to front of vec */
    int idx = findjob_idx(jobid);
    if (idx < 0) {
        snprintf(buff, 255, "The job %i cannot be urged.\n", jobid);
        send_list_line(s, buff);
        return;
    }
    struct Job *p_moved = (struct Job *)vec_remove(&active_jobs, (size_t)idx);
    vec_insert(&active_jobs, 0, p_moved);
    movetop_DB(jobid);
    send_urgent_ok(s);
}

void s_swap_jobs(int s, int jobid1, int jobid2) {
    int idx1 = findjob_idx(jobid1);
    int idx2 = findjob_idx(jobid2);

    if (idx1 < 0 || idx2 < 0) {
        snprintf(buff, 255, "The jobs %i and %i cannot be swapped.\n", jobid1, jobid2);
        send_list_line(s, buff);
        return;
    }

    struct Job *p1 = (struct Job *)vec_get(&active_jobs, (size_t)idx1);
    struct Job *p2 = (struct Job *)vec_get(&active_jobs, (size_t)idx2);
    vec_set(&active_jobs, (size_t)idx1, p2);
    vec_set(&active_jobs, (size_t)idx2, p1);
    swap_DB(jobid1, jobid2);
    send_swap_jobs_ok(s);
}

static void send_state(int s, enum Jobstate state) {
    struct Msg m = default_msg();

    m.type = ANSWER_STATE;
    m.u.state = state;

    send_msg(s, &m);
}

void s_send_state(int s, int jobid) {
    struct Job *p = 0;

    if (jobid == -1) {
        size_t an = vec_size(&active_jobs);
        if (an > 0) {
            p = (struct Job *)vec_get(&active_jobs, an - 1);
        } else {
            size_t fn = vec_size(&finished_jobs);
            if (fn > 0) p = (struct Job *)vec_get(&finished_jobs, fn - 1);
        }
    } else {
        p = get_job(jobid);
    }

    if (p == 0) {
        snprintf(buff, 255, "The job %i cannot be stated.\n", jobid);
        send_list_line(s, buff);
        return;
    }

    send_state(s, p->state);
}

static void dump_job_struct(FILE *out, const struct Job *p) {
    fprintf(out, "  new_job\n");
    fprintf(out, "    jobid %i\n", p->jobid);
    fprintf(out, "    command \"%s\"\n", p->command);
    fprintf(out, "    state %s\n", jstate2string(p->state));
    fprintf(out, "    result.errorlevel %i\n", p->result.errorlevel);
    fprintf(out, "    output_filename \"%s\"\n",
            p->output_filename ? p->output_filename : "NULL");
    fprintf(out, "    store_output %i\n", p->store_output);
    fprintf(out, "    pid %i\n", p->pid);
    fprintf(out, "    should_keep_finished %i\n", p->should_keep_finished);
}

void dump_jobs_struct(FILE *out) {
    size_t an = vec_size(&active_jobs);
    size_t fn = vec_size(&finished_jobs);

    fprintf(out, "New_jobs\n");

    for (size_t i = 0; i < an; i++)
        dump_job_struct(out, (const struct Job *)vec_get(&active_jobs, i));
    for (size_t i = 0; i < fn; i++)
        dump_job_struct(out, (const struct Job *)vec_get(&finished_jobs, i));
}

static void dump_notify_struct(FILE *out, const struct Notify *n) {
    fprintf(out, "  notify\n");
    fprintf(out, "    jobid %i\n", n->jobid);
    fprintf(out, "    socket \"%i\"\n", n->socket);
}

void dump_notifies_struct(FILE *out) {
    const struct Notify *n;

    fprintf(out, "New_notifies\n");

    n = first_notify;
    while (n != 0) {
        dump_notify_struct(out, n);
        n = n->next;
    }
}

void joblist_dump(int fd) {
    char *buffer;

    buffer = joblistdump_headers();
    write(fd, buffer, strlen(buffer));
    free(buffer);

    buffer = joblist_headers();
    write(fd, "# ", 2);
    write(fd, buffer, strlen(buffer));

    /* Show Finished jobs */
    size_t fn = vec_size(&finished_jobs);
    for (size_t i = 0; i < fn; i++) {
        struct Job *p = (struct Job *)vec_get(&finished_jobs, i);
        buffer = joblist_line(p);
        write(fd, "# ", 2);
        write(fd, buffer, strlen(buffer));
        free(buffer);
    }

    write(fd, "\n", 1);

    /* Show Queued or Running jobs */
    size_t an = vec_size(&active_jobs);
    for (size_t i = 0; i < an; i++) {
        struct Job *p = (struct Job *)vec_get(&active_jobs, i);
        buffer = joblistdump_torun(p);
        write(fd, buffer, strlen(buffer));
        free(buffer);
    }
}

void s_get_logdir(int s) {
    send_list_line(s, logdir);
}

void s_set_logdir(char const *path) {
    char *newdir = realloc(logdir, strlen(path) + 1);
    if (newdir == NULL) return;
    logdir = newdir;
    strcpy(logdir, path);
}

void s_get_env(int s, int size) {
    char *var = malloc(size);
    int res = recv_bytes(s, var, size);
    if (res != size) {
        error("Receiving environment variable name");
    }

    char *val = getenv(var);
    struct Msg m = default_msg();
    m.type = LIST_LINE;
    m.u.size = val ? strlen(val) + 1 : 0;
    send_msg(s, &m);
    if (val) {
        send_bytes(s, val, m.u.size);
    }

    free(var);
}

void s_set_env(int s, int size) {
    char *var = malloc(size);
    int res = recv_bytes(s, var, size);
    if (res != size) {
        error("Receiving environment variable name");
    }

    /* get the var name */
    char *name = strtok(var, "=");

    /* get the var value */
    char *val = strtok(NULL, "=");
    setenv(name, val, 1);
    free(var);
}

void s_unset_env(int s, int size) {
    char *var = malloc(size);
    int res = recv_bytes(s, var, size);
    if (res != size) {
        error("Receiving environment variable name");
    }

    unsetenv(var);
    free(var);
}
