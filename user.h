/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef USER_H
#define USER_H

#include <stdint.h>

#define USER_NAME_WIDTH 256
#define USER_MAX 100
#define root_UID 0

struct Job;

extern char user_name[USER_MAX][USER_NAME_WIDTH];
extern int server_uid;
extern int user_max_slots[USER_MAX];
extern int user_UID[USER_MAX];
extern int user_busy[USER_MAX];
extern int user_jobs[USER_MAX];
extern int user_queue[USER_MAX];
extern int user_locked[USER_MAX];
extern int user_number;
extern char *logfile_path;

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

#endif /* USER_H */
