#!/usr/bin/env bash

# timer.sh
# Usage:
#   ./timer.sh            # 默认运行 10 秒
#   ./timer.sh 30         # 运行 30 秒

duration=${1:-10}

# 检查输入是否为正整数
if ! [[ "$duration" =~ ^[0-9]+$ ]] || [ "$duration" -le 0 ]; then
    echo "Error: please provide a positive integer number of seconds."
    exit 1
fi

start_time=$(date +%s)

for ((i=0; i<duration; i++)); do
    now=$(date '+%Y-%m-%d %H:%M:%S')
    elapsed=$i
    remaining=$((duration - i))

    printf "[%s] Elapsed: %3ds | Remaining: %3ds\n" \
        "$now" "$elapsed" "$remaining"

    sleep 1
done

end_time=$(date '+%Y-%m-%d %H:%M:%S')
echo "[$end_time] Timer completed successfully."

exit 0

