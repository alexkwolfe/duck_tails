# duck_tails: `git_read` Function — Product Requirements Document (PRD)

## 1. Overview
Enhance the `duck_tails` extension by adding a table function `git_read` that allows reading Git blobs directly from queries. This removes the current limitation where DuckDB’s built-in `read_*` functions only accept literal arguments.

The new function should:
- Accept URIs built at query time (e.g. `'git://path/to/file@commit'`).
- Determine the Git file **mode** and classify it (file, symlink, tree, submodule).
- Return both **text** and **binary** content in one schema, with a clear discriminator.
- Work seamlessly with `JOIN LATERAL` for per-row reads, and (optionally) accept a relation of URIs for bulk reads.

---

## 2. Goals
- One SQL query can **find matching files and read their content**.
- Always return a consistent schema with `text` and `blob` columns.
- Make it trivial to insert into tables with separate `TEXT` and `BLOB` columns.
- Handle symlinks, submodules, trees gracefully.
- Offer parameters for `max_bytes`, base64 decoding, and transcoding.

---

## 3. Non-Goals
- No Git write support (commits, branches).
- No Git LFS object resolution (beyond pointer recognition).
- No auto-application of Git smudge/clean filters unless explicitly requested.
- No guaranteed parallelism in Python UDFs (native C++ only).

---

## 4. User Stories
- *As a developer*, I can query a Git tree and insert source code contents into a `TEXT` column.
- *As a data engineer*, I can capture binary assets into a `BLOB` column while skipping text files.
- *As a doc indexer*, I can filter only rows with `is_text=TRUE` to build a search index.

---

## 5. API Surface

### 5.1 Function Signature
```sql
git_read(
  uri VARCHAR,
  max_bytes BIGINT DEFAULT NULL,
  decode_base64 VARCHAR DEFAULT 'auto', -- 'auto' | 'always' | 'never'
  transcode VARCHAR DEFAULT 'utf8',     -- 'utf8' | 'raw'
  filters VARCHAR DEFAULT 'raw'         -- 'raw' | 'worktree' (future)
)
RETURNS TABLE (
  uri        VARCHAR,
  mode       INTEGER,
  kind       VARCHAR,   -- 'file' | 'symlink' | 'tree' | 'submodule'
  is_text    BOOLEAN,
  encoding   VARCHAR,
  size_bytes BIGINT,
  truncated  BOOLEAN,
  text       TEXT,
  blob       BLOB,
  note       VARCHAR
);
```

---

## 6. Semantics
- **Mode/kind detection**:
    - `100644/100755`: regular file → may be text or binary.
    - `120000`: symlink → `text=target path`, `is_text=TRUE`.
    - `040000`: tree → metadata only.
    - `160000`: submodule → metadata only.
- **Text vs binary**:
    1. Prefer `.gitattributes` if available (future).
    2. Heuristics (NUL/control chars, BOM/encoding sniff).
    3. If decodable → `is_text=TRUE`, fill `text`. Else `is_text=FALSE`, fill `blob`.
- **Base64**:
    - `auto`: decode only if strongly looks like base64.
    - `always`: decode unconditionally.
    - `never`: don’t decode.
- **max_bytes**: apply to raw bytes before decoding; set `truncated=TRUE` if exceeded.
- **Errors**: return row with `note='error:<msg>'`, leave `text`/`blob` NULL.

---

## 7. Example Usage

### Insert into dual-typed table
```sql
WITH commit_one AS (SELECT commit_hash FROM commit LIMIT 1),
candidates AS (
  SELECT t.repo_id, t.file_hash, t.size,
         'git://' || t.path || '@' || commit_one.commit_hash AS uri
  FROM tree t
  CROSS JOIN commit_one
  LEFT JOIN blobs b ON b.repo_id=t.repo_id AND b.file_hash=t.file_hash
  WHERE b.file_hash IS NULL
)
INSERT INTO blobs (repo_id, file_hash, size, content_text, content_blob)
SELECT c.repo_id, c.file_hash, c.size, r.text, r.blob
FROM candidates c
JOIN LATERAL git_read(c.uri, 1000000) AS r ON TRUE
WHERE r.kind='file';
```

### Index only text files
```sql
INSERT INTO text_index (repo_id, file_hash, text)
SELECT c.repo_id, c.file_hash, r.text
FROM candidates c
JOIN LATERAL git_read(c.uri, 1000000) AS r ON TRUE
WHERE r.is_text;
```

