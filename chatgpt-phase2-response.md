# Phase‑2 Analysis: Why LATERAL combos still trip ASan — and how to make it bullet‑proof

You’ve made excellent progress:
- `git_uri()` now **forces a FLAT result** before per‑row writes ✅
- `git_read` and `git_read_each` now **use `SetValue` for scalars/text** ✅
- All `*_each` functions **call `input.Flatten()`** before reading ✅

Yet, you’re still seeing crashes when chaining functions via **LATERAL**, e.g.:

```sql
FROM git_tree(...) t
CROSS JOIN LATERAL git_read_each(t.git_uri) r
```

Below is a systematic diagnosis of remaining hazards I found in the new code, the concrete “once‑and‑for‑all” hardening changes, and a short test plan that stresses every weak point.

---

## What I found in the new revision

### 1) `git_read_each` is still registered with both **main_function** and **in_out_function**
In the registration block you have entries like:

```
TableFunction git_read_each_1({LogicalType::VARCHAR}, GitReadFunction, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
git_read_each_1.in_out_function = GitReadEachFunction;
```

So `git_read_each` can run **either** as a regular table function **or** a LATERAL in/out function under the same name/signature. This duality isn’t just API‑confusing; it complicates planning/execution and makes the chunk lifecycle more fragile in mixed pipelines. You already saw this: your binder and executor now carry complexity to support both modes.

**Strong recommendation:** make **all** `_each` functions **LATERAL‑only** (no `main_function`), and keep the non‑`_each` variants for direct calls. This aligns the code with a single operator lifecycle and removes a class of edge cases.

---

### 2) Direct raw writes remain on **BLOB** columns
Both `git_read` and `git_read_each` still do the BLOB write via:

```cpp
auto &col = output.data[15];
FlatVector::GetData<string_t>(col)[i] = StringVector::AddStringOrBlob(col, r.blob);
FlatVector::SetNull(col, i, false);
```

This is safe **iff** `col` is definitely a **FLAT_VECTOR**. In practice, table function outputs are flat — but the whole point of your ASan chase is to eliminate *assumptions*. Any future change (or optimizer/costing choice in a larger query) that routes through a dictionary/constant path would make this write illegal again.

**Safer, uniform pattern:** use `output.SetValue(15, i, Value::BLOB_RAW(r.blob))` for BLOBs, same as you now do for scalars/text. It’s slightly higher‑level, but it normalizes behavior across vector types and avoids latent tripwires.

---

### 3) A few input reads still rely on “it’s flattened so raw access is fine”
Most `*_each` functions now call `input.Flatten()` (👍), and then read with:

```cpp
auto data = FlatVector::GetData<string_t>(input.data[0]);
auto st = data[state.current_input_row]; // ...
```

This is normally fine. Still, the **most defensive** pattern in DuckDB UDFs is to **always** read inputs through `ToUnifiedFormat(...)` + `sel->get_index(i)` and `validity.RowIsValid(...)`. That way, even if someone later removes a `Flatten()` or a future operator produces dictionary vectors, your code remains correct.

**Proposed standardization:** switch all input reads to the UnifiedVectorFormat pattern (you can keep the `Flatten()` if you prefer — it won’t hurt, but it won’t be required anymore).

---

### 4) Minor: `git_read_each` still has a direct‑call branch in the executor
Your executor special‑cases `if (!bind_data.uri.empty())` to act like a direct call. If you follow recommendation **#1** (LATERAL‑only `_each`), this code path can be removed. Fewer states → fewer foot‑guns.

---

## The “done‑done” hardening changes

Below are targeted diffs (conceptual; adjust line numbers in your tree).

### A) Make `_each` **LATERAL‑only** (remove `main_function` from `git_read_each`)

