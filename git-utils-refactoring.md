# Git Utils Refactoring Plan

## Overview

**⚠️ CURRENT RECOMMENDATION: This refactoring path is NOT recommended. Proceed directly to URI schema work using `uri-clarity.md` instead.**

This document outlines an alternative **incremental, risk-managed** approach to extracting common git operations before URI schema work. After analysis, this approach is more complex than necessary for achieving URI consistency goals.

**Current Status: Alternative path (not recommended)**
**Recommended Path: Direct URI schema work with inline utilities as needed**

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

## Implementation Plan: Incremental & Risk-Managed

### ~~Phase 0: Discovery & Go/No-Go Decision~~ **DECISION MADE: NO-GO**

**DECISION OUTCOME: Refactoring path rejected in favor of direct URI schema work.**

**Analysis Results:**
- 14 different OID conversion implementations found (high complexity)
- Build currently broken with StringVector API issues
- URI schema work can proceed without refactoring
- Risk/reward analysis favors direct implementation

**Selected Path:** Direct URI schema work using `uri-clarity.md` with `refactoring-danger-zones.md` as safety guide.

**This document is retained for reference only - implementation should use `uri-clarity.md`**

### ~~Phase 1-3: All Refactoring Phases~~ **CANCELLED**

**All refactoring phases cancelled in favor of direct URI schema work.**

**Rationale:**
- Analysis revealed 14 different OID conversion patterns (too complex)
- Build is currently broken, need to fix existing issues first
- URI schema work achieves the goal without this complexity
- Ghost bug risks are better managed through the danger zones document

**Implementation Path:** Use `uri-clarity.md` instead of this document.

**Test approach (Catch2 format):**
```cpp
TEST_CASE("GitRepository RAII", "[git_utils]") {
    {
        GitRepository repo(".");
        REQUIRE(repo.is_valid());
        // Repo automatically freed when out of scope
    }
    // Test that repository was properly freed
}
```

**Critical RAII Design Considerations:**

1. **Constructor Error Handling Strategy:**
```cpp
class GitRepository {
private:
    git_repository *repo_;
    bool valid_;
public:
    // NO throwing constructor - use explicit validation
    explicit GitRepository(const string &path) : repo_(nullptr), valid_(false) {
        int error = git_repository_open(&repo_, path.c_str());
        valid_ = (error == 0 && repo_ != nullptr);
        // Don't throw - caller checks is_valid()
    }
    
    bool is_valid() const { return valid_ && repo_ != nullptr; }
};
```

2. **Move Semantics Safety:**
```cpp
// git2 objects may have internal callbacks/state
// Use simpler transfer semantics instead of complex move
GitRepository(GitRepository&& other) noexcept 
    : repo_(other.repo_), valid_(other.valid_) {
    other.repo_ = nullptr;
    other.valid_ = false;
    // This is safe for libgit2 objects
}
```

3. **Thread Safety Considerations:**
```cpp
// Check if libgit2 was initialized with threading support
// Some git operations may need serialization
```

### Phase 0.3: Implement Hash Resolution (1.5 hours)

**Implementation priority:**
1. `SafeOidToHex()` - Replace all existing `oid_to_hex()` calls
2. `ResolveCommitHash()` - Core commit resolution 
3. `GetTreeHashForCommit()` - Extract tree from commit
4. `GetBlobHashForFile()` - File-specific blob lookup
5. `ResolveFileHashes()` - Combined operation

**Testing strategy (Catch2 format):**
```cpp
TEST_CASE("Resolve file hashes", "[git_utils]") {
    GitRepository repo(".");
    REQUIRE(repo.is_valid());  // Fail fast if no git repo
    
    auto hashes = ResolveFileHashes(repo, "HEAD", "README.md");
    
    REQUIRE_FALSE(hashes.commit_hash.empty());
    REQUIRE_FALSE(hashes.tree_hash.empty());  
    REQUIRE_FALSE(hashes.blob_hash.empty());
    
    // Verify hashes are valid SHA format
    REQUIRE(hashes.commit_hash.length() == 40);
    REQUIRE(std::all_of(hashes.commit_hash.begin(), hashes.commit_hash.end(), ::isxdigit));
}
```

