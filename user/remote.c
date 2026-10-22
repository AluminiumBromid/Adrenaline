/*
	Adrenaline
	Copyright (C) 2026, AluminiumBromid

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Remote savestate commands, for automated testing. A one-shot request file
// ux0:data/adrenaline_cmd.txt containing "loadstate N" or "savestate N" is picked up by a polling
// thread, checked (game running, menu closed, no savestate in progress, slot belongs to the running
// game) and executed with the same loadState()/saveState() the menu uses. Results are printed to
// the Vita log as "[remote] ...".

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "states.h"
#include "remote.h"
#include "../adrenaline_vita.h"

#define REMOTE_CMD_PATH "ux0:data/adrenaline_cmd.txt"
#define REMOTE_POLL_US (500 * 1000)
#define REMOTE_START_TIMEOUT_US (10 * 1000 * 1000)
#define REMOTE_DONE_TIMEOUT_US (60 * 1000 * 1000)

extern int menu_open;

static SceAdrenaline *get_adrenaline() {
	return (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
}

int remoteParseCommand(const char *text, int *is_load, int *num) {
	char verb[16];
	int n;
	char extra;
	if (sscanf(text, "%15s %d %c", verb, &n, &extra) != 2 || n < 0 || n >= MAX_STATES) {
		return -1;
	}
	if (strcmp(verb, "loadstate") == 0) {
		*is_load = 1;
	} else if (strcmp(verb, "savestate") == 0) {
		*is_load = 0;
	} else {
		return -1;
	}
	*num = n;
	return 0;
}

// Title stored in a savestate, or < 0 if the slot is empty or not a savestate.
static int read_state_title(int num, char *title, int size) {
	char path[128];
	makeSaveStatePath(path, num);

	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return fd;
	}
	AdrenalineStateHeader header;
	int read = sceIoRead(fd, &header, sizeof(header));
	sceIoClose(fd);
	if (read != sizeof(header) || header.magic != ADRENALINE_SAVESTATE_MAGIC) {
		return -1;
	}
	header.title[sizeof(header.title) - 1] = '\0';
	snprintf(title, size, "%s", header.title);
	return 0;
}

int remoteCheck(int is_load, int num, char *why, int size) {
	SceAdrenaline *adrenaline = get_adrenaline();

	if (menu_open) {
		snprintf(why, size, "the Adrenaline menu is open");
		return -1;
	}
	if (adrenaline->savestate_mode != SAVESTATE_MODE_NONE) {
		snprintf(why, size, "a savestate is already in progress");
		return -1;
	}
	if (!adrenaline->pops_mode && adrenaline->app_type != SCE_APP_TYPE_GAME && adrenaline->app_type != SCE_APP_TYPE_APP) {
		snprintf(why, size, "no game is running");
		return -1;
	}
	if (is_load) {
		char title[128];
		if (read_state_title(num, title, sizeof(title)) < 0) {
			snprintf(why, size, "slot %d is empty", num);
			return -1;
		}
		// loading another game's state would corrupt the running game
		if (strcmp(title, adrenaline->title) != 0) {
			snprintf(why, size, "slot %d is from \"%s\", running \"%s\"", num, title, adrenaline->title);
			return -1;
		}
	}
	return 0;
}

// Wait until the PSP side starts (savestate_mode set) and finishes (back to NONE).
static int wait_for_savestate(const char *verb, int num) {
	int waited = 0;
	while (get_adrenaline()->savestate_mode == SAVESTATE_MODE_NONE) {
		if (waited >= REMOTE_START_TIMEOUT_US) {
			sceClibPrintf("[remote] %s %d: PSP side did not start within %d s\n", verb, num, REMOTE_START_TIMEOUT_US / 1000000);
			return -1;
		}
		sceKernelDelayThread(100 * 1000);
		waited += 100 * 1000;
	}
	waited = 0;
	while (get_adrenaline()->savestate_mode != SAVESTATE_MODE_NONE) {
		if (waited >= REMOTE_DONE_TIMEOUT_US) {
			sceClibPrintf("[remote] %s %d: not finished after %d s\n", verb, num, REMOTE_DONE_TIMEOUT_US / 1000000);
			return -1;
		}
		sceKernelDelayThread(100 * 1000);
		waited += 100 * 1000;
	}
	sceClibPrintf("[remote] %s %d: done\n", verb, num);
	return 0;
}

static void handle_request() {
	char text[64];
	memset(text, 0, sizeof(text));

	SceUID fd = sceIoOpen(REMOTE_CMD_PATH, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return;
	}
	sceIoRead(fd, text, sizeof(text) - 1);
	sceIoClose(fd);
	// one-shot, even if the command fails
	sceIoRemove(REMOTE_CMD_PATH);

	char *nl = strpbrk(text, "\r\n");
	if (nl) {
		*nl = '\0';
	}

	int is_load, num;
	if (remoteParseCommand(text, &is_load, &num) < 0) {
		sceClibPrintf("[remote] invalid command \"%s\" (expected \"loadstate N\" or \"savestate N\", N 0-%d)\n", text, MAX_STATES - 1);
		return;
	}
	const char *verb = is_load ? "loadstate" : "savestate";

	char why[256];
	if (remoteCheck(is_load, num, why, sizeof(why)) < 0) {
		sceClibPrintf("[remote] %s %d: refused, %s\n", verb, num, why);
		return;
	}

	sceClibPrintf("[remote] %s %d: starting\n", verb, num);
	if (is_load) {
		loadState(num);
	} else {
		saveState(num);
	}
	wait_for_savestate(verb, num);
}

static int remote_thread(SceSize args, void *argp) {
	SceIoStat stat;
	while (1) {
		sceKernelDelayThread(REMOTE_POLL_US);
		if (sceIoGetstat(REMOTE_CMD_PATH, &stat) >= 0) {
			handle_request();
		}
	}
	return 0;
}

void remoteStart() {
	static int started = 0;
	if (started) {
		return;
	}
	SceUID thid = sceKernelCreateThread("AdrenalineRemote", remote_thread, 0x10000100, 0x2000, 0, 0, NULL);
	if (thid >= 0 && sceKernelStartThread(thid, 0, NULL) >= 0) {
		started = 1;
	}
}
