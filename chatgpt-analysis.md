# Resolving the ASan “memory corruption” in `duck_tails`
**Scope:** `git_read_each` (table in/out) and `git_uri` (scalar UDF)  
**Status:** Root causes identified; deterministic fixes + tests below.

---

## TL;DR (do these first)

1) **Scalar UDF `git_uri`:** before writing per-row results, force the **result** vector to `FLAT_VECTOR`.

```cpp
// inside GitUriFunction(...) general (non-constant) path:
result.SetVectorType(VectorType::FLAT_VECTOR);     // ← critical
auto result_data = FlatVector::GetData<string_t>(result);
// ... write result_data[i] = StringVector::AddString(result, uri);
```

2) **Table in/out `git_read_each`:**
    - **Do not call** `output.Reset()`
    - Write rows, then `output.SetCardinality(count)`
    - Prefer `output.SetValue(col,row, ...)` for safety; if writing via `FlatVector::GetData`, explicitly manage **validity**.
    - For the BLOB column, use `StringVector::AddStringOrBlob` and mark non-null.

```cpp
// emit loop
output.SetValue(0,  i, Value(r.git_uri));
...
if (!r.blob.empty()) {
  auto &col = output.data[15];
  FlatVector::GetData<string_t>(col)[i] = StringVector::AddStringOrBlob(col, r.blob);
  FlatVector::SetNull(col, i, false);
} else {
  FlatVector::SetNull(output.data[15], i, true);
}
...
output.SetCardinality(output_count);
```

3) **API surface:** keep all `*_each` functions **LATERAL-only** (in_out_function), and use non-`_each` names for direct scans. Remove any lingering “direct call” flow from the `_each` binders/tests.

---

## What was happening & why it was flaky

### Symptom
- Under ASan (AddressSanitizer) with verification enabled, queries involving your functions crashed later in the pipeline with UTF-8 or vector verification errors (frequently observed in `Utf8Proc::Analyze` → `DataChunk::Verify` stacks).

### Root causes (two distinct issues)
1) **`git_uri` scalar UDF wrote into a non-flat result vector.**  
   DuckDB may hand scalar UDFs a `CONSTANT_VECTOR` or dictionary-backed result. Writing row-by-row without first switching the **result** to flat leads to out-of-bounds or misaddressed writes → hidden heap corruption → crash downstream.

2) **`git_read_each` previously combined `output.Reset()` with raw vector writes.**  
   `Reset()` clears cardinality/validity. If you then write via `FlatVector::GetData` without re-establishing validity (or you set cardinality afterward), ASan rightfully catches reads/writes to poisoned slots.  
   (You already stopped doing this, but the scalar bug above can independently corrupt memory.)

---

## Minimal, concrete fixes

### 1) Patch `git_uri` (scalar UDF)

**Before (problem):** writing into `result` as if it were flat.

**After (safe):** force `FLAT_VECTOR` in the general path *before* row writes.

```diff
static void GitUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    // fast-path: all three inputs constant → you already set CONSTANT and return
    // ...

    // general path: one or more inputs non-constant
+   result.SetVectorType(VectorType::FLAT_VECTOR); // ensure writeable per-row storage

    UnifiedVectorFormat repo_path_fmt, file_path_fmt, ref_fmt;
    args.data[0].ToUnifiedFormat(args.size(), repo_path_fmt);
    args.data[1].ToUnifiedFormat(args.size(), file_path_fmt);
    args.data[2].ToUnifiedFormat(args.size(), ref_fmt);

    auto repo_data = UnifiedVectorFormat::GetData<string_t>(repo_path_fmt);
    auto file_data = UnifiedVectorFormat::GetData<string_t>(file_path_fmt);
    auto ref_data  = UnifiedVectorFormat::GetData<string_t>(ref_fmt);

    auto out = FlatVector::GetData<string_t>(result);
    for (idx_t i = 0; i < args.size(); i++) {
        const auto ri = repo_path_fmt.sel->get_index(i);
        const auto fi = file_path_fmt.sel->get_index(i);
        const auto ci = ref_fmt.sel->get_index(i);

        if (!repo_path_fmt.validity.RowIsValid(ri) ||
            !file_path_fmt.validity.RowIsValid(fi) ||
            !ref_fmt.validity.RowIsValid(ci)) {
            FlatVector::SetNull(result, i, true);
            continue;
        }

        const auto repo = repo_data[ri];
        const auto file = file_data[fi];
        const auto ref  = ref_data[ci];

        const string uri = BuildGitURI(repo, file, ref); // your helper
        out[i] = StringVector::AddString(result, uri);
    }
}
```

