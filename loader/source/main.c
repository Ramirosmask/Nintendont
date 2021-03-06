/*

Nintendont (Loader) - Playing Gamecubes in Wii mode on a Wii U

Copyright (C) 2013  crediar

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/lwp_threads.h>
#include <wiiuse/wpad.h>
#include <wupc/wupc.h>
#include <di/di.h>

#include <unistd.h>

#include "exi.h"
#include "dip.h"
#include "global.h"
#include "font.h"
#include "Config.h"
#include "FPad.h"
#include "menu.h"
#include "MemCard.h"
#include "Patches.h"
#include "kernel_bin.h"
#include "multidol_ldr_bin.h"
#include "stub_bin.h"
#include "titles.h"
#include "ipl.h"
#include "HID.h"
#include "TRI.h"

#include "ff_utf8.h"
#include "diskio.h"
// from diskio.c
extern DISC_INTERFACE *driver[_VOLUMES];

extern void __exception_setreload(int t);
extern void __SYS_ReadROM(void *buf,u32 len,u32 offset);
extern u32 __SYS_GetRTC(u32 *gctime);

extern syssram* __SYS_LockSram();
extern u32 __SYS_UnlockSram(u32 write);
extern u32 __SYS_SyncSram(void);

#define STATUS			((void*)0x90004100)
#define STATUS_LOADING	(*(vu32*)(0x90004100))
#define STATUS_SECTOR	(*(vu32*)(0x90004100 + 8))
#define STATUS_DRIVE	(*(float*)(0x9000410C))
#define STATUS_GB_MB	(*(vu32*)(0x90004100 + 16))
#define STATUS_ERROR	(*(vu32*)(0x90004100 + 20))

#define HW_DIFLAGS		0xD800180
#define MEM_PROT		0xD8B420A

#define IOCTL_ExecSuspendScheduler	1

static GXRModeObj *vmode = NULL;

static const unsigned char Boot2Patch[] =
{
    0x48, 0x03, 0x49, 0x04, 0x47, 0x78, 0x46, 0xC0, 0xE6, 0x00, 0x08, 0x70, 0xE1, 0x2F, 0xFF, 0x1E, 
    0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x25,
};
static const unsigned char AHBAccessPattern[] =
{
	0x68, 0x5B, 0x22, 0xEC, 0x00, 0x52, 0x18, 0x9B, 0x68, 0x1B, 0x46, 0x98, 0x07, 0xDB,
};
static const unsigned char AHBAccessPatch[] =
{
	0x68, 0x5B, 0x22, 0xEC, 0x00, 0x52, 0x18, 0x9B, 0x23, 0x01, 0x46, 0x98, 0x07, 0xDB,
};
static const unsigned char FSAccessPattern[] =
{
    0x9B, 0x05, 0x40, 0x03, 0x99, 0x05, 0x42, 0x8B, 
};
static const unsigned char FSAccessPatch[] =
{
    0x9B, 0x05, 0x40, 0x03, 0x1C, 0x0B, 0x42, 0x8B, 
};

// Forbid the use of MEM2 through malloc
u32 MALLOC_MEM2 = 0;

s32 __IOS_LoadStartupIOS(void)
{
	int res;

	res = __ES_Init();
	if(res < 0)
		return res;

	return 0;
}

// Storage devices. (defined in global.c)
// 0 == SD, 1 == USB
extern FATFS *devices[2];

extern vu32 FoundVersion;
vu32 KernelLoaded = 0;
u32 entrypoint = 0;
char launch_dir[MAXPATHLEN] = {0};
extern void __exception_closeall();
static u8 loader_stub[0x1800]; //save internally to prevent overwriting
static ioctlv IOCTL_Buf ALIGNED(32);
static const char ARGSBOOT_STR[9] ALIGNED(0x10) = {'a','r','g','s','b','o','o','t','\0'}; //makes it easier to go through the file
static const char NIN_BUILD_STRING[] ALIGNED(32) = NIN_VERSION_STRING; // Version detection string used by nintendont launchers "$$Version:x.xxx"

/**
 * Update meta.xml.
 */
