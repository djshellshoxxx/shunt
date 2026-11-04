# Research 04: rekordbox PSSI (song structure / phrase) tag

Status: spike P0 (risk R2) and the PSSI half of N0 (R3), 4 November 2026.
Input to `docs/specs/FS-03-phrase-countdown.md`.

Method. The layout was read from three independent public implementations,
then checked by a clean-room parser written for this spike (about 60 lines of
Python, not committed) run on every `.EXT` file found in two of the
repositories. "Verified" below means: confirmed by that parser on real files.
"Documented" means stated by a source but not exercised by any real file
available here. "Unverified" is said explicitly.

All multi-byte integers are big-endian. Beat numbers are 1-based.

## 1. Sources and licences

Sources were fetched with `git clone --depth 1` / raw.githubusercontent.com
on 2026-11-04. Commit ids are the shallow-clone heads.

| Source | URL | Commit | Licence | Use rule for Shunt |
|---|---|---|---|---|
| Deep Symmetry crate-digger (ANLZ doc, Kaitai spec) | https://github.com/Deep-Symmetry/crate-digger | main (shallow) | repo `LICENSE` is **EPL-2.0** (https://github.com/Deep-Symmetry/crate-digger/blob/main/LICENSE). The `.ksy` header says `license: EPL-2.0 OR MPL-2.0 OR LGPL-3.0-only` (https://github.com/Deep-Symmetry/crate-digger/blob/main/src/main/kaitai/rekordbox_anlz.ksy) | Facts only. No code, no copied prose. |
| Doc text for the tag | https://github.com/Deep-Symmetry/crate-digger/blob/main/doc/modules/ROOT/pages/anlz.adoc (section "Song Structure Tag", credits Michael Ganss for the analysis) ; rendered at https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/anlz.html#song-structure-tag (rendered site not fetched, blocked) | | EPL-2.0 (same repo) | Facts only. |
| Deep Symmetry dysentery (network side) | https://github.com/Deep-Symmetry/dysentery | 7f61b1c | EPL-2.0 (`LICENSE`) | Facts only. |
| Deep Symmetry beat-link | https://github.com/Deep-Symmetry/beat-link | 1483c00 | EPL-2.0 (`LICENSE`) | Facts only. |
| pyrekordbox (Python, `pyrekordbox/anlz/{structs,file,tags}.py`, docs, tests) | https://github.com/dylanljones/pyrekordbox | 02a2088 | **MIT** (`LICENSE`, "Copyright (c) 2022-2025, Dylan Jones") | Code could be reused with notice, but we write clean-room C++ anyway. |
| rekordcrate (Rust, `src/anlz.rs`) | https://github.com/Holzhaus/rekordcrate | 68cd702 | **MPL-2.0** (`COPYING`, `Cargo.toml` `license = "MPL-2.0"`, README "Mozilla Public License 2.0"). Not MIT/Apache. | MPL is file-level copyleft: do not paste its code into Shunt; facts and reading it are fine. |

Rule for the implementer: write `AnlzPssi.cpp` from section 3 of this note
(facts), not from any source file. EPL material (crate-digger, dysentery,
beat-link) is a fact source only.

## 2. Where the tag lives

