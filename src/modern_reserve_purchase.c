#include "modern_reserve_purchase.h"
#include "modern_career_runtime.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* One authoritative transaction for players in ALL roster positions. The
 * historic 0..15 restriction was a UI limitation, never a player identity. */
static int owned(const ModernRoster *roster,const char *player_id) {
    size_t i;
    if (!roster || !player_id || !player_id[0]) return 0;
    for(i=0;i<roster->count;i++)
        if(!strcmp(roster->players[i].id,player_id)) return 1;
    return 0;
}

int ModernPurchase_Transfer(ModernWorld *world,
                            const char *seller_id,const char *buyer_id,
                            const char *player_id,int32_t fee,
                            int32_t *bank_balance,int32_t *accounting_total,
                            int is_sale) {
    ModernRoster *seller,*buyer;
    if(!world||!seller_id||!buyer_id||!player_id||
       !bank_balance||!accounting_total||!strcmp(seller_id,buyer_id)||
       fee<=0||*accounting_total<0||*accounting_total>INT32_MAX-fee)
        return 0;
    if(is_sale) {
        if(*bank_balance>INT32_MAX-fee)return 0;
    } else {
        if(*bank_balance<fee)return 0;
    }
    seller=ModernWorld_Find(world,seller_id);
    buyer=ModernWorld_Find(world,buyer_id);
    if(!seller||!buyer||!owned(seller,player_id)||
       ModernRuntime_PlayerIsBound(player_id))return 0;
    int moved = is_sale ? ModernCareer_SellWithLoanCover(world,player_id,seller_id,buyer_id)
                        : ModernCareer_Transfer(world,player_id,seller_id,buyer_id);
    if(!moved)return 0;
    /* Money only moves after the identity-preserving roster transaction. */
    *bank_balance+=is_sale?fee:-fee;
    *accounting_total+=fee;
    return 1;
}

int ModernPurchase_Reserve(ModernWorld *world,
                           const char *seller_id,const char *buyer_id,
                           const char *player_id,int32_t fee,
                           int32_t *bank_balance,int32_t *purchase_total) {
    return ModernPurchase_Transfer(world,seller_id,buyer_id,player_id,fee,
                                   bank_balance,purchase_total,0);
}
int ModernPurchase_SellReserve(ModernWorld *world,
                           const char *seller_id,const char *buyer_id,
                           const char *player_id,int32_t fee,
                           int32_t *bank_balance,int32_t *sale_total) {
    return ModernPurchase_Transfer(world,seller_id,buyer_id,player_id,fee,
                                   bank_balance,sale_total,1);
}

static int copy_roster(ModernRoster *copy, const ModernRoster *roster)
{
    *copy = *roster;
    copy->players = malloc(roster->count * sizeof *copy->players);
    if (!copy->players) return 0;
    memcpy(copy->players, roster->players, roster->count * sizeof *copy->players);
    copy->capacity = roster->count;
    return 1;
}

static int remove_offer_players(ModernRoster *roster, const char *const ids[],
                                size_t count, ModernRosterPlayer players[2])
{
    for (size_t i = 0; i < count; ++i) {
        size_t slot = 0;
        while (slot < roster->count && strcmp(roster->players[slot].id, ids[i])) ++slot;
        if (slot == roster->count || ModernRuntime_PlayerIsBound(ids[i]) ||
            (roster->players[slot].swos.positionAndFace & PF_LOAN)) return 0;
        players[i] = roster->players[slot];
        ModernRoster_Remove(roster, players[i].id);
    }
    return 1;
}

static int add_offer_players(ModernRoster *roster, ModernRosterPlayer players[2], size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        unsigned shirt = 1;
        for (; shirt < 100; ++shirt) {
            size_t j = 0;
            while (j < roster->count && (uint8_t)roster->players[j].swos.shirtNumber != shirt) ++j;
            if (j == roster->count) break;
        }
        players[i].swos.shirtNumber = (int8_t)shirt;
        if (!ModernRoster_Add(roster, &players[i])) return 0;
    }
    return 1;
}

static int cover_offer_roster(ModernWorld *world, ModernRoster *roster, const PlayerFile *origin)
{
    int has_keeper = 0;
    for (size_t i = 0; i < roster->count; ++i)
        if (!(roster->players[i].swos.positionAndFace & PF_POSITION_MASK)) {
            has_keeper = 1;
            break;
        }
    while (roster->count < MODERN_MATCH_PLAYERS || !has_keeper) {
        unsigned position = has_keeper ? 6u << PF_POSITION_SHIFT : 0;
        ModernRosterPlayer loan = ModernCareer_NewLoan(world, roster, origin, position, 0);
        if (!ModernRoster_Add(roster, &loan)) return 0;
        has_keeper = 1;
    }
    return 1;
}

int ModernPurchase_Exchange(ModernWorld *world, ModernRoster *seller, ModernRoster *buyer,
                            const char *const wanted[], size_t wanted_count,
                            const char *const offered[], size_t offered_count,
                            int32_t fee, int32_t *balance, int32_t *purchases)
{
    if (!world || !seller || !buyer || seller == buyer || !wanted_count ||
        wanted_count > 2 || offered_count > 2 || !balance || !purchases ||
        fee < 0 || (fee && *balance < fee) ||
        *purchases < 0 || *purchases > INT32_MAX - fee) return 0;

    ModernRoster staged_seller = {0}, staged_buyer = {0};
    ModernRosterPlayer incoming[2], outgoing[2];
    int ok = copy_roster(&staged_seller, seller) && copy_roster(&staged_buyer, buyer) &&
        remove_offer_players(&staged_seller, wanted, wanted_count, incoming) &&
        remove_offer_players(&staged_buyer, offered, offered_count, outgoing) &&
        add_offer_players(&staged_seller, outgoing, offered_count) &&
        add_offer_players(&staged_buyer, incoming, wanted_count) &&
        cover_offer_roster(world, &staged_seller, &incoming[0].swos) &&
        cover_offer_roster(world, &staged_buyer, &incoming[0].swos) &&
        staged_buyer.count <= 32;
    if (ok) {
        free(seller->players);
        free(buyer->players);
        *seller = staged_seller;
        *buyer = staged_buyer;
        *balance -= fee;
        *purchases += fee;
    } else {
        free(staged_seller.players);
        free(staged_buyer.players);
    }
    return ok;
}