static void updateMetaXml(void)
{
	char filepath[MAXPATHLEN];
	bool dir_argument_exists = strlen(launch_dir);
	
	snprintf(filepath, sizeof(filepath), "%smeta.xml",
		dir_argument_exists ? launch_dir : "/apps/Nintendont/");

	if (!dir_argument_exists) {
		gprintf("Creating new directory\r\n");
		f_mkdir_char("/apps");
		f_mkdir_char("/apps/Nintendont");
	}

	char new_meta[1024];
	int len = snprintf(new_meta, sizeof(new_meta),
		META_XML "\r\n<app version=\"1\">\r\n"
		"\t<name>" META_NAME "</name>\r\n"
		"\t<coder>" META_AUTHOR "</coder>\r\n"
		"\t<version>%d.%d%s</version>\r\n"
		"\t<release_date>20150531000000</release_date>\r\n"
		"\t<short_description>" META_SHORT "</short_description>\r\n"
		"\t<long_description>" META_LONG1 "\r\n\r\n" META_LONG2 "</long_description>\r\n"
		"\t<ahb_access/>\r\n"
		"</app>\r\n",
		NIN_VERSION >> 16, NIN_VERSION & 0xFFFF,
#ifdef NIN_SPECIAL_VERSION
		NIN_SPECIAL_VERSION
#else
		""
#endif
  			);
	if (len > sizeof(new_meta))
		len = sizeof(new_meta);

	// Check if the file already exists.
	FIL meta;
	if (f_open_char(&meta, filepath, FA_READ|FA_OPEN_EXISTING) == FR_OK)
	{
		// File exists. If it's the same as the new meta.xml,
		// don't bother rewriting it.
		char orig_meta[1024];
		if (len == meta.obj.objsize)
		{
			// File is the same length.
			UINT read;
			f_read(&meta, orig_meta, len, &read);
			if (read == (UINT)len &&
			    !strncmp(orig_meta, new_meta, len))
			{
				// File is identical.
				// Don't rewrite it.
				f_close(&meta);
				return;
			}
		}
		f_close(&meta);
	}

	// File does not exist, or file is not identical.
	// Write the new meta.xml.
	if (f_open_char(&meta, filepath, FA_WRITE|FA_CREATE_ALWAYS) == FR_OK)
	{
		UINT wrote;
		f_write(&meta, new_meta, len, &wrote);
		f_close(&meta);
	}
}

