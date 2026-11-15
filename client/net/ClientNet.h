// client/net - the game client's server connection (built on shared/net). Umbrella header:
//   GameClient.h      io_context + ClientSession, events for scenes, ping/RTT, request timeouts
//   AccountState.h    logged-in account, character list, world entry
//   NetText.h         Turkish texts for every connect/login/character/disconnect outcome  (core)
//   RequestTracker.h  request deadlines and ping bookkeeping, socket-free                 (core)
#pragma once

#include "client/net/AccountState.h"
#include "client/net/GameClient.h"
#include "client/net/NetText.h"
#include "client/net/RequestTracker.h"
