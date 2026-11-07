/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
/* ai.c — AI helper functions
 *
 * Chunk 52: AI_SetDirectionTowardOpponentsGoal, AI_Kick,
 *           AI_DecideWhetherToTriggerFire, FindClosestPlayerToBallFacing.
 * Chunk 54: AI_SetControlsDirection.
 *
 * AI_ResumeGameDelay is left in asm (3 instructions, carry flag return).
 */

#include "ai.h"
#include "career_plus_options.h"
#include "dos_mode.h"       /* _gDosGameplay: the DOS build has no position gate here */
#include "game_types.h"
#include "math68k.h"
#include "ball_possession.h"  /* actual pass-target selection */

extern Sprite          _ballSprite;
extern int16_t         _gsResultAutoAdvance;   /* GS_660_600 600/660 — dos_gameplay.c */
extern int16_t         _gsHalftimeStoppage;    /* GS_385_350 350/385 — dos_gameplay.c */
extern TeamGeneralInfo _topTeamData;
extern TeamGeneralInfo _bottomTeamData;
extern uint16_t        _currentGameTick;
extern int16_t         _aiCounter;
extern int16_t         _aiAttackHalf;
extern int16_t         _aiCounterWriteOnly;
extern int16_t         _deadVarAlways0;
extern TeamGeneralInfo *_playerKickerPtr;

/* AI_SetControlsDirection globals — declared in SWOS2.s data section */
extern int16_t  _aiTurnDirection;
extern int16_t  _aiResumePlayTimer;
extern int16_t  _aiRand;
extern int16_t  _aiRandomRotateTable[2];
extern int16_t  _aiLeftSpinTable[3];
extern int16_t  _aiRotateRightTable[3];
extern int16_t  _aiLongKickTable[3];
extern uint8_t  _aiThrowInDirections[6];
extern int16_t  _AI_maxStoppageTime;
extern int16_t  _gameState;
extern int16_t  _gameStatePl;
extern int16_t  _resultTimer;
extern int16_t  _stoppageTimerActive;
extern int16_t  _stoppageTimerTotal;
extern int16_t  _team1Computer;
extern int16_t  _team2Computer;
extern int16_t  _playingPenalties;
extern int16_t  _penalty;
extern int16_t  _cameraDirection;
extern uint8_t  _playerTurnFlags;
extern int16_t  _frameCount;
extern int16_t  _inSubstitutesMenu;
extern TeamGeneralInfo *_lastTeamPlayedBeforeBreak;
extern TeamGeneralInfo *_lastTeamPlayed;

extern uint16_t Rand(void);

/* Written by FindClosestPlayerToBallFacing, read by asm stub to return in d2 */
int32_t __attribute__((used, externally_visible)) _findClosestBallDist;


/* ===== AI_SetDirectionTowardOpponentsGoal =====
 *
 * Sets the AI team's allowed direction toward the opposing goal based on
 * ball X position. Only acts when aiCounter is non-zero.
 *
 * Original: SWOS2.s lines 19776-19809
 */
void __attribute__((used, externally_visible))
AI_SetDirectionTowardOpponentsGoal(TeamGeneralInfo *team)
{
    int16_t ballX, dir;

    if (_aiCounter == 0)
        return;

    ballX = _ballSprite.x.s.i;

    if (_aiAttackHalf == 1) {
        /* Bottom team attacks upward: directions 7, 0, 1 */
        if (team != &_bottomTeamData)
            return;
        if (ballX < 300)
            dir = 1;
        else if (ballX > 371)
            dir = 7;
        else
            dir = 0;
    } else {
        /* Top team attacks downward: directions 3, 4, 5 */
        if (team != &_topTeamData)
            return;
        if (ballX < 300)
            dir = 3;
        else if (ballX > 371)
            dir = 5;
        else
            dir = 4;
    }

    team->currentAllowedDirection = dir;
}


/* ===== AI_Kick =====
 *
 * AI kick decision. Checks if the opponent's controlled player is facing
 * a significantly different direction from our player. If so, triggers
 * a kick (fire + direction).
 *
 * Original: SWOS2.s lines 19939-19974
 */
void __attribute__((used, externally_visible))
AI_Kick(Sprite *player, TeamGeneralInfo *team)
{
    TeamGeneralInfo *opponent;
    Sprite *opponentPlayer;
    int16_t playerDir;
    uint8_t opponentDirScaled, playerDirScaled, diff;
    int8_t sdiff;

    /* deadVarAlways0 is always 0, so this path is never taken */
    if (_deadVarAlways0 != 0) {
        if (team == (TeamGeneralInfo *)_playerKickerPtr)
            return;
        opponent = team->opponentsTeam;
        if (opponent == (TeamGeneralInfo *)_playerKickerPtr)
            return;
    }

    /* Ball must be close to player (distance <= 200) */
    if (player->ballDistance > 200)
        return;

    /* Save player's 8-way direction for later */
    playerDir = player->direction;

    /* Opponent must have the ball */
    opponent = team->opponentsTeam;
    if (opponent->playerHasBall == 0)
        return;

    /* Opponent must have a controlled player */
    opponentPlayer = opponent->controlledPlayer;
    if (opponentPlayer == (Sprite *)0)
        return;

    /* Compare directions: scale 0-7 to 0-224 range and do byte subtraction */
    opponentDirScaled = (uint8_t)(opponentPlayer->direction * 32);
    playerDirScaled = (uint8_t)(player->direction * 32);
    diff = opponentDirScaled - playerDirScaled;
    sdiff = (int8_t)diff;

    /* Kick when angle difference is outside +-32 (more than 1 direction step apart) */
    if (sdiff >= -32 && sdiff <= 32)
        return;

    /* Trigger kick */
    team->currentAllowedDirection = playerDir;
    team->firePressed = 1;
    team->fireThisFrame = 1;
    team->normalFire = 1;
}


/* ===== AI_DecideWhetherToTriggerFire =====
 *
 * Checks if the AI player should shoot. Tests facing direction vs goal,
 * proximity to ball, and ball height window.
 *
 * Returns 0 if firing (zero flag set), 1 if not firing (zero flag clear).
 *
 * Original: SWOS2.s lines 19819-19879
 */
int16_t __attribute__((used, externally_visible))
AI_DecideWhetherToTriggerFire(Sprite *player, TeamGeneralInfo *team, int16_t dir)
{
    uint16_t reverseDir, roundedDir;

    /* Goalkeeper never shoots */
    if (player->playerOrdinal == 1)
        return 1;

    /* Check if player is facing toward opponent's goal */
    if (team == &_topTeamData) {
        /* Top team: must face downward (directions 3, 4, 5) */
        if (dir != 3 && dir != 4 && dir != 5)
            return 1;
    } else {
        /* Bottom team: must face upward (directions 7, 0, 1) */
        if (dir != 7 && dir != 0 && dir != 1)
            return 1;
    }

    /* Player must be close to ball (distance <= 648 = 18^2 + 18^2) */
    if (player->ballDistance > 648)
        return 1;

    /* Check ball height based on vertical movement */
    if (_ballSprite.deltaZ.raw < 0) {
        /* Ball descending: wider height window [12..20] */
        if (_ballSprite.z.s.i < 12 || _ballSprite.z.s.i > 20)
            return 1;
    } else {
        /* Ball going up or on ground: height window [8..14] */
        if (_ballSprite.z.s.i < 8 || _ballSprite.z.s.i > 14)
            return 1;
    }

    /* Arm the shot */
    team->fireThisFrame = 1;

    /* Calculate reverse direction: add 128 to fullDirection (flip 180 degrees) */
    reverseDir = ((uint16_t)(player->fullDirection + 128)) & 0xFF;
    reverseDir >>= 5; /* convert to 0..7 range */
    team->currentAllowedDirection = (int16_t)reverseDir;

    /* Second pass: round up by adding 16 (half a step) before converting */
    roundedDir = ((uint16_t)(player->fullDirection + 128 + 16)) & 0xFF;
    roundedDir >>= 5;

    /* Must match the current 8-way direction */
    if ((int16_t)roundedDir != dir)
        return 1;

    team->currentAllowedDirection = (int16_t)roundedDir;
    _aiCounter = 15;
    _aiCounterWriteOnly = (int16_t)roundedDir;
    _aiAttackHalf = 2;

    if (team != &_topTeamData)
        _aiAttackHalf = 1;

    return 0; /* firing */
}


