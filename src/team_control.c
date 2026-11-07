#include "career_plus_options.h"
/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
#include "team_control.h"
#include "dos_mode.h"      /* _gDosGameplay: DOS alternates teams on its own counter */

/* ==================================================================
 * Chunk 54 — Team Controls + Player Selection
 *
 * Ported functions:
 *   UpdateControlledPlayer      (A6=team)
 *   UpdatePlayerBeingPassedTo   (A6=team)
 *   UpdateAndApplyTeamControls  (no params)
 * ================================================================== */

/* ---- External globals ---- */
extern Sprite     _ballSprite;
extern TeamGeneralInfo _topTeamData;
extern TeamGeneralInfo _bottomTeamData;

extern int16_t    _gameStatePl;
extern int16_t    _playingPenalties;
extern Sprite    *_penaltyShooterSprite;
extern int16_t    _fireBlocked;
uint16_t          _teamSwitchCounter;   /* DOS-only; see UpdateAndApplyTeamControls */

/* ==================================================================
 * A BUG IN THE ORIGINAL AMIGA GAME, DELIBERATELY FIXED HERE.
 *
 * This is NOT a port inaccuracy. SWOS2.s does exactly what is described below,
 * instruction for instruction, and so did this port until now. The owner's call
 * (2026-09-13) was that it is a defect in the original and should be fixed,
 * with the original behaviour kept available.
 *
 *   0 (default) = fixed: an OPPONENT's tackle no longer wipes your shot charge.
 *   1           = the original Amiga behaviour, bit-exact.
 *
 * THE MECHANISM
 *
 * `plNFireCounter` goes to -1 when fire is pressed and one more negative per
 * poll while it is held. `processInputForPlayer` arms `normalFire` — the SHOT —
 * on the single poll it passes -5, and zeroes the counter in the same breath.
 * `PlayerNStatusProc` only decrements a NON-ZERO counter, so once it is zero a
 * still-held button never charges again, and a release negates 0 to 0 and does
 * nothing. Zero means "this hold is spent".
 *
 * Both headerOrTackle tails below spend the hold that way (SWOS2.s:25650 /
 * 25701, swos.asm:100708 — two independent listings agree).
 *
 * WHAT IS DELIBERATE, AND WHAT IS AN ACCIDENT
 *
 * For a HUMAN team spending its own hold is by design: the press made your
 * player lunge, so it must not ALSO become a shot. One press, one action. That
 * tail is left exactly as the original wrote it.
 *
 * The other one is an accident of the asm's control flow. A team with
 * `playerNumber` 0 — the COMPUTER — has no tail of its own: `beq.w
 * post_input_common_p1` drops it into the PLAYER 1 tail, so when an AI player
 * tackles, the `clr.w (pl1FireCounter)` it lands on is the HUMAN's. Nothing
 * about the human's press was consumed; an opponent simply lunged at him, and
 * his charge dies for it. There is no design reading of that — it is the shared
 * tail, and the original's own author would have had to write a separate tail
 * to avoid it. It is also why it hits hardest exactly where a player notices:
 * in the box, with a defender on you, being tackled at constantly.
 *
 * MEASURED over the replay corpus, one full match: 21 of these AI-caused wipes
 * against 40 shots that actually armed. A tester who plays the original well
 * reported it as "sometimes your fire button is blocked when in a scoring
 * position; I am hitting/holding fire and the player won't release the shot",
 * with a screenshot of a defender overlapping him.
 *
 * WHAT THE FIX DOES — AND WHAT IT DELIBERATELY DOES NOT
 *
 * Only this: the computer team's tail no longer touches the human's counter.
 * The human's hold then resolves exactly as it would have if the opponent had
 * not lunged — it keeps charging, arms the shot at -5, and a release still
 * produces the pass or shot it always would.
 *
 * It does NOT touch the human team's own tail. Restarting a charge there was
 * tried and rejected: the original's zero also makes the RELEASE inert, so
 * reviving the counter turns letting go after your own tackle into a surprise
 * pass — a new bug, not a fix. "One press, one action" stays.
 *
 * Set from [gameplay] fire_hold_lockout / --fire-hold-lockout=on. The replay
 * rig forces it ON: the corpus compares against a reference implementation of
 * the ORIGINAL, so the rig has to play the original. Amiga builds have no
 * command line and get the fix (the default).
 * ================================================================== */
