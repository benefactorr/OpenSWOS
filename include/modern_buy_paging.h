#ifndef SWOS_MODERN_BUY_PAGING_H
#define SWOS_MODERN_BUY_PAGING_H
#include <stddef.h>
/* Keep the original 16-row SWOS buying screen. In the expanded mode the
 * final row becomes a continuation item, leaving 15 players per page. */
#define MODERN_BUY_ROWS 16u
#define MODERN_BUY_PAGE_PLAYERS 15u
static inline size_t ModernBuy_PageCount(size_t players) {
    return players > MODERN_BUY_ROWS ?
           (players + MODERN_BUY_PAGE_PLAYERS - 1u)/MODERN_BUY_PAGE_PLAYERS : 1u;
}
static inline int ModernBuy_IsContinuation(size_t players,size_t row) {
    return players > MODERN_BUY_ROWS && row == MODERN_BUY_ROWS-1u;
}
static inline int ModernBuy_PlayerIndex(size_t players,size_t page,size_t row,size_t *index) {
    size_t start,position;
    if(!index || row >= MODERN_BUY_ROWS || ModernBuy_IsContinuation(players,row))return 0;
    if(players <= MODERN_BUY_ROWS) {
        if(row>=players)return 0;
        *index=row;return 1;
    }
    if(page >= ModernBuy_PageCount(players))return 0;
    start=page*MODERN_BUY_PAGE_PLAYERS;
    position=start+row;
    if(position>=players)return 0;
    *index=position;return 1;
}
#endif
