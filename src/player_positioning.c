#include "career_plus_options.h"
/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
#include "player_positioning.h"

/* ==================================================================
 * Chunk 53 — Player Tactical Positioning
 *
 * Ported functions:
 *   SetPlayerWithNoBallDestination (A1=player, A6=team, D6=ballX, D7=ballY)
 * ================================================================== */

/* ---- External globals ---- */
extern TeamGeneralInfo _topTeamData;
extern TeamGeneralInfo _bottomTeamData;

extern int16_t  _gameState;
extern int16_t  _gameStatePl;

extern int16_t  _ballQuadrantIndex;
extern int16_t  _playerXQuadrantOffset;
extern int16_t  _playerYQuadrantOffset;

/* Tactics lookup table: array of raw 68k pointers to TeamTactics structures */
extern void *_teamTacticsTable[];

/* Position lookup tables (in SWOS2.s code section) */
extern int16_t  _playerXQuadrantsCoordinates[];
extern int16_t  _playerYQuadrantCoordinates[];


/* Career+ advanced AI v3: supporting runs for computer teams.
 * Preserve SWOS tactical destinations; only the two best eligible nearby
 * midfield/forward players receive a small, bounded open-play adjustment.
 * No new random values or persistent match state are required.
 */
static int32_t SupportDistSq(int32_t x, int32_t y)
{
    return x * x + y * y;
}

static int16_t SupportClamp(int32_t x, int16_t lo, int16_t hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return (int16_t)x;
}

/* Estimate space around a proposed destination and potential passing lane.
 * Small integer geometry keeps this cheap and deterministic. */
static int32_t SupportPositionScore(TeamGeneralInfo *team, Sprite *carrier,
                                    Sprite *runner, int16_t x, int16_t y)
{
    Sprite **opps = team->opponentsTeam->spritesTable;
    Sprite **mates = team->spritesTable;
    int32_t closestOpp = 18000, closestMate = 18000;
    int32_t dx = (int32_t)x - carrier->x.s.i;
    int32_t dy = (int32_t)y - carrier->y.s.i;
    int32_t passLen2 = SupportDistSq(dx, dy);
    int32_t score;
    int i;

    if (passLen2 < 900 || passLen2 > 90000)
        return -100000;
    for (i = 0; i < 11; ++i) {
        Sprite *opp = opps ? opps[i] : 0;
        if (opp && !opp->sentAway && opp->playerState == PL_NORMAL) {
            int32_t ox = (int32_t)opp->x.s.i - x;
            int32_t oy = (int32_t)opp->y.s.i - y;
            int32_t d2 = SupportDistSq(ox, oy);
            if (d2 < closestOpp) closestOpp = d2;
            /* Reject heavily blocked corridors, but not speculative runs. */
            ox = (int32_t)opp->x.s.i - carrier->x.s.i;
            oy = (int32_t)opp->y.s.i - carrier->y.s.i;
            {
                int32_t dot = ox * dx + oy * dy;
                if (dot > passLen2 / 8 && dot < passLen2 * 7 / 8) {
                    int64_t cross = (int64_t)ox * dy - (int64_t)oy * dx;
                    if (cross * cross < (int64_t)passLen2 * 324)
                        return -100000;
                }
            }
        }
        if (mates) {
            Sprite *mate = mates[i];
            if (mate && mate != runner && mate != carrier &&
                !mate->sentAway && mate->playerState == PL_NORMAL) {
                int32_t mx = (int32_t)mate->x.s.i - x;
                int32_t my = (int32_t)mate->y.s.i - y;
                int32_t d2 = SupportDistSq(mx, my);
                if (d2 < closestMate) closestMate = d2;
            }
        }
    }
    if (closestOpp < 625 || closestMate < 625)
        return -100000;
    score = (closestOpp > 8100 ? 8100 : closestOpp) / 45;
    score += (closestMate > 3600 ? 3600 : closestMate) / 60;
    /* Prefer a forward option, without excessively disrupting tactics. */
    score += (team == &_topTeamData ? dy : -dy) / 2;
    return score;
}

