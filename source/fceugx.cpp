/****************************************************************************
 * FCE Ultra
 * Nintendo Wii/Gamecube Port
 *
 * Tantric 2008-2009
 *
 * fceugx.cpp
 *
 * This file controls overall program flow. Most things start and end here!
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <gctypes.h>
#include <ogc/system.h>
#include <fat.h>
#include <wiiuse/wpad.h>
#include <wupc/wupc.h>
#include <malloc.h>
#include <sys/iosupport.h>

#ifdef HW_RVL
#include <di/di.h>
#endif

#include "fceugx.h"
#include "fceuload.h"
#include "fceustate.h"
#include "fceuram.h"
#include "fceusupport.h"
#include "menu.h"
#include "preferences.h"
#include "fileop.h"
#include "filebrowser.h"
#include "networkop.h"
#include "gcaudio.h"
#include "gcvideo.h"
#include "pad.h"
#include "filelist.h"
#include "gui/gui.h"
#include "utils/FreeTypeGX.h"
#ifdef USE_VM
	#include "vmalloc.h"
#endif

#include "fceultra/types.h"

void FCEUD_Update(uint8 *XBuf, int32 *Buffer, int32 Count);
void FCEUD_UpdatePulfrich(uint8 *XBuf, int32 *Buffer, int32 Count);
void FCEUD_UpdateLeft(uint8 *XBuf, int32 *Buffer, int32 Count);
void FCEUD_UpdateRight(uint8 *XBuf, int32 *Buffer, int32 Count);

extern "C" {
#ifdef USE_VM
	#include "utils/vm/vm.h"
#endif
extern char* strcasestr(const char *, const char *);
extern void __exception_setreload(int t);
}

int fskipc = 0;
int fskip = 0;
static uint8 *gfx=0;
static int32 *sound=0;
static int32 ssize=0;
int ScreenshotRequested = 0;
int ConfigRequested = 0;
int ShutdownRequested = 0;
int ResetRequested = 0;
int ExitRequested = 0;
char appPath[1024] = { 0 };

int frameskip = 0;
int turbomode = 0;
unsigned char * nesrom = NULL;

/****************************************************************************
 * Shutdown / Reboot / Exit
 ***************************************************************************/

static void ExitCleanup()
{
	ShutdownAudio();
	StopGX();

	HaltDeviceThread();
	UnmountAllFAT();

#ifdef HW_RVL
	DI_Close();
#endif
}

#ifdef HW_DOL
	#define PSOSDLOADID 0x7c6000a6
	int *psoid = (int *) 0x80001800;
	void (*PSOReload) () = (void (*)()) 0x80001800;
#endif

void ExitToWiiflow()
{
	ShutoffRumble();
	SavePrefs(SILENT);
	if (romLoaded && !ConfigRequested && GCSettings.AutoSave == 1)
		SaveRAMAuto(SILENT);
	ExitCleanup();

	if( !!*(u32*)0x80001800 ) 
	{
		// Were we launched via HBC? (or via wiiflows stub replacement? :P)
		exit(1);
	}
	else
	{
		// Wii channel support
		SYS_ResetSystem( SYS_RETURNTOMENU, 0, 0 );
	}
}

void ExitApp()
{
#ifdef HW_RVL
	ShutoffRumble();
#endif

	SavePrefs(SILENT);

	if (romLoaded && !ConfigRequested && GCSettings.AutoSave == 1)
		SaveRAMAuto(SILENT);

	ExitCleanup();

	if(ShutdownRequested)
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);

	#ifdef HW_RVL
	if(GCSettings.ExitAction == 0) // Auto
	{
		char * sig = (char *)0x80001804;
		if(
			sig[0] == 'S' &&
			sig[1] == 'T' &&
			sig[2] == 'U' &&
			sig[3] == 'B' &&
			sig[4] == 'H' &&
			sig[5] == 'A' &&
			sig[6] == 'X' &&
			sig[7] == 'X')
			GCSettings.ExitAction = 3; // Exit to HBC
		else
			GCSettings.ExitAction = 1; // HBC not found
	}
	#endif

	if(GCSettings.ExitAction == 1) // Exit to Menu
	{
		#ifdef HW_RVL
			SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
		#else
			#define SOFTRESET_ADR ((volatile u32*)0xCC003024)
			*SOFTRESET_ADR = 0x00000000;
		#endif
	}
	else if(GCSettings.ExitAction == 2) // Shutdown Wii
	{
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	}
	else // Exit to Loader
	{
		#ifdef HW_RVL
			exit(0);
		#else
			if (psoid[0] == PSOSDLOADID)
				PSOReload();
		#endif
	}
}

