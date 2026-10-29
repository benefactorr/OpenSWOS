#ifndef SWOS_AI_CAREER_POOL_CAPACITY_H
#define SWOS_AI_CAREER_POOL_CAPACITY_H
/* AI transfer replacement records can now cover many seasons.
 * The career record counter is uint16_t, so this value MUST stay below 65536.
 * Original SWOS foreign-market refill quotas remain based on 432,
 * independently of this expanded replacement-record storage.
 * The active records are serialised by the JSON career save/load routines.
 */
#define SWOS_AI_CAREER_POOL_CAPACITY 50000u
#define SWOS_AI_CAREER_POOL_LEGACY_COUNT 432u
#endif
