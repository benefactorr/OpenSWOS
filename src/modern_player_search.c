/* Career+ search screen backed by the active Modernised UUID rosters. */
#include "menu_data.h"
#include "menus.h"
#include "modern_career_runtime.h"
#include "modern_national.h"
#include "teams.h"
#include "string_utils.h"
#include "world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEARCH_ROWS 10

typedef enum { SEARCH_WORLD, SEARCH_NATIONAL } SearchMode;

typedef struct SearchPlayer {
    uint16_t team, slot;
    const ModernRosterPlayer *nationalPlayer;
    uint8_t position, age;
    uint32_t valueK;
    char id[MODERN_ID_LEN];
    char name[24], club[18];
} SearchPlayer;

static SearchPlayer *s_players;
static int *s_filtered;
static int s_count, s_visible;
static int s_filter, s_page, s_action, s_selected, s_min, s_max;
static int s_sort;
extern uint16_t _isNationalSelector, _careerNumPlayers, _careerAddIntlPlayerResult;
extern uint8_t _nationalTeamNationality;
extern uint16_t _selTeamNumber, _transferCompleted;
extern void SetExitMenuFlag(void);
extern void ShowMenu(uint8_t *menu);

static char s_nationalTitle[]="NATIONAL PLAYER SEARCH";
static char s_title[]="WORLD PLAYER SEARCH";
static char s_rows[SEARCH_ROWS][75];
static char s_filterText[35], s_minText[35], s_maxText[35], s_pageText[40];
static const unsigned s_priceBands[] = {0,100,500,1000,2500,5000,10000,25000,50000,100000,180000};
/* SWOS positionAndFace bits 5..7: GK RB LB D RW LW M ATT.
 * Search filter 0 means ALL; encoded position + 1 is the filter ID. */
static const char *s_posLabel[] = {"ALL", "GK", "RB", "LB", "D", "RW", "LW", "M", "ATT"};
#define SEARCH_POSITION_FILTERS (sizeof(s_posLabel) / sizeof(s_posLabel[0]))

static void search_row0(void),search_row1(void),search_row2(void),search_row3(void),search_row4(void);
static void search_row5(void),search_row6(void),search_row7(void),search_row8(void),search_row9(void);
static void search_filter(void),search_min(void),search_max(void),search_prev(void),search_next(void),search_exit(void);

/* SWOS navigation uses absolute menu entry ordinals, including headings. */
#pragma pack(push,1)
typedef struct SearchMenu {
    MenuHeader hdr;
    MenuMark mark;
    MenuEntry_Byte_Colors_TextPtr title;
    MenuEntry_Byte_Colors_TextPtr columnHead;
    MenuEntry_Byte_Nav_Colors_TextPtr_OnSelect filter, minPrice, maxPrice;
    MenuEntry_Byte_Nav_Colors_TextPtr_OnSelect rows[SEARCH_ROWS];
    MenuEntry_Byte_Colors_TextPtr pager;
    MenuEntry_Byte_Nav_Colors_TextPtr_OnSelect prev, next, exit;
    MenuMark end;
} SearchMenu;
#pragma pack(pop)

enum {
    SEARCH_FILTER_ENTRY = 2,
    SEARCH_MIN_ENTRY = 3,
    SEARCH_MAX_ENTRY = 4,
    SEARCH_FIRST_ROW_ENTRY = 5,
    SEARCH_LAST_ROW_ENTRY = SEARCH_FIRST_ROW_ENTRY + SEARCH_ROWS - 1,
    SEARCH_PREV_ENTRY = 16,
    SEARCH_NEXT_ENTRY = 17,
    SEARCH_EXIT_ENTRY = 18,
};
static char s_colHead[]="PLAYER (AGE)         CLUB       POS   VALUE";
#define ROW(y,i,fn) { { 8, (y), -16, 9 }, \
    { MENU_CMD_NAV, SEARCH_FILTER_ENTRY, SEARCH_MIN_ENTRY, \
      ((i)==0 ? SEARCH_MAX_ENTRY : (SEARCH_FIRST_ROW_ENTRY+(i)-1)), \
      ((i)==SEARCH_ROWS-1 ? SEARCH_PREV_ENTRY : (SEARCH_FIRST_ROW_ENTRY+(i)+1)) }, \
    {MENU_CMD_COLORS,0xE}, {MENU_CMD_TEXT_PTR,STR_ALIGN_LEFT,s_rows[i]}, \
    {MENU_CMD_ONSELECT_FIRE,fn}, MENU_CMD_END }