int _gOriginalFireHoldLockout = 0;
extern int16_t    _inSubstitutesMenu;
extern uint16_t   _frameCount;

/* Input state (in SWOS2.s code section) */
extern int8_t     _pl1Fire;
extern int8_t     _pl1ShortFire;
extern int16_t    _pl1FireCounter;
extern int16_t    _pl1Direction;
extern int8_t     _pl2Fire;
extern int8_t     _pl2ShortFire;
extern int16_t    _pl2FireCounter;
extern int16_t    _pl2Direction;

/* ---- External functions ---- */
extern void Player1StatusProc(void);
extern void Player2StatusProc(void);
extern int InputSDL_TakeSpecialShot(void); /* SDL-only: 1 kick, 2 opposite aftertouch */
extern void InputSDL_SpecialShotCompleted(void);
extern void ApplyBallAfterTouch(TeamGeneralInfo *team);
extern void UpdateGameTimersAndCameraBreakMode(void);

/* UpdatePlayers is now ported to C */
#include "update_players.h"

/* ==================================================================
 * UpdateControlledPlayer — find nearest player to ball for team
 *
 * Scans all 11 players, computes squared distance to ball,
 * stores in SPRITE_BALLDISTANCE. Selects nearest eligible player
 * as controlledPlayer. If new controlled player differs from current,
 * stops old one (sets dest = current pos) and updates pointer.
 * Also clears passToPlayerPtr if new controlled player was the
 * pass target.
 * ================================================================== */
__attribute__((used, externally_visible))
void UpdateControlledPlayer(TeamGeneralInfo *team) {
    Sprite *ball = &_ballSprite;
    int16_t ballX = ball->x.s.i;
    int16_t ballY = ball->y.s.i;
    Sprite **sprTable = team->spritesTable;
    Sprite *nearest = (Sprite *)0;
    int32_t minDist = -1;  /* 0xFFFFFFFF as unsigned = max */
    int16_t i;

    /* Scan the table FORWARD, 0..10. The asm's loop counter runs DOWN
     * (`move.w #10,d6` … `dbf d6`) but the table pointer runs UP
     * (`movea.l (a1)+,a2`), so the players are visited in ASCENDING order —
     * translating the counter into the index reverses that. It is invisible
     * until two candidates TIE on ballDistance, and then it silently picks the
     * other man: the replay rig caught exactly that (playa-10090613 frame 795,
     * slots 7 and 10 both at 1625, reference takes 7 and we took 10).
     * See docs/TESTS.md, "The loop that counted down while the pointer went
     * up". */
    for (i = 0; i <= 10; i++) {
        Sprite *pl = sprTable[i];
        int16_t dx = pl->x.s.i - ballX;
        int16_t dy = pl->y.s.i - ballY;
        int32_t dist = (int32_t)dx * dx + (int32_t)dy * dy;
        pl->ballDistance = dist;

        /* Skip filtering when game not in progress */
        if (_gameStatePl == ST_GAME_IN_PROGRESS) {
            if (!pl->onScreen)
                continue;
        }

        /* Skip sent-away players */
        if (pl->sentAway)
            continue;

        /* Skip goalkeeper if not playing */
        if (!team->goaliePlayingOrOut) {
            if (pl->playerOrdinal == 1)
                continue;
        }

        /* Skip passing/kicking player */
        if (pl == team->passingKickingPlayer)
            continue;

        /* Skip players in certain states */
        if (pl->playerState == PL_TACKLING ||
            pl->playerState == PL_TACKLED ||
            pl->playerState == PL_JUMP_HEADING ||
            pl->playerState == PL_STATIC_HEADING ||
            pl->playerState == PL_ROLLING_INJURED)
            continue;

        /* Skip pass target */
        if (pl == team->passToPlayerPtr)
            continue;

        /* Check if closer than current best (unsigned compare) */
        if ((uint32_t)dist >= (uint32_t)minDist)
            continue;

        minDist = dist;
        nearest = pl;
    }

    /* During penalties + game in progress: skip controlled player update */
    if (_playingPenalties) {
        if (_gameStatePl == ST_GAME_IN_PROGRESS)
            return;
    }

    if (nearest == (Sprite *)0)
        return;

    /* Only update if ball is out of play (player switch allowed) */
    if (!team->ballOutOfPlay)
        return;

    if (nearest != team->controlledPlayer) {
        /* Stop old controlled player */
        Sprite *old = team->controlledPlayer;
        if (old != (Sprite *)0) {
            if (old->playerState == PL_NORMAL) {
                old->destX = old->x.s.i;
                old->destY = old->y.s.i;
            }
        }
    }

    team->controlledPlayer = nearest;

    /* If new controlled player was the pass target, clear pass target */
    if (nearest == team->passToPlayerPtr) {
        Sprite *oldPass = team->passToPlayerPtr;
        if (oldPass != (Sprite *)0) {
            if (oldPass->playerState == PL_NORMAL) {
                oldPass->destX = oldPass->x.s.i;
                oldPass->destY = oldPass->y.s.i;
            }
        }
        team->passToPlayerPtr = (Sprite *)0;
    }
}

