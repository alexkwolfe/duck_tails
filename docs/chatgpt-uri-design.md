# GIT_URI Custom Type Design (v4)

> **Formatting note:** This file is wrapped in **quadruple** fences so inner ``` code fences render correctly. Inline backticks are literal inside this outer block.

---

## Overview

Define a **typed, canonical Git selector** for DuckDB so queries can pass a repo + revision (or range) + pathspec **as data**, not ad‑hoc strings. The type:

- Accepts ergonomic **shorthand** (e.g., `#HEAD:README.md`) on input
- Normalizes to a stable, **key–value canonical** fragment (e.g., `#ref=expr:HEAD;path=README.md`)
- Exposes **typed access** to refs, ranges, and path filters in SQL
- Carries **execution options** (`submodules`, `encoding`, `errors`, `depth_commits`, `depth_tree`)
- Defers any repo-graph work (e.g., **merge-bases for `A...B`**) to **execution time**, not parse time

No tables are required—this is a **custom logical type** plus casts/UDFs (including LATERAL-friendly `_each` functions).

---

## Canonical vs. Shorthand

**Canonical fragment (recommended for storage/printing):**
```
#ref=expr:<rev>[;path=<pathspec>...]
#ref=oid:<40|64-hex>[;path=<pathspec>...]
#range=<range-expr>[;path=<pathspec>...]
[;submodules=<ignore|list|recurse>]
[;encoding=<name>][;errors=<strict|replace>]
[;depth_commits=<N>][;depth_tree=<N>]
```

**Shorthand (accepted on input for ergonomics):**
```
#<rev>[:<pathspec>]            -- e.g., #HEAD:README.md
#A..B[:<pathspec>]             -- two-dot range
#A...B[:<pathspec>]            -- three-dot (symmetric difference)
```

**Canonicalization rules (summary):**
1. Normalize repo URL: accept `git+file://`, `git+ssh://`, `git+https://`; convert SCP `git@host:org/repo.git` → `git+ssh://git@host/org/repo.git`. Normalize `file://Users/...` → `file:///Users/...`.
2. If the fragment contains `=` → treat as canonical; else parse shorthand:
    - `<rev>[:path]` → `ref=expr:<rev>[;path=…]`
    - `A..B[:path]` / `A...B[:path]` → `range=<expr>[;path=…]`
3. **Do not** compute merge-bases at parse time. Keep `A...B` as a `range_expr` string for later evaluation.
4. Percent‑encode reserved characters when printing (`^`, `{`, `}`, space, `#`, `;`, `&`).

---

## Type System (DuckDB)

### Atom: `git_ref`
```sql
CREATE TYPE git_ref AS UNION(oid BLOB, ref VARCHAR, expr VARCHAR);
/* oid = raw 20/32 bytes; ref = full refname (refs/heads/main);
   expr = gitrevisions string (HEAD~2, v1^{}, feature@{yesterday}) */
```

### Revision Set / Range
```sql
-- For two-dot and '^X Y' style:
CREATE TYPE git_rev_term AS STRUCT(include BOOLEAN, ref git_ref);
CREATE TYPE git_revset   AS LIST(git_rev_term);

-- Selector carries either a single ref, a plus/minus revset, or a raw range expression (e.g., 'A...B').
CREATE TYPE git_selector AS STRUCT(
  sel  UNION(ref git_ref, revset git_revset, range_expr VARCHAR),
  path git_pathspec,
  opts git_opts
);
```

### Pathspec
```sql
CREATE TYPE git_pathspec_mode AS ENUM('literal','glob','icase');

CREATE TYPE git_pathspec AS STRUCT(
  include LIST(VARCHAR),   -- e.g., ['README.md', ':(glob)src/**/*.ts']
  exclude LIST(VARCHAR),   -- e.g., [':(glob)**/*_test.go']
  mode    git_pathspec_mode
);
```

### Execution Options (with **defaults**)
```sql
CREATE TYPE git_opts AS STRUCT(
  submodules    VARCHAR,   -- 'ignore'|'list'|'recurse' | NULL (unspecified -> libgit default)
  encoding      VARCHAR,   -- e.g., 'utf-8' | NULL (unspecified -> return bytes)
  errors        VARCHAR,   -- 'strict'|'replace' | NULL (executor default, typically 'replace')
  depth_commits INTEGER,   -- NULL = unlimited
  depth_tree    INTEGER    -- NULL = unlimited
);
```

