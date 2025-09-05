# Changing `git_uri(...)` to Return a **Typed** `GIT_URI` (proposal)

> This doc focuses on flipping the existing `git_uri(...)` helper from returning a **string** to returning the **custom logical type** `GIT_URI`, and how to integrate that cleanly with the current feature-branch code (`git_tree*`, `git_read*`, `git://` filesystem, LATERAL joins, libgit2).  
> Wrapped in quadruple fences so inner ``` blocks render correctly.

---

## Decision
**Yes — change `git_uri(...)` to return `GIT_URI`.**  
This improves type safety and ergonomics across the board, while staying compatible with the existing `git://…@…` execution paths via a small “FS formatter” helper.

---

## New Surface (summary)

### Types
```sql
-- constituent atoms (unchanged from earlier plan)
CREATE TYPE git_ref         AS UNION(oid BLOB, ref VARCHAR, expr VARCHAR);
CREATE TYPE git_rev_term    AS STRUCT(include BOOLEAN, ref git_ref);
CREATE TYPE git_revset      AS LIST(git_rev_term);
CREATE TYPE git_pathspec_mode AS ENUM('literal','glob','icase');
CREATE TYPE git_pathspec    AS STRUCT(include LIST(VARCHAR), exclude LIST(VARCHAR), mode git_pathspec_mode);
CREATE TYPE git_opts        AS STRUCT(submodules VARCHAR, encoding VARCHAR, errors VARCHAR, depth_commits INTEGER, depth_tree INTEGER);

-- main selector & URI
CREATE TYPE git_selector AS STRUCT(
  sel  UNION(ref git_ref, revset git_revset, range_expr VARCHAR),
  path git_pathspec,
  opts git_opts
);
CREATE TYPE GIT_URI AS STRUCT(repo_url VARCHAR, selector git_selector);
```

### Core functions (scalar)
```sql
-- *** changed: now returns the typed value ***
CREATE FUNCTION git_uri(uri_text_or_repo VARCHAR, file_or_ref VARCHAR := NULL, ref VARCHAR := NULL,
                        submodules VARCHAR := NULL, encoding VARCHAR := NULL, errors VARCHAR := NULL,
                        depth_commits INTEGER := NULL, depth_tree INTEGER := NULL) RETURNS GIT_URI;

-- explicit parse/format (keep even with casts)
CREATE FUNCTION git_uri_parse(s VARCHAR) RETURNS GIT_URI;
CREATE FUNCTION git_uri_format(u GIT_URI) RETURNS VARCHAR;   -- canonical '#ref=…;path=…' form

-- FS formatter to interop with current 'git://…@…' filesystem & readers
CREATE FUNCTION git_uri_format_fs(u GIT_URI) RETURNS VARCHAR;
```

### Casts
```sql
CREATE CAST (VARCHAR AS GIT_URI) WITH FUNCTION git_uri_parse;  -- accepts both 'git://…@…' and 'git+…#…'
CREATE CAST (GIT_URI AS VARCHAR) WITH FUNCTION git_uri_format;  -- canonical fragment
```

### `_each` LATERAL companions
```sql
CREATE TABLE FUNCTION git_uri_each(s VARCHAR)
RETURNS TABLE(input VARCHAR, value GIT_URI);

CREATE TABLE FUNCTION git_uri_format_each(u GIT_URI)
RETURNS TABLE(input GIT_URI, value VARCHAR);

CREATE TABLE FUNCTION git_uri_format_fs_each(u GIT_URI)
RETURNS TABLE(input GIT_URI, value VARCHAR);
```

> Rationale: `_each` echoes the input, enabling per-row construction/formatting in LATERAL joins.

---

## `git_uri(...)` (constructor) Overloads & Behavior

We keep the familiar call shapes but now **build a typed value**:

```sql
-- 1) Pass-through
SELECT git_uri(my_typed_uri::GIT_URI);  -- returns same value

-- 2) One-arg string: parse either canonical or legacy FS string
SELECT git_uri('git://README.md@HEAD');                                    -- current repo
SELECT git_uri('git:///abs/repo/path/docs/help.md@HEAD~1...HEAD');         -- absolute repo
SELECT git_uri('git+file:///abs/repo#range=HEAD~1...HEAD;path=docs/help.md');

-- 3) Two/three-arg builder
SELECT git_uri('/abs/repo', 'docs/help.md', 'HEAD');
SELECT git_uri('/abs/repo', 'HEAD');                       -- no path (tree selection)
SELECT git_uri('/abs/repo', 'v2.0..main');                 -- range shorthand

-- 4) Options (all default NULL → defer to libgit defaults at execution)
SELECT git_uri('/abs/repo', 'docs/help.md', 'HEAD',
               submodules := 'recurse', depth_commits := 500);
```

