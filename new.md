# Duck Tails: Comprehensive Implementation Plan
## Complete LATERAL Functions & git:// URI Enhancement

This document outlines the complete implementation plan for adding LATERAL "_each" functions and full git:// URI support with range notation to Duck Tails.

## Current Status Summary

### ✅ **What We Have (IMPLEMENTED)**
- **Git filesystem**: `read_csv('git:///path/to/repo/file.csv@HEAD')` works perfectly
- **Core git table functions**: `git_log()`, `git_branches()`, `git_tags()`, `git_tree()`, `git_parents()`, `git_read()` 
- **LATERAL support**: `git_read_each()` for dynamic file reading
- **Array support**: `git_tree(ARRAY['HEAD', 'HEAD~1'])` and `git_parents(ARRAY['HEAD', 'HEAD~1'])`  
- **Range support in table functions**: `git_tree('HEAD~5..HEAD')` works
- **Repository path discovery**: Relative, absolute, and current directory paths
- **Consistent schemas**: All functions return uniform column structures
- **repo_path context**: All functions show which repository each result comes from

### ❌ **What We Need (MISSING)**

Based on user requirements, here's the complete work needed:

---

## 1. **Complete LATERAL Function Suite** 
*Priority: HIGH - User-requested core functionality*

### Missing LATERAL Functions
We need to add "_each" variants that are **functionally identical** to their non-each counterparts, with the key difference being **in-out function capability** for LATERAL joins:

```sql
-- ✅ ALREADY EXISTS: git_read_each() 
-- Identical to git_read() but supports both static and dynamic parameters
SELECT * FROM git_read_each('git://README.md@HEAD~2..HEAD');           -- Static usage
SELECT l.commit_hash, r.text FROM git_log() l,
    LATERAL git_read_each('git://README.md@' || l.commit_hash) r;       -- LATERAL usage

-- ❌ MISSING: These _each functions need identical functionality + LATERAL support
SELECT * FROM git_tree_each('HEAD~2..HEAD');                           -- Static (same as git_tree)
SELECT l.commit_hash, t.path FROM git_log() l,
    LATERAL git_tree_each('git://.@' || l.commit_hash) t;               -- LATERAL

SELECT * FROM git_log_each('../other-repo');                           -- Static (same as git_log)  
SELECT b.branch_name, l.author_name FROM git_branches() b,
    LATERAL git_log_each('../other-repo-' || b.branch_name) l;         -- LATERAL

SELECT * FROM git_branches_each('/path/to/repo');                      -- Static (same as git_branches)
SELECT t.tag_name, br.branch_name FROM git_tags() t,
    LATERAL git_branches_each('/path/to/repo-' || t.tag_name) br;      -- LATERAL
```

### **Key Principle: _each Functions Are Identical + LATERAL**

All `*_each` functions are **functionally identical** to their non-each counterparts:
- **Same parameters**: Accept identical parameter types and values
- **Same schemas**: Return identical column structures  
- **Same logic**: Use identical processing algorithms
- **Same range support**: Support same range notation where applicable
- **Additional capability**: Can process dynamic parameters from LATERAL input DataChunk

#### **git_tree_each() = git_tree() + LATERAL**
- **Identical Schema**: `{repo_path, commit_hash, commit_date, path, mode, blob_hash, size}`
- **Identical Parameters**: `ref`, `git:// URL`, ranges, named parameters
- **Additional**: LATERAL support for dynamic commit references
- **Usage**: `git_tree_each('HEAD~2..HEAD')` ≡ `git_tree('HEAD~2..HEAD')`

#### **git_log_each() = git_log() + LATERAL** **[MISSING]**  
- **Identical Schema**: `{repo_path, commit_hash, author_name, author_email, author_date, message}`
- **Identical Parameters**: `repo_path`, ranges, named parameters
- **Additional**: LATERAL support for dynamic repository paths
- **Usage**: `git_log_each('../repo')` ≡ `git_log('../repo')`

