/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef SERVER_H
#define SERVER_H

#include "msg.h"

struct Client_conn {
  int socket;
  int hasjob;
  int jobid;
  int ts_UID;
};

/* server.c */
void server_main(int notify_fd, char *_path);
void dump_conns_struct(FILE *out);

/* jobs.c — core queue */
void init_jobs(void);
void destroy_jobs(void);
struct Job *findjob(int jobid);
const char *jstate2string(enum Jobstate s);
int job_is_running(int jobid);
int job_is_holding_client(int jobid);
int wake_hold_client(void);
int next_run_job(void);

void s_set_jobids(int i);
int s_newjob(int s, struct Msg *m, int ts_UID);
void s_delete_job(int jobid);
void job_finished(const struct Result *result, int jobid);
void s_mark_job_running(int jobid);
void s_clear_finished(int ts_UID);
void s_process_runjob_ok(int jobid, char *oname, int pid);
void s_send_runjob(int s, int jobid);
void s_send_newjob_ok(int socket, int jobid);
void s_set_max_slots(int s, int new_max_slots);
void s_get_max_slots(int s);
void s_kill_all_jobs(int s, int ts_UID);
void s_count_running_jobs(int s, int ts_UID);
void s_check_holdon(void);
int s_check_relink(int s, pid_t pid, int ts_UID);
int s_check_running_pid(pid_t pid);
void s_read_sqlite(void);
int s_update_slots_usage(void);
void send_list_line(int s, const char *str);
void setup_ssmtp(void);

int is_sleep(const struct Job *p);

/* notify.c — wait/notify */
void check_notify_list(int jobid);
void s_remove_notification(int s);
void s_wait_job(int s, int jobid);
void s_wait_running_job(int s, int jobid);

/* job_ops.c — job operations */
void s_list(int s, int ts_UID, enum ListFormat listFormat);
void s_list_all(int s, enum ListFormat listFormat);
void s_swap_jobs(int s, int jobid1, int jobid2);
void s_move_urgent(int s, int jobid);
void s_job_info(int s, int jobid);
void s_get_label(int s, int jobid);
void s_send_cmd(int s, int jobid);
void s_send_state(int s, int jobid);
void s_send_last_id(int s);
void s_send_output(int s, int jobid);
int s_remove_job(int s, int *jobid, int client_uid);
void s_sort_jobs(void);
void s_add_wtime(int s, int jobid, int64_t add_wtime);

/* server_user.c — user management */
void s_user_status_all(int s);
void s_user_status(int s, int i);
void s_refresh_users(int s);
int s_get_job_tsUID(int jobid);
void s_suspend_user_all(int s);
void s_suspend_user(int s, int uid);
void s_resume_user(int s, int uid);
void s_resume_user_all(int s);
void s_hold_job(int s, int jobid, int uid);
void s_cont_job(int s, int jobid, int uid);
void s_lock_server(int s, int uid);
void s_unlock_server(int s, int uid);
int s_check_locker(int uid);

/* server_env.c — env/logdir */
void s_get_logdir(int s);
void s_set_logdir(const char *path);
void s_get_env(int s, int size);
void s_set_env(int s, int size);
void s_unset_env(int s, int size);

/* jobs.c — dump/debug */
void dump_jobs_struct(FILE *out);
void dump_notifies_struct(FILE *out);
void joblist_dump(int fd);

#endif /* SERVER_H */