int main(int argc, char **argv)
{
	// Exit after 10 seconds if there is an error
	__exception_setreload(10);
//	u64 timeout = 0;
	CheckForGecko();
	DCInvalidateRange(loader_stub, 0x1800);
	memcpy(loader_stub, (void*)0x80001800, 0x1800);

	RAMInit();

	//Meh, doesnt do anything anymore anyways
	//STM_RegisterEventHandler(HandleSTMEvent);

	Initialise();

	// Initializing IOS58...
	ShowMessageScreen("Initializing IOS58...");

	u32 u;
	//Disables MEMPROT for patches
	write16(MEM_PROT, 0);
	//Patches FS access
	for( u = 0x93A00000; u < 0x94000000; u+=2 )
	{
		if( memcmp( (void*)(u), FSAccessPattern, sizeof(FSAccessPattern) ) == 0 )
		{
		//	gprintf("FSAccessPatch:%08X\r\n", u );
			memcpy( (void*)u, FSAccessPatch, sizeof(FSAccessPatch) );
			DCFlushRange((void*)u, sizeof(FSAccessPatch));
			break;
		}
	}

	//for BT.c
	CONF_GetPadDevices((conf_pads*)0x932C0000);
	DCFlushRange((void*)0x932C0000, sizeof(conf_pads));
	*(vu32*)0x932C0490 = CONF_GetIRSensitivity();
	*(vu32*)0x932C0494 = CONF_GetSensorBarPosition();
	DCFlushRange((void*)0x932C0490, 8);

	// Load and patch IOS58.
	if (LoadKernel() < 0)
	{
		// NOTE: Attempting to initialize controllers here causes a crash.
		// Hence, we can't wait for the user to press the HOME button, so
		// we'll just wait for a timeout instead.
		PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*20, "Returning to loader in 10 seconds.");
		UpdateScreen();
		VIDEO_WaitVSync();

		// Wait 10 seconds...
		usleep(10000000);

		// Return to the loader.
		ExitToLoader(1);
	}

	InsertModule((char*)kernel_bin, kernel_bin_size);

	memset( (void*)0x92f00000, 0, 0x100000 );
	DCFlushRange( (void*)0x92f00000, 0x100000 );

	DCInvalidateRange( (void*)0x939F02F0, 0x20 );

	memcpy( (void*)0x939F02F0, Boot2Patch, sizeof(Boot2Patch) );

	DCFlushRange( (void*)0x939F02F0, 0x20 );

	//libogc still has that, lets close it
	__ES_Close();
	s32 fd = IOS_Open( "/dev/es", 0 );

	memset( STATUS, 0xFFFFFFFF, 0x20  );
	DCFlushRange( STATUS, 0x20 );

	memset( (void*)0x91000000, 0xFFFFFFFF, 0x20  );
	DCFlushRange( (void*)0x91000000, 0x20 );

	*(vu32*)0xD3003420 = 0; //make sure kernel doesnt reload

	raw_irq_handler_t irq_handler = BeforeIOSReload();
	IOS_IoctlvAsync( fd, 0x1F, 0, 0, &IOCTL_Buf, NULL, NULL );
	AfterIOSReload( irq_handler, FoundVersion );

	while(1)
	{
		DCInvalidateRange( STATUS, 0x20 );
		if((STATUS_LOADING > 0 || abs(STATUS_LOADING) > 1) && STATUS_LOADING < 20)
		{
			gprintf("Kernel sent signal\n");
			break;
		}
	}

	// Checking for storage devices...
	ShowMessageScreen("Checking storage devices...");

	// Initialize devices.
	// TODO: Only mount the device Nintendont was launched from
	// Mount the other device asynchronously.
	bool foundOneDevice = false;
	int i;
	for (i = DEV_SD; i <= DEV_USB; i++)
	{
		const WCHAR *devNameFF = MountDevice(i);
		if (devNameFF && !foundOneDevice)
		{
			// Set this device as primary.
			f_chdrive(devNameFF);
			foundOneDevice = true;
		}
	}

	// FIXME: Show this information in the menu instead of
	// aborting here.
	if (!devices[DEV_SD] && !devices[DEV_USB])
	{
		ClearScreen();
		gprintf("No FAT device found!\n");
		PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, 232, "No FAT device found!");
		ExitToLoader(1);
	}

	gprintf("Nintendont at your service!\r\n%s\r\n", NIN_BUILD_STRING);
	KernelLoaded = 1;

	char* first_slash = strrchr(argv[0], '/');
	if (first_slash != NULL) strncpy(launch_dir, argv[0], first_slash-argv[0]+1);
	gprintf("launch_dir = %s\r\n", launch_dir);

	// Initialize controllers.
	// FIXME: Initialize before storage devices.
	// Doing that right now causes usbstorage to fail...
	FPAD_Init();
	FPAD_Update();

	/* Read IPL Font before doing any patches */
	void *fontbuffer = memalign(32, 0x50000);
	__SYS_ReadROM((void*)fontbuffer,0x50000,0x1AFF00);
	memcpy((void*)0xD3100000, fontbuffer, 0x50000);
	DCInvalidateRange( (void*)0x93100000, 0x50000 );
	free(fontbuffer);
	//gprintf("Font: 0x1AFF00 starts with %.4s, 0x1FCF00 with %.4s\n", (char*)0x93100000, (char*)0x93100000 + 0x4D000);

	// Update meta.xml.
	updateMetaXml();

	// Load titles.txt.
	LoadTitles();

	memset((void*)ncfg, 0, sizeof(NIN_CFG));
	bool argsboot = false;
	if(argc > 1) //every 0x00 gets counted as one arg so just make sure its more than the path and copy
	{
		memcpy(ncfg, argv[1], sizeof(NIN_CFG));
		UpdateNinCFG(); //support for old versions with this
		if(ncfg->Magicbytes == 0x01070CF6 && ncfg->Version == NIN_CFG_VERSION && ncfg->MaxPads <= NIN_CFG_MAXPAD)
		{
			if(ncfg->Config & NIN_CFG_AUTO_BOOT)
			{	//do NOT remove, this can be used to see if nintendont knows args
				gprintf(ARGSBOOT_STR);
				argsboot = true;
			}
		}
	}
	if(argsboot == false)
	{
		if (LoadNinCFG() == false)
		{
			memset(ncfg, 0, sizeof(NIN_CFG));

			ncfg->Magicbytes = 0x01070CF6;
			ncfg->Version = NIN_CFG_VERSION;
			ncfg->Language = NIN_LAN_AUTO;
			ncfg->MaxPads = NIN_CFG_MAXPAD;
			ncfg->MemCardBlocks = 0x2;//251 blocks
		}

		// Prevent autobooting if B is pressed
		int i = 0;
		while((ncfg->Config & NIN_CFG_AUTO_BOOT) && i < 1000000) // wait for wiimote re-synch
		{
			if (i == 0) {
				PrintInfo();
				PrintFormat(DEFAULT_SIZE, BLACK, 320 - 90, MENU_POS_Y + 20*10, "B: Cancel Autoboot");
				GRRLIB_Render();
				ClearScreen();
			}
			
			FPAD_Update();

			if (FPAD_Cancel(0)) {
				ncfg->Config &= ~NIN_CFG_AUTO_BOOT;
				break;
			}

			i++;
		}
	}
	ReconfigVideo(rmode);
	UseSD = (ncfg->Config & NIN_CFG_USB) == 0;

	bool progressive = (CONF_GetProgressiveScan() > 0) && VIDEO_HaveComponentCable();
	if(progressive) //important to prevent blackscreens
		ncfg->VideoMode |= NIN_VID_PROG;
	else
		ncfg->VideoMode &= ~NIN_VID_PROG;

	bool SaveSettings = false;
	if(!(ncfg->Config & NIN_CFG_AUTO_BOOT))
	{
		// Not autobooting.
		// Prompt the user to select a device and game.
		SaveSettings = SelectDevAndGame();
	}
	else
	{
		// Autobooting.
		gprintf("Autobooting:\"%s\"\r\n", ncfg->GamePath );
		PrintInfo();
		GRRLIB_Render();
		ClearScreen();
	}

