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

// One-shot boot request: when the XMB starts from a cold boot and ms0:/adrenaline_boot.bin
// exists, the file is deleted and the game it names is booted instead of showing the XMB.
// The file uses the layout of Adrenaline Bubble Manager's data/boot.bin; only magic, driver,
// execute and path are used.

#include <stdio.h>
#include <string.h>

#include <pspinit.h>
#include <pspiofilemgr.h>
#include <pspumd.h>

#include <systemctrl.h>
#include <systemctrl_se.h>
#include <systemctrl_adrenaline.h>

#include "externs.h"
#include "boot_request.h"

#define BOOT_REQUEST_PATH "ms0:/adrenaline_boot.bin"
#define BOOT_MAGIC "ABB"
#define BOOT_DRIVER_MARKER 0x31444242 // "BBD1": driver field uses the encoding below

enum BootDriver {
	BOOT_DRIVER_NP9660 = 0,
	BOOT_DRIVER_INFERNO = 1,
	BOOT_DRIVER_MARCH33 = 2,
};

enum BootExecute {
	BOOT_EXECUTE_EBOOT_BIN = 0,
	BOOT_EXECUTE_EBOOT_OLD = 1,
	BOOT_EXECUTE_BOOT_BIN = 2,
};

typedef struct {
	char magic[4];      // "ABB\0"
	u32 driver;         // BootDriver if driver_marker is set
	u32 execute;        // BootExecute
	u32 customized;
	u8 unused1[0x1C];   // CPU speed, savestate, ... (not used here)
	u32 driver_marker;  // BOOT_DRIVER_MARKER
	u8 unused2[0x10];
	char path[256];     // ISO/CSO or EBOOT.PBP, PSP ("ms0:/...") or Vita ("ux0:pspemu/...") style
} AdrBootBin;

_Static_assert(sizeof(AdrBootBin) == 0x140, "boot.bin layout");

// Print to the Vita side (psp2shell log), independent of the TTY redirection setting.
static void boot_log(const char *fmt, const char *arg) {
	snprintf(g_adrenaline->printbuf, sizeof(g_adrenaline->printbuf), fmt, arg);
	sctrlSendAdrenalineCmd(ADRENALINE_VITA_CMD_PRINT, 0);
}

// "ux0:pspemu/ISO/x.iso" or "ux0:/pspemu/ISO/x.iso" -> "ms0:/ISO/x.iso"
static void to_psp_path(const char *in, char *out, int size) {
	const char *colon = strchr(in, ':');
	if (colon) {
		const char *p = colon + 1;
		while (*p == '/') {
			p++;
		}
		if (strncmp(p, "pspemu/", 7) == 0) {
			snprintf(out, size, "ms0:/%s", p + 7);
			return;
		}
	}
	snprintf(out, size, "%s", in);
}

static int has_extension(const char *path, const char *ext) {
	int len = strlen(path), ext_len = strlen(ext);
	if (len < ext_len) {
		return 0;
	}
	for (int i = 0; i < ext_len; i++) {
		char c = path[len - ext_len + i];
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}
		if (c != ext[i]) {
			return 0;
		}
	}
	return 1;
}

static int is_iso(const char *path) {
	return has_extension(path, ".iso") || has_extension(path, ".cso") || has_extension(path, ".zso") ||
	       has_extension(path, ".dax") || has_extension(path, ".jso");
}

