# FS-07: Venue profiles

Status: draft 0.1. Depends on FS-00 (settings merge, registry). Feature id `profiles`. UI `66-profiles.js`.

## Goal

A touring act arrives at a venue and loads "Club A (wired)" or "Rehearsal
room" in one action instead of re-entering outputs, ports and offsets.

## What a profile contains

A named snapshot of a whitelisted subset of settings:

| Included | Notes |
|---|---|
| `mode`, `deviceNumber`, `beatOnly`, `profile` (stage/rehearsal) | |
| `latencyMs` | |
| `link`, `midi`, `osc` sections | including OSC profile and QLab cues |
| `venue`, `performer` | |
| `features.<id>` for every feature | each feature's settings |
| `interface` | stored, but only applied when "include interface" is ticked (names differ between machines), default off |

Never included: bar-offset memory (per track, lives in settings raw), HTTP
bind and port, anything unknown to the whitelist.

## Files

One file per profile in `<config dir>/profiles/<slug>.json` (sharing and
version control friendly):

```json
{ "schema": 1, "name": "Club A (wired)", "slug": "club-a-wired",
  "created": "2026-10-09T10:00:00Z", "includeInterface": false,
  "settings": { "mode": "follow", "latencyMs": 1.5, "midi": {}, "osc": {}, "features": {} } }
```

Rules: slug `[a-z0-9-]{1,40}` derived from the name (collision gets `-2`);
max 100 profiles; file max 64 KB; writes are atomic (temp file then rename);
only slugs are ever turned into paths (no traversal possible).

## Operations (`/api/f/profiles/`)

| Route | Body | Result |
|---|---|---|
| `GET list` | | `[{slug,name,created,includeInterface}]`, plus `active`, `dirty` |
| `POST save` | `{name, includeInterface?}` | snapshot of current settings; overwrites same slug only with `overwrite:true` |
| `POST diff` | `{slug}` | list of `{path, current, profile}` changes that Apply would make |
| `POST apply` | `{slug, confirm?}` | applies via `Core::applyConfig`; if the diff changes `interface`, `mode` or `deviceNumber` (network restart) requires `confirm:true` else 409 with the diff |
| `POST rename`, `duplicate`, `delete` | `{slug,...}` | |
| `GET export?slug=x` | | JSON download |
| `POST import` | profile JSON | validates, assigns slug, never overwrites silently |

Validation on import and apply: schema 1 only; values go through
`Settings::fromJson` clamping; unknown keys are dropped and returned in
`warnings`; wrong types are rejected with the JSON path.

## Active and dirty

`active` is the slug last applied (stored in settings raw under
`features.profiles.active`). `dirty` is true when the current settings differ
from the stored profile in the whitelisted subset (computed by the same
`diff`). Both appear in status.

## Actions and CLI

Registry: `profile.next`, `profile.prev` (alphabetical cycle) and, for each
profile, `profile.apply:<slug>` (re-registered when the list changes; applies
only when no restart-causing difference exists, otherwise ignored with a
status warning, because a footswitch must never restart the network).
`shunt_headless --profile slug` applies at startup.

## UI (`#/profiles`, plus a select in the Live control bar)

List with Active badge and Dirty dot; Save current as...; Apply opens an
inline diff view (no modal) with Confirm; Rename, Duplicate, Delete (with
confirm), Export, Import (file input). The Live control bar widget shows the
active profile name and a quick-switch select (disabled while the diff needs
confirmation, which sends the user to the page).

## Tests

- PR-T1 save then apply round trip on a modified settings object.
- PR-T2 diff correctness and the restart-confirmation rule.
- PR-T3 whitelist: bar-offset memory and http bind untouched.
- PR-T4 import: wrong schema, wrong types, oversize, unknown keys warning.
- PR-T5 slug rules, collision suffix, traversal attempts (`../`, absolute, NUL,
  unicode confusables become `-`).
- PR-T6 dirty flag toggles with a setting change.
- PR-T7 atomic write (kill between temp and rename leaves old file valid).
- PR-T8 registry actions per profile appear and disappear with the list.

## Acceptance and effort

Applying a profile in the simulator changes outputs live without restarting
the network unless the diff says so. Effort: M (3 days).
