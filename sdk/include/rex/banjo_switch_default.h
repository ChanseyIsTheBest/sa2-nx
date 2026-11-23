// banjo-nx (2026-11-01, the user's lean renut.toml): the shipped Switch settings are the cvars' own defaults on the
// Switch, so renut.toml lists only the keys meant to be changed (any other key still works when a toml sets it).
// BANJO_SWITCH_DEFAULT(on_switch, elsewhere): the Switch build's default, and the one every other build (the PC tests)
// keeps.
#pragma once
#ifndef BANJO_SWITCH_DEFAULT

#if defined(__SWITCH__)
#define BANJO_SWITCH_DEFAULT(on_switch, elsewhere) (on_switch)
#else
#define BANJO_SWITCH_DEFAULT(on_switch, elsewhere) (elsewhere)
#endif
#endif  // BANJO_SWITCH_DEFAULT
