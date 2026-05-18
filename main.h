/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Sub-headers — included for backward compatibility */
#include "job.h"
#include "msg.h"
#include "utils.h"
#include "server.h"
#include "client.h"

enum Request {
  c_QUEUE,
  c_TAIL,
  c_KILL_SERVER,
  c_LIST,
  c_LIST_ALL,
  c_DAEMON,
  c_CHECK_DAEMON,
  c_REFRESH_USER,
  c_SUSPEND_USER,
  c_RESUME_USER,
  c_LOCK_SERVER,
  c_UNLOCK_SERVER,
  c_HOLD_JOB,
  c_CONT_JOB,
  c_CLEAR_FINISHED,
  c_SHOW_HELP,
  c_SHOW_VERSION,
  c_CAT,
  c_SHOW_OUTPUT_FILE,
  c_SHOW_PID,
  c_REMOVEJOB,
  c_WAITJOB,
  c_URGENT,
  c_GET_STATE,
  c_SWAP_JOBS,
  c_INFO,
  c_SET_MAX_SLOTS,
  c_GET_MAX_SLOTS,
  c_KILL_JOB,
  c_COUNT_RUNNING,
  c_GET_LABEL,
  c_ADD_WTIME,
  c_LAST_ID,
  c_KILL_ALL,
  c_SHOW_CMD,
  c_GET_LOGDIR,
  c_SET_LOGDIR,
  c_GET_ENV,
  c_SET_ENV,
  c_UNSET_ENV
};

struct CommandLine {
  enum Request request;
  int need_server;
  int store_output;
  int stderr_apart;
  int should_go_background;
  int should_keep_finished;
  int send_output_by_mail;
  int gzip;
  int *depend_on; /* -1 means depend on previous */
  int depend_on_size;
  int max_slots;
  int jobid;
  int jobid2;
  int wait_enqueuing;
  struct {
    char **array;
    int num;
  } command;
  char *linux_cmd;
  char *label;
  char *email;
  char *logfile;
  char *outfile;
  int num_slots;
  int taskpid;
  int require_elevel;
  time_t start_time;
  int64_t wall_time;
  enum ListFormat list_format;
};

/* runtime_limit.c */
typedef struct {
  double value;
  char unit;
} time_repr_t;

enum ProcessType { CLIENT, SERVER };

/* Global variables */
extern struct CommandLine command_line;
extern enum ProcessType process_type;
extern int server_socket;
extern char *logdir;
extern int term_width;
extern int user_locker;
extern time_t locker_time;
extern int jobsort_flag;

/* main.c */
struct Msg default_msg(void);
struct Result default_result(void);

/* error.c */
void error(const char *str, ...);
void warning(const char *str, ...);
void debug(const char *str, ...);
void error_msg(const struct Msg *m, const char *str, ...);
void warning_msg(const struct Msg *m, const char *str, ...);

/* msg.c */
void send_bytes(int fd, const char *data, int bytes);
int recv_bytes(int fd, char *data, int bytes);
void send_msg(int fd, const struct Msg *m);
int recv_msg(int fd, struct Msg *m);
void send_ints(int fd, const int *data, int num);
int *recv_ints(int fd, int *num);

/* msgdump.c */
void msgdump(FILE *f, const struct Msg *m);

/* list.c */
char *joblist_headers(void);
char *joblist_line(const struct Job *p);
char *joblist_line_plain(const struct Job *p);
char *joblistdump_torun(const struct Job *p);
char *joblistdump_headers(void);

/* print.c */
int fd_nprintf(int fd, int maxsize, const char *fmt, ...);

/* info.c */
void pinfo_dump(const struct Procinfo *p, int fd);
void pinfo_addinfo(struct Procinfo *p, int maxsize, const char *line, ...);
void pinfo_free(struct Procinfo *p);
int pinfo_size(const struct Procinfo *p);
void pinfo_set_enqueue_time(struct Procinfo *p);
void pinfo_set_start_time(struct Procinfo *p);
void pinfo_set_start_time_check(struct Procinfo *info);
void pinfo_set_end_time(struct Procinfo *p);
void pinfo_init(struct Procinfo *p);

/* env.c */
char *get_environment(void);

/* tail.c */
int tail_file(const char *fname, int last_lines);

/* user.c */
static const int root_UID = 0;
const char *get_kill_sh_path(void);
void read_user_file(const char *path);
int get_tsUID(int uid);
void c_refresh_user(void);
const char *get_user_path(void);
const char *set_server_logfile(void);
void write_logfile(const struct Job *p);
int get_env(const char *env, int v0);
const char *uid2user_name(int uid);
int read_first_jobid_from_logfile(const char *path);
void kill_pids(int ppid, int signal, const char *cmd);

/* mail.c */
void send_mail(int jobid, int errorlevel, const char *ofname, const char *command);
void hook_on_finish(int jobid, int errorlevel, const char *ofname, const char *command);

/* signals.c */
void ignore_sigpipe(void);
void restore_sigmask(void);
void block_sigint(void);
void unblock_sigint_and_install_handler(void);

/* server_start.c */
int try_connect(int s);
void wait_server_up(int fd);
int ensure_server_up(int);
void notify_parent(int fd);
void create_socket_path(char **path);

/* execute.c */
int run_job(int jobid, struct Result *res);

/* runtime_limit.c */
time_repr_t format_time(time_t t);
int64_t i64abs(int64_t x);
void check_relink(int pid);
time_t get_cpu_time_by_pid(int pid);
time_t get_max_wall_time(void);
int parse_time(const char *s, time_t *out);
time_t get_work_time_by_job(const struct Job *p);
time_t get_pause_time_by_job(const struct Job *p);
time_t get_monotonic_sec(void);

/* cgroups.c */
void cgroups_create_job(const struct Job *p);
void cgroups_clean_job(const struct Job *p);
void cgroups_clean_all_finished(void);
int cgroups_thaw_job(const struct Job *p);
int cgroups_freeze_job(const struct Job *p);
int cgroups_is_frozen(const struct Job *p);

/* sqlite.c */
const char *get_sqlite_path(void);
int open_sqlite(void);
int close_sqlite(void);
int update_field_int64(const char *tableName, int jobid, const char *columnName, int64_t newValue);
int insert_DB(struct Job *job, const char *table);
int insert_or_replace_DB(struct Job *job, const char *table);
struct Job *read_DB(int jobid, const char *table);
int read_jobid_DB(int **jobids, const char *table);
int delete_DB(int jobid, const char *table);
int movetop_DB(int jobid);
int swap_DB(int jobid0, int jobid1);
int set_jobids_DB(int value);
int get_jobids_DB(void);
int set_state_DB(int jobid, int state);

#endif /* MAIN_H */
