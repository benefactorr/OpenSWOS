"""Structural checks for the R8b source integration.
These do not substitute for testing mouse actions in the compiled SWOS game.
"""
from pathlib import Path
p = Path(__file__).resolve().parents[1] / 'src' / 'transfer_market.c'
s=p.read_text(encoding='utf-8')
assert 'CLICK AGAIN' not in s, 'Fixed-price purchase shortcut still present'
assert 'ShowPlayerOfferMenuForPlayer(team_number, slot)' in s
assert 'if(ModernRuntime_Enabled() && !modernNegotiatedBuy.committed)' in s
assert 'fee = (int32_t)_offerOrdinals.offerAmount * 1000' in s
assert 'modernNegotiatedBuy.player_id' in s
assert 'memcpy(player_id, modernBuyRoster->players[realIndex].id, sizeof player_id)' in s
assert 'ModernBuy_ClearOffer();' in s
assert 'modernNegotiatedBuy.saved_preview' in s
assert 'ModernRuntime_Suspend(world,modernNegotiatedBuy.team)' in s
assert 'modernNegotiatedBuy.committed = ModernPurchase_Exchange' in s
assert 'if (!modernBuyRoster || realIndex >= modernBuyRoster->count)' in s
assert 'wanted[wanted_count++] = second;' in s
assert 'offered[offered_count++] = previous[slot];' in s
print('R8b negotiation path structural checks: PASS')
