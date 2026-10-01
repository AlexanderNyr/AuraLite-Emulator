CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
CPPFLAGS ?= -Isrc

SRC := $(wildcard src/*.c)

.PHONY: all clean firmware demo test test-unit test-boot
all: x86emu

x86emu: $(SRC) src/cpu.h src/machine.h src/platform.h src/pci.h src/devices.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SRC) -o $@ -lm

firmware/firmware.bin: firmware/sample_firmware.nas
	nasm -f bin $< -o $@

disk/test_kernel.bin: disk/test_kernel.asm
	nasm -f bin $< -o $@

disk/disk.img: disk/test_kernel.bin
	python3 -c "k=open('disk/test_kernel.bin','rb').read(); open('disk/disk.img','wb').write(k+b'\\0'*(1024*1024-len(k)))"

demo: x86emu firmware/firmware.bin disk/disk.img
	./x86emu --platform=haswell --max-instr=200000 --dump-fb=framebuffer.ppm

test-unit: tests/test_cpu.c tests/test_usb.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c src/devices.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_cpu.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-cpu
	./test-cpu
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_usb.c src/devices.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c -o test-usb -lm
	./test-usb
	rm -f test-cpu test-usb

test-boot: x86emu firmware/firmware.bin disk/disk.img
	@set -e; ./x86emu --platform=haswell --max-instr=600000 >/tmp/x86emu-boot.out 2>/tmp/x86emu-boot.err; cat /tmp/x86emu-boot.err; grep -q '\[usb-msc\] READ(10) LBA=0 blocks=1' /tmp/x86emu-boot.out; grep -q '\[usb-msc\] bulk-IN 512 bytes -> guest 0x00100000' /tmp/x86emu-boot.out; grep -q '\[usb-msc\] synthesized CSW (status=OK)' /tmp/x86emu-boot.out; grep -q '\[guest\] conventional entry reached at 0x00100000' /tmp/x86emu-boot.out; grep -q '\[guest\] framebuffer marker OK: pixel\[0\]=0x00200000' /tmp/x86emu-boot.out; grep -q '\[serial\] GUEST_OK' /tmp/x86emu-boot.out; echo "boot integration test: ok"

test: test-unit test-boot
	@set -e; for p in sandybridge ivybridge haswell broadwell baytrail; do echo "== $$p =="; ./x86emu --platform=$$p --max-instr=20000 >/tmp/x86emu-$$p.out 2>/tmp/x86emu-$$p.err; grep -E 'CPU state|STATE:|usb-msc|FAULT' /tmp/x86emu-$$p.out /tmp/x86emu-$$p.err || true; done

clean:
	rm -f x86emu test-cpu framebuffer.ppm
	rm -f x86emu framebuffer.ppm
