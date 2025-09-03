# Code Consolidation Plan for Git Functions

## Executive Summary

The `git_functions.cpp` file has grown to 2866 lines with significant code duplication. This document outlines a consolidation strategy that will:
- Fix the missing `repo_path` column in `git_tree` and `git_parents`
- Reduce code duplication by ~40%
- Make future maintenance easier
- Ensure consistency across all functions

## Current State Analysis

### File Structure (2866 lines total)
| Lines | Content |
|-------|---------|
| 1-800 | Helper functions, data structures, git_tree implementation |
| 801-1080 | GitTreeFunction and GitParentsFunction implementations |
| 1181-1973 | All _each functions (git_log_each, git_branches_each, git_tags_each, git_tree_each) |
| 1976-2039 | Registration functions |
| 2046-2700 | git_read and git_read_each |
| 2765-2866 | git_uri function and main registration |

### Key Findings

#### Functions WITH repo_path (correct) ✅
- `git_log` - has repo_path as first column
- `git_branches` - has repo_path as first column  
- `git_tags` - has repo_path as first column
- `git_log_each` - has repo_path as first column
- `git_branches_each` - has repo_path as first column
- `git_tags_each` - has repo_path as first column
- `git_tree_each` - has repo_path as first column

#### Functions WITHOUT repo_path (need fixing) ❌
- `git_tree` - missing repo_path as first column
- `git_parents` - missing repo_path as first column

#### Functions with URI instead (probably OK) ℹ️
- `git_read` - has "uri" as first column (different paradigm)
- `git_read_each` - has "uri" as first column (different paradigm)

## Code Duplication Patterns

### Pattern 1: Schema Definitions
```cpp
// Current: git_tree has 3 identical schema definitions
// Lines 557-559, 570-572, 576-578
names = {"commit_hash", "commit_date", "path", "mode", "blob_hash", "size", "git_file_uri"};
return_types = {LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR, 
               LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR};
```

### Pattern 2: Output Writing
```cpp
// Current: Manual column indexing prone to errors
output.SetValue(0, i, Value(row.commit_hash));      // Wrong! Should be repo_path
output.SetValue(1, i, Value::TIMESTAMP(row.commit_date));
output.SetValue(2, i, Value(row.path));
// ... etc
```

### Pattern 3: Repository Opening
- Same git repository opening code repeated 10+ times
- Identical error handling patterns throughout

## Proposed Solution: Helper Functions

### 1. Schema Definition Helpers

```cpp
// Helper to define schema for git_tree variants
static void DefineGitTreeSchema(vector<LogicalType> &return_types, 
                                vector<string> &names,
                                bool include_repo_path = true) {
    if (include_repo_path) {
        return_types = {LogicalType::VARCHAR};  // repo_path
        names = {"repo_path"};
    } else {
        return_types = {};
        names = {};
    }
    
    // Common git_tree columns
    vector<LogicalType> tree_types = {
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::TIMESTAMP,  // commit_date
        LogicalType::VARCHAR,    // path
        LogicalType::INTEGER,    // mode
        LogicalType::VARCHAR,    // blob_hash
        LogicalType::BIGINT,     // size
        LogicalType::VARCHAR     // git_file_uri
    };
    vector<string> tree_names = {
        "commit_hash", "commit_date", "path", "mode", 
        "blob_hash", "size", "git_file_uri"
    };
    
    return_types.insert(return_types.end(), tree_types.begin(), tree_types.end());
    names.insert(names.end(), tree_names.begin(), tree_names.end());
}

// Similar helpers for git_parents, git_log, etc.
static void DefineGitParentsSchema(vector<LogicalType> &return_types,
                                   vector<string> &names,
                                   bool include_repo_path = true) {
    if (include_repo_path) {
        return_types = {LogicalType::VARCHAR};  // repo_path
        names = {"repo_path"};
    } else {
        return_types = {};
        names = {};
    }
    
    vector<LogicalType> parent_types = {
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // parent_hash
        LogicalType::INTEGER     // parent_index
    };
    vector<string> parent_names = {
        "commit_hash", "parent_hash", "parent_index"
    };
    
    return_types.insert(return_types.end(), parent_types.begin(), parent_types.end());
    names.insert(names.end(), parent_names.begin(), parent_names.end());
}
```

### 2. Output Writing Helpers

```cpp
// Helper to output a git_tree row with automatic column indexing
static void OutputGitTreeRow(DataChunk &output, idx_t row_idx, 
                             const GitTreeRow &row,
                             const string *repo_path = nullptr) {
    idx_t col = 0;
    if (repo_path) {
        output.SetValue(col++, row_idx, Value(*repo_path));
    }
    output.SetValue(col++, row_idx, Value(row.commit_hash));
    output.SetValue(col++, row_idx, Value::TIMESTAMP(row.commit_date));
    output.SetValue(col++, row_idx, Value(row.path));
    output.SetValue(col++, row_idx, Value::INTEGER(row.mode));
    output.SetValue(col++, row_idx, Value(row.blob_hash));
    output.SetValue(col++, row_idx, Value::BIGINT(row.size));
    output.SetValue(col++, row_idx, Value(row.git_file_uri));
}

// Helper for git_parents output
static void OutputGitParentsRow(DataChunk &output, idx_t row_idx,
                                const GitParentsRow &row,
                                const string *repo_path = nullptr) {
    idx_t col = 0;
    if (repo_path) {
        output.SetValue(col++, row_idx, Value(*repo_path));
    }
    output.SetValue(col++, row_idx, Value(row.commit_hash));
    output.SetValue(col++, row_idx, Value(row.parent_hash));
    output.SetValue(col++, row_idx, Value::INTEGER(row.parent_index));
}
```

