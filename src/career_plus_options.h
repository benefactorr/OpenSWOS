/* OpenSWOS Career+ runtime feature switches.  PC gameplay only. */
#ifndef CAREER_PLUS_OPTIONS_H
#define CAREER_PLUS_OPTIONS_H
#include <stdint.h>
/* Stored as uint16_t to match the original menu string-table ABI. */
extern uint16_t _careerImprovedAiOption;  /* 1=improved, 0=original */
extern uint16_t _careerThreeButtonOption; /* 0=classic, 1=A/S/D */
#endif
