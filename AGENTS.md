# AGENTS.md — rime-ids-reverse

Librime plugin providing **IDS component search** (部件检字). It answers
queries such as `I木口`, `Ikouhao`, `O木木`, and `O森-木+氵` from a single
memory-mapped `ids_data.bin` file.

There are two interchangeable implementations of the same behavior:

- **C++** in `src/` — the native plugin `librime-ids-reverse`.
- **Lua 5.5** in `ids_reverse.lua` — four `lua_*` components, no compilation.

Keep them behaviorally equivalent. The README and all user-facing text are in
Chinese; preserve that.

## Repository layout

| Path | Responsibility |
|---|---|
| `src/ids_reverse_module.cc` | `RIME_REGISTER_MODULE(ids_reverse)`; registers the four components. |
| `src/ids_reverse_config.{h,cc}` | Shared config loading/defaults, `ids_reverse` namespace, hybrid state property, lazy `IdsStore` loading. |
| `src/ids_store.{h,cc}` | IDS4 data layer: mmap, dense ids, section parsing, tokens, postings, canon/block tables. |
| `src/ids_text.{h,cc}` | UTF-8/IDS helpers, `FormatIds`, block names, candidate comments. |
| `src/ids_reverse_composer.{h,cc}` | `IdsReverseProcessor`, `IdsReverseSegmentor`, `IdsReverseFilter`; all key handling. |
| `src/ids_reverse_translator.{h,cc}` | Translator, I-mode search, O-mode candidate construction, pinyin/shuangpin decoding. |
| `src/ids_reverse_search.{h,cc}` | O-mode `SearchHybrid` (add/subtract/exclude algebra) and component candidates. |
| `src/ids_reverse_group.{h,cc}` | O-mode single-group pending parser and group-component formatting helpers. |
| `ids_reverse.lua` | Whole Lua implementation; exports `processor`/`segmentor`/`translator`/`filter` and `_internal` for tests. |
| `data/` | Generator, raw input tables, ignored outputs. |
| `examples/` | Standalone schema and patch templates. |
| `tests/` | Pure-Lua functional tests and latency benchmark. |
| `README.md` | User documentation (Chinese). |

This directory is its **own git repository**, nested inside librime and ignored
by the parent via `plugins/*/`. Commit plugin changes here, not in the librime
root.

## Build (C++)

Configure from the librime source root. To get a standalone plugin `.so`:

```sh
cmake -S . -B build -DENABLE_EXTERNAL_PLUGINS=ON -DBUILD_MERGED_PLUGINS=OFF
cmake --build build --target rime-ids-reverse -j
# build/lib/rime-plugins/librime-ids-reverse.so
```

- `BUILD_MERGED_PLUGINS` defaults to `ON`; in that mode plugin objects are
  compiled into librime and the target `rime-ids-reverse` **does not exist**.
- With merged plugins plus `BUILD_SEPARATE_LIBS=ON`, the output is
  `build/lib/librime-plugins.so` instead.
- New `.cc` files are collected with `aux_source_directory`; re-run CMake after
  adding or removing source files.
- `RIME_PLUGINS_DIR` (default `rime-plugins`) controls the plugin output
  directory; librime loads separate plugins from there at runtime.

## Lua deployment

No build step. Rime's `require` searches `lua/` under the user and shared data
directories, so install or copy the file to one of:

```text
<user-data-dir>/lua/ids_reverse.lua
<shared-data-dir>/lua/ids_reverse.lua
```

`CMakeLists.txt` installs it to `${CMAKE_INSTALL_FULL_DATADIR}/${RIME_DATA_DIR}/lua`
when `RIME_DATA_DIR` is defined. Use the component forms shown in
`examples/ids_reverse.lua.schema.yaml`; `@ids_reverse` makes the Lua components
read the same `ids_reverse` config node as the C++ components.

Never mount the C++ and Lua component sets in the same schema.

## Tests and verification

```sh
# From the librime source root.
lua5.5 plugins/ids-reverse/tests/test_ids_reverse.lua
lua5.5 plugins/ids-reverse/tests/test_ids_reverse.lua path/to/ids_data.bin
lua5.5 plugins/ids-reverse/tests/bench_ids_reverse.lua
```