#ifdef HW_RVL
void ShutdownCB()
{
	ShutdownRequested = 1;
}
void ResetCB()
{
	ResetRequested = 1;
}
#endif

#ifdef HW_DOL
/****************************************************************************
 * ipl_set_config
 * lowlevel Qoob Modchip disable
 ***************************************************************************/

static void ipl_set_config(unsigned char c)
{
	volatile unsigned long* exi = (volatile unsigned long*)0xCC006800;
	unsigned long val,addr;
	addr=0xc0000000;
	val = c << 24;
	exi[0] = ((((exi[0]) & 0x405) | 256) | 48);	//select IPL
	//write addr of IPL
	exi[0 * 5 + 4] = addr;
	exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
	while (exi[0 * 5 + 3] & 1);
	//write the ipl we want to send
	exi[0 * 5 + 4] = val;
	exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
	while (exi[0 * 5 + 3] & 1);

	exi[0] &= 0x405;	//deselect IPL
}
#endif

/****************************************************************************
 * IOS Check
 ***************************************************************************/
#ifdef HW_RVL
bool SupportedIOS(u32 ios)
{
	if(ios == 58 || ios == 61)
		return true;

	return false;
}

bool SaneIOS(u32 ios)
{
	bool res = false;
	u32 num_titles=0;
	u32 tmd_size;

	if(ios > 200)
		return false;

	if (ES_GetNumTitles(&num_titles) < 0)
		return false;

	if(num_titles < 1) 
		return false;

	u64 *titles = (u64 *)memalign(32, num_titles * sizeof(u64) + 32);
	
	if(!titles)
		return false;

	if (ES_GetTitles(titles, num_titles) < 0)
	{
		free(titles);
		return false;
	}
	
	u32 *tmdbuffer = (u32 *)memalign(32, MAX_SIGNED_TMD_SIZE);

	if(!tmdbuffer)
	{
		free(titles);
		return false;
	}

	for(u32 n=0; n < num_titles; n++)
	{
		if((titles[n] & 0xFFFFFFFF) != ios) 
			continue;

		if (ES_GetStoredTMDSize(titles[n], &tmd_size) < 0)
			break;

		if (tmd_size > 4096)
			break;

		if (ES_GetStoredTMD(titles[n], (signed_blob *)tmdbuffer, tmd_size) < 0)
			break;

		if (tmdbuffer[1] || tmdbuffer[2])
		{
			res = true;
			break;
		}
	}
	free(tmdbuffer);
    free(titles);
	return res;
}
#endif
/****************************************************************************
 * USB Gecko Debugging
 ***************************************************************************/

static bool gecko = false;
static mutex_t gecko_mutex = 0;

static ssize_t __out_write(struct _reent *r, void* fd, const char *ptr, size_t len)
{
	if (!gecko || len == 0)
		return len;
	
	if(!ptr || len < 0)
		return -1;

	u32 level;
	LWP_MutexLock(gecko_mutex);
	level = IRQ_Disable();
	usb_sendbuffer(1, ptr, len);
	IRQ_Restore(level);
	LWP_MutexUnlock(gecko_mutex);
	return len;
}

const devoptab_t gecko_out = {
	"stdout",	// device name
	0,			// size of file structure
	NULL,		// device open
	NULL,		// device close
	__out_write,// device write
	NULL,		// device read
	NULL,		// device seek
	NULL,		// device fstat
	NULL,		// device stat
	NULL,		// device link
	NULL,		// device unlink
	NULL,		// device chdir
	NULL,		// device rename
	NULL,		// device mkdir
	0,			// dirStateSize
	NULL,		// device diropen_r
	NULL,		// device dirreset_r
	NULL,		// device dirnext_r
	NULL,		// device dirclose_r
	NULL		// device statvfs_r
};

