/* monitor.h -- TOOLING T5: live HTTP/WebSocket monitor frontend. */
#ifndef MONITOR_H
#define MONITOR_H
#include "machine.h"

/* Listen on `port` or `host:port` (host defaults to 127.0.0.1) and attach
 * a non-blocking monitor frontend. Returns 0 or -1 with a stderr reason.
 *
 * UPKEEP U7: `token` is the capability a WebSocket client must present
 * (query `?token=`, `X-X86emu-Token`, or `Authorization: Bearer`); NULL
 * or empty falls back to the X86EMU_MONITOR_TOKEN environment variable.
 * A non-loopback bind without any token is refused outright. */
int monitor_attach(machine_t *m, const char *spec, const char *token);

#endif