**Defaulting policy (critical):**
- **Unspecified (`NULL`) means “let the underlying Git library choose its default.”**
    - `submodules=NULL` → typically **no recursion**; gitlinks appear as opaque (mode `160000`).
    - `encoding=NULL` → functions return **raw bytes**; if you request decoding, set `encoding` and (optionally) `errors`.
    - `depth_commits=NULL` / `depth_tree=NULL` → unlimited traversal (executor may impose safety limits).

### Full URI container
```sql
CREATE TYPE git_uri AS STRUCT(
  repo_url VARCHAR,     -- git+file://… | git+ssh://… | git+https://…
  selector git_selector
);
```

---

## Function Design: Scalar + LATERAL-friendly `_each`

We follow the **`*_each` companion pattern** for every scalar and table function to support **per-row application** via `LATERAL` joins and to make “in/out” usage explicit.

### Naming pattern

- For a **scalar** `f(x) → y`, provide:
    - `f(x) → y` (scalar)
    - `f_each(x) → TABLE(input <type(x)>, value <type(y)>)`

- For a **table** `g(x) → TABLE(...)`, provide:
    - `g(x) → TABLE(...)` (table)
    - `g_each(x) → TABLE(input <type(x)>, ...)` (same output plus first column echoes the input)

> Rationale: the `_each` variant always **echoes the input** so users can LATERAL-join and still reference the originating value without extra projection.

### Core casts & their `_each` wrappers

Even though we support **casts**, we keep parse/format functions for clarity, portability (some clients don’t like explicit casts), optional flags, and the `_each` pattern.

```sql
-- Casts
CREATE FUNCTION git_uri_parse(s VARCHAR) RETURNS git_uri;
CREATE CAST (VARCHAR AS git_uri) WITH FUNCTION git_uri_parse;

CREATE FUNCTION git_uri_format(u git_uri) RETURNS VARCHAR;
CREATE CAST (git_uri AS VARCHAR) WITH FUNCTION git_uri_format;

-- LATERAL-friendly wrappers
CREATE TABLE FUNCTION git_uri_parse_each(s VARCHAR)
RETURNS TABLE(input VARCHAR, value git_uri);

CREATE TABLE FUNCTION git_uri_format_each(u git_uri)
RETURNS TABLE(input git_uri, value VARCHAR);
```

**Example usage**
```sql
-- Scalar cast is concise
SELECT raw::git_uri AS uri FROM t;

-- LATERAL form keeps the original raw string next to the parsed value
SELECT e.input AS raw, e.value AS uri
FROM t
JOIN LATERAL git_uri_parse_each(t.raw) AS e ON TRUE;
```

### Building-block parsers (exposure policy)

We have lower-level helpers:

```sql
-- Building blocks
CREATE FUNCTION git_ref_parse(s VARCHAR)        RETURNS git_ref;
CREATE FUNCTION git_revset_parse(s VARCHAR)     RETURNS git_revset;
CREATE FUNCTION git_ref_format(r git_ref)       RETURNS VARCHAR;
CREATE FUNCTION git_revset_format(v git_revset) RETURNS VARCHAR;

-- LATERAL wrappers
CREATE TABLE FUNCTION git_ref_parse_each(s VARCHAR)
RETURNS TABLE(input VARCHAR, value git_ref);

CREATE TABLE FUNCTION git_revset_parse_each(s VARCHAR)
RETURNS TABLE(input VARCHAR, value git_revset);
```

**Should we expose these?**
- **Yes, but as “expert API.”** Two options:
    1) Put them under a namespace (e.g., `gitx.git_ref_parse`) and document as *advanced/experimental*.
    2) Gate behind a pragma: `PRAGMA git.enable_expert_api=1;` (register functions on demand).

They are invaluable for power users (e.g., programmatically normalizing refs/ranges) while keeping the primary surface small.

---

## Repo-aware helpers (verification & evaluation)

Parsing is **syntax-only**. Anything that needs the repo happens at execution.

