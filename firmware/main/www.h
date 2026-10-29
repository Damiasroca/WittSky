#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void www_start(void);
/* True once port 80 has run a heartbeat in the last 45 s. */
bool www_alive(void);

#ifdef __cplusplus
}
#endif
