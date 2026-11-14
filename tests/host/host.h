/* Simulated platform for tests and host tools (tests/host/host_platform.c).
 * Time only moves when host_frame() is called: 1/60 s of audio per frame,
 * so a whole song runs in a fraction of a second and always the same way. */
#ifndef TP_HOST_H
#define TP_HOST_H

#include <stdbool.h>
#include <stdint.h>

bool host_open_library(const char *path);
void host_option(const char *name_value);          /* as "--set name=value" */
void host_frame(void);                             /* one app_frame() and 800 samples of audio */
void host_button(uint32_t button, bool down);      /* edge stamped with the current sample counter */
void host_tap_at(uint32_t button, int64_t counter); /* press and release, stamped `counter` */
const uint16_t *host_screen(void);                 /* the frame shown last */
int64_t host_counter(void);

#endif
