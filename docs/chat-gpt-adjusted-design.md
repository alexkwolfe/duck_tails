# Adjusted Plan for `GIT_URI` (strings-only, unified URI, **git+file://** emission)

> **What changed (per your direction):**
> - Public SQL surface stays **strings only** (no SQL logical type).
> - All `read_*` functions **accept only** `git://` and `git+file://` schemes for Git inputs.
> - All `git_*` functions that **return URIs** must emit **`git+file://`** (never `git://`).
> - Internally we still parse into a private `GitUri` struct; users never see it.

---

## 1) Goals & guiding rules

1. **One output format** for Git URIs returned by our functions:  
   **`git+file://<abs-repo>#<revspec>[:<path>]`**
2. **Two input schemes** recognized by `read_*` (and by our own `git_*` acceptors):
    - `git+file://…` (preferred)
    - `git://…` (accepted **input** for convenience; never emitted)
3. **Two syntaxes** for the Git selector inside the URI, both parsed:
    - **Fragment form (preferred):** `#<revspec>[:<path>]`
    - **Legacy form:** `/path@<revspec>` (we parse, then normalize internally)

> The rest of DuckDB keeps working unchanged. Users can pipe returned URIs straight into `read_text`, `read_json_auto`, `read_csv`, etc.

---

## 2) URI forms (accepted vs emitted)

### Emitted (from `git_*` functions)
- Always **`git+file://<abs-repo>#<revspec>[:<path>]`**
    - Examples:
        - File at HEAD:  
          `git+file:///Users/alex/Dev/duck_tails#HEAD:README.md`
        - File at pinned commit:  
          `git+file:///Users/alex/Dev/duck_tails#4f9e8d7…:docs/help.md`
        - Directory at tag:  
          `git+file:///Users/alex/Dev/duck_tails#v2.0:src/`

### Accepted input (for `read_*` and our own functions)
- **Preferred:** `git+file://<repo>#<revspec>[:<path>]`
- **Also accepted:** `git://<repo>#<revspec>[:<path>]`
- **Legacy:** `git+file://<repo>/<path>@<revspec>` or `git://<repo>/<path>@<revspec>`

**Colon-in-path rule:** In `#<revspec>[:<path>]`, we **split at the first `:`** after `<revspec>`; **all remaining `:` belong to `path`**.  
Example: `#HEAD:docs/a:b:c.md` → `revspec='HEAD'`, `path='docs/a:b:c.md'`.

**`@` split rule (legacy):** Split on the **last** `@` to tolerate rare `@` in directory names.

**Repo normalization:** We discover and store the **absolute repo root**, so emitted URIs always carry an absolute path after `git+file://`.

---

## 3) Filesystem & parser (single source of truth)

- Extend `GitPath::Parse` to recognize:
    - Schemes: **`git+file://`** (preferred), **`git://`** (input only)
    - Selectors: fragment `#<revspec>[:<path>]` and legacy `/path@<revspec>`
- Parse into internal `GitUri { abs_repo_path, revspec, path_opt }`.
- All `read_*` and `git_*` executors call through this parser.

---

## 4) `read_*` behavior (no new overloads)

- **Do not** add typed overloads; keep DuckDB built-ins as-is:
    - `read_text(path VARCHAR)`, `read_json_auto(path VARCHAR, ...)`, `read_csv(path VARCHAR, ...)`, …
- When the path starts with **`git+file://`** or **`git://`**, our FS intercepts:
    1) Parse to `GitUri`
    2) Resolve via libgit2 (tree/blob)
    3) Stream bytes to the reader
- For any other scheme or plain path: defer to the normal reader behavior unchanged.

---

## 5) `git_*` functions (inputs & outputs)

### Inputs
- Accept **either**:
    - `(repo_path VARCHAR, rev VARCHAR DEFAULT 'HEAD')` (whole-tree focus), or
    - `(uri_or_repo VARCHAR, …)` where `uri_or_repo` may be:
        - `git+file://…` (preferred)
        - `git://…` (legacy input)
        - a filesystem path (we add `rev` separately)
- In bind, **if it looks like a Git URI**, parse to `GitUri` and ignore extra `rev` arg.

### Outputs
- Whenever we return a per-object handle (file snapshot, diff pair, etc.), include:
    - **`uri VARCHAR`**: **always** the **`git+file://`** fragment form
- Keep other metadata as today (blob hash, size, mode, kind, commit info, etc.)

**Examples**

- `git_tree('git+file:///abs/repo#HEAD')` returns rows with:
    - `path`, `mode`, `size`, `blob_hash`, …, **`uri` = `git+file:///abs/repo#HEAD:<path>`**