**Critical Hash Resolution Implementation Notes:**

1. **Integration with Existing GitPath:**
```cpp
// DON'T duplicate GitPath::Parse - integrate with it
GitObjectHashes ResolveFileHashes(const string &git_uri) {
    auto git_path = GitPath::Parse(git_uri);  // Use existing parsing
    GitRepository repo(git_path.repository_path);
    // ... resolve hashes using git_path components
}
```

2. **Error Handling Strategy:**
```cpp
// Define git error -> DuckDB exception mapping
void ThrowGitError(const string &operation, const string &context = "") {
    const git_error *e = git_error_last();
    string msg = StringUtil::Format("Git operation '%s' failed", operation);
    if (!context.empty()) msg += StringUtil::Format(" (%s)", context);
    if (e) msg += StringUtil::Format(": %s", e->message);
    throw IOException(msg);
}
```

### Phase 0.4: Implement URI Processing (1 hour)

**Implementation priority:**
1. `ExtractFileExtension()` - Replace existing function
2. `ConstructGitUri()` - Enhance existing function
3. `ParseGitUriComponents()` - Complete existing function
4. `ParseAndResolveGitUri()` - New comprehensive function

**Testing strategy (Catch2 format):**
```cpp
TEST_CASE("Parse and resolve git URI", "[git_utils]") {
    auto components = ParseAndResolveGitUri("git://./src/main.cpp@HEAD");
    
    REQUIRE(components.file_ext == ".cpp");
    REQUIRE(components.file_path == "src/main.cpp");
    REQUIRE(components.ref == "HEAD");
    REQUIRE_FALSE(components.commit_hash.empty());
    REQUIRE_FALSE(components.blob_hash.empty());
}
```

**Critical URI Processing Notes:**

1. **GitPath Integration Strategy:**
```cpp
// DON'T replace existing ParseGitUriComponents - enhance it
GitUriComponents ParseAndResolveGitUri(const string &git_uri) {
    GitUriComponents result;
    result.git_uri = git_uri;
    
    // Use existing GitPath parsing
    auto git_path = GitPath::Parse(git_uri);
    result.repo_path = git_path.repository_path;
    result.file_path = git_path.file_path;
    result.ref = git_path.revision;
    
    // Add new functionality: hash resolution
    auto hashes = ResolveFileHashes(git_path.repository_path, git_path.revision, git_path.file_path);
    result.commit_hash = hashes.commit_hash;
    result.tree_hash = hashes.tree_hash;
    result.blob_hash = hashes.blob_hash;
    
    // Add new functionality: file extension
    result.file_ext = ExtractFileExtension(git_path.file_path);
    
    return result;
}
```

### Phase 0.5: Implement Content Analysis (1 hour)

**Implementation priority:**
1. `GetObjectKind()` - Object type identification
2. `IsTextContent()` - Text detection
3. `DetectEncoding()` - Encoding detection  
4. `AnalyzeGitObject()` - Combined analysis

**Testing strategy (Catch2 format):**
```cpp
TEST_CASE("Analyze git object content", "[git_utils]") {
    GitRepository repo(".");
    REQUIRE(repo.is_valid());
    
    auto hashes = ResolveFileHashes(repo, "HEAD", "README.md");
    auto props = AnalyzeGitObject(repo, hashes.blob_hash);
    
    REQUIRE(props.kind == "blob");
    REQUIRE(props.is_text);  // README should be text
    REQUIRE(props.size_bytes > 0);
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

**Each utility function gets comprehensive tests (Catch2 format):**
```cpp
// Hash resolution tests
TEST_CASE("Resolve commit hash", "[git_utils][hash]") { ... }
TEST_CASE("Get tree hash for commit", "[git_utils][hash]") { ... }
TEST_CASE("Get blob hash for file", "[git_utils][hash]") { ... }  
TEST_CASE("Resolve file hashes", "[git_utils][hash]") { ... }

