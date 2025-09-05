# Git Clone Function Specification and Implementation Plan

## Overview
Add `git_clone` and `git_clone_each` functions to Duck Tails extension, enabling repository cloning directly from SQL with automatic directory name extraction when local path is not specified.

## Function Signatures

### git_clone
```sql
-- Basic usage with auto-generated local path
git_clone(url VARCHAR) → TABLE

-- With explicit local path
git_clone(url VARCHAR, local_path VARCHAR) → TABLE

-- With options and auto-generated path
git_clone(url VARCHAR, options STRUCT) → TABLE

-- With explicit local path and options
git_clone(url VARCHAR, local_path VARCHAR, options STRUCT) → TABLE
```

**Options STRUCT fields** (all optional):
- `branch` VARCHAR: Specific branch to clone (default: default branch)
- `depth` INTEGER: Shallow clone depth (default: NULL = full clone)
- `bare` BOOLEAN: Create bare repository (default: false)
- `no_checkout` BOOLEAN: Skip checkout after clone (default: false)
- `timeout` INTEGER: Timeout in seconds (default: 300 = 5 minutes)
- `force` BOOLEAN: Overwrite existing directory (default: false) [Future enhancement]

**Returns:**
- `url` VARCHAR: The source repository URL
- `local_path` VARCHAR: The actual path where repository was cloned
- `status` VARCHAR: 'success' or 'error'
- `action` VARCHAR: 'cloned', 'updated', 'up_to_date', or 'error'
- `message` VARCHAR: Success message or error details
- `commit_hash` VARCHAR: HEAD commit hash of cloned/updated repository
- `previous_hash` VARCHAR: Previous HEAD commit (for updates, NULL for clones)
- `commit_time` TIMESTAMP: HEAD commit timestamp
- `size_bytes` BIGINT: Total size of cloned repository (optional)

### git_clone_each
```sql
-- For LATERAL joins with column references
-- Auto-generate local path from URL
git_clone_each(url VARCHAR) → TABLE

-- Explicit local path from column
git_clone_each(url VARCHAR, local_path VARCHAR) → TABLE

-- With options and auto-generated path
git_clone_each(url VARCHAR, options STRUCT) → TABLE

-- With explicit local path and options
git_clone_each(url VARCHAR, local_path VARCHAR, options STRUCT) → TABLE
```

**Key capability**: All parameters can be column references in LATERAL joins, enabling dynamic path and option generation:
```sql
-- Dynamic paths based on other columns
SELECT * FROM repos r, LATERAL git_clone_each(r.url, '/clones/' || r.name || '/' || r.branch);

-- Dynamic options based on repository size
WITH repo_configs AS (
    SELECT 
        url,
        CASE 
            WHEN size_mb > 1000 THEN {'depth': 1, 'branch': 'main'}
            ELSE {'branch': 'main'}
        END as options
    FROM large_repos
)
SELECT * FROM repo_configs r, LATERAL git_clone_each(r.url, r.options);
```

**Returns:** Same schema as `git_clone`

## Local Path Logic

When `local_path` is not specified, is NULL, or is an empty string:

1. Extract repository name from URL:
   - `https://github.com/user/repo.git` → `repo`
   - `https://github.com/user/repo` → `repo`
   - `git@github.com:user/repo.git` → `repo`
   - `file:///path/to/repo.git` → `repo`

