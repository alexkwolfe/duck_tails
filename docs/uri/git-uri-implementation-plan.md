# Git URI Implementation Plan for Duck Tails

## Executive Summary

This document provides a comprehensive implementation plan for the git+file:// URI format as specified in claude-uri-design.md. The implementation maintains backward compatibility while introducing ultrashort refspecs and standardizing URI output formats.

## Implementation Phases

### Phase 1: Parser Infrastructure (Day 1)
**Goal:** Create robust URI parsing that handles all input formats

#### 1.1 GitUri Internal Structure
**File:** `src/include/git_types.hpp` (NEW)
```cpp
namespace duckdb {

struct GitUri {
    string abs_repo_path;      // Always absolute path to repo root
    string revspec;            // Can be ref, range, or empty (use current branch)
    optional<string> path;     // File/directory path within repo
    
    // Computed fields (lazy evaluation)
    bool is_range = false;     // Contains .. or ...
    string range_type;         // "two-dot" or "three-dot"
    
    // Helper methods
    bool IsUltrashort() const;
    bool HasPath() const { return path.has_value(); }
    string ToString() const;   // Always returns git+file:// format
};

} // namespace duckdb
```

#### 1.2 Enhanced Parser
**File:** `src/git_filesystem.cpp` (MODIFY)

Add new parsing functions:
```cpp
// Main entry point for URI parsing
static GitUri ParseGitUri(const string &input);

// Helper functions
static GitUri ParseFullUri(const string &uri);
static GitUri ParseFilesystemPath(const string &path);
static GitUri ParseUltrashortRefspec(const string &input);
static bool IsUltrashortRefspec(const string &input);
static string GetCurrentBranch(git_repository *repo);
static string FindGitRepository(const string &start_path);

// Fragment parsing (#revspec[:path])
static pair<string, optional<string>> ParseFragment(const string &fragment);

// Legacy @ syntax parsing (backward compatibility)
static pair<string, string> ParseLegacySyntax(const string &uri);
```

**Testing Requirements:**
- Test all input format variations
- Test ultrashort refspec detection
- Test fragment parsing with colons in paths
- Test legacy @ syntax compatibility
- Test repository discovery from various paths

### Phase 2: URI Construction (Day 1)
**Goal:** Standardize URI output to git+file:// format

#### 2.1 Update ConstructGitUri
**File:** `src/git_functions.cpp` (MODIFY)

Replace existing implementation:
```cpp
static string ConstructGitUri(const string &repo_path, 
                              const string &file_path, 
                              const string &revision) {
    // Ensure absolute repository path
    string abs_repo = MakeAbsolutePath(repo_path);
    
    // Build git+file:// URI with fragment syntax
    string uri = "git+file://" + abs_repo + "#" + revision;
    
    if (!file_path.empty()) {
        uri += ":" + file_path;
    }
    
    return uri;
}
```

#### 2.2 Backward Compatibility Layer
**File:** `src/git_filesystem.cpp` (MODIFY)

Update `GitPath::Parse` to handle both formats:
```cpp
GitPath GitPath::Parse(const string &git_url) {
    // Handle both git:// and git+file:// schemes
    GitUri parsed = ParseGitUri(git_url);
    
    // Convert to GitPath for existing code
    GitPath result;
    result.repository_path = parsed.abs_repo_path;
    result.file_path = parsed.path.value_or("");
    result.revision = parsed.revspec.empty() ? 
                     GetCurrentBranch(parsed.abs_repo_path) : 
                     parsed.revspec;
    
    return result;
}
```

### Phase 3: Function Overloads (Day 2)
**Goal:** Add URI overloads to all git functions

#### 3.1 Unified Parameter Parsing
**File:** `src/git_functions.cpp` (MODIFY)