**Why it works:** you guarantee the result has per-row mutable storage, independent of planner choices (constant folding, dictionary selection vectors, etc.).

---

### 2) Patch `git_read_each` (table in/out UDF)

**Key rules to follow in all in_out functions:**
- Don’t `Reset()` the output chunk.
- Either:
    - use `output.SetValue(col,row, ...)` (this updates validity for you), **or**
    - if writing via `FlatVector::GetData`, also set validity bits (`FlatVector::SetNull(..., false)` for non-nulls).
- Set cardinality **after** filling `output_count` rows.

**Reference implementation snippet (core emit loop):**
```cpp
idx_t output_count = 0;
while (state.current_output_row < state.current_results.size() &&
       output_count < STANDARD_VECTOR_SIZE) {
    const auto &r = state.current_results[state.current_output_row];

    // Scalars
    output.SetValue(0,  output_count, Value(r.git_uri));
    output.SetValue(1,  output_count, Value(r.repo_path));
    output.SetValue(2,  output_count, Value(r.commit_hash));
    output.SetValue(3,  output_count, Value(r.tree_hash));
    output.SetValue(4,  output_count, Value(r.file_path));
    output.SetValue(5,  output_count, Value(r.file_ext));
    output.SetValue(6,  output_count, Value(r.ref));
    output.SetValue(7,  output_count, Value(r.blob_hash));
    output.SetValue(8,  output_count, Value::INTEGER(r.mode));
    output.SetValue(9,  output_count, Value(r.kind));
    output.SetValue(10, output_count, Value::BOOLEAN(r.is_text));
    output.SetValue(11, output_count, Value(r.encoding));
    output.SetValue(12, output_count, Value::BIGINT(r.size_bytes));
    output.SetValue(13, output_count, Value::BOOLEAN(r.truncated));

    // TEXT column
    if (!r.text.empty()) {
        output.SetValue(14, output_count, Value(r.text));
    } else {
        FlatVector::SetNull(output.data[14], output_count, true);
    }

    // BLOB column (heap-backed)
    if (!r.blob.empty()) {
        auto &col = output.data[15];
        FlatVector::GetData<string_t>(col)[output_count] =
            StringVector::AddStringOrBlob(col, r.blob);
        FlatVector::SetNull(col, output_count, false);
    } else {
        FlatVector::SetNull(output.data[15], output_count, true);
    }

    output_count++;
    state.current_output_row++;
}

output.SetCardinality(output_count);
```

**Binder/registration:** make `git_read_each` **in_out_function only**. Remove “direct call” handling from the bind data; keep `git_read` as the direct table function.

---

## Test plan (to be “done-done”)

### Build & run
```bash
# Debug with ASan (your existing pattern):
VCPKG_TOOLCHAIN_PATH="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" make debug

# Full test sweep (replace filters as needed):
./build/debug/test/unittest "*git_uri*"
./build/debug/test/unittest "*git_read_each*"
./build/debug/test/unittest "*duck_tails*"

# CLI smoke with verification on:
./build/debug/duckdb -c "
  PRAGMA enable_verification;
  LOAD 'build/debug/duck_tails.duckdb_extension';
  SELECT 1;
"
```

### New tests to add (or expand)

