#ifndef SWOS2027_MODERN_CAREER_SAVE_H
#define SWOS2027_MODERN_CAREER_SAVE_H
#include "modern_world.h"
/* The original .CAR and .r04 world must match. Only used in opt-in R4 mode.
 * Paths follow the same per-user save directory used by swos_io.c. */
int ModernSave_Write(const unsigned char *career_filename,const ModernWorld *world);
int ModernSave_PrepareLoad(const unsigned char *career_filename,ModernWorld *staged);
/* Checkpoint the previous .CAR before its JSON writer runs. If the roster
 * sidecar write fails, restore that CAR rather than retaining a mismatched
 * career/roster pairing. Not power-loss atomic. */
int ModernSave_BeginCheckpoint(const unsigned char *career_filename);
void ModernSave_EndCheckpoint(void);
int ModernSave_RollbackCheckpoint(void);
#endif
