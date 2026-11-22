/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
#ifndef STRING_UTILS_H
#define STRING_UTILS_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef short int16_t;
typedef int int32_t;

/* ---- String utility functions ---- */
uint16_t GetStringLength(const uint8_t *str);
int16_t StringEquals(const uint8_t *s1, const uint8_t *s2);
uint8_t *StringCopy(const uint8_t *src, uint8_t *dst);
uint8_t *StringCopyUntilChar(const uint8_t *src, uint8_t *dst, uint8_t stopChar);

/* ---- Number formatting (Chunk 100) ---- */
/* Convert stored economy units to the amounts shown in the current world. */
unsigned CareerMoneyScale(void);
/* Format a displayed amount in thousands as K or M, without a currency sign. */
void FormatCompactMoney(uint32_t thousands, uint8_t *dst);
void Int2Pounds(uint32_t pounds, uint8_t *dst);
void Int2PoundsSigned(int32_t pounds, uint8_t *dst);
void WriteAccountBalance(int32_t delta, uint8_t *dst);
void GetBankBalance(uint8_t *dst);
void PrepareMenuForFadeIn(void);
/* Select the money symbol: dollar!=0 -> '$', else the default '£'. */
void Currency_SetDollar(int dollar);

#endif