/* ===== FindClosestPlayerToBallFacing =====
 *
 * Finds the closest player (from both teams) who is:
 * - not the controlled player
 * - not sent away
 * - in normal state
 * - facing within +-16 of the target direction
 * - closest to the ball
 *
 * Returns the player pointer (or (Sprite*)-1 if none found).
 * Ball distance is stored in findClosestBallDist for the asm stub.
 *
 * Original: SWOS2.s lines 19986-20021
 */
Sprite * __attribute__((used, externally_visible))
FindClosestPlayerToBallFacing(uint16_t fullDir, TeamGeneralInfo *team)
{
    Sprite *bestPlayer = (Sprite *)0xFFFFFFFF;
    int32_t bestDist = -1; /* 0xFFFFFFFF as unsigned = max value */
    Sprite **sprites;
    int16_t i, t;
    TeamGeneralInfo *scanTeam;

    scanTeam = &_topTeamData;

    for (t = 0; t < 2; t++) {
        sprites = scanTeam->spritesTable;

        for (i = 0; i < 11; i++) {
            Sprite *pl = sprites[i];
            int8_t dirDiff;

            /* Skip controlled player */
            if (pl == team->controlledPlayer)
                goto next;

            /* Skip sent-away players */
            if (pl->sentAway != 0)
                goto next;

            /* Skip non-normal players */
            if (pl->playerState != PL_NORMAL)
                goto next;

            /* Check direction: must be within +-16 of target */
            dirDiff = (int8_t)((uint8_t)(pl->fullDirection) - (uint8_t)fullDir);
            if (dirDiff < -16 || dirDiff > 16)
                goto next;

            /* Check if closer to ball than current best */
            if ((uint32_t)pl->ballDistance >= (uint32_t)bestDist)
                goto next;

            bestDist = pl->ballDistance;
            bestPlayer = pl;

        next:
            ;
        }

        scanTeam = &_bottomTeamData;
    }

    _findClosestBallDist = bestDist;
    return bestPlayer;
}


/* ===== Career+ advanced AI (experimental) =====
 * Changes decisions only. DoPass and the standard ball/shot engines remain
 * responsible for execution, skill-based misses, trajectory and interception.
 * Never run for a human side or while taking a restart.
 */
static int32_t AI_DistanceSq(int32_t dx, int32_t dy)
{
    return dx * dx + dy * dy;
}

static int32_t AI_OpponentDistanceSq(TeamGeneralInfo *team, int16_t x,
                                      int16_t y)
{
    int32_t nearest = 0x7FFFFFFF;
    Sprite **players = team->opponentsTeam->spritesTable;
    int i;
    if (!players)
        return nearest;
    for (i = 0; i < 11; ++i) {
        Sprite *opp = players[i];
        int32_t d;
        if (!opp || opp->sentAway || opp->playerState != PL_NORMAL)
            continue;
        d = AI_DistanceSq((int32_t)opp->x.s.i - x,
                          (int32_t)opp->y.s.i - y);
        if (d < nearest)
            nearest = d;
    }
    return nearest;
}

/* Discourage a pass that crosses an opponent near the middle of its path.
 * Integer projection avoids floating point in the match loop. */
static int AI_LaneBlocked(TeamGeneralInfo *team, Sprite *from, Sprite *to)
{
    int32_t vx = (int32_t)to->x.s.i - from->x.s.i;
    int32_t vy = (int32_t)to->y.s.i - from->y.s.i;
    int32_t len2 = AI_DistanceSq(vx, vy);
    Sprite **opps = team->opponentsTeam->spritesTable;
    int i;
    if (!opps || len2 < 1)
        return 0;
    for (i = 0; i < 11; ++i) {
        Sprite *opp = opps[i];
        int32_t ox, oy, along, cross;
        if (!opp || opp->sentAway || opp->playerState != PL_NORMAL)
            continue;
        ox = (int32_t)opp->x.s.i - from->x.s.i;
        oy = (int32_t)opp->y.s.i - from->y.s.i;
        along = ox * vx + oy * vy;
        if (along <= len2 / 10 || along >= (len2 * 9) / 10)
            continue;
        cross = ox * vy - oy * vx;
        /* 20 world units is a conservative interception corridor. */
        if ((int64_t)cross * cross < (int64_t)len2 * 400)
            return 1;
    }
    return 0;
}

/* A target is useful only if the actual SWOS pass resolver selects it.
 * This avoids "smart" directional passes going to an unexpected player. */
/* Evaluate the eight ACTUAL short-pass directions, rather than guessing a
 * direction from the receiver's facing angle. SWOS uses the receiver's
 * facing to select a target, so we must score precisely the player that
 * GetClosestNonControlledPlayerInDirection() will choose. */
