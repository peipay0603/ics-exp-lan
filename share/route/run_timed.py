#!/usr/bin/env python3
"""Docker起動時間を除き、全条件で同じ境界（子プロセス起動〜保存・終了）を測る。"""
import json
import resource
import subprocess
import sys
import time

start = time.monotonic()
result = subprocess.call(sys.argv[1:])
duration = time.monotonic() - start
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
print('ROUTE_METRICS ' + json.dumps({'seconds': duration, 'exit': result,
                                    'user_s': usage.ru_utime, 'system_s': usage.ru_stime}), flush=True)
sys.exit(result if result >= 0 else 128 - result)