static void SupportAdjustRun(Sprite *runner, TeamGeneralInfo *team)
{
    Sprite *carrier = team->controlledPlayer;
    Sprite **players = team->spritesTable;
    int32_t dx, dy, forwardDist, range;
    int bestIndex = -1, secondIndex = -1;
    int32_t best = 0x7fffffff, second = 0x7fffffff;
    int32_t baseScore, chosenScore;
    int16_t chosenX, chosenY;
    int forward, i;
    static const int16_t shiftX[5] = { 0, -38, 38, -25, 25 };
    static const int16_t shiftY[5] = { 0, 12, 12, 44, 44 };

    if (team->playerNumber != 0 || !team->playerHasBall ||
        _gameStatePl != ST_GAME_IN_PROGRESS || !players || !carrier ||
        carrier->playerState != PL_NORMAL || carrier->sentAway ||
        carrier->ballDistance > 900 || runner == carrier ||
        runner->playerOrdinal < 6 || runner->sentAway ||
        runner->playerState != PL_NORMAL)
        return;

    forward = team == &_topTeamData ? 1 : -1;
    dx = (int32_t)runner->x.s.i - carrier->x.s.i;
    dy = (int32_t)runner->y.s.i - carrier->y.s.i;
    forwardDist = dy * forward;
    range = SupportDistSq(dx, dy);
    if (forwardDist < -45 || forwardDist > 210 || range < 2500 || range > 62500)
        return;

    /* Choose at most two closest eligible supporting players.  Stable squad
     * index resolves ties and avoids every teammate running at once. */
    for (i = 0; i < 11; ++i) {
        Sprite *pl = players[i];
        int32_t px, py, pd, progress;
        if (!pl || pl == carrier || pl->playerOrdinal < 6 ||
            pl->sentAway || pl->playerState != PL_NORMAL)
            continue;
        px = (int32_t)pl->x.s.i - carrier->x.s.i;
        py = (int32_t)pl->y.s.i - carrier->y.s.i;
        progress = py * forward;
        pd = SupportDistSq(px, py);
        if (progress < -45 || progress > 210 || pd < 2500 || pd > 62500)
            continue;
        if (pd < best) {
            second = best;
            secondIndex = bestIndex;
            best = pd;
            bestIndex = i;
        } else if (pd < second) {
            second = pd;
            secondIndex = i;
        }
    }
    if (runner != (bestIndex >= 0 ? players[bestIndex] : 0) &&
        runner != (secondIndex >= 0 ? players[secondIndex] : 0))
        return;

    chosenX = runner->destX;
    chosenY = runner->destY;
    baseScore = SupportPositionScore(team, carrier, runner, chosenX, chosenY);
    chosenScore = baseScore;
    for (i = 1; i < 5; ++i) {
        int16_t x = SupportClamp((int32_t)runner->destX + shiftX[i], 105, 566);
        int16_t y = SupportClamp((int32_t)runner->destY + forward * shiftY[i], 157, 741);
        int32_t rating;
        /* Do not move the tactical target too far from the player. */
        if (SupportDistSq((int32_t)x - runner->x.s.i,
                          (int32_t)y - runner->y.s.i) > 22500)
            continue;
        rating = SupportPositionScore(team, carrier, runner, x, y);
        if (rating > chosenScore + 30) {
            chosenScore = rating;
            chosenX = x;
            chosenY = y;
        }
    }
    runner->destX = chosenX;
    runner->destY = chosenY;
}

/* ==================================================================
 * SetPlayerWithNoBallDestination
 *   from: SetPlayerWithNoBallDestination
 *   in:  A1=player, A6=team, D6=ballX, D7=ballY
 *
 * Looks up team tactics table, gets position from quadrant-based
 * positioning table (35 entries per player x ballQuadrantIndex),
 * maps to world coordinates via playerXQuadrantsCoordinates /
 * playerYQuadrantCoordinates tables.
 * Goalkeeper uses separate proportional positioning (mulu/divu).
 * ================================================================== */
