#ifndef __REMOTE_H__
#define __REMOTE_H__

// Remote commands (ux0:data/adrenaline_cmd.txt): savestates and screenshots, see remote.c.
void remoteStart();

enum RemoteCommand {
	REMOTE_CMD_SAVESTATE = 0,
	REMOTE_CMD_LOADSTATE = 1,
	REMOTE_CMD_SCREENSHOT = 2,
};

// Exposed for tests.
int remoteParseCommand(const char *text, int *cmd, int *num);
int remoteCheck(int cmd, int num, char *why, int size);
int remoteScreenshot(char *info, int size);

#endif
