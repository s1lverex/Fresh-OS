#!/bin/bash
# static analysis of all Fresh OS sources; prints findings, exit 1 if any
cd "$(dirname "$0")/.."
INC=$(clang -print-file-name=include)
KF="--target=x86_64-unknown-none-elf -std=gnu11 -ffreestanding -nostdinc -isystem $INC -fno-pic -mcmodel=kernel -mno-red-zone -mno-sse -Iinclude -Ikernel -Ilib -DFRESH_KERNEL"
UF="--target=x86_64-unknown-none-elf -std=gnu11 -ffreestanding -nostdinc -isystem $INC -Iinclude -Iuser/libc/include -Ilib"
n=0
for f in $(find kernel lib -name '*.c' | sort); do
  out=$(clang --analyze $KF $f -o /dev/null 2>&1)
  if [ -n "$out" ]; then echo "$out"; n=$((n+1)); fi
done
for f in user/libc/*.c user/bin/*.c user/test/*.c; do
  out=$(clang --analyze $UF $f -o /dev/null 2>&1)
  if [ -n "$out" ]; then echo "$out"; n=$((n+1)); fi
done
echo "static analysis: $n files with findings"
[ $n -eq 0 ]
