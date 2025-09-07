# URI Clarity: Consistent Schema Standardization Plan

## Overview

This document outlines a comprehensive plan to standardize all Duck Tails functions that return URIs with consistent column ordering and naming conventions. This ensures predictable schemas across all git functions for better composability and user experience.

## Expected Outcomes

### User-Facing Improvements

#### Consistent URI Schema Across All Functions
```sql
-- BEFORE: Different schemas, manual URI passing
SELECT t.git_file_uri FROM git_tree('HEAD') t;  -- Column 8
SELECT r.uri FROM git_read('git://file.txt@HEAD') r;  -- Column 1

-- AFTER: Identical first 8 columns, seamless joining
SELECT t.git_uri, r.git_uri, t.file_ext, r.encoding
FROM git_tree('HEAD') t
JOIN git_read_each(t.git_uri) r ON t.git_uri = r.git_uri;  
-- Both have same column order: git_uri, repo_path, commit_hash, tree_hash, file_path, file_ext, ref, blob_hash
```

#### Rich File Metadata in git_tree
```sql
-- NEW: git_tree now has content analysis (from git_read)
SELECT git_uri, file_path, file_ext, is_text, encoding, kind
FROM git_tree('HEAD') 
WHERE is_text = true AND file_ext = '.md';
-- Get all markdown files with encoding info
```

#### Powerful LATERAL Joins
```sql
-- NEW: Rich cross-function analysis possible
SELECT 
  t.file_path,
  t.commit_hash,
  r.size_bytes,
  r.is_text,
  l.author_name,
  l.commit_date
FROM git_tree('HEAD~10..HEAD') t
JOIN LATERAL git_read_each(t.git_uri) r ON TRUE  
JOIN git_log('HEAD~10..HEAD') l ON t.commit_hash = l.commit_hash
WHERE r.is_text = true
ORDER BY l.commit_date DESC;
-- Analyze text file changes with author info across commit range
```

### Technical Improvements

**Schema Predictability:**
- Standard 8-column URI prefix across all functions
- Consistent hash ordering (matches git_log pattern)
- Standardized property names (size_bytes, file_path, git_uri)

**Measurable Value:**
- Enables complex repository analysis queries that were difficult/impossible before
- Composable git operations via LATERAL joins
- Content analysis queries across entire repositories

### Trade-offs

**What we get:** Integrated git analysis toolkit with predictable, composable schemas  
**What we don't get:** Code refactoring improvements (retained existing technical debt)  
**Breaking changes:** Existing queries need updates (migration guide provided)

## Current State Analysis

### Functions Currently Returning URIs

1. **`git_tree` / `git_tree_each`** - Returns `git_file_uri`
   - Current: `"repo_path", "commit_hash", "commit_date", "path", "mode", "blob_hash", "size", "git_file_uri"`
   - Has: commit_hash ✅, blob_hash ✅
   - Missing: Extracted components (file_path, file_ext, ref), tree_hash

2. **`git_read` / `git_read_each`** - Returns `uri`
   - Current: `"uri", "mode", "kind", "is_text", "encoding", "size_bytes", "truncated", "text", "blob"`
   - Missing: repo_path, commit_hash, blob_hash, extracted components

### Functions That May Need Schema Updates

**Functions that reference or consume URIs:**
- Any function that takes git URIs as input will benefit from consistent output schemas
- Functions that join with URI-returning functions need schema awareness
- Documentation examples that show LATERAL joins between functions

## Target Schema Standard

### **CRITICAL REQUIREMENT: _each Function Schema Consistency**

**ALL `*_each` functions MUST have identical signatures and return schemas as their parent `git_*` counterparts.**

### **CRITICAL REQUIREMENT: Replace Custom Binary Detection**

**ALL functions using custom `IsTextContent()` MUST be updated to use libgit2's `git_blob_is_binary()`:**
- **Current git_read implementation** has custom binary detection that reads blob content
- **New implementation** uses `git_blob_is_binary(blob)` - efficient libgit2 heuristic
- **Performance benefit:** Uses libgit2's efficient heuristic (checks ~4-8KB vs full blob content)  
- **Consistency benefit:** All functions use same libgit2 binary detection algorithm

