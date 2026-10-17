#pragma once

#include <Notecard.h>

// Its own translation unit so notecard_manager and time_sync can both reach the
// instance without depending on each other.
extern Notecard notecard;

