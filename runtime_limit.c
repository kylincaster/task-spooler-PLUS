#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include <time.h>
#include "main.h"
#include <time.h>

#include "default.inc"


static double str2double(const char *str) {
    char *endptr;
    errno = 0;

    double val = strtod(str, &endptr);

    if (str == endptr) {
        return -1;
    }

    if (errno == ERANGE) {
        return -1;
    }

    // 3. 有非法字符（非空格）
    while (*endptr) {
        if (!isspace((unsigned char)*endptr)) {
            return -1;
        }
        endptr++;
    }

    return val;
}

time_t get_monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

double get_max_wall_time() {
  const char* str = getenv("TS_MAX_WALL_TIME");
  if (str == NULL || strlen(str) == 0) {
    ;
  } else {
    double max_walltime = str2double(str);
    if (max_walltime > 0)
        return max_walltime;
  }
  return DEFAULT_MAX_WALL_TIME;
}

time_t get_job_time_by_job(struct Job* p) { // return in seconds
    time_t t = get_monotonic_sec() - p->info.start_time;
    return t;
}

time_t get_cost_time_by_job(struct Job* p) { // return in seconds
    time_t t = p->info.end_time - p->info.start_time;
    return t;
}

double get_cpu_time_by_pid(int pid) {
    char path[64];
    FILE *fp;
    long utime, stime;
    long ticks;
    double total_time;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    /*
     * /proc/[pid]/stat 格式复杂：
     * 前面很多字段，我们只关心第14和15个：
     * utime 和 stime
     */
    int i;
    char buf[1024];

    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    char *ptr = buf;

    // 跳过前13个字段
    for (i = 0; i < 13; i++) {
        ptr = strchr(ptr, ' ');
        if (!ptr) return -1;
        ptr++;
    }

    // 读取 utime 和 stime
    if (sscanf(ptr, "%ld %ld", &utime, &stime) != 2) {
        return -1;
    }

    // 获取系统 ticks（每秒多少 tick）
    ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) {
        return -1;
    }

    total_time = (double)(utime + stime) / ticks;
    // printf("total_time = %f, %ld, %ld %ld\n", total_time, utime, stime, ticks);
    return total_time; // 单位：秒
}