static Sprite *AI_BestSafePass(TeamGeneralInfo *team, Sprite *from,
                               int16_t *passDir, int32_t *passScore,
                               int preferLong)
{
    Sprite *best = 0;
    int32_t bestScore = -1000000;
    int16_t forward = team == &_topTeamData ? 1 : -1;
    int dir;
    if (!team->spritesTable || !team->opponentsTeam)
        return 0;
    for (dir = 0; dir < 8; ++dir) {
        Sprite *pl = GetClosestNonControlledPlayerInDirection((uint16_t)dir, team);
        int32_t dx, dy, range, progress, nearby, score;
        int32_t ownGoalDist, edge, carrierOwnGoalDist, ballEdge;
        int32_t backAmount;
        if (!pl || pl == (Sprite *)0xFFFFFFFF || pl == from ||
            pl->playerOrdinal == 1 || pl->sentAway ||
            pl->playerState != PL_NORMAL)
            continue;
        dx = (int32_t)pl->x.s.i - from->x.s.i;
        dy = (int32_t)pl->y.s.i - from->y.s.i;
        range = AI_DistanceSq(dx, dy);
        if (range < 900 || range > 160000)
            continue;
        progress = forward * dy;
        backAmount = progress < 0 ? -progress : 0;
        carrierOwnGoalDist = forward > 0 ? from->y.s.i - 129 : 769 - from->y.s.i;
        nearby = AI_OpponentDistanceSq(team, pl->x.s.i, pl->y.s.i);
        if (nearby < 900) /* avoid receivers under very close marking */
            continue;
        /* The standard pass flies beyond the receiver rather than stopping
         * on their feet. Keep a wide reception corridor, especially when a
         * long pass would continue towards a touchline. */
        edge = pl->x.s.i < 336 ? pl->x.s.i - 81 : 590 - pl->x.s.i;
        ballEdge = from->x.s.i < 336 ? from->x.s.i - 81 : 590 - from->x.s.i;
        if (edge < 75 || pl->y.s.i < 193 || pl->y.s.i > 705)
            continue;
        if (edge < 112 && (range > 10000 || edge < ballEdge))
            continue;
        /* The angle of a pass leaving the narrow side of the pitch is
         * dangerous even with an accurate player. Avoid the outward vector. */
        if ((from->x.s.i < 175 && dx < -8) ||
            (from->x.s.i > 496 && dx > 8) ||
            (pl->x.s.i < 195 && dx < -25) ||
            (pl->x.s.i > 476 && dx > 25))
            continue;

        ownGoalDist = forward > 0 ? pl->y.s.i - 129 : 769 - pl->y.s.i;
        /* In the defensive half, avoid speculative backward passing.
         * Small backwards resets are allowed when safely clear of the goal,
         * but no retreat into the goalmouth or a deep corner. */
        if (ownGoalDist < 160 && progress < 24)
            continue;
        if (carrierOwnGoalDist < 340 && backAmount > 15)
            continue;
        if (carrierOwnGoalDist < 435 && backAmount > 65)
            continue;
        /* v8: a backward pass is a deliberate safe reset, never a routine
         * way of escaping the defensive half. Receiver needs generous room,
         * a central lane and a short retreat; keep the wing restrictions above
         * exactly as they were in v7. */
        if (backAmount > 0 &&
            (carrierOwnGoalDist < 450 || ownGoalDist < 325 ||
             backAmount > 55 || nearby < 6400 || edge < 125))
            continue;
        if (AI_LaneBlocked(team, from, pl))
            continue;
        /* A receiver who can turn forward in space beats a marked square
         * pass. Still favour open long passes on some possession windows. */
        score = progress * 3 - range / 230;
        if (nearby > 6400) score += 105;
        else if (nearby > 2500) score += 55;
        if (edge < 100) score -= (100 - edge) * 3;
        /* Prefer progression, yet allow a safe sideways reset or a small
         * backpass in midfield when no sensible forward outlet exists. */
        if (backAmount > 0) score -= 155 + backAmount * 5;
        if (progress > 20) score += 35;
        if (ownGoalDist < 225 && progress < 20) score -= 140;
        if (range > 22500) {
            if (preferLong && progress > 45 && nearby > 2500)
                score += 190 + (range > 62500 ? 55 : 0);
            else
                score -= 25;
        }
        if (range > 90000 && !preferLong)
            score -= 45;
        if (score > bestScore) {
            best = pl;
            bestScore = score;
            *passDir = (int16_t)dir;
        }
    }
    *passScore = bestScore;
    return best;
}

/* Called only for a team in open play with a playable ball nearby.
 * Returns 1 if we deliberately selected a pass or carry direction. */
static int AI_AdvancedPossession(TeamGeneralInfo *team, Sprite *carrier,
                                 int32_t goalDistSq)
{
    Sprite *target;
    int16_t dir = -1;
    int32_t score = 0, pressure;
    int16_t forward = team == &_topTeamData ? 1 : -1;
    int16_t bx, by;
    int32_t leftSpace, rightSpace;
    int32_t attackingLineDist;
    int16_t carryDir;

    if (team->playerNumber != 0 || _gameStatePl != ST_GAME_IN_PROGRESS ||
        _playingPenalties || _penalty || !carrier ||
        carrier->playerOrdinal == 1 || carrier->playerState != PL_NORMAL ||
        carrier->ballDistance > 650 || team->passKickTimer >= 13 ||
        _aiResumePlayTimer != 0 || team->spinTimer >= 0)
        return 0;

    pressure = AI_OpponentDistanceSq(team, carrier->x.s.i, carrier->y.s.i);

    /* v16: Near the attacking byline, a wide ball carrier must not keep
     * advancing straight into touch or over the goal line. The original AI
     * only treated the touchline, and its inward diagonal could still run
     * out over the byline. This is an open-play, wide-player rule only;
     * preserve central shots, real set pieces and pressured clearances. */
    attackingLineDist = forward > 0 ? 769 - carrier->y.s.i :
                                       carrier->y.s.i - 129;
    if (attackingLineDist < 125 && attackingLineDist > -12 &&
        (carrier->x.s.i < 205 || carrier->x.s.i > 467) &&
        pressure > 625) {
        int16_t safeDir = -1;
        int32_t safeScore = 0;
        Sprite *safeReceiver = AI_BestSafePass(team, carrier,
                                               &safeDir, &safeScore, 0);
        if (safeReceiver && safeScore > 0) {
            team->currentAllowedDirection = safeDir;
            team->quickFire = 1;
            _aiResumePlayTimer = 13;
            return 1;
        }
        /* A forward diagonal is safe further from the line, but in the
         * final 80 units we must run parallel to the goal line instead. */
        if (attackingLineDist <= 80)
            carryDir = carrier->x.s.i < 336 ? 2 : 6;
        else
            carryDir = carrier->x.s.i < 336 ?
                (forward > 0 ? 3 : 1) : (forward > 0 ? 5 : 7);
        team->currentAllowedDirection = carryDir;
        return 1;
    }

    /* Do not interfere with genuine attempts near the centre of the goal. */
    if (goalDistSq < 22500)
        return 0;
    /* Eleven of 32 possession windows favour a purposeful longer
     * forward pass, provided a real and reasonably open teammate exists. */
    target = AI_BestSafePass(team, carrier, &dir, &score,
                             (team->AI_timer & 31) < 11 && pressure > 1200);

    /* Close to the touchline, prefer an inward pass or inward movement.
     * Never override a genuine emergency clearance when heavily pressed. */
    if (carrier->x.s.i < 125 || carrier->x.s.i > 546) {
        if (pressure > 625) {
            if (target && _aiResumePlayTimer == 0) {
                team->currentAllowedDirection = dir;
                team->quickFire = 1;
                _aiResumePlayTimer = 13;
                return 1;
            }
            /* SWOS directions: 0=N, 2=E, 4=S, 6=W.
             * A diagonal carries the ball towards both goal and pitch centre. */
            team->currentAllowedDirection = carrier->x.s.i < 125 ?
                (forward > 0 ? 3 : 1) : (forward > 0 ? 5 : 7);
            return 1;
        }
    }

    /* Pressure makes passing more urgent. In open space, retain possession
     * instead of spamming one-touch passes. A deliberately modest random
     * element preserves classic mistakes and different match rhythms. */
    if (target && (pressure < 3600 ||
                   (score > 260 && (team->AI_timer & 3) == 0) ||
                   (score > 90 && (team->AI_timer & 7) == 0) ||
                   (score > 0 && (team->AI_timer & 15) == 0))) {
        team->currentAllowedDirection = dir;
        team->quickFire = 1;
        _aiResumePlayTimer = 13;
        return 1;
    }

    /* If no safe pass is available and the AI has room, do not return to
     * SWOS's old speculative directional kick. Carry the ball into an open
     * channel, unless a defender is so close that an emergency clearance
     * is genuinely justified. */
    if (pressure > 900 && goalDistSq > 42000) {
        bx = carrier->x.s.i;
        by = carrier->y.s.i;
        leftSpace = AI_OpponentDistanceSq(team, bx - 35, by + forward * 42);
        rightSpace = AI_OpponentDistanceSq(team, bx + 35, by + forward * 42);
        carryDir = forward > 0 ? 4 : 0;
        if (leftSpace > rightSpace + 1200 && bx > 120)
            carryDir = forward > 0 ? 3 : 7;
        else if (rightSpace > leftSpace + 1200 && bx < 550)
            carryDir = forward > 0 ? 5 : 1;
        team->currentAllowedDirection = carryDir;
        return 1;
    }
    /* Conservative defensive fallback: if there is no reasonable receiver
     * and we are not under pressing pressure, hold the ball and move into
     * space instead of invoking an unassessed aimless pass. */
    /* v15: a non-pressured carrier must never return to the original
     * speculative pass/kick branch merely because a good pass was not
     * available (or was deferred this tick). Carry instead. Defenders
     * immediately under pressure and attackers in shooting range retain
     * the original clearance/shooting decisions. */
    if (pressure > 625 && goalDistSq > 22500) {
        team->currentAllowedDirection = carrier->x.s.i < 155 ?
            (forward > 0 ? 3 : 1) :
            (carrier->x.s.i > 516 ? (forward > 0 ? 5 : 7) :
             (forward > 0 ? 4 : 0));
        return 1;
    }
    return 0;
}

