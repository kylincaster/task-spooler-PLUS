/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef NOTIFY_H
#define NOTIFY_H

void check_notify_list(int jobid);
void s_remove_notification(int s);
void s_wait_job(int s, int jobid);
void s_wait_running_job(int s, int jobid);

#endif /* NOTIFY_H */
