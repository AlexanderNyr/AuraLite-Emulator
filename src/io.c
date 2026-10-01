// io.c -- 64K I/O port space
#include <string.h>
#include "machine.h"

void io_init(machine_t *m) {
    memset(m->io, 0, sizeof m->io);
}

void io_register(machine_t *m, uint16_t port, int count, io_read_fn r, io_write_fn w, void *ctx, const char *name) {
    for (int i = 0; i < count; i++) {
        m->io[(uint16_t)(port + i)].read = r;
        m->io[(uint16_t)(port + i)].write = w;
        m->io[(uint16_t)(port + i)].ctx = ctx;
        m->io[(uint16_t)(port + i)].name = name;
    }
}

uint32_t io_read(machine_t *m, uint16_t port, int size) {
    io_port_t *p = &m->io[port];
    if (p->read) return p->read(p->ctx, port, size);
    /* unconnected port: real hardware floats high */
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
}

void io_write(machine_t *m, uint16_t port, int size, uint32_t val) {
    io_port_t *p = &m->io[port];
    if (p->write) p->write(p->ctx, port, size, val);
}
