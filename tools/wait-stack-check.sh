#!/bin/sh
# wait-stack-check.sh <llvm bin dir> <kernel ELF>
# k_sleep_thread and k_wait_sema switch threads from inside (the switch
# syscall) and must keep nothing on the stack over it: with a frame there
# (the compiler saves ra and s0-s3 as soon as they call a function that is
# not inlined), a program that started another and waited for it came back
# at address 1 (found 2026-11-13). Fails if either touches sp.
n=$("$1/llvm-objdump" -d --no-show-raw-insn "$2" |
    awk '/<k_wait_sema>:/{p=1;next} /<k_sleep_thread>:/{p=1;next} /^[0-9a-f]+ <.*>:$/{p=0} p && /\$sp/' | wc -l)
if [ "$n" != 0 ]; then
    echo "wait-stack-check: k_sleep_thread or k_wait_sema uses the stack in $2 ($n instructions):" >&2
    echo "  they must stay leaf functions: inline what they call (ee/ksched.c)" >&2
    exit 1
fi