**Complete List of Function Pairs:**
- `git_log('.')` and `git_log_each(path)` → **Identical** return schemas
- `git_branches('.')` and `git_branches_each(path)` → **Identical** return schemas
- `git_tags('.')` and `git_tags_each(path)` → **Identical** return schemas  
- `git_tree('HEAD')` and `git_tree_each(path)` → **Identical** return schemas
- `git_parents('HEAD')` and `git_parents_each(commit)` → **Identical** return schemas
- `git_read('git://file@HEAD')` and `git_read_each(uri)` → **Identical** return schemas  
- `git_clone('url')` and `git_clone_each(url)` → **Identical** return schemas

**This ensures:**
- Predictable schemas across static and LATERAL usage patterns
- Seamless transitions between different function variants
- No schema surprises when switching from static to dynamic parameters

### Core URI Columns (First 8 columns, consistent across ALL functions)
```
1. git_uri      VARCHAR    - The complete git:// URI
2. repo_path    VARCHAR    - Repository path on filesystem  
3. commit_hash  VARCHAR    - Git commit hash
4. tree_hash    VARCHAR    - Git tree hash containing the file
5. file_path    VARCHAR    - File path within repository
6. file_ext     VARCHAR    - File extension (e.g., .js, .cpp, .md)
7. ref          VARCHAR    - Git reference (commit SHA, branch, tag)
8. blob_hash    VARCHAR    - Git blob hash of file content
```

**Rationale for hash ordering:** This follows the established `git_log` pattern (`repo_path` → `commit_hash` → `tree_hash`) to maintain consistency across all git functions. The Git object hierarchy is: commit → tree → blob, which this ordering reflects.

### Function-Specific Columns (After core 8)
Each function appends its specific columns after the standard 8.

## New Schemas

### `git_tree` / `git_tree_each`
```
"git_uri", "repo_path", "commit_hash", "tree_hash", "file_path", "file_ext", "ref", "blob_hash",
"commit_date", "mode", "size_bytes", "kind", "is_text", "encoding"
```

**Key changes:**
- **REMOVED:** `path` (redundant with `file_path`)
- **RENAMED:** `size` → `size_bytes` (consistency with git_read)
- **ADDED:** `kind`, `is_text`, `encoding` (content analysis properties from git_read)

### `git_read` / `git_read_each`  
```
"git_uri", "repo_path", "commit_hash", "tree_hash", "file_path", "file_ext", "ref", "blob_hash",
"mode", "kind", "is_text", "encoding", "size_bytes", "truncated", "text", "blob"
```

**Key changes:**
- **RENAMED:** `uri` → `git_uri`
- **ADDED:** `repo_path`, `commit_hash`, `tree_hash`, `file_path`, `file_ext`, `ref`, `blob_hash`
- **REORDERED:** All columns to match standard URI schema

## Implementation Plan

**⚠️ CRITICAL: Read `refactoring-danger-zones.md` before starting - contains ghost bug prevention strategies**

### Phase 1: Preparation & Safety Setup (1 hour)

#### 1.1 Fix Current Build Issues (30 minutes)
**File:** `src/git_clone.cpp` 
- Fix StringVector::AddString API issues preventing compilation
- Ensure clean build before schema changes

#### 1.2 Establish Ghost Bug Detection (30 minutes)
**Files:** Create baseline test queries from `refactoring-danger-zones.md`
```sql
-- Capture baseline outputs before any changes
CREATE TABLE baseline_hashes AS 
SELECT 'git_log' as source, commit_hash, tree_hash FROM git_log('HEAD') LIMIT 10
UNION ALL
SELECT 'git_tree' as source, commit_hash, tree_hash FROM git_tree('HEAD') LIMIT 10;

CREATE TABLE baseline_uris AS
SELECT git_uri FROM git_tree('HEAD') LIMIT 10;
```

