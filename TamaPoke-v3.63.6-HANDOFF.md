# TamaPoke KO v3.63.6 — current handoff

Current baseline: **v3.63.6 Reserved Species Guard + Training Reward Boost + Stability**.

## Temporarily disabled species
- National Dex **#1020 Gouging Fire / 꿰뚫는화염** is disabled from gameplay.
- National Dex **#1021 Raging Bolt / 날뛰는우레** is disabled from gameplay.
- Their National Dex numbers do not move. Under the current deterministic `natdex + 18` base-ID rule, their TamaPoke internal IDs remain **1038** and **1039**.
- The catalog still emits those positional rows, but `DEX_ENABLED` is 0, so they are not hatchable, encounterable, boss candidates, or selectable battle members.
- Their old normal/shiny SD files are included in `retired_sprite_ids`, so WebSerial sprite synchronization can delete stale p1038/ps1038/p1039/ps1039 resources.
- Do not reuse 1038/1039 for another Pokemon. Re-enable the same National Dex entries later by removing them from `USER_DISABLED_NATDEX` after acceptable sprites are available.

## Current training behavior
- Properly completed training always grants **5 matching IV berries**.
- The bonus roll remains **30%**; on success the reward is **9 total**.
- A separate **30%** roll grants one Shiny Berry.
- The existing Shiny Charm remains unchanged and boosts only the next egg's Shiny probability.

## Stability guards
- `speciesHasArt()`/`DEX_ENABLED` remains authoritative for random pools and boss candidates.
- Home sprite loading and team/care-slot battle selection also reject disabled entries, preventing stale SD files or old saves from leaking a disabled species back into active gameplay.
- `verify_reserved_species.py` checks fixed IDs, disabled flags, tombstone/deletion IDs, and packed-file absence.
- Existing learnset expansion, staggered training persistence, battle/audio/display stability and PUT4 SD transfer guards remain enabled.

## Release hygiene
- Do not include historical UPDATE/FINAL_VALIDATION/PROJECT_STATE change-log files in release ZIPs.
- Do not create or include SHA-256 checksum files in user-facing release packages.

Use v3.63.6 as the latest baseline.
