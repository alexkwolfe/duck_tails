# Duck Tails — ASan Hardening & LATERAL Safety Playbook

**Goal:** Eliminate AddressSanitizer crashes and data-chunk corruption in the DuckDB extension by enforcing safe vector semantics, clean LATERAL-only `_each` operators, and robust tests.

---

## TL;DR

- Make **all** `*_each` functions **LATERAL-only** (no `main_function` for `_each` names).
- In **scalar UDFs** that write per-row (e.g., `git_uri`), call:
  ```cpp
  result.SetVectorType(VectorType::FLAT_VECTOR);
  ```
  before writing `result[i]`.
- Prefer `output.SetValue(col, row, ...)` for table/in-out functions.  
  If you must use `FlatVector::GetData`, you **must** also set validity (and never assume FLAT unless you set it).
- For **BLOB** outputs, use `output.SetValue(..., Value::BLOB_RAW(...))`.
- Read **inputs** using `ToUnifiedFormat` + selection index + validity checks.
- Add tests that force **CONSTANT** and **dictionary** vectors; chain operators via **LATERAL** under parallelism; enable **verification**.

---

## What This Patch Set Changes (High-Level)

1. **Registration:**  
   `git_read_each` (all arities) registered **only** as `in_out_function` (LATERAL-only). No `main_function`.

2. **Binders:**  
   `_each` binders do **not** attempt “direct call” detection or stash a URI; runtime input always comes via the `DataChunk`.

3. **Executors:**  
   `_each` executors remove any “direct” branch; they always read from `input` and emit via a single, consistent state machine:
    - Read inputs with `UnifiedVectorFormat`
    - Fill rows
    - `output.SetCardinality(n)`

4. **BLOB writes:**  
   Use `SetValue(..., Value::BLOB_RAW(...))` for BLOB outputs in both `git_read` and `git_read_each`.

5. **Scalar UDFs:**  
   `git_uri` forces **FLAT** result in the general path (`result.SetVectorType(FLAT_VECTOR)`) before per-row writes.

---

## Applying the Patch

Follow these steps to apply the **“Duck Tails ASan hardening (phase-2)”** patch and verify it under ASan + verification.

### 0) Prereqs
- You’re at the **repo root** (contains `src/` and `test/`).
- Working tree is clean (or stashed).

```bash
git status
# if not clean:
git add -A && git stash push -m "pre-asan-hardening"
```

### 1) Save the patch
Paste the patch content you received into a file named `asan_hardening_phase2.patch` at the repo root.

```bash
ls asan_hardening_phase2.patch
```

### 2) Dry-run (sanity check)
```bash
git apply --check asan_hardening_phase2.patch
```

If this fails, jump to **Troubleshooting**.

### 3) Apply (and stage) the patch
```bash
git apply --index asan_hardening_phase2.patch
```

> Prefer reviewing hunks? Drop `--index`, run `git diff`, then stage selectively.

### 4) Resolve any rejects (if needed)
If you see `*.rej` files (you used `--reject` or context drifted), manually integrate the hunks. The intended effects:

- **Registration:** `git_read_each` has **no `main_function`**; only `in_out_function`.
- **Binder:** `_each` never stashes a direct URI; `bind_data.uri` is always empty for `_each`.
- **Executor:** `_each` never branches on `bind_data.uri`; it always reads from `input`.
- **BLOB writes:** use `SetValue(..., Value::BLOB_RAW(...))` everywhere.
- **Scalar UDFs:** general path calls `result.SetVectorType(FLAT_VECTOR)`.

Then:

```bash
git add -A
```

### 5) Commit
```bash
git commit -m "ASan hardening: LATERAL-only *_each, FLAT scalar results, SetValue for BLOBs, UFV inputs, tests"
```

### 6) Rebuild (debug + ASan) and run tests
Use your existing build:

```bash
VCPKG_TOOLCHAIN_PATH="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" make debug
```

Run focused tests first:

```bash
./build/debug/test/unittest "*asan_hardening_lateral*"
./build/debug/test/unittest "*git_read_each_negative_direct*"
```

Then run your broader suite:

```bash
./build/debug/test/unittest "*duck_tails*"
```

Smoke test in the CLI with verification:

```bash
./build/debug/duckdb -c "
  PRAGMA enable_verification;
  LOAD 'build/debug/duck_tails.duckdb_extension';
  SELECT 1;
"
```

### 7) Revert (if needed)
```bash
git reset --hard HEAD~1
# or, if uncommitted:
git restore --staged :/ && git checkout -- :/
```

---

## Verification & Test Plan

**Compiler/Runtime:**
- Build **debug** with **ASan** enabled (your current debug build does this).
- Run with `PRAGMA enable_verification;`.
- Exercise with `PRAGMA threads=4` (or `8`) to stress chunking and interleaving.

**Focused SQL tests (examples):**

```sql
-- Enable verification (catches vector invariants)
PRAGMA enable_verification;
```