2. Use current working directory as base:
   - Final path: `./repo` (relative to DuckDB's working directory)

3. Handle conflicts:
   - If directory doesn't exist: Create it and clone
   - If directory exists but not a git repo: Return error with clear message
   - If directory exists and is a git repo:
     - Default behavior: Attempt to pull latest changes (fetch + merge/fast-forward)
     - With `force: true`: Delete and re-clone fresh
     - Return status indicating whether it was cloned fresh or updated

## Implementation Architecture

### Option 1: Separate File (Recommended)
Create new files for better modularity:
- `src/git_clone.cpp` - Implementation
- `src/include/git_clone.hpp` - Headers and structures

Benefits:
- Easier iteration during development
- Cleaner separation of concerns
- Simpler testing and debugging
- Can be easily merged into git_functions.cpp later if desired

### Option 2: Integrated
Add to existing `git_functions.cpp` (2700+ lines already)

## Implementation Plan

### Phase 1: Core Implementation
1. **Create git_clone.hpp**
   - Define `GitCloneBindData` structure
   - Define `GitCloneLocalState` structure
   - Function declarations

2. **Implement URL parsing**
   ```cpp
   string ExtractRepoNameFromURL(const string &url) {
       // Handle various URL formats
       // Strip .git suffix
       // Extract last path component
   }
   ```

3. **Implement git_clone function**
   ```cpp
   static void GitCloneFunction(DataChunk &output, GitCloneFunctionData &data) {
       // Initialize libgit2
       // Parse URL and determine local_path
       // Call git_clone
       // Populate output columns
       // Clean up
   }
   ```

4. **Implement git_clone_each function**
   - Similar to other _each functions
   - Support URI parsing for consistency
   - Enable LATERAL join compatibility

### Phase 2: Integration
1. **Register functions**
   ```cpp
   void RegisterGitCloneFunctions(DatabaseInstance &db) {
       // Register git_clone
       // Register git_clone_each with in_out_function
   }
   ```

2. **Update main registration**
   - Add to `RegisterGitFunctions` or call separately
   - Update CMakeLists.txt if using separate file

### Phase 3: Testing

#### Unit Tests
Create `test/sql/git_clone.test`:
```sql
# Test basic clone
statement ok
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git', '/tmp/test_clone');

# Test auto-path extraction
statement ok
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git');

# Test various URL formats
statement ok
SELECT * FROM git_clone('git@github.com:duckdb/duckdb.git');
```

#### Integration Tests
Use Duck Tails own repository:
```sql
# Clone Duck Tails repository
query IIIII
SELECT 
    status,
    length(commit_hash) = 40 as valid_hash,
    local_path LIKE '%duck_tails%' as has_repo_name
FROM git_clone('https://github.com/teaguesterling/duck_tails.git', '/tmp/duck_tails_test');
----
success	true	true

# Test git_clone_each with multiple repos
WITH repos AS (
    SELECT 'https://github.com/teaguesterling/duck_tails.git' as url, '/tmp/dt1' as path
    UNION ALL
    SELECT 'https://github.com/teaguesterling/duck_tails.git' as url, '/tmp/dt2' as path
)
SELECT COUNT(*) as cloned
FROM repos r, LATERAL git_clone_each(r.url, r.path) c
WHERE c.status = 'success';
```

#### Error Tests
```sql
# Test invalid URL
statement error
SELECT * FROM git_clone('not-a-valid-url');

# Test existing directory
statement error
SELECT * FROM git_clone('https://github.com/user/repo.git', '/existing/repo/path');

# Test network failure
statement error
SELECT * FROM git_clone('https://nonexistent-domain-12345.com/repo.git');
```

## Error Handling

1. **Invalid URL**: Clear error message about URL format
2. **Network errors**: Report connection failures
3. **Authentication**: Handle private repos (future: support tokens)
4. **Disk space**: Check available space before clone
5. **Permissions**: Handle directory permission errors
6. **Existing directory**: Clear message about existing path

## Progress Reporting (Future Enhancement)

Consider adding progress callback support:
```cpp
git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
opts.checkout_opts.progress_cb = checkout_progress;
opts.fetch_opts.callbacks.transfer_progress = fetch_progress;
```

## Security Considerations

1. **Path validation**: Prevent directory traversal attacks
2. **URL validation**: Sanitize URLs before passing to libgit2
3. **Resource limits**: Consider max clone size/time limits
4. **Concurrent clones**: Limit number of simultaneous clones

## Example Usage

```sql
-- Simple clone with auto-path
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git');

-- Clone to specific location
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git', '/data/repos/duckdb');

-- Clone with options (shallow clone, specific branch)
SELECT * FROM git_clone(
    'https://github.com/duckdb/duckdb.git', 
    {'branch': 'main', 'depth': 1}
);

-- Clone with both path and options
SELECT * FROM git_clone(
    'https://github.com/duckdb/duckdb.git',
    '/tmp/duckdb_shallow',
    {'branch': 'v0.9.2', 'depth': 10}
);

-- Clone multiple repositories with mixed path strategies
WITH repos_to_clone AS (
    VALUES 
    ('https://github.com/duckdb/duckdb.git', NULL),  -- auto-generate path
    ('https://github.com/teaguesterling/duck_tails.git', '/tmp/duck_tails'),
    ('https://github.com/duckdb/postgres_scanner.git', NULL)
)
SELECT url, local_path, status 
FROM repos_to_clone r(url, path), 
     LATERAL git_clone_each(r.url, r.path) c;

-- Dynamic path generation based on metadata
WITH project_repos AS (
    SELECT 
        project_name,
        repo_url,
        '/repos/' || project_name || '/' || DATE_PART('year', CURRENT_DATE) || '/' || repo_name as target_path
    FROM projects
)
SELECT * FROM project_repos p, LATERAL git_clone_each(p.repo_url, p.target_path) c;

-- Clone different versions to different directories
WITH versions AS (
    VALUES 
    ('v1.0', 'https://github.com/duckdb/duckdb.git'),
    ('v2.0', 'https://github.com/duckdb/duckdb.git')
)
SELECT 
    v.version,
    c.*
FROM versions v(version, url),
     LATERAL git_clone_each(v.url, '/versions/duckdb_' || v.version) c;

-- Analyze cloned repository immediately
WITH cloned AS (
    SELECT * FROM git_clone('https://github.com/teaguesterling/duck_tails.git')
)
SELECT 
    c.local_path,
    COUNT(*) as file_count
FROM cloned c, 
     LATERAL git_tree(c.local_path, 'HEAD') t
GROUP BY c.local_path;
```

## File Structure

```
src/
├── git_clone.cpp          # New file with implementation
├── include/
│   └── git_clone.hpp      # New header file
└── git_functions.cpp      # Add registration call

test/
└── sql/
    └── git_clone.test     # Comprehensive test suite
```

## Dependencies

- libgit2 (already available via vcpkg)
- Standard C++ libraries
- DuckDB extension framework

## Timeline Estimate

1. Core implementation: 2-3 hours
2. Testing and debugging: 1-2 hours  
3. Documentation updates: 30 minutes
4. Integration and build verification: 30 minutes

Total: ~4-6 hours

## Open Questions

1. ~~Should we support shallow clones (`--depth` option)?~~ ✅ Yes, included in options
2. ~~Should we add branch/tag selection during clone?~~ ✅ Yes, via `branch` option
3. How to handle large repositories (size limits)?
4. Should we support authentication tokens/credentials?
5. Add recursive submodule support?
6. Should we auto-increment directory names on conflicts (e.g., `repo`, `repo_1`, `repo_2`)?

## ⚠️ CRITICAL IMPLEMENTATION STATUS UPDATE

### 🚨 **ULTRATHINK ANALYSIS FINDINGS**

**Build Status**: 🔴 **NEVER SUCCESSFULLY BUILT OR TESTED**
- Implementation has never been compiled successfully
- No verification of actual git operations  
- Core functionality remains unvalidated

### **CRITICAL ISSUES IDENTIFIED**

1. **STRUCT Options Parsing** 🔴 **BLOCKER**
   - Line 340 in git_clone.cpp: `// TODO: Parse struct options` - completely unimplemented
   - Breaks 2 of 4 function signatures
   - Tests will fail at runtime

2. **Registration Pattern Mismatch** 🔴 **CRITICAL** 
   - Using separate `RegisterGitCloneFunctions()` instead of integrating with existing `RegisterGitFunctions()`
   - Missing `named_parameters["repo_path"]` on all functions
   - May cause DuckDB function resolution failures

3. **Test Coverage Gaps** 🟡 **MAJOR**
   - Tests only validate return schemas, not actual git operations
   - No verification of smart update behavior (pull existing repos)
   - No network failure or real-world error testing

### 🔧 **IMMEDIATE FIXES REQUIRED**

#### **Must Fix Before Testing**
1. **Implement STRUCT options parsing** - parse branch, depth, bare, no_checkout, timeout options
2. **Fix registration pattern** - move registration to existing `RegisterGitFunctions()` 
3. **Add named_parameters** - add to all function registrations for DuckDB compatibility
4. **Build and compile successfully** - resolve all compilation errors
5. **Test real git operations** - verify actual clones work, not just schemas

### **CURRENT IMPLEMENTATION STATUS**

❌ **NOT PRODUCTION READY** - Critical gaps prevent functioning correctly
✅ **Solid Foundation** - Core logic and structure are sound  
🔄 **Fixable Issues** - All problems have clear solutions

## Next Steps (UPDATED)

1. ✅ ~~Review and approve specification~~
2. ✅ ~~Create git_clone.hpp with structures~~
3. 🔄 **FIX CRITICAL ISSUES** (current priority)
   - Implement STRUCT options parsing
   - Fix registration pattern
   - Add named_parameters
4. 🔄 **BUILD AND TEST** (next priority)
   - Resolve compilation errors
   - Verify extension loads correctly
   - Test actual git operations
5. ⏳ Update documentation with verified functionality
6. ⏳ Consider future enhancements