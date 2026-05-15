#include "main.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// TODO: add the cgroups2 supporting
// now only the cpus is limited

#define SPOOL_PATTERN "TASK_SPOOLER_%d_%d"

/**
 * 向 cgroup 文件写入字符串（自动处理打开/写入/关闭）
 *
 * @param path  文件路径
 * @param fmt   格式化字符串
 * @param ...   可变参数
 * @return      0 成功，-1 失败
 */
static int cg_write(char const *path, char const *fmt, ...) {
    char buf[256];
    va_list ap;

    va_start(ap, fmt);

    int len = vsnprintf(buf, sizeof(buf), fmt, ap);

    va_end(ap);

    if (len < 0) {
        fprintf(stderr, "Error: vsnprintf failed\n");
        return -1;
    }

    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        fprintf(stderr, "Error: open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    ssize_t ret = write(fd, buf, len);

    if (ret != len) {
        fprintf(stderr, "Error: write %s failed: %s\n", path, strerror(errno));

        close(fd);
        return -1;
    }

    close(fd);

    return 0;
}

/**
 * 创建目录（已存在不报错）
 *
 * @param path  目录路径
 * @return      0 成功，-1 失败
 */
static int cg_mkdir(char const *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: failed to create %s: %s\n in cg_mkdir()", path,
                strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * 将进程加入 cgroup v2 并限制其可用 CPU 核数
 *
 * @param pid     进程 PID
 * @param jobid   任务 ID（用于创建 cgroup 目录名）
 * @param cpus    可用的 CPU 核数（支持小数，如 1.5 表示 1.5 个核）
 * @return        0 成功，-1 失败
 */

int cgroup_v1_limit_cpu(int jobid, int pid, int cpus) {
    char buf[256];
    snprintf(buf, sizeof(buf), "/sys/fs/cgroup/cpu/" SPOOL_PATTERN, jobid, pid);
    printf("Create cgroups folder at %s\n", buf);

    char path[512];
    long period = 100000;       // 100ms 调度周期
    long quota = cpus * period; // 周期内可用 CPU 时间（微秒）

    /* 1. 创建 cgroup 目录 */
    if (cg_mkdir(buf) != 0) {
        return -1;
    }

    /* 2. 启用 CPU 控制器 */
    // snprintf(path, sizeof(path),
    // "/sys/fs/cgroup/unified/cgroup.subtree_control"); cg_write(path, "+cpu");
    // // 父级启用

    // snprintf(path, sizeof(path), "%s/cgroup.subtree_control", buf);
    // cg_write(path, "+cpu"); // 自身启用

    /* 3. 设置 CPU 限制 */
    snprintf(path, sizeof(path), "%s/cpu.cfs_period_us", buf);
    if (cg_write(path, "%ld", period) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/cpu.cfs_quota_us", buf);
    if (cg_write(path, "%ld", quota) != 0) {
        return -1;
    }

    /* 4. 加入进程 */
    snprintf(path, sizeof(path), "%s/cgroup.procs", buf);
    if (cg_write(path, "%d", pid) != 0) {
        return -1;
    }
    return 0;
}

/**
 * 清理 cgroup 目录
 */

static int cgroup_v1_cleanup(const char *group) {
    char path[512];
    char procs_path[512];

    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu/%s", group);

    if (access(path, F_OK) != 0) {
        return 0;
    }

    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", path);

    // 最多 retry 100 次
    for (int retry = 0; retry < 100; retry++) {
        FILE *fp = fopen(procs_path, "r");

        if (!fp) {
            fprintf(stderr, "Error: open %s failed: %s\n", procs_path,
                    strerror(errno));
            break;
        }

        char line[64];

        while (fgets(line, sizeof(line), fp)) {
            pid_t pid = (pid_t)atoi(line);

            if (pid <= 0) {
                continue;
            }

            // SIGKILL 整个进程
            if (kill(pid, SIGKILL) != 0) {
                // ESRCH = 已不存在
                if (errno != ESRCH) {
                    fprintf(stderr, "Error: kill(%d) failed: %s\n", pid,
                            strerror(errno));
                }
            }
        }

        fclose(fp);

        // 再检查一次是否为空
        fp = fopen(procs_path, "r");

        if (!fp) {
            break;
        }

        int empty = (fgets(line, sizeof(line), fp) == NULL);

        fclose(fp);

        if (empty) {
            break;
        }

        // 给 kernel 时间清理 zombie / threads
        usleep(10000);
    }

    // 最后删除 cgroup
    if (rmdir(path) != 0) {
        fprintf(stderr, "Error: rmdir %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

void cgroup_v1_clean_folder(char const *spool_dir) {
    DIR *dir = opendir(spool_dir);
    if (!dir) {
        fprintf(stderr, "Error: cannot open dir %s: %s\n", spool_dir, strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int jobid;
        int pid;

        if (sscanf(ent->d_name, "TASK_SPOOLER_%d_%d", &jobid, &pid) != 2) {
            continue;
        }
        if (s_check_running_pid(pid) == 0) {
            printf("[CLEAN] dead task: %s\n", ent->d_name);
            cgroup_v1_cleanup(ent->d_name);
        }
    }
}

void cgroups_clean_all_finished() {
    cgroup_v1_clean_folder("/sys/fs/cgroup/cpu/");
}

// 调度器收到任务
void cgroups_create_job(const struct Job *p) {
    if (p->pid == 0) {
        printf("cannot set cgroups for group: missing PID\n");
        return;
    }
    cgroup_v1_limit_cpu(p->jobid, p->pid, p->num_allocated);
}

// 任务结束
void cgroups_clean_job(const struct Job *p) {
    int jobid = p->jobid;
    int pid = p->pid;
    if (pid == 0) {
        printf("cannot set cgroups for group: missing PID\n");
        return;
    }
    if (s_check_running_pid(pid) == 1) {
        printf("Cannot clear the cgroups for a RUNNING job[%d](PID: %d)\n",
               jobid, pid);
        return;
    }

    char group[64];
    snprintf(group, sizeof(group), SPOOL_PATTERN, p->jobid, p->pid);
    printf("clear cgroups: %s\n", group);
    cgroup_v1_cleanup(group);
}