**1) Constant → LATERAL**
```sql
WITH t(repo) AS (
  SELECT CONCAT('git://', FIXTURE_PATH(), '@HEAD')
)
SELECT COUNT(*) FROM t, LATERAL git_tree_each(t.repo);
```

**2) Dictionary (selection) → LATERAL**
```sql
WITH base(repo) AS (
  SELECT FIXTURE_PATH() UNION ALL SELECT FIXTURE_PATH()
)
SELECT COUNT(*)
FROM base b, LATERAL git_read_each(CONCAT('git://', b.repo, '/README.md@HEAD'));
```

**3) Chained operators under parallelism**
```sql
PRAGMA threads=4;
SELECT COUNT(*)
FROM git_tree(CONCAT('git://', FIXTURE_PATH(), '@HEAD')) t
CROSS JOIN LATERAL git_read_each(t.git_uri) r
WHERE t.file_path LIKE '%.md';
```

**4) Negative: `_each` must be LATERAL-only**
```sql
SELECT * FROM git_read_each(CONCAT('git://', FIXTURE_PATH(), '/README.md@HEAD'));
-- Expect error instructing to use git_read(...) for direct calls
```

**5) `git_uri` mixed inputs (forces non-flat result path)**
```sql
WITH t(repo, file, ref) AS (
  VALUES
    ('/tmp/repo','README.md','HEAD'),
    (NULL       ,'README.md','HEAD'),
    ('/tmp/repo',NULL       ,'HEAD'),
    ('/tmp/repo','README.md',NULL)
)
SELECT git_uri(repo, file, ref) AS u FROM t;
```

Expected:
```
/tmp/repo/README.md@HEAD
NULL
NULL
NULL
```

---

## Rules of the Road (Keep These)

1. **Scalar UDFs**
    - Writing per-row? → `result.SetVectorType(FLAT_VECTOR)` first.
    - Read inputs via `ToUnifiedFormat` + `sel->get_index(i)` + validity checks.

2. **Table functions (direct) & In-Out (LATERAL)**
    - **Never** call `output.Reset()` inside your function.
    - Fill rows → `output.SetCardinality(n)` **once** per emission.
    - Prefer `output.SetValue(...)` for safety and validity management.

3. **Strings/Blobs**
    - Strings: `StringVector::AddString` (or `SetValue` with `Value(str)`).
    - Blobs: `SetValue(..., Value::BLOB_RAW(...))`.  
      If you must use `AddStringOrBlob`, also flip validity to non-null.

4. **API Surface**
    - `_each` = **LATERAL-only**; direct scans use non-`_each` names.
    - Align schemas and option names across pairs (predictable UX).

5. **Binder/Executor**
    - Binder: parse named options & declare schema; don’t “peek” runtime inputs for `_each`.
    - Executor: Prefer `UnifiedVectorFormat`; optionally keep `input.Flatten()` as a belt-and-suspenders.

6. **CI & Sanitizers**
    - Keep ASan/UBSan on in debug CI. Consider TSan if you introduce shared state.

---

## Troubleshooting

- **`git apply --check` fails**  
  Use 3-way apply (leverages index to reconcile drift):
  ```bash
  git apply --3way --index asan_hardening_phase2.patch
  ```
  Or accept rejects and fix by hand:
  ```bash
  git apply --reject asan_hardening_phase2.patch
  # edit *.rej locations, then:
  git add -A && git commit -m "manual apply of asan hardening"
  ```

- **ASan still trips after patch**
    - `make clean && make debug` (no stale objects).
    - Run with `PRAGMA enable_verification;`.
    - Note the **function/column** and **vector type** in the failure; that pinpoints missing validity updates or raw writes.

- **CMake/harness doesn’t see tests**  
  Ensure test files are placed under the same `test/sql/` path used by your suite and rebuild.

---

## Appendix: Minimal Patterns

**UnifiedVectorFormat read:**
```cpp
UnifiedVectorFormat fmt;
vec.ToUnifiedFormat(count, fmt);
auto *vals = UnifiedVectorFormat::GetData<string_t>(fmt);
for (idx_t i = 0; i < count; i++) {
  const auto idx = fmt.sel->get_index(i);
  if (!fmt.validity.RowIsValid(idx)) continue;
  auto s = vals[idx].GetString();
  // ...
}
```

**Safe emit loop (table/in-out):**
```cpp
idx_t out_count = 0;
while (have_more && out_count < STANDARD_VECTOR_SIZE) {
  output.SetValue(0, out_count, Value(row.col0));
  // ...
  if (has_blob) output.SetValue(15, out_count, Value::BLOB_RAW(blob));
  else          FlatVector::SetNull(output.data[15], out_count, true);
  out_count++;
}
output.SetCardinality(out_count);
```

**Scalar per-row writes:**
```cpp
result.SetVectorType(VectorType::FLAT_VECTOR);
auto out = FlatVector::GetData<string_t>(result);
// fill out[i] with StringVector::AddString(result, ...)
```

---

**Done.** With these changes, `_each` call-shapes are unambiguous, vector writes are safe across all forms (FLAT/CONSTANT/dictionary), and tests catch regressions early—so ASan stays quiet.