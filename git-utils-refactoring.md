# Git Utils Refactoring Plan

## Overview

This document outlines a comprehensive refactoring to extract common git operations into a shared `git_utils` module. This refactoring must be completed **before** the URI schema standardization work, as it provides the foundation utilities needed for consistent URI handling across all functions.

## Current State Analysis

### Code Duplication Patterns

**Git Object Operations (repeated 8+ times):**
```cpp
// Pattern 1: Commit lookup + error handling
git_commit *commit = nullptr;
int error = git_commit_lookup(&commit, repo, &oid);
if (error != 0) {
    // Error handling varies across functions
}

// Pattern 2: Tree extraction from commit
git_tree *tree = nullptr;
error = git_commit_tree(&tree, commit);
if (error != 0) {
    // Cleanup and error handling
}

// Pattern 3: OID to hex conversion
char hex[GIT_OID_HEXSZ + 1];
git_oid_tostr(hex, sizeof(hex), oid);
return string(hex);
```

**URI Processing (duplicated across git_tree, git_read):**
```cpp
// Extract components from git URI
string file_path, ref;
ParseGitUriComponents(git_uri, file_path, ref);
string file_ext = ExtractFileExtension(file_path);
```

**Resource Management (no RAII, manual cleanup everywhere):**
```cpp
// Repeated throughout every function
git_repository_free(repo);
git_commit_free(commit);
git_tree_free(tree);
// Easy to leak or double-free
```

### Functions With Heavy Duplication

1. **git_log:** Commit traversal, OID conversion
2. **git_tree:** Tree traversal, URI construction, hash resolution
3. **git_read:** URI parsing, blob lookup, content analysis
4. **git_parents:** Commit traversal, parent extraction
5. **git_filesystem:** Tree/blob lookups for file access

## Target Architecture: git_utils Module

### Core Utilities Namespace
```cpp
namespace duckdb {
// Follow existing DuckDB patterns - single namespace
// All git utility functions in duckdb namespace with git_utils prefix
}
```

## Shared Data Structures

### 1. Git Object Hashes
```cpp
struct GitObjectHashes {
    string commit_hash;   // SHA of commit
    string tree_hash;     // SHA of tree containing file
    string blob_hash;     // SHA of file content
    
    GitObjectHashes() = default;
    GitObjectHashes(const string &commit, const string &tree, const string &blob)
        : commit_hash(commit), tree_hash(tree), blob_hash(blob) {}
};
```

### 2. URI Components (Complete Resolution)
```cpp
struct GitUriComponents {
    // Input components
    string git_uri;           // Full git:// URI
    string repo_path;         // Filesystem path to repository
    string file_path;         // Path within repository
    string file_ext;          // File extension (.js, .cpp, etc.)
    string ref;               // Git reference (branch/tag/SHA)
    
    // Resolved hashes
    string commit_hash;       // Resolved commit SHA
    string tree_hash;         // Tree containing the file
    string blob_hash;         // File content hash
    
    GitUriComponents() = default;
};
```

### 3. Content Analysis Properties
```cpp
struct ContentProperties {
    string kind;              // "blob", "tree", "commit", etc.
    bool is_text;             // Whether content is human-readable text
    string encoding;          // Text encoding (UTF-8, ASCII, etc.)
    int64_t size_bytes;       // Content size in bytes
    
    ContentProperties() : is_text(false), size_bytes(0) {}
};
```

## RAII Resource Management

### 1. Repository Wrapper
```cpp
class GitRepository {
private:
    git_repository *repo_;
    string path_;
    
public:
    explicit GitRepository(const string &path);
    ~GitRepository();
    
    // Non-copyable, movable
    GitRepository(const GitRepository&) = delete;
    GitRepository& operator=(const GitRepository&) = delete;
    GitRepository(GitRepository&& other) noexcept;
    GitRepository& operator=(GitRepository&& other) noexcept;
    
    git_repository* get() const { return repo_; }
    const string& path() const { return path_; }
    bool is_valid() const { return repo_ != nullptr; }
};
```

### 2. Commit Wrapper  
```cpp
class GitCommit {
private:
    git_commit *commit_;
    
public:
    GitCommit(git_repository *repo, const git_oid *oid);
    GitCommit(git_repository *repo, const string &ref);
    ~GitCommit();
    
    // Non-copyable, movable
    GitCommit(const GitCommit&) = delete;
    GitCommit& operator=(const GitCommit&) = delete;
    GitCommit(GitCommit&& other) noexcept;
    GitCommit& operator=(GitCommit&& other) noexcept;
    
    git_commit* get() const { return commit_; }
    bool is_valid() const { return commit_ != nullptr; }
    
    // Convenience methods
    string get_hash() const;
    timestamp_t get_timestamp() const;
    string get_tree_hash() const;
};
```

