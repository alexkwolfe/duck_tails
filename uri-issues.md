# URI Schema Refactor Issues and Status

## Overview
This document tracks the issues encountered during the URI schema standardization refactor for the Duck Tails extension. The refactor aimed to implement consistent 8-column prefixes across `git_tree`, `git_read`, and related functions to enable seamless LATERAL joins.

## Completed Work

### 1. URI Schema Standardization ✅
- Updated `GitTreeRow` struct from 10 to 14 fields
- Added new fields: `repo_path`, `tree_hash`, `kind`, `is_text`, `encoding`
- Renamed fields for consistency: `git_file_uri` → `git_uri`, `size` → `size_bytes`, `path` → `file_path`
- Implemented consistent 8-column prefix across all git functions

### 2. Tree Hash Computation ✅
- **Issue**: `tree_hash` field was empty in git_tree output
- **Root Cause**: Missing computation in `traverse_tree` function
- **Fix**: Added `oid_to_hex(git_tree_id(tree))` to properly compute tree hash
- **Status**: Fixed and committed

### 3. Object Type Consistency ✅
- **Issue**: All objects incorrectly hardcoded as "blob"
- **Root Cause**: Hardcoded string instead of filemode detection
- **Fix**: Implemented proper filemode detection using git filemode constants
  ```cpp
  switch (mode) {
      case GIT_FILEMODE_BLOB:
      case GIT_FILEMODE_BLOB_EXECUTABLE:
          kind = "file";
          break;
      case GIT_FILEMODE_LINK:
          kind = "symlink";
          break;
      // ... etc
  }
  ```
- **Status**: Fixed and committed

## Critical Issue: LATERAL Join Segmentation Fault

### Problem Description
LATERAL joins between git functions cause segmentation faults after the URI schema refactor. The issue manifests as memory corruption during string operations.

### Root Cause Analysis

1. **Schema Mismatch** (Partially Fixed)
   - GitTreeInOutFunction and GitTreeEachFunction were outputting 7-8 fields instead of 14
   - Fixed by implementing `OutputGitTreeRow` helper function
   - However, segfault persists after fix

2. **String Memory Lifecycle Issues** (Ongoing)
   - **Symptom**: AddressSanitizer detects poison pattern `0xbebebebebebebebe` in string_t objects
   - **Location**: UTF8 verification during LATERAL join processing
   - **Attempted Fixes**:
     - Changed from `output.SetValue(Value(string))` to `StringVector::AddString()` for proper string lifecycle
     - Modified GitTreeRow construction to use explicit string copies and move semantics
   - **Current Status**: Still experiencing segfaults despite fixes

### Investigation Findings

1. **Pre-refactor Working State**
   - Commit `bac6fd5` confirmed working with LATERAL joins
   - Used 10-field GitTreeRow structure
   - Used `StringVector::AddString()` for string handling

2. **Post-refactor Issues**
   - 14-field GitTreeRow structure
   - Initial implementation used `output.SetValue()` which creates temporary references
   - String memory corruption occurs even after reverting to `StringVector::AddString()`

### Test Case
```sql
WITH commits AS (
    SELECT commit_hash FROM git_log('.', 'HEAD~2')
)
SELECT COUNT(*) 
FROM commits, 
LATERAL (SELECT * FROM git_tree_each(commits.commit_hash)) AS tree_files;
```

## Other Issues

### 1. Git Clone Compilation Errors ✅
- **Issue**: StringVector API mismatch in git_clone.cpp
- **Resolution**: Temporarily disabled git_clone registration
- **TODO**: Fix StringVector calls and re-enable functionality

### 2. Test Expectations
- **Issue**: `git_uri_composability.test` expects 2 branches but finds 13
- **Status**: Needs investigation - likely test environment has more branches than expected

## Current Build Status
- Clean build in progress after fixing compilation errors
- All field name mismatches resolved (`row.path` → `row.file_path`, etc.)
- Git clone functionality commented out to avoid build errors

## Next Steps

1. **Fix String Memory Corruption**
   - Investigate deeper into how GitTreeRow objects are passed between functions
   - Consider using shared_ptr or unique_ptr for string management
   - Review DuckDB's memory management patterns for LATERAL joins

2. **Validate LATERAL Join Functionality**
   - Once memory issues are fixed, thoroughly test all LATERAL join combinations
   - Ensure git_tree_each, git_log_each, git_branches_each all work correctly

3. **Re-enable Git Clone**
   - Fix StringVector API usage in git_clone.cpp
   - Uncomment registration code
   - Add tests for git_clone functionality

4. **Update Test Expectations**
   - Review and update git_uri_composability.test for actual branch counts
   - Add comprehensive tests for new URI schema
   - Add regression tests for LATERAL joins

## Technical Details

### Memory Layout Changes
```cpp
// Pre-refactor (10 fields)
struct GitTreeRow {
    string commit_hash;
    timestamp_t commit_date;
    string path;
    int32_t mode;
    string blob_hash;
    int64_t size;
    string git_file_uri;
    string file_path;
    string file_ext;
    string ref;
};

// Post-refactor (14 fields)
struct GitTreeRow {
    string git_uri;        // Renamed from git_file_uri
    string repo_path;      // NEW
    string commit_hash;
    string tree_hash;      // NEW
    string file_path;      // Renamed from path
    string file_ext;
    string ref;
    string blob_hash;
    timestamp_t commit_date;
    int32_t mode;
    int64_t size_bytes;    // Renamed from size
    string kind;           // NEW
    bool is_text;          // NEW
    string encoding;       // NEW
};
```

### String Handling Patterns
```cpp
// Problematic pattern (creates temporary references)
output.SetValue(col, row_idx, Value(row.field));

// Correct pattern (proper string lifecycle)
FlatVector::GetData<string_t>(output.data[col])[row_idx] = 
    StringVector::AddString(output.data[col], row.field);
```

## References
- Original plan: `uri-clarity.md`
- Git clone issues: `git-clone.md`
- Commit with fixes: `25e0263` - "Fix tree_hash computation and object type consistency"
- Working pre-refactor commit: `bac6fd5`