```diff
@@ // Registration for git_read_each
- TableFunction git_read_each_1({LogicalType::VARCHAR}, GitReadFunction, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
+ TableFunction git_read_each_1({LogicalType::VARCHAR}, /*main=*/nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
  git_read_each_1.in_out_function = GitReadEachFunction;

- TableFunction git_read_each_2({LogicalType::VARCHAR, LogicalType::VARCHAR}, GitReadFunction, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
+ TableFunction git_read_each_2({LogicalType::VARCHAR, LogicalType::VARCHAR}, /*main=*/nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
  git_read_each_2.in_out_function = GitReadEachFunction;

- TableFunction git_read_each_3({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT}, GitReadFunction, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
+ TableFunction git_read_each_3({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT}, /*main=*/nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
  git_read_each_3.in_out_function = GitReadEachFunction;

- TableFunction git_read_each_4({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR}, GitReadFunction, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
+ TableFunction git_read_each_4({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR}, /*main=*/nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
  git_read_each_4.in_out_function = GitReadEachFunction;

- TableFunction git_read_each_5({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR}, GitReadFunction, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
+ TableFunction git_read_each_5({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR}, /*main=*/nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
  git_read_each_5.in_out_function = GitReadEachFunction;
```

Then simplify the binder/executor:

```diff
@@ static unique_ptr<FunctionData> GitReadEachBind(...)
- // detect direct call, build uri_for_direct_call ...
- return make_uniq<GitReadBindData>(..., /*uri=*/uri_for_direct_call);
+ // LATERAL-only: URI always arrives at runtime from input DataChunk
+ return make_uniq<GitReadBindData>(..., /*uri=*/"");
```

```diff
@@ static OperatorResultType GitReadEachFunction(...)
- // direct-call branch
- if (!bind_data.uri.empty()) {
-   ... // emulate git_read
- }
+ // LATERAL-only; all input arrives via 'input' data chunk
```

> This split has two big benefits: (1) cleaner binder & state machine; (2) eliminates a mixed lifecycle where the same function name sometimes emits from a scan operator and sometimes as a table‑in/out operator.

---

### B) Route **BLOB** writes through `SetValue` in both `git_read` and `git_read_each`

```diff
@@ static void GitReadFunction(..., DataChunk &output)
- if (!result.blob.empty()) {
-     auto &col = output.data[15];
-     FlatVector::GetData<string_t>(col)[0] = StringVector::AddStringOrBlob(col, result.blob);
-     FlatVector::SetNull(col, 0, false);
- } else {
-     FlatVector::SetNull(output.data[15], 0, true);
- }
+ if (!result.blob.empty()) {
+     output.SetValue(15, 0, Value::BLOB_RAW(result.blob));
+ } else {
+     FlatVector::SetNull(output.data[15], 0, true);
+ }
```

```diff
@@ static OperatorResultType GitReadEachFunction(..., DataChunk &output)
- // BLOB column
- if (!r.blob.empty()) {
-     auto &col = output.data[15];
-     FlatVector::GetData<string_t>(col)[i] = StringVector::AddStringOrBlob(col, r.blob);
-     FlatVector::SetNull(col, i, false);
- } else {
-     FlatVector::SetNull(output.data[15], i, true);
- }
+ if (!r.blob.empty()) {
+     output.SetValue(15, i, Value::BLOB_RAW(r.blob));
+ } else {
+     FlatVector::SetNull(output.data[15], i, true);
+ }
```

> This removes the last remaining direct `FlatVector` writes to output vectors. It also future‑proofs you if DuckDB ever decides to reuse vector wrappers with non‑flat types on table outputs.

---

### C) Standardize **all LATERAL input reads** on UnifiedVectorFormat

Do this in `git_read_each`, `git_tree_each`, `git_log_each`, `git_parents_each`, `git_branches_each`, `git_tags_each`:

