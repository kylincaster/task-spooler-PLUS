/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.

    Server-side environment and logdir functions.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"

/* from server.c */
extern char *logdir;

void s_get_logdir(int s) {
  send_list_line(s, logdir);
}

void s_set_logdir(const char *path) {
  char *newdir = realloc(logdir, strlen(path) + 1);
  if (newdir == NULL) return;
  logdir = newdir;
  strcpy(logdir, path);
}

void s_get_env(int s, int size) {
  char *var = malloc(size);
  int res = recv_bytes(s, var, size);
  if (res != size)
    error("Receiving environment variable name");

  char *val = getenv(var);
  struct Msg m = default_msg();
  m.type = LIST_LINE;
  m.u.size = val ? strlen(val) + 1 : 0;
  send_msg(s, &m);
  if (val)
    send_bytes(s, val, m.u.size);

  free(var);
}

void s_set_env(int s, int size) {
  char *var = malloc(size);
  int res = recv_bytes(s, var, size);
  if (res != size)
    error("Receiving environment variable name");

  char *name = strtok(var, "=");
  char *val = strtok(NULL, "=");
  setenv(name, val, 1);
  free(var);
}

void s_unset_env(int s, int size) {
  char *var = malloc(size);
  int res = recv_bytes(s, var, size);
  if (res != size)
    error("Receiving environment variable name");

  unsetenv(var);
  free(var);
}