Enhance `ParseUnifiedGitParams` to detect URI input:
```cpp
static ParsedGitParams ParseUnifiedGitParams(
    const string &param1,
    const optional<string> &param2 = nullopt) {
    
    ParsedGitParams result;
    
    // Check if first parameter is a URI
    if (IsGitUri(param1)) {
        GitUri uri = ParseGitUri(param1);
        result.repo_path = uri.abs_repo_path;
        result.ref = uri.revspec;
        result.file_path = uri.path;
        result.is_uri_input = true;
    } else {
        // Traditional two-parameter form
        result.repo_path = param1;
        result.ref = param2.value_or("HEAD");
        result.is_uri_input = false;
    }
    
    // Handle current branch default
    if (result.ref.empty()) {
        result.needs_current_branch = true;
    }
    
    return result;
}
```

#### 3.2 Function Signature Updates
**Files:** `src/include/git_functions.hpp`, `src/git_functions.cpp`

Add overloads for each function:
```cpp
// git_tree overloads
static void GitTreeFunction(DataChunk &args, ExpressionState &state, Vector &result);
static void GitTreeUriFunction(DataChunk &args, ExpressionState &state, Vector &result);

// git_log overloads  
static void GitLogFunction(DataChunk &args, ExpressionState &state, Vector &result);
static void GitLogUriFunction(DataChunk &args, ExpressionState &state, Vector &result);

// Similar for git_branches, git_tags, git_read, git_parents
```

#### 3.3 Registration Updates
**File:** `src/git_extension.cpp` (MODIFY)

Register both traditional and URI overloads:
```cpp
// git_tree registration
auto git_tree_func = TableFunction("git_tree", {LogicalType::VARCHAR, LogicalType::VARCHAR}, 
                                   GitTreeFunction, GitTreeBind);
git_tree_func.named_parameters["ref"] = LogicalType::VARCHAR;
connection.CreateTableFunction(git_tree_func);

// git_tree URI overload
auto git_tree_uri_func = TableFunction("git_tree", {LogicalType::VARCHAR}, 
                                       GitTreeUriFunction, GitTreeUriBind);
connection.CreateTableFunction(git_tree_uri_func);

// Similar for all other functions
```

### Phase 4: Current Branch Resolution (Day 2)
**Goal:** Implement default to current branch behavior

#### 4.1 Branch Resolution Function
**File:** `src/git_utils.cpp` (NEW)
```cpp
namespace duckdb {

string GetCurrentBranch(const string &repo_path) {
    git_repository *repo = nullptr;
    
    if (git_repository_open(&repo, repo_path.c_str()) != 0) {
        throw InvalidInputException("Failed to open repository: %s", repo_path);
    }
    
    git_reference *head = nullptr;
    string result;
    
    if (git_repository_head(&head, repo) != 0) {
        // Detached HEAD or error - fall back to SHA
        git_oid head_oid;
        if (git_reference_name_to_id(&head_oid, repo, "HEAD") == 0) {
            char sha[GIT_OID_HEXSZ + 1];
            git_oid_tostr(sha, sizeof(sha), &head_oid);
            result = string(sha);
        } else {
            result = "HEAD";  // Ultimate fallback
        }
    } else {
        const char *branch = git_reference_shorthand(head);
        result = string(branch);
        git_reference_free(head);
    }
    
    git_repository_free(repo);
    return result;
}

} // namespace duckdb
```

#### 4.2 Execution-Time Resolution
**File:** `src/git_functions.cpp` (MODIFY)

Update execution functions to resolve current branch when needed:
```cpp
static void GitTreeExecute(ClientContext &context, TableFunctionInput &data_p, 
                           DataChunk &input, DataChunk &output) {
    auto &data = data_p.bind_data->Cast<GitTreeBindData>();
    
    // Resolve current branch if needed
    if (data.needs_current_branch) {
        data.resolved_ref = GetCurrentBranch(data.repo_path);
    }
    
    // Continue with existing logic using resolved_ref
    // ...
}
```

### Phase 5: Ultrashort Refspec Support (Day 3)
**Goal:** Enable ultrashort refspecs like "HEAD" using cwd

