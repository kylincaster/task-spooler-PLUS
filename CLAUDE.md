# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is ts?

**Task Spooler PLUS (`ts`) is a C-language multi-user job scheduler — think of it as a lightweight Slurm for shared workstations.** A single `ts` binary acts as both client and server, communicating over a Unix domain socket. No daemon management, no database setup, no cluster infrastructure.

### One-line mental model

```
You type "ts sleep 30" → main.c parses CLI → client sends NEWJOB over Unix socket
→ server enqueues → next_run_job() fires when slots/deps are ready →
server tells client to RUNJOB → client fork()+execvp(your command) →
waitpid() → server gets ENDJOB → writes SQLite, frees slots, triggers callbacks
```

### Who it's for

Shared workstations with a handful of users running simulation, ML training, or batch jobs — where you want Slurm-like scheduling (queues, dependencies, resource limits, multi-user) without deploying a cluster infrastructure.

### Key differentiators vs. original Task Spooler

| | Original TS | TS PLUS |
|---|---|---|
| **Users** | One queue per user | Central server, multi-user with per-user slot limits |
| **Recovery** | Jobs lost on crash | SQLite3 WAL — all jobs survive crashes and reboots |
| **Resource control** | None | cgroups v1/v2 CPU limiting, freezer pause/resume, NUMA CPU binding |
| **Scheduling** | FIFO only | Dependency chains, wall-time auto-pause, `--at` scheduled execution |
| **User mgmt** | None | Dynamic user config, suspend/resume per user, `ts -X` hot-reload |
| **Resilience** | Disconnected on server restart | Auto-reconnect, re-attach running jobs seamlessly |

## Build & Run

```bash
make                      # Build `ts` binary (cgroups v1, default)
make CGROUP_V2=1          # Build with cgroups v2 support
make clean                # Remove objects and binary
./install_make            # Install to /usr/local (needs root)
./testbench.sh            # Integration test script
```

No configure step needed. No unit test framework — `testbench.sh` exercises the binary end-to-end.

Build uses `-std=gnu11 -Wall -ansi -pedantic -fcommon -Wno-format-truncation`. The `version.h` is generated at build time or created manually; `Makefile` appends git describe output when inside a worktree.

CPU binding (`TS_CPU_BIND`): requires `hwloc-calc` (from hwloc) to generate topology headers.

**Installing hwloc**:
```bash
# Debian/Ubuntu
sudo apt install hwloc

# RHEL/CentOS/Fedora
sudo dnf install hwloc

# Verify
hwloc-calc --version
```

**Topology detection** — `gen_topology.py` issues these hwloc-calc queries:

```bash
# 1. Count objects of each type
hwloc-calc --number-of NUMANode all    # → NUM_NODES
hwloc-calc --number-of L3Cache all     # → by_l3 count
hwloc-calc --number-of L2Cache all     # → by_l2 count
hwloc-calc --number-of L1Cache all     # → by_l1 count
hwloc-calc --number-of Core all        # → total cores

# 2. For each object i, find which cores it contains (top-down: type → Core)
hwloc-calc NUMANode:0 --intersect Core  # → cores on node 0
hwloc-calc L3Cache:0  --intersect Core  # → cores in L3 0
hwloc-calc L2Cache:0  --intersect Core  # → cores in L2 0
hwloc-calc L1Cache:0  --intersect Core  # → cores in L1 0

# 3. For each core, find its physical PUs (top-down: Core → PU)
hwloc-calc Core:0 --intersect PU --physical  # → physical PUs on core 0
```

The `--physical` flag on PU queries excludes Hyper-Threading siblings (HT excluded by default; pass `--ht` to include them). The script precomputes `core_pu_map` (core→PUs) and `core_node_map` (core→NUMA node) once, then each strategy builds groups by intersecting `type:i` with `Core` and expanding through the cache.

**Generating topology headers**:
```bash
python3 tools/gen_topology.py          # generate all non-trivial strategies
python3 tools/gen_topology.py by_l2    # generate a specific strategy
python3 tools/gen_topology.py --ht     # include HT siblings
cp topology_by_l2.h topology.h         # pick one and link
make TS_CPU_BIND=1                     # build with CPU binding
make TS_CPU_BIND=1 CGROUP_V2=1        # CPU binding + cgroups v2
```

## Architecture