### Phase 2: Schema Updates (3-4 hours)

#### 2.1 Helper Functions (Inline as needed - 1 hour)
**Strategy:** Add utilities inline during schema work, don't extract until pattern established
```cpp
// Only add these if multiple functions need them:
static string ExtractFileExtension(const string &file_path);
static string ResolveCommitHash(const string &repo_path, const string &ref);
static string GetTreeHashForFile(const string &repo_path, const string &commit_hash, const string &file_path);
static string GetBlobHashForFile(const string &repo_path, const string &commit_hash, const string &file_path);

// Content analysis - EFFICIENT implementation using libgit2 built-ins
static string GetObjectKind(git_object *obj) {
    switch (git_object_type(obj)) {
        case GIT_OBJECT_BLOB: return "blob";
        case GIT_OBJECT_TREE: return "tree"; 
        case GIT_OBJECT_COMMIT: return "commit";
        default: return "unknown";
    }
}

static bool IsTextBlob(git_blob *blob) {
    return !git_blob_is_binary(blob);  // libgit2 heuristic - checks ~4-8KB
}

static string DetectEncoding(git_blob *blob) {
    return git_blob_is_binary(blob) ? "binary" : "utf8";  // Simple heuristic
}

// Hash computation patterns for git_read (reuse existing patterns from danger zones doc):
static string ComputeCommitHashFromRef(git_repository *repo, const string &ref) {
    git_object *obj;
    git_revparse_single(&obj, repo, ref.c_str());
    char hash_str[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hash_str, sizeof(hash_str), git_object_id(obj));
    git_object_free(obj);
    return string(hash_str);
}

static string ComputeTreeHashFromCommit(git_repository *repo, const string &commit_hash) {
    git_oid commit_oid;
    git_oid_fromstr(&commit_oid, commit_hash.c_str());
    git_commit *commit;
    git_commit_lookup(&commit, repo, &commit_oid);
    const git_tree *tree = git_commit_tree(commit);
    char tree_hash[GIT_OID_HEXSZ + 1];
    git_oid_tostr(tree_hash, sizeof(tree_hash), git_tree_id(tree));
    git_commit_free(commit);
    return string(tree_hash);
}
```

#### 2.2 Data Structure Updates (30 minutes)
**File:** `src/include/git_functions.hpp`

**Update GitTreeRow struct:**
```cpp
struct GitTreeRow {
    string git_uri;
    string commit_hash;
    timestamp_t commit_date; 
    int32_t mode;
    string blob_hash;
    int64_t size_bytes;      // RENAMED: size → size_bytes
    string file_path;        // NEW: Extracted from URI  
    string file_ext;         // NEW: File extension
    string ref;              // NEW: Git reference
    string tree_hash;        // NEW: Tree hash
    string kind;             // NEW: Object kind (blob, tree, etc.)
    bool is_text;            // NEW: Whether content is text
    string encoding;         // NEW: Text encoding
    // REMOVED: string path; // Redundant with file_path
};
```

**Update GitReadLocalState::ReadResult struct:**
```cpp
// In GitReadLocalState::ReadResult, add/modify:
string git_uri;
string repo_path;        // NEW
string commit_hash;      // NEW
string tree_hash;        // NEW
string file_path;        // NEW  
string file_ext;         // NEW
string ref;              // NEW
string blob_hash;        // NEW
int32_t mode;            // EXISTING
string kind;             // EXISTING
bool is_text;            // EXISTING
string encoding;         // EXISTING
int64_t size_bytes;      // EXISTING
bool truncated;          // EXISTING
string text;             // EXISTING
string blob;             // EXISTING
```

#### 2.3 Update `git_tree` / `git_tree_each` (2-3 hours)
**Files:** `src/git_functions.cpp`, `src/include/git_functions.hpp`

**⚠️ CRITICAL:** Both `git_tree` and `git_tree_each` MUST have identical schemas after updates.

1. **Update DefineGitTreeSchema():**
   - Reorder columns to match target schema
   - Column `git_uri` moved to first position
   - Rename `size` → `size_bytes`
   - Add `kind`, `is_text`, `encoding` columns
   - Remove redundant `path` column

