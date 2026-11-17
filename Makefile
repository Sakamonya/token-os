CC      := clang
LD      := ld.lld
TARGET  := --target=aarch64-none-elf
CFLAGS  := $(TARGET) -ffreestanding -fno-stack-protector -fno-pic -mgeneral-regs-only \
           -O2 -Wall -Wextra -mstrict-align
LDFLAGS := -T linker.ld -nostdlib

QEMU    := qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256M -nographic

OBJS    := startup.o vectors.o main.o agent.o

all: check-key kernel.elf

# key 只在这个文件里存在，编译进镜像，不进仓库、不进日志
check-key:
	@test -f key.txt || (echo ""; \
	  echo "缺少 key.txt。做法："; \
	  echo "   cp key.txt.example key.txt"; \
	  echo "   把一行 API key 写进去（以 sk- 开头）"; \
	  echo ""; exit 1)

key.inc: key.txt
	@printf 'static const char API_KEY[] = "%s";\n' "$$(tr -d '\r\n' < key.txt)" > $@
	@echo "已把 key 编进内核镜像（长度 $$(tr -d '\r\n' < key.txt | wc -c) 字节）"

%.o: %.s
	$(CC) $(TARGET) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

main.o: key.inc
agent.o: key.inc

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# 交互式跑（Ctrl-A X 退出）
run: kernel.elf
	$(QEMU) -kernel kernel.elf

# 看门狗自测版：主循环会故意卡死 8 秒
stall.elf: $(OBJS:.o=.stall.o) linker.ld
	$(LD) $(LDFLAGS) startup.stall.o vectors.stall.o main.stall.o -o $@

%.stall.o: %.c
	$(CC) $(CFLAGS) -DSTALL_TEST -c $< -o $@

%.stall.o: %.s
	$(CC) $(TARGET) -c $< -o $@

stall: stall.elf
	@timeout 16 $(QEMU) -kernel stall.elf 2>&1 | head -40

# 跑 N 秒，输出同时进日志
SECS ?= 6
capture: kernel.elf
	@mkdir -p log
	@{ echo "# $$(date -Iseconds) token-os boot"; echo "# $(QEMU)"; } > log/latest.boot.log
	@timeout $(SECS) $(QEMU) -kernel kernel.elf >> log/latest.boot.log 2>/dev/null || true
	@cat log/latest.boot.log

# 真链路：内核 <-> 中转 <-> 云端
token: kernel.elf
	bash run-token.sh $(SECS)

clean:
	rm -f *.o *.elf key.inc

distclean: clean
	rm -rf log

.PHONY: all run capture clean distclean token stall check-key
