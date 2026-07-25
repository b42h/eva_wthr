#!/bin/sh
# Run all host-side C unit tests under tools/test_*.c.
set -e
cd "$(dirname "$0")/.."
fail=0
for t in tools/test_*.c; do
    n=$(basename "$t" .c)
    if cc -std=c11 -Wall -Wextra -lm -I main -o "/tmp/$n" "$t" 2>/tmp/$n.log; then
        if "/tmp/$n"; then
            echo "PASS $n"
        else
            echo "FAIL $n"
            fail=1
        fi
    else
        echo "BUILD-FAIL $n"
        cat /tmp/$n.log
        fail=1
    fi
done
exit $fail