**Client-server over Unix domain socket** (usually `/tmp/socket-ts.root`). The server must run as root (uses `SO_PEERCRED` for auth). The client forks and executes the actual job — the server never runs user commands.

### Process model

```
ts (client)  ──Unix socket──▶  ts (server daemon)
   │                              │
   fork() + exec(cmd)             │  manages queue, slots, users
   │                              │  persists to SQLite3
   waitpid() → notify server      │
```

1. `main.c` — CLI arg parsing (`parse_opts`), dispatches to `c_*` client functions
2. `server_start.c` — socket path creation, auto-launches server if not running via `ensure_server_up()`
3. `server.c` — `server_main()` → `server_loop()`: single-threaded `select()` loop over client connections
4. `client.c` — client-side message send/recv for every action (queue, list, kill, tail, etc.)
5. `jobs.c` — core job queue: `vec_t active_jobs` / `vec_t finished_jobs`, job lifecycle (QUEUED→RUNNING→FINISHED), slot accounting, wall-time timeout check, dependency resolution, cgroups freeze/thaw integration
6. `execute.c` — `run_job()`: fork+exec the user command, capture exit status and timing
7. `sqlite.c` — SQLite3 persistence: `Jobs` table (active queue), `Finished` table (completed), `Global` table (jobid counter). Schema mirrors `struct Job`/`Result`/`Procinfo`
8. `cgroups.c` — cgroups v1/v2: CPU quota limiting and freezer-based pause/resume. V2 selected via `-DCGROUP_V2` compile flag
9. `runtime_limit.c` — wall-time enforcement helpers, `parse_time()` for human durations like "30s", "3.4m", "1.5H", "2d"
10. `user.c` — multi-user config in `vec_t users_vec`, `struct User` with `uid_t uid`, slot mapping, user lock/suspend state

### Key data structures

- **`struct Job`** (`jobs.h`) — all job state: id, command, state, result, output filename, dependencies, wall-time, cgroup info, `struct User *user` pointer
- **`struct User`** (`user.h`) — user state: name, uid, max_slots, busy/jobs/queue counters, locked flag. Stored in dynamic `vec_t users_vec`
- **`struct Msg`** (`msg.h`) — wire protocol message with union of all request/response types; type-tagged binary over Unix socket
- **`struct Client_conn`** (`server.h`) — per-connection state: socket fd, hasjob flag, jobid, `struct User *user`
- **`struct CommandLine`** (`main.h`) — parsed CLI state, passed through to client functions

### Full request flow (submitting a job)

```
ts sleep 30
  │
  ├─ main.c: parse_opts()
  │   → command_line.request = c_QUEUE
  │   → command_line.command = ["sleep", "30"]
  │
  ├─ server_start.c: ensure_server_up()
  │   → if server not running, fork()+exec(ts --daemon)
  │
  ├─ client.c: c_new_job()
  │   → build Msg{NEWJOB, command, path, env, slots, wall_time...}
  │   → send_msg() over Unix socket
  │   → recv_msg() → gets jobid back
  │
  ├─ server.c: server_loop() — single-threaded select()
  │   │
  │   ├─ accept() → SO_PEERCRED → find_user_by_uid()
  │   ├─ recv_msg() → client_read()
  │   │   └─ case NEWJOB:
  │   │       └─ jobs.c: s_newjob()
  │   │           ├─ s_update_slots_usage()     # clean dead PIDs first
  │   │           ├─ newjobptr() → vec_push(&active_jobs)
  │   │           ├─ set state=QUEUED
  │   │           ├─ recv dependencies & command & env strings
  │   │           └─ insert_DB("Jobs")
  │   │
  │   └─ (next select() tick) → next_run_job()
  │       ├─ for each QUEUED job:
  │       │   ├─ check user slot limit (user->busy + N ≤ user->max_slots)
  │       │   ├─ check global busy_slots + N ≤ max_slots
  │       │   ├─ check dependencies satisfied
  │       │   ├─ check schedule_time reached
  │       │   └─ if all OK → s_mark_job_running() + s_send_runjob()
  │       └─ s_send_runjob():
  │           └─ send Msg{RUNJOB} to client
  │
  ├─ client.c: recv RUNJOB → execute.c: run_job(jobid)
  │   ├─ fork()
  │   │   ├─ CHILD: run_child()
  │   │   │   ├─ wait for cgroup creation (cgroups_freeze_ok)
  │   │   │   ├─ open output file → dup2(stdout/stderr)
  │   │   │   ├─ setsid()  # new process group
  │   │   │   ├─ optionally retry on transient failure
  │   │   │   └─ execvp("sleep", ["30"])
  │   │   └─ PARENT: run_parent()
  │   │       ├─ read output filename + start time from pipe
  │   │       ├─ install SIGINT handler for ts -k
  │   │       ├─ send Msg{RUNJOB_OK, ofname, pid} to server
  │   │       ├─ waitpid(pid)  # blocks until child exits
  │   │       ├─ times() → calculate real/user/system_sec
  │   │       └─ c_end_of_job() → send Msg{ENDJOB, result}
  │   └─ (client exits or waits for next command)
  │
  ├─ server.c: recv ENDJOB → jobs.c: job_finished()
  │   ├─ move job from active_jobs → finished_jobs
  │   ├─ update DB: insert Finished, delete Jobs
  │   ├─ cgroups_clean_job()  # remove cgroup directory
  │   ├─ cpu_bind_free()  # release CPU allocation
  │   ├─ notify_errorlevel() → wake dependent jobs
  │   ├─ on_finish hook (TS_ONFINISH or --on-finish)
  │   └─ next_run_job()  # try to dispatch waiting jobs
  │
  └─ Final state: job is FINISHED in finished_jobs vec + "Finished" DB table
```

