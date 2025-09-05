# Duck Tails — Feature Branch Technical Assessment & Integration Plan

> **Scope.** I unpacked and reviewed the uploaded feature-branch snapshot of **duck_tails** (`/src`, `/test`, and the design docs). This write‑up explains:  
> (a) what needs the code serves,  
> (b) how the new APIs behave (ergonomics + LATERAL joins),  
> (c) how they map to libgit2 capabilities, and  
> (d) the likely impact when merging into the current **main** (which, per your note, doesn’t have `git_tree*` / `git_read*` yet).

---

## 1) What this branch aims to do (in plain language)

- **Bring “Git‑as‑a‑filesystem” to DuckDB**  
  Users can address repo contents with `git://` URIs, at specific refs/ranges, and read those files through DuckDB I/O (CSV/Parquet readers) or via first‑class table functions.

- **Add first-class, queryable Git primitives**  
  New table functions (with `_each` LATERAL variants) surface commits/trees/blobs as rows:
    - `git_log`, `git_branches`, `git_tags` (already existed; consistency improvements present)
    - **`git_tree` / `git_tree_each`**: list tree entries at a ref/commit; includes blob metadata
    - **`git_read` / `git_read_each`**: read one file’s contents and metadata at a given ref/path/URI

- **Support concrete analysis patterns**
    - “Show this file across the last N commits” (diff/time series)
    - “List the tree at HEAD or at tag vX.Y”
    - “Join my dataset against repo content addressed by Git URIs per row” (LATERAL)

---

## 2) What’s actually in the feature branch (observed)

### 2.1 New/extended functions & schemas

- **`git_tree`** (table function) — schema (as implemented in `src/git_functions.cpp`):
  ```
  repo_path VARCHAR,      -- ALWAYS first column
  commit_hash VARCHAR,
  commit_date TIMESTAMP,
  path VARCHAR,
  mode INTEGER,           -- filemode (e.g., 100644, 100755, 120000, 160000)
  blob_hash VARCHAR,
  size BIGINT,
  git_file_uri VARCHAR    -- helper string for addressing this row’s blob
  ```
  Notes:
    - There is a single‑commit mode (compatible with earlier tests) and “ranged” behavior via parameters.
    - The repo path is consistently the **first column** (good: makes multi‑repo unioning ergonomic).

- **`git_tree_each`** (LATERAL)
    - Designed for **per-row repo/ref binding**: repo path comes from the left input at execution time.
    - Schema mirrors `git_tree`, with the same first column `repo_path`.

- **`git_read` / `git_read_each`** (table functions returning one row per input URI)
  ```
  -- observed from registration/output helpers:
  uri VARCHAR,            -- echoed input (for LATERAL correlation)
  mode INTEGER,           -- filemode at the ref/path
  kind VARCHAR,           -- 'file' | 'symlink' | 'submodule' | ...
  is_text BOOLEAN,        -- heuristic (e.g., UTF‑8 detection)
  encoding VARCHAR,       -- 'utf8' | 'binary' | 'unknown'
  size_bytes BIGINT,
  truncated BOOLEAN,      -- indicates partial read (safety)
  text VARCHAR,           -- decoded text if is_text && encoding set
  blob BLOB               -- raw bytes
  ```
  Notes:
    - **`git_read_each`** is already used in tests with `JOIN LATERAL` on a column of `git://...@...` URIs.
    - Heuristic UTF‑8 detection is present (sets `is_text` / `encoding`). When not text, `encoding='binary'`.

- **`git_uri`** (scalar)
    - Helper to build `git://` URIs from `(repo, file, ref)` consistently for users and tests.

- **Parameter parsing helpers**
    - *`UnifiedGitParams`*, `ParseUnifiedGitParams(...)`, `ParseLateralGitParams(...)` implement the binding story for both regular and `_each` functions (static args at bind time; column‑bound args at runtime).

### 2.2 `git://` filesystem integration

