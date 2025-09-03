# File Split Implementation Plan

## Overview

Split the 2866-line `git_functions.cpp` into multiple focused files for better maintainability and PR reviewability. All NEW functions from this branch will go into separate files, making it crystal clear what's being added.

## Proposed File Structure

```
src/
├── git_functions.cpp           (1200 lines - existing functions only)
├── git_functions_common.hpp    (200 lines - shared helpers)
├── git_tree_functions.cpp      (400 lines - NEW: git_tree, git_tree_each)
├── git_parents_functions.cpp   (150 lines - NEW: git_parents)
├── git_read_functions.cpp      (500 lines - NEW: git_read, git_read_each)
├── git_uri_function.cpp        (100 lines - NEW: git_uri)
└── include/
    ├── git_functions.hpp        (existing, update with new declarations)
    └── git_functions_common.hpp (NEW: shared helpers and structs)
```

## File Contents

### 1. `git_functions_common.hpp` (NEW - Shared Helpers)

```cpp
#pragma once
#include "duckdb.hpp"
#include <git2.h>

namespace duckdb {

// Common deleter for git objects
struct GitRepoDeleter {
    void operator()(git_repository *repo) {
        if (repo) git_repository_free(repo);
    }
};

// Helper to open a git repository with standard error handling
inline unique_ptr<git_repository, GitRepoDeleter> OpenGitRepository(const string &path) {
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

// Schema definition for git_tree (ALWAYS includes repo_path)
inline void DefineGitTreeSchema(vector<LogicalType> &return_types, 
                                vector<string> &names) {
    return_types = {
        LogicalType::VARCHAR,    // repo_path (ALWAYS first)
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::TIMESTAMP,  // commit_date
        LogicalType::VARCHAR,    // path
        LogicalType::INTEGER,    // mode
        LogicalType::VARCHAR,    // blob_hash
        LogicalType::BIGINT,     // size
        LogicalType::VARCHAR     // git_file_uri
    };
    names = {"repo_path", "commit_hash", "commit_date", "path", "mode", 
             "blob_hash", "size", "git_file_uri"};
}

// Schema definition for git_parents (ALWAYS includes repo_path)
inline void DefineGitParentsSchema(vector<LogicalType> &return_types,
                                   vector<string> &names) {
    return_types = {
        LogicalType::VARCHAR,    // repo_path (ALWAYS first)
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // parent_hash
        LogicalType::INTEGER     // parent_index
    };
    names = {"repo_path", "commit_hash", "parent_hash", "parent_index"};
}

// Output helper for git_tree rows (repo_path is REQUIRED)
inline void OutputGitTreeRow(DataChunk &output, idx_t row_idx, 
                             const GitTreeRow &row,
                             const string &repo_path) {
    idx_t col = 0;
    output.SetValue(col++, row_idx, Value(repo_path));
    output.SetValue(col++, row_idx, Value(row.commit_hash));
    output.SetValue(col++, row_idx, Value::TIMESTAMP(row.commit_date));
    output.SetValue(col++, row_idx, Value(row.path));
    output.SetValue(col++, row_idx, Value::INTEGER(row.mode));
    output.SetValue(col++, row_idx, Value(row.blob_hash));
    output.SetValue(col++, row_idx, Value::BIGINT(row.size));
    output.SetValue(col++, row_idx, Value(row.git_file_uri));
}

// Output helper for git_parents rows (repo_path is REQUIRED)
inline void OutputGitParentsRow(DataChunk &output, idx_t row_idx,
                                const GitParentsRow &row,
                                const string &repo_path) {
    idx_t col = 0;
    output.SetValue(col++, row_idx, Value(repo_path));
    output.SetValue(col++, row_idx, Value(row.commit_hash));
    output.SetValue(col++, row_idx, Value(row.parent_hash));
    output.SetValue(col++, row_idx, Value::INTEGER(row.parent_index));
}

// Common parameter parsing helpers
UnifiedGitParams ParseUnifiedGitParams(TableFunctionBindInput &input, int ref_param_index = 1);
UnifiedGitParams ParseLateralGitParams(TableFunctionBindInput &input, int ref_param_index = 1);

} // namespace duckdb
```

### 2. `git_tree_functions.cpp` (NEW)

```cpp
#include "git_functions_common.hpp"
#include "git_functions.hpp"

namespace duckdb {

// git_tree implementation
unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
    DefineGitTreeSchema(return_types, names);  // Always includes repo_path
    auto params = ParseUnifiedGitParams(input, 1);
    // ... rest of bind logic
}

void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = const_cast<GitTreeFunctionData&>(data_p.bind_data->Cast<GitTreeFunctionData>());
    // ... setup
    for (idx_t i = 0; i < count; i++) {
        auto &row = bind_data.rows[bind_data.current_index + i];
        OutputGitTreeRow(output, i, row, bind_data.repo_path);  // repo_path required
    }
    // ... cleanup
}

// git_tree_each implementation
unique_ptr<FunctionData> GitTreeEachBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    DefineGitTreeSchema(return_types, names);  // Always includes repo_path
    // ... bind logic
}

// ... GitTreeEachFunction, etc.

void RegisterGitTreeFunctions(DatabaseInstance &db) {
    // Register git_tree
    TableFunctionSet git_tree_set("git_tree");
    // ... add variants
    ExtensionUtil::RegisterFunction(db, git_tree_set);
    
    // Register git_tree_each
    TableFunctionSet git_tree_each_set("git_tree_each");
    // ... add variants
    ExtensionUtil::RegisterFunction(db, git_tree_each_set);
}

} // namespace duckdb
```

### 3. `git_parents_functions.cpp` (NEW)