#### 5.1 Detection Logic
**File:** `src/git_filesystem.cpp` (MODIFY)
```cpp
static bool IsUltrashortRefspec(const string &input) {
    // Empty or starts with scheme/absolute path - not ultrashort
    if (input.empty() || 
        StringUtil::StartsWith(input, "git://") || 
        StringUtil::StartsWith(input, "git+file://") ||
        StringUtil::StartsWith(input, "/")) {
        return false;
    }
    
    // Common git refs
    static const unordered_set<string> common_refs = {
        "HEAD", "main", "master", "develop", "dev", 
        "staging", "production", "release"
    };
    
    // Check if it's a common ref or contains ref syntax
    string ref_part = input;
    auto colon_pos = input.find(':');
    if (colon_pos != string::npos) {
        ref_part = input.substr(0, colon_pos);
    }
    
    if (common_refs.count(ref_part)) {
        return true;
    }
    
    // Contains revision syntax
    if (ref_part.find('~') != string::npos ||
        ref_part.find('^') != string::npos ||
        ref_part.find("@{") != string::npos) {
        return true;
    }
    
    // Range syntax
    if (ref_part.find("..") != string::npos) {
        return true;
    }
    
    // Looks like SHA (6-40 hex chars)
    if (ref_part.length() >= 6 && ref_part.length() <= 40) {
        bool is_hex = all_of(ref_part.begin(), ref_part.end(),
                             [](char c) { return isxdigit(c); });
        if (is_hex) return true;
    }
    
    return false;
}
```

#### 5.2 CWD Repository Discovery
**File:** `src/git_filesystem.cpp` (MODIFY)
```cpp
static GitUri ParseUltrashortRefspec(const string &input) {
    GitUri result;
    
    // Get current working directory
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        throw IOException("Failed to get current directory");
    }
    
    // Find repository from cwd
    result.abs_repo_path = FindGitRepository(string(cwd));
    if (result.abs_repo_path.empty()) {
        throw InvalidInputException(
            "Not in a git repository (ultrashort refspec requires git repo)"
        );
    }
    
    // Parse revspec[:path]
    auto colon_pos = input.find(':');
    if (colon_pos != string::npos) {
        result.revspec = input.substr(0, colon_pos);
        result.path = input.substr(colon_pos + 1);
    } else {
        result.revspec = input;
    }
    
    // Detect range type
    if (result.revspec.find("...") != string::npos) {
        result.is_range = true;
        result.range_type = "three-dot";
    } else if (result.revspec.find("..") != string::npos) {
        result.is_range = true;
        result.range_type = "two-dot";
    }
    
    return result;
}
```

### Phase 6: Test Suite (Day 3-4)
**Goal:** Comprehensive test coverage for new functionality

#### 6.1 Parser Tests
**File:** `test/sql/git_uri_parser.test` (NEW)
```sql
# Test URI format parsing

# Test git+file:// format
query I
SELECT COUNT(*) FROM git_tree('git+file:///Users/alex/Dev/duck_tails#HEAD');
----
<expected_count>

# Test fragment with path
query I
SELECT COUNT(*) FROM git_tree('git+file:///Users/alex/Dev/duck_tails#HEAD:src/');
----
<expected_count>

# Test legacy git:// format (backward compatibility)
query I
SELECT COUNT(*) FROM git_tree('git:///Users/alex/Dev/duck_tails@HEAD');
----
<expected_count>

# Test ultrashort refspec
query I
SELECT COUNT(*) FROM git_tree('HEAD');
----
<expected_count>

# Test ultrashort with path
query I
SELECT COUNT(*) FROM git_tree('HEAD:src/');
----
<expected_count>

# Test range specifications
query I
SELECT COUNT(*) FROM git_log('main..feature');
----
<expected_count>

# Test three-dot range
query I
SELECT COUNT(*) FROM git_log('main...feature');
----
<expected_count>
```

#### 6.2 Output Format Tests
**File:** `test/sql/git_uri_output.test` (NEW)
```sql
# Verify output is always git+file:// format

# Check git_tree output
query I
SELECT git_file_uri FROM git_tree('.') LIMIT 1;
----
<match_pattern>git+file://.*#.*

# Check git_log output
query I
SELECT uri FROM git_log('.') LIMIT 1;
----
<match_pattern>git+file://.*#.*

# Verify absolute paths in output
query I
SELECT SUBSTRING(git_file_uri, 1, 11) FROM git_tree('.') LIMIT 1;
----
git+file://
```