/* ==================================================================
 * UpdatePlayerBeingPassedTo — find closest secondary player
 *
 * Two code paths: game in progress vs game stopped.
 * Game in progress: checks onScreen, various exclusions.
 * Game stopped: skips onScreen check but otherwise similar.
 * ================================================================== */
__attribute__((used, externally_visible))
void UpdatePlayerBeingPassedTo(TeamGeneralInfo *team) {
    Sprite *ball = &_ballSprite;

    if (_gameStatePl != ST_GAME_IN_PROGRESS) {
        /* ----- Stopped path ----- */
        team->ballX = ball->x.s.i;
        team->ballY = ball->y.s.i;

        if (!team->ballInPlay)
            return;
        if (team->playerSwitchTimer)
            return;

        if (team->passingToPlayer) {
            if (team->passToPlayerPtr)
                return;
        }

        /* Penalties special case */
        if (_playingPenalties) {
            if (_gameStatePl != ST_PENALTIES)
                return;
            Sprite *shooter = _penaltyShooterSprite;
            team->ballInPlay = 0;
            team->passToPlayerPtr = shooter;
            return;
        }

        /* Scan team for nearest eligible (stopped) */
        {
            Sprite **sprTable = team->spritesTable;
            int16_t bx = ball->x.s.i;
            int16_t by = ball->y.s.i;
            Sprite *nearest = (Sprite *)0;
            int32_t minDist = -1;
            int16_t i;

            /* Ascending — see UpdateControlledPlayer above. */
            for (i = 0; i <= 10; i++) {
                Sprite *pl = sprTable[i];

                if (pl->sentAway)
                    continue;

                if (!team->ballOutOfPlayOrKeeper) {
                    if (pl->playerOrdinal == 1)
                        continue;
                }

                if (pl == team->controlledPlayer)
                    continue;
                if (pl == team->passingKickingPlayer)
                    continue;
                if (pl->playerState != PL_NORMAL)
                    continue;

                {
                    int32_t dist = pl->ballDistance;
                    if ((uint32_t)dist >= (uint32_t)minDist)
                        continue;
                    minDist = dist;
                    nearest = pl;
                }
            }

            if (nearest == (Sprite *)0)
                return;

            team->passToPlayerPtr = nearest;
        }
        return;
    }

    /* ----- Game in progress path ----- */
    team->ballX = ball->x.s.i;
    team->ballY = ball->y.s.i;

    if (!team->ballInPlay)
        return;
    if (team->playerSwitchTimer)
        return;

    if (team->passingToPlayer) {
        if (team->passToPlayerPtr)
            return;
    }

    /* Penalties special case */
    if (_playingPenalties) {
        if (_gameStatePl != ST_PENALTIES)
            return;
        Sprite *shooter = _penaltyShooterSprite;
        team->ballInPlay = 0;
        team->passToPlayerPtr = shooter;
        return;
    }

    /* Scan team for nearest eligible (in progress) */
    {
        Sprite **sprTable = team->spritesTable;
        Sprite *nearest = (Sprite *)0;
        int32_t minDist = -1;
        int16_t i;

        /* Ascending — see UpdateControlledPlayer above. */
        for (i = 0; i <= 10; i++) {
            Sprite *pl = sprTable[i];

            if (!pl->onScreen)
                continue;
            if (pl->sentAway)
                continue;

            if (!team->ballOutOfPlayOrKeeper) {
                if (pl->playerOrdinal == 1)
                    continue;
            }

            if (pl == team->controlledPlayer)
                continue;
            if (pl == team->passingKickingPlayer)
                continue;
            if (pl->playerState != PL_NORMAL)
                continue;

            {
                int32_t dist = pl->ballDistance;
                if ((uint32_t)dist >= (uint32_t)minDist)
                    continue;
                minDist = dist;
                nearest = pl;
            }
        }

        if (nearest == (Sprite *)0)
            return;

        /* Update passToPlayerPtr, stopping old if needed */
        if (nearest != team->passToPlayerPtr) {
            Sprite *old = team->passToPlayerPtr;
            if (old != (Sprite *)0) {
                if (old->playerState == PL_NORMAL) {
                    old->destX = old->x.s.i;
                    old->destY = old->y.s.i;
                }
            }
        }

        team->passToPlayerPtr = nearest;
    }
}

