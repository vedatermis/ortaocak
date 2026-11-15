// shared/game - gameplay definitions shared by client and server. Umbrella header.
//   Character.h    races, classes, appearance limits, starting values
//   Names.h        username / password / character name rules and uniqueness keys
//   Movement.h     speeds, validation tolerances, send/relay rates, AOI grid constants
//   TerrainWalk.h  straight-segment walkability on a terrain walk mask (not included here: it pulls
//                  in shared/assets)
#pragma once

#include "shared/game/Character.h"
#include "shared/game/Movement.h"
#include "shared/game/Names.h"
