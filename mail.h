/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef MAIL_H
#define MAIL_H

void send_mail(int jobid, int errorlevel, const char *ofname, const char *command);
void hook_on_finish(int jobid, int errorlevel, const char *ofname, const char *command);

#endif /* MAIL_H */