static int boot_iso(const char *path, const AdrBootBin *req) {
	static char exec[64];

	sctrlSESetUmdFile((char *)path);
	sctrlSESetDiscType(PSP_UMD_TYPE_GAME);

	int driver = -1;
	if (req->driver_marker == BOOT_DRIVER_MARKER) {
		driver = req->driver;
	} else if (g_cfw_config.umd_mode == ISO_MODE_MARCH33) {
		driver = BOOT_DRIVER_MARCH33;
	} else if (g_cfw_config.umd_mode == ISO_MODE_NP9660) {
		driver = BOOT_DRIVER_NP9660;
	} else if (g_cfw_config.umd_mode == ISO_MODE_INFERNO) {
		driver = BOOT_DRIVER_INFERNO;
	}
	if (driver == BOOT_DRIVER_MARCH33) {
		sctrlSESetBootConfFileIndex(MODE_MARCH33);
	} else if (driver == BOOT_DRIVER_NP9660) {
		sctrlSESetBootConfFileIndex(MODE_NP9660);
	} else if (driver == BOOT_DRIVER_INFERNO) {
		sctrlSESetBootConfFileIndex(MODE_INFERNO);
	} else if (g_cfw_config.umd_mode == ISO_MODE_ME) {
		sctrlSESetBootConfFileIndex(MODE_ME);
	}

	if (req->execute == BOOT_EXECUTE_EBOOT_OLD) {
		strcpy(exec, "disc0:/PSP_GAME/SYSDIR/EBOOT.OLD");
	} else if (req->execute == BOOT_EXECUTE_BOOT_BIN) {
		strcpy(exec, "disc0:/PSP_GAME/SYSDIR/BOOT.BIN");
	} else {
		strcpy(exec, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
	}

	SceKernelLoadExecVSHParam param;
	memset(&param, 0, sizeof(param));
	param.size = sizeof(param);
	param.argp = exec;
	param.args = strlen(exec) + 1;
	param.key = "umdemu";

	return sctrlKernelLoadExecVSHWithApitype(PSP_INIT_APITYPE_UMD_EMU_MS1, exec, &param);
}

static int boot_pbp(const char *path) {
	static char file[256];
	static char disc_eboot[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
	strcpy(file, path);

	// Started the way the XMB starts them (measured on the device), chosen by PARAM.SFO CATEGORY:
	// "EG" PSN game: UMD image inside the PBP, booted like a UMD from disc0: (0x123, "umdemu")
	// "ME" PSOne: 0x144, "pops"
	// else (e.g. "MG" homebrew): 0x141, "game"
	char category[4] = {0};
	u32 size = sizeof(category);
	sctrlGetSfoPARAM(file, "CATEGORY", NULL, &size, category);
	category[sizeof(category) - 1] = '\0';
	boot_log("[boot request] PBP category '%s'\n", category);

	SceKernelLoadExecVSHParam param;
	memset(&param, 0, sizeof(param));
	param.size = sizeof(param);

	int apitype;
	if (strcmp(category, "EG") == 0) {
		apitype = PSP_INIT_APITYPE_UMD_EMU_MS1;
		param.argp = disc_eboot;
		param.key = "umdemu";
	} else if (strcmp(category, "ME") == 0) {
		apitype = PSP_INIT_APITYPE_MS5;
		param.argp = file;
		param.key = "pops";
	} else {
		apitype = PSP_INIT_APITYPE_MS2;
		param.argp = file;
		param.key = "game";
	}
	param.args = strlen(param.argp) + 1;

	return sctrlKernelLoadExecVSHWithApitype(apitype, file, &param);
}

int bootRequestRun() {
	static AdrBootBin req;
	static char path[256];

	SceUID fd = sceIoOpen(BOOT_REQUEST_PATH, PSP_O_RDONLY, 0);
	if (fd < 0) {
		return 0;
	}
	memset(&req, 0, sizeof(req));
	int read = sceIoRead(fd, &req, sizeof(req));
	sceIoClose(fd);
	// one-shot: never boot the same request twice, even if it fails
	sceIoRemove(BOOT_REQUEST_PATH);

	req.path[sizeof(req.path) - 1] = '\0';
	if (read != sizeof(req) || strcmp(req.magic, BOOT_MAGIC) != 0) {
		boot_log("[boot request] invalid %s, ignored\n", BOOT_REQUEST_PATH);
		return 0;
	}

	to_psp_path(req.path, path, sizeof(path));
	SceIoStat stat;
	if (sceIoGetstat(path, &stat) < 0) {
		boot_log("[boot request] not found: %s\n", path);
		return 0;
	}

	boot_log("[boot request] booting %s\n", path);
	LoadExecForKernel_AA2029EC();
	int res = is_iso(path) ? boot_iso(path, &req) : boot_pbp(path);

	// only reached if LoadExec failed
	snprintf(g_adrenaline->printbuf, sizeof(g_adrenaline->printbuf), "[boot request] LoadExec failed: 0x%08X\n", res);
	sctrlSendAdrenalineCmd(ADRENALINE_VITA_CMD_PRINT, 0);
	return res;
}