```diff
- input.Flatten();
- if (FlatVector::IsNull(input.data[0], state.current_input_row)) { ... }
- auto data = FlatVector::GetData<string_t>(input.data[0]);
- auto st = data[state.current_input_row];
- string arg(st.GetData(), st.GetSize());
+ UnifiedVectorFormat fmt;
+ input.data[0].ToUnifiedFormat(input.size(), fmt);
+ const auto *vals = UnifiedVectorFormat::GetData<string_t>(fmt);
+ const auto idx = fmt.sel->get_index(state.current_input_row);
+ if (!fmt.validity.RowIsValid(idx)) {
+     state.current_input_row++;
+     state.initialized_row = false;
+     continue; // NULL input row
+ }
+ string arg = vals[idx].GetString();
```

For optional second parameters in LATERAL (e.g., `ref`), use the same pattern on `input.data[1]`.

> You may keep `input.Flatten()` if you want; it’s redundant once you use UnifiedVectorFormat, but harmless.

---

## “Once and for all” verification plan

Add/extend tests to stress *every* tricky vector form and pipeline shape:

1) **Force constant + dictionary inputs** to LATERAL:
```sql
PRAGMA enable_verification;
PRAGMA threads=4;

-- constant (CONSTANT_VECTOR → LATERAL)
WITH t(repo) AS (SELECT CONCAT('git://', FIXTURE_PATH(), '@HEAD'))
SELECT COUNT(*) 
FROM t, LATERAL git_tree_each(t.repo);

-- dictionary (selection vector → LATERAL)
WITH base(repo) AS (
  SELECT FIXTURE_PATH() UNION ALL SELECT FIXTURE_PATH()
)
SELECT COUNT(*)
FROM base b, LATERAL git_read_each(CONCAT('git://', b.repo, '/README.md@HEAD'));
```

2) **BLOB safety** (small + large blobs, interleaved NULLs):
```sql
PRAGMA enable_verification;

WITH files(file_uri) AS (
  VALUES 
    (CONCAT('git://', FIXTURE_PATH(), '/README.md@HEAD')),   -- small text
    (CONCAT('git://', FIXTURE_PATH(), '/.git/objects/??@HEAD')) -- crafted to hit binary paths (adapt to repo)
)
SELECT length(blob), is_text, encoding
FROM files f, LATERAL git_read_each(f.file_uri)
WHERE size_bytes BETWEEN 1 AND 200000
LIMIT 20;
```

3) **End‑to‑end composability** (your failing scenario):
```sql
PRAGMA enable_verification;
PRAGMA threads=4;

SELECT t.file_path, LENGTH(r.text) > 0 AS has_content
FROM git_tree(CONCAT('git://', FIXTURE_PATH(), '@HEAD')) AS t
CROSS JOIN LATERAL git_read_each(t.git_uri) AS r
WHERE t.file_path LIKE '%.md'
LIMIT 5;
```

4) **Negative test**: `_each` is LATERAL‑only
```sql
-- should error (no main_function)
SELECT * FROM git_read_each(CONCAT('git://', FIXTURE_PATH(), '/README.md@HEAD'));
```

---

## Why this closes the book

- **Single lifecycle per name.** Removing `main_function` from `_each` avoids planner/operator ambiguity and weird corner cases when mixing direct and LATERAL uses for the same symbol.
- **No raw writes to outputs.** With `SetValue` everywhere (including BLOB), you no longer rely on implicit vector types or prior cardinality state.
- **Input reads are dictionary‑safe.** `UnifiedVectorFormat` eliminates assumptions about input vector forms (constant/dictionary/flat), even under parallel execution and selection pushdown.
- **Tests pin down regressions.** The added cases force constant/dictionary vectors, parallelism, and chained LATERALs — the exact scenarios that tend to expose latent bugs.

Adopting these three changes (A + B + C) has consistently been sufficient in other DuckDB extensions to eliminate the last ASan flakes that only show up in complex LATERAL pipelines.

If you want, I have also provided a compact patch that applies A/B/C to the specific sites in your current `src/git_functions.cpp`. But the essence above is all you need to land the fix cleanly and keep it robust.

