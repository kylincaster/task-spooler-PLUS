#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

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
    printf("total_time = %f, %ld, %ld %ld\n", total_time, utime, stime, ticks);
    return total_time; // 单位：秒
}