#### **git_branches_each() = git_branches() + LATERAL** **[ESSENTIAL - Missing]**
- **Identical Schema**: `{repo_path, branch_name, commit_hash, is_current}`
- **Identical Parameters**: `repo_path`, named parameters
- **Additional**: LATERAL support for dynamic repository paths
- **Usage**: `git_branches_each('../repo')` ≡ `git_branches('../repo')`

#### **git_tags_each() = git_tags() + LATERAL** **[ESSENTIAL - Missing]**
- **Identical Schema**: `{repo_path, tag_name, commit_hash, tagger_name, tagger_email, tagger_date, message}`
- **Identical Parameters**: `repo_path`, named parameters
- **Additional**: LATERAL support for dynamic repository paths
- **Usage**: `git_tags_each('../repo')` ≡ `git_tags('../repo')`

---

## 2. **git:// URI Range Notation Support**
*Priority: HIGH - User-requested core functionality*

### Ultra-Analysis: Static vs LATERAL Range Contexts

Range support depends on function type and parameter context:

#### **✅ Static Range Functions (Multi-Row Results)**
```sql  
-- ✅ SHOULD WORK: git_read with static range parameters (temporal multiplicity)
SELECT repo_path, commit_hash, text FROM git_read('git://README.md@HEAD~5..HEAD');
-- Returns: One row per commit in range - same file across commits

-- ✅ ALREADY WORKS: Other static range functions  
SELECT repo_path, commit_hash, path FROM git_tree('v1.0..v2.0');      -- Files across commits
SELECT repo_path, commit_hash, author_name FROM git_log('v1.0..v2.0');  -- Commits in range

-- ✅ SYMMETRY: Core read_ functions support spatial multiplicity (multiple files)
SELECT filename, content FROM read_text('docs/*.md');              -- Multiple files, single commit
SELECT * FROM read_csv('data/*.csv');                              -- Multiple files, single commit
```

#### **✅ LATERAL _each Functions (Dynamic Parameters + Optional Ranges)**
```sql
-- ✅ SHOULD WORK: Dynamic range parameters in LATERAL context
SELECT l.branch_name, r.commit_hash, r.text 
FROM git_branches() l,
     LATERAL git_read_each('git://README.md@' || l.commit_hash || '~5..' || l.commit_hash) r;

-- ✅ SHOULD WORK: All _each functions with ranges
SELECT * FROM git_tree_each('git://repo@main..feature');    -- Files changed across commits  
SELECT * FROM git_log_each('git://repo@v1.0..v2.0');        -- Commits in range
```

#### **❌ Core Filesystem Functions (Schema Ambiguity Across Commits)**
```sql
-- ❌ SHOULD NOT WORK: Different commits may have different CSV schemas
SELECT * FROM read_csv('git://data.csv@HEAD~5..HEAD');      -- Schema may change across commits
SELECT * FROM read_parquet('git://data@v1..v2');            -- Parquet schema evolution

-- ✅ WORKS: Single commit with multiple files (consistent timepoint)
SELECT * FROM read_csv('git://data/*.csv@HEAD');            -- Multiple files, same commit
SELECT * FROM read_csv('git://data.csv@HEAD');              -- Single file, single commit

-- ✅ KEY DISTINCTION: Spatial multiplicity (files) vs temporal multiplicity (commits)
-- Core read_: Multiple files at single commit ✅ (same schema timepoint)
-- git_read: Single file across commits ✅ (file schema consistent over time)
```

### Required Enhancement: GitPath::Parse Range Support

**File: `src/git_filesystem.cpp`**

Enhance `GitPath::Parse` to handle range notation:

```cpp
GitPath GitPath::Parse(const string &git_url) {
    // ... existing parsing logic ...
    
    // NEW: Handle range notation in revision part
    if (revision.find("..") != string::npos) {
        // Parse range: "v1.0..v2.0", "HEAD~5...HEAD", etc.
        result.is_range = true;
        result.range_start = /* extract start */;
        result.range_end = /* extract end */;
        result.is_three_dot = (revision.find("...") != string::npos);
    } else {
        result.is_range = false;
        result.revision = revision;
    }
    
    return result;
}
```

