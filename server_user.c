/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.

    Server-side user management: suspend, resume, lock, hold, cont.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "main.h"
#include "user.h"
#include "vec.h"

/* from jobs.c */
extern vec_t active_jobs;
extern int busy_slots;
extern int max_slots;
extern char buff[256];

/* jobs.c internals made accessible */
struct Job *get_job(int jobid);
void free_cores(struct Job *p);
int config_running(struct Job *p);
void pause_job_config(struct Job *p);
void rerun_job_config(struct Job *p);
int safe_pause_job(struct Job *p);

/* from user.c */
extern int user_locker;
extern time_t locker_time;

void s_user_status_all(int s) {
  char buffer[256];
  char *extra;
  send_list_line(s, "-- Users ----------- \n");
  for (int i = 0; i < user_number; i++) {
    extra = user_locked[i] != 0 ? "Locked" : "";
    if (user_max_slots[i] == 0 && user_busy[i] == 0)
      continue;
    snprintf(buffer, 256, "[%04d] %3d/%-4d Q:%-3d %16s Run. %2d %s\n",
             user_UID[i], user_busy[i], abs(user_max_slots[i]), user_queue[i],
             user_name[i], user_jobs[i], extra);
    send_list_line(s, buffer);
  }
  snprintf(buffer, 256, "Service at UID:%d\n", server_uid);
  send_list_line(s, buffer);
}

void s_user_status(int s, int i) {
  char buffer[256];
  char *extra = "";
  if (user_locked[i] != 0)
    extra = "Locked";
  snprintf(buffer, 256, "[%04d] %3d/%-4d Q:%-3d %16s Run. %2d %s\n",
           user_UID[i], user_busy[i], abs(user_max_slots[i]), user_queue[i],
           user_name[i], user_jobs[i], extra);
  send_list_line(s, buffer);
}

int s_get_job_tsUID(int jobid) {
  struct Job *p = get_job(jobid);
  if (p == NULL) return -1;
  return p->ts_UID;
}

void s_refresh_users(int s) {
  read_user_file(get_user_path());
  send_list_line(s, "refresh the list success!\n");
  s_update_slots_usage();
}

void s_suspend_user_all(int s) {
  for (int i = 1; i < user_number; i++)
    s_suspend_user(s, i);
}

void s_resume_user_all(int s) {
  for (int i = 1; i < user_number; i++)
    s_resume_user(s, i);
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
  snprintf(buff, 255, "Resume user: [%04d] %-20s\n", user_UID[ts_UID], user_name[ts_UID]);
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

  snprintf(buff, 255, "Suspend user: [%04d] %-20s\n", user_UID[ts_UID], user_name[ts_UID]);
  send_list_line(s, buff);
  s_update_slots_usage();
}

int s_check_locker(int ts_UID) {
  time_t dt = get_monotonic_sec() - locker_time;
  int res;
  if (user_locker != 0 && dt > 30)
    user_locker = -1;

  if (user_locker == -1)
    res = 0;
  else if (user_locker == ts_UID)
    res = 0;
  else
    res = 1;
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
                 "The task-spooler server has already been locked by [%d] `%s`\n",
                 user_UID[user_locker], user_name[user_locker]);
      } else {
        snprintf(buff, 255,
                 "Error: the task-spooler server has already been locked by other user [%d] `%s`\n",
                 user_UID[user_locker], user_name[user_locker]);
      }
    }
  }
  send_list_line(s, buff);
}

void s_unlock_server(int s, int ts_UID) {
  if (user_locker == -1) {
    snprintf(buff, 255, "The task-spooler server has already been unlocked\n");
  } else {
    if (ts_UID == 0) {
      user_locker = -1;
      snprintf(buff, 255, "Unlock the task-spooler server by Root\n");
    } else {
      if (user_locker == ts_UID) {
        user_locker = -1;
        snprintf(buff, 255, "Unlock the task-spooler server by [%d] `%s`\n",
                 user_UID[ts_UID], user_name[ts_UID]);
      } else {
        snprintf(buff, 255,
                 "Error: the task-spooler server locked by other user cannot be unlocked by [%d] `%s`\n",
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

void s_hold_job(int s, int jobid, int ts_UID) {
  if (user_max_slots[ts_UID] < 0) {
    snprintf(buff, 255, "Error: The owner `%s` is locked\n", user_name[ts_UID]);
    send_list_line(s, buff);
    return;
  }
  struct Job *p = findjob(jobid);
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
      return;
    } else {
      snprintf(buff, 255, "Cannot hold on the queued job [%d].\n", jobid);
      send_list_line(s, buff);
      return;
    }
  }

  if (p->state == LOCKED) {
    snprintf(buff, 255, "The queued job [%d] is already in locked.\n", jobid);
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
    if (safe_pause_job(p) == 0) {
      p->state = PAUSE;
      snprintf(buff, 255, "To pause job [%d] successfully!\n", jobid);
    } else {
      snprintf(buff, 255, "Error: cannot pause job [%d] using kill SIGSTOP\n", jobid);
    }
  } else {
    snprintf(buff, 255, "Error: cannot pause job [%d]\n", jobid);
  }
  send_list_line(s, buff);
}

void s_cont_job(int s, int jobid, int ts_UID) {
  s_update_slots_usage();
  if (user_max_slots[ts_UID] < 0) {
    snprintf(buff, 255, "Error: The owner `%s` is locked\n", user_name[ts_UID]);
    send_list_line(s, buff);
    return;
  }
  struct Job *p = findjob(jobid);
  if (p == 0) {
    snprintf(buff, 255, "Error: cannot find job [%d]\n", jobid);
    send_list_line(s, buff);
    return;
  }

  if (p->state == LOCKED) {
    if (p->ts_UID == ts_UID || ts_UID == 0) {
      snprintf(buff, 255, "The locked job [%d] is in queue.\n", jobid);
      s_unlock_queue(p);
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
        if (config_running(p))
          printf("Cannot set Job %i as RUNNING", p->jobid);
        snprintf(buff, 255, "To rerun job [%d] successfully!\n", jobid);
      } else {
        snprintf(buff, 255, "Not enough slots for job [%d], set as time-out wait\n", jobid);
        p->wall_time = -i64abs(p->wall_time);
      }
    } else {
      snprintf(buff, 255, "Error: cannot rerun job [%d]\n", jobid);
    }
  }
  send_list_line(s, buff);
}