- `PSSI` is in the `.EXT` analysis file only (crate-digger `.ksy` enum comment
  "PSSI (seen in .EXT)"; pyrekordbox docs list `.EXT: PPTH, PCOB, PCOB, PCO2,
  PCO2, PQT2, PWV3, PWV4, PWV5, PVB2, PSSI`:
  https://github.com/dylanljones/pyrekordbox/blob/master/docs/source/formats/anlz.md).
- It exists only when the DJ ran phrase analysis in rekordbox 6 or later and
  exported (crate-digger anlz.adoc, "starting with rekordbox version 6 it also
  gets exported to external media so CDJ-3000 players can use it to control
  lighting looks"). Verified: most of the `.EXT` files in the two repos (all the
  rekordbox 5 ones, and several rekordbox 6 ones) have no PSSI tag at all (tag order in the files that have it: PPTH, PWV3, PCOB,
  PCOB, PCO2, PCO2, PQT2, PWV5, PWV4, PSSI). So `shunt_anlz` must treat "no
  PSSI" as a normal result, not an error.
- File structure: `PMAI` header (`len_header` u32 at 4, `len_file` u32 at 8),
  then tags each with a 12-byte common header: fourcc (4), `len_header` (u32),
  `len_tag` (u32, whole tag including header). Walk tags by `len_tag`.
  Verified.

## 3. Tag layout (verified on 5 real tags, 3 distinct songs)

Offsets are from the start of the tag (the `P` of `PSSI`).

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x00 | 4 | fourcc `PSSI` | 0x50535349 |
| 0x04 | 4 | `len_header` | always 0x20 (32) in all real files |
| 0x08 | 4 | `len_tag` | verified equal to `32 + 24 * len_entries` in all 5 tags (no trailing bytes) |
| 0x0C | 4 | `len_entry_bytes` | 0x00000018 (24) in all 5 tags. See disagreement D1 (pyrekordbox reads this as u16 version + u16 size) |
| 0x10 | 2 | `len_entries` | number of phrases. **Not masked.** Used as the mask offset |
| 0x12 | 2 | `mood` | 1 high, 2 mid, 3 low. **First masked byte.** Everything from 0x12 to `len_tag` is masked in newer exports |
| 0x14 | 6 | unknown | all zero in the 5 tags |
| 0x1A | 2 | `end_beat` | beat number where the last recognised phrase ends. Track may continue (silence) |
| 0x1C | 2 | unknown | `01 00` in the two mid-mood songs, `00 00` in the high-mood song (observation, meaning unknown) |
| 0x1E | 1 | `bank` | Lighting bank: 0 default (treated as Cool), 1 Cool, 2 Natural, 3 Hot, 4 Subtle, 5 Warm, 6 Vivid, 7 Club 1, 8 Club 2. Unexpected values `f6`, `f9` have been seen elsewhere (crate-digger doc), so do not reject on unknown bank. All 5 tags here have 0 |
| 0x1F | 1 | unknown | 0 |
| 0x20 | 24 * n | entries | |

Source for the field list: crate-digger anlz.adoc (header bytes 10-11
`len_e`, 12-13 mood, 14-19 unknown, 1a-1b end beat, 1c-1d unknown, 1e bank,
entries at 20) and the `.ksy` (`song_structure_tag`, `song_structure_body`);
independently matched by pyrekordbox `structs.py` `PSSI` and rekordcrate
`SongStructure`/`SongStructureData`.

### Entry (24 bytes; offsets from entry start)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 2 | `index` | phrase number, starts at 1, increments (verified 1..n in all tags) |
| 2 | 2 | `beat` | first beat of the phrase (1-based). The phrase runs to the next entry's `beat`, or to `end_beat` for the last one |
| 4 | 2 | `kind` | phrase kind id, meaning depends on mood (section 5) |
| 6 | 1 | unknown | 0 |
| 7 | 1 | `k1` | high-mood variant flag |
| 8 | 1 | unknown | 0 |
| 9 | 1 | `k2` | high-mood variant flag |
| 10 | 1 | unknown | 0 |
| 11 | 1 | `b` | 0: only `beat2` valid; 1: `beat2`, `beat3`, `beat4` valid (only on high-mood "Up 3") |
| 12 | 2 | `beat2` | extra beat inside the phrase (lighting hint), else 0 |
| 14 | 2 | `beat3` | as above |
| 16 | 2 | `beat4` | as above |
| 18 | 1 | unknown | 0 |
| 19 | 1 | `k3` | high-mood variant flag |
| 20 | 1 | unknown | 0 |
| 21 | 1 | `fill` | non-zero: there is a fill-in at the end of the phrase |
| 22 | 2 | `beat_fill` | beat where the fill starts (valid if `fill` != 0) |

Verified details: in the high-mood file entry 2 (Up) has k2=1, b=1 and
beat2/3/4 = 35/51/59, all inside the phrase 35..67, increasing, which matches
the doc's "Up 3" rule. Entry 5 of the same file has `fill`=1 and
`beat_fill`=127 for a phrase starting at 115 and ending at 131, i.e. fill
starts 4 beats before the next phrase, matching the rekordbox manual quote in
the crate-digger doc ("up to 4 beats").

For FS-03 only `beat` and `kind` (plus mood and `end_beat`) are needed.
`beat_fill` is a free bonus (a "fill coming" warning); the doc says a fill is
detected at the end of Intro, Up and Chorus.

## 4. Obfuscation ("XOR mask")

Documented identically by three sources and verified on real files.

- Applies to the tag body from offset 0x12 (the `mood` field) to the end of
  the tag. `len_entries` at 0x10 stays in clear. (crate-digger anlz.adoc note:
  "All bytes after len_e (bytes 10-11) are XOR-masked"; `.ksy` `process:
  xor(is_masked ? mask : [0])` on the `size-eos` body.)
- Base pattern, 19 bytes:
  `CB E1 EE FA E5 EE AD EE E9 D2 E9 EB E1 E9 F3 E8 E9 F4 E1`.
  Same bytes in crate-digger doc and `.ksy`, pyrekordbox `XOR_MASK`
  (https://github.com/dylanljones/pyrekordbox/blob/master/pyrekordbox/anlz/file.py)
  and rekordcrate `KEY_DATA`. pyrekordbox notes the pattern equals the ASCII
  string "Kanzen-niRikaishita" with 0x80 added to each byte (checked: 'K' 0x4B
  + 0x80 = 0xCB).
- Key byte i = `(base[i mod 19] + len_entries) mod 256`. Plain
  addition, wrap at 256. The key restarts at base index 0 at the first masked
  byte (offset 0x12) and cycles every 19 bytes through the whole body
  including the entries.
- Unmasking = XOR each byte at `0x12 + x` with `key[x mod 19]`. Masking is the
  same operation.
- Detecting a masked tag (three variants, equivalent on every real file):
  - crate-digger `.ksy`: `raw_mood > 20` means masked.
  - pyrekordbox: masked unless `1 <= mood <= 3`.
  - rekordcrate: XOR the first two bytes with the key, accept if the result is
    a valid `Mood`.
  Recommended for Shunt: masked iff the raw u16 at 0x12 is not in 1..3; after
  unmasking, require mood in 1..3 and `len_tag == 32 + 24*n`, else report
  "unsupported PSSI" and skip the file.
- Worked check (file with 19 entries): key[0..1] = CB+13, E1+13 = DE F4;
  stored mood bytes `DE F6` = 57078 raw; XOR gives `00 02` = mid. Verified.

Cross-check that proves the mask on real data: two pairs exist where the same
song appears once masked (rekordbox export, "USBANLZ") and once unmasked
(rekordbox 6 desktop "backup" folder). After applying the mask above to the
masked file, all 19 (resp. 11) entries and the end beat are byte-for-byte equal
to the unmasked file's. The unmasked rekordbox 6 desktop files show the
PSSI is only obfuscated on export, matching the doc.

## 5. Mood-dependent kind tables

`mood` 1 = high, 2 = mid, 3 = low (crate-digger doc and `.ksy` `track_mood`;
same in pyrekordbox docs and rekordcrate `Mood`). An unknown mood: the `.ksy`
falls back to the mid table; rekordcrate and pyrekordbox's docs have no
fallback. Recommended: reject unknown moods.

### Raw ids per mood

| id | low | mid | high |
|---|---|---|---|
| 1 | Intro | Intro | Intro (1 or 2, see below) |
| 2 | Verse 1 | Verse 1 | Up (1, 2 or 3) |
| 3 | Verse 1 | Verse 2 | Down |
| 4 | Verse 1 | Verse 3 | (not defined) |
| 5 | Verse 2 | Verse 4 | Chorus (1 or 2) |
| 6 | Verse 2 | Verse 5 | Outro (1 or 2) |
| 7 | Verse 2 | Verse 6 | (not defined) |
| 8 | Bridge | Bridge | (not defined) |
| 9 | Chorus | Chorus | (not defined) |
| 10 | Outro | Outro | (not defined) |

Low mood: ids 2-4 are three internal variants of "Verse 1" and ids 5-7 of
"Verse 2"; rekordbox shows no difference (crate-digger doc; `.ksy` names them
`verse_1`, `verse_1b`, `verse_1c`, `verse_2`, `verse_2b`, `verse_2c`).

High-mood variants (flags; crate-digger doc "High mood phrase variants";
verified on the one high-mood file):

| id | k1 | k2 | k3 | Label |
|---|---|---|---|---|
| 1 | 1 | | | Intro 1 |
| 1 | 0 | | | Intro 2 |
| 2 | | 0 | 0 | Up 1 |
| 2 | | 0 | 1 | Up 2 |
| 2 | | 1 | 0 | Up 3 |
| 3 | | | | Down |
| 5 | 1 | | | Chorus 1 |
| 5 | 0 | | | Chorus 2 |
| 6 | 1 | | | Outro 1 |
| 6 | 0 | | | Outro 2 |

(Combination k2=1,k3=1 is not defined in the doc.) In the sample file: id 1
k1=0 gave Intro 2; id 5 k1=1 gave Chorus 1; id 6 k1=0 gave Outro 2.

### Mapping to FS-03's normalised strings

FS-03 stores `intro|verse|bridge|up|down|chorus|outro`. The mapping that
follows from the tables:

- high: 1 intro, 2 up, 3 down, 5 chorus, 6 outro; ids 4 and 7+ unknown.
- mid: 1 intro, 2..7 verse, 8 bridge, 9 chorus, 10 outro.
- low: 1 intro, 2..7 verse, 8 bridge, 9 chorus, 10 outro.
- Unknown id: keep as `unknown`, do not drop the phrase (it still bounds the
  countdown).

Real-data observation that FS-03 should respect: a mid-mood song can open with
a **chorus** at beat 1 (id 9 at beat 1 in one of the demo tracks), and a mid
song can have three consecutive Verse-2 style phrases then a long run of
chorus phrases (ids 9 repeated). Do not assume the first phrase is an intro or
that kinds alternate.

## 6. Real parsed data (for the tests)

Output of the clean-room parser. Entries are `beat:kind`.

- Song A (mid, 19 phrases, end 353, bank 0):
  1:9, 37:2, 73:2, 121:3, 129:4, 141:5, 161:4, 173:5, 193:6, 201:8, 209:6,
  225:6, 241:8, 257:9, 273:9, 289:9, 305:9, 321:9, 337:9.
- Song B (mid, 11 phrases, end 257, bank 0):
  1:1, 33:2, 61:3, 93:3, 109:4, 125:8, 157:8, 173:5, 189:6, 205:5, 221:10.
- Song C (high, 8 phrases, end 257, bank 0):
  18:1, 35:2 (k2=1 b=1 beats 35/51/59), 67:5 (k1=1), 87:3, 115:2 (fill at
  127), 131:2, 163:3, 175:6.
  Note the high-mood file's first phrase starts at beat 18, not 1.

Beat convention: first phrase beat is 1 in both mid songs, so beats are
1-based and match the 1-based `beatNumber` of the status packet that FS-03
uses (the status offset 0xa0 claim itself lives in research 01 and is not
re-verified here).

`end_beat` semantic: documented as "the beat number at which the last
recognized phrase ends". FS-03 currently computes the last phrase's end as
`totalBeats + 1`. Use `end_beat` directly as the end boundary of the last
phrase and as `totalBeats` in the JSON; do not add 1 (see open point O3).

## 7. Disagreements between implementations and with FS-03

- **D1. Bytes 0x0C-0x0F.** crate-digger (`len_entry_bytes: u4`), rekordcrate
  (`len_entry_bytes: u32`, asserted == 24) treat it as the u32 24.
  pyrekordbox (current master, CHANGELOG entries "Parse PSSI version 1 song
  structure tags #217", "Parse PSSI version and entry size separately") reads
  a u16 `version` then a u16 entry size (Const 24), and says "version 1 is
  00 01 00 18". Evidence for a version-1 tag: only pyrekordbox's own
  synthetic unit tests (`tests/test_anlz.py`, `_build_pssi`); **no real file
  with version != 0 was found** (all 5 real tags have 00 00 00 18).
  **Unverified.** Safe handling: read u16 version + u16 entry size; require
  entry size 24; accept version 0; for version != 0 either refuse or apply
  D2; log it.
- **D2. Mask start index.** crate-digger and rekordcrate: key always starts at
  base index 0. pyrekordbox: starts at `version % 19`, i.e. identical for
  version 0, rotated for version 1 (claims "Version 1 exports rotate the mask").
  Unverified for version 1 (no real sample). Version 0 verified.
- **D3. Masked detection rule.** `raw_mood > 20` (Kaitai) vs `not 1..3`
  (pyrekordbox) vs trial decode (rekordcrate). Same result on all real data.
  Differ only for corrupt files.
- **D4. Strictness on bank.** The Deep Symmetry doc says `bank` values `f6`
  and `f9` have been seen; the `.ksy` keeps `raw_bank` and makes the enum
  optional. rekordcrate's `Bank` is a strict `repr(u8)` enum (would fail on
  those values); pyrekordbox keeps a plain int. Shunt should not parse bank
  strictly (or at all).
- **D5. Low-mood labels vs FS-03.** FS-03 says "Mid and low mood: intro,
  verse 1..6, bridge, chorus, outro". Per all sources low mood has only Verse 1
  and Verse 2 (three raw ids each). Harmless for the normalised strings
  (both collapse to `verse`), but the UI text "Verse n" must not be derived
  from the raw id in low mood.
- **D6. High-mood id 4 and 7+.** Not defined by any source (doc table leaves
  blank, `.ksy` enum has 1,2,3,5,6 only). Handle as unknown.
- **D7. `totalBeats + 1` (FS-03) vs `end_beat`.** See section 6.
- Entry byte layout and header layout otherwise agree exactly between
  crate-digger doc, `.ksy`, pyrekordbox `structs.py` and rekordcrate
  `anlz.rs` (field by field, including the k1/k2/k3, b, beat2-4, fill and
  beat_fill positions).

Mask-formula note: `.ksy` uses signed `as<s1>` conversions of `(base+c)`; this
is the same as taking the sum modulo 256.

## 8. Fixtures for FS-03 (PH-T5 wants three real files)

Files found, all `.EXT` with PSSI (sha256 prefix of whole file; PSSI tag
offset and length):

| # | Repo path | Mood | Masked | Phrases | Size | sha256 (first 16) |
|---|---|---|---|---|---|---|
| 1 | rekordcrate `data/anlz/sweep/ANLZ0000.EXT` | high | yes | 8 | 75792 | 80ca6a0b01c60ce4 (tag at 75568, 224 B) |
| 2 | rekordcrate `data/complete_export/demo_tracks/PIONEER/USBANLZ/P053/0001D21F/ANLZ0000.EXT` | mid | yes | 11 | 65998 | 837ac9acbcf19b19 (tag at 65702, 296 B) |
| 3 | rekordcrate `data/complete_export/demo_tracks/PIONEER/USBANLZ/P016/0000875E/ANLZ0000.EXT` | mid | yes | 19 | 86386 | e5fbc932a0f22869 (tag at 85898, 488 B) |
| 4 | pyrekordbox `.testdata/rekordbox 6/backup/share/PIONEER/USBANLZ/36d/ad2b4-5d4e-4b94-9a5d-a5bb241aee4b/ANLZ0000.EXT` | mid (same song as #2) | no | 11 | 65932 | 366b61509291638c |
| 5 | pyrekordbox `.testdata/rekordbox 6/backup/share/PIONEER/USBANLZ/e35/fa187-3f34-47e2-9880-2b33cb8d1304/ANLZ0000.EXT` | mid (same song as #3) | no | 19 | 86320 | 14c8a696889f177b |

(pyrekordbox's `.testdata/export/.../P016/0000875E` and `P053/0001D21F` are
byte-identical to rekordcrate's #3 and #2: same md5, i.e. the two repos share
the same files.)

So the repos give: 1 high mood and 2 mid mood songs, each of the mid songs
also as a masked and an unmasked twin (good for PH-T5 "masked and unmasked"),
and **no low-mood sample anywhere**. A low-mood fixture must be hand built
(unit-test synthetic, as pyrekordbox does) or come from a user's own library;
label it synthetic.

Licence and provenance. Neither repo carries a separate licence file for test
data; repository-level licences apply (rekordcrate MPL-2.0; pyrekordbox MIT).
Provenance caveats, all unverified for permission:
- #1 is analysis of a frequency sweep produced by the rekordcrate author
  (README in `data/anlz/sweep/`: "frequency sweep from 20kHz to 20Hz, three
  times"; filename `sweep.wav`, artist "UnknownArtist"). Cleanest provenance.
- #2, #3 (and #4, #5) are analysis files of "Demo Track 1/2" by Loopmasters
  ("Tracks by www.loopmasters.com" in rekordcrate
  `src/pdb/test_roundtrip.rs`). Only analysis data, no audio, is in the repos,
  but the audio's owner is a third party and no permission statement was
  found. The files embed the track path in a `PPTH` tag
  (`/Contents/Loopmasters/UnknownAlbum/Demo Track N.mp3`) and carry
  waveforms.
- The licence of each repo's data is therefore "repo licence by default, not
  stated for data". MPL-2.0 file-level copyleft would attach to an unmodified
  copy of #1-#3.

Recommended fixture plan (decision for FS-03 owner):
1. Do not vendor whole files. Extract only the 12+ bytes `PMAI` header and the
   `PSSI` tag (224, 296 and 488 bytes), re-wrapped in a minimal valid PMAI
   file. That removes the `PPTH` path and the waveforms ("anonymised"),
   and the beats and kinds are facts about a rhythm structure rather than the
   audio. Record the source repo, path and sha256 in a README beside the
   fixtures and keep the upstream copyright notice.
2. Use #1 (high, masked, includes Up 3 beats and a fill), #3 (mid, masked,
   19 phrases, starts with a chorus) and one **unmasked** mid (#5 or #4);
   that covers masked, unmasked and two moods. Add a synthetic low-mood tag
   built by `FixtureBuilder` for the third mood.
3. If the project does not want any third-party-derived bytes, generate all
   fixtures synthetically with the mask in section 4 and keep #1-#5 only as an
   optional, not committed, local conformance check.

## 9. Does a CDJ-3000 broadcast phrase info on the network? (N0, desk research)

Conclusion from public analysis: **no broadcast was found; the data is pulled
on request, not pushed.** Hardware capture is still needed to confirm for
Shunt's own use.

- Players do not broadcast PSSI. The Deep Symmetry protocol analysis
  describes obtaining it with a **dbserver "analysis tag request"**: message
  type `2c04` asking for tag `PSSI` (1230197584) from the `EXT` file of a
  given rekordbox track id and slot, answered with a `4f02` message whose
  fourth argument is the raw tag bytes "as described in the raw file"
  (https://github.com/Deep-Symmetry/dysentery/blob/main/doc/modules/ROOT/pages/track_metadata.adoc,
  section "Requesting Song Structure (Phrase Information)"). That is a TCP
  request to the player's dbserver (research 01 covers the dbserver port),
  not UDP 50002. Beat Link's changelog says it added "support for obtaining
  song structure (phrase analysis) information from players, now that
  rekordbox 6 and later export it"
  (https://github.com/Deep-Symmetry/beat-link/blob/main/CHANGELOG.md).
- Consequences: (a) the data returned over dbserver is whatever the `EXT`
  file holds, so the same masked or unmasked bytes and the same parser apply
  (the doc says "as described in the raw file"; whether the player un-masks
  before replying is **not stated**, handle both); (b) reading it makes Shunt
  a dbserver client, which collides with the 2025 advisory rule in FS-03
  ("Shunt never reads a player's USB or NFS") only for NFS/USB, not
  dbserver, but it still needs a device number 1-4 (1-6 on CDJ-3000) per the
  dysentery vcdj note; that is a design decision for N0/FS-03 source B.
- The CDJ-3000 status packet (200 bytes) and the precise-position packets
  carry beat number and position but the searched Deep Symmetry pages do
  not describe a phrase field in them (searched `vcdj.adoc`, `beats.adoc`,
  `packets.adoc`; no phrase hit other than the PSSI request above).
  Absence in documentation is not proof; **unverified without a capture**.
- Opus Quad and rekordbox-Lighting style devices use a different channel:
  Beat Link's `VirtualRekordbox` (source file
  `src/main/java/org/deepsymmetry/beatlink/VirtualRekordbox.java`) sends a
  UDP request to port 50002 imitating rekordbox's lighting app and the Opus
  Quad answers with UDP packets (type byte `0x0a` at packet offset 0x25) that
  are reassembled into a PSSI blob. The code notes the length of the reply is
  unknown and trailing zeros are trimmed. So for Opus the phrase data is also
  request/response, on UDP 50002. Credit in the Deep Symmetry packets page to
  Kyle Awayan's analysis
  (https://github.com/kyleawayan/opus-quad-pro-dj-link-analysis) and the
  `@cprepos` Beat Link work
  (https://github.com/Deep-Symmetry/dysentery/blob/main/doc/modules/ROOT/pages/packets.adoc).
- Beat Link Trigger consumes phrase info as "Phrase Triggers" since version 7
  (https://github.com/Deep-Symmetry/beat-link-trigger/blob/main/doc/modules/ROOT/pages/Shows_PhraseTriggers.adoc),
  confirming rekordbox-side phrase data is usable by a network client, which
  validates the idea of FS-03 source B but still not "broadcast".
- Suggested N0 capture: with a CDJ-3000 pair loading phrase-analysed tracks
  from USB, capture ports 50000-50002 and the dbserver TCP flow for 60 s while
  loading and playing; search for fourcc `PSSI` / the 1230197584 constant and
  for the PSSI header bytes `00 00 00 18` followed by a plausible entry
  count.

## 10. What this means for FS-03

1. The parser can be implemented now from section 3 and 4 with confidence for
   version-0 tags (verified on 3 distinct songs, masked and unmasked).
2. Add to PH-T5 an explicit case for: masked file, unmasked file, no PSSI
   (empty result), `len_tag` mismatch (reject), unknown mood (reject), bank
   value out of range (accept).
3. Countdown math: use `end_beat` as the last phrase's end; do not assume the
   first phrase begins at beat 1 (the high-mood sample starts at 18) or at
   an intro. Before the first phrase beat treat the state as "pre-phrase",
   counting down to the first phrase.
4. Optional "fill" warning from `beat_fill`.
5. Remaining open points: O1 PSSI version != 0 (D1, D2); O2 low-mood sample;
   O3 whether `end_beat` is exclusive and relation to the beat-grid length
   (needs a PQT2/PQTZ comparison on a real file, not done here); O4 whether a
   CDJ-3000 returns the tag masked over dbserver; O5 permission status of the
   Loopmasters-derived analysis data.

## Sources (all fetched 2026-11-04 unless noted)

- https://github.com/Deep-Symmetry/crate-digger (anlz.adoc, rekordbox_anlz.ksy, LICENSE)
- https://github.com/Deep-Symmetry/dysentery (track_metadata.adoc, packets.adoc, vcdj.adoc)
- https://github.com/Deep-Symmetry/beat-link (CHANGELOG.md, VirtualRekordbox.java)
- https://github.com/Deep-Symmetry/beat-link-trigger (Shows_PhraseTriggers.adoc)
- https://github.com/dylanljones/pyrekordbox (anlz/structs.py, anlz/file.py, anlz/tags.py, docs/source/formats/anlz.md, CHANGELOG.md, tests/test_anlz.py, .testdata/)
- https://github.com/Holzhaus/rekordcrate (src/anlz.rs, data/, COPYING, README.md)
- Not reachable from the research environment (listed so nobody re-tries):
  djl-analysis.deepsymmetry.org (proxy), rekordbox.com manuals.