### 3. Common Repository Operations

```cpp
// Helper to open a git repository with standard error handling
static unique_ptr<git_repository, GitRepoDeleter> OpenGitRepository(const string &path) {
    git_libgit2_init();
    
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, path.c_str());
    if (error != 0) {
        const git_error *e = git_error_last();
        throw IOException("Failed to open repository '%s': %s", 
                        path, e ? e->message : "Unknown error");
    }
    
    return unique_ptr<git_repository, GitRepoDeleter>(repo);
}
```

## Implementation Plan

### Phase 1: Create Helper Infrastructure (Foundation)
1. Add schema definition helpers for all git function types
2. Add output helpers with automatic column indexing
3. Extract common repository opening logic
4. Place helpers at the top of `git_functions.cpp` (lines 100-300)

### Phase 2: Fix git_tree and git_parents (Core Fix)
1. Update `GitTreeBind` to use `DefineGitTreeSchema(types, names, true)`
2. Update `GitTreeFunction` to use `OutputGitTreeRow` with repo_path
3. Update `GitParentsBind` to use `DefineGitParentsSchema(types, names, true)`
4. Update `GitParentsFunction` to use `OutputGitParentsRow` with repo_path

### Phase 3: Consolidate Existing Functions (Cleanup)
1. Update all _each functions to use output helpers
2. Consolidate duplicate schema definitions
3. Replace manual repository opening with helper

### Phase 4: Test Updates
1. Update tests to expect repo_path as first column
2. Fix weak tests using `COUNT(*) >= 0`
3. Add specific assertions for repo_path values

## Benefits

### Immediate Benefits
- ✅ Fixes missing repo_path columns
- ✅ Reduces code by ~500 lines
- ✅ Automatic column index management
- ✅ Single source of truth for schemas

### Long-term Benefits
- 🚀 Easier to add new columns (change in one place)
- 🚀 Less prone to column indexing errors
- 🚀 Consistent behavior across all functions
- 🚀 Easier to understand and maintain

## Example: Before and After

### Before (Current git_tree - 15 lines)
```cpp
void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    // ... setup code ...
    for (idx_t i = 0; i < count; i++) {
        auto &row = bind_data.rows[bind_data.current_index + i];
        output.SetValue(0, i, Value(row.commit_hash));           // WRONG! Missing repo_path
        output.SetValue(1, i, Value::TIMESTAMP(row.commit_date));
        output.SetValue(2, i, Value(row.path));
        output.SetValue(3, i, Value::INTEGER(row.mode));
        output.SetValue(4, i, Value(row.blob_hash));
        output.SetValue(5, i, Value::BIGINT(row.size));
        output.SetValue(6, i, Value(row.git_file_uri));
    }
    // ... cleanup code ...
}
```

### After (With helpers - 5 lines)
```cpp
void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    // ... setup code ...
    for (idx_t i = 0; i < count; i++) {
        auto &row = bind_data.rows[bind_data.current_index + i];
        OutputGitTreeRow(output, i, row, &bind_data.repo_path);
    }
    // ... cleanup code ...
}
```

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Breaking existing queries | Add tests for backward compatibility |
| Introducing bugs | Use helpers incrementally, test each change |
| PR too large | Can split into 2 PRs: helpers first, then apply |
| Performance impact | Inline helpers, no virtual functions |

## Metrics

### Expected Code Reduction
- Schema definitions: -200 lines (consolidate 10 duplicates)
- Output functions: -150 lines (consolidate SetValue calls)  
- Repository opening: -100 lines (consolidate error handling)
- **Total: ~450 lines removed**

### Expected Quality Improvements
- Column indexing errors: 0 (automatic indexing)
- Schema inconsistencies: 0 (single source of truth)
- Missing repo_path: 0 (included by default)

## Next Steps

1. **Review and approve this plan**
2. **Create helper functions** (can be tested independently)
3. **Apply to git_tree and git_parents** (fixes the bug)
4. **Incrementally update other functions** (cleanup)
5. **Update tests** (ensure correctness)

## Questions for Review

1. **Scope**: Should we do all phases in one PR or split them?
2. **Location**: Keep helpers in `git_functions.cpp` or create `git_functions_helpers.hpp`?
3. **Testing**: Should we add performance benchmarks?
4. **Documentation**: Should we document the schema format in a comment block?

---

*This consolidation will make the codebase more maintainable while fixing the immediate repo_path issue.*
