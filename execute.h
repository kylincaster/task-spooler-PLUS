/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef EXECUTE_H
#define EXECUTE_H

#include "jobs.h"

int run_job(int jobid, struct Result *res);

#endif /* EXECUTE_H */