/* Prefer a short, controlled restart after a goal or at half-time.
 * Ordinary set pieces are deliberately unaffected.  SWOS's DoPass resolver
 * selects a teammate by facing direction, not just the geometric direction
 * of a pass: check the actual resolved teammate before choosing a direction.
 */
static int AI_ControlledKickoff(TeamGeneralInfo *team, Sprite *kicker)
{
    int16_t bestDir = -1;
    int32_t bestScore = -0x7FFFFFFF;
    int dir;
    if (!kicker || kicker->playerOrdinal == 1 ||
        !team->spritesTable || kicker->ballDistance > 650 ||
        (!team->plCloseToBall && !team->plVeryCloseToBall))
        return 0;
    for (dir = 0; dir < 8; dir++) {
        Sprite *pl;
        int32_t dx, dy, d2, score;
        if (!(_playerTurnFlags & (1 << dir)))
            continue;
        pl = GetClosestNonControlledPlayerInDirection((uint16_t)dir, team);
        if (!pl || pl == (Sprite *)0xFFFFFFFF || pl == kicker ||
            pl->playerOrdinal == 1 || pl->sentAway ||
            pl->playerState != PL_NORMAL)
            continue;
        dx = (int32_t)pl->x.s.i - kicker->x.s.i;
        dy = (int32_t)pl->y.s.i - kicker->y.s.i;
        d2 = AI_DistanceSq(dx, dy);
        if (d2 < 225 || d2 > 62500 || pl->x.s.i < 101 || pl->x.s.i > 570)
            continue;
        /* Favour midfield receivers and lateral/backward starts. */
        score = 250 - d2 / 120;
        if (dir == 2 || dir == 6) score += 110;
        if (dir == 1 || dir == 3 || dir == 5 || dir == 7) score += 35;
        if (AI_OpponentDistanceSq(team, pl->x.s.i, pl->y.s.i) < 900)
            score -= 200;
        if (score > bestScore) {
            bestScore = score;
            bestDir = (int16_t)dir;
        }
    }
    if (bestDir < 0) {
        /* Do not hold up the match indefinitely if all squad members are
         * out of position. A sideways ground pass is safer than the old
         * long shot from the centre circle. */
        bestDir = (_playerTurnFlags & (1 << 2)) ? 2 :
                  (_playerTurnFlags & (1 << 6)) ? 6 : -1;
    }
    if (bestDir < 0)
        return 0;
    team->currentAllowedDirection = bestDir;
    team->quickFire = 1;
    return 1;
}

/* ===== AI_SetControlsDirection =====
 *
 * Main AI controller for computer-controlled teams.
 * Called from UpdatePlayers once per frame for AI teams.
 *
 * Decides direction, fire buttons, aftertouch and spin for AI.
 * Handles all game states: in-progress, set pieces, penalties,
 * result display, etc.
 *
 * Original: SWOS2.s lines 17832-18668 (~836 lines of asm)
 *
 * Register mapping:
 *   a6 = team, a5 = controlledPlayer, a4 = passToPlayer
 *   d7 = controlled player direction (or -1)
 *   d6 = squared distance ball-to-goal-center
 *   d5 = angle ball-to-goal-center (0..255)
 *   d2 = direction delta (byte arithmetic)
 */