//Init DI and set correct ID if needed
	u32 CurDICMD = 0;
	if( memcmp(ncfg->GamePath, "di", 3) == 0 )
	{
		ShowLoadingScreen();

		DI_UseCache(false);
		DI_Init();
		DI_Mount();
		while (DI_GetStatus() & DVD_INIT)
			usleep(20000);
		if(!(DI_GetStatus() & DVD_READY))
		{
			ShowMessageScreenAndExit("The Disc Drive could not be initialized!", 1);
		}
		DI_Close();

		u8 *DIBuf = memalign(32,0x800);
		memset(DIBuf, 0, 0x20);
		DCFlushRange(DIBuf, 0x20);
		CurDICMD = DIP_CMD_NORMAL;
		ReadRealDisc(DIBuf, 0, 0x20, CurDICMD);
		if( IsGCGame(DIBuf) == false )
		{
			memset(DIBuf, 0, 0x800);
			DCFlushRange(DIBuf, 0x800);
			CurDICMD = DIP_CMD_DVDR;
			ReadRealDisc(DIBuf, 0, 0x800, CurDICMD);
			if( IsGCGame(DIBuf) == false )
			{
				free(DIBuf);
				ShowMessageScreenAndExit("The Disc in the Drive is not a GC Disc!", 1);
			}
		}
		memcpy(&(ncfg->GameID), DIBuf, 4);
		free(DIBuf);
	}

	if(SaveSettings)
	{
		// TODO: If the boot device is the same as the game device,
		// don't write it twice.

		// Write config to the boot device, which is loaded on next launch.
		FIL cfg;
		if (f_open_char(&cfg, "/nincfg.bin", FA_WRITE|FA_OPEN_ALWAYS) == FR_OK)
		{
			UINT wrote;
			f_write(&cfg, ncfg, sizeof(NIN_CFG), &wrote);
			f_close(&cfg);
		}

		// Write config to the game device, used by the Nintendont kernel.
		char ConfigPath[20];
		snprintf(ConfigPath, sizeof(ConfigPath), "%s:/nincfg.bin", GetRootDevice());
		if (f_open_char(&cfg, ConfigPath, FA_WRITE|FA_OPEN_ALWAYS) == FR_OK)
		{
			UINT wrote;
			f_write(&cfg, ncfg, sizeof(NIN_CFG), &wrote);
			f_close(&cfg);
		}
	}

	// Check for multi-game disc images.
	u32 ISOShift = 0;
	if(memcmp(&(ncfg->GameID), "COBR", 4) == 0 || memcmp(&(ncfg->GameID), "GGCO", 4) == 0
		|| memcmp(&(ncfg->GameID), "GCO", 3) == 0 || memcmp(&(ncfg->GameID), "RGCO", 4) == 0)
	{
		u32 i, j = 0;
		u32 Offsets[15];
		gameinfo gi[15];
		u8 *MultiHdr = memalign(32, 0x800);

		FIL f;
		UINT read;
		FRESULT fres = FR_DISK_ERR;

		if(CurDICMD)
		{
			ReadRealDisc(MultiHdr, 0, 0x800, CurDICMD);
		}
		else if(strstr(ncfg->GamePath, ".iso") != NULL)
		{
			char GamePath[260];
			snprintf(GamePath, sizeof(GamePath), "%s:%s", GetRootDevice(), ncfg->GamePath);
			fres = f_open_char(&f, GamePath, FA_READ|FA_OPEN_EXISTING);
			if (fres == FR_OK)
				f_read(&f, MultiHdr, 0x800, &read);
		}

		//Damn you COD for sharing this ID!
		if (fres != FR_OK || (memcmp(MultiHdr, "GCO", 3) == 0 && memcmp(MultiHdr+4, "52", 3) == 0))
		{
			free(MultiHdr);
			if (fres == FR_OK)
				f_close(&f);
		}
		else
		{
			u8 *GameHdr = memalign(32, 0x800);
			u32 NeedShift = (*(vu32*)(MultiHdr+4) == 0x44564439);
			for(i = 0x40; i < 0x100; i += 4)
			{
				u32 TmpOffset = *(vu32*)(MultiHdr+i);
				if(TmpOffset > 0)
				{
					Offsets[j] = NeedShift ? TmpOffset << 2 : TmpOffset;
					if(CurDICMD)
					{
						ReadRealDisc(GameHdr, Offsets[j], 0x800, CurDICMD);
					}
					else
					{
						f_lseek(&f, Offsets[j]);
						f_read(&f, GameHdr, 0x800, &read);
					}

					// TODO: titles.txt support?
					// FIXME: This memory is never freed!
					memcpy(gi[j].ID, GameHdr, 6);
					gi[j].Name = strdup((char*)GameHdr+0x20);
					j++;
					if(j == 15) break;
				}
			}
			free(GameHdr);
			free(MultiHdr);
			f_close(&f);

			bool redraw = 1;
			ClearScreen();
			u32 PosX = 0;
			u32 UpHeld = 0, DownHeld = 0;
			while(1)
			{
				VIDEO_WaitVSync();
				FPAD_Update();
				if( FPAD_OK(0) )
					break;
				else if( FPAD_Down(1) )
				{
					if(DownHeld == 0 || DownHeld > 10)
					{
						PosX++;
						if(PosX == j) PosX = 0;
						redraw = true;
					}
					DownHeld++;
				}
				else
					DownHeld = 0;
				if( FPAD_Up(1) )
				{
					if(UpHeld == 0 || UpHeld > 10)
					{
						if(PosX == 0) PosX = j;
						PosX--;
						redraw = true;
					}
					UpHeld++;
				}
				else
					UpHeld = 0;
				if( redraw )
				{
					PrintInfo();
					for( i=0; i < j; ++i )
						PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*4 + i * 20, "%50.50s [%.6s]%s", 
							gi[i].Name, gi[i].ID, i == PosX ? ARROW_LEFT : " " );
					GRRLIB_Render();
					Screenshot();
					ClearScreen();
					redraw = false;
				}
			}
			ISOShift = Offsets[PosX];
			memcpy(&(ncfg->GameID), gi[PosX].ID, 4);
		}
	}