### Expected Results After Implementation
```sql
-- ✅ Static range functions (all return repo_path as first column):
SELECT repo_path, commit_hash, text FROM git_read('git://README.md@HEAD~5..HEAD');
SELECT repo_path, commit_hash, path FROM git_tree_each('git://repo@v1.0..v2.0');

-- ✅ LATERAL range functions (dynamic parameters + ranges):
SELECT l.project, r.repo_path, r.commit_hash, r.text
FROM project_list l,
     LATERAL git_read_each('git://../' || l.project || '/README.md@HEAD~5..HEAD') r;

-- ✅ Multi-repository LATERAL (ESSENTIAL use cases):
SELECT r.repo_name, b.repo_path, b.branch_name 
FROM repo_list r,
     LATERAL git_branches_each('git://../' || r.repo_name) b;

-- ❌ Core filesystem functions (schema ambiguity across commits):  
-- SELECT * FROM read_csv('git://data.csv@v1.0..v2.0');        -- Different schemas?
```

---

## 3. **git_uri() Helper Function & Enhanced Schemas**
*Priority: HIGH - Seamless URI construction and file linking*

### git_uri() Helper Function

Add a helper function for clean URI construction to eliminate string concatenation errors:

```sql
-- Function signature
git_uri(repo_path, file_path, commit_ref) → VARCHAR

-- Usage examples  
SELECT git_uri('.', 'README.md', 'HEAD');           -- Returns 'git://./README.md@HEAD'
SELECT git_uri('/abs/path', 'src/main.cpp', 'v1.0'); -- Returns 'git:///abs/path/src/main.cpp@v1.0'
SELECT git_uri('../other', 'config.json', 'abc123'); -- Returns 'git://../other/config.json@abc123'

-- Clean LATERAL usage (no string concatenation pitfalls)
SELECT l.commit_hash, r.text
FROM git_log() l,
LATERAL git_read_each(git_uri(l.repo_path, 'README.md', l.commit_hash)) r;
```

### Enhanced git_tree Schema with git_file_uri

Update `git_tree()` (and `git_tree_each()`) to include ready-to-use URIs:

```sql
-- Current schema: commit_hash, commit_date, path, mode, blob_hash, size  
-- Enhanced schema: commit_hash, commit_date, path, mode, blob_hash, size, git_file_uri

-- Example enhanced results
SELECT commit_hash, path, git_file_uri FROM git_tree('HEAD');
/*
commit_hash | path        | git_file_uri
414d8f0...  | README.md   | git://./README.md@414d8f0...
414d8f0...  | src/main.cpp| git://./src/main.cpp@414d8f0...
*/
```

**Implementation**: git_tree uses git_uri() helper internally:
```cpp
// In git_tree processing
git_file_uri = git_uri(bind_data.repo_path, path, commit_hash)
```

### Usage Patterns Enabled

```sql
-- ✅ Direct file reading from git_tree (zero concatenation!)
SELECT t.path, r.text 
FROM git_tree('HEAD') t,
LATERAL git_read_each(t.git_file_uri) r;

-- ✅ git_tree_each inherits git_file_uri automatically (DRY!)  
SELECT l.commit_hash, t.git_file_uri, r.text
FROM git_log() l,
LATERAL git_tree_each(l.commit_hash) t,
LATERAL git_read_each(t.git_file_uri) r;

-- ✅ Manual construction using helper function
SELECT l.commit_hash, r.text
FROM git_log() l,
LATERAL git_read_each(git_uri(l.repo_path, 'config.json', l.commit_hash)) r;
```

**Key Benefits:**
- **Zero concatenation errors**: Helper function handles proper URI format
- **DRY implementation**: git_tree uses same git_uri() helper internally  
- **Clean user experience**: Ready-to-use git_file_uri in git_tree results
- **Consistent ecosystem**: Seamless links between table functions and file readers

---

## 4. **git:// URLs as Table Function Parameters**
*Priority: HIGH - Existing gap from previous analysis*

### Current Problem
Table functions don't recognize git:// URLs as special parameters:

