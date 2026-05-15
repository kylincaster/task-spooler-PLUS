/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include "main.h"

void pinfo_init(struct Procinfo *p)
{
    p->ptr = 0;
    p->nchars = 0;
    p->allocchars = 0;
    p->start_time = 0;
    p->end_time = 0;
    p->enqueue_time = 0;
    p->pause_time = 0;
    p->pause_duration = 0;
}

void pinfo_free(struct Procinfo *p)
{
    if (p->ptr)
    {
        free(p->ptr);
    }
    p->nchars = 0;
    p->allocchars = 0;
}

void pinfo_addinfo(struct Procinfo *p, int maxsize, const char *line, ...)
{
    va_list ap;

    int newchars = p->nchars + maxsize;
    void *newptr;
    int res;

    va_start(ap, line);

    /* Ask for more memory for the array, if needed */
    if (newchars > p->allocchars)
    {
        int newmem;
        int newalloc;
        newalloc = newchars;
        newmem = newchars * sizeof(*p->ptr);
        newptr = realloc(p->ptr, newmem);
        if(newptr == 0)
        {
            warning("Cannot realloc more memory (%i) in pinfo_addline. "
                    "Not adding the content.", newmem);
            return;
        }
        p->ptr = (char *) newptr;
        p->allocchars = newalloc;
    }

    res = vsnprintf(p->ptr + p->nchars, (p->allocchars - p->nchars), line, ap);
    p->nchars += res; /* We don't store the final 0 */
}

void pinfo_dump(const struct Procinfo *p, int fd)
{
    if (p->ptr)
    {
        int res;
        int rest = p->nchars;
        while (rest > 0)
        {
            res = write(fd, p->ptr, rest);
            if (res == -1)
            {
                warning("Cannot write more chars in pinfo_dump");
                return;
            }
            rest -= res;
        }
    }
}

int pinfo_size(const struct Procinfo *p)
{
    return p->nchars;
}

void pinfo_set_enqueue_time(struct Procinfo *p)
{
    p->enqueue_time = get_monotonic_sec();
    p->start_time = 0;
    p->end_time = 0;
}

void pinfo_set_start_time_check(struct Procinfo *info) {
    if (info->start_time == 0) {
        pinfo_set_start_time(info);
    }
}
void pinfo_set_start_time(struct Procinfo *p)
{
    p->start_time = get_monotonic_sec();
    p->end_time = 0;
}

void pinfo_set_pause_time(struct Procinfo *p)
{
    if (p->pause_time == 0) {
        p->pause_time = get_monotonic_sec();
    }
}
void pinfo_set_pause_duration(struct Procinfo *p)
{
    if (p->pause_time != 0) {
        p->pause_duration += get_monotonic_sec() - p->pause_time;
    }
}

time_t pinfo_get_pause_duration(struct Procinfo *p) {
    time_t duration = p->pause_duration;
    if (p->pause_time != 0) {
        duration += get_monotonic_sec() - p->pause_time;
    }
    return duration;
}

void pinfo_set_end_time(struct Procinfo *p)
{
    p->end_time = get_monotonic_sec();
}

