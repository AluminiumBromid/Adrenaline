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

// Remote commands, for automated testing. A one-shot request file ux0:data/adrenaline_cmd.txt is
// picked up by a polling thread:
//   "loadstate N" / "savestate N": checked (game running, menu closed, no savestate in progress,
//     slot belongs to the running game), then the menu's own loadState()/saveState()
//   "screenshot": the current PSP frame, unscaled, to ux0:data/adrenaline_shot.bin (see REMOTE_SHOT)
// Results are printed to the Vita log as "[remote] ...".

#include <psp2/compat.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "menu.h"
#include "states.h"
#include "remote.h"
#include "../adrenaline_vita.h"

#define REMOTE_CMD_PATH "ux0:data/adrenaline_cmd.txt"
#define REMOTE_POLL_US (500 * 1000)
#define REMOTE_START_TIMEOUT_US (10 * 1000 * 1000)
#define REMOTE_DONE_TIMEOUT_US (60 * 1000 * 1000)

// Screenshot file: header + rows of width * bytes_per_pixel (no stride padding).
// Written to a .tmp file first, then renamed, so readers never see a partial file.
#define REMOTE_SHOT_PATH "ux0:data/adrenaline_shot.bin"
#define REMOTE_SHOT_TMP_PATH "ux0:data/adrenaline_shot.tmp"
#define REMOTE_SHOT_MAGIC 0x54485341 // "ASHT"

enum RemoteShotFormat {
	REMOTE_SHOT_RGBA8888 = 0, // 4 bytes per pixel as in memory (R, G, B, A)
	REMOTE_SHOT_RGB565 = 1,   // 2 bytes per pixel, little endian
};

enum RemoteShotSource {
	REMOTE_SHOT_PSP = 0,    // PSP framebuffer copy, 480x272 (games, XMB)
	REMOTE_SHOT_NATIVE = 1, // "draw native" framebuffer, 960x544
	REMOTE_SHOT_POPS = 2,   // PSOne output, 960x544
};

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t source;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t reserved[2];
} RemoteShotHeader;

extern int menu_open;
extern void *pops_data;