/* ==================================================================
 * processInputForPlayer — common input dispatch logic
 *
 * Shared by player 1 and player 2 paths. Reads joystick state,
 * writes direction/fire fields on team struct, handles fire counter
 * classification (quickFire vs normalFire), subs menu override.
 * Returns: 1 if should exit early, 0 otherwise.
 * ================================================================== */
static int16_t processInputForPlayer(TeamGeneralInfo *team,
                                     int16_t direction, int8_t _shortFire,
                                     int8_t _fire, int16_t *fireCounterPtr, int specialShot) {
    int16_t fc;

    team->currentAllowedDirection = direction;
    team->direction = direction;
    team->fireThisFrame = _shortFire;
    team->firePressed = _fire;

    if (_fire) {
        team->fireCounter++;
    } else {
        team->fireCounter = 0;
    }

    /* Classify fire counter into quickFire / normalFire */
    fc = *fireCounterPtr;
    if (fc == 0) {
        /* No fire press pending */
        team->quickFire = 0;
        team->normalFire = 0;
    } else if (fc < 0) {
        /* Negative fire counter (release-bounce) */
        team->quickFire = 0;
        if (team->normalFire) {
            team->normalFire = 0;
        }
        team->quickFire = 0;
        /* Check threshold: fc < -4 → normalFire */
        if (fc < -4) {
            team->normalFire = -1; /* 0xFF = true (slt sets to -1 when true) */
        } else {
            team->normalFire = 0;
        }
        if (fc >= -4) {
            /* fc in [-4..-1]: no action, keep as-is */
        } else {
            *fireCounterPtr = 0;
        }
    } else {
        /* Positive fire counter (press registered) */
        team->quickFire = -1;  /* sne: non-zero → 0xFF */
        team->normalFire = -1;
        /* fc <= 4 → quickFire, fc > 4 → normalFire */
        if (fc <= 4) {
            team->quickFire = -1;
            team->normalFire = 0;
        } else {
            team->quickFire = 0;
            team->normalFire = -1;
        }
        *fireCounterPtr = 0;
    }

    /* Present the dedicated shot to the original engine in ANY match state.
     * In particular, stoppages have gameStatePl == ST_STOPPED and the engine
     * itself validates direction, possession, ball height and player state.
     * The old proximity and live-play checks discarded valid presses. */
    if (specialShot && !_fire && !_inSubstitutesMenu && !team->resetControls) {
        team->quickFire = 0;
        team->normalFire = -1;

        /* A and D also request the ORIGINAL header action for an airborne
         * ball.  Headers are not normalFire actions: UpdatePlayers checks
         * fireThisFrame separately when the ball is above foot height.
         * Limit this extra signal to a nearby outfield player and a high
         * ball, so it cannot accidentally produce a ground-level tackle. */
        if (_gameStatePl == ST_GAME_IN_PROGRESS && _ballSprite.z.s.i > 8 &&
            team->controlledPlayer &&
            team->controlledPlayer->playerOrdinal != 1 &&
            team->controlledPlayer->ballDistance <= 2450) {
            team->fireThisFrame = -1;
        }
    }

    /* Check resetControls */
    if (team->resetControls)
        return 1;

    /* If in substitutes menu, clear all controls */
    if (_inSubstitutesMenu) {
        team->currentAllowedDirection = -1;
        team->quickFire = 0;
        team->normalFire = 0;
        team->firePressed = 0;
        team->fireThisFrame = 0;
        team->fireCounter = 0;
    }

    /* Call UpdatePlayers */
    {
        int specialHeaderRequest = specialShot && !_fire &&
                                   team->fireThisFrame && _ballSprite.z.s.i > 8;
        UpdatePlayers(team);
        /* A successful header consumes the queued A/D press just as a kick
         * does.  Otherwise it could produce a surprise shot after the ball
         * lands.  With an airborne ball the extra fireThisFrame signal can
         * only enter the game's existing heading path, not the tackle path. */
        if (specialHeaderRequest && team->headerOrTackle)
            InputSDL_SpecialShotCompleted();
        /* The kick has actually started.  Stop retrying this press.
         * On throw-ins the release animation is queued before the kick. */
        if (specialShot && (team->shooting ||
                            (team->controlledPlayer &&
                             team->controlledPlayer->playerState == PL_THROW_IN &&
                             team->normalFire))) {
            /* A replicates the original high/long kick: after shooting,
             * pulling the stick 180 degrees triggers high-kick aftertouch
             * at spinTimer=4. Apply that branch directly rather than
             * steering the player backwards and possibly changing aim. */
            if (specialShot == 2 && team->shooting && team->spinTimer >= 0) {
                int16_t savedDirection = team->currentAllowedDirection;
                team->currentAllowedDirection = (team->controlledPlDirection + 4) & 7;
                team->spinTimer = 4;
                ApplyBallAfterTouch(team);
                team->currentAllowedDirection = savedDirection;
            }
            InputSDL_SpecialShotCompleted();
        }
    }

    /* Check headerOrTackle. THIS one is deliberate in the original and stays:
     * the press made your own player lunge, so it must not also become a shot.
     * See the block over _gOriginalFireHoldLockout. */
    if (team->headerOrTackle) {
        team->headerOrTackle = 0;
        *fireCounterPtr = 0;
        return 1;
    }

    return 0;
}