#### 6.3 LATERAL Join Tests
**File:** `test/sql/git_uri_lateral.test` (NEW)
```sql
# Test LATERAL joins with new URI format

# Test with multiple refs
statement ok
WITH refs AS (SELECT 'HEAD' as r UNION ALL SELECT 'HEAD~1')
SELECT r, COUNT(*) as file_count 
FROM refs, LATERAL git_tree_each(r) t
GROUP BY r
ORDER BY r;

# Test with URI values
statement ok
WITH uris AS (
    SELECT 'git+file://' || CURRENT_SETTING('git_test_repo') || '#HEAD' as uri
    UNION ALL 
    SELECT 'git+file://' || CURRENT_SETTING('git_test_repo') || '#HEAD~1'
)
SELECT COUNT(*) FROM uris, LATERAL git_tree_each(uri) t;
```

#### 6.4 Migration Tests
**File:** `test/sql/git_uri_migration.test` (NEW)
```sql
# Test that old and new formats work identically

# Create test data with both formats
statement ok
CREATE TABLE test_uris AS
SELECT 
    'git://' || CURRENT_SETTING('git_test_repo') || '/README.md@HEAD' as old_format,
    'git+file://' || CURRENT_SETTING('git_test_repo') || '#HEAD:README.md' as new_format;

# Verify both formats return same data
query I
SELECT 
    (SELECT COUNT(*) FROM git_read(old_format)) = 
    (SELECT COUNT(*) FROM git_read(new_format))
FROM test_uris;
----
true
```

#### 6.5 Update Existing Tests
**Strategy:** Modify existing tests to work with new output format

1. **Update expected outputs** in existing test files to expect `git+file://` format
2. **Preserve test logic** - don't delete tests, adapt them
3. **Add parallel tests** for backward compatibility where needed

**Files to update:**
- `test/sql/git_tree_each_comprehensive.test`
- `test/sql/git_log_each_comprehensive.test`
- `test/sql/git_read_each_comprehensive.test`
- `test/sql/git_branches_each_comprehensive.test`
- `test/sql/git_tags_each_comprehensive.test`
- `test/sql/git_tree_parents.test`
- `test/sql/git_read_functions.test`

**Update pattern example:**
```sql
# OLD expectation
query I
SELECT git_file_uri FROM git_tree('.') LIMIT 1;
----
git://.*@.*

# NEW expectation
query I
SELECT git_file_uri FROM git_tree('.') LIMIT 1;
----
git+file://.*#.*
```

### Phase 7: Integration Testing (Day 4)
**Goal:** Ensure seamless integration with DuckDB readers

#### 7.1 Reader Integration Tests
**File:** `test/sql/git_uri_readers.test` (NEW)
```sql
# Test integration with DuckDB readers

# Test with read_csv using LATERAL
statement ok
WITH csv_files AS (
    SELECT git_file_uri FROM git_tree('HEAD:data/') 
    WHERE path LIKE '%.csv' LIMIT 1
)
SELECT * FROM csv_files, LATERAL read_csv(csv_files.git_file_uri);

# Test with read_json_auto using LATERAL
statement ok
WITH json_files AS (
    SELECT git_file_uri FROM git_tree('HEAD:data/')
    WHERE path LIKE '%.json' LIMIT 1
)
SELECT * FROM json_files, LATERAL read_json_auto(json_files.git_file_uri);

# Test with read_text using LATERAL
statement ok
WITH readme AS (
    SELECT git_file_uri FROM git_tree('HEAD')
    WHERE path = 'README.md'
)
SELECT * FROM readme, LATERAL read_text(readme.git_file_uri);
```

#### 7.2 Complex Query Tests
**File:** `test/sql/git_uri_complex.test` (NEW)
```sql
# Test complex query patterns

# Pipeline URIs through readers
statement ok
WITH files AS (
    SELECT git_file_uri as uri 
    FROM git_tree('main:data/')
    WHERE path LIKE '%.json'
)
SELECT COUNT(*) FROM files, LATERAL read_json_auto(files.uri);

# Multiple refs with aggregation
statement ok
WITH commits AS (
    SELECT commit_hash FROM git_log('HEAD~5..HEAD')
)
SELECT c.commit_hash, COUNT(*) as file_count
FROM commits c, LATERAL git_tree_each('.', c.commit_hash) t
GROUP BY c.commit_hash;
```

