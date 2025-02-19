#!/usr/bin/env python
import sys
import datetime 
import psutil
import os

logfile = "/home/kylin/task-spooler/log.txt"
ts_CMD="ts"
days_num = 30
if len(sys.argv) != 1:
    days_num = int(sys.argv[2]);
    exit(1)


def parse(s):
    p1 = s.find("]")
    p2 = s.find("P:")
    p4 = s.find("> Pid:")
    p3 = s[p2:p4].find("<")+p2
    p5 = s[p4:].find("CMD:") + p4
    p6 = s.rfind("@")
    CMD = s[p5+4:p6].strip()
    time_str = s[p6+1:].strip()
    pid = int(s[p4+6:p5].strip())
    procs = int(s[p2+2:p3].strip())
    user = s[p1+1:p2].strip()
    tag  = s[p3+1:p4]
    jobid = s[1:p1]
    t_time = datetime.datetime.strptime(time_str, "%Y-%m-%d %H:%M:%S")
    return user, procs, pid, tag, CMD, t_time, jobid;


print("read from", logfile)

with open(logfile, "r") as r:
    lines = [i.strip() for i in r.readlines()]

t_now = datetime.datetime.now()
# t_line = time.gmtime() 
t_line = t_now - datetime.timedelta(days = days_num)
print("  only restore tasks with {} days, start by".format(days_num), t_line)
tasks = []
for l in lines[:]:
    user, procs, pid, tag, CMD, t_time, jobid = parse(l)
    if (psutil.pid_exists(pid)):
        if (t_time > t_line):
            
            p = psutil.Process(pid)
            cmd = " ".join(p.cmdline()).replace("/bin/sh /opt/intel/oneapi/mpi/2021.3.0/bin/mpirun", "mpirun")
            if (cmd == CMD):
                print("add:", l)
                tasks.append([tag, pid, jobid, procs, CMD])
                # tasks.append([tag, pid, procs, int(t_time.timestamp()), CMD])
            else:
                print("add:", l)
                tasks.append([tag, pid, jobid, procs, CMD])
                # print("  N/A:", l) # "#", cmd, p.name(), p.cmdline())
        else:
            print("  UNK:", l)

for i in tasks[:]:
    if i[0] == "..":
        CMD = '{} --relink {} -J {} -N {} "{}"'.format(ts_CMD, *i[1:])
        # CMD = 'ts --pid {} -N {} --stime {:} "{}"'.format(*i[1:])
    else:
        CMD = '{} -L {} --relink {} -J {} -N {} "{}"'.format(ts_CMD, *i)
        # CMD = 'ts -L {} --pid {} -N {} --stime {:} "{}"'.format(*i)
    print(CMD)
    os.system(CMD)
    
