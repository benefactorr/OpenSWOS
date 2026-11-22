#ifndef CAREER_WINNERS_H
#define CAREER_WINNERS_H

#include "mod_api.h"
#include "mjson.h"

void CareerWinners_Clear(void);
void CareerWinners_Record(const ModCompetitionResult *result);
void CareerWinners_Save(mjson_print_fn_t output, void *context);
void CareerWinners_LoadRow(const char *row, int length);
void CareerWinners_Open(void);

#endif