### Job state machine

```
QUEUED → RUNNING → FINISHED
  ↓         ↓
LOCKED    PAUSE (freezer cgroup)
  ↓         ↓
QUEUED    RUNNING (on resume/cont)
              ↓
         ABNORMAL (health check: stuck job, slots freed)
              ↓
         FINISHED (ts -k → SIGTERM → ENDJOB)
              
Special states: HOLDING_CLIENT (queue full), DELINK (client disconnected)
```

### User auth & slots

The server reads a user config file (path from `TS_USER_PATH` or env var) that maps usernames to max slot counts. Usernames are resolved to UIDs via `getpwnam()`. Users are stored in a dynamic `vec_t users_vec` with `struct User` entries. `SO_PEERCRED` on the Unix socket identifies the caller — the server looks up the peer's UID in the user vec. Root (UID 0) has full control. Each user gets a slot budget; jobs specify required slots via `-N`.

**user.txt format**:
```
# <username> <max_slots>
#TS_SLOTS = 16         # global default (can be overridden per-user)
john    4
mary    2
```

### CPU Binding (TS_CPU_BIND)

Optional compile-time feature activated with `make TS_CPU_BIND=1`.

1. **tools/gen_topology.py** — runs `hwloc-calc`, detects NUMA nodes, L2/L3/L1 caches, and PUs. Generates `topology_{strategy}.h` with `#define` constants (`NUM_NODES`, `NUM_GROUPS`, `MAX_CORES_PER_GROUP`, `MAX_OS_CPU`, `MAX_GROUPS_PER_NODE`) + `TOPOLOGY_INIT` macro. Handles HT exclusion/inclusion. Skips trivial strategies (1:1 object-to-core mapping) and duplicate topologies. Auto-selects best strategy by average cores/group closest to 4.
2. **cpu_bind.c** — allocator: best-fit group selection, cross-node merge. Tracks CPU ownership via `cpu_owner[]` array using real job IDs.
3. **`cpu_bind_defrag()`** — two-phase NUMA-aware defrag triggered after each job finishes:
   - Phase 1-2: freeze quality>0 jobs, rebuild allocation state, restore quality=0 allocs
   - Phase 3: try same-node reassignment first (NUMA affinity preserved)
   - Phase 4: cross-node merge for remaining jobs
   - `primary_node` / `mem_nodes` are preserved across defrag cycles
4. **cgroups_set_cpuset()** — writes cpuset.cpus/mems to the cgroup (v1: `/sys/fs/cgroup/cpuset/`, v2: unified hierarchy).
5. **Restart recovery** — `cgroups_restore_all_cpu_bind()` scans cgroup directories, reads cpuset.cpus, rebuilds alloc tracking via `cpu_bind_claim()`.

Ordering: cpuset cgroup is created BEFORE freezer cgroup, so the child process's `cgroups_freeze_ok()` wait covers all cgroups.

