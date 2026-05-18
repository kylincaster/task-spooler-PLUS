/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef JOB_H
#define JOB_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

enum Jobstate {
  QUEUED,
  RUNNING,
  PAUSE,
  FINISHED,
  SKIPPED,
  HOLDING_CLIENT,
  RELINK,
  WAIT,
  DELINK,
  LOCKED,
};

struct Result {
  int errorlevel;
  int died_by_signal;
  int signal;
  time_t user_sec;
  time_t system_sec;
  time_t real_sec;
  int skipped;
};

struct Procinfo {
  char *ptr;
  int nchars;
  int allocchars;
  time_t enqueue_time;
  time_t start_time;
  time_t end_time;
  time_t pause_time;
  time_t pause_duration;
};

struct Job {
  int jobid;
  char *command;
  char *work_dir;
  int command_strip;
  enum Jobstate state;
  struct Result result;
  char *output_filename;
  int store_output;
  pid_t pid;
  int ts_UID;
  int64_t wall_time; /* wall-time limit */
  int should_keep_finished;
  int *depend_on;
  int depend_on_size;
  int *notify_errorlevel_to;
  int notify_errorlevel_to_size;
  int dependency_errorlevel;
  char *label;
  char *email;
  struct Procinfo info;
  int num_slots;
  int num_allocated;
  int client_socket; /* socket to send RUNJOB / NEWJOB_OK back to */
};

enum ExitCodes {
  EXITCODE_OK = 0,
  EXITCODE_UNKNOWN_ERROR = -1,
  EXITCODE_QUEUE_FULL = 2,
  EXITCODE_RELINK_FAILED = 3
};

#endif /* JOB_H */