- Tests are pure Lua and do not need a running Rime engine, but they do need
  `data/ids_data.bin`. If omitted, the path defaults relative to the script.
- They use `M._internal`; keep that table usable when refactoring.
- There are **no C++ unit tests** in this plugin. Verify C++ changes by loading
  the plugin in a real frontend and running the checks listed in the README
  (I mode, O mode, subtraction/addition/exclusion, variants, hotkeys).

Inspect the binary data with:

```sh
cd plugins/ids-reverse/data
python3 dump_ids_v4.py ids_data.bin
```

## Data generation (IDS4)

```sh
cd plugins/ids-reverse/data
python3 generate_data_v4.py    # requires pypinyin
```

Run it from `data/`; all input paths are relative. It reads `IDS_raw.TXT`,
`ids_zitools.txt`, `ids_lv1.txt`, `EquivalentUnifiedIdeograph.txt`,
`excel_sheet2_pinyin.json`, `ids_variants.txt`, and optionally
`wanxiang_charset.dict.yaml`. It writes `ids_index.txt`, `ids_meta.txt`, and
`ids_data.bin` (~30 MB). These outputs are git-ignored.

- The only supported format is **IDS4 v3** (`IDS4` magic, version 3).
  `PY_CHAR` (section 20) is required; v2/v1 and IDS3 files must be regenerated.
- `PY_CHAR` maps every toneless pinyin (including `pypinyin` heteronyms) to all
  character ids, sorted by frequency. Exact single-syllable candidates are not
  capped; prefix matches keep the caller's limit.
- The generator emits text intermediates and the binary; do not hand-edit any
  of them.
- IDS description operators U+2FF0–U+2FFF (including `⿼⿽⾾⿿`) are stripped from
  LEAVES; the generator also strips its `idc` placeholder/variation markers.
  Keep at least the U+2FF0–U+2FFF range synchronized between the generator,
  `IsIdc` / `is_idc`, and the bracket-format label tables. In particular U+2FFF
  (`⿿`, rotation) is a unary operator, not a component.
- `ids_custom.txt` is optional runtime customization, not generator input. It
  supports `#` comments, `-block`/`-区` to disable blocks, `-pinyin` to delete
  a reading, `部件 拼音...` to add mappings, and `=` to replace mappings.

## Architecture

### Components and configuration

All four components share one namespace (`ids_reverse` by default, overridable
by the ticket namespace). Shared settings live at the top level; only
`prefix`, `tag`, and `tips` are split into `search_*` and `hybrid_*`.

- I mode = **search**: `search_prefix` (default `I`), `search_tag`,
  `search_tips`.
- O mode = **hybrid**: `hybrid_prefix` (default `O`), `hybrid_tag`,
  `hybrid_tips`.

If both prefixes are equal, search mode wins. Config defaults and normalization
live in `ReverseConfig` and `LoadReverseConfig`; mirror them in the Lua
`load_config` defaults.

Load the store lazily through `EnsureIdsStoreLoaded` / `EnsureLoaded`; never
load ~30 MB of data in a component constructor.

### I mode (search)

1. `IdsReverseProcessor` accepts the leading prefix when idle and not in
   `ascii_mode`, pushing it into the context.
2. `IdsReverseSegmentor` tags the whole input with `search_tag`.
3. `IdsReverseTranslator::QuerySearch` strips the prefix. Empty query yields a
   tips-only candidate; otherwise it calls `SearchComponents`.
4. `IdsReverseFilter` suppresses other translators when the input is exactly the
   bare prefix.

`SearchComponents` handles components, full pinyin, and shuangpin; stems are
ANDed, then scored (`ScoreCandidate`) by direct/ordered match, atomic-match
tier, frequency, and depth. Component queries also inject exact top-level leaf
multiset hits from `LEAF_STRUCT` ahead of the normal candidate pool; these hits
receive a score floor so they are not displaced by high-frequency ALTS matches.

### O mode (hybrid)

State lives in the context property `ids_reverse_components`, one
`<component>\t<code>\n` line per accepted component. `ReadHybridState` validates
the input against the prefix and codes and returns the uncommitted `pending`
text; `SaveHybridState` writes it back.

