#ifndef DEVICES_H
#define DEVICES_H
#include "machine.h"

void devices_init_common(machine_t *m);     /* LPC, SATA, GPU, EHCI, CMOS, SuperIO, CAR window */
void devices_init_platform(machine_t *m);   /* DDR controller models for the selected platform */
void devices_add_ahci2(machine_t *m);       /* STORE S4: second AHCI controller (--ahci2) */
void devices_done(machine_t *m);            /* C10: frees every mmio region + rw2 window + PCI list */

/* VIDEO V1: VGA text mode 3 display (see devices.c VGA section). */
void vga_set_display_mode(machine_t *m, int text);      /* text=1: mode 3 */
int  vga_display_is_text(machine_t *m);
void vga_render_display(machine_t *m, uint32_t *px, int w, int h); /* display -> 0x00RRGGBB */
void vga_dump_text(machine_t *m, const char *path);     /* mode-3 console as text */

/* VIDEO V3: harness-side VBE client -- programs the dispi through the same
 * 0x1CE/0x1CF ports a guest client uses (mode-set / mode-set-off). */
void vbe_mode_set(machine_t *m, int w, int h, int bpp);
void vbe_mode_disable(machine_t *m);

/* NET N0: e1000 82540EM "RX-ready" model + TAP socket ingress (src/e1000.c).
 * The device is opt-in: e1000_test_add is the socketless unit entry, and
 * e1000_attach binds the fixed TAP endpoint (ground rule 127.0.0.1:8080).
 * NET N1 adds the TX half (TDT/TCTL doorbell -> TDH walk -> TAP datagram,
 * descriptors completed with status.DD inside the doorbell store, because
 * the guest's e1000_send() spins on DD with a bounded PAUSE budget). */
void e1000_test_add(machine_t *m);
int  e1000_attach(machine_t *m, const char *hostport);

/* VIRTIO V4: modern virtio-net (src/virtio_net.c on the V1 core).
 * Same opt-in/TAP discipline as the e1000, mutually exclusive
 * with it (one TAP endpoint owner; main enforces). */
int  virtio_net_attach(machine_t *m, const char *hostport);
void virtio_net_test_add(machine_t *m);
void virtio_net_tick(void);

/* VIRTIO V2: modern virtio-blk (src/virtio_blk.c on the V1 core).
 * Opt-in like the e1000: no PCI function exists until attached.
 * The test entry takes a caller-owned image; the CLI entry reads
 * the whole host file into RAM (the sata_img convention). */
int  virtio_blk_attach(machine_t *m, const char *path);
void virtio_blk_test_add(machine_t *m, uint8_t *img, size_t len);
void e1000_rx_frame(machine_t *m, const uint8_t *frame, unsigned len);
void e1000_tick(void);
void e1000_tx_stats(uint64_t *sent, uint64_t *dropped);   /* N1 wire counters */
void devices_tick(machine_t *m);            /* USB U3: async-schedule sample at the instruction cadence */

/* Decode the big-endian 16-bit fields used by SCSI READ(10). */
uint16_t usb_msc_be16(const uint8_t *p);

/* XHCI X4: the shared MSC stick's transfer engine (devices.c usb_serve,
 * U1-U5).  ctl selects the per-controller BOT state (0 EHCI, 1 UHCI,
 * 2 XHCI) -- one stick instance per controller, as UHCI/EHCI already
 * do.  pid: 0 OUT, 1 IN, 2 SETUP.  Returns bytes moved into the guest
 * buffer for IN transfers. */
uint32_t usb_stick_serve(machine_t *m, int ctl, int pid, uint32_t total, uint32_t buf);

#endif