//multi-iso game hack
	*(vu32*)0xD300300C = ISOShift;

//Set Language
	if(ncfg->Language == NIN_LAN_AUTO || ncfg->Language >= NIN_LAN_LAST)
	{
		switch (CONF_GetLanguage())
		{
			case CONF_LANG_GERMAN:
				ncfg->Language = NIN_LAN_GERMAN;
				break;
			case CONF_LANG_FRENCH:
				ncfg->Language = NIN_LAN_FRENCH;
				break;
			case CONF_LANG_SPANISH:
				ncfg->Language = NIN_LAN_SPANISH;
				break;
			case CONF_LANG_ITALIAN:
				ncfg->Language = NIN_LAN_ITALIAN;
				break;
			case CONF_LANG_DUTCH:
				ncfg->Language = NIN_LAN_DUTCH;
				break;
			default:
				ncfg->Language = NIN_LAN_ENGLISH;
				break;
		}
	}

	if(ncfg->Config & NIN_CFG_MEMCARDEMU)
	{
		// Memory card emulation is enabled.
		// Set up the memory card file.
		char BasePath[20];
		snprintf(BasePath, sizeof(BasePath), "%s:/saves", GetRootDevice());
		f_mkdir_char(BasePath);

		char MemCardName[8];
		memset(MemCardName, 0, 8);
		if ( ncfg->Config & NIN_CFG_MC_MULTI )
		{
			// "Multi" mode enabled.
			// Use one memory card for USA/PAL games,
			// and another memory card for JPN games.
			if ((ncfg->GameID & 0xFF) == 'J')  // JPN games
				memcpy(MemCardName, "ninmemj", 7);
			else
				memcpy(MemCardName, "ninmem", 6);
		}
		else
		{
			// One card per game.
			memcpy(MemCardName, &(ncfg->GameID), 4);
		}

		char MemCard[32];
		snprintf(MemCard, sizeof(MemCard), "%s/%s.raw", BasePath, MemCardName);
		gprintf("Using %s as Memory Card.\r\n", MemCard);
		FIL f;
		if (f_open_char(&f, MemCard, FA_READ|FA_OPEN_EXISTING) != FR_OK)
		{
			// Memory card file not found. Create it.
			if(GenerateMemCard(MemCard) == false)
			{
				ClearScreen();
				ShowMessageScreenAndExit("Failed to create Memory Card File!", 1);
			}
		}
		else
		{
			// Memory card file found.
			f_close(&f);
		}
	}
	else
	{
		// Using real memory card slots. (Wii only)
		// Setup real SRAM language.
		syssram *sram;
		sram = __SYS_LockSram();
		sram->lang = ncfg->Language;
		__SYS_UnlockSram(1); // 1 -> write changes
		while(!__SYS_SyncSram());
	}

	#define GCN_IPL_SIZE 2097152
	#define TRI_IPL_SIZE 1048576
	void *iplbuf = NULL;
	bool useipl = false;
	bool useipltri = false;

	//Check if game is Triforce game
	u32 IsTRIGame = 0;
	if (ncfg->GameID != 0x47545050) //Damn you Knights Of The Temple!
		IsTRIGame = TRISetupGames(ncfg->GamePath, CurDICMD, ISOShift);

	if(IsTRIGame == 0)
	{
		// Attempt to load the GameCube IPL.
		char iplchar[32];
		iplchar[0] = 0;
		switch (ncfg->GameID & 0xFF)
		{
			case 'E':	// USA region
				snprintf(iplchar, sizeof(iplchar), "%s:/iplusa.bin", GetRootDevice());
				break;
			case 'J':	// JPN region
				snprintf(iplchar, sizeof(iplchar), "%s:/ipljap.bin", GetRootDevice());
				break;
			case 'P':	// PAL region
				// FIXME: PAL IPL is broken on Wii U.
				if (!IsWiiU())
					snprintf(iplchar, sizeof(iplchar), "%s:/iplpal.bin", GetRootDevice());
				break;
			default:
				break;
		}

		FIL f;
		if (iplchar[0] != 0 &&
		    f_open_char(&f, iplchar, FA_READ|FA_OPEN_EXISTING) == FR_OK)
		{
			if (f.obj.objsize == GCN_IPL_SIZE)
			{
				iplbuf = malloc(GCN_IPL_SIZE);
				UINT read;
				f_read(&f, iplbuf, GCN_IPL_SIZE, &read);
				useipl = (read == GCN_IPL_SIZE);
			}
			f_close(&f);
		}
	}
	else
	{
		// Attempt to load the Triforce IPL. (segaboot)
		char iplchar[32];
		snprintf(iplchar, sizeof(iplchar), "%s:/segaboot.bin", GetRootDevice());
		FIL f;
		if (f_open_char(&f, iplchar, FA_READ|FA_OPEN_EXISTING) == FR_OK)
		{
			if (f.obj.objsize == TRI_IPL_SIZE)
			{
				f_lseek(&f, 0x20);
				void *iplbuf = (void*)0x92A80000;
				UINT read;
				f_read(&f, iplbuf, TRI_IPL_SIZE - 0x20, &read);
				useipltri = (read == (TRI_IPL_SIZE - 0x20));
			}
			f_close(&f);
		}
	}

	//sync changes
	CloseDevices();

	WPAD_Disconnect(0);
	WPAD_Disconnect(1);
	WPAD_Disconnect(2);
	WPAD_Disconnect(3);

	WUPC_Shutdown();
	WPAD_Shutdown();

	//before flushing do game specific patches
	if(ncfg->Config & NIN_CFG_FORCE_PROG &&
			ncfg->GameID == 0x47584745)
	{	//Mega Man X Collection does progressive ingame so
		//forcing it would mess with the interal game setup
		gprintf("Disabling Force Progressive for this game\r\n");
		ncfg->Config &= ~NIN_CFG_FORCE_PROG;
	}

	//make sure the cfg gets to the kernel
	DCStoreRange((void*)ncfg, sizeof(NIN_CFG));

