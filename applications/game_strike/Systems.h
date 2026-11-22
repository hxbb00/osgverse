#ifndef VERSE_STRIKE_SYSTEMS_H
#define VERSE_STRIKE_SYSTEMS_H

#include "GameContext.h"

namespace game
{
    /** Player movement (physics character) + camera + gun viewmodel update */
    void updatePlayer(GameContext& ctx, double dt);

    /** Fire requests: physics raycast, damage targets, kick props, spawn effects */
    void updateCombat(GameContext& ctx);

    /** Respawn dead targets after their delay */
    void updateTargets(GameContext& ctx, double dt);

    /** Retire transient effect entities (tracers) whose lifetime is over */
    void updateEffects(GameContext& ctx, double dt);

    /** HUD updates (score, timed messages) */
    void updateHud(GameContext& ctx, double dt);
}

#endif