```sql
-- Verify & resolve (scalar + _each)
CREATE FUNCTION git_uri_verify(u git_uri)
RETURNS STRUCT(ok BOOLEAN, message VARCHAR,
               resolved_ref git_ref,           -- oid arm when sel=ref and resolvable
               commits LIST(BLOB));            -- when sel=revset or range_expr

CREATE TABLE FUNCTION git_uri_verify_each(u git_uri)
RETURNS TABLE(input git_uri,
              ok BOOLEAN, message VARCHAR, resolved_ref git_ref, commits LIST(BLOB));

-- Commit enumeration (two-dot / '^X Y')
CREATE FUNCTION git_revset_commits(repo VARCHAR, v git_revset, depth_commits INTEGER)
RETURNS LIST(BLOB);

CREATE TABLE FUNCTION git_revset_commits_each(repo VARCHAR, v git_revset, depth_commits INTEGER)
RETURNS TABLE(input_repo VARCHAR, input git_revset, value LIST(BLOB));

-- Expand 'A...B' at execution time
CREATE FUNCTION git_range_expr_commits(repo VARCHAR, s VARCHAR, depth_commits INTEGER)
RETURNS LIST(BLOB);

CREATE TABLE FUNCTION git_range_expr_commits_each(repo VARCHAR, s VARCHAR, depth_commits INTEGER)
RETURNS TABLE(input_repo VARCHAR, input VARCHAR, value LIST(BLOB));
```

**Abbreviated OIDs**
- Accept in input for ergonomics (short hex treated as `expr` until resolved).
- Resolution upgrades to `oid` when unique; if **ambiguous**, error and (optionally) provide a suggestion helper.
- Printing (`git_uri_format`) always uses **full OIDs** once resolved.

---

## Content & tree/table functions (with `_each`)

```sql
-- Snapshots of a path at each commit in the selector (ref / revset / range_expr)
CREATE TABLE FUNCTION git_tree(u git_uri)
RETURNS (
  commit_oid BLOB, commit_time TIMESTAMP, path VARCHAR,
  object_type VARCHAR, filemode INTEGER, blob_oid BLOB, size_bytes BIGINT
);

CREATE TABLE FUNCTION git_tree_each(u git_uri)
RETURNS (
  input git_uri,
  commit_oid BLOB, commit_time TIMESTAMP, path VARCHAR,
  object_type VARCHAR, filemode INTEGER, blob_oid BLOB, size_bytes BIGINT
);

-- Per-commit changes, honoring opts.path, opts.submodules, depth settings
CREATE TABLE FUNCTION git_changes(u git_uri) RETURNS (...);
CREATE TABLE FUNCTION git_changes_each(u git_uri) RETURNS (input git_uri, ...);

-- Read blob content; obey encoding/errors if provided
CREATE TABLE FUNCTION git_read(u git_uri)
RETURNS (path VARCHAR, blob_oid BLOB, size_bytes BIGINT,
         content_bytes BLOB, content_text VARCHAR);

CREATE TABLE FUNCTION git_read_each(u git_uri)
RETURNS (input git_uri, path VARCHAR, blob_oid BLOB, size_bytes BIGINT,
         content_bytes BLOB, content_text VARCHAR);
```

**Option semantics**
- `submodules='recurse'` → `git_tree/git_changes` traverse into submodules using the same selector; consider a recursion guard and respect `depth_commits`.
- `encoding=NULL` → `content_bytes` filled; `content_text` is `NULL`.  
  `encoding='utf-8'` (+ `errors`) → also fill `content_text`.
- `depth_commits` caps commit traversal; `depth_tree` caps directory recursion.

---

## Do we “need” `git_uri_parse` / `git_uri_format` if we support casts?

- **Casts are great for the common case** (`raw::git_uri`, `uri::VARCHAR`).
- **Keep the functions anyway** because they:
    - Enable the `_each` LATERAL pattern (`git_uri_parse_each`, `git_uri_format_each`)
    - Allow **flags/overloads** later (e.g., `accept_abbrev := TRUE`, strict mode parsing)
    - Are clearer in some client ORMs that struggle with explicit casts in prepared statements
    - Provide a **stable API name** you can deprecate/redirect independently of casts

Think of casts as **syntax sugar**; the named functions are the **API surface**.

---

## Examples

### Parse column values with `_each` (keep input + parsed)
```sql
SELECT p.input AS raw, p.value AS uri, p.value::VARCHAR AS canonical
FROM my_raw_table t
JOIN LATERAL git_uri_parse_each(t.raw_text) AS p ON TRUE;
```

