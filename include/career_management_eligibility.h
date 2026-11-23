#ifndef CAREER_MANAGEMENT_ELIGIBILITY_H
#define CAREER_MANAGEMENT_ELIGIBILITY_H

#include <stdint.h>
#include "world.h"

/* Career+ management restriction only. Keep teams, players, fixtures and
 * AI-controlled competitions intact. IDs are packed country/file:ordinal. */
static inline int Career_ManagementTeamAllowed(uint16_t teamId)
{
    uint8_t file = (uint8_t)(teamId >> 8);
    uint8_t ordinal = (uint8_t)teamId;
    if (file == 80 && (ordinal == 4 || ordinal == 37))
        return 0; /* Belarus and Russia national teams, both eras */
    if (file >= 80 && file <= 85)
        return 1;
    file = World_FileCountry(file); /* handles second TEAM file in modern world */
    return file != 31 && file != 76; /* Russian and Belarusian clubs */
}

#endif
