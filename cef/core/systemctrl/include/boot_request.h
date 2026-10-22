#ifndef _BOOT_REQUEST_H_
#define _BOOT_REQUEST_H_

// Boot the game named in ms0:/adrenaline_boot.bin (one-shot), see boot_request.c.
// Returns 0 when there is no (valid) request; does not return when the game boots.
int bootRequestRun();

#endif