#define BTN(x,y,w,left,right,up,down,str,fn) { {x,y,w,13}, \
    {MENU_CMD_NAV,left,right,up,down}, {MENU_CMD_COLORS,0xE}, \
    {MENU_CMD_TEXT_PTR,0x0,str}, {MENU_CMD_ONSELECT_FIRE,fn}, MENU_CMD_END }
static SearchMenu s_menu={
    {0,0,0,SEARCH_FIRST_ROW_ENTRY}, MENU_MARK_BYTE_COORDS,
    {{48,4,-96,14},{MENU_CMD_COLORS,0x17},{MENU_CMD_TEXT_PTR,0,s_title},MENU_CMD_END},
    {{15,63,-28,9},{MENU_CMD_COLORS,0x0},{MENU_CMD_TEXT_PTR,STR_ALIGN_LEFT,s_colHead},MENU_CMD_END},
    BTN(8,23,145, SEARCH_MAX_ENTRY,SEARCH_MIN_ENTRY,SEARCH_EXIT_ENTRY,SEARCH_MAX_ENTRY,s_filterText,search_filter),
    BTN(163,23,145,SEARCH_FILTER_ENTRY,SEARCH_MAX_ENTRY,SEARCH_EXIT_ENTRY,SEARCH_MAX_ENTRY,s_minText,search_min),
    BTN(80,42,160,SEARCH_MIN_ENTRY,SEARCH_FILTER_ENTRY,SEARCH_MIN_ENTRY,SEARCH_FIRST_ROW_ENTRY,s_maxText,search_max),
    { ROW(74,0,search_row0), ROW(84,1,search_row1), ROW(94,2,search_row2),
      ROW(104,3,search_row3), ROW(114,4,search_row4), ROW(124,5,search_row5),
      ROW(134,6,search_row6), ROW(144,7,search_row7), ROW(154,8,search_row8),
      ROW(164,9,search_row9) },
    {{164,178,143,10},{MENU_CMD_COLORS,0x0},{MENU_CMD_TEXT_PTR,0,s_pageText},MENU_CMD_END},
    BTN(14,187,67, SEARCH_EXIT_ENTRY,SEARCH_NEXT_ENTRY,SEARCH_LAST_ROW_ENTRY,SEARCH_FILTER_ENTRY,"PREV",search_prev),
    BTN(90,187,67, SEARCH_PREV_ENTRY,SEARCH_EXIT_ENTRY,SEARCH_LAST_ROW_ENTRY,SEARCH_MIN_ENTRY,"NEXT",search_next),
    BTN(236,187,71,SEARCH_NEXT_ENTRY,SEARCH_PREV_ENTRY,SEARCH_LAST_ROW_ENTRY,SEARCH_MAX_ENTRY,"EXIT",search_exit),
    MENU_MARK_END_MENU
};

static void search_choose(int n) { s_action=1; s_selected=n; SetExitMenuFlag(); }
static void search_row0(void){search_choose(0);} static void search_row1(void){search_choose(1);}
static void search_row2(void){search_choose(2);} static void search_row3(void){search_choose(3);}
static void search_row4(void){search_choose(4);} static void search_row5(void){search_choose(5);}
static void search_row6(void){search_choose(6);} static void search_row7(void){search_choose(7);}
static void search_row8(void){search_choose(8);} static void search_row9(void){search_choose(9);}
static void search_filter(void){s_action=2;SetExitMenuFlag();}
static void search_min(void){s_action=3;SetExitMenuFlag();}
static void search_max(void){s_action=7;SetExitMenuFlag();}
static void search_prev(void){s_action=4;SetExitMenuFlag();}
static void search_next(void){s_action=5;SetExitMenuFlag();}
static void search_exit(void){s_action=6;SetExitMenuFlag();}