Runtime controls: `--no-bind` disables binding per-job (via NEWJOB message) or server-wide (via `cpu_bind_set_disabled()`). `--no-bind-defrag` disables auto-defrag at server start (root only; for MPI workloads, see Known Issues).

### Cgroups support

Build with `make CGROUP_V2=1` for cgroups v2, or just `make` for v1 (default). Path constants defined at top of `cgroups.c`:
- v1: `/sys/fs/cgroup/cpu/` + `/sys/fs/cgroup/freezer/`
- v2: `/sys/fs/cgroup/` (unified hierarchy, `cpu.max` + `cgroup.freeze`)

### Environment variables

Key overrides: `TS_SOCKET`, `TS_SLOTS`, `TS_USER_PATH`, `TS_LOGFILE_PATH`, `TS_SQLITE_PATH`, `TS_MAXFINISHED`, `TS_MAX_WALL_TIME`, `TS_SORTJOBS`, `TS_SAVELIST`, `TS_ONFINISH`.

## Wall-Time Timeout Mechanism

### How wall-time is set

```
ts --wtime 30s   sleep 100     # job runs max 30 seconds
ts --wtime 2.5h  make -j8     # max 2.5 hours
ts --wtime 1d    ./train.py   # max 1 day
```

- Use `--wtime` with a duration string (suffixes: `s`, `m`, `H`, `d`, `w`; no suffix = seconds).
- Default is **7 days** (`DEFAULT_MAX_WALL_TIME = 604800`), overridden by env `TS_MAX_WALL_TIME`.
- The effective wall_time is `min(user's --wtime, get_max_wall_time())`, set at job creation (`jobs.c:808-813`).

### Parse duration format (`parse_time`, runtime_limit.c)

- Concatenated durations like `1h30m`, `30s`, `2d`, `1.5H` (case-insensitive).
- Suffixes: `s`=1, `m`=60, `h`=3600, `d`=86400, `w`=604800.
- Range: `±86400000` seconds (±1000 days).
- `--add-wtime` accepts negative values (e.g. `-30m`) to reduce remaining time.

### Timeout detection flow

```
s_update_slots_usage()          ← called every server loop tick (~1s)
  └─ s_check_timeout()          ← jobs.c:232
      ├─ backward scan active_jobs for RUNNING jobs
      ├─ for each: check_timeout(p)  ← jobs.c:211
      │   ├─ get_work_time_by_job()  → actual work time (excluding pauses)
      │   ├─ if work_time > i64abs(wall_time) && work_time >= 3:
      │   │   ├─ safe_pause_job(p)   → SIGSTOP / cgroup freezer
      │   │   ├─ wall_time = -i64abs(wall_time) - 86400  (negative + 24h)
      │   │   ├─ update_field_int64("Jobs", wall_time)
      │   │   ├─ state = PAUSE
      │   │   └─ return 1 (timed out)
      │   └─ else → return 0 (not yet)
      └─ for each timed-out job:
          ├─ vec_remove + vec_push to back of active_jobs
          └─ movebottom_DB(jobid)  → order_id = max+1 (DB sync)
```

**Key function — `get_work_time_by_job()`** (runtime_limit.c):
```c
time_t t = (p->state == PAUSE) ? p->info.pause_time : get_monotonic_sec();
return t - p->info.start_time - p->info.pause_duration;
```
This is **wall-clock time excluding pauses**. Uses `CLOCK_MONOTONIC` so system sleep doesn't count.

### What happens to the timed-out job

1. **Process is frozen** — `safe_pause_job()` sends `SIGSTOP` to the process group (or freezes the cgroup), and frees CPU binding cores.
2. **wall_time becomes negative** — the negative sign is a flag meaning "timeout-paused" (vs user-paused). The extra `-86400` (24h) is a **cooldown offset**.
3. **Job moves to queue back** — `vec_remove` + `vec_push` + `movebottom_DB()` ensures other jobs get dispatched first.
4. **DB synced** — `wall_time` and `order_id` are written to SQLite so ordering survives restart.

### Negative wall_time / 24h cooldown

The negative wall_time serves two purposes:

**Display** (`list.c:172-176`):
```c
if (p->wall_time < 0)  jobstate = "timeout";
else                   jobstate = "pause  ";
```