There is **no direct Chinese-character input** in O mode: Chinese characters
enter the composition only when a pinyin/shuangpin candidate is selected. A
selected component replaces its pinyin code in the input and is stored as both
`component` and `code` (so the preedit shows `Oshao` → `O少`). Components picked
as a multi-character combo are stored as one layer; Backspace removes
`len(code) + 1` bytes (the code plus its following quote).

- **Processor**: Return confirms result/multi-char candidates or turns a pending
  code into a component; Backspace removes pending text or the last component;
  `1`–`6` and `;` select; space confirms; `/` starts a variant query;
  `Shift+V` replaces the preedit character with the selected variant;
  letters and `-`/`+`/`!` are pushed as input. `select_notifier` temporarily
  disables `_auto_commit` and commits result candidates itself.
- **Parentheses** are edited incrementally: while `pending` is a single
  `[op]? ( ... )?` expression, selecting a component replaces only the last
  unresolved ASCII pinyin run and does not commit state. When `)` closes the
  outermost group and no ASCII runs remain, the processor evaluates the inner
  expression with `EvaluateAlgebraExpression`, stores the remaining components
  (plus any `!` exclusions) as one component, and pushes a quote.
- **Segmentor**: emits the prefix segment, confirmed component segments, and a
  pending `hanzi_component` segment; operators get confirmed segments.
- **Translator** (`QueryHybrid`), in order: variant-result stage, variant query,
  single-group editor (component candidates for the last ASCII run), results for
  selected components, pending multi-syllable/multi-character candidates,
  single-component candidates, raw component expressions.
- **Filter**: drops non-plugin candidates, deduplicates component expressions,
  and scans at most 2000 candidates to avoid walking huge dictionaries.
- **Search**: `SearchHybrid` applies `-`, `+`/`=`, and `!` algebra with `()`
  grouping, expands pure subtraction to characters containing the remainder,
  matches leaves, then ranks. Exact top-level leaf multiset hits from
  `LEAF_STRUCT` are merged first and scored with the same exact-structure floor
  as I mode. Group parentheses are resolved before state entry, so `SearchHybrid`
  normally receives only paren-free component strings.
- **Group evaluator**: `EvaluateAlgebraExpression` (C++) /
  `evaluate_algebra_expression` (Lua) computes `+`/`-`/`!` and nested
  parentheses over components only, without dictionary lookup. It is used when
  committing a closed group, not by `SearchHybrid`.

Variants use the properties `ids_reverse_variants` (base character) and
`ids_reverse_variants_result` (replacement), plus the internal segment tag
`ids_reverse_result`.

### Data layer

`IdsStore` is a process-wide singleton with an mmap-backed IDS4 image:

- Dense character ids `0..N-1`; `CP2ID` / `ID2CP` translation.
- 24-byte hot record per character: frequency, flags, char token, depth, block,
  and offsets into ALTS/LEAVES/ATOMS.
- ALTS: ordered component variants grouped by length, plus signatures.
- LEAVES / ATOMS: precomputed top-level and atomic component tokens.
- COMPSET: sorted component ids per character.
- Inverted index addressed by token id and multiplicity via `KEY_LOOKUP` /
  `KEY_REC`; `POST` holds a ranked `head` plus the full posting (sorted array,
  or bitmap above 1024 entries).
- `PY` / `TOKENS`: pinyin-to-component and component token tables.
- `PY_CHAR` (section 20, required): toneless pinyin -> all character ids,
  including heteronyms, sorted by frequency; feeds O-mode component candidates.
- `VAR_OFF` / `VAR`: variants; `TOK_CANON` / `ATOMIC_SET`: precomputed sets.
- `LEAF_STRUCT`: sorted `{hash, id}` records for the canonical top-level leaf
  multiset; query by binary search and verify against `LEAVES` to tolerate hash
  collisions.

Section ids 1–20 are declared in `ids_store.h` (`kSec*`), mirrored by `SEC_*`
in `ids_reverse.lua`, and written by `data/generate_data_v4.py`. Any storage
change must update all three and regenerate the binary.

`LEAF_STRUCT` (section 19) is the canonical top-level leaf multiset index. It
maps `(len + sorted canonical leaf tokens) -> character ids` and must stay in
sync across the generator, `IdsStore::LookupLeafStructure`, and
`Store:lookup_leaf_structure`. Exact structure hits are injected ahead of the
normal candidate pool and given a score floor in both O and I modes.