static int scan_world(SearchMode mode)
{
    ModernWorld *world = ModernWorld_Active();
    size_t capacity = 0;
    int national = mode == SEARCH_NATIONAL;
    for (size_t c = 0; c < world->count; ++c)
        capacity += world->clubs[c].count;
    if (!capacity) return 0;
    SearchPlayer *players = calloc(capacity, sizeof *players);
    int *filtered = malloc(capacity * sizeof *filtered);
    if (!players || !filtered) {
        free(players);
        free(filtered);
        return 0;
    }
    free(s_players);
    free(s_filtered);
    s_players = players;
    s_filtered = filtered;
    s_count = 0;
    for (size_t c = 0; c < world->count; ++c) {
        const ModernRoster *roster = &world->clubs[c];
        uint16_t team = 0xFFFF;
        int active = ModernRuntime_ActiveClub(roster, &team);
        if (!national && (!active || team == _selTeamNumber))
            continue;
        for (size_t i = 0; i < roster->count; ++i) {
            const ModernRosterPlayer *player = &roster->players[i];
            if ((player->swos.positionAndFace & PF_LOAN) || !player->swos.playerName[0] || (uint8_t)player->swos.playerName[0] == 255)
                continue;
            if (national && ((uint8_t)player->swos.nationality != _nationalTeamNationality ||
                             !ModernNational_PlayerEligible(player)))
                continue;
            SearchPlayer *entry = &s_players[s_count++];
            entry->team = team;
            entry->slot = (uint16_t)i;
            entry->nationalPlayer = national ? player : NULL;
            entry->valueK = ModernPlayerValueThousands((uint8_t)player->swos.price);
            entry->position = ((uint8_t)player->swos.positionAndFace >> 5) + 1;
            entry->age = player->age;
            memcpy(entry->id, player->id, sizeof entry->id);
            snprintf(entry->name, sizeof entry->name, "%.23s", (const char *)player->swos.playerName);
            snprintf(entry->club, sizeof entry->club, "%.17s", (const char *)roster->header.teamName);
        }
    }
    return 1;
}