### 3. Tree Wrapper
```cpp
class GitTree {
private:
    git_tree *tree_;
    
public:
    GitTree(git_repository *repo, const git_oid *oid);
    GitTree(GitCommit &commit);  // Extract tree from commit
    ~GitTree();
    
    // Non-copyable, movable
    GitTree(const GitTree&) = delete;
    GitTree& operator=(const GitTree&) = delete;
    GitTree(GitTree&& other) noexcept;
    GitTree& operator=(GitTree&& other) noexcept;
    
    git_tree* get() const { return tree_; }
    bool is_valid() const { return tree_ != nullptr; }
    
    // Convenience methods
    string get_hash() const;
    bool has_entry(const string &path) const;
    string get_blob_hash(const string &path) const;
};
```

## Core Utility Functions

### 1. Hash Resolution Functions
```cpp
// Resolve all hashes for a file in one atomic operation
GitObjectHashes ResolveFileHashes(GitRepository &repo, const string &ref, const string &file_path);

// Individual hash resolution (for when you only need specific hashes)
string ResolveCommitHash(GitRepository &repo, const string &ref);
string GetTreeHashForCommit(GitRepository &repo, const string &commit_hash);
string GetBlobHashForFile(GitRepository &repo, const string &commit_hash, const string &file_path);

// Safe OID conversions with validation
string SafeOidToHex(const git_oid *oid);
git_oid SafeHexToOid(const string &hex_string);
```

### 2. URI Processing Functions
```cpp
// Complete URI parsing with hash resolution
GitUriComponents ParseAndResolveGitUri(const string &git_uri);

// Individual URI operations
string ConstructGitUri(const string &repo_path, const string &file_path, const string &ref);
string ExtractFileExtension(const string &file_path);
void ParseGitUriComponents(const string &git_uri, string &repo_path, string &file_path, string &ref);
```

### 3. Content Analysis Functions
```cpp
// Analyze git object content
ContentProperties AnalyzeGitObject(GitRepository &repo, const string &blob_hash);
ContentProperties AnalyzeGitBlob(git_repository *repo, const git_oid *blob_oid);

// Individual content analysis functions
string GetObjectKind(git_object *obj);
bool IsTextContent(const void *content, size_t size);
string DetectEncoding(const void *content, size_t size);
```

### 4. Error Handling & Utilities
```cpp
// Standardized git error handling
void ThrowGitError(const string &operation, const string &context = "");
void LogGitWarning(const string &operation, const string &context = "");

// Validation utilities
bool IsValidGitRepository(const string &path);
bool IsValidGitUri(const string &uri);
bool IsValidGitRef(GitRepository &repo, const string &ref);
```

## Implementation Plan

### Phase 0.0: Discovery & Environment Analysis (30 minutes) **NEW**

**Critical Prerequisites - Must be completed first:**

1. **Build System Analysis:**
```bash
# Primary build file: /Users/alex/Dev/duck_tails/CMakeLists.txt
# Extension sources: set(EXTENSION_SOURCES src/duck_tails_extension.cpp src/git_filesystem.cpp src/git_functions.cpp src/git_clone.cpp src/text_diff.cpp)
# libgit2 already linked: libgit2::libgit2package
```

2. **Test Framework Discovery:**
```bash
# Uses Catch2: /Users/alex/Dev/duck_tails/duckdb/third_party/catch/catch.hpp
# Test file pattern: /Users/alex/Dev/duck_tails/test/test_basic.cpp
# Test macros: TEST_CASE(...), REQUIRE(...) - Catch2 syntax, not Google Test
```

3. **Existing Git Integration Analysis:**
```bash
# Existing GitPath class in git_filesystem.hpp - MUST integrate, not duplicate
# Existing git operations in git_functions.cpp - patterns to preserve
# libgit2 version and capabilities used
```

4. **Namespace Analysis:**
```cpp
// Current pattern: namespace duckdb { ... } - single namespace
// NOT: namespace duckdb { namespace git_utils { } } - would break convention
```

**Discovery Tasks:**
```bash
# 1. Check libgit2 version and thread safety
pkg-config --modversion libgit2
rg -n "git_threads_init|git_libgit2_init" src/

# 2. Find existing git error handling patterns  
rg -A3 -B3 "git_error_last|IOException.*git" src/

# 3. Map existing GitPath integration points
rg -n "GitPath::" src/

# 4. Check for existing RAII patterns in DuckDB
rg -n "class.*RAII|unique_ptr.*git" src/
```

### Phase 0.1: Create Module Structure (30 minutes)

**Files to create:**
```
src/include/git_utils.hpp    - Header with all declarations
src/git_utils.cpp           - Implementation  
test/test_git_utils.cpp      - Unit tests (Catch2 format)
```

