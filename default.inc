#define DEFAULT_USER_PATH "/home/kylin/Public/task-spooler/user.txt"
#define DEFAULT_LOG_PATH "/home/kylin/Public/task-spooler/log.txt"
#define DEFAULT_SQLITE_PATH "/home/kylin/Public/task-spooler/task-spooler.db"
#define DEFAULT_EMAIL_SENDER "XXX@foxmail.com"
#define DEFAULT_EMAIL_TIME 45.0
#define DEFAULT_USER_LOCK_TIME 30
#define DEFAULT_ROOT_LOCK_TIME 86400
#define DEFAULT_MAX_WALL_TIME 604800
#define DEFAULT_HPC_NAME "intel_laptop"

enum { MAXCONN = 1000 };
enum { DEFAULT_MAXFINISHED = 1000 };

#define DEFAULT_NOTIFICATION_SOUND "/home/kylin/Public/task-spooler/notifications-sound.wav"
#define DEFAULT_ERROR_SOUND "/home/kylin/Public/task-spooler/error.wav"
#define DEFAULT_PULSE_SERVER "unix:/mnt/wslg/PulseServer"