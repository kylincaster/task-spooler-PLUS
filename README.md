# Task Spooler PLUS

This project builds upon [Task Spooler by Lluís Batlle i Rossell](https://vicerveza.homeunix.net/~viric/soft/ts/), a software providing fundamental task management capabilities. The software has been enhanced with additional features of practical significance, including support for **multiple users, recovery from fatal crashes, and processor allocation and binding**. The objective of this project is to offer a self-contained, immediately operational task management system. In contrast to systems such as *SLURM* and *PBS*, which are encumbered by complex installation procedures and dependency challenges, this <u>task-spooler-PLUS</u> is designed for effective task management on personal computers and workstations accommodating several to tens of users.

## Introduction 

As a computer scientist, I frequently need to submit multiple—sometimes dozens—of simulation tasks on my personal workstations while sharing computational resources with other users. I initially experimented with the original <u>task-spooler</u> software, but it lacked multi-user support, as each user maintained an independent task queue. To address this limitation, I modified the software and developed <u>task-spooler-PLUS</u>, enabling support for multiple users.

More recently, I introduced two key enhancements: **fatal crash recovery and processor binding**. In the event of a fatal crash, <u>task-spooler-PLUS</u> utilizes *SQLite3* to restore all tasks, including those that were running, queued, or completed. Unlike the original version, <u>task-spooler-PLUS</u> operates as a background service and **requires root privileges.**

### Changelog

See [CHANGELOG](CHANGELOG.md).

## Features

I have enhanced <u>task-spooler</u> to support task execution on my workstation with multiple users. Below are the key features of <u>task-spooler-PLUS</u>:

- **Cross-platform task queue management** for GNU/Linux, Darwin, Cygwin, and FreeBSD
- **Multi-user support** with customizable limits on maximum processor usage
- **Fatal crash recovery** ensuring task persistence by logging data to an *SQLite3* database
- **Wall-time management** auto-requeue for timed-out tasks in the tail of queue for the deferred execution.
- **Pause and resume functionality** for any running or queued task
- **Global control** to pause or resume all tasks for a single user
- **Comprehensive information output**, available in default, JSON, and tab-separated formats
- **Simple installation and configuration** for ease of use
- **Optional separation of stdout and stderr** for better log management

### Install Task Spooler PLUS

Simple run the provided script

```
./make
```
**The default positions** of log file and database is defined in `default.inc`.

```c
#define DEFAULT_USER_PATH "/home/kylin/task-spooler/user.txt"
#define DEFAULT_LOG_PATH "/home/kylin/task-spooler/log.txt"
#define DEFAULT_SQLITE_PATH "/home/kylin/task-spooler/task-spooler.db"
#define DEFAULT_EMAIL_SENDER "kylincaster@foxmail.com"
#define DEFAULT_EMAIL_TIME 45.0
#define DEFAULT_USER_LOCK_TIME 30
#define DEFAULT_ROOT_LOCK_TIME 86400
#define DEFAULT_MAX_WALL_TIME 10080 // in minutes
#define DEFAULT_HPC_NAME "intel_laptop"

enum { MAXCONN = 1000 };
enum { DEFAULT_MAXFINISHED = 1000 };

#define DEFAULT_NOTIFICATION_SOUND "/home/kylin/task-spooler/notifications-sound.wav"
#define DEFAULT_ERROR_SOUND "/home/kylin/task-spooler/error.wav"
#define DEFAULT_PULSE_SERVER "·"
```

You can specific the positions by the environment variables `TS_USER_PATH`, `TS_LOGFILE_PATH`, and `TS_SQLITE_PATH`, respectively on the invoking of daemon server. Otherwise, you could specify the positions in the `user_config` file.



To use `ts` anywhere, `ts`  needs to be added to `$PATH` if it hasn't been done already.

#### Common problems
* Once the suspending of the task-spooler Plus client: try to remove the socket file `/tmp/socket-ts.root` define by `TS_SOCKET` 
* After a fatal crash, the recovered server cannot capture the exit-code and signal of the running task

## User configuration

using the `TS_USER_PATH` environment variable to specify the path to the user configuration. The format of the user config file is shown as an example. The UID could be found by `id -u [user]`.

```
# 1231 	# comments
TS_SLOTS = 4 # The number of Total slots
# uid     name    slots
1000     Kylin    10
3021     test1    10
1001     test0    100
34       user2    30

qweq qweq qweq # error, automatically skipped
```

Note that the number of slots can be specified in the user configuration file (2nd line).

## How it works

The queue is managed by a *server process*, which is automatically started if it is not already running. Communication between the client and server occurs via a <u>Unix socket</u>, typically located in `/tmp/`.

When a user submits a job using a <u>ts client</u>, the client waits for a response from the server to determine when execution can begin. Once the server grants permission, the client typically *forks* and executes the command within the appropriate environment. Unlike `at` or `cron`, the client, not the server, runs the job. As a result, user-specific settings such as ulimits, environment variables, and working directory (pwd) are applied.

Upon job completion, the client *notifies the server*, which may then signal waiting clients and record both the *job's output and exit status*.

Additionally, clients can retrieve various details from the server, such as *job completion status and output locations*, enabling better monitoring and management of queued tasks.

## History 

Андрей Пантюхин (Andrew Pantyukhin) maintains the BSD port.

Alessandro Öhler provided a Gentoo ebuild for 0.4, which with simple changes I updated to the ebuild for 0.6.4. Moreover, the Gentoo Project Sunrise already has also an ebuild (maybe old) for ts.

Alexander V. Inyukhin maintains unofficial debian packages for several platforms. Find the official packages in the debian package system.

Pascal Bleser packed the program for SuSE and openSuSE in RPMs for various platforms.

Gnomeye maintains the AUR package.

Eric Keller wrote a nodejs web server showing the status of the task spooler queue (github project).

Duc Nguyen took the project and develops a GPU-support version.

Kylin wrote the multiple user support, fatal crush recovery through Sqlite3 database.

## Manual

See below or `man ts` for more details.

```
Task Spooler 2.1.1a - a task queue system for the unix user.
Copyright (C) 2007-2024  Kylin JIANG - Duc Nguyen - Lluis Batlle i Rossell
usage: ./ts [action] [-ngfmdE] [-L <lab>] [-D <id>] [cmd...]

Environment Variables:
  TS_SOCKET        : Unix socket path (default: $TMPDIR/socket-ts.root)
  TS_MAIL_FROM     : Sender email for results (default: kylincaster@foxmail.com)
  TS_MAIL_TIME     : Email threshold in seconds (default: 45.000 sec.)
  TS_SERVICE_NAME  : Service name for Email notifications (default: intel_laptop)
  TS_MAXFINISHED   : Max finished jobs in queue (default: 1000)
  TS_MAXCONN       : Max concurrent connections (max 1000, default: 1000)  TS_ONFINISH      : Binary executed post-job (args: ID, status, output, cmd)
  TS_ENV           : Command to gather job info during enqueue
  TS_SAVELIST      : Crash recovery file for job list
  TS_SLOTS         : Max concurrent jobs (server start, default: 1)
  TS_USER_PATH     : User config file path (server start)
  TS_LOGFILE_PATH  : Job log path (server start)
  TS_SQLITE_PATH   : SQLite DB path for logs (server start)
  TS_FIRST_JOBID   : Initial job ID (server start, default: 1000)
  TS_SORTJOBS      : Job queue sorting control (server start)
  TS_MAX_WALL_TIME : Max job wall-time in hours, (server start, default: 10080 minutes or 7.0 days)
  TMPDIR           : Temporary Output files directory

Long option actions:
  --getenv   [var]                Get server environment variable
  --setenv   [var]                Set server environment flag
  --unsetenv   [var]              Remove server environment flag
  --get_label      || -a [id]     Show job label (last added if unspecified)
  --full_cmd       || -F [id]     Show full command (last added if unspecified)
  --check_daemon                  Verify daemon status
  --count_running  || -R          Count running jobs
  --last_queue_id  || -q          Show last added job ID
  --get_logdir                    Display log directory path
  --set_logdir [path]             Configure log directory
  --serialize   ||  -M [format]   Export job list (formats: default/json/tab)
  --tmp                           Store logs in tmp folder
  --hold [jobid]                  Pause specified job
  --cont [jobid]                  Resume paused job
  --suspend [user]                User: pause tasks & lock account
                                  Root: lock all/specific user account
  --resume [user]                 User: resume tasks & unlock account
                                  Root: unlock all/specific user accounts
  --lock                          Lock server (5 sec. timeout; root has no timeout)
  --unlock                        Release server lock
  --relink [PID]                  Reconnect tasks after unexpected failures
  --job [joibid] || -J [joibid]   Assign/relink job ID
  --wtime [walltime]              Wall time limit in minutes (will be clamped to system maximum if exceeded).
  --add-wtime <jobid,ADD_time>    Increase job wall time by ADD_time in minutes (root only)
  --daemon                        Run as daemon (root only)

Actions:
  -A           List info for all users
  -X           Update user config by UID (root only, max 100 users)
  -K           Stop server (root only)
  -C           Clear finished jobs for current user
  -l           Show the job list (default action).
  -S [num]     Get/set max concurrent jobs (root only)
  -t [id]      Tail -f last 10 lines (last job if unspecified)
  -c [id]      Show complete output (last job if unspecified)
  -p [id]      Display job PID (last job if unspecified)
  -o [id]      Show output file path (last job if unspecified)
  -i [id]      Display job info (last job if unspecified)
  -s [id]      Show job state (last added if unspecified)
  -r [id]      Remove job (last added if unspecified)
  -w [id]      Wait for job (last added if unspecified)
  -k [id]      Send SIGTERM to job (last run if unspecified)
  -T           SIGTERM all jobs (root only)
  -u [id]      Prioritize job (last added if unspecified)
  -U <id-id>   Swap two jobs in queue
  -h | --help  Show help
  -V           Display version

Options adding jobs:
  -B           Exit if server full
  -n           Disable output storage
  -E           Separate stderr to .e file
  -O           Set log filename (no path)
  -z           Gzip output (unless -n)
  -f           Run in foreground
  -m <email>   Email results via ssmtp
  -d           Schedule execution after last job
  -D <id,...>  Schedule execution after specified IDs
  -W <id,...>  Schedule after successful IDs (exit 0)
  -L [label]   Assign job label
  -N [num]     Required slots (default: 1)
```


## Restore from a fatal crush
If the <u>task-spooler-PLUS</u> server crashes, the service will automatically recover all tasks. Alternatively, manual recovery can be performed using an automated Python script by running `python relink.py`.

```
# relink.py setup
logfile = "/home/kylin/task-spooler/log.txt" # Path to the log file of tasks
days_num = 10 # only tasks starts within [days_num] will be relinked
```
or through the command line as

```
ts -N 10 --relink [pid] task-argv ...
ts -L myjob -N 4 --relink [pid] -J [Jobid] task-argv ...
```

where `[pid]` is the *PID* of the running task and `[Jobid]` is the specified job id.

**Author**

- Kylin JIANG, gengpingjing@wust.edu.cn

- Duc Nguyen, <adnguyen@yonsei.ac.kr>

- Lluís Batlle i Rossell, <lluis@vicerveza.homeunix.net>

  

**Acknowledgement**

* To Raúl Salinas, for his inspiring ideas
* To Alessandro Öhler, the first non-acquaintance user, who proposed and created the mailing list.
* Андрею Пантюхину, who created the BSD port.
* To the useful, although sometimes uncomfortable, UNIX interface.
* To Alexander V. Inyukhin, for the debian packages.
* To Pascal Bleser, for the SuSE packages.
* To Sergio Ballestrero, who sent code and motivated the development of a multislot version of ts.
* To Duc Nguyen, for his faithful working on GPU versions
* To GNU, an ugly but working and helpful ol' UNIX implementation. 

**Software**

Memory checks with [Valgrind](https://valgrind.org/).

## Related projects

[Messenger](https://github.com/justanhduc/messenger)