- `git_changes(…)` may return `old_uri`, `new_uri` in the same **`git+file://`** format.

---

## 6) Range & revspec semantics (execution-time)

- `revspec` is opaque until execution:
    - **Two-dot** `A..B` → revwalk: **push B**, **hide A**
    - **Three-dot** `A...B` → compute merge-base(s); **push A**, **push B**, **hide base\***
    - `^X Y` → **hide X**, **push Y**
    - `HEAD~N`, `^N`, `^{}` parsed via libgit2 as usual

Optional caps (future-friendly, default `NULL`):
- `depth_commits` to limit history, `depth_tree` to limit recursion (exposed as **named parameters** on functions if/when desired).

---

## 7) `_each` LATERAL pattern (unchanged)

- Keep `_each` variants; they accept `VARCHAR` inputs and **echo the input** first:
    - `git_tree_each(uri_or_repo VARCHAR, rev VARCHAR DEFAULT 'HEAD')`
    - `git_read_each(uri_or_path VARCHAR, …)`
- In runtime, each row is normalized via the same parser.
- This preserves per-row binding in LATERAL joins.

---

## 8) Error messages (teach by example)

- On invalid Git URIs, show both accepted syntaxes and the preferred emission:
    - *“Expected `git+file://repo#revspec[:path]` (preferred) or `git+file://repo/path@revspec`. `git://…` is accepted as input only; results are returned as `git+file://…`.”*

---

## 9) Examples (end-to-end)

```sql
-- List HEAD tree (preferred fragment form)
SELECT path, size, uri
FROM git_tree('git+file:///Users/alex/Dev/duck_tails#HEAD')
ORDER BY path;

-- Pipe directly to a reader
WITH T AS (
  SELECT uri
  FROM git_tree('git+file:///Users/alex/Dev/duck_tails#HEAD')
  WHERE path LIKE 'data/%.csv'
)
SELECT *
FROM T, LATERAL read_csv(T.uri);

-- Legacy input accepted, emitted as git+file://
SELECT uri
FROM git_tree('git:///Users/alex/Dev/duck_tails/docs/help.md@HEAD~1');
-- returns rows whose `uri` are git+file://…#HEAD~1:docs/help.md

-- Three-dot range (execution computes merge-base)
SELECT path, uri
FROM git_tree('git+file:///Users/alex/Dev/duck_tails#feature...main');
```

Colon-in-path (no encoding required):
```sql
SELECT *
FROM git_read_each('git+file:///repo#HEAD:docs/a:b:c.md');
-- revspec='HEAD', path='docs/a:b:c.md'
```

---

## 10) Test plan

- **Scheme acceptance:** `read_*` accept `git+file://…` and `git://…`; other schemes rejected/forwarded.
- **Emission:** `git_*` functions always emit **`git+file://…#revspec[:path]`** (never `git://`).
- **Round-trip:** URIs from `git_tree` feed into `read_text/read_csv/read_json_auto`.
- **Legacy parse:** `/path@revspec` input yields the same rows as the fragment form.
- **Colon split:** `#HEAD:docs/a:b.md` splits once; all other colons stay in path.
- **Ranges:** `A..B` and `A...B` sets match `git log` truth on a tiny fixture.
- **LATERAL:** `_each` with a mix of repo-paths, `git://`, and `git+file://` values works identically.

---

## 11) Minimal code touches

1. **Parser:** Extend `GitPath::Parse` to support schemes + split rules above; return absolute repo root.
2. **FS hook:** Recognize **`git+file://`** (and **`git://`**) and dispatch to libgit2 resolvers.
3. **Binders:** In every `git_*` entrypoint:
    - If the first arg looks like a Git URI, parse now and stash `GitUri` in bind data.
    - Else treat as repo path + optional `rev`; build a `GitUri`.
4. **Emitters:** When building result rows with URIs, format **only** as **`git+file://…#revspec[:path]`**.
5. **Readers:** No signature changes; they rely on FS interception.

---

## 12) Future-proofing

- If later we decide to support remote fetches or SSH, we can accept `git+ssh://…` / `git+https://…` **as inputs**, but still **emit** only `git+file://` once the repo is materialized locally.
- If we add pathspec magic (`:(glob)` etc.), it can live inside the `[:<pathspec>]` slot in the fragment form without changing parser shape.

---

### Bottom line

- **Users pass strings everywhere.**
- `read_*` accepts **`git+file://`** (preferred) and **`git://`** (input only).
- `git_*` functions **emit** only **`git+file://…#revspec[:path]`**, so downstream composition is consistent.
- Internally, we parse once and run through a single libgit2 execution path.