2. **Update traverse_tree():**
   - Populate new fields: file_path, file_ext, ref, tree_hash
   - Add content analysis: kind, is_text, encoding using **libgit2 built-ins**
   - **Get blob objects during tree traversal:**
     ```cpp
     // During tree entry processing:
     git_object *obj;
     git_object_lookup(&obj, repo, &entry_oid, GIT_OBJECT_BLOB);
     if (git_object_type(obj) == GIT_OBJECT_BLOB) {
         git_blob *blob = (git_blob*)obj;
         row.is_text = !git_blob_is_binary(blob);
         row.kind = "blob";
         row.encoding = git_blob_is_binary(blob) ? "binary" : "utf8";
     }
     git_object_free(obj);
     ```
   - **Error handling:** If blob lookup fails, set `is_text=false`, `kind="unknown"`, `encoding="unknown"`

3. **Update OutputGitTreeRow():**
   - Reorder output to match new schema
   - Add new column outputs for content analysis

4. **Test:** `SELECT * FROM git_tree('HEAD') WHERE is_text = true LIMIT 5;`

#### 2.4 Update `git_read` / `git_read_each` (2-3 hours)
**Files:** `src/git_functions.cpp`

**⚠️ CRITICAL:** Both `git_read` and `git_read_each` MUST have identical schemas after updates.
**⚠️ CRITICAL:** Use existing hash conversion patterns - see danger zones doc
**⚠️ CRITICAL:** Replace existing `IsTextContent()` with libgit2's `git_blob_is_binary()`

1. **Update bind functions:**
   - Modify return schema to match target

2. **Update ProcessGitURI():**
   - **URI parsing:** Use existing `GitPath::Parse()` to extract components from `git://./file.txt@HEAD`
     ```cpp
     auto git_path = GitPath::Parse(uri);  // Already implemented
     repo_path = git_path.repository_path;
     file_path = git_path.file_path;
     ref = git_path.revision;
     file_ext = ExtractFileExtension(file_path);
     ```
   - Add commit hash resolution using **existing `oid_to_hex()` function**
   - Add blob hash computation using **existing patterns**  
   - Add tree hash computation using **existing `git_oid_tostr` calls**

3. **Replace custom binary detection with libgit2 built-ins:**
   - **REMOVE:** Custom `IsTextContent(const char* data, size_t size)` function
   - **REPLACE WITH:** `git_blob_is_binary(blob)` calls
   - **BENEFIT:** Efficient libgit2 heuristic (checks ~4-8KB vs full blob content)
   - **UPDATE:** All `is_text = IsTextContent(...)` calls to use new pattern

4. **Update output functions:**
   - Reorder all column outputs to match new schema
   - Column `git_uri` moved to first position

4. **Test:** `SELECT * FROM git_read('git://./README.md@HEAD');`

### Phase 3: Integration & Testing (1-2 hours)

#### 3.1 Build Testing
```bash
# Test compilation
VCPKG_TOOLCHAIN_PATH="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" make

# Test specific functions
./build/debug/duckdb -c "SELECT * FROM git_tree('HEAD') LIMIT 3;"
./build/debug/duckdb -c "SELECT * FROM git_read('git://./README.md@HEAD');"
```

#### 3.2 Schema Consistency Testing
```sql
-- Test that both functions have same first 8 columns
.schema git_tree
.schema git_read

-- CRITICAL: Test ALL _each functions match parent schemas
.schema git_log_each
.schema git_branches_each  
.schema git_tags_each
.schema git_tree_each  
.schema git_parents_each
.schema git_read_each
.schema git_clone_each
-- ALL should be IDENTICAL to their respective parent schemas

-- Test LATERAL joins work with new schemas
SELECT t.git_uri, r.text 
FROM git_tree('HEAD') t 
LIMIT 5, 
LATERAL git_read_each(t.git_uri) r;
```

