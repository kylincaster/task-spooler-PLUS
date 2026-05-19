/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2013  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#ifndef SERVER_START_H
#define SERVER_START_H

int try_connect(int s);
void wait_server_up(int fd);
int ensure_server_up(int daemonFlag);
void notify_parent(int fd);
void create_socket_path(char **path);

#endif /* SERVER_START_H */