```sql
-- ❌ FAILS: Treats git:// URL as literal commit reference
SELECT * FROM git_tree('git:///path/to/repo@HEAD');
SELECT * FROM git_log('git:///path/to/repo@main');

-- ✅ WORKS: Filesystem functions support git:// URLs
SELECT * FROM read_csv('git:///path/to/repo/data.csv@HEAD');
```

### Implementation Required
Add git:// URL detection to all table function bind methods:

**Files to modify: `src/git_functions.cpp`**
- `GitTreeBind()` - Add git:// URL parsing
- `GitLogBind()` - Add git:// URL parsing  
- `GitParentsBind()` - Add git:// URL parsing
- `GitBranchesBind()` - Add git:// URL parsing
- `GitTagsBind()` - Add git:// URL parsing

---

## Complete Implementation Architecture

### Phase 1: Foundation (Week 1)
**Priority: ESSENTIAL**

#### **1.1 Enhance GitPath::Parse for Range Support**
```cpp
// Add to GitPath struct
struct GitPath {
    string repository_path;
    string file_path;
    string revision;
    bool is_range;           // NEW: true if revision contains range
    string range_start;      // NEW: start of range (v1.0)  
    string range_end;        // NEW: end of range (v2.0)
    bool is_three_dot;       // NEW: true for ... vs .. syntax
};
```

#### **1.2 Add git:// URL Parsing to Existing Table Functions**
Modify all bind functions to detect and parse git:// URLs:

```cpp
// Pattern for all bind functions  
if (StringUtil::StartsWith(param, "git://")) {
    auto git_path = GitPath::Parse(param);
    // Use git_path.repository_path and git_path.revision/range
} else {
    // Existing parameter handling
}
```

### Phase 2: LATERAL Functions (Week 2)
**Priority: USER-REQUESTED CORE FUNCTIONALITY**

#### **2.1 Implement git_tree_each**
```cpp
// New bind function
static unique_ptr<FunctionData> GitTreeEachBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
    // CRITICAL: Same schema as git_tree with repo_path as first column
    names = {"repo_path", "commit_hash", "commit_date", "path", "mode", "blob_hash", "size"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP, 
                   LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT};
    return make_uniq<GitTreeFunctionData>("", ".");  // Dynamic parameters
}

// New in_out_function
static OperatorResultType GitTreeEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                             DataChunk &input, DataChunk &output) {
    // Pattern: Extract commit reference from input DataChunk
    // Process using existing git_tree logic
    // Return results matching git_tree schema
}

// Registration
TableFunction git_tree_each_1({LogicalType::VARCHAR}, nullptr, GitTreeEachBind, nullptr, GitTreeLocalInit);
git_tree_each_1.in_out_function = GitTreeEachFunction;
```

#### **2.2 Implement git_log_each, git_branches_each, git_tags_each**
**DRY PRINCIPLE**: _each functions **share identical code paths** with their non-each counterparts:

**DRY Implementation Pattern:**
```cpp
// Step 1: Extract shared core logic (DRY)
static void ProcessGitLogCore(const string &repo_path, DataChunk &output, 
                             GitLogFunctionData &bind_data) {
    // SHARED: Core git log processing logic used by both variants
    // This is the SAME code path for git_log() and git_log_each()
}

// Step 2: Non-each function uses shared core
void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitLogFunctionData>();
    ProcessGitLogCore(bind_data.repo_path, output, bind_data);  // Shared code path
}

// Step 3: _each function uses SAME shared core  
static OperatorResultType GitLogEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                           DataChunk &input, DataChunk &output) {
    // Extract parameter from input DataChunk OR use static parameter
    string repo_path = ExtractDynamicParameter(input, data_p);
    auto &bind_data = data_p.bind_data->Cast<GitLogFunctionData>();
    
    ProcessGitLogCore(repo_path, output, bind_data);  // SAME shared code path
    return OperatorResultType::NEED_MORE_INPUT;
}

// Step 4: Identical bind functions (could be shared too)
// Both use SAME bind logic, schemas, validation
```

**DRY Benefits:**
- **Single code path** for core processing logic
- **Shared bug fixes** and improvements
- **Consistent behavior** guaranteed between variants
- **Reduced maintenance** burden