**Parsing rules**
- If the first argument starts with `git://` or `git+`, treat as a single **string form** and parse.
- Otherwise:
    - `(repo, file, ref)` if three args.
    - `(repo, ref)` if two args and the second looks like a revision/range (`..`, `...`, `~`, `^`, `@{}`); `(repo, file)` otherwise.
- All options are stored in `selector.opts`; **`NULL` means “unspecified”** (executor/libgit uses its default).

---

## Interop with Existing Execution (zero rewrite)

Your feature branch executes via:
- **`git://` filesystem** (for readers like `read_csv`)
- **table functions** (`git_tree*`, `git_read*`) that already parse `git://…@…` internally

We keep that intact:

```sql
-- Feed existing functions using the FS formatter:
WITH u AS (
  SELECT git_uri('/abs/repo', 'docs/help.md', 'HEAD~2..HEAD') AS uri
)
SELECT *
FROM u,
LATERAL git_tree_each( git_uri_format_fs(u.uri) ) t;  -- emits 'git:///abs/repo/docs/help.md@HEAD~2..HEAD'
```

**Under the hood**
- `git_uri_format_fs(GIT_URI)` → `git://<repo_or_abs>/<path>@<rev_or_range>`
- Your current binders detect `git://` and route to `GitPath::Parse` → **same libgit2 paths as before**.

> This lets us adopt the type **without touching libgit2 code paths** in `git_tree*` / `git_read*`. Only the binders and a small formatter are needed.

---

## Binder Upgrades (nice-to-have but small)

- In each table function binder (`git_tree`, `git_log`, `git_read`):
    1) If the first arg is **`GIT_URI`**, call `git_uri_format_fs()` and proceed exactly as today.
    2) If the first arg is `VARCHAR` **and** starts with `git://`, treat it as a URI (existing behavior).
    3) Else treat first arg as repo path (existing behavior).

This three-way dispatch keeps string users happy, adds typed users, and preserves current semantics.

---

## Ergonomics in LATERAL Joins

```sql
-- Build typed URIs per row, keep the raw token, and execute
WITH files AS (
  SELECT '/abs/repo' AS repo, 'README.md' AS path, 'HEAD~5...HEAD' AS rev
  UNION ALL SELECT '/abs/repo', 'docs/help.md', 'HEAD'
)
SELECT f.repo, f.path, f.rev, r.path, r.size_bytes
FROM files f
JOIN LATERAL git_uri_each( git_uri(f.repo, f.path, f.rev)::VARCHAR ) p ON TRUE
JOIN LATERAL git_read_each( git_uri_format_fs(p.value) ) r ON TRUE;
```

- If you prefer to skip the intermediate cast, use `git_uri_each(f.repo || ':' || f.path || '@' || f.rev)` or call `git_uri(f.repo, f.path, f.rev)` directly and then `git_uri_format_fs_each`.

---

## Breaking Change Analysis & Mitigation

**What changes**
- `git_uri(repo, file, ref)` previously returned `VARCHAR`. It will now return `GIT_URI`.

**Who breaks**
- Any SQL that passes `git_uri(...)` directly to a function expecting a **string** path (e.g., `read_csv(git_uri(...))`).

**Mitigations**
- Provide **`git_uri_format_fs(GIT_URI) → VARCHAR`** and an `_each` wrapper.
- Provide an alias `git_uri_text(...)` (temporary) that returns the old string form for users who want to defer migration.
- `CAST(git_uri(...) AS VARCHAR)` prints **canonical**, which is great for logs but not accepted by the `git://` FS; prefer `git_uri_format_fs(...)` when you need an executable URI.