**Resume decision** (`next_run_job()`, jobs.c:1085-1095):
```c
if (p->state == PAUSE && p->wall_time < 0) {
    if (i64abs(p->wall_time) <= get_cpu_time_by_pid(p->pid))
        continue;  // still cooling
    config_running(p);
    s_send_runjob(p->client_socket, p->jobid);
}
```
- The absolute value of negative wall_time = `old_limit + 86400` is the **CPU time threshold** the process must reach before retry.
- `get_cpu_time_by_pid()` reads `/proc/pid/stat` (user+system ticks → seconds).
- Once the process has consumed enough CPU time to exceed the threshold, it can be re-dispatched.

### Resume: `config_running()` (jobs.c:151)

```c
p->state = RUNNING;
p->wall_time = i64abs(p->wall_time);    // clear negative flag → positive limit
update_field_int64("Jobs", "wall_time");
rerun_job_config(p);                    // clear pause_time, update pause_duration
cgroups_thaw_job(p);                    // SIGCONT / thaw cgroup
```

The positive `wall_time` means the job can now run for its full limit again.

### User commands on paused jobs

| Command | Sign change | Effect |
|---------|-------------|--------|
| `ts -c <id>` (cont) | `-` → `+` | Resume immediately, clear cooldown |
| `ts -p <id>` (pause) | `-` → `+` then... | Convert timeout-pause to user-pause (if timeout was waiting) |
| `ts --add-wtime <id> 1h` | sign preserved | Add or reduce wall_time (root only) |

### Ordering consistency (`movebottom_DB`)

When a job times out, `s_check_timeout()` removes it from its current position in `active_jobs` and pushes it to the back. The function `movebottom_DB(jobid)` (sqlite.c) sets the SQLite `order_id` to `max(order_id) + 1`, ensuring `SELECT jobid FROM Jobs ORDER BY order_id` produces the same order as the runtime queue after restart.

## Restart Recovery

When the server dies (crash / reboot / kill), the **client processes survive** because they forked the actual user commands. SQLite3 in WAL mode preserves all job state. Here is the complete recovery sequence:

### 1. Server restart — startup sequence

```
server_main()
  │
  ├─ 1. Bind Unix socket, init users, read user.txt
  ├─ 2. cgroups_clean_all_finished()      # remove stale cgroup dirs
  ├─ 3. cgroups_v1_init() / v2_init()     # verify cgroup controllers accessible
  ├─ 4. cpu_bind_init()                   # build topology index, reset cpu_owner[]
  ├─ 5. open_sqlite()                     # open DB in WAL mode
  ├─ 6. init_jobs()                       # vec_init(&active/finished_jobs)
  ├─ 7. get_jobids_DB()                   # restore jobid counter from Global table
  ├─ 8. s_read_sqlite()                   # ← CORE: restore all jobs from DB
  │   └─ for each job in "Jobs" table:
  │       └─ s_add_job() — state-dependent restore logic:
  │           ├─ RUNNING + pause_time>0 + PID alive  → PAUSE (frozen cgroup)
  │           ├─ RUNNING + PID alive                 → RUNNING (client will reconnect)
  │           ├─ RUNNING + PID dead                  → delete from DB (zombie)
  │           ├─ QUEUED / LOCKED                     → QUEUED (waiting for reconnect)
  │           ├─ PAUSE + PID alive                   → PAUSE (frozen, waiting for cont)
  │           └─ else                                → destroy_job() (discard)
  ├─ 9. s_update_slots_usage()            # recalc busy_slots from alive RUNNING jobs
  ├─10. cgroups_restore_all_cpu_bind()    # scan /sys/fs/cgroup/ for TASK_SPOOLER_* dirs
  │   └─ for each cgroup dir:
  │       ├─ PID dead              → remove orphan cgroup
  │       ├─ no matching ts job    → remove orphan cgroup (let kernel clean up)
  │       ├─ RUNNING/PAUSE + alive → cpu_bind_claim() + cgroups_set_cpuset()
  │       └─ other states ignore   → (cgroup shouldn't exist)
  │
  └─ server_loop()                    # enter select() loop, accept clients
```

### 2. Client reconnect flow

When a running client's `recv_msg()` returns 0 or -1 (server socket broken):