- **`src/git_filesystem.cpp/.hpp`** provide a DuckDB filesystem implementation that recognizes:
    - `git://<file>@<rev>` against **current repo** (default),
    - `git:///absolute/repo/path/<file>@<rev>` (absolute path),
    - `git://../relative/repo/<file>@<rev>` and `git://./other-repo/<file>@<rev>` (relative repos).

- **Revision/range understanding:**
    - Recognizes `<rev>` including two‑dot (`A..B`) and three‑dot (`A...B`) range forms in the URI’s `@<rev>` part.
    - Path normalization handles `.` and `..` in both repo paths and file paths.

- **libgit2** is the backend (headers included), with typical repo open/discover + revwalk + tree/blob lookups.

### 2.3 Tests reflect intended usage

- LATERAL patterns like:
  ```sql
  SELECT t.uri, r.kind
  FROM (VALUES ('git://README.md@HEAD'), ('git://path@HEAD~1')) t(uri),
       LATERAL git_read_each(t.uri) r;
  ```
- Comprehensive tests for `git_tree_each` (presence of `repo_path` first col, unionability, named args).
- Tests note **current limitation** (in this branch) where **passing `git://...` as a table‑function parameter** to `git_tree`/`git_log` is not fully plumbed: they are interpreted as literal refs instead of URIs (binder needs to detect/route URI cases).

---

## 3) Ergonomics review (what’s good, what to adjust)

### 3.1 What feels good already

- **Consistent “first column”** identity:  
  Tables that enumerate repo objects (`git_log`, `git_tree`, `git_branches`, `git_tags`) put `repo_path` first. This makes multi‑repo analysis and joins straightforward.

- **`_each` variants** mirror DuckDB’s LATERAL style:  
  They **echo the input** (`uri` or `repo_path`) and resolve the rest per row. That plays perfectly with CSV or app‑generated lists of URIs.

- **`git_read_each`** is a strong ergonomic anchor:  
  You can point a DuckDB reader directly at Git content like:
  ```sql
  -- read_csv over Git-backed content:
  SELECT * FROM read_csv('git://data/sales.csv@HEAD');
  -- or preflight it via git_read_each to get metadata:
  WITH u(uri) AS (VALUES ('git://data/sales.csv@HEAD'))
  SELECT * FROM u, LATERAL git_read_each(u.uri);
  ```

### 3.2 Where to tighten UX

- **Make `git://...` a *first-class* parameter for table functions**  
  Currently, `git_tree('git:///repo/file@rev')` fails because the binder treats the string as a ref. For ergonomics:
    - Detect a `git://` prefix in the first argument.
    - If present, **parse into `(repo_path, file_path, ref)`** and dispatch to the same codepath used by `git_read_each` (you already have parsing in `git_filesystem` — reuse it).
    - This makes:
      ```sql
      SELECT * FROM git_tree('git:///path/to/repo@HEAD'); -- list the whole tree at HEAD
      SELECT * FROM git_tree('git://README.md@HEAD');     -- current repo
      ```
      behave as users expect.

- **Unify “input echo” columns**
    - Today `git_tree(_each)` starts with `repo_path`, while `git_read_each` starts with `uri`.
    - That’s rational (the former targets a tree at a repo/ref; the latter targets a specific blob), but consider a **strict rule**:
        - Functions that **consume a URI** → first column is `uri` (echo).
        - Functions that **consume repo/ref** → first column is `repo_path` (echo), with `ref` also present if supplied.
    - Add `ref` (or `effective_ref`) as a **second column** in `git_tree` outputs for perfect symmetry and easier provenance joins.

- **Error messages that teach**
    - If the binder sees a string that *looks* like a `git://` URI but the function signature expects `repo_path`, suggest the correct call shape:
      > “`git_tree('git:///path@HEAD')` looks like a Git URI. Did you mean `git_tree('git:///path@HEAD')` (URI mode) or `git_tree('/path', ref := 'HEAD')` (repo/ref mode)?”

- **Shorthand triple-dot**
    - You already parse `A...B` in URIs. For `git_tree`/`git_changes`, allow `ref := 'A...B'` (string form) so people can keep one mental model across URIs and functions.