//set current time
	u32 bias = 0, cur_time = 0;
	__SYS_GetRTC(&cur_time);
	if(CONF_GetCounterBias(&bias) >= 0)
		cur_time += bias;
	settime(secs_to_ticks(cur_time));
//hand over approx time passed since 1970
	*(vu32*)0xD3003424 = (cur_time+946684800);

//set status for kernel to start running
	*(vu32*)0xD3003420 = 0x0DEA;
	while(1)
	{
		DCInvalidateRange( STATUS, 0x20 );
		if( STATUS_LOADING == 0xdeadbeef )
			break;

		PrintInfo();

		PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*6, "Loading patched kernel... %d", STATUS_LOADING);
		if(STATUS_LOADING == 0)
		{
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*7, "ES_Init...");
			// Cleans the -1 when it's past it to avoid confusion if another error happens. e.g. before it showed "81" instead of "8" if the controller was unplugged.
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 163, MENU_POS_Y + 20*6, " ");
		}
		if((STATUS_LOADING > 0 || abs(STATUS_LOADING) > 1) && STATUS_LOADING < 20)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*7, "ES_Init... Done!");
		if(STATUS_LOADING == 2)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*8, "Initing storage devices...");
		if(abs(STATUS_LOADING) > 2 && abs(STATUS_LOADING) < 20)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*8, "Initing storage devices... Done!");
		if(STATUS_LOADING == -2)
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*8, "Initing storage devices... Error! %d  Shutting down", STATUS_ERROR);
		if(STATUS_LOADING == 3)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*9, "Mounting USB/SD device...");
		if(abs(STATUS_LOADING) > 3 && abs(STATUS_LOADING) < 20)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*9, "Mounting USB/SD device... Done!");
		if(STATUS_LOADING == -3)
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*9, "Mounting USB/SD device... Error! %d  Shutting down", STATUS_ERROR);
		if(STATUS_LOADING == 5) {
/* 			if (timeout == 0)
				timeout = ticks_to_secs(gettime()) + 20; // Set timer for 20 seconds
			else if (timeout <= ticks_to_secs(gettime())) {
				STATUS_ERROR = -7;
				DCFlushRange(STATUS, 0x20);
				usleep(100);
				//memset( (void*)0x92f00000, 0, 0x100000 );
				//DCFlushRange( (void*)0x92f00000, 0x100000 );
				//ExitToLoader(1);
			}*/
			PrintFormat(DEFAULT_SIZE, (STATUS_ERROR == -7) ? MAROON:BLACK, MENU_POS_X, MENU_POS_Y + 20*10, (STATUS_ERROR == -7) ? "Checking FS... Timeout!" : "Checking FS...");
		}
		if(abs(STATUS_LOADING) > 5 && abs(STATUS_LOADING) < 20)
		{
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*10, "Checking FS... Done!");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*11, "Drive size: %.02f%s Sector size: %d", STATUS_DRIVE, STATUS_GB_MB ? "GB" : "MB", STATUS_SECTOR);
		}
		if(STATUS_LOADING == -5)
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*10, "Checking FS... Error! %d Shutting down", STATUS_ERROR);
		if(STATUS_LOADING == 6)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*12, "ES_LoadModules...");
		if(abs(STATUS_LOADING) > 6 && abs(STATUS_LOADING) < 20)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*12, "ES_LoadModules... Done!");
		if(STATUS_LOADING == -6)
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*12, "ES_LoadModules... Error! %d Shutting down", STATUS_ERROR);
		if(STATUS_LOADING == 7)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*13, "Loading config...");
		if(abs(STATUS_LOADING) > 7 && abs(STATUS_LOADING) < 20)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*13, "Loading config... Done!");
		/*if(STATUS_LOADING == 8)
		{
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... ");
			if ( STATUS_ERROR == 1)
			{
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*15, "          Make sure the Controller is plugged in");
			}
			else
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*15, "%50s", " ");
		}
		if(abs(STATUS_LOADING) > 8 && abs(STATUS_LOADING) < 20)
		{
			if (ncfg->Config & NIN_CFG_NATIVE_SI)
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... Using ONLY NATIVE Gamecube Ports");
			else if ((ncfg->MaxPads == 1) && (ncfg->Config & NIN_CFG_HID))
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... Using Gamecube and HID Ports");
			else if ((ncfg->MaxPads > 0) && (ncfg->Config & NIN_CFG_HID))
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... Using Gamecube, HID, and BT Ports");
			else if (ncfg->MaxPads > 0)
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... Using Gamecube and BT Ports");
			else if (ncfg->Config & NIN_CFG_HID)
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... Using HID and Bluetooth Ports");
			else
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... Using Bluetooth Ports... Done!");
		}
		if(STATUS_LOADING == -8)
		{
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*14, "Init HID devices... Failed! Shutting down");
			switch (STATUS_ERROR)
			{
				case -1:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "No Controller plugged in! %25s", " ");
					break;
				case -2:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "Missing %s:/controller.ini %20s", GetRootDevice(), " ");
					break;
				case -3:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "Controller does not match %s:/controller.ini %6s", GetRootDevice(), " ");
					break;
				case -4:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "Invalid Polltype in %s:/controller.ini %12s", GetRootDevice(), " ");
					break;
				case -5:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "Invalid DPAD value in %s:/controller.ini %9s", GetRootDevice(), " ");
					break;
				case -6:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "PS3 controller init error %25s", " ");
					break;
				case -7:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "Gamecube adapter for Wii u init error %13s", " ");
					break;
				default:
					PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*15, "Unknown error %d %35s", STATUS_ERROR, " ");
					break;
			}
		}*/
		if(STATUS_LOADING == 9)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init DI... %40s", " ");
		if(abs(STATUS_LOADING) > 9 && abs(STATUS_LOADING) < 20)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*14, "Init DI... Done! %35s", " ");
		if(STATUS_LOADING == 10)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*15, "Init CARD...");
		if(abs(STATUS_LOADING) > 10 && abs(STATUS_LOADING) < 20)
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*15, "Init CARD... Done!");
		GRRLIB_Screen2Texture(0, 0, screen_buffer, GX_FALSE); // Copy all status messages
		GRRLIB_Render();
		ClearScreen();
		while((STATUS_LOADING < -1) && (STATUS_LOADING > -20)) //displaying a fatal error
				; //do nothing wait for shutdown
	}
	DrawBuffer(); // Draw all status messages
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*17, "Nintendont kernel looping, loading game...");
	GRRLIB_Render();
	DrawBuffer(); // Draw all status messages
