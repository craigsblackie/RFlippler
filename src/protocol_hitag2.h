#pragma once

// Flipper Zero LFRFID ProtocolBase entry for HiTag2.
// Drop protocol_hitag2.c/h into lib/lfrfid/protocols/ in the firmware repo.
// Then register in lfrfid_protocols.h/c (see PLAN.md Phase 5).

// Forward-declared to match Flipper toolbox/protocols/protocol.h
typedef struct ProtocolBase ProtocolBase;

extern const ProtocolBase protocol_hitag2;
