SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

$SHELL_FOLDER/output/qemu/bin/qemu-system-riscv64 \
# -M nanshan \
# -m 1G \
# -smp 8 \
# -nographic --parallel none