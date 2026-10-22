#ifndef __REMOTE_H__
#define __REMOTE_H__

// Remote savestate commands (ux0:data/adrenaline_cmd.txt), see remote.c.
void remoteStart();

// Exposed for tests.
int remoteParseCommand(const char *text, int *is_load, int *num);
int remoteCheck(int is_load, int num, char *why, int size);

#endif