/* ==================================================================
 * UpdateAndApplyTeamControls — per-frame input dispatch
 *
 * Called from GameLoop. Alternates between top and bottom team
 * each frame (frameCount bit 0). Polls joystick, sets direction/fire
 * on TeamGeneralInfo, calls UpdatePlayers.
 * ================================================================== */
__attribute__((used, externally_visible))
void UpdateAndApplyTeamControls(void) {
    TeamGeneralInfo *team;

    UpdateGameTimersAndCameraBreakMode();

    /* Fire blocking: wait until both fire buttons released */
    if (_fireBlocked) {
        Player1StatusProc();
        if (_pl1Fire)
            return;
        Player2StatusProc();
        if (_pl2Fire)
            return;
        _fireBlocked = 0;
        _pl1FireCounter = 0;
        _pl2FireCounter = 0;
        return;
    }

    /* Alternate teams each frame — but the two releases count with DIFFERENT
     * VARIABLES, the same split as `BenchCheckControls` (bench_subs.c):
     *
     *   Amiga  btst #0,(frameCount+1)      SWOS2.s:25593 — the global frame
     *                                      counter, which advances EVERY frame
     *   DOS    add teamSwitchCounter,1     swos.asm:100580 — a dedicated
     *          test byte ptr ...,1         counter, and the `add` sits BELOW
     *                                      the fireBlocked early-exit above,
     *                                      so it stands still for every frame
     *                                      a held fire button blocks
     *
     * The phases therefore separate across a fire-block and never come back.
     * Measured on zkz-12065730: player 2 holds fire through a stoppage to
     * f1249 and releases at f1250, and the reference resumes writing the
     * bottom team's `direction` at f1252 where frameCount would have said
     * f1251 — which is the frame the bench's double-tap detector then latches
     * `subsPrevPl2Direction` on (recorded -1 at 1251, 7 at 1253).
     *
     * Same proof as `benchPickTeamCounter`: one site in each listing, and the
     * conversion did not force it — the DOS build still has `frameCount` and
     * uses it elsewhere. The Amiga path is unchanged. */
    {
        uint16_t pick;
        if (_gDosGameplay) {
            _teamSwitchCounter++;
            pick = _teamSwitchCounter;
        } else {
            pick = _frameCount;
        }
        team = (pick & 1) ? &_topTeamData : &_bottomTeamData;
    }

    /* Update controlled player and pass target */
    UpdateControlledPlayer(team);
    UpdatePlayerBeingPassedTo(team);

    /* Dispatch based on playerNumber */
    if (team->playerNumber == 0) {
        /* No human player for this team */
        /* resetControls check */
        if (team->resetControls)
            return;

        /* Subs menu override */
        if (_inSubstitutesMenu) {
            team->currentAllowedDirection = -1;
            team->quickFire = 0;
            team->normalFire = 0;
            team->firePressed = 0;
            team->fireThisFrame = 0;
            team->fireCounter = 0;
        }

        UpdatePlayers(team);

        if (team->headerOrTackle) {
            team->headerOrTackle = 0;
            /* Yes, player 1's counter — even though this team is the
             * computer's. In the asm a playerNumber of 0 does not get a tail
             * of its own: `beq.w post_input_common_p1` drops it into the
             * PLAYER 1 tail, so the `clr.w (pl1FireCounter)` after
             * UpdatePlayers is shared (SWOS2.s:25650, swos.asm:100708).
             * The effect is real and load-bearing: a tackle by the AI side
             * resets the human's fire hold, so a button held across an
             * opponent's challenge never reaches the -5 that arms normalFire.
             * playa-10095214 diverged at f10129 on exactly that — we shot,
             * the reference kept dribbling. */
            if (_gOriginalFireHoldLockout)
                _pl1FireCounter = 0;
        }
        return;
    }

    if (team->playerNumber == 2) {
        /* Player 2 controls this team */
        Player2StatusProc();
        processInputForPlayer(team, _pl2Direction, _pl2ShortFire, _pl2Fire,
                              &_pl2FireCounter, 0);
        return;
    }

    /* Player 1 controls this team (playerNumber == 1 or other) */
    Player1StatusProc();
    processInputForPlayer(team, _pl1Direction, _pl1ShortFire, _pl1Fire,
                          &_pl1FireCounter, _careerThreeButtonOption ? InputSDL_TakeSpecialShot() : 0);
}