static void USBGeckoOutput()
{
	gecko = usb_isgeckoalive(1);
	LWP_MutexInit(&gecko_mutex, false);

	devoptab_list[STD_OUT] = &gecko_out;
	devoptab_list[STD_ERR] = &gecko_out;
}

extern "C" { 
	s32 __STM_Close();
	s32 __STM_Init();
}

/****************************************************************************
 * main
 * This is where it all happens!
 ***************************************************************************/

int main(int argc, char *argv[])
{
	#ifdef USE_VM
	VM_Init(ARAM_SIZE, MRAM_BACKING);		// Setup Virtual Memory with the entire ARAM
	#endif

	#ifdef HW_RVL
	L2Enhance();
	
	u32 ios = IOS_GetVersion();

	if(!SupportedIOS(ios))
	{
		s32 preferred = IOS_GetPreferredVersion();

		if(SupportedIOS(preferred))
			IOS_ReloadIOS(preferred);
	}
	#else
	ipl_set_config(6); // disable Qoob modchip
	#endif

	USBGeckoOutput(); // uncomment to enable USB gecko output
	__exception_setreload(8);

	InitGCVideo (); // Initialize video
	ResetVideo_Menu (); // change to menu video mode
	
	#ifdef HW_RVL
	// Wii Power/Reset buttons
	__STM_Close();
	__STM_Init();
	__STM_Close();
	__STM_Init();
	SYS_SetPowerCallback(ShutdownCB);
	SYS_SetResetCallback(ResetCB);
	
	WUPC_Init();
	WPAD_Init();
	WPAD_SetPowerButtonCallback((WPADShutdownCallback)ShutdownCB);
	DI_Init();
	USBStorage_Initialize();
	StartNetworkThread();
	#else
	DVD_Init (); // Initialize DVD subsystem (GameCube only)
	#endif
	
	SetupPads();
	InitDeviceThread();
	MountAllFAT(); // Initialize libFAT for SD and USB
	
	#ifdef HW_RVL
	// store path app was loaded from
	if(argc > 0 && argv[0] != NULL)
		CreateAppPath(argv[0]);
	#endif

	DefaultSettings(); // Set defaults
	InitialiseAudio();
	InitFreeType((u8*)font_ttf, font_ttf_size); // Initialize font system
#ifdef USE_VM
	gameScreenPng = (u8 *)vm_malloc(512*1024);
#else
	gameScreenPng = (u8 *)malloc(512*1024);
#endif
	browserList = (BROWSERENTRY *)malloc(sizeof(BROWSERENTRY)*MAX_BROWSER_SIZE);
	InitGUIThreads();

	// allocate memory to store rom
#ifdef USE_VM
	nesrom = (unsigned char *)vm_malloc(1024*1024*4); // 4 MB should be plenty
#else
	nesrom = (unsigned char *)memalign(32,1024*1024*4); // 4 MB should be plenty
#endif
	/*** Minimal Emulation Loop ***/
	if (!FCEUI_Initialize())
		ExitApp();

	FCEUI_SetGameGenie(1); // 0 - OFF, 1 - ON
	
	FDSBIOS=(uint8 *)malloc(8192);
	memset(FDSBIOS, 0, sizeof(FDSBIOS)); // clear FDS BIOS memory

	FCEUI_SetSoundQuality(1); // 0 - low, 1 - high, 2 - high (alt.)
	int currentTiming = 0;

	bool autoboot = false;
	if(argc > 2 && argv[1] != NULL && argv[2] != NULL)
	{
		autoboot = true;
		ResetBrowser();
		LoadPrefs();
		if(strcasestr(argv[1], "sd:/") != NULL)
		{
			GCSettings.SaveMethod = DEVICE_SD;
			GCSettings.LoadMethod = DEVICE_SD;
		}
		else
		{
			GCSettings.SaveMethod = DEVICE_USB;
			GCSettings.LoadMethod = DEVICE_USB;
		}
		SavePrefs(SILENT);
		selectLoadedFile = 1;
		std::string dir(argv[1]);
		dir.assign(&dir[dir.find_last_of(":") + 2]);
		char arg_filename[1024];
		strncpy(arg_filename, argv[2], sizeof(arg_filename));
		strncpy(GCSettings.LoadFolder, dir.c_str(), sizeof(GCSettings.LoadFolder));
		OpenGameList();
		strncpy(GCSettings.Exit_Dol_File, argc > 3 && argv[3] != NULL ? argv[3] : "", sizeof(GCSettings.Exit_Dol_File));
		if(argc > 5 && argv[4] != NULL && argv[5] != NULL)
		{
			sscanf(argv[4], "%08x", &GCSettings.Exit_Channel[0]);
			sscanf(argv[5], "%08x", &GCSettings.Exit_Channel[1]);
		}
		else
		{
			GCSettings.Exit_Channel[0] = 0x00010008;
			GCSettings.Exit_Channel[1] = 0x57494948;
		}
		if(argc > 6 && argv[6] != NULL)
			strncpy(GCSettings.LoaderName, argv[6], sizeof(GCSettings.LoaderName));
		else
			snprintf(GCSettings.LoaderName, sizeof(GCSettings.LoaderName), "WiiFlow");
		for(int i = 0; i < browser.numEntries; i++)
		{
			// Skip it
			if (strcmp(browserList[i].filename, ".") == 0 || strcmp(browserList[i].filename, "..") == 0)
				continue;
			if(strcasestr(browserList[i].filename, arg_filename) != NULL)
			{
				browser.selIndex = i;
				if(IsSz())
				{
					BrowserLoadSz();
					browser.selIndex = 1;
				}
				break;
			}
		}
		BrowserLoadFile();
	}

	while (1) // main loop
	{
		// go back to checking if devices were inserted/removed
		// since we're entering the menu
		ResumeDeviceThread();

		SwitchAudioMode(1);

		if(!autoboot)
		{
			if(!romLoaded)
				MainMenu(MENU_GAMESELECTION);
			else
				MainMenu(MENU_GAME);

			ConfigRequested = 0;
			ScreenshotRequested = 0;
		}
		else if(romLoaded && autoboot)
			autoboot = false;
		else
			ExitApp();

		if(currentTiming != GCSettings.timing)
		{
			GameInfo->vidsys=(EGIV)GCSettings.timing;
			FCEU_ResetVidSys(); // causes a small 'pop' in the audio
		}

		currentTiming = GCSettings.timing;
		ConfigRequested = 0;
		ScreenshotRequested = 0;
		SwitchAudioMode(0);

		// stop checking if devices were removed/inserted
		// since we're starting emulation again
		HaltDeviceThread();

		ResetVideo_Emu();
		SetControllers();
		setFrameTimer(); // set frametimer method before emulation
		SetPalette();
		FCEUI_DisableSpriteLimitation(GCSettings.spritelimit ^ 1);

		fskip=0;
		fskipc=0;
		frameskip=0;

		while(1) // emulation loop
		{
			fskip = 0;
			
			if(turbomode)
			{
				fskip = 1;
								
				if(fskipc >= 18)
				{
					fskipc = 0;
					fskip = 0;
				}
				else
				{
					fskipc++;
				}
			}
			else if(frameskip > 0)
			{
				fskip = 1;
				
				if(fskipc >= frameskip)
				{
					fskipc = 0;
					fskip = 0;
				}
				else
				{
					fskipc++;
				}
			}

			//CAK: Currently this is designed to be used before the frame is emulated
			Check3D();

			FCEUI_Emulate(&gfx, &sound, &ssize, fskip);

			if (!shutter_3d_mode && !anaglyph_3d_mode)
				FCEUD_Update(gfx, sound, ssize);
			else if (eye_3d)
				FCEUD_UpdateRight(gfx, sound, ssize);
			else
				FCEUD_UpdateLeft(gfx, sound, ssize);

			SyncSpeed();

			if(ResetRequested)
			{
				PowerNES(); // reset game
				ResetRequested = 0;
			}
			if(ConfigRequested)
			{
				ConfigRequested = 0;
				ResetVideo_Menu();
				break;
			}
			#ifdef HW_RVL
			if(ShutdownRequested)
				ExitApp();
			#endif

		} // emulation loop
    } // main loop
}