### Phase 8: Documentation (Day 5)
**Goal:** User-facing documentation and migration guide

#### 8.1 API Documentation
**File:** `docs/git-uri-api.md` (NEW)
- Document all URI formats
- Provide examples for each function
- Explain ultrashort refspecs
- Cover LATERAL join patterns

#### 8.2 Migration Guide
**File:** `docs/git-uri-migration.md` (NEW)
- Before/after examples
- Common patterns and their updates
- Troubleshooting guide
- FAQ section

#### 8.3 Update README
**File:** `README.md` (MODIFY)
- Update examples to use new format
- Add note about URI format change
- Link to migration guide

## Implementation Timeline

### Day 1: Parser Infrastructure
- [ ] Create GitUri structure
- [ ] Implement ParseGitUri and helpers
- [ ] Update ConstructGitUri
- [ ] Add backward compatibility layer
- [ ] Unit tests for parser

### Day 2: Function Integration  
- [ ] Add URI overloads to all functions
- [ ] Implement current branch resolution
- [ ] Update bind and execute functions
- [ ] Integration tests for functions

### Day 3: Advanced Features
- [ ] Implement ultrashort refspec support
- [ ] Add CWD repository discovery
- [ ] Create comprehensive test suite
- [ ] Update existing tests

### Day 4: Testing & Polish
- [ ] Reader integration tests
- [ ] Complex query tests
- [ ] Performance testing
- [ ] Bug fixes from testing

### Day 5: Documentation
- [ ] API documentation
- [ ] Migration guide
- [ ] Update examples
- [ ] Code review and cleanup

## Risk Mitigation

### Backward Compatibility
- **Risk:** Breaking existing user queries
- **Mitigation:** Accept both formats, only output new format
- **Testing:** Parallel tests for both formats

### Performance Impact
- **Risk:** Slower parsing with more complex logic
- **Mitigation:** Cache parsed URIs, optimize detection logic
- **Testing:** Performance benchmarks before/after

### Current Branch Resolution
- **Risk:** Performance hit from opening repository
- **Mitigation:** Cache branch resolution per repository
- **Testing:** Measure impact on query performance

### Edge Cases
- **Risk:** Unexpected input formats or repository states
- **Mitigation:** Comprehensive error handling and fallbacks
- **Testing:** Fuzzing and edge case test suite

## Success Criteria

1. **All existing tests pass** with updated expectations
2. **New URI format tests** achieve 100% coverage
3. **Performance regression** < 5% for typical queries
4. **Zero breaking changes** for existing user queries
5. **Documentation complete** with migration guide
6. **Ultrashort refspecs work** in common scenarios

## Dependencies

- libgit2 (existing)
- DuckDB core (existing)
- No new external dependencies required

## Notes

- Keep GitUri struct internal - users only see VARCHAR
- Prioritize backward compatibility over ideal design
- Focus on common use cases for ultrashort refspecs
- Consider caching for performance optimization
- Ensure thread safety for all new code

## Appendix: Test Matrix

| Feature | Parser | Function | Output | LATERAL | Reader | Migration |
|---------|--------|----------|--------|---------|--------|-----------|
| git+file:// | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| git:// legacy | ✓ | ✓ | → git+file:// | ✓ | ✓ | ✓ |
| Ultrashort | ✓ | ✓ | ✓ | ✓ | ✓ | N/A |
| Ranges (..) | ✓ | ✓ | ✓ | ✓ | N/A | ✓ |
| Three-dot (...) | ✓ | ✓ | ✓ | ✓ | N/A | ✓ |
| Path prefix | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Current branch | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

This comprehensive plan ensures smooth implementation of the new git+file:// URI format while maintaining full backward compatibility and adding powerful new features like ultrashort refspecs.