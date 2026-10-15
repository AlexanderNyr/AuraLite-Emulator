/* monitor.h -- TOOLING T5: live HTTP/WebSocket monitor frontend. */
#ifndef MONITOR_H
#define MONITOR_H
#include "machine.h"

/* Listen on `port` or `host:port` (host defaults to 127.0.0.1) and attach
 * a non-blocking monitor frontend. Returns 0 or -1 with a stderr reason. */
int monitor_attach(machine_t *m, const char *spec);

#endif