**Doc snippet to include**
```sql
-- Before:
SELECT * FROM read_csv( git_uri('/repo', 'data.csv', 'HEAD') );

-- After (two options):
SELECT * FROM read_csv( git_uri_format_fs( git_uri('/repo', 'data.csv', 'HEAD') ) );
-- or temporarily:
SELECT * FROM read_csv( git_uri_text('/repo', 'data.csv', 'HEAD') );  -- legacy alias
```

---

## Ranges & Triple-dot (execution)

- Parse-time remains **syntax-only**:
    - `A..B` → `revset = [{+B}, {-A}]`
    - `A...B` → `range_expr = 'A...B'` (no merge-base yet)
- Execution in `git_tree*`/`git_read*`:
    - **two-dot**: `revwalk_push(B)` + `revwalk_hide(A)`
    - **three-dot**: compute merge-base(s) then `push(A)`, `push(B)`, `hide(base*)`
- `_each` variants should **echo input** so users can correlate rows back to the originating URI.

---

## Defaults (delegate to Git)

The `git_opts` inside `GIT_URI.selector` carries optional execution hints:

- `submodules`: `NULL` → no recursion (list gitlinks as opaque entries)
- `encoding`: `NULL` → return bytes; optionally also fill `text` when UTF-8 heuristic passes (current behavior)
- `errors`: `NULL` → executor default (e.g., `'replace'`)
- `depth_commits`/`depth_tree`: `NULL` → unlimited (executor may enforce safety caps)

We omit `NULL` keys when printing canonical strings.

---

## Tests to Add

1) **Return type flip**
```sql
SELECT typeof(git_uri('/r','f','HEAD')) = 'GIT_URI';
```

2) **FS interop**
```sql
WITH u AS (SELECT git_uri('/r','f.csv','HEAD') AS u)
SELECT * FROM read_csv( git_uri_format_fs((SELECT u FROM u)) );
```

3) **Round-trip**
```sql
SELECT git_uri_parse( git_uri_format( git_uri('/r','f','HEAD') ) ) IS NOT NULL;
```

4) **Shorthand range parse**
```sql
SELECT (git_uri('/r','HEAD~2..HEAD')::VARCHAR) LIKE '%range=HEAD~2..HEAD%';
SELECT (git_uri('/r','HEAD~2...HEAD')::VARCHAR) LIKE '%range=HEAD~2...HEAD%';
```

5) **LATERAL each**
```sql
WITH t(x) AS (VALUES ('git://README.md@HEAD'), ('git://docs/help.md@HEAD~1'))
SELECT e.input, e.value::VARCHAR FROM t, LATERAL git_uri_each(t.x) e;
```

---

## Migration Guidance (short)

- Prefer **typed** `git_uri(...)` everywhere.
- When you need an **executable string** for readers or legacy paths, wrap with `git_uri_format_fs(...)`.
- For logging/hashes/equality, use `CAST(GIT_URI AS VARCHAR)` (canonical key–value fragment).

---

## Edge Notes

- Avoid naming clash: keep the logical type uppercase **`GIT_URI`**; keep function `git_uri(...)` (lowercase) as the constructor.
- Consider keeping `git_uri_text(...)` for one minor release, then deprecate.
- Keep `git_ref_parse`, `git_revset_parse` available as **expert API**, plus `_each` variants.

---

## Example Cheat Sheet

```sql
-- Build typed
SELECT git_uri('/repo','docs/help.md','HEAD');

-- Canonical string (for logs)
SELECT git_uri('/repo','docs/help.md','HEAD')::VARCHAR;
-- -> 'git+file:///repo#ref=expr:HEAD;path=docs/help.md'

-- FS string (for read_csv or current binders)
SELECT git_uri_format_fs( git_uri('/repo','docs/help.md','HEAD~2..HEAD') );
-- -> 'git:///repo/docs/help.md@HEAD~2..HEAD'

-- LATERAL read
WITH u(uri) AS (VALUES (git_uri('/repo','docs/help.md','HEAD~1...HEAD')))
SELECT r.path, r.size_bytes
FROM u, LATERAL git_read_each( git_uri_format_fs(u.uri) ) r;
```

---

### Bottom line
Switching `git_uri(...)` to return `GIT_URI` is a **net win** with very little disruption. Add the FS-formatter and binder tweaks and you can keep all the existing libgit2 execution paths intact while unlocking type-safe composition, better defaults, and a clean story for LATERAL joins.