void __attribute__((used, externally_visible))
SetPlayerWithNoBallDestination(Sprite *player, TeamGeneralInfo *team,
                                int16_t ballX, int16_t ballY)
{
    /* Look up tactics structure */
    TeamTactics *tactics = (TeamTactics *)(_teamTacticsTable[team->tactics]);

    /* If ball out of play: use redirect tactics */
    if (_gameState == ST_KEEPER_HOLDS_BALL ||
        _gameState == ST_GOAL_OUT_LEFT ||
        _gameState == ST_GOAL_OUT_RIGHT) {
        int16_t idx = (int16_t)(int8_t)tactics->ballOutOfPlayTactics;
        tactics = (TeamTactics *)(_teamTacticsTable[idx]);
    }

    /* Advance to positions array */
    uint8_t *positions = (uint8_t *)tactics->positions;

    int16_t ordinal = player->playerOrdinal;
    int16_t d0 = ordinal - 2;

    if (d0 < 0) {
        /* Goalkeeper: proportional positioning */
        int16_t leftX = 285;
        int16_t rightX = 387;
        int16_t xRange = rightX - leftX + 1; /* 103 */
        uint16_t xRel = (uint16_t)(ballX - 81);
        uint16_t destXCalc = (uint16_t)(((uint32_t)xRel * (uint32_t)xRange) / 510u);
        player->destX = (int16_t)(leftX + destXCalc);

        int16_t topY = 135;
        int16_t botY = 161;
        if (team != &_topTeamData) {
            topY = 737;
            botY = 763;
        }
        int16_t yRange = botY - topY + 1;
        uint16_t yRel = (uint16_t)(ballY - 129);
        uint16_t destYCalc = (uint16_t)(((uint32_t)yRel * (uint32_t)(uint16_t)yRange) / 641u);
        player->destY = (int16_t)(topY + destYCalc);
        return;
    }

    /* Non-goalkeeper: look up position from tactics table */
    uint16_t tableOffset = (uint16_t)d0 * 35;
    int16_t d3 = _playerXQuadrantOffset;
    int16_t d4 = _playerYQuadrantOffset;
    uint8_t positionByte;

    if (team == &_topTeamData) {
        tableOffset += (uint16_t)_ballQuadrantIndex;
        positionByte = positions[tableOffset];
    } else {
        /* Right/bottom team: mirror index and position */
        uint16_t mirrorIdx = 34 - (uint16_t)_ballQuadrantIndex;
        tableOffset += mirrorIdx;
        positionByte = (uint8_t)(0xEF - positions[tableOffset]);
    }

    /* Extract X and Y indices from packed position byte */
    int16_t xIdx = (positionByte >> 4) & 0xF;
    int16_t yIdx = positionByte & 0xF;

    d3 += _playerXQuadrantsCoordinates[xIdx];
    d4 += _playerYQuadrantCoordinates[yIdx];

    /* Team-specific X adjustment */
    d3 -= 4;
    if (team != &_topTeamData)
        d3 += 8; /* net +4 for bottom team */

    /* Clamp to pitch bounds */
    if (d3 < 81) d3 = 81;
    if (d3 > 590) d3 = 590;
    if (d4 < 129) d4 = 129;
    if (d4 > 769) d4 = 769;

    player->destX = d3;
    player->destY = d4;
    if (_careerImprovedAiOption)
        SupportAdjustRun(player, team);

    /* Career+ defensive pressure: support the active defender against a human
     * ball carrier, without pulling the entire defensive line out of shape.
     * Only a single nearby outfield teammate receives a modest adjustment. */
    if (_careerImprovedAiOption && team->playerNumber == 0 && !team->playerHasBall &&
        team->opponentsTeam && team->opponentsTeam->playerNumber != 0 &&
        team->opponentsTeam->playerHasBall &&
        _gameStatePl == ST_GAME_IN_PROGRESS &&
        player != team->controlledPlayer &&
        player->playerOrdinal > 1 && !player->sentAway &&
        player->playerState == PL_NORMAL && team->spritesTable) {
        Sprite *attacker = team->opponentsTeam->controlledPlayer;
        Sprite *candidate = 0;
        int32_t closest = 16000;
        int i;
        if (attacker && !attacker->sentAway &&
            attacker->playerState == PL_NORMAL) {
            for (i = 0; i < 11; ++i) {
                Sprite *p = team->spritesTable[i];
                int32_t dx, dy, d2;
                if (!p || p == team->controlledPlayer ||
                    p->playerOrdinal <= 1 || p->sentAway ||
                    p->playerState != PL_NORMAL)
                    continue;
                dx = (int32_t)p->x.s.i - attacker->x.s.i;
                dy = (int32_t)p->y.s.i - attacker->y.s.i;
                d2 = dx * dx + dy * dy;
                if (d2 < closest) {
                    closest = d2;
                    candidate = p;
                }
            }
            if (candidate == player && closest > 900) {
                int32_t dx = (int32_t)attacker->x.s.i - player->destX;
                int32_t dy = (int32_t)attacker->y.s.i - player->destY;
                /* Do not drag the supporting defender more than 85 world
                 * units from the original tactical destination. */
                if (dx > 85) dx = 85;
                if (dx < -85) dx = -85;
                if (dy > 85) dy = 85;
                if (dy < -85) dy = -85;
                player->destX = SupportClamp((int32_t)player->destX + dx / 2, 81, 590);
                player->destY = SupportClamp((int32_t)player->destY + dy / 2, 129, 769);
            }
        }
    }
}
