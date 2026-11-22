#ifndef SWOS2027_MODERN_RESERVE_PURCHASE_H
#define SWOS2027_MODERN_RESERVE_PURCHASE_H
#include "modern_career_roster.h"
#include <stdint.h>
/* Unified permanent-ID transaction, covering both original 16 and reserves.
 * is_sale=0 debits the managed club, is_sale=1 credits it. The caller
 * supplies seller and buyer club UUIDs and a confirmed fee. */
int ModernPurchase_Transfer(ModernWorld *world,
                           const char *seller_id,const char *buyer_id,
                           const char *player_id,int32_t fee,
                           int32_t *bank_balance,int32_t *accounting_total,
                           int is_sale);
/* Returns 1 only when the full-roster transfer and finance debit have committed.
 * A failed purchase has no changes to either club or to the bank balance.
 * The caller must show the fee and seek confirmation before invoking this. */
int ModernPurchase_Reserve(ModernWorld *world,
                           const char *seller_id, const char *buyer_id,
                           const char *player_id, int32_t fee,
                           int32_t *bank_balance, int32_t *purchase_total);
/* Complementary ID-based sale of a player outside the original 16.
 * UI/negotiation callers must supply a confirmed fee. */
int ModernPurchase_SellReserve(ModernWorld *world,
                           const char *seller_id, const char *buyer_id,
                           const char *player_id, int32_t fee,
                           int32_t *bank_balance, int32_t *sale_total);
/* Commit a multi-player deal and its cash component as one transaction. */
int ModernPurchase_Exchange(ModernWorld *world, ModernRoster *seller, ModernRoster *buyer,
                            const char *const wanted[], size_t wanted_count,
                            const char *const offered[], size_t offered_count,
                            int32_t fee, int32_t *balance, int32_t *purchases);
#endif