```
c_wait_server_commands()
  │ recv_msg() → error
  │
  ├─ reconnect_to_server()
  │   ├─ close old socket
  │   ├─ loop: socket() + connect() every 5 sec until success
  │   └─ return new server_socket
  │
  ├─ send Msg{RECONNECT, jobid, pid}   # claim the job back
  │
  └─ server.c: case RECONNECT:
      ├─ get_job(jobid)
      ├─ reject if job FINISHED/SKIPPED
      ├─ verify job belongs to same user (SO_PEERCRED)
      ├─ if job->pid != 0: verify pid matches (anti-hijacking)
      ├─ clear old connection entry → set new socket
      ├─ send Msg{RECONNECT_OK}
      └─ if job was RUNNING but pid==0: re-send RUNJOB
    
    Client receives RECONNECT_OK
      → continue c_wait_server_commands() loop
```

### 3. ENDJOB after reconnect

After the job finishes, the client sends `ENDJOB` via the new socket:

```
client: run_parent() → waitpid() → c_end_of_job()
  │ send Msg{ENDJOB, result} over new socket
  │ recv Msg{ENDJOB_OK}
  │
  ├─ if ENDJOB_OK not received → reconnect_to_server() again
  │   → send RECONNECT → recv RECONNECT_OK
  │   → resend ENDJOB
  │   → recv ENDJOB_OK
  │
  └─ run --on-finish callback (user context)
      return errorlevel to ts exit code
```

### 4. What survives vs. what is lost

| Survives | Lost |
|---|---|
| All job states (QUEUED/RUNNING/PAUSE) | Output file descriptors (re-opened via `/proc/PID/fd/1`) |
| Job commands, labels, environment | Client socket connections (re-established via RECONNECT) |
| Timing data (start/end/pause times) | In-flight `select()` notifications |
| Job IDs counter (Global table) | CPU binding state (restored from cgroup filesystem) |
| Finished job list | |

### 5. Queue restore detail (s_add_job)

```
s_add_job(job)
  │
  ├─ state == RUNNING:
  │   ├─ pause_time > 0 && PID alive  → PAUSE (restore frozen state)
  │   ├─ PID alive                     → RUNNING (keep, clear client_socket=0)
  │   └─ PID dead                      → delete_DB() + destroy_job()
  │
  ├─ state == QUEUED / LOCKED:
  │   │  → keep QUEUED, client_socket=0 (client reconnects to run it)
  │   └─ user->queue++
  │
  ├─ state == PAUSE:
  │   │  → keep PAUSE, set pause_time if 0
  │   └─ (cgroup is still frozen, waiting for `ts -c <id>`)
  │
  └─ else → destroy_job()
```

### 6. QUEUED job reconnect & DB leak (s_delete_job)

RECONNECT handles QUEUED jobs too (server.c RECONNECT case accepts `jp->state == QUEUED`).
After a restart, a QUEUED job is restored with `client_socket = 0` (see §5 above).
When the original client reconnects, the server sets `jp->client_socket = s` and the job
can be dispatched normally.

**⚠️ DB leak scenario** — If the client disconnects a **second time** while the job is still QUEUED:

```
1. Client submits QUEUED job    → insert_DB(job, "Jobs")
2. Server restart               → s_add_job() restores from DB, client_socket = 0
3. Client RECONNECT             → jp->client_socket = s (valid socket)
4. Client disconnects again     → clean_after_client_disappeared()
                                   → s_delete_job(jobid)
                                     → vec_remove(&active_jobs)
                                     → destroy_job(p)
                                     → ~~~ NO delete_DB() ~~~ (pre-v2.8.0)
```

Before the fix, `s_delete_job()` only removed the job from `active_jobs` and freed
memory, but **never deleted the SQLite "Jobs" row**. On the next restart the orphaned
job would be resurrected. This also affects `remove_connection()`, which also calls
`s_delete_job()`.

**Fix** (jobs.c): `s_delete_job()` now calls `delete_DB(jobid, "Jobs")` before
`destroy_job()`, matching the behavior of `s_remove_job()` (the `ts -r` path).

**Orphan QUEUED auto-cleanup** — After server restart, QUEUED jobs are restored with
`client_socket = 0` waiting for client RECONNECT. If no client reconnects within 30
minutes, the job is considered orphaned and is automatically removed.