### Save binaries
```sql
INSERT INTO binaries (repo_id, file_hash, blob)
SELECT c.repo_id, c.file_hash, r.blob
FROM candidates c
JOIN LATERAL git_read(c.uri, 50_000_000, 'auto', 'raw') AS r ON TRUE
WHERE NOT r.is_text;
```

---

## 8. Performance & Concurrency
- Implement in native C++ inside `duck_tails`.
- LATERAL enables per-row URIs; not inherently parallel, but internal batching/parallelism can be added.
- Relation-overload (`git_read((SELECT uri FROM ...), ...)`) can enable bulk I/O and parallel reads.
- Enforce `max_bytes` for large blobs.

---

## 9. Security
- Sanitize `uri` (`git://path@commit`) to prevent traversal.
- Respect DuckDB’s unsigned extension policies (explicit user consent).
- Guard against zip-bombs/large encodings.
- Fail gracefully with clear diagnostics.

---

## 10. Edge Cases
- Git LFS pointers: detect and set `note='lfs-pointer'`.
- Accidental UTF-8 binaries: prefer `is_text=FALSE` if ambiguous.
- Trees and submodules: metadata-only rows.
- Huge files: `truncated=TRUE`.

---

## 11. Testing Plan
- Fixture repo with UTF-8, UTF-16, binary PNG, base64, symlink, submodule, LFS pointer, large file.
- Unit tests for mode detection, text vs binary, base64 behavior, truncation.
- Integration tests with `tree` + `JOIN LATERAL`.
- Benchmarks for thousands of files.

---

## 12. Success Criteria
- One-query ingestion of text + binary into distinct columns.
- < 2% misclassification of text vs binary on test corpus.
- Handles 10k small files without OOM.
- Inserts align with expected DuckDB column types.

---

## 13. C++ Implementation Skeleton

```cpp
// git_read.cpp
#include "duckdb.hpp"
using namespace duckdb;

// Bind data
struct GitReadBindData : public TableFunctionData {
    int64_t max_bytes;
    string decode_base64;
    string transcode;
    string filters;
};

// Bind
static unique_ptr<FunctionData> GitReadBind(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &names) {

    auto bind = make_uniq<GitReadBindData>();
    bind->max_bytes = input.inputs.size() >= 2 && !input.inputs[1].IsNull()
        ? input.inputs[1].GetValue<int64_t>() : -1;
    bind->decode_base64 = (input.inputs.size() >= 3) ? input.inputs[2].GetValue<string>() : "auto";
    bind->transcode = (input.inputs.size() >= 4) ? input.inputs[3].GetValue<string>() : "utf8";
    bind->filters   = (input.inputs.size() >= 5) ? input.inputs[4].GetValue<string>() : "raw";

    return_types = {
        LogicalType::VARCHAR,  // uri
        LogicalType::INTEGER,  // mode
        LogicalType::VARCHAR,  // kind
        LogicalType::BOOLEAN,  // is_text
        LogicalType::VARCHAR,  // encoding
        LogicalType::BIGINT,   // size_bytes
        LogicalType::BOOLEAN,  // truncated
        LogicalType::VARCHAR,  // text
        LogicalType::BLOB,     // blob
        LogicalType::VARCHAR   // note
    };
    names = {"uri","mode","kind","is_text","encoding","size_bytes",
             "truncated","text","blob","note"};

    return std::move(bind);
}

// State
struct GitReadState : public GlobalTableFunctionState {
    idx_t pos = 0;
};

// Init
static unique_ptr<GlobalTableFunctionState> GitReadInit(
    ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GitReadState>();
}

// Main execution
static void GitReadFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &bind = (GitReadBindData &)*input.bind_data;
    auto &state = (GitReadState &)*input.global_state;

    // Example: read one URI from input.inputs[0], resolve git mode, fill row
    // TODO: integrate with duck_tails git:// VFS to fetch blob by hash/uri

    idx_t out_idx = 0;
    // Fill output columns here...
    output.SetCardinality(out_idx);
}

// Register
void LoadGitReadExtension(DatabaseInstance &db) {
    TableFunction fun("git_read",
                      {LogicalType::VARCHAR, LogicalType::BIGINT,
                       LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                      GitReadFunc, GitReadBind, GitReadInit);
    ExtensionUtil::RegisterFunction(db, fun);
}
```