---

## 4) LATERAL join scenarios (what to ensure)

- **Bind‑time vs runtime parameters:**  
  Your `ParseLateralGitParams` approach is right: static options at bind, **repo/ref from the left DataChunk at execution**. Keep:
    - Left argument is the dynamic **repo path** (or URI).
    - Secondary named args at bind time: `ref`, `path_filter`, `max_depth`, etc.

- **Echo the input for correlation:**  
  `_each` functions should **always** include the echo column first:
    - `git_tree_each(repo_path, ref)` → `repo_path` first.
    - `git_read_each(uri)` → `uri` first.

- **Variadic / union inputs**  
  Many users will pass a **mixed bag**: some rows with plain paths (`/repo`), some with URIs (`git://...@...`). Consider:
    - A single `_each` that **dispatches per row**: if the string starts with `git://`, treat it as a URI; else treat it as `repo_path`. This avoids CASE/UNION boilerplate in SQL.

- **No row explosion surprises**
    - Clarify in docs that `git_read_each` returns **one row per URI**.
    - `git_tree_each` may return **many rows per input row** (files under that tree). That’s expected, just call it out.

---

## 5) libgit2 compatibility notes (practical)

- **Discovery & opening**
    - Use `git_repository_open_ext(..., GIT_REPOSITORY_OPEN_CROSS_FS, ...)` or `git_repository_discover` for relative paths. You appear to be doing this; keep it consistent for `git://` and table‑function parameters.

- **Rev parsing**
    - Keep parse strict at syntax level; do **rev resolution** only during execution (`git_revparse_single`, `git_reference_dwim`, `git_object_peel`, `git_revwalk_*`).
    - For `A...B`, compute **merge base(s)** at execution (`git_merge_base_many` or equivalent). Avoid assumptions at parse time.

- **Memory/resource safety**
    - Confirm every `git_*_free` call is paired; your code shows the typical patterns (free `repo`, `walker`, `commit`, `tree`, `blob`, etc.).
    - For multi‑row enumerations, reuse `git_repository*` across a task (per‑executor state) to avoid reopen cost; tests suggest you’re doing per‑bind/per‑init state correctly.

- **Text detection & encoding**
    - You’re setting `encoding='utf8'|'binary'|'unknown'` and exposing `is_text`. That’s a solid start.
    - Pair this with user‑controlled **decoding** in the future (e.g., `encoding` option on the URI/function). If unspecified, keep returning raw bytes and only opportunistically fill `text` when UTF‑8.

- **Submodules**
    - `kind='submodule'` shows up in your code paths. If you don’t recurse (default Git behavior), that’s fine. Consider future options: `submodules = ignore|list|recurse`.

- **Abbreviated OIDs**
    - Accept as **input**; resolve to **full OIDs** at execution. If ambiguous, **error** and (optionally) expose a `git_suggest(prefix)` helper that lists candidates.

---

## 6) Impact when merging to `main`

`main` (per your note) **does not** have `git_tree*` / `git_read*`. Expected impacts:

- **Pure additions** (no breaking change) if you:
    - Keep existing function names/signatures for `git_log`, `git_branches`, `git_tags`, `diff_text`, `read_git_diff` untouched.
    - Register **both** scalar and `_each` variants of the new functions.
    - Ensure the `git://` filesystem remains backwards‑compatible with current URIs (`git://FILE@HEAD` in the CWD).

- **Binder behavior change for strings**
    - If you introduce “string arg **might** be a URI” to table functions, be careful not to break callers that pass a **ref name** that happens to start with `git` (unlikely, but guard by **requiring `git://` prefix** to trigger URI mode).

- **Column order stability**
    - You already standardize `repo_path` as the **first column** for repo‑enumeration functions. Keep that invariant.

- **Versioning**
    - Include `Extension::Version()` bump and a brief CHANGELOG in `README.md` (you already have “What’s New” sections in the doc; keep it crisp).

---

## 7) Concrete recommendations (code-level)