**Update build system in CMakeLists.txt:**
```cmake
# Line 18: Update EXTENSION_SOURCES
set(EXTENSION_SOURCES 
    src/duck_tails_extension.cpp 
    src/git_filesystem.cpp 
    src/git_functions.cpp 
    src/git_utils.cpp         # NEW
    src/git_clone.cpp 
    src/text_diff.cpp)
```

### Phase 0.2: Implement RAII Wrappers (1 hour)

**Priority order:**
1. `GitRepository` - Used by everything
2. `GitCommit` - Used by most functions  
3. `GitTree` - Used by tree operations
4. Error handling utilities

**Test approach:**
```cpp
TEST(GitUtils, GitRepositoryRAII) {
    {
        GitRepository repo(".");
        ASSERT_TRUE(repo.is_valid());
        // Repo automatically freed when out of scope
    }
    // Test that repository was properly freed
}
```

### Phase 0.3: Implement Hash Resolution (1.5 hours)

**Implementation priority:**
1. `SafeOidToHex()` - Replace all existing `oid_to_hex()` calls
2. `ResolveCommitHash()` - Core commit resolution 
3. `GetTreeHashForCommit()` - Extract tree from commit
4. `GetBlobHashForFile()` - File-specific blob lookup
5. `ResolveFileHashes()` - Combined operation

**Testing strategy:**
```cpp
TEST(GitUtils, ResolveFileHashes) {
    GitRepository repo(".");
    auto hashes = ResolveFileHashes(repo, "HEAD", "README.md");
    
    ASSERT_FALSE(hashes.commit_hash.empty());
    ASSERT_FALSE(hashes.tree_hash.empty());  
    ASSERT_FALSE(hashes.blob_hash.empty());
    
    // Verify hashes are valid SHA format
    ASSERT_EQ(hashes.commit_hash.length(), 40);
    ASSERT_TRUE(std::all_of(hashes.commit_hash.begin(), hashes.commit_hash.end(), ::isxdigit));
}
```

### Phase 0.4: Implement URI Processing (1 hour)

**Implementation priority:**
1. `ExtractFileExtension()` - Replace existing function
2. `ConstructGitUri()` - Enhance existing function
3. `ParseGitUriComponents()` - Complete existing function
4. `ParseAndResolveGitUri()` - New comprehensive function

**Testing strategy:**
```cpp
TEST(GitUtils, ParseAndResolveGitUri) {
    auto components = ParseAndResolveGitUri("git://./src/main.cpp@HEAD");
    
    ASSERT_EQ(components.file_ext, ".cpp");
    ASSERT_EQ(components.file_path, "src/main.cpp");
    ASSERT_EQ(components.ref, "HEAD");
    ASSERT_FALSE(components.commit_hash.empty());
    ASSERT_FALSE(components.blob_hash.empty());
}
```

### Phase 0.5: Implement Content Analysis (1 hour)

**Implementation priority:**
1. `GetObjectKind()` - Object type identification
2. `IsTextContent()` - Text detection
3. `DetectEncoding()` - Encoding detection  
4. `AnalyzeGitObject()` - Combined analysis

**Testing strategy:**
```cpp
TEST(GitUtils, AnalyzeGitObject) {
    GitRepository repo(".");
    auto hashes = ResolveFileHashes(repo, "HEAD", "README.md");
    auto props = AnalyzeGitObject(repo, hashes.blob_hash);
    
    ASSERT_EQ(props.kind, "blob");
    ASSERT_TRUE(props.is_text);  // README should be text
    ASSERT_GT(props.size_bytes, 0);
}
```

## Phase 0.6: Refactor Existing Functions (1.5 hours)

### Refactoring Strategy

**One function at a time, verify behavior unchanged:**

1. **git_log (easiest)** - Replace `oid_to_hex` calls with `SafeOidToHex`
2. **git_tree** - Use `ResolveFileHashes` and `ParseAndResolveGitUri`  
3. **git_read** - Use `ParseAndResolveGitUri` and `AnalyzeGitObject`
4. **git_parents** - Use `SafeOidToHex` and RAII wrappers
5. **git_filesystem** - Use RAII wrappers and hash utilities

**Validation approach for each function:**
```bash
# Before refactoring
./build/debug/duckdb -c "SELECT * FROM git_log('HEAD') LIMIT 5;" > before.txt

# After refactoring  
./build/debug/duckdb -c "SELECT * FROM git_log('HEAD') LIMIT 5;" > after.txt

# Verify identical output
diff before.txt after.txt  # Should be empty
```

## Testing Strategy

### Unit Test Coverage

