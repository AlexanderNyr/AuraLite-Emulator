CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
CPPFLAGS ?= -Isrc

SRC := $(wildcard src/*.c)

.PHONY: all clean firmware demo test
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

test: x86emu firmware/firmware.bin disk/disk.img
	@set -e; for p in sandybridge ivybridge haswell broadwell baytrail; do echo "== $$p =="; ./x86emu --platform=$$p --max-instr=20000 >/tmp/x86emu-$$p.out 2>/tmp/x86emu-$$p.err; grep -E 'CPU state|STATE:|usb-msc|FAULT' /tmp/x86emu-$$p.out /tmp/x86emu-$$p.err || true; done

clean:
	rm -f x86emu framebuffer.ppm