1. **Binder: detect `git://` for table functions**  
   In each `*Bind`:
    - If first input is present and starts with `git://`, **parse URI into (repo, file, rev)**.
    - For `git_tree`, ignore file and list the **whole tree** unless you add an optional `path_filter`.
    - For `git_log`, drop the file and just use `(repo, rev)`.

2. **Echo/provenance**
    - Add `ref` (or `effective_ref`) as a **second column** to `git_tree(_each)` (non‑breaking, just additive).
    - Keep `uri` as the **first column** for `git_read_each`. Consider adding a `repo_path` column too for convenience.

3. **Options surface (future‑proofing, all default‑NULL to defer to Git):**
    - `submodules = 'ignore'|'list'|'recurse'`
    - `encoding = 'utf-8'|...` and `errors = 'strict'|'replace'`
    - `depth_commits`, `depth_tree` (caps)  
      These can be **named parameters** on table functions, and/or carried within a canonical URI fragment later.

4. **Uniform `_each` pattern**
    - Every scalar/table gets a sibling `_each` with an **echoed input** column named `input` or a domain‑specific name (`repo_path`, `uri`). Keep naming consistent.

5. **Tests to add before merge**
    - `git_tree('git:///abs/repo@HEAD')` returns rows (table‑param URI path).
    - `git_tree_each(t.repo_column, ref := 'main')` LATERAL join across multiple repos.
    - `git_read_each` on binary blobs returns `blob` filled, `text=NULL`.
    - `A...B` triple‑dot works in both URI and `ref := 'A...B'`.

---

## 8) “Now vs Later” (incremental plan)

- **Now (merge window)**
    - Land `git_tree`, `git_tree_each`, `git_read`, `git_read_each` as **additive APIs**.
    - Teach table binders to accept `git://` as a first argument (URI mode).
    - Keep current `encoding` heuristics; don’t add new options yet (to minimize surface churn).

- **Next minor**
    - Add named options (`submodules`, `encoding`, `errors`, `depth_*`).
    - Consider the `GIT_URI` logical type + casts discussed earlier, while keeping string/URI compatibility.

---

## 9) Quick examples (showing intended UX)

```sql
-- List HEAD tree in current repo
SELECT path, mode, size
FROM git_tree('HEAD')
ORDER BY path;

-- List HEAD tree from an absolute repo (first arg detected as URI)
SELECT path, mode, size
FROM git_tree('git:///Users/alex/Dev/duck_tails@HEAD');

-- LATERAL: read different URIs per row
WITH uris(uri) AS (
  VALUES ('git://README.md@HEAD'),
         ('git://docs/help.md@HEAD~1')
)
SELECT r.uri, r.kind, r.size_bytes
FROM uris u, LATERAL git_read_each(u.uri) r;

-- Multi-repo: log HEAD on each repo
WITH repos(repo_path) AS (VALUES ('/repo/a'), ('/repo/b'))
SELECT l.repo_path, l.commit_hash, l.commit_date
FROM repos r, LATERAL git_log_each(r.repo_path, ref := 'HEAD') l
ORDER BY l.repo_path, l.commit_date DESC;
```

---

## 10) Open questions (for your call)

- Do we want `git_tree` to **filter by path prefix** (a directory) when users pass `git://.../<dir>@<ref>` as the first arg? (Nice to have; otherwise the URI drives only repo/ref.)
- Should `git_read_each` **always** return both `blob` and `text` (when text) or make `text` opt‑in via `encoding`? (Current heuristic is fine; keep `text` nullable.)
- Are we comfortable **not** recursing into submodules by default? (Matches Git’s defaults; surface the option later.)

---

### Bottom line

The feature branch moves the extension from *“Git metadata only”* toward *“Git is a first‑class storage backend”* in DuckDB—**exactly the right direction**. The big ticket to finish before merge is **teaching table functions to recognize `git://...` inputs** and keeping the **`_each` LATERAL ergonomics** uniformly applied. Everything else (submodules/encoding/depth options, typed URIs) can land incrementally without breaking users.