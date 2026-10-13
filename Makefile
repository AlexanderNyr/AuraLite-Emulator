CC ?= gcc
# C10: -Werror is on; the whole tree builds warning-free (measured in the
# C10 patch message) and CI keeps it that way.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS ?= -Isrc

SRC := $(wildcard src/*.c)

.DELETE_ON_ERROR:
.PHONY: all clean firmware demo test test-unit test-boot test-table test-sanitize fuzz test-video-modes test-net-tx test-net-rx test-net-e2e test-tap-responder test-perf bench bench-nc bench-binary test-perf-mem test-perf-mem-nc test-tlb test-tlb-nc test-decode test-decode-nc test-perf-compare test-perf-compare-nc bench-compare test-perf-history
all: x86emu

# PERF P0 bench knobs.  Repetitions must be odd so the median is an actual
# sample rather than an average of two; the script rejects an even value.
BENCH_REPS    ?= 5
BENCH_CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
BENCH_PAIR_REPS ?= 9

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

test-unit: tests/test_cpu.c tests/test_usb.c tests/test_video.c tests/test_net.c tests/test_pic.c tests/test_pit.c tests/test_rtc.c tests/test_kbc.c tests/test_serial.c tests/test_chipset.c tests/test_lapic.c tests/test_ioapic.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c src/devices.c src/vga_font.h src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c tests/test_acpi.c src/acpi.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_cpu.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-cpu -lm
	./test-cpu
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_usb.c src/ahci.c src/devices.c src/e1000.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/serial.c -o test-usb -lm
	./test-usb
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_video.c src/ahci.c src/devices.c src/e1000.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/serial.c -o test-video -lm
	./test-video
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_net.c src/ahci.c src/devices.c src/e1000.c src/cpu.c src/mem.c src/io.c src/platform.c src/pci.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/serial.c -o test-net -lm
	./test-net
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_pic.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-pic -lm
	./test-pic
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_pit.c src/pit.c src/pic.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-pit -lm
	./test-pit
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_rtc.c src/rtc.c src/pit.c src/pic.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-rtc -lm
	./test-rtc
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_kbc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/pic.c src/pit.c src/rtc.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-kbc -lm
	./test-kbc
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_serial.c src/serial.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-serial -lm
	./test-serial
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_chipset.c src/chipset.c src/lapic.c src/ioapic.c src/kbc.c src/pic.c src/pit.c src/rtc.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-chipset -lm
	./test-chipset
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_lapic.c src/lapic.c src/ioapic.c src/chipset.c src/kbc.c src/pic.c src/pit.c src/rtc.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-lapic -lm
	./test-lapic
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_ioapic.c src/ioapic.c src/lapic.c src/chipset.c src/kbc.c src/pic.c src/pit.c src/rtc.c src/cpu.c src/mem.c src/io.c src/platform.c -o test-ioapic -lm
	./test-ioapic
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_acpi.c src/acpi.c -o test-acpi
	./test-acpi
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_ahci.c src/ahci.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c src/cpu.c -o test-ahci -lm
	./test-ahci
	rm -f test-cpu test-usb test-video test-net test-pic test-pit test-rtc test-kbc test-serial test-chipset test-lapic test-ioapic test-acpi test-ahci

# KERNEL-BOOT K1: direct kernel-load lane. The fixture is a synthetic
# higher-half kernel linked at the real AuraLite VMA (tests/kload_fixture.ld);
# its flags are frozen on purpose -- the guest image must not inherit the
# sanitizers (it runs inside the emulated machine, not on the host).
tests/kload_fixture.elf: tests/kload_fixture.c tests/kload_fixture.ld
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pie \
	    -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-red-zone -nostdlib -no-pie \
	    -Wl,-z,max-page-size=0x1000 -T tests/kload_fixture.ld \
	    tests/kload_fixture.c -o $@

test-kload: tests/test_kload.c tests/kload_fixture.elf src/kloader.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_kload.c src/kloader.c src/acpi.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-kload -lm
	./test-kload
	rm -f test-kload

test-ring3: tests/test_ring3.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_ring3.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-ring3 -lm
	./test-ring3
	rm -f test-ring3

# C10: table-driven ISA vectors (>=300 rows; row count printed by the runner)
test-table: tests/test_table.c tests/harness.h src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_table.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-table -lm
	./test-table
	rm -f test-table

# PERF P0: the honest work counter (cpu.executed_steps) and machine_executed_total().
# Deterministic -- no vector here reads a clock; wall time is bench's job.
test-perf: tests/test_perf.c tests/harness.h src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_perf.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-perf -lm
	./test-perf
	rm -f test-perf

# PERF P1: measured-hot 8-byte RAM read fast path.  Six deterministic
# bus/alias/endian/width vectors; the NC mutates a scratch source copy.
test-perf-mem: tests/test_perf_mem.c tests/harness.h src/mem.c src/cpu.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_perf_mem.c src/mem.c src/cpu.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-perf-mem -lm
	./test-perf-mem
	rm -f test-perf-mem

# Deliberately failing, never in `make test`: inner assertion failure is the
# proof that swapping the fast path's LE bytes changes guest-visible state.
test-perf-mem-nc: tests/test_perf_mem.c tests/integration/p1_mem_nc.sh src/mem.c
	sh tests/integration/p1_mem_nc.sh

# PERF P2: successful-translation TLB under page-table writes, CR3/
# INVLPG, fault error/RIP/CR2, mode/permission keys, PS leaves and SMP.
test-tlb: tests/test_tlb.c tests/harness.h src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_tlb.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-tlb -lm
	./test-tlb
	rm -f test-tlb

# Exactly one P2 NC.  The wrapper MUST fail: scratch mem.c without the
# RAM-store epoch bump returns a stale warmed PTE after guest/DMA writes.
test-tlb-nc: tests/test_tlb.c tests/integration/p2_tlb_nc.sh src/mem.c
	sh tests/integration/p2_tlb_nc.sh

# PERF P3: narrow RAM-backed prefix/opcode predecode, live ModRM and #PF.
test-decode: tests/test_decode.c tests/harness.h src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_decode.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o test-decode -lm
	./test-decode
	rm -f test-decode

# The one P3 NC removes ONLY the P3 generation check, leaving the P2
# translation invalidation intact; the same SMC vector must then go red.
test-decode-nc: tests/test_decode.c tests/integration/p3_decode_nc.sh src/cpu.c
	sh tests/integration/p3_decode_nc.sh

test-boot: x86emu firmware/firmware.bin disk/disk.img
	@set -e; ./x86emu --platform=haswell --max-instr=600000 >/tmp/x86emu-boot.out 2>/tmp/x86emu-boot.err; cat /tmp/x86emu-boot.err; grep -q '\[usb-msc\] READ(10) LBA=0 blocks=1' /tmp/x86emu-boot.out; grep -q '\[usb-msc\] bulk-IN 512 bytes -> guest 0x00100000' /tmp/x86emu-boot.out; grep -q '\[usb-msc\] synthesized CSW (status=OK)' /tmp/x86emu-boot.out; grep -q '\[guest\] conventional entry reached at 0x00100000' /tmp/x86emu-boot.out; grep -q '\[guest\] framebuffer marker OK: pixel\[0\]=0x00200000' /tmp/x86emu-boot.out; grep -q '\[serial\] GUEST_OK' /tmp/x86emu-boot.out; echo "boot integration test: ok"

# NET N1: the TX wire lane.  The payload (tests/nic_tx.asm) rides the real
# firmware-boot transport off a throwaway --disk= image, so this needs nasm +
# python3 (it SKIPs, exit 0, when either is missing) and the ground-rule TAP
# endpoint UDP 127.0.0.1:8080 free on the host.  The frozen-guest lane inside
# it SKIPs unless KERNEL/INITRD point at the frozen AuraLite-OS build.
test-net-tx: x86emu firmware/firmware.bin tests/nic_tx.asm \
             tests/integration/n1_tx_tap.sh tests/integration/n1_tap_responder.py
	sh tests/integration/n1_tx_tap.sh

# NET N2: the RX wire lane + the host responder.  tools/tap_responder.py is the
# host side of the --tap= socket: it holds slirp's user-mode addressing (guest
# 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3) and answers exactly the three
# protocols the frozen guest speaks.  Its protocol vectors are pure Python -- no
# sockets, no emulator -- so test-tap-responder runs everywhere python3 does.
# The wire payload rides the K1 kernel-load transport (--kernel=), not N1's
# firmware-boot single 512-byte block: DHCP + ARP + ICMP together are far more
# than 512 bytes of code, and the firmware memory map (RAM 0x100000-0x1FFFFF
# only) leaves no room for a 64-deep RX ring.  tests/nic_net.c reproduces the
# frozen guest's own protocol code, byte for byte on the wire, from
# AuraLite-OS@0ed0d29 kernel/net/net.c and drivers/e1000/e1000.c.
tests/nic_net.elf: tests/nic_net.c tests/nic_net.ld
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pie \
	    -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-red-zone -nostdlib -no-pie \
	    -Wl,-z,max-page-size=0x1000 -T tests/nic_net.ld \
	    tests/nic_net.c -o $@

test-tap-responder: tools/tap_responder.py tests/test_tap_responder.py
	python3 tests/test_tap_responder.py

test-net-rx: x86emu tests/nic_net.elf tools/tap_responder.py \
             tests/test_tap_responder.py tests/integration/n2_responder.sh
	sh tests/integration/n2_responder.sh

# NET N3: the end-to-end close-out.  Same payload and same responder as N2, but
# the tape now runs what the frozen guest runs at boot once DHCP has succeeded
# (kernel/kernel.c:428-430): net_self_test (the ping N2 closed),
# net_dns_self_test and tcp_x5_self_test.  So the responder grew a DNS answer
# (UDP/53 plus the RFC 1035 TCP carriage) and a TCP connection table -- 16
# concurrent connections to 10.0.2.3:53, opened in sequence, held, closed -- and
# it answers who-has for any on-link address, not just the gateway, because
# net_arp_resolve ARPs the TARGET for a local address (net.c:405+).
# 76 frames on the wire, byte-identical twice; --no-tcp is the NC.
test-net-e2e: x86emu tests/nic_net.elf tools/tap_responder.py \
              tests/test_tap_responder.py tests/integration/n3_net_e2e.sh
	sh tests/integration/n3_net_e2e.sh

test: test-unit test-table test-kload test-ring3 test-perf test-perf-mem test-tlb test-decode test-perf-compare test-boot test-net-tx test-tap-responder test-net-rx test-net-e2e

# VIDEO V3: mode-switch (dispi) + V1 guest lanes, each twice, byte-identical
test-video-modes: x86emu
	bash tests/integration/test_video_modes.sh
	@set -e; for p in sandybridge ivybridge haswell broadwell baytrail; do echo "== $$p =="; ./x86emu --platform=$$p --max-instr=20000 >/tmp/x86emu-$$p.out 2>/tmp/x86emu-$$p.err; grep -E 'CPU state|STATE:|usb-msc|FAULT' /tmp/x86emu-$$p.out /tmp/x86emu-$$p.err || true; done

# C10 sanitizer lanes: same suites under ASan+UBSan. CI runs this target.
test-sanitize:
	$(MAKE) clean
	$(MAKE) test-unit test-table test-kload test-ring3 test-perf test-perf-mem test-tlb test-decode test-perf-compare test-boot CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"

# PERF P5: confidence intervals are reports; guest work/clock equality is a
# deterministic gate.  No implicit binary rebuild (sanitizers must not leak
# into one side).  Set PERF_BEFORE and PERF_AFTER to explicit O2 binaries.
test-perf-compare: tests/perf_compare.py tests/test_perf_compare.py
	python3 tests/test_perf_compare.py

test-perf-compare-nc: tests/integration/p5_compare_nc.sh tests/perf_compare.py tests/test_perf_compare.py
	sh tests/integration/p5_compare_nc.sh

test-perf-history: tests/integration/p5_history.sh firmware/firmware.bin disk/disk.img
	@test -n "$(PERF_BEFORE)" -a -n "$(PERF_AFTER)" || { echo 'set PERF_BEFORE and PERF_AFTER' >&2; exit 1; }
	bash tests/integration/p5_history.sh "$(PERF_BEFORE)" "$(PERF_AFTER)"

bench-compare: tests/perf_compare.py firmware/firmware.bin disk/disk.img
	@test -n "$(PERF_BEFORE)" -a -n "$(PERF_AFTER)" || { echo 'set PERF_BEFORE and PERF_AFTER to explicit binary paths' >&2; exit 1; }
	python3 tests/perf_compare.py --before "$(PERF_BEFORE)" --after "$(PERF_AFTER)" --reps=$(BENCH_PAIR_REPS)

# PERF P0: wall-clock bench.  NOT part of 'make test': its numbers are this
# machine's, so a gate on them would be a gate on the runner.  What it DOES
# gate is deterministic -- per-tape accounting identical across repetitions,
# no tape cut off by its --max-instr cap, and MIPS computed from executed
# instruction attempts rather than from the cap.  Compare ratios between two builds
# on one machine; never compare against a figure quoted in the docs.
# Always rebuild a separate O2 binary: `make test-sanitize` overwrites x86emu
# with an O1 ASan+UBSan binary (its timestamps still look up-to-date to make).
# Without this, the same `make bench` command can measure two DIFFERENT
# builds depending on which test target ran immediately before it.  No flags
# from CFLAGS/CPPFLAGS are silently inherited; BENCH_CFLAGS is explicit.
bench-binary:
	$(CC) $(BENCH_CFLAGS) -Isrc $(SRC) -o x86emu-bench -lm

bench: bench-binary firmware/firmware.bin disk/disk.img
	EMUX=./x86emu-bench bash tests/perf_bench.sh --reps=$(BENCH_REPS)

# The P0 negative control: the same measurement with the cap as the MIPS
# numerator.  Exits 1 by design and is not part of any passing gate.
bench-nc: bench-binary firmware/firmware.bin disk/disk.img
	EMUX=./x86emu-bench bash tests/perf_bench.sh --reps=$(BENCH_REPS) --nc

# C11 differential fuzzer vs the host CPU (ptrace single-step) + crash
# invariant sweep. Deliberately NOT part of 'make test': needs ptrace.
fuzz: tests/diff_fuzz.c tests/harness.h src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/diff_fuzz.c src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c -o diff_fuzz -lm
	./diff_fuzz $(FUZZ_PROGRAMS) $(FUZZ_INSTR)
	rm -f diff_fuzz

clean:
	rm -f x86emu x86emu-bench test-cpu test-usb test-video test-net test-table test-pic test-pit test-rtc test-kbc test-chipset test-lapic test-ioapic test-kload test-perf test-perf-mem test-tlb framebuffer.ppm tests/kload_fixture.elf