static int compar_player(const void*a,const void*b){
    const SearchPlayer *x=&s_players[*(const int*)a],*y=&s_players[*(const int*)b];
    int d = s_sort == 2 ? 0 : (int)y->valueK - (int)x->valueK;
    if (s_sort == 1) d = -d;
    if (!d) d = strcmp(x->name, y->name);
    return d ? d : strcmp(x->id, y->id);
}
static void build_list(SearchMode mode){
    s_visible=0;
    for (int i = 0; i < s_count; ++i) {
        const SearchPlayer *p = &s_players[i];
        if (s_filter && p->position != s_filter)
            continue;
        if (p->valueK < s_priceBands[s_min] || p->valueK > s_priceBands[s_max])
            continue;
        if (p->nationalPlayer && ModernNational_PlayerCalledUp(p->team, &p->nationalPlayer->swos, p->club))
            continue;
        s_filtered[s_visible++] = i;
    }
    qsort(s_filtered,s_visible,sizeof(s_filtered[0]),compar_player);
    int total = (s_visible + SEARCH_ROWS - 1) / SEARCH_ROWS;
    if (!total) total = 1;
    if (s_page >= total) s_page = total - 1;
    snprintf(s_filterText,sizeof(s_filterText),"POSITION: %s",s_posLabel[s_filter]);
    uint8_t minValue[16], maxValue[16];
    FormatCompactMoney(s_priceBands[s_min], minValue);
    FormatCompactMoney(s_priceBands[s_max], maxValue);
    if (mode == SEARCH_NATIONAL)
        snprintf(s_minText, sizeof s_minText, "SORT: %s", s_sort == 0 ? "HIGH" : s_sort == 1 ? "LOW" : "NAME");
    else
        snprintf(s_minText,sizeof(s_minText),"MIN: %s", minValue);
    snprintf(s_maxText,sizeof(s_maxText),"MAX: %s",s_max==10?"ANY":"LIMIT");
    if(s_max!=10)snprintf(s_maxText,sizeof(s_maxText),"MAX: %s", maxValue);
    snprintf(s_pageText,sizeof(s_pageText),"%d MATCHES  %d/%d",s_visible,s_page+1,total);
    for(int row=0;row<SEARCH_ROWS;row++){
        int at=s_page*SEARCH_ROWS+row;
        if(at>=s_visible){s_rows[row][0]=0;continue;}
        SearchPlayer *p=&s_players[s_filtered[at]];
        uint8_t val[16];
        FormatCompactMoney((uint32_t)p->valueK, val);
        /* Left-aligned name and age in one field. Keep the overall row width
         * identical to the original so the right-hand value stays in place. */
        char namedAge[48];
        /* The SWOS row font is proportional, so spaces alone do not
         * guarantee exact columns. Left-align the *menu entry itself* and
         * use concise fixed-width fields within its 288px row. */
        snprintf(namedAge,sizeof(namedAge),"%.16s (%u)",
                 p->name, (unsigned)p->age);
        snprintf(s_rows[row],sizeof(s_rows[row]),"%-21.21s %-10.10s %-3s %6s",
                 namedAge,p->club,s_posLabel[p->position],val);

    }
}
static void show_player_search(SearchMode mode)
{
    if (!ModernRuntime_Enabled() || !scan_world(mode)) return;
    s_menu.title.text.textPtr = mode == SEARCH_NATIONAL ? s_nationalTitle : s_title;
    s_filter = s_min = s_page = s_sort = 0;
    s_max = 10;
    unsigned focus = SEARCH_FIRST_ROW_ENTRY;
    for (;;) {
        build_list(mode);
        if (mode == SEARCH_NATIONAL && focus >= SEARCH_FIRST_ROW_ENTRY &&
            focus <= SEARCH_LAST_ROW_ENTRY &&
            s_page * SEARCH_ROWS + (int)focus - SEARCH_FIRST_ROW_ENTRY >= s_visible)
            focus = SEARCH_FILTER_ENTRY;
        s_action = 0;
        s_menu.hdr.defaultEntry = focus;
        ShowMenu((uint8_t *)&s_menu);
        if (!s_action || s_action == 6) break;
        if (s_action == 1) {
            int at = s_page * SEARCH_ROWS + s_selected;
            focus = SEARCH_FIRST_ROW_ENTRY + s_selected;
            if (at < s_visible) {
                SearchPlayer selected = s_players[s_filtered[at]];
                if (mode == SEARCH_NATIONAL) {
                    uint16_t result = ModernNational_CallUp(selected.team, selected.slot,
                                                          &selected.nationalPlayer->swos, selected.club);
                    if (result == 1 || _careerNumPlayers >= 20)
                        break;
                } else {
                    ModernSearch_Offer(selected.team, selected.id);
                    if (_transferCompleted) break;
                }
            }
        } else if (s_action == 2) {
            s_filter = (s_filter + 1) % SEARCH_POSITION_FILTERS;
            s_page = 0;
            focus = SEARCH_FILTER_ENTRY;
        } else if (s_action == 3) {
            if (mode == SEARCH_NATIONAL)
                s_sort = (s_sort + 1) % 3;
            else {
                s_min = (s_min + 1) % 11;
                if (s_min > s_max) s_max = s_min;
            }
            s_page = 0;
            focus = SEARCH_MIN_ENTRY;
        } else if (s_action == 7) {
            s_max = (s_max + 1) % 11;
            if (s_max < s_min) s_min = s_max;
            s_page = 0;
            focus = SEARCH_MAX_ENTRY;
        } else if (s_action == 4) {
            if (s_page > 0) --s_page;
            focus = SEARCH_PREV_ENTRY;
        } else if (s_action == 5) {
            if ((s_page + 1) * SEARCH_ROWS < s_visible) ++s_page;
            focus = SEARCH_NEXT_ENTRY;
        }
    }
    free(s_players);
    free(s_filtered);
    s_players = NULL;
    s_filtered = NULL;
}


void ModernSearch_Open(void)
{
    show_player_search(SEARCH_WORLD);
}


void ModernSearch_National(void)
{
    _careerAddIntlPlayerResult = 0xFFFF;
    if (_isNationalSelector)
        show_player_search(SEARCH_NATIONAL);
}