## Keeping C++ and Lua in sync

When changing behavior, update both implementations or explicitly state why only
one changed. Applies to:

- Config keys, defaults, and fallback rules.
- Pinyin segmentation and flypy/zrm shuangpin decoders.
- O-mode component candidate sources, `PY_CHAR` exact/prefix rules, and limits.
- Query parsing, matching, scoring, and result limits.
- Candidate types (`ids_reverse`, `hanzi_result`, `hanzi_component`), text,
  preedit, comments, and quality.
- Processor key handling, properties, and state transitions.
- Filtering/deduplication.

## Gotchas and invariants

- The section enum values are part of the on-disk format.
- Canonical component tables are duplicated in `src/ids_store.cc` (`CanonMap`),
  `data/generate_data_v4.py` (`EQUIV_GROUPS`), and `ids_reverse.lua`
  (`CANON_MAP`). Update them together.
- Block names/aliases appear in `src/ids_store.cc` (`BlockAliasMap`),
  `src/ids_text.cc` and `src/ids_reverse_translator.cc` (`kBlockNames`),
  `ids_reverse.lua` (`BLOCK_NAMES`, `BLOCK_ALIAS_MAP`), and
  `data/dump_ids_v4.py`.
- Property and tag strings are duplicated as `k*` constants in C++ and locals in
  Lua; keep them identical.
- `disabled_blocks` is the union of config values and rules merged from the
  custom file.
- `valid_pinyins` is the standard pinyin set used for full-pinyin segmentation
  and shuangpin validation. Do **not** insert `PY_CHAR` keys into it: PY_CHAR
  includes heteronym/interjection readings such as `m`/`n`, which would make
  `mn` segment as `m + n` instead of decoding to `miao`.
- For flypy/zrm, 2-letter chunks are decoded as shuangpin before full-pinyin
  segmentation; keep this rule when changing `SegmentAlphaString`.
- Relative data/custom paths resolve first in the user data directory, then the
  shared data directory.
- Preserve Chinese prompts and candidate comments.
- Performance target for C++: single-character query < 0.1 ms, multi-component
  query < 1 ms. The Lua version is expected to be slower; avoid adding
  per-keystroke full scans.
- Cached query results must remain valid for the loaded data, config, and
  disabled-block set.

## Where to make changes

| Change | Files |
|---|---|
| Add a config option | `src/ids_reverse_config.{h,cc}`, Lua `load_config`/`default_cfg`, `README.md`, `examples/*.yaml` |
| Add a data section | `data/generate_data_v4.py`, `src/ids_store.h` enum, `ids_store.cc` parse, Lua `SEC_*`/`Store.load`, `data/dump_ids_v4.py` |
| Add or remap a key binding | `src/ids_reverse_composer.cc` (`ProcessKeyEvent`, hotkeys), Lua `processor_func`, README |
| Change candidate output | `src/ids_reverse_translator.cc` (`Build*`, `BuildComment`), `src/ids_text.cc`, Lua `build_*`/`build_comment` |
| Add component alias | C++ `CanonMap`, generator `EQUIV_GROUPS`, Lua `CANON_MAP` |
| Add block name/alias | `ids_store.cc`, both `kBlockNames`, Lua tables, dump script |
| Change search/ranking | `src/ids_reverse_translator.cc` (`SearchComponents`, `ScoreCandidate`) and/or `src/ids_reverse_search.cc` (`SearchHybrid`), plus Lua equivalents |
| Change O-mode group editing | `src/ids_reverse_group.{h,cc}`, composer `OnSelectAfter`/`ProcessKeyEvent`, translator `QueryHybrid`, `EvaluateAlgebraExpression`, plus Lua equivalents |

## Before committing

- Run the Lua functional test.
- If the binary format or data inputs changed, regenerate `ids_data.bin` and
  verify with `dump_ids_v4.py`. IDS4 v3 requires `LEAF_STRUCT` with one record
  per character and a non-empty `PY_CHAR` section.
- If C++ changed, build the target and smoke-test the README scenarios in a
  frontend.
- Do not commit `*.bin`, `ids_index.txt`, `ids_meta.txt`, or generated dump
  files.