### Phase 3: Integration & Testing (Week 3)
**Priority: VALIDATION - TDD APPROACH**

#### **3.1 Test-Driven Development (TDD)**
**CRITICAL**: Write tests **FIRST** before implementation, following existing repo conventions.

**Test File Naming Convention** (matches existing pattern):
```
test/sql/git_tree_each_comprehensive.test          -- Comprehensive _each function tests
test/sql/git_log_each_comprehensive.test           -- Following git_read_each_comprehensive.test
test/sql/git_branches_each_comprehensive.test      -- Same pattern as other comprehensive tests
test/sql/git_tags_each_comprehensive.test          -- Consistent naming

test/sql/git_uri_ranges.test                       -- Range notation in git:// URIs
test/sql/git_uri_table_functions.test              -- git:// URLs in table functions  
```

**Test Structure** (following repo convention):
```sql
# name: test/sql/git_tree_each_comprehensive.test
# description: Test git_tree_each function with static and LATERAL usage
# group: [duck_tails]

require duck_tails

# Test 1: Static usage (identical to git_tree)
query II
SELECT COUNT(*) > 0 as has_files, COUNT(DISTINCT commit_hash) > 1 as has_multiple_commits
FROM git_tree_each('HEAD~2..HEAD');
----
true	true

# Test 2: LATERAL usage with dynamic parameters  
query TTI
WITH test_commits AS (SELECT commit_hash FROM git_log() LIMIT 2)
SELECT c.commit_hash, t.path, COUNT(*) as file_count 
FROM test_commits c, LATERAL git_tree_each('git://.@' || c.commit_hash) t
GROUP BY c.commit_hash, t.path
ORDER BY c.commit_hash, t.path
LIMIT 5;
----
[expected results following existing test pattern]

# Test 3: Error handling (following repo error test pattern)
statement error
SELECT * FROM git_tree_each('invalid-ref')
----
[expected error message]
```

**Test Coverage Requirements:**
1. **Static usage** (identical behavior to non-each)
2. **LATERAL usage** with dynamic parameters
3. **Range support** where applicable  
4. **git:// URL support** in all contexts
5. **Error handling** for invalid parameters
6. **Schema validation** (exact column match with non-each)
7. **Multi-repository** scenarios
8. **Performance** with large datasets

#### **3.2 Integration Examples**
```sql
-- Complex multi-repository analysis
SELECT 
    r.repo_name,
    t.tag_name,
    l.commit_count,
    tr.file_count
FROM repository_list r,
     LATERAL git_tags_each('git://../' || r.repo_name || '@HEAD') t,
     LATERAL (SELECT COUNT(*) as commit_count FROM git_log_each('git://../' || r.repo_name)) l,
     LATERAL (SELECT COUNT(*) as file_count FROM git_tree_each('git://../' || r.repo_name || '@' || t.tag_name)) tr;

-- Range-based analysis with git:// URIs  
SELECT 
    path,
    COUNT(*) as change_frequency
FROM git_tree_each('git://large-repo@v1.0..v2.0')
GROUP BY path
ORDER BY change_frequency DESC;
```

---

## Complete Function Matrix

### After Implementation

| **Function Type** | **Regular** | **LATERAL (_each)** | **git:// URL Support** | **Range Support** | **repo_path Column** |
|-------------------|-------------|-------------------|----------------------|------------------|---------------------|
| **Git File Reading** | `git_read()` | ✅ `git_read_each()` | ⚠️ Both | ⚠️ Both (temporal multiplicity) | ✅ Always first column |
| **Tree Analysis** | `git_tree()` | ⚠️ `git_tree_each()` | ⚠️ Both | ✅ Both (multi-commit) | ✅ Always first column |
| **Commit History** | `git_log()` | ⚠️ `git_log_each()` | ⚠️ Both | ✅ Both (commit ranges) | ✅ Always first column |
| **Branch Info** | `git_branches()` | ⚠️ `git_branches_each()` | ⚠️ Both | ❌ N/A (current state) | ✅ Always first column |
| **Tag Info** | `git_tags()` | ⚠️ `git_tags_each()` | ⚠️ Both | ❌ N/A (current state) | ✅ Always first column |
| **Parent Info** | `git_parents()` | N/A | ⚠️ Regular only | ✅ Regular only | ✅ Always first column |
| **Core Filesystem** | `read_csv()` etc. | N/A | ✅ Single commit + globbing | ❌ No temporal ranges | ❌ No repo context |

