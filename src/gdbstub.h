// gdbstub.h -- TOOLING T3: GDB Remote Serial Protocol server.
//
// `--gdb=[host:]port` listens (default host 127.0.0.1), waits for one gdb
// connection, and registers as a T2 front end: every stop/resume goes
// through the run-control core (TOOLING_PLAN rule 4). Breakpoints are
// virtual (Z0/Z1 never patch guest memory), memory reads are the
// side-effect-free linear reads of src/dbg.c, vCPUs are threads, and
// `monitor CMD` runs the T2 command interpreter.
#ifndef GDBSTUB_H
#define GDBSTUB_H
#include "machine.h"

/* Listen on `spec` ("port" or "host:port"), block until gdb connects and
 * attach. Returns 0 on success, -1 (with a message on stderr) otherwise. */
int gdb_attach(machine_t *m, const char *spec);

#endif