#### 3.3 Ghost Bug Detection (CRITICAL)
**Run queries from `refactoring-danger-zones.md` after each function change:**
```sql
-- Verify hash consistency across functions
SELECT commit_hash, COUNT(DISTINCT source)
FROM (
  SELECT 'git_log' as source, commit_hash FROM git_log('HEAD') LIMIT 5
  UNION ALL
  SELECT 'git_tree' as source, commit_hash FROM git_tree('HEAD') LIMIT 5
) 
GROUP BY commit_hash
HAVING COUNT(DISTINCT source) < 2;
-- Should return empty - same commits must have same hashes

-- Verify URI format consistency  
SELECT 
  CASE WHEN git_uri LIKE 'git://%@%' THEN 'valid' ELSE 'invalid' END,
  COUNT(*)
FROM git_tree('HEAD') 
GROUP BY 1;
-- Should only show 'valid' format
```

#### 3.3 Regression Testing  
```bash
# Run existing test suite
test/run_tests.sh

# Test specific git function tests
./build/debug/duckdb -c ".read test/sql/git_*.test"
```

### Phase 4: Function Impact Analysis & Updates

#### 4.1 Internal Functions Requiring Updates

**Functions that consume git URIs:**
- Any function using `git_tree` or `git_read` output needs schema awareness
- LATERAL join examples in code need column index updates

**Search for impacts:**
```bash
# Find code references to old column names
rg "git_uri|repo_path.*commit_hash" src/
rg "git_uri.*mode.*kind" src/

# Find test files that might break
rg "git_tree|git_read" test/sql/
```

#### 4.2 External Function Consistency Review

**Hash Ordering Consistency Analysis:**

Current `git_log` schema: `"repo_path", "commit_hash", "author_name", "author_email", "committer_name", "committer_email", "author_date", "commit_date", "message", "parent_count", "tree_hash"`

Our URI functions now follow the same hash pattern: `repo_path` → `commit_hash` → `tree_hash` → [other fields]

**Functions that don't need changes (no URI output):**

1. **`git_log` / `git_log_each`** - Returns commit metadata, repository-level scope
   - ✅ No changes needed - already follows `repo_path` → `commit_hash` → `tree_hash` pattern

2. **`git_branches` / `git_branches_each`** - Returns branch information
   - ✅ No changes needed - returns branch metadata, not file-specific data

3. **`git_tags` / `git_tags_each`** - Returns tag information
   - ✅ No changes needed - returns tag metadata, not file-specific data

4. **`git_parents` / `git_parents_each`** - Returns parent-child relationships
   - ✅ No changes needed - returns commit relationships, not file-specific data

5. **`git_clone` / `git_clone_each`** - Returns clone operation results
   - ✅ No changes needed - returns operation metadata, not file URIs

**Future Enhancement Opportunities:**
- **Path-aware `git_log`:** Could support file/directory-specific history with URI output
- **Repository URIs:** Other functions could return repository-level URIs (e.g., `git://./repo@branch_name`)

#### 4.3 Breaking Changes Documentation

**Document schema changes:**
```markdown
## Breaking Changes in v.X.X

### Schema Reordering for Consistency
Both `git_tree` and `git_read` now follow consistent hash ordering that matches `git_log`:
`repo_path` → `commit_hash` → `tree_hash` → [file-specific fields]

### `git_tree` / `git_tree_each`
- **BREAKING:** Column order changed to match standard URI schema
- **BREAKING:** `git_uri` moved to first position (renamed from `git_file_uri`)
- **BREAKING:** `size` renamed to `size_bytes` for consistency
- **REMOVED:** `path` column (redundant with `file_path`)
- **NEW:** Added `tree_hash`, `file_path`, `file_ext`, `ref`, `blob_hash` columns
- **NEW:** Added content analysis: `kind`, `is_text`, `encoding` columns
- **Migration:** Update column references and SELECT statements

### `git_read` / `git_read_each`  
- **BREAKING:** Complete column reordering to match standard URI schema
- **BREAKING:** `git_uri` moved to first position (renamed from `uri`)
- **NEW:** Added `repo_path`, `commit_hash`, `tree_hash`, `file_path`, `file_ext`, `ref`, `blob_hash` columns
- **Migration:** Update all column references - this is a major schema change

### Compatibility
- LATERAL joins between `git_tree` and `git_read` now have consistent first 8 columns
- All URI-returning functions now follow the same column standard
- Functions that don't return URIs (`git_log`, `git_branches`, etc.) are unchanged
```

