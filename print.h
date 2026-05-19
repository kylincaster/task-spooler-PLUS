/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef PRINT_H
#define PRINT_H

int fd_nprintf(int fd, int maxsize, const char *fmt, ...);

#endif /* PRINT_H */
