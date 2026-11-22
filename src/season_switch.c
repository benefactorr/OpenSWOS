#include "season_switch.h"
#ifdef CAREER_PLUS_SEASON_SWITCH
#include <SDL.h>
#include "menu_data.h"
#include "menu_system.h"
#include "platform_sdl.h"
#include <stdio.h>
#include <string.h>
#include "world.h"
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

CareerPlusBootAction CareerPlus_BootAction;
static CareerPlusBootAction requestedAction;
static int requested;

static int selectedSeason;

static void ChooseClassicSeason(void)
{
    selectedSeason = 1996;
    SetExitMenuFlag();
}

static void ChooseModernSeason(void)
{
    selectedSeason = 2026;
    SetExitMenuFlag();
}

#pragma pack(push, 1)
static struct {
    MenuHeader header;
    MenuMark byteCoords;
    MenuEntry_Byte_Nav_Colors_TextPtr_OnSelect classic, modern, back;
    MenuMark wideCoords;
    struct {
        MenuCoords16 rc;
        MenuCmd_Colors colors;
        MenuCmd_TextPtr text;
        uint16_t end;
    } title;
    MenuMark end;
} seasonMenu = {
    {NULL, NULL, NULL, 0},
    MENU_MARK_BYTE_COORDS,
    {
        {92, 35, 120, 25},
        {MENU_CMD_NAV, -1, -1, -1, 1},
        {MENU_CMD_COLORS, 0xE},
        {MENU_CMD_TEXT_PTR, 0, "1996/97"},
        {MENU_CMD_ONSELECT_FIRE, ChooseClassicSeason},
        MENU_CMD_END
    },
    {
        {92, 70, 120, 25},
        {MENU_CMD_NAV, -1, -1, 0, 2},
        {MENU_CMD_COLORS, 0xE},
        {MENU_CMD_TEXT_PTR, 0, "2026/27"},
        {MENU_CMD_ONSELECT_FIRE, ChooseModernSeason},
        MENU_CMD_END
    },
    {
        {102, -71, 100, 15},
        {MENU_CMD_NAV, -1, -1, 1, -1},
        {MENU_CMD_COLORS, 0xC},
        {MENU_CMD_TEXT_PTR, 0, "BACK"},
        {MENU_CMD_ONSELECT_FIRE, SetExitMenuFlag},
        MENU_CMD_END
    },
    MENU_MARK_WIDE_COORDS,
    {
        {0, 0, 304, 15},
        {MENU_CMD_COLORS, 0x17},
        {MENU_CMD_TEXT_PTR, 0, "SELECT STARTING SEASON"},
        MENU_CMD_END
    },
    MENU_MARK_END_MENU
};
#pragma pack(pop)

int CareerPlus_ChooseSeason(CareerPlusBootAction action)
{
    selectedSeason = 0;
    seasonMenu.title.text.textPtr = action == CAREER_PLUS_LOAD ?
        "SELECT SAVE SEASON" : "SELECT STARTING SEASON";
    seasonMenu.header.defaultEntry = World_Current() == WORLD_2627 ? 1 : 0;
    ShowMenu((uint8_t *)&seasonMenu);
    int season = selectedSeason;
    if (season == 0)
        return 0;
    if ((season == 2026) == (World_Current() == WORLD_2627))
        return 1;
    requested = season;
    requestedAction = action;
    Platform_RequestQuit();
    return 0;
}

int CareerPlus_LaunchRequestedSeason(void)
{
    if (!requested)
        return 0;
    const char *worldArg = requested == 2026 ? "--world=2627" : "--world=9697";
    const char *startArg = requestedAction == CAREER_PLUS_LOAD ?
        "--career-plus-load" : "--career-plus-setup";
#ifdef _WIN32
    char target[4096], quoted[sizeof target + 2];
    DWORD len = GetModuleFileNameA(NULL, target, sizeof target);
    if (len && len < sizeof target) {
        snprintf(quoted, sizeof quoted, "\"%s\"", target);
        if (_spawnl(_P_NOWAIT, target, quoted, worldArg, startArg, NULL) != -1)
            return 0;
    }
#else
    const char *target = "/proc/self/exe";
    execl(target, "SWOS.x86_64", worldArg, startArg, (char *)NULL);
#endif
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Career+ starting season",
                             "Could not launch the selected season.", NULL);
    return 1;
}
#endif