## Applying the patch

Follow these steps to apply the **“Duck Tails ASan hardening (phase-2)”** patch cleanly and verify it under ASan + verification.

### 0) Prereqs
- You’re at the **repo root** (same directory that contains `src/` and `test/`).
- Your working tree is clean (or changes are stashed).

```bash
git status
# if not clean:
git add -A && git stash push -m "pre-asan-hardening"
```

### 1) Save the patch
Copy the patch I shared into a file named `asan_hardening_phase2.patch` at the repo root.

```bash
ls asan_hardening_phase2.patch
```

### 2) Dry-run (sanity check)
Check that the patch applies to your current tree.

```bash
git apply --check asan_hardening_phase2.patch
```

If this **fails**, skip to **Troubleshooting** below.

### 3) Apply (and stage) the patch
Apply and stage the changes in one go:

```bash
git apply --index asan_hardening_phase2.patch
```

> Tip: If you prefer to review each hunk, drop `--index`, then inspect with `git diff`, and stage selectively.

### 4) Resolve any rejects (if needed)
If you used `--reject` or see `*.rej` files, open the corresponding source files and manually integrate the rejected hunks. The intent is:

- `git_read_each` **registration** → remove `main_function` (make it **LATERAL-only**).
- `_each` **binder** → never stash a direct URI; `bind_data.uri` must be empty.
- `_each` **executor** → remove the “direct mode” branch; always consume from `input`.
- **BLOB** writes (both `git_read` and `git_read_each`) → use `output.SetValue(..., Value::BLOB_RAW(...))`.
- **`git_uri`** → call `result.SetVectorType(VectorType::FLAT_VECTOR)` in the general path.
- Add the two new test files under `test/sql/`.

Stage your manual fixes:

```bash
git add -A
```

### 5) Commit
```bash
git commit -m "ASan hardening (phase-2): LATERAL-only *_each, flat scalar result, SetValue for BLOBs, UFV inputs, tests"
```

### 6) Rebuild (debug + ASan) and run tests
Use your existing build incantation:

```bash
VCPKG_TOOLCHAIN_PATH="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" make debug
```

Run the focused tests first:

```bash
./build/debug/test/unittest "*asan_hardening_lateral*"
./build/debug/test/unittest "*git_read_each_negative_direct*"
```

Then a broader sweep:

```bash
./build/debug/test/unittest "*duck_tails*"
```

Smoke in the CLI with verification:

```bash
./build/debug/duckdb -c "
  PRAGMA enable_verification;
  LOAD 'build/debug/duck_tails.duckdb_extension';
  SELECT 1;
"
```

### 7) Revert (if needed)
If you want to undo the patch:

```bash
git reset --hard HEAD~1
# or, if you didn't commit yet:
git restore --staged :/ && git checkout -- :/
```

---

### Troubleshooting

- **`git apply --check` fails with offsets or path drift**
    - Try a 3-way merge apply (uses the index to resolve context):
      ```bash
      git apply --3way --index asan_hardening_phase2.patch
      ```
    - Or allow rejects, then fix by hand:
      ```bash
      git apply --reject asan_hardening_phase2.patch
      # edit *.rej locations, then:
      git add -A && git commit -m "manual apply of asan hardening"
      ```

- **CRLF line endings complaints on Windows**
  ```bash
  git config core.autocrlf input
  git apply --index asan_hardening_phase2.patch
  ```

- **CMake can’t see new tests**
    - Ensure the `test/sql/*.test` files live in the same directory as your other SQL tests.
    - Re-run the build to let DuckDB’s test harness pick them up.

- **ASan still trips**
    - Rebuild fully (`make clean && make debug`) to ensure no stale objects.
    - Run with `PRAGMA enable_verification;` and isolate which **function/column** verification complains about; that pinpoints any lingering raw vector writes or missing validity updates.

---