### Phase 5: Documentation Updates (2-3 hours)

#### 5.1 Update Core Documentation
**Files to update:**
- `docs/llmtxt.md` - Update ALL examples with new schemas
- `docs/git-uris.md` - Add section on consistent schema format
- `README.md` - Update any examples using git_tree or git_read

#### 5.2 Example Updates in llmtxt.md

**Find and update examples:**
```bash
# Find examples that need updating
rg "git_tree|git_read" docs/llmtxt.md
```

**Update patterns like:**
```sql
-- OLD git_tree:
SELECT repo_path, commit_hash, path, git_file_uri 
FROM git_tree('HEAD');

-- NEW git_tree:  
SELECT git_uri, repo_path, commit_hash, tree_hash, file_path
FROM git_tree('HEAD');

-- OLD git_read:
SELECT uri, mode, text FROM git_read('git://./README.md@HEAD');

-- NEW git_read:
SELECT git_uri, repo_path, commit_hash, tree_hash, file_path, text
FROM git_read('git://./README.md@HEAD');
```

**Update LATERAL join examples:**
```sql
-- OLD: Different schemas, manual URI passing
SELECT t.git_file_uri, r.text
FROM git_tree('HEAD') t,
LATERAL git_read_each(t.git_file_uri) r;

-- NEW: Consistent schemas, rich joining possibilities
SELECT t.git_uri, r.text, t.file_ext, r.encoding,
       t.commit_hash = r.commit_hash as same_commit
FROM git_tree('HEAD') t,
LATERAL git_read_each(t.git_uri) r;
```

#### 5.3 Schema Reference Documentation

**Add to docs/git-uris.md:**
```markdown
## Consistent Schema Standard

All functions returning git URIs follow this standard, maintaining consistency with `git_log` hash ordering:

### Core Columns (1-8)
1. `git_uri` - Complete git:// URI
2. `repo_path` - Repository filesystem path  
3. `commit_hash` - Git commit hash (follows git_log pattern)
4. `tree_hash` - Git tree hash containing the file (follows git_log pattern)
5. `file_path` - File path within repository
6. `file_ext` - File extension (.js, .cpp, etc.)
7. `ref` - Git reference (SHA/branch/tag) 
8. `blob_hash` - Git blob hash of file content

### Hash Hierarchy Rationale
This ordering reflects the Git object hierarchy and maintains consistency with `git_log`:
- **Commit** contains trees and metadata
- **Tree** contains blobs and subdirectories  
- **Blob** contains file content

### Function-Specific Columns (9+)
Each function adds its specific columns after the standard 8.

### Cross-Function Compatibility
With consistent schemas, powerful LATERAL joins become possible:
```sql
-- Compare file content across different commits
SELECT t1.commit_hash, t2.commit_hash, t1.file_path,
       r1.text = r2.text as content_identical
FROM git_tree('HEAD') t1,
     git_tree('HEAD~1') t2,
     LATERAL git_read_each(t1.git_uri) r1,
     LATERAL git_read_each(t2.git_uri) r2
