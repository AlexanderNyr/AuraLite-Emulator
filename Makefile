CC ?= gcc
# C10: -Werror is on; the whole tree builds warning-free (measured in the
# C10 patch message) and CI keeps it that way.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS ?= -Isrc

SRC := $(wildcard src/*.c)

.DELETE_ON_ERROR:
.PHONY: all clean firmware demo test test-unit test-boot test-table test-sanitize fuzz
all: x86emu

# C11 differential fuzzer knobs (bigger numbers = nightly runs)
FUZZ_PROGRAMS ?= 512
FUZZ_INSTR    ?= 24

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

test-unit: tests/test_cpu.c tests/test_usb.c tests/test_pic.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c src/devices.c src/pic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_cpu.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c -o test-cpu
	./test-cpu
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_usb.c src/devices.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c src/pic.c -o test-usb -lm
	./test-usb
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_pic.c src/pic.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-pic
	./test-pic
	rm -f test-cpu test-usb test-pic

# C10: table-driven ISA vectors (>=300 rows; row count printed by the runner)
test-table: tests/test_table.c tests/harness.h src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_table.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c -o test-table
	./test-table
	rm -f test-table

test-boot: x86emu firmware/firmware.bin disk/disk.img
	@set -e; ./x86emu --platform=haswell --max-instr=600000 >/tmp/x86emu-boot.out 2>/tmp/x86emu-boot.err; cat /tmp/x86emu-boot.err; grep -q '\[usb-msc\] READ(10) LBA=0 blocks=1' /tmp/x86emu-boot.out; grep -q '\[usb-msc\] bulk-IN 512 bytes -> guest 0x00100000' /tmp/x86emu-boot.out; grep -q '\[usb-msc\] synthesized CSW (status=OK)' /tmp/x86emu-boot.out; grep -q '\[guest\] conventional entry reached at 0x00100000' /tmp/x86emu-boot.out; grep -q '\[guest\] framebuffer marker OK: pixel\[0\]=0x00200000' /tmp/x86emu-boot.out; grep -q '\[serial\] GUEST_OK' /tmp/x86emu-boot.out; echo "boot integration test: ok"

test: test-unit test-table test-boot
	@set -e; for p in sandybridge ivybridge haswell broadwell baytrail; do echo "== $$p =="; ./x86emu --platform=$$p --max-instr=20000 >/tmp/x86emu-$$p.out 2>/tmp/x86emu-$$p.err; grep -E 'CPU state|STATE:|usb-msc|FAULT' /tmp/x86emu-$$p.out /tmp/x86emu-$$p.err || true; done

# C10 sanitizer lanes: same suites under ASan+UBSan. CI runs this target.
test-sanitize:
	$(MAKE) clean
	$(MAKE) test-unit test-table test-boot CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"

# C11 differential fuzzer vs the host CPU (ptrace single-step) + crash
# invariant sweep. Deliberately NOT part of 'make test': needs ptrace.
fuzz: tests/diff_fuzz.c tests/harness.h src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/diff_fuzz.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c -o diff_fuzz
	./diff_fuzz $(FUZZ_PROGRAMS) $(FUZZ_INSTR)
	rm -f diff_fuzz

clean:
	rm -f x86emu test-cpu test-usb test-table test-pic framebuffer.ppm