```cpp
#include "git_functions_common.hpp"
#include "git_functions.hpp"

namespace duckdb {

unique_ptr<FunctionData> GitParentsBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
    DefineGitParentsSchema(return_types, names);  // Always includes repo_path
    auto params = ParseUnifiedGitParams(input, 1);
    // ... rest of bind logic
}

void GitParentsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = const_cast<GitParentsFunctionData&>(data_p.bind_data->Cast<GitParentsFunctionData>());
    // ... setup
    for (idx_t i = 0; i < count; i++) {
        auto &row = bind_data.rows[bind_data.current_index + i];
        OutputGitParentsRow(output, i, row, bind_data.repo_path);  // repo_path required
    }
    // ... cleanup
}

void RegisterGitParentsFunction(DatabaseInstance &db) {
    // Register all variants
    // ...
}

} // namespace duckdb
```

### 4. `git_read_functions.cpp` (NEW)

```cpp
#include "git_functions_common.hpp"
#include "git_functions.hpp"

namespace duckdb {

// git_read and git_read_each implementations
// These use "uri" as first column (different paradigm)

void RegisterGitReadFunctions(DatabaseInstance &db) {
    // Register git_read and git_read_each
    // ...
}

} // namespace duckdb
```

### 5. `git_uri_function.cpp` (NEW)

```cpp
#include "git_functions.hpp"

namespace duckdb {

static void GitUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    // ... implementation
}

void RegisterGitUriFunction(DatabaseInstance &db) {
    // Register scalar function
    // ...
}

} // namespace duckdb
```

### 6. `git_functions.cpp` (EXISTING - Reduced)

```cpp
// Only contains ORIGINAL functions from main branch:
// - git_log (no _each variant in main!)
// - git_branches (no _each variant in main!)
// - git_tags (no _each variant in main!)
// - Main registration function

void RegisterGitFunctions(DatabaseInstance &db) {
    RegisterGitLogFunction(db);
    RegisterGitBranchesFunction(db);
    RegisterGitTagsFunction(db);
    
    // NEW functions (defined in their own files)
    RegisterGitLogEachFunction(db);   // NEW! from git_log_each.cpp
    RegisterGitBranchesEachFunction(db); // NEW! from git_branches_each.cpp
    RegisterGitTagsEachFunction(db);  // NEW! from git_tags_each.cpp
    RegisterGitTreeFunctions(db);     // NEW! from git_tree_functions.cpp
    RegisterGitParentsFunction(db);   // NEW! from git_parents_functions.cpp
    RegisterGitReadFunctions(db);     // NEW! from git_read_functions.cpp
    RegisterGitUriFunction(db);       // NEW! from git_uri_function.cpp
}
```

### 7. Additional NEW Files for _each Functions

Since ALL _each functions are new, we should split them out too:

- `git_log_each.cpp` (NEW: git_log_each)
- `git_branches_each.cpp` (NEW: git_branches_each)
- `git_tags_each.cpp` (NEW: git_tags_each)

## Implementation Steps

### Phase 1: Create Common Infrastructure
1. Create `git_functions_common.hpp` with shared helpers
2. Update CMakeLists.txt to include new source files
3. Test that build still works

### Phase 2: Extract NEW Functions
1. Move git_tree and git_tree_each to `git_tree_functions.cpp`
2. Move git_parents to `git_parents_functions.cpp`
3. Move git_read and git_read_each to `git_read_functions.cpp`
4. Move git_uri to `git_uri_function.cpp`
5. Update git_functions.cpp to call the new registration functions

### Phase 3: Apply Fixes
1. Use common helpers to ensure repo_path is always first
2. Fix all output functions to use helpers
3. Update tests to expect repo_path

## Benefits of This Approach

### For PR Review
- **Clear separation**: Reviewers can see NEW code in dedicated files
- **Smaller diffs**: Each file is focused on one feature
- **Easy to verify**: No mixing of old and new code
- **Better context**: Related functions grouped together

### For Maintenance
- **Modular structure**: Each feature in its own file
- **Shared helpers**: DRY principle, single source of truth
- **Consistent patterns**: All functions use same helpers
- **Easy to extend**: Add new functions to appropriate file

### For Testing
- **Isolated changes**: Can test each file independently
- **Clear dependencies**: Shared code in common header
- **Consistent behavior**: All functions include repo_path

## File Size Comparison

| File | Before | After | Change |
|------|--------|-------|--------|
| git_functions.cpp | 2866 lines | 1200 lines | -1666 |
| git_functions_common.hpp | 0 | 200 lines | +200 |
| git_tree_functions.cpp | 0 | 400 lines | +400 |
| git_parents_functions.cpp | 0 | 150 lines | +150 |
| git_read_functions.cpp | 0 | 500 lines | +500 |
| git_uri_function.cpp | 0 | 100 lines | +100 |
| **Total** | 2866 lines | 2550 lines | **-316** |

## Migration Checklist

- [ ] Create git_functions_common.hpp
- [ ] Create git_tree_functions.cpp
- [ ] Create git_parents_functions.cpp  
- [ ] Create git_read_functions.cpp
- [ ] Create git_uri_function.cpp
- [ ] Update CMakeLists.txt
- [ ] Move code to new files
- [ ] Update git_functions.cpp to call new registration functions
- [ ] Test build
- [ ] Run all tests
- [ ] Update documentation

## Questions Resolved

1. **include_repo_path parameter?** NO - Always include it, no parameter needed
2. **Backward compatibility?** Not a concern for new functions
3. **File organization?** Separate files for new functions improves PR review

---

*This structure makes the PR much cleaner and easier to review while also improving long-term maintainability.*