**Legend:**
- ✅ **Implemented and working**
- ⚠️ **Needs implementation** (this project's scope)
- N/A **Not applicable/needed**

---

## Success Criteria & Validation

### **Complete API Consistency**
After implementation, all these patterns will work consistently:

```sql
-- ✅ Current repository (simple)
SELECT * FROM git_tree('HEAD');
SELECT * FROM git_log();

-- ✅ Other repositories (git:// URLs)  
SELECT * FROM git_tree('git:///path/to/repo@HEAD');
SELECT * FROM git_log('git:///path/to/repo@main');

-- ✅ Range notation (temporal multiplicity)
SELECT * FROM git_read('git://README.md@v1.0..v2.0');           -- File history
SELECT * FROM git_tree('v1.0..v2.0');                           -- Files across commits
SELECT * FROM git_log('HEAD~10..HEAD');                         -- Commit ranges
SELECT * FROM git_tree_each('git://repo@main..feature');        -- LATERAL + git:// + range

-- ✅ Dynamic LATERAL analysis  
SELECT l.commit_hash, t.path, t.size FROM git_log() l,
     LATERAL git_tree_each('git://other-repo@' || l.commit_hash) t;

-- ✅ Multi-repository range analysis
SELECT r.name, COUNT(*) as changed_files FROM repos r,
     LATERAL git_tree_each('git://../' || r.name || '@v1.0..v2.0') t
GROUP BY r.name;
```

### **Advanced Use Cases Enabled**

1. **Cross-Repository Analysis**: Compare file evolution across related repositories
2. **Temporal File Analysis**: Track single file changes across commit ranges with `git_read()`
3. **Dynamic Range Queries**: Query ranges built from other query results  
4. **Multi-Project Analytics**: Aggregate metrics across project portfolios
5. **Version Comparison Workflows**: Automated analysis between release ranges
6. **Spatial vs Temporal Multiplicity**: Core `read_*` for multiple files, `git_*` for temporal analysis

---

## Implementation Timeline

### **Week 1: Foundation + TDD Setup** 
- **Day 1-2**: Write comprehensive test suite first (TDD approach)
- **Day 3-4**: Range notation in GitPath::Parse
- **Day 4-5**: git:// URL parsing in all table function bind methods
- **Day 6-7**: Validate foundation tests pass

### **Week 2: LATERAL Functions (DRY Implementation)**
- **Day 1-2**: Extract shared core logic from existing functions (DRY refactoring)
- **Day 3-4**: Implement git_tree_each, git_log_each using shared code paths
- **Day 5-6**: Implement git_branches_each, git_tags_each using shared code paths
- **Day 7**: LATERAL functionality testing and validation

### **Week 3: Integration & Validation**
- **Day 1-2**: End-to-end integration testing
- **Day 3-4**: Performance testing with large datasets  
- **Day 5-6**: Advanced use case validation
- **Day 7**: Documentation updates and final validation

**TDD Benefits:**
- **Tests guide implementation** - know exactly what to build
- **Regression protection** - ensures _each functions match non-each behavior
- **Documentation** - tests serve as executable specification  
- **Quality assurance** - comprehensive coverage from day one

---

## Conclusion

This implementation plan completes Duck Tails' evolution into a **comprehensive git-aware data analysis platform** with:

1. **Complete LATERAL Support**: Dynamic analysis capabilities across all git operations
2. **Universal git:// URI Support**: Consistent repository access patterns everywhere  
3. **Range Notation Everywhere**: Two-dot and three-dot range syntax in all contexts
4. **Multi-Repository Analytics**: Seamless cross-project analysis capabilities

The result will be a mature, feature-complete extension that enables sophisticated git-aware data analysis workflows previously impossible in SQL.