//	memcpy( (void*)0x80000000, (void*)0x90140000, 0x1200000 );
	GRRLIB_FreeTexture(background);
	GRRLIB_FreeTexture(screen_buffer);
	GRRLIB_FreeTTF(myFont);
	GRRLIB_Exit();

	gprintf("GameRegion:");

	u32 vidForce = (ncfg->VideoMode & NIN_VID_FORCE);
	u32 vidForceMode = (ncfg->VideoMode & NIN_VID_FORCE_MASK);

	progressive = (ncfg->Config & NIN_CFG_FORCE_PROG)
		&& !useipl && !useipltri;

	switch (ncfg->GameID & 0x000000FF)
	{
		// EUR
		case 'D':
		case 'F':
		case 'H':
		case 'I':
		case 'M':
		case 'P':
		case 'S':
		case 'U':
		case 'X':
		case 'Y':
		case 'Z':
			*(vu32*)0x800000CC = 1;
			vmode = &TVPal528IntDf;
			break;
		//US
		case 'E':
			if((vidForce && vidForceMode == NIN_VID_FORCE_MPAL)
				|| (!vidForce && CONF_GetVideo() == CONF_VIDEO_MPAL))
			{
				*(vu32*)0x800000CC = 3;
				vmode = &TVMpal480IntDf;
			}
			else
			{
				*(vu32*)0x800000CC = 0;
				vmode = &TVNtsc480IntDf;
			}
			break;
		//JP
		case 'J':
		default:
			*(vu32*)0x800000CC = 0;
			vmode = &TVNtsc480IntDf;
			break;
	}

	if(progressive)
		vmode = &TVNtsc480Prog;
	else if(vidForce)
	{
		switch(vidForceMode)
		{
			case NIN_VID_FORCE_PAL50:
				vmode = &TVPal528IntDf;
				break;
			case NIN_VID_FORCE_PAL60:
				vmode = &TVEurgb60Hz480IntDf;
				break;
			case NIN_VID_FORCE_MPAL:
				vmode = &TVMpal480IntDf;
				break;
			case NIN_VID_FORCE_NTSC:
			default:
				vmode = &TVNtsc480IntDf;
				break;
		}
	}
	if((ncfg->Config & NIN_CFG_MEMCARDEMU) == 0) //setup real sram video
	{
		syssram *sram;
		sram = __SYS_LockSram();
		sram->display_offsetH = 0;	// Clear Offset
		sram->ntd		&= ~0x40;	// Clear PAL60
		sram->flags		&= ~0x80;	// Clear Progmode
		sram->flags		&= ~3;		// Clear Videomode
		switch(ncfg->GameID & 0xFF)
		{
			case 'E':
			case 'J':
				//BMX XXX doesnt even boot on a real gc with component cables
				if( (ncfg->GameID >> 8) != 0x474233 &&
					(ncfg->VideoMode & NIN_VID_PROG) )
					sram->flags |= 0x80;	// Set Progmode
				break;
			default:
				sram->ntd		|= 0x40;	// Set PAL60
				break;
		}
		if(*(vu32*)0x800000CC == 1 || *(vu32*)0x800000CC == 5)
			sram->flags	|= 1; //PAL Video Mode
		__SYS_UnlockSram(1); // 1 -> write changes
		while(!__SYS_SyncSram());
	}
	
	ReconfigVideo(vmode);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(vmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	else while(VIDEO_GetNextField())
		VIDEO_WaitVSync();

	*(u16*)(0xCC00501A) = 156;	// DSP refresh rate
	/* from libogc, get all gc pads to work */
	u32 buf[2];
	u32 bits = 0;
	u8 chan;
	for(chan = 0; chan < 4; ++chan)
	{
		bits |= (0x80000000>>chan);
		SI_GetResponse(chan,(void*)buf);
		SI_SetCommand(chan,(0x00000300|0x00400000));
		SI_EnablePolling(bits);
	}
	*(vu32*)0xD3003004 = 1; //ready up HID Thread

	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(vmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	else while(VIDEO_GetNextField())
		VIDEO_WaitVSync();
	GX_AbortFrame();

	DCInvalidateRange((void*)0x93000000, 0x3000);
	memset((void*)0x93002700, 0, 0x200); //clears alot of pad stuff
	memset((void*)0x93002C00, 0, 0x400); //clears alot of multidol stuff
	strcpy((char*)0x930028A0, "ARStartDMA: %08x %08x %08x\n"); //ARStartDMA Debug
	DCFlushRange((void*)0x93000000, 0x3000);

	DCInvalidateRange((void*)0x93010010, 0x10000);
	memcpy((void*)0x93010010, loader_stub, 0x1800);
	memcpy((void*)0x93011810, stub_bin, stub_bin_size);
	DCFlushRange((void*)0x93010010, 0x10000);

	DCInvalidateRange((void*)0x93020000, 0x10000);
	memset((void*)0x93020000, 0, 0x10000);
	DCFlushRange((void*)0x93020000, 0x10000);

	DCInvalidateRange((void*)0x93003000, 0x20);
	//*(vu32*)0x93003000 = currev; //set kernel rev (now in LoadKernel)
	*(vu32*)0x93003008 = 0x80000004; //just some address for SIGetType
	//0x9300300C is already used for multi-iso
	memset((void*)0x93003010, 0, 0x10); //disable rumble on bootup
	DCFlushRange((void*)0x93003000, 0x20);

	//lets prevent weird events
	__STM_Close();

	//THIS thing right here, it interrupts some games and breaks them
	//To fix that, call ExecSuspendScheduler so WC24 just sleeps
	u32 out = 0;
	fd = IOS_Open("/dev/net/kd/request", 0);
	IOS_Ioctl(fd, IOCTL_ExecSuspendScheduler, NULL, 0, &out, 4);
	IOS_Close(fd);

	write16(0xD8B420A, 0); //disable MEMPROT again after reload
	//u32 level = IRQ_Disable();
	__exception_closeall();
	__lwp_thread_closeall();

	DVDStartCache(); //waits for kernel start
	DCInvalidateRange((void*)0x90000000, 0x1000000);
	memset((void*)(void*)0x90000000, 0, 0x1000000); //clear ARAM
	DCFlushRange((void*)0x90000000, 0x1000000);

	gprintf("Game Start\n");

	if(useipl)
	{
		load_ipl(iplbuf);
		*(vu32*)0xD3003420 = 0x5DEA;
		while(*(vu32*)0xD3003420 == 0x5DEA) ;
		/* Patches */
		DCInvalidateRange((void*)0x80001800, 0x1800);
		ICInvalidateRange((void*)0x80001800, 0x1800);
		/* IPL */
		DCInvalidateRange((void*)0x81300000, 0x300000);
		ICInvalidateRange((void*)0x81300000, 0x300000);
		/* Seems to boot more stable this way */
		//gprintf("Using 32kHz DSP (No Resample)\n");
		write32(0xCD806C00, 0x68);
		free(iplbuf);
	}
	else //use our own loader
	{
		if(useipltri)
		{
			*(vu32*)0xD3003420 = 0x6DEA;
			while(*(vu32*)0xD3003420 == 0x6DEA) ;
		}
		memcpy((void*)0x81300000, multidol_ldr_bin, multidol_ldr_bin_size);
		DCFlushRange((void*)0x81300000, multidol_ldr_bin_size);
		ICInvalidateRange((void*)0x81300000, multidol_ldr_bin_size);
	}
	asm volatile (
		"lis %r3, 0x8130\n"
		"mtlr %r3\n"
		"blr\n"
	);
	//IRQ_Restore(level);

	return 0;
}
