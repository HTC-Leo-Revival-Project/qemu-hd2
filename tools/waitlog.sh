#!/bin/sh
# waitlog.sh MAX REGEX: poll ramcon.log until REGEX, a panic, or MAX seconds
cd "$(dirname "$0")/../images" || exit 1
T=0
while [ $T -lt "$1" ]; do
  sleep 2; T=$((T+2))
  grep -qaE "$2" ramcon.log 2>/dev/null && { echo "matched after ${T}s"; exit 0; }
  grep -qaE "Kernel panic|Internal error: Oops" ramcon.log 2>/dev/null && { echo "panic after ${T}s"; exit 2; }
done
echo "timeout"; exit 1