### Emit canonical strings for storage/logging
```sql
SELECT f.value AS canonical
FROM my_git_table t
JOIN LATERAL git_uri_format_each(t.uri) AS f ON TRUE;
```

### Walk trees with options (unspecified → libgit defaults)
```sql
-- Shorthand; opts unspecified (NULL) → executor uses library defaults
WITH u AS (
  SELECT 'git+file:///repo#HEAD:docs/help.md'::git_uri AS uri
)
SELECT *
FROM u
JOIN LATERAL git_tree_each(u.uri) AS e ON TRUE
ORDER BY commit_time DESC;
```

### Three-dot evaluated at execution time
```sql
WITH u AS (
  SELECT 'git+ssh://git@github.com/org/repo.git#range=feature...main;path=src/'::git_uri AS uri
)
SELECT *
FROM u
JOIN LATERAL git_changes_each(u.uri) AS e ON TRUE;
```

### Decoding content
```sql
WITH u AS (
  SELECT 'git+https://github.com/org/repo.git#ref=expr:HEAD;path=README.md;encoding=utf-8;errors=replace'::git_uri AS uri
)
SELECT path, content_text
FROM u
JOIN LATERAL git_read_each(u.uri) r ON TRUE;
```

---

## Error Handling (highlights)

- **Colon in paths (shorthand ambiguity)** → recommend canonical `;path=:(literal)…`.
- **Ambiguous abbreviated OID** → error with candidates; never guess.
- **Unknown option value** (e.g., `submodules=wut`) → parse error.
- **Selector invariant**: exactly one of `ref`, `revset`, or `range_expr`.

---

## Testing Matrix

- **Round‑trip**: `VARCHAR → git_uri → VARCHAR` yields canonical; options omitted when `NULL`.
- **Ranges**: `A..B` → `revset` (`+B, -A`), `A...B` → `range_expr` preserved; executor expands.
- **Pathspec**: include/exclude; `:(glob)`/`:(literal)`; empty lists.
- **URI normalization**: SCP SSH → `git+ssh://…`; `file://` → `file:///…`; percent-encoding.
- **Options**: default omission delegates to libgit; encoding/text columns behavior; depth caps enforced.
- **_each**: inputs echoed; works with `JOIN LATERAL` and column-valued parameters.

---

## Implementation Steps

**Phase 1 — Types & Casts**
1. `CREATE TYPE`: `git_ref`, `git_rev_term`, `git_revset`, `git_pathspec_mode`, `git_pathspec`, `git_opts`, `git_selector`, `git_uri`.
2. Implement `git_uri_parse` + cast `VARCHAR→git_uri`; implement `git_uri_format` + cast `git_uri→VARCHAR`.
3. Implement `_each` wrappers for both.

**Phase 2 — Building Blocks & Expert API**
1. (Optionally) expose `git_ref_parse(_each)`, `git_revset_parse(_each)` under `gitx.*` or behind `PRAGMA git.enable_expert_api=1`.

**Phase 3 — Repo-aware helpers & table functions**
1. Implement `git_uri_verify(_each)`, `git_revset_commits(_each)`, `git_range_expr_commits(_each)`.
2. Implement `git_tree(_each)`, `git_changes(_each)`, `git_read(_each)` honoring `opts`.

---

## Appendix: Canonical Fragment Mini‑Grammar (with options)

```
fragment    = ( ref-kv | range-kv ) *( ";" ( path-kv | exclude-kv | mode-kv | opt-kv ) ) ;
ref-kv      = "ref=" ( "oid:" HEX+ | "ref:" REFNAME | "expr:" REVEXPR ) ;
range-kv    = "range=" RANGEEXPR ;  -- may be "A..B" or "A...B" (merge-base computed at execution)
path-kv     = "path=" PATHSPEC ;
exclude-kv  = "exclude=" PATHSPEC ;
mode-kv     = "mode=" ( "literal" | "glob" | "icase" ) ;
opt-kv      = ( "submodules=" ( "ignore" | "list" | "recurse" ) )
            | ( "encoding=" ENCODING )
            | ( "errors=" ( "strict" | "replace" ) )
            | ( "depth_commits=" INT )
            | ( "depth_tree=" INT ) ;
```

**Defaulting rule:** Any option **absent** from the fragment is stored as `NULL` in `git_opts`, which means **“unspecified”**—execution defers to libgit (and the called function’s defaults).

---