1) **`git_uri` mixed formats (forces non-constant result and selection vectors):**
```sql
PRAGMA enable_verification;

-- mixed NULLs and non-NULLs, forces non-constant path
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

2) **`git_read_each` is LATERAL-only** (negative test):
```sql
-- should error: direct call to *_each
SELECT * FROM git_read_each('/tmp/repo','HEAD');
```
Assert: error message instructs to use `git_read`.

3) **`git_read_each` positive with LATERAL:**
```sql
PRAGMA enable_verification;
WITH inputs(repo, ref) AS (VALUES ('/tmp/repo','HEAD'))
SELECT r.file_path, r.blob_hash
FROM inputs i, LATERAL git_read_each(i.repo, i.ref) AS r
WHERE r.file_ext = 'md'
LIMIT 5;
```

4) **Concurrency / multiple chunks:**
```sql
PRAGMA enable_verification;
PRAGMA threads=4;
WITH inputs(repo, ref) AS (VALUES ('/tmp/repo','HEAD'),('/tmp/repo','HEAD'))
SELECT count(*) FROM inputs i, LATERAL git_read_each(i.repo, i.ref);
```

5) **String/UTF-8 verification on TEXT outputs:**
```sql
PRAGMA enable_verification;
SELECT LENGTH(r.text)
FROM (VALUES ('/tmp/repo','HEAD')) i(repo,ref),
LATERAL git_read_each(i.repo, i.ref) r
WHERE r.is_text AND r.size_bytes < 50000
LIMIT 10;
```

---

## Guardrails & best practices (to avoid regressions)

- **Scalar/vectorized UDFs**
    - If you write row-by-row to `result`, call `result.SetVectorType(FLAT_VECTOR)` first.
    - When inputs vary in const-ness, always go through `ToUnifiedFormat` and index via the selection vector.

- **Table functions (direct)**
    - Set `output.SetCardinality(n)` after writing `n` rows.
    - Use `SetValue` unless you’re deliberately managing validity and string heaps yourself.

- **Table in/out (`*_each`)**
    - Never call `output.Reset()`; the engine handles chunk lifecycle.
    - Flatten `input` at the start: `input.Flatten();`
    - Treat each input row as a stateful mini-scan; emit up to `STANDARD_VECTOR_SIZE` rows; advance counters; set cardinality; return `HAVE_MORE_OUTPUT` / `NEED_MORE_INPUT` appropriately.

- **Strings & BLOBs**
    - Use `StringVector::AddString` / `AddStringOrBlob` for heap-backed storage.
    - Explicitly set validity when bypassing `SetValue`.

- **API shape**
    - Keep `_each` strictly LATERAL and non-`_each` for direct scans.
    - Align binder and executor with that rule; update tests accordingly.

- **Verification**
    - Keep `PRAGMA enable_verification;` in critical tests; it catches invariants regressions early.
    - Run ASan builds regularly; these are exactly the bugs ASan is meant to surface.

---

## Appendix A – Suggested diffs (high-level)

> Adjust line numbers to your tree; these are conceptual patches.

### A1) `git_uri` result flattening

```diff
- // general path
- auto result_data = FlatVector::GetData<string_t>(result);
+ // general path
+ result.SetVectorType(VectorType::FLAT_VECTOR);
+ auto result_data = FlatVector::GetData<string_t>(result);
```

### A2) `git_read_each` emit loop & BLOB handling

```diff
- output.Reset(); // remove any resets
  // ...
- output.SetValue(15, i, Value::BLOB_RAW(result.blob));
+ if (!result.blob.empty()) {
+   auto &col = output.data[15];
+   FlatVector::GetData<string_t>(col)[i] =
+       StringVector::AddStringOrBlob(col, result.blob);
+   FlatVector::SetNull(col, i, false);
+ } else {
+   FlatVector::SetNull(output.data[15], i, true);
+ }
  // ...
+ output.SetCardinality(output_count);
```

### A3) Registration / binder cleanup for `_each`
- Register `_each` as **only** `in_out_function`.
- Remove any bind-time branches that parse a direct URI for `_each`.

---

## Appendix B – Minimal mental model (DuckDB vector types)

- `FLAT_VECTOR` → contiguous, per-row slots; safe for row writes.
- `CONSTANT_VECTOR` → one slot shared for all rows; **never** write row-by-row unless you first switch to flat.
- Dictionary (selection) → indirection via selection vector; reading is via `ToUnifiedFormat`; writing to **result** still requires a flat result.

---

## Final checklist

- [ ] `git_uri` sets result to FLAT in general path.
- [ ] `git_read_each` never calls `output.Reset()`.
- [ ] `git_read_each` sets cardinality *after* writes.
- [ ] BLOB column uses `AddStringOrBlob` + validity set.
- [ ] All `_each` are LATERAL-only; tests updated.
- [ ] Suite passes with `PRAGMA enable_verification;` and ASan debug build.

If any test still trips ASan after these changes, capture the failing **function name + column index** from `DataChunk::Verify` and we can instrument just that output path with extra validity assertions to pinpoint stragglers.