static SceAdrenaline *get_adrenaline() {
	return (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
}

int remoteParseCommand(const char *text, int *cmd, int *num) {
	char verb[16];
	int n;
	char extra;
	int fields = sscanf(text, "%15s %d %c", verb, &n, &extra);
	if (fields == 1 && strcmp(verb, "screenshot") == 0) {
		*cmd = REMOTE_CMD_SCREENSHOT;
		*num = 0;
		return 0;
	}
	if (fields != 2 || n < 0 || n >= MAX_STATES) {
		return -1;
	}
	if (strcmp(verb, "loadstate") == 0) {
		*cmd = REMOTE_CMD_LOADSTATE;
	} else if (strcmp(verb, "savestate") == 0) {
		*cmd = REMOTE_CMD_SAVESTATE;
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

int remoteCheck(int cmd, int num, char *why, int size) {
	SceAdrenaline *adrenaline = get_adrenaline();
	int is_load = cmd == REMOTE_CMD_LOADSTATE;

	if (cmd == REMOTE_CMD_SCREENSHOT) {
		// works in the XMB too; only a running savestate swaps the screen content
		if (adrenaline->savestate_mode != SAVESTATE_MODE_NONE) {
			snprintf(why, size, "a savestate is in progress");
			return -1;
		}
		return 0;
	}
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

static int write_rows(SceUID fd, const uint8_t *base, int row_bytes, int stride_bytes, int rows) {
	for (int y = 0; y < rows; y++) {
		if (sceIoWrite(fd, base + y * stride_bytes, row_bytes) != row_bytes) {
			return -1;
		}
	}
	return 0;
}

// The same three sources saveFrameBuffer() (states.c) uses for savestate thumbnails, at full size.
int remoteScreenshot(char *info, int size) {
	SceAdrenaline *adrenaline = get_adrenaline();
	int draw_native = *(uint32_t *)CONVERT_ADDRESS(DRAW_NATIVE);

	RemoteShotHeader header;
	memset(&header, 0, sizeof(header));
	header.magic = REMOTE_SHOT_MAGIC;
	header.version = 1;

	const uint8_t *base;
	int bpp, stride;
	if (draw_native) {
		header.source = REMOTE_SHOT_NATIVE;
		header.format = REMOTE_SHOT_RGB565;
		header.width = SCREEN_WIDTH;
		header.height = SCREEN_HEIGHT;
		base = (const uint8_t *)CONVERT_ADDRESS(NATIVE_FRAMEBUFFER);
		bpp = 2;
		stride = SCREEN_WIDTH;
	} else if (adrenaline->pops_mode) {
		header.source = REMOTE_SHOT_POPS;
		header.format = REMOTE_SHOT_RGBA8888;
		header.width = SCREEN_WIDTH;
		header.height = SCREEN_HEIGHT;
		base = (const uint8_t *)pops_data;
		bpp = 4;
		stride = SCREEN_LINE;
	} else {
		header.source = REMOTE_SHOT_PSP;
		header.format = REMOTE_SHOT_RGBA8888;
		header.width = PSP_SCREEN_WIDTH;
		header.height = PSP_SCREEN_HEIGHT;
		base = (const uint8_t *)SCE_PSPEMU_FRAMEBUFFER;
		bpp = 4;
		stride = PSP_SCREEN_LINE;
	}
	if (base == NULL) {
		snprintf(info, size, "no framebuffer for this mode yet");
		return -1;
	}

	SceUID fd = sceIoOpen(REMOTE_SHOT_TMP_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		snprintf(info, size, "cannot create %s (0x%08X)", REMOTE_SHOT_TMP_PATH, fd);
		return fd;
	}
	sceCompatLCDCSync(); // as saveFrameBuffer(): read between frames
	int res = sceIoWrite(fd, &header, sizeof(header)) == sizeof(header) ? 0 : -1;
	if (res == 0) {
		res = write_rows(fd, base, header.width * bpp, stride * bpp, header.height);
	}
	sceIoClose(fd);
	if (res < 0) {
		sceIoRemove(REMOTE_SHOT_TMP_PATH);
		snprintf(info, size, "write failed");
		return res;
	}
	sceIoRemove(REMOTE_SHOT_PATH);
	if (sceIoRename(REMOTE_SHOT_TMP_PATH, REMOTE_SHOT_PATH) < 0) {
		snprintf(info, size, "rename failed");
		return -1;
	}
	static const char *sources[] = {"psp", "native", "pops"};
	snprintf(info, size, "%ux%u %s", (unsigned int)header.width, (unsigned int)header.height, sources[header.source]);
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

	int cmd, num;
	if (remoteParseCommand(text, &cmd, &num) < 0) {
		sceClibPrintf("[remote] invalid command \"%s\" (expected \"loadstate N\", \"savestate N\" (N 0-%d) or \"screenshot\")\n", text, MAX_STATES - 1);
		return;
	}

	char why[256];
	if (cmd == REMOTE_CMD_SCREENSHOT) {
		if (remoteCheck(cmd, 0, why, sizeof(why)) < 0) {
			sceClibPrintf("[remote] screenshot: refused, %s\n", why);
		} else if (remoteScreenshot(why, sizeof(why)) < 0) {
			sceClibPrintf("[remote] screenshot: failed, %s\n", why);
		} else {
			sceClibPrintf("[remote] screenshot: done (%s)\n", why);
		}
		return;
	}

	const char *verb = cmd == REMOTE_CMD_LOADSTATE ? "loadstate" : "savestate";
	if (remoteCheck(cmd, num, why, sizeof(why)) < 0) {
		sceClibPrintf("[remote] %s %d: refused, %s\n", verb, num, why);
		return;
	}

	sceClibPrintf("[remote] %s %d: starting\n", verb, num);
	if (cmd == REMOTE_CMD_LOADSTATE) {
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
