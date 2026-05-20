/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef CGROUPS_H
#define CGROUPS_H

#include "jobs.h"

void cgroups_create_job(const struct Job *p);
void cgroups_clean_job(const struct Job *p);
void cgroups_clean_all_finished(void);
int cgroups_thaw_job(const struct Job *p);
int cgroups_freeze_job(const struct Job *p);
int cgroups_is_frozen(const struct Job *p);

int cgroups_v1_freezer_ok(int jobid, pid_t pid);
#endif /* CGROUPS_H */