```
server_loop() — every ~1s select() tick:
  │
  ├─ health check (every 10s)
  │
  ├─ ORPHAN QUEUED CLEANUP (once, ~30min after boot):
  │   ├─ s_cleanup_orphan_queued()
  │   │   └─ backward scan active_jobs
  │   │       └─ state == QUEUED && client_socket <= 0
  │   │           → s_delete_job() (vec_remove + delete_DB + destroy)
  │   └─ cleanup_done = 1  (never runs again)
  │
  ├─ next_run_job()
  └─ s_check_holdon()
```

Implementation uses two `static` variables in `server_loop()`:
- `cleanup_start` — set to `get_monotonic_sec()` on first tick
- `cleanup_done` — set to 1 after cleanup executes

No thread, no pipe, no extra socket — the existing 1-second `select()` timeout serves
as the timer. Jobs whose client reconnects before the 30-minute window get a valid
`client_socket` and are skipped by the cleanup.

**Timeout order_id sync** — When a RUNNING job wall-time expires, `s_check_timeout()`
moves it to the end of `active_jobs` (backward scan → `vec_remove` + `vec_push`).
Previously only the in-memory queue was reordered; the SQLite `order_id` was not
updated. After restart the job would reappear at its original position instead of
the end.

**Fix** — Added `movebottom_DB(jobid)` (sqlite.c) which sets `order_id = max(order_id) + 1`,
called from `s_check_timeout()` after each timed-out job is pushed to the back.
This keeps `SELECT jobid FROM Jobs ORDER BY order_id` consistent with the runtime
queue order after restart.

**`get_order_id()` bug (order_id=0)** — `get_order_id()` used `sqlite3_exec` with a
callback, which could not distinguish "no row found" from "order_id = 0". For new
jobs not yet in the DB, it returned `order_id = 0` with `err = 0`, so the fallback
`max_order_id + 1` in `edit_DB()` never triggered. All non-timeout jobs ended up
with `order_id = 0`, causing `ORDER BY order_id` to return them in arbitrary order
after restart.

**Fix** — Rewrote `get_order_id()` with `sqlite3_prepare_v2` / `sqlite3_step`. When
`sqlite3_step()` returns something other than `SQLITE_ROW` (no row found), it sets
`err = -1`, triggering the correct `max_order_id + 1` assignment.

## Current branch work

The `cpu-only` branch focuses on CPU binding and cgroups refinements, plus job health monitoring:

- **Async CPU binding defrag** — `cpu_bind_defrag()` moved to a background pthread so the server `select()` loop stays responsive. The main thread sorts allocs and builds the jobs array (safe `findjob()`), then a detached thread does the cgroup I/O (freeze → rebuild → cpuset → thaw). During defrag, CPU bind allocation/free and pause/resume are gated; new jobs dispatch without binding.
- **`cpu_bind_defrag_start()` / `cpu_bind_defrag_poll()`** — spawn and reap the defrag thread; retrigger flag defers freed allocs until the current defrag finishes.
- **Synchronization** — `defrag_in_progress` flag (checked by server before touching CPU bind state) + `defrag_mutex` (held by defrag thread only). Server never blocks on the mutex.
- **`gen_topology.py`** — multi-strategy output with `MAX_GROUPS_PER_NODE`, skips trivial/duplicate topologies
- **`cpu_owner[]`** tracks real job IDs throughout allocation and defrag
- **Job health check** — `s_check_running_health()` runs every 10s in the server loop. Detects stuck RUNNING jobs (output empty + no child processes, ≥5 min) and transitions to `ABNORMAL` state, freeing slots. Does not freeze cgroup — `ts -k` works normally.
- **Dead PID cleanup** — `s_update_slots_usage()` auto-detects dead PIDs among RUNNING/PAUSE/ABNORMAL jobs and moves them to finished.
- **PAUSE restore on restart** — `s_add_job()` restores paused jobs from DB (`pause_time > 0` → PAUSE), with PID liveness check to avoid zombies.

## Known Issues

### MPI + cpuset: dynamic cpuset changes may cause deadlock

Dynamically rewriting `cpuset.cpus` on running MPI jobs may cause deadlocks. MPI runtimes (Open MPI, MPICH) read cpuset at startup to determine process binding and topology. Changing the cpuset mid-run may leave the MPI runtime with a stale view of available CPUs, causing collective operations to hang.

**Workaround**: start the server with `--no-bind-defrag` to disable automatic defrag for MPI workloads. CPU binding allocated at job start persists correctly across forks (including `mpirun`). Use `--no-bind` if CPU binding is not needed at all.
