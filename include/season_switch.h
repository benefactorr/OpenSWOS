#ifndef CAREER_PLUS_SEASON_SWITCH_H
#define CAREER_PLUS_SEASON_SWITCH_H
#if defined(SWOS_SDL_BUILD) && (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))) && !defined(SWOS_WASM_BUILD) && !defined(SWOS_SWITCH_BUILD)
#define CAREER_PLUS_SEASON_SWITCH 1
typedef enum {
    CAREER_PLUS_NONE, CAREER_PLUS_SETUP, CAREER_PLUS_LOAD
} CareerPlusBootAction;
extern CareerPlusBootAction CareerPlus_BootAction;
int CareerPlus_ChooseSeason(CareerPlusBootAction action);
int CareerPlus_LaunchRequestedSeason(void);
#endif
#endif