// URI processing tests
TEST_CASE("Construct git URI", "[git_utils][uri]") { ... }
TEST_CASE("Extract file extension", "[git_utils][uri]") { ... }
TEST_CASE("Parse git URI components", "[git_utils][uri]") { ... }
TEST_CASE("Parse and resolve git URI", "[git_utils][uri]") { ... }

// Content analysis tests  
TEST_CASE("Get object kind", "[git_utils][content]") { ... }
TEST_CASE("Is text content", "[git_utils][content]") { ... }
TEST_CASE("Detect encoding", "[git_utils][content]") { ... }
TEST_CASE("Analyze git object", "[git_utils][content]") { ... }

// RAII tests
TEST_CASE("GitRepository RAII", "[git_utils][raii]") { ... }
TEST_CASE("GitCommit RAII", "[git_utils][raii]") { ... }
TEST_CASE("GitTree RAII", "[git_utils][raii]") { ... }

// Error handling tests
TEST_CASE("Invalid repository", "[git_utils][error]") { ... }
TEST_CASE("Invalid ref", "[git_utils][error]") { ... }
TEST_CASE("Invalid URI", "[git_utils][error]") { ... }
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

### Critical Risk Areas Added:

1. **GitPath Integration Conflicts**
   - Risk: Breaking existing URI parsing logic
   - Mitigation: Integrate with GitPath::Parse, don't replace it

2. **Thread Safety Assumptions**
   - Risk: libgit2 operations aren't thread-safe by default
   - Mitigation: Check git_threads_init status, document thread requirements

3. **Constructor Error Handling**
   - Risk: RAII constructors can't return error codes
   - Mitigation: Use explicit validation pattern, not throwing constructors

4. **Move Semantics with git2 Objects**
   - Risk: libgit2 objects may have internal callbacks/state
   - Mitigation: Use simple pointer transfer, document non-copyable semantics

5. **Existing Function Signature Changes**
   - Risk: Subtle behavior changes in refactored functions
   - Mitigation: Byte-for-byte output validation before/after refactor

## Success Criteria

1. ✅ **All existing functions produce identical output** after refactoring
2. ✅ **No performance regression** > 5% for any function  
3. ✅ **100% unit test coverage** for all git_utils functions
4. ✅ **No resource leaks** detected with valgrind
5. ✅ **All existing tests pass** without modification
6. ✅ **Build system updated** to include git_utils module
7. ✅ **Documentation updated** with new utility functions

## Final Decision: Refactoring Path Cancelled

**Selected Option:** Skip refactoring entirely and proceed to URI schema work.

**Time Saved:** 2.5-4.5 hours of complex refactoring work  
**Approach:** Direct implementation using `uri-clarity.md` (6-8 hours) with safety measures from `refactoring-danger-zones.md`

**Key Insight:** URI consistency can be achieved without complex refactoring. The danger zones document provides adequate ghost bug prevention.

## Relationship to URI Clarity Plan

**FINAL DECISION:** No refactoring - proceed directly to URI schema work.

**Implementation Path:** Use `uri-clarity.md` with the following safety measures:
- **`refactoring-danger-zones.md`** prevents ghost bugs during URI schema changes
- **Inline utilities** added as needed during schema work
- **Existing patterns preserved** (use `oid_to_hex()`, `ConstructGitUri()`, etc.)

---

## Document Status: Reference Only

**This document is retained for reference but should NOT be implemented.**

**Active Implementation Document:** `uri-clarity.md`  
**Safety Guide:** `refactoring-danger-zones.md`

*The refactoring path was fully analyzed and determined to be unnecessary for achieving URI consistency goals.*