#!/usr/bin/env python3
"""Run HMP commands through a QEMU QMP unix socket and print the output.

QMON_CPU=n runs them with CPU n selected (1 = the modem core, baseband=lle).
"""
import json, os, socket, sys
sock, cmds = sys.argv[1], sys.argv[2:]
s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile('rw')
def rx():
    while True:
        m = json.loads(f.readline())
        if 'return' in m or 'error' in m:
            return m
json.loads(f.readline())
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); rx()
for c in cmds:
    args = {"command-line": c}
    if os.environ.get("QMON_CPU"):
        args["cpu-index"] = int(os.environ["QMON_CPU"])
    f.write(json.dumps({"execute": "human-monitor-command",
                        "arguments": args}) + "\n"); f.flush()
    r = rx()
    print(r.get('return', r.get('error')), end='' if isinstance(r.get('return'), str) else '\n')