void __attribute__((used, externally_visible))
AI_SetControlsDirection(TeamGeneralInfo *team)
{
    Sprite *a5;             /* controlled player */
    Sprite *a4;             /* pass-to player */
    /* a2 arrives holding the BALL. The asm never initialises it — it is the
     * caller's register, and `UpdatePlayers` calls this with a2 = ballSprite
     * (SWOS2.s:30123, `movem.l a1-a3,-(sp)` around the bsr because the routine
     * clobbers them). Only `field84_cooldown_expired_path` overwrites it, with
     * the opponent's controlled player or pass target. Starting it at NULL and
     * guarding the reads made the ST_FOUL spin test below silently skip:
     * zkz-10174427 frame 13248 is a free kick with the ball at y=693, the gate
     * is `>= 682`, and the recording's spin is 0 where ours was 1. */
    Sprite *a2 = &_ballSprite;
    Sprite *closestPlayer;
    int16_t d7;             /* controlled player direction, or -1 */
    int32_t d6;             /* squared distance ball to goal center */
    int16_t d5;             /* angle ball to goal center (byte range 0..255) */
    int16_t goalY;          /* opponent's goal line Y */
    int16_t ballX, ballY;
    int32_t dx32, dy32;
    int32_t outDeltaX, outDeltaY;
    int16_t direction;
    int16_t d0, d1, d2;
    int8_t d2_byte;
    int16_t wVar;

    /* Early exit if controls are reset */
    if (team->resetControls != 0)
        return;

    /* Decrement AI counters */
    if (_aiCounter != 0)
        _aiCounter--;
    if (_aiResumePlayTimer != 0)
        _aiResumePlayTimer--;

    /* Generate random value for this frame */
    _aiRand = (int16_t)Rand();

    /* Increment AI timer, clear all control outputs */
    team->AI_timer++;
    team->currentAllowedDirection = -1;
    team->firePressed = 0;
    team->fireThisFrame = 0;
    team->quickFire = 0;
    team->normalFire = 0;

    /* Don't process AI during substitutes menu */
    if (_inSubstitutesMenu != 0)
        return;

    /* Load controlled player and pass target */
    d7 = -1;
    a5 = team->controlledPlayer;
    a4 = team->passToPlayerPtr;
    if (a5 != (Sprite *)0)
        d7 = a5->direction;

    /* Compute goal coordinates and squared distance */
    if (team == &_bottomTeamData)
        goalY = 129;    /* upper goal line (bottom team attacks up) */
    else
        goalY = 769;    /* lower goal line (top team attacks down) */

    ballX = _ballSprite.x.s.i;
    ballY = _ballSprite.y.s.i;
    dx32 = (int32_t)(int16_t)(ballX - 336);
    dy32 = (int32_t)(int16_t)(ballY - goalY);
    d6 = dx32 * dx32 + dy32 * dy32;

    /* Calculate angle from ball toward center of opponent's goal */
    direction = (int16_t)CalculateDeltaXAndY(
        336, goalY, ballX, ballY, 256, &outDeltaX, &outDeltaY);
    if (direction < 0)
        direction = 0;
    d5 = direction;

    /* ---- Game NOT in progress ---- */
    if (_gameStatePl != ST_GAME_IN_PROGRESS) {

        /* Only the team that last played gets AI control */
        if (team != _lastTeamPlayedBeforeBreak)
            return;

        /* Check if showing result screen (both teams computer) */
        if (_gameState >= ST_STARTING_GAME && _gameState <= ST_GAME_ENDED &&
            _team1Computer != 0 && _team2Computer != 0) {

            if (_gameState == ST_RESULT_ON_HALFTIME ||
                _gameState == ST_RESULT_AFTER_THE_GAME) {
                if (_stoppageTimerTotal < _gsResultAutoAdvance)   /* GS_660_600 */
                    return;
            } else {
                /* 350 ticks for other result states */
                if (_stoppageTimerTotal < _gsHalftimeStoppage)   /* GS_385_350 */
                    return;
            }
            /* Simulate fire press to dismiss result */
            team->firePressed = 1;
            return;
        }

        /* game_not_over: check stoppage timer threshold */
        d0 = (_currentGameTick & 0x3F) + 100;
        if (_stoppageTimerActive < (uint16_t)d0)
            goto set_direction;

        /* Kick-offs are restarts, not open-play shots. Never use the
         * original centre-circle long-shot logic for CPU sides. */
        if (_careerImprovedAiOption && _gameState == ST_PLAYERS_TO_INITIAL_POSITIONS &&
            team->playerNumber == 0 && team == _lastTeamPlayedBeforeBreak &&
            AI_ControlledKickoff(team, a5))
            return;

        /* Check penalties */
        if (_playingPenalties != 0 || _gameState == ST_PENALTY)
            goto penalties_after_the_game;

        /* Keeper holds ball / goal out */
        if (_gameState == ST_KEEPER_HOLDS_BALL ||
            _gameState == ST_GOAL_OUT_LEFT ||
            _gameState == ST_GOAL_OUT_RIGHT) {
            /* keepers_ball */
            if ((_aiRand & 1) != 0) {
                if ((uint16_t)d7 == (uint16_t)_cameraDirection)
                    goto apply_turn_delta_from_angle;
                if (_ballSprite.x.s.i <= 336) {
                    if (d7 == 1 || d7 == 3)
                        goto apply_turn_delta_from_angle;
                } else {
                    if (d7 == 5 || d7 == 7)
                        goto apply_turn_delta_from_angle;
                }
            }
            goto check_closest_player_facing;
        }

        /* Players to initial positions (goal scored) */
        if (_gameState == ST_PLAYERS_TO_INITIAL_POSITIONS) {
            /* goal_scored */
            if (_stoppageTimerActive < 150)
                goto check_closest_player_facing;
            if ((uint16_t)d7 == (uint16_t)_cameraDirection)
                goto apply_turn_delta_from_angle;
            goto check_closest_player_facing;
        }

        /* Free kicks */
        if (_gameState >= ST_FREE_KICK_LEFT1 && _gameState <= ST_FREE_KICK_RIGHT3)
            goto foul;

        /* Throw-ins */
        if (_gameState >= ST_THROW_IN_FORWARD_RIGHT &&
            _gameState <= ST_THROW_IN_BACK_LEFT) {
            uint8_t throwBits;
            d1 = _gameState - ST_THROW_IN_FORWARD_RIGHT;
            throwBits = _aiThrowInDirections[d1];
            if (team != &_bottomTeamData) {
                /* ror.b #4,d1 → swap nibbles */
                throwBits = (uint8_t)((throwBits << 4) | (throwBits >> 4));
            }
            goto decide_throw_in_direction_bits_entry;

            decide_throw_in_direction_bits_entry:
            if ((_aiRand & 0xF) != 0)
                goto check_closest_player_facing;
            if (d7 >= 0 && d7 <= 7 && (throwBits & (1 << (d7 & 7))) != 0)
                goto apply_turn_delta_from_angle;
            goto check_closest_player_facing;
        }

        /* Foul */
        if (_gameState == ST_FOUL)
            goto foul;

        /* Default: apply turn from angle */
        goto apply_turn_delta_from_angle;
    }

    /* ---- Game in progress ---- */

    /* Penalty check */
    if (_playingPenalties != 0 || _penalty != 0) {
        if (team->spinTimer >= 0)
            goto we_have_ball_spin;
    }

    /* AI counter direction toward goal */
    if (_aiCounter != 0)
        AI_SetDirectionTowardOpponentsGoal(team);

    /* Need a controlled player */
    if (a5 == (Sprite *)0)
        return;

    /* Check if we're in spin mode */
    if (team->spinTimer >= 0)
        goto we_have_ball_spin;

    /* Career+ defensive pressure: when facing a human ball carrier, the
     * selected computer defender should keep closing down rather than
     * turning away under the original random-direction fallback. Leave
     * close-range tackling, ball contact, and every restart to SWOS. */
    if (_careerImprovedAiOption && team->playerNumber == 0 && !team->playerHasBall &&
        team->opponentsTeam && team->opponentsTeam->playerNumber != 0 &&
        team->opponentsTeam->playerHasBall &&
        a5->playerState == PL_NORMAL && !a5->sentAway &&
        a5->ballDistance > 750 && a5->ballDistance < 19600 &&
        (_currentGameTick & 3) != 0) {
        Sprite *attacker = team->opponentsTeam->controlledPlayer;
        if (attacker && attacker->playerState == PL_NORMAL &&
            !attacker->sentAway) {
            int32_t ax = (int32_t)attacker->x.s.i - a5->x.s.i;
            int32_t ay = (int32_t)attacker->y.s.i - a5->y.s.i;
            if (ax * ax + ay * ay < 19600) {
                int16_t dir = (int16_t)CalculateDeltaXAndY(
                    attacker->x.s.i, attacker->y.s.i,
                    a5->x.s.i, a5->y.s.i, 256,
                    &outDeltaX, &outDeltaY);
                if (dir >= 0) {
                    team->currentAllowedDirection =
                        (int16_t)(((uint16_t)(dir + 16)) & 0xFF) >> 5;
                    return;
                }
            }
        }
    }

    /* Check if a player is near the ball */
    if (team->plVeryCloseToBall != 0 || team->plCloseToBall != 0)
        goto theres_a_player_near;

    /* noone_near: */
    goto noone_near;

    /* ---- theres_a_player_near ---- */
theres_a_player_near:
    if (AI_DecideWhetherToTriggerFire(a5, team, d7) == 0)
        return;  /* firing */
    /* Career+ advanced possession decisions for CPU-controlled teams.
     * Preserve original shots, clearances, marking and set pieces. */
    if (_careerImprovedAiOption && AI_AdvancedPossession(team, a5, d6))
        return;

    /* deadVarAlways0 check (always 0 in practice) */
    if (_deadVarAlways0 != 0) {
        if (team->opponentsTeam == (TeamGeneralInfo *)_playerKickerPtr) {
            if (team->opponentsTeam->playerHasBall != 0)
                return;
        }
    }

    /* check_direction_and_field_half_gates
     *
     * A player facing sideways does NOT hit it while the ball is deep in the
     * attacking third — there the fallback flow turns him toward goal instead.
     * SWOS2.s:32052 `cmpi.w #158,ballY / ble no_fire` for the bottom team
     * (which attacks the y=129 line) and :32057 `cmpi.w #740,ballY / bge
     * no_fire` for the top (y=769). Both branch targets are the REJECT label,
     * so the C test carries the same sense as the branch, not its opposite. */
    if (a5->direction == 2 || a5->direction == 6) {
        /* horizontal_dir_half_gate */
        if (team == &_topTeamData) {
            if (_ballSprite.y.s.i >= 740)
                goto no_fire_fallback_flow;
        } else {
            if (_ballSprite.y.s.i <= 158)
                goto no_fire_fallback_flow;
        }
        goto distance_and_random_fire_gate;
    }

distance_and_random_fire_gate:
    if (d6 > 28800)
        goto no_fire_fallback_flow;
    if (d6 >= 12800) {
        if ((_aiRand & 3) != 0)
            goto no_fire_fallback_flow;
    }

    /* set_angle_tolerance_by_distance */
    {
        int8_t tolerance = 0x0F;
        if (d6 <= 3200)
            tolerance = 0x32;

        /* compare_angle_to_tolerance */
        d2 = a5->direction;
        if (d2 < 0)
            goto no_fire_fallback_flow;
        d2_byte = (int8_t)((uint8_t)(d2 << 5) - (uint8_t)d5);
        /* Fire iff -tolerance < d2_byte <= tolerance: the asm's second test is
         * `neg.b d1 / cmp.b d1,d2 / bgt fire` (SWOS2.s:32081), so d2_byte
         * exactly ON the negative bound falls through to no-fire. */
        if (d2_byte > tolerance || d2_byte <= -tolerance)
            goto no_fire_fallback_flow;
    }

    /* angle_within_tolerance_action */
    if (_aiResumePlayTimer != 0)
        return;
    if (team->passKickTimer >= 13)  /* AI_ResumeGameDelay: bcc = passKickTimer >= 13 */
        return;
    team->AI_afterTouchStrength = 0;
    goto resume_play_and_apply_turn_bias;

no_fire_fallback_flow:
    if (team->field_84 != 0) {
        team->field_84--;
        closestPlayer = FindClosestPlayerToBallFacing(
            (uint16_t)(d7 << 5), team);
        if (closestPlayer != (Sprite *)0xFFFFFFFF &&
            a5->teamNumber == closestPlayer->teamNumber)
            goto our_player_closest;
        goto closest_facing_player_not_ours;
    }

    /* field84_cooldown_expired_path */
    if (d6 < 9800)
        goto choose_dir_when_not_very_close;
    if (a5 == (Sprite *)0)
        goto choose_dir_when_not_very_close;

    /* Load opponent's controlled player into a2 */
    {
        TeamGeneralInfo *opponentTeam = team->opponentsTeam;
        a2 = opponentTeam->controlledPlayer;
        if (a2 == (Sprite *)0)
            goto use_pass_target_if_valid;
        if (a2->ballDistance < 800)
            goto evaluate_closest_facing_player_midrange;
        if (a2->ballDistance < 5000)
            goto far_distance_decision_gate;
    }

use_pass_target_if_valid:
    {
        TeamGeneralInfo *opponentTeam = team->opponentsTeam;
        Sprite *passTarget = opponentTeam->passToPlayerPtr;
        if (passTarget == (Sprite *)0)
            goto choose_dir_when_not_very_close;
        if (passTarget->ballDistance < 800)
            goto evaluate_closest_facing_player_midrange;
        if (passTarget->ballDistance < 5000)
            goto far_distance_decision_gate;
        goto choose_dir_when_not_very_close;
    }

evaluate_closest_facing_player_midrange:
    if (d6 > 180000)
        goto decide_immediate_turn_or_gate;
    closestPlayer = FindClosestPlayerToBallFacing(
        (uint16_t)(d7 << 5), team);
    if (closestPlayer == (Sprite *)0xFFFFFFFF)
        goto gate_field84_cooldown_and_set;
    if (a5->teamNumber == closestPlayer->teamNumber)
        goto our_player_closest;
    goto gate_field84_cooldown_and_set;

far_distance_decision_gate:
    if (_aiRand <= 8) {
        if (d6 > 48400)
            goto decide_immediate_turn_or_gate;
    }
    /* tick_gated_closest_facing_check */
    if ((_currentGameTick & 0xC) != 0)
        goto choose_dir_when_not_very_close;
    closestPlayer = FindClosestPlayerToBallFacing(
        (uint16_t)(d7 << 5), team);
    if (closestPlayer == (Sprite *)0xFFFFFFFF)
        goto gate_field84_cooldown_and_set;
    if (a5->teamNumber == closestPlayer->teamNumber)
        goto our_player_closest;
    /* bra.w *+4 → fall through */

gate_field84_cooldown_and_set:
    if (team->field_84 != 0)
        goto choose_dir_when_not_very_close;
    if (((uint16_t)_frameCount & 0x7F) >= 0x20)
        goto choose_dir_when_not_very_close;
    team->field_84 = 4;

closest_facing_player_not_ours:
    if (team->plVeryCloseToBall == 0)
        goto copy_current_direction_as_allowed;
    if ((uint8_t)(_currentGameTick) & 0x80) {
        /* CW turn */
        d0 = (a5->direction + 1) & 7;
    } else {
        /* CCW turn */
        d0 = (a5->direction - 1) & 7;
    }
    team->currentAllowedDirection = d0;
    return;

choose_dir_when_not_very_close:
    if (team->plVeryCloseToBall == 0)
        goto copy_current_direction_as_allowed;
    /* set_allowed_dir_from_angle_bucket */
    goto set_allowed_dir_from_angle_bucket;

set_allowed_dir_from_angle_bucket:
    team->currentAllowedDirection = (int16_t)(((uint16_t)(d5 + 16)) & 0xFF) >> 5;
    return;

copy_current_direction_as_allowed:
    team->currentAllowedDirection = a5->direction;
    return;

derive_allowed_dir_from_full_angle:
    {
        uint16_t computedDir;
        computedDir = (uint16_t)(((uint8_t)(a5->fullDirection + 128) + 16) & 0xFF) >> 5;
        if (computedDir != (uint16_t)a5->direction &&
            a5->ballDistance >= 800 &&
            (_currentGameTick & 0xE) != 0) {
            team->currentAllowedDirection = a5->direction;
            return;
        }
        team->currentAllowedDirection = (int16_t)computedDir;
        return;
    }

our_player_closest:
    team->field_84 = 0;
    if (_aiResumePlayTimer != 0)
        return;
    if (team->passKickTimer >= 13)  /* AI_ResumeGameDelay */
        return;
    _aiResumePlayTimer = 15;
    team->currentAllowedDirection = d7;
    team->quickFire = 1;
    if (_AI_maxStoppageTime <= _stoppageTimerActive)
        _AI_maxStoppageTime = _stoppageTimerActive;
    return;

decide_immediate_turn_or_gate:
    {
        int16_t angleBucket = (int16_t)(((uint16_t)(d5 + 16)) & 0xFF) >> 5;
        int16_t diff = (angleBucket - d7) & 7;
        /* The asm tests 0, 1 and $FFFF (SWOS2.s:32243-32248) — but the compare
         * sits after `andi.w #7,d0`, so the -1 arm can never be taken and only
         * a bucket AT or one step CLOCKWISE of the facing gets the shot. What
         * is written is not what executes; reading the third test as `diff == 7`
         * added a counter-clockwise arm the original never had. */
        if (diff == 0 || diff == 1)
            goto setup_resume_play_and_turn_bias;
        goto gate_field84_cooldown_and_set;
    }

setup_resume_play_and_turn_bias:
    /* Avoid direction-only long kicks in open space. If there is an actual
     * pass outlet use it; otherwise advance with the ball. Do not interfere
     * with pressured defensive clearances or shooting-range decisions. */
    if (_careerImprovedAiOption && team->playerNumber == 0 && _gameStatePl == ST_GAME_IN_PROGRESS &&
        a5 && a5->playerOrdinal != 1 && a5->ballDistance <= 650 &&
        d6 > 60000 &&
        AI_OpponentDistanceSq(team, a5->x.s.i, a5->y.s.i) > 3600) {
        int16_t safeDir;
        int32_t safeScore;
        Sprite *safeTarget = AI_BestSafePass(team, a5, &safeDir, &safeScore,
                                               (team->AI_timer & 15) < 5);
        if (safeTarget && safeScore > -25 && _aiResumePlayTimer == 0) {
            team->currentAllowedDirection = safeDir;
            team->quickFire = 1;
            _aiResumePlayTimer = 13;
        } else {
            team->currentAllowedDirection = d7;
        }
        return;
    }
    team->AI_afterTouchStrength = 2;
    if (d6 <= 115600)
        team->AI_afterTouchStrength = 1;

    team->currentAllowedDirection = d7;
    team->normalFire = 1;
    d2_byte = (int8_t)((uint8_t)(d7 << 5) - (uint8_t)d5);
    _aiResumePlayTimer = 15;
    d0 = -1;
    if (d2_byte < 0)
        d0 = 1;

    /* adjust_turn_by_ball_motion */
    if (_ballSprite.deltaY.raw < 0) {
        if (_ballSprite.y.s.i < 342)
            goto commit_turn_to_ai_state;
    } else {
        if (_ballSprite.y.s.i > 555)
            goto commit_turn_to_ai_state;
    }

    /* time_bucket_and_ballx_branch */
    {
        int16_t timeBucket = (_currentGameTick & 0x1C) >> 2;
        int16_t bx = _ballSprite.x.s.i;

        /* `cmpi.w #478,ballX / bcs mid_right_x_gate` (SWOS2.s:32277) is an
         * unsigned LOWER test, so x == 478 belongs to the midfield band. */
        if (bx >= 193 && bx < 478) {
            /* mid_right_x_gate */
            if (timeBucket == 0)
                goto set_zero_turn;
            if (timeBucket < 5)
                goto commit_turn_to_ai_state;
            goto invert_turn;
        }
        if (bx >= 118 && bx <= 553) {
            /* midfield_x_gate */
            if (timeBucket == 0)
                goto invert_turn;
            if (timeBucket < 4)
                goto set_zero_turn;
            goto commit_turn_to_ai_state;
        }
        /* outerwide_x_gate */
        if (timeBucket < 4)
            goto commit_turn_to_ai_state;
    }

set_zero_turn:
    d0 = 0;
    goto commit_turn_to_ai_state;

invert_turn:
    d0 = -d0;

commit_turn_to_ai_state:
    team->AI_ballSpinDirection = d0;
    return;

    /* ---- Penalty path (game not in progress) ---- */
penalties_after_the_game:
    {
        int16_t randDir = _aiRand & 7;
        if (_playerTurnFlags & (1 << randDir)) {
            team->currentAllowedDirection = randDir;
            return;
        }
    }
    /* random_direction_disallowed */
    if (a5->direction == 0 || a5->direction == 4)
        return;
    goto apply_turn_delta_from_angle;

foul:
    if ((_aiRand & 0xF) == 0)
        goto check_closest_player_facing;
    /* compute_allowed_dir_from_angle_bucket */
    {
        int16_t angleBucket = (int16_t)(((uint16_t)(d5 + 16)) & 0xFF) >> 5;
        if (angleBucket == d7)
            goto apply_turn_delta_from_angle;
        team->currentAllowedDirection = angleBucket;
        return;
    }

apply_turn_delta_from_angle:
    if (d7 < 0)
        return;
    d2_byte = (int8_t)((uint8_t)(d7 << 5) - (uint8_t)d5);
    goto prep_aftertouch_and_fire_decision;

check_closest_player_facing:
    closestPlayer = FindClosestPlayerToBallFacing(
        (uint16_t)(d7 << 5), team);
    if (closestPlayer != (Sprite *)0xFFFFFFFF &&
        a5 != (Sprite *)0 &&
        a5->teamNumber == closestPlayer->teamNumber)
        goto our_player_closest;

    /* no_player_facing */
    goto no_player_facing;

no_player_facing:
    if ((_currentGameTick & 0xE) != 0)
        return;

    if (_aiTurnDirection < 0) {
        if (_aiTurnDirection != -1) {
            _aiTurnDirection++;
            if (_aiTurnDirection != -1)
                return;
        }
        d1 = 1;
    } else {
        if (_aiTurnDirection != 1) {
            _aiTurnDirection--;
            if (_aiTurnDirection != 1)
                return;
        }
        d1 = -1;
    }

    /* try_apply_turn_step */
    d0 = (d7 + _aiTurnDirection) & 7;
    if (_playerTurnFlags & (1 << d0)) {
        team->currentAllowedDirection = d0;
        return;
    }
    /* commit_turn_direction_and_return */
    _aiTurnDirection = d1;
    return;

set_direction:
    /* set_direction_core */
    if (_resultTimer != 0)
        return;
    if (_gameState == ST_GOAL_OUT_LEFT ||
        _gameState == ST_GOAL_OUT_RIGHT ||
        _gameState == ST_KEEPER_HOLDS_BALL)
        goto no_player_facing;
    if (_gameState >= ST_THROW_IN_FORWARD_RIGHT &&
        _gameState <= ST_THROW_IN_BACK_LEFT)
        goto no_player_facing;
    if (_gameState == ST_FOUL)
        goto foul_or_free_kick_set_dir;
    if (_gameState < ST_FREE_KICK_LEFT1 || _gameState > ST_FREE_KICK_RIGHT3)
        return;

foul_or_free_kick_set_dir:
    /* Force direction toward opponent's goal */
    team->currentAllowedDirection = (int16_t)(((uint16_t)(d5 + 16)) & 0xFF) >> 5;
    return;

    /* ---- Aftertouch and fire decision ---- */
prep_aftertouch_and_fire_decision:
    if (_AI_maxStoppageTime <= _stoppageTimerActive)
        _AI_maxStoppageTime = _stoppageTimerActive;
    _aiResumePlayTimer = 15;

    if (_gameState == ST_PENALTY || _gameState == ST_PENALTIES) {
        team->AI_afterTouchStrength = 0;
        goto apply_dir_and_normal_fire;
    }

    if (_gameState >= ST_FREE_KICK_LEFT1 && _gameState <= ST_FREE_KICK_RIGHT3)
        goto aftertouch_by_distance;

    if (_gameState == ST_CORNER_LEFT || _gameState == ST_CORNER_RIGHT) {
        team->AI_afterTouchStrength = 1;
        goto apply_dir_and_normal_fire;
    }

    /* non_setpiece_random_branch */
    if ((_aiRand & 0x18) != 0)
        goto aftertouch_by_distance;
    /* Fall through to set_aftertouch_strong */
    team->AI_afterTouchStrength = 2;
    goto apply_dir_and_normal_fire;

aftertouch_by_distance:
    if (d6 < 28800) {
        team->AI_afterTouchStrength = 0;
    } else if (d6 < 57800) {
        team->AI_afterTouchStrength = 1;
    } else {
        team->AI_afterTouchStrength = 2;
    }

apply_dir_and_normal_fire:
    team->currentAllowedDirection = d7;
    team->normalFire = 1;

    /* Penalty/penalties: zero spin and return */
    if (_gameState == ST_PENALTY || _gameState == ST_PENALTIES) {
        team->AI_ballSpinDirection = 0;
        return;
    }

    /* Corner: random spin direction */
    if (_gameState == ST_CORNER_LEFT || _gameState == ST_CORNER_RIGHT) {
        int16_t randVal = _aiRand & 7;
        int16_t spinDir;
        int16_t testDir;
        if (randVal < 3) {
            spinDir = 1;
            testDir = (d7 + 1) & 7;
        } else if (randVal < 6) {
            spinDir = -1;
            testDir = (d7 - 1) & 7;
        } else {
            team->AI_ballSpinDirection = 0;
            return;
        }
        if (_playerTurnFlags & (1 << testDir)) {
            team->AI_ballSpinDirection = spinDir;
            return;
        }
        team->AI_ballSpinDirection = 0;
        return;
    }

    /* General foul/free kick: compute turn from angle sign */
    _aiResumePlayTimer = 15;
    team->currentAllowedDirection = d7;
    team->normalFire = 1;
    wVar = 0;

    if (_gameState == ST_FOUL) {
        if (team == &_topTeamData) {
            if (a2->y.s.i >= 682)
                goto apply_turn_and_return;
        } else {
            if (a2->y.s.i <= 216)
                goto apply_turn_and_return;
        }
    }

    /* compute_turn_from_angle_sign */
    wVar = -1;
    if (d2_byte < 0)
        wVar = 1;

apply_turn_and_return:
    team->AI_ballSpinDirection = wVar;
    return;

resume_play_and_apply_turn_bias:
    _aiResumePlayTimer = 15;
    team->currentAllowedDirection = d7;
    team->normalFire = 1;
    d0 = -1;
    if (d2_byte < 0)
        d0 = 1;
    team->AI_ballSpinDirection = d0;
    return;

    /* ---- noone_near path ---- */
noone_near:
    if (AI_DecideWhetherToTriggerFire(a5, team, d7) == 0)
        return;

    /* Check pass-to player distance difference */
    if (a4 != (Sprite *)0) {
        if (a4->ballDistance - a5->ballDistance >= 50) {
            /* Swap controlled and pass-to player */
            Sprite *tmp = team->passToPlayerPtr;
            team->passToPlayerPtr = team->controlledPlayer;
            team->controlledPlayer = tmp;
            return;
        }
    }

    /* choose_full_angle_or_random_bucket */
    if (a4 == (Sprite *)0)
        goto derive_allowed_dir_from_full_angle;

    /* deadVarAlways0 is always 0 */
    if (_deadVarAlways0 != 0)
        goto set_allowed_dir_from_angle_bucket;

    if (_topTeamData.playerNumber != 0 || _bottomTeamData.playerNumber != 0)
        goto derive_allowed_dir_from_full_angle;

    /* random_rotate_when_no_target: both teams are computer */
    {
        d0 = -1;
        /* SWOS2.s:32462-32469. The x tests jump TO the gate, the y tests jump
         * PAST it: a player inside the central corridor and already deep in
         * either box gets no forced direction at all (-1). Everyone else — off
         * the corridor, or between the boxes — reaches the gate.
         *
         * The DOS build does not have this test. Its
         * `@@randomly_flip_or_continue_direction` (swos.asm:118956) opens
         * straight on `mov ax, currentGameTick / and 18h`; all four position
         * compares are absent, and 273/398 appear nowhere else in that listing
         * except the goalkeeper-area check. So on DOS every player reaches the
         * gate. Measured: zkz-10174427 frame 1259 has the controlled player at
         * (361, 757) — inside the corridor and past 740, so the Amiga leaves
         * him unforced — and the recording forces direction 6, which is what
         * the gate's `tick & 0x18` arm gives.
         *
         * Reached only when BOTH teams are computer-controlled, which in the
         * corpus means that one coach-mode recording. */
        if (!_gDosGameplay &&
            a5->x.s.i >= 273 && a5->x.s.i <= 398 &&
            (a5->y.s.i <= 158 || a5->y.s.i >= 740)) {
            /* loc_118040: leave the direction unforced */
            team->currentAllowedDirection = d0;
            return;
        }

        /* random_rotate_or_base_dir_gate */
        if ((_currentGameTick & 0x18) != 0) {
            /* 87.5%: use base direction */
            team->currentAllowedDirection = d7;
            return;
        }
        /* 12.5%: random rotate */
        {
            int16_t rotateIdx = (_aiRand & 2);  /* 0 or 2 = byte offset */
            int16_t rotateOffset = _aiRandomRotateTable[rotateIdx >> 1];
            uint8_t rotated = (uint8_t)(a5->fullDirection + rotateOffset);
            rotated = (uint8_t)(rotated + 128);  /* flip 180 degrees (byte add) */
            d0 = (int16_t)(((uint16_t)(rotated + 16)) & 0xFF) >> 5;
        }
        team->currentAllowedDirection = d0;
        return;
    }

    /* ---- Spin/aftertouch when we have ball ---- */
we_have_ball_spin:
    d1 = team->controlledPlDirection;

    if (_playingPenalties == 0 && _penalty == 0) {
        if ((_aiRand & 1) == 0)
            goto apply_spin_quick_or_copy_dir;
        if (team->AI_ballSpinDirection < 0) {
            /* use_left_spin_table */
            d0 = (d1 - 1) & 7;
            {
                int16_t tableIdx = team->AI_afterTouchStrength;
                if (tableIdx > 2) tableIdx = 2;
                d1 = (d1 + _aiLeftSpinTable[tableIdx]) & 7;
            }
            team->currentAllowedDirection = d1;
            return;
        }
        if (team->AI_ballSpinDirection > 0) {
            /* use_right_spin_table */
            d0 = (d1 + 1) & 7;
            {
                int16_t tableIdx = team->AI_afterTouchStrength;
                if (tableIdx > 2) tableIdx = 2;
                d1 = (d1 + _aiRotateRightTable[tableIdx]) & 7;
            }
            team->currentAllowedDirection = d1;
            return;
        }
        /* AI_ballSpinDirection == 0: fall through to apply_spin_quick_or_copy_dir */
    }

apply_spin_quick_or_copy_dir:
    if (team->AI_afterTouchStrength == 1) {
        team->currentAllowedDirection = -1;
        return;
    }
    /* use_long_kick_table */
    {
        int16_t tableIdx = team->AI_afterTouchStrength;
        if (tableIdx > 2) tableIdx = 2;
        d1 = (d1 + _aiLongKickTable[tableIdx]) & 7;
    }
    team->currentAllowedDirection = d1;
    return;
}