WHERE t1.file_path = t2.file_path;
```

## Parallelization Opportunities

### Parallel Development Streams:

**Stream A: Infrastructure (1 person, 2-3 hours)**
- Helper functions
- Data structure updates
- Build infrastructure

**Stream B: git_tree Updates (1 person, 3-4 hours)** 
- Schema updates
- Function implementation
- Testing

**Stream C: git_read Updates (1 person, 4-5 hours)**
- More complex due to missing hash computations
- Schema updates  
- Function implementation
- Testing

**Stream D: Documentation (1 person, 2-3 hours)**
- Can start once schema is finalized
- Update all examples
- Write migration guide

### Dependencies:
- Stream B & C depend on Stream A completion
- Stream D can start once target schemas are confirmed
- Integration testing requires B & C completion

## Testing Strategy

### Per-Repository Convention Testing

**Test in multiple repository types:**

1. **Simple repo:** Single branch, few files
   ```bash
   mkdir test_simple && cd test_simple
   git init && echo "test" > file.txt && git add . && git commit -m "test"
   duckdb -c "SELECT * FROM git_tree('HEAD');"
   ```

2. **Complex repo:** Multiple branches, deep directory structure  
   ```bash
   cd /path/to/complex/repo
   duckdb -c "SELECT * FROM git_tree('HEAD') WHERE file_ext = '.cpp';"
   ```

3. **Large repo:** Performance testing
   ```bash
   cd /path/to/large/repo  
   duckdb -c "SELECT COUNT(*) FROM git_tree('HEAD');"
   ```

### Schema Consistency Testing

**Cross-function consistency:**
```sql
-- Verify first 8 columns match between functions
WITH tree_schema AS (
  SELECT * FROM git_tree('HEAD') LIMIT 1
),
read_schema AS (
  SELECT * FROM git_read('git://./README.md@HEAD') LIMIT 1
)
SELECT 
  t.git_uri = r.git_uri as uri_match,
  t.repo_path = r.repo_path as repo_match,
  t.file_path = r.file_path as path_match,
  t.ref = r.ref as ref_match,
  t.commit_hash = r.commit_hash as commit_match
FROM tree_schema t, read_schema r;
```

### Regression Testing

**Existing functionality:**
```bash
# Test all existing git functions still work
for func in git_log git_branches git_tags git_parents git_tree git_read; do
  echo "Testing $func..."
  duckdb -c "SELECT COUNT(*) FROM $func('HEAD') WHERE rowid < 5;"
done
```

## Risk Mitigation

### Breaking Changes
- **Risk:** Existing queries break due to column reordering
- **Mitigation:** Clear documentation of changes, migration examples

### Performance Impact
- **Risk:** Additional hash computations slow down functions
- **Mitigation:** Benchmark before/after, optimize hot paths

### Implementation Complexity
- **Risk:** Complex git hash resolution introduces bugs
- **Mitigation:** Extensive testing, fallback error handling

## Success Criteria

1. ✅ Both `git_tree` and `git_read` have identical first 8 columns
2. ✅ All hash values (blob, tree, commit) are correctly computed
3. ✅ LATERAL joins between functions work seamlessly  
4. ✅ No performance regression > 10%
5. ✅ All existing tests pass
6. ✅ Documentation fully updated with new schemas
7. ✅ Clear migration guide for existing users

## Timeline Estimate

- **Phase 1 (Infrastructure):** 2-3 hours
- **Phase 2 (Functions):** 6-8 hours (parallel)  
- **Phase 3 (Testing):** 2 hours
- **Phase 4 (Analysis):** 1 hour
- **Phase 5 (Documentation):** 2-3 hours

**Total: 6-8 hours** (reduced from 13-17 via inline utility strategy and danger zone awareness)

## Next Steps

1. **Read `refactoring-danger-zones.md`** - Critical ghost bug prevention
2. Fix current build issues (StringVector API)
3. Establish baseline ghost bug detection queries  
4. Begin Phase 2 schema updates with safety checks
5. Run ghost bug detection after each function change
6. Integration testing and documentation

## Related Documents

- **`refactoring-danger-zones.md`** - MUST READ before starting implementation
- **`docs/git-uris.md`** - Git URI format specification (will be updated post-implementation)
- **`docs/llmtxt.md`** - Function documentation (will need schema updates after implementation)
- **`git-utils-refactoring.md`** - Alternative approach (currently not recommended)

---

*This plan achieves URI schema consistency while avoiding the complexity and risks of large-scale refactoring. Safety-first approach with multiple validation checkpoints.*