**Each utility function gets comprehensive tests:**
```cpp
// Hash resolution tests
TEST(GitUtils, ResolveCommitHash);
TEST(GitUtils, GetTreeHashForCommit);
TEST(GitUtils, GetBlobHashForFile);
TEST(GitUtils, ResolveFileHashes);

// URI processing tests  
TEST(GitUtils, ConstructGitUri);
TEST(GitUtils, ExtractFileExtension);
TEST(GitUtils, ParseGitUriComponents);
TEST(GitUtils, ParseAndResolveGitUri);

// Content analysis tests
TEST(GitUtils, GetObjectKind);
TEST(GitUtils, IsTextContent);
TEST(GitUtils, DetectEncoding);
TEST(GitUtils, AnalyzeGitObject);

// RAII tests
TEST(GitUtils, GitRepositoryRAII);
TEST(GitUtils, GitCommitRAII);
TEST(GitUtils, GitTreeRAII);

// Error handling tests
TEST(GitUtils, InvalidRepository);
TEST(GitUtils, InvalidRef);
TEST(GitUtils, InvalidUri);
```

### Integration Testing

**Verify refactored functions produce identical output:**
```bash
# Create test reference data before refactoring
for func in git_log git_tree git_read git_parents; do
    ./build/debug/duckdb -c "SELECT * FROM $func('HEAD') LIMIT 10;" > test_${func}_before.txt
done

# After refactoring, verify identical output
for func in git_log git_tree git_read git_parents; do
    ./build/debug/duckdb -c "SELECT * FROM $func('HEAD') LIMIT 10;" > test_${func}_after.txt
    diff test_${func}_before.txt test_${func}_after.txt
done
```

### Performance Testing

**Verify no performance regression:**
```bash
# Benchmark before refactoring
time ./build/release/duckdb -c "SELECT COUNT(*) FROM git_log('HEAD') WHERE rowid < 1000;"

# Benchmark after refactoring (should be similar or faster due to RAII)
time ./build/release/duckdb -c "SELECT COUNT(*) FROM git_log('HEAD') WHERE rowid < 1000;"
```

## Benefits Analysis

### Code Reduction
- **Estimated 60-70% reduction** in duplicated git operations
- **~500-800 lines removed** from git_functions.cpp
- **Centralized error handling** eliminates inconsistent error messages

### Quality Improvements
- **RAII eliminates resource leaks** - automatic cleanup guaranteed
- **Comprehensive unit testing** of all git operations
- **Consistent error handling** across all functions
- **Better separation of concerns** - git logic vs. table function logic

### Maintainability
- **Single source of truth** for git operations
- **Easier debugging** - centralized logging and error handling
- **Simpler function implementations** - focus on table function logic
- **Reusable utilities** for future git functions

### Preparation for URI Schema Work
- **Hash resolution utilities ready** for new schema columns
- **URI parsing completely handled** by tested utilities  
- **Content analysis ready** for git_tree enhancement
- **Schema changes become trivial** column reordering

## Risk Mitigation

### Regression Prevention
- **Identical output validation** for all existing functions
- **Comprehensive unit tests** before integration
- **Incremental refactoring** one function at a time
- **Performance benchmarking** to catch slowdowns

### Error Handling Improvements
- **Consistent error messages** across all functions
- **Better error context** with operation and repository info
- **Graceful degradation** for invalid inputs
- **Resource leak prevention** with RAII

## Success Criteria

1. ✅ **All existing functions produce identical output** after refactoring
2. ✅ **No performance regression** > 5% for any function  
3. ✅ **100% unit test coverage** for all git_utils functions
4. ✅ **No resource leaks** detected with valgrind
5. ✅ **All existing tests pass** without modification
6. ✅ **Build system updated** to include git_utils module
7. ✅ **Documentation updated** with new utility functions

## Timeline

- **Phase 0.1 (Module Structure):** 30 minutes
- **Phase 0.2 (RAII Wrappers):** 1 hour  
- **Phase 0.3 (Hash Resolution):** 1.5 hours
- **Phase 0.4 (URI Processing):** 1 hour
- **Phase 0.5 (Content Analysis):** 1 hour
- **Phase 0.6 (Refactor Functions):** 1.5 hours

**Total: ~6.5 hours** (can be parallelized to ~4 hours with careful coordination)

## Integration with URI Clarity Plan

Once this refactoring is complete, the URI schema standardization work becomes:

1. **Update schemas** to use pre-tested utilities
2. **Add new columns** by calling existing hash resolution functions  
3. **Reorder columns** to match standard - no complex implementation needed
4. **Test schema changes** - utilities already validated

The complex git operations are handled by tested utilities, making the schema work low-risk column manipulation.

---

*This refactoring creates a solid foundation for all future git operations while significantly improving code quality, testability, and maintainability of the Duck Tails extension.*