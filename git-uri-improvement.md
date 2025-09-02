# Git URI Improvement: Comprehensive Enhancement

## Overview

This document outlines comprehensive enhancements to git:// URI support in duck_tails, including:
1. **Absolute repository paths** in git:// filesystem URLs
2. **git:// URLs as table function parameters** (git_tree, git_log, etc.)  
3. **Smart commit reference resolution** with automatic tag/branch detection
4. **Path filtering** within git_tree() for subdirectory analysis

These improvements make git:// URIs consistent with DuckDB's filesystem patterns while adding powerful new capabilities.

## Current Behavior

### What Works Today
```sql
-- Relative paths work when run from within a git repository
SELECT * FROM read_csv('git://data.csv@HEAD');
SELECT * FROM git_read_each('git://README.md@HEAD');

-- Table functions support explicit repository paths
SELECT * FROM git_log('/path/to/repo');
SELECT * FROM git_tree('HEAD', '/path/to/repo');
SELECT * FROM git_tree('HEAD', repo_path := '/path/to/repo');
```

### Current Limitations
```sql
-- ❌ No way to specify repository path in git:// URLs
SELECT * FROM read_csv('git:///path/to/repo/data.csv@HEAD');  -- Doesn't work
SELECT * FROM git_read_each('git:///path/to/repo/README.md@HEAD');  -- Doesn't work

-- Current git:// URLs are hardcoded to use current directory (".")
```

### Current Implementation
- **GitPath::Parse()** hardcodes `repository_path = "."` 
- All git:// URLs assume current working directory is the git repository
- Inconsistent with table functions that accept explicit repo paths

## Proposed Enhancements

### Enhancement 1: Absolute Paths in git:// URLs
```sql
-- Relative paths (unchanged behavior)
SELECT * FROM read_csv('git://data.csv@HEAD');          -- Uses current directory
SELECT * FROM read_csv('git://subdir/file.csv@HEAD');   -- Uses current directory

-- Absolute paths (NEW capability)
SELECT * FROM read_csv('git:///path/to/repo/data.csv@HEAD');        -- Uses /path/to/repo
SELECT * FROM git_read_each('git:///home/user/project/README.md@HEAD'); -- Uses /home/user/project
```

### Enhancement 2: git:// URLs as Table Function Parameters
```sql
-- Traditional parameter style (current)
SELECT * FROM git_tree('HEAD', '/path/to/repo');
SELECT * FROM git_log('/path/to/repo');

-- git:// URL style (NEW capability)
SELECT * FROM git_tree('git:///path/to/repo@HEAD');
SELECT * FROM git_tree('git:///path/to/repo@v12.41.11');
SELECT * FROM git_log('git:///path/to/repo@main');
SELECT * FROM git_parents('git:///path/to/repo@feature-branch');
```

### Enhancement 3: Smart Commit Reference Resolution
```sql
-- Automatic tag/branch resolution (NEW capability)
SELECT * FROM git_tree('git://./repo@12.41.11');      -- Tries: 12.41.11, v12.41.11, release/12.41.11
SELECT * FROM git_tree('git://./repo@main');          -- Tries: main, origin/main, refs/heads/main
SELECT * FROM git_tree('git://./repo@abc123');        -- Tries: abc123*, prefix matching
```

### Enhancement 4: Path Filtering in git_tree()
```sql
-- Full repository tree (current behavior)
SELECT * FROM git_tree('git://./repo@HEAD');

-- Filtered to specific directory (NEW capability)  
SELECT * FROM git_tree('git://./repo/src/controllers@HEAD');  -- Only src/controllers/*
SELECT * FROM git_tree('git://./repo/docs@HEAD');             -- Only docs/*

-- Single file analysis (NEW capability)
SELECT * FROM git_tree('git://./repo/package.json@HEAD');     -- Just package.json
```

### Consistency with DuckDB Patterns

This follows the exact same pattern as DuckDB's core filesystem URLs:

```sql
-- Local filesystem
SELECT * FROM read_csv('/tmp/data.csv');           -- Absolute path
SELECT * FROM read_csv('file:///tmp/data.csv');    -- file:// URL

-- Proposed git filesystem (matches the pattern)
SELECT * FROM read_csv('git:///path/to/repo/data.csv@HEAD');  -- git:// URL with absolute path
```

## Implementation Details

### Enhancement 1: GitPath::Parse() Changes for Absolute Paths

**Current Implementation:**
```cpp
// File: src/git_filesystem.cpp, lines 31-34
result.repository_path = ".";              // Always current directory
result.file_path = url;                     // Entire URL is file path
```

**Enhanced Implementation:**
```cpp
if (url.empty()) {
    result.repository_path = ".";
    result.file_path = "";
} else if (url[0] == '/') {
    // Absolute path: split into repository and file components
    size_t last_slash = url.find_last_of('/');
    if (last_slash == 0) {
        // Root directory case: "/file.txt"
        result.repository_path = "/";
        result.file_path = url.substr(1);
    } else {
        // Normal case: "/path/to/repo/file.txt"
        result.repository_path = url.substr(0, last_slash);
        result.file_path = url.substr(last_slash + 1);
    }
} else {
    // Relative path (unchanged behavior)
    result.repository_path = ".";
    result.file_path = url;
}
```

### Enhancement 2: Table Function git:// URL Parameter Support

**New function signature detection:**
```cpp
// In git_tree bind function
if (input.inputs[0].type().id() == LogicalTypeId::VARCHAR) {
    string param = input.inputs[0].GetValue<string>();
    
    if (StringUtil::StartsWith(param, "git://")) {
        // Parse git:// URL for repo + commit + path info
        auto git_path = GitPath::Parse(param);
        return make_uniq<GitTreeFunctionData>(
            git_path.revision,           // commit_ref
            git_path.repository_path,    // repo_path  
            git_path.file_path          // path_filter (new!)
        );
    } else {
        // Traditional commit string parameter
        return make_uniq<GitTreeFunctionData>(param, ".");
    }
}
```

### Enhancement 3: Smart Commit Reference Resolution

**Intelligent commit resolution:**
```cpp
string ResolveCommitRef(git_repository* repo, const string& ref) {
    // Build candidate list based on reference pattern
    vector<string> candidates;
    
    // Always try exact match first
    candidates.push_back(ref);
    
    // Version-like patterns (12.41.11)
    if (std::regex_match(ref, std::regex(R"(\d+\.\d+\.\d+)"))) {
        candidates.push_back("v" + ref);
        candidates.push_back("release/" + ref);
        candidates.push_back("releases/v" + ref);
    }
    
    // Branch-like patterns (main, feature-auth)  
    if (!ref.empty() && ref.find('.') == string::npos) {
        candidates.push_back("origin/" + ref);
        candidates.push_back("refs/heads/" + ref);
    }
    
    // SHA-like patterns (abc123)
    if (std::regex_match(ref, std::regex(R"([0-9a-f]{6,40})"))) {
        // Try prefix matching for partial SHAs
        candidates.push_back(ref + "*");
    }
    
    // Try each candidate
    for (const auto& candidate : candidates) {
        git_object* obj = nullptr;
        if (git_revparse_single(&obj, repo, candidate.c_str()) == 0) {
            git_object_free(obj);
            return candidate;
        }
    }
    
    throw IOException("Could not resolve commit reference: %s", ref);
}
```

### Enhancement 4: Path Filtering in git_tree()

**Filter tree results by path:**
```cpp
void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &data = (GitTreeFunctionData &)*data_p.bind_data;
    
    // ... existing tree walking code ...
    
    // NEW: Apply path filter if specified
    if (!data.path_filter.empty()) {
        // Filter entries to those matching path prefix
        for (auto it = tree_entries.begin(); it != tree_entries.end();) {
            if (!StringUtil::StartsWith(it->path, data.path_filter)) {
                it = tree_entries.erase(it);
            } else {
                ++it;
            }
        }
        
        // For exact file matches, ensure we found something
        if (tree_entries.empty()) {
            // Try exact file match
            auto file_entry = FindExactFile(tree, data.path_filter);
            if (file_entry) {
                tree_entries.push_back(*file_entry);
            }
        }
    }
    
    // ... existing output generation ...
}
```

### Path Parsing Examples

#### Filesystem URLs (Enhancement 1)
| Git URI | Repository Path | File Path |
|---------|----------------|-----------|
| `git://README.md@HEAD` | `.` | `README.md` |
| `git://docs/guide.md@HEAD` | `.` | `docs/guide.md` |
| `git:///home/user/repo/file.txt@HEAD` | `/home/user/repo` | `file.txt` |
| `git:///projects/web/src/main.js@HEAD` | `/projects/web/src` | `main.js` |
| `git:///repo/data.csv@HEAD` | `/repo` | `data.csv` |

#### Table Function URLs (Enhancements 2-4)
| Git URI | Repository Path | Commit Ref | Path Filter |
|---------|----------------|------------|-------------|
| `git://./repo@HEAD` | `./repo` | `HEAD` | `` (all files) |
| `git:///path/to/repo@v12.41.11` | `/path/to/repo` | `v12.41.11` → resolved | `` (all files) |
| `git://./repo/src@main` | `./repo` | `main` → resolved | `src` (filter) |
| `git:///project/docs/api@12.41.11` | `/project` | `12.41.11` → `v12.41.11` | `docs/api` (filter) |
| `git://./repo/package.json@HEAD` | `./repo` | `HEAD` | `package.json` (single file) |

#### Smart Resolution Examples (Enhancement 3)
| Input Ref | Resolution Attempts | Final Result |
|-----------|-------------------|--------------|
| `12.41.11` | `12.41.11` → `v12.41.11` → `release/12.41.11` | `v12.41.11` (if tag exists) |
| `main` | `main` → `origin/main` → `refs/heads/main` | `main` (if local branch) |
| `abc123` | `abc123` → `abc123*` (prefix match) | `abc123def...` (full SHA) |
| `feature-auth` | `feature-auth` → `origin/feature-auth` | `origin/feature-auth` |

## Breaking Change Analysis

### Risk Assessment: **MINIMAL**

### What Could Break

1. **Files with leading slash in name (extremely rare):**
   ```sql
   -- If someone has a file literally named "/data.csv" in repo root
   -- Current: git:///data.csv@HEAD treats "/data.csv" as file path
   -- New:     git:///data.csv@HEAD treats "/" as repo path, "data.csv" as file
   ```
   **Mitigation:** URL-encode the slash: `git://%2Fdata.csv@HEAD`

2. **Error messages change:**
   ```sql
   -- Current: repository_path="." in error messages  
   -- New:     repository_path="/actual/path" in error messages
   ```
   **Impact:** This is actually an improvement for debugging

### What Won't Break

✅ **Relative paths unchanged:** `git://file.csv@HEAD` works exactly the same  
✅ **Current working directory behavior:** No change when using relative paths  
✅ **Table function APIs:** All `git_log()`, `git_tree()`, etc. unchanged  
✅ **Existing queries:** 99.9% of current usage continues working  

### Migration Path

For the unlikely edge case of files named with leading slash:
```sql
-- Old (if file named "/data.csv"):
SELECT * FROM read_csv('git:///data.csv@HEAD');

-- New (URL-encoded slash):  
SELECT * FROM read_csv('git://%2Fdata.csv@HEAD');
```

## Multi-Repository Analysis Impact

### Current Challenge
When working with multiple repositories, there's a mismatch between table function results and git:// URL construction:

```sql
-- This works but paths are relative to each repo root
SELECT 'repo1' as repo, path FROM git_tree('HEAD', '/path/to/repo1')
UNION ALL 
SELECT 'repo2' as repo, path FROM git_tree('HEAD', '/path/to/repo2');
-- Returns: path = "src/main.cpp" (relative to each repo)

-- But to read these files via git://, we need absolute paths:
-- git:///path/to/repo1/src/main.cpp@HEAD
-- git:///path/to/repo2/src/main.cpp@HEAD
```

### Two Solution Approaches

#### Option A: Enhanced Table Function Results (BREAKING CHANGE)
Add `repository_path` column to table function results:
```sql
-- Modified git_tree() would return:
SELECT repository_path, path, size FROM git_tree('HEAD', '/path/to/repo');
-- Returns: repository_path="/path/to/repo", path="src/main.cpp"

-- Enables clean multi-repo LATERAL joins:
WITH repos AS (
    SELECT '/path/to/repo1' as repo_path UNION ALL
    SELECT '/path/to/repo2' as repo_path
)
SELECT r.repo_path, t.path, f.text
FROM repos r,
     git_tree('HEAD', r.repo_path) t,
     LATERAL git_read_each('git://' || r.repo_path || '/' || t.path || '@HEAD') f;
```

#### Option B: Keep Current API, Use Path Construction (NON-BREAKING)
Users manually construct full paths:
```sql
WITH repos(name, path) AS (
    VALUES ('repo1', '/path/to/repo1'), ('repo2', '/path/to/repo2')
)
SELECT r.name, t.path, f.text
FROM repos r,
     git_tree('HEAD', r.path) t,
     LATERAL git_read_each('git://' || r.path || '/' || t.path || '@HEAD') f;
```

### Recommendation: **Option B (Non-Breaking)**
- Users can already construct paths manually
- Absolute git:// URLs enable the missing piece
- No breaking changes to existing table function schemas
- Clean, explicit path construction

## Benefits

### 1. **Intuitive Syntax**
Users expect absolute paths to work in URLs - this matches universal filesystem conventions.

### 2. **Multi-Repository Analysis**
```sql
-- Simple cross-repo file comparison (Enhancement 1)
WITH repo_data AS (
    SELECT 'main' as repo, * FROM read_csv('git:///path/to/main-repo/metrics.csv@HEAD')
    UNION ALL
    SELECT 'backup' as repo, * FROM read_csv('git:///path/to/backup-repo/metrics.csv@HEAD')
)
SELECT repo, COUNT(*) FROM repo_data GROUP BY repo;

-- Cross-repo structure analysis with git:// URLs (Enhancement 2)
SELECT 
    'frontend' as repo,
    COUNT(*) as file_count,
    SUM(size) as total_size
FROM git_tree('git:///projects/frontend@HEAD')
UNION ALL
SELECT 
    'backend' as repo,
    COUNT(*) as file_count, 
    SUM(size) as total_size
FROM git_tree('git:///projects/backend@HEAD');
```

### 3. **Version Analysis with Smart Resolution**
```sql
-- Compare API changes across versions (Enhancement 3)
SELECT 
    version,
    COUNT(*) as api_endpoint_count,
    AVG(size) as avg_file_size
FROM (
    SELECT 'v12.41.11' as version, * FROM git_tree('git://./repo/api@12.41.11')  -- Auto-resolves to tag
    UNION ALL
    SELECT 'v12.40.0' as version, * FROM git_tree('git://./repo/api@12.40.0')   -- Auto-resolves to tag  
    UNION ALL
    SELECT 'main' as version, * FROM git_tree('git://./repo/api@main')          -- Auto-resolves branch
) GROUP BY version;
```

### 4. **Focused Directory Analysis**
```sql
-- Analyze specific subdirectories without full repo scan (Enhancement 4)
SELECT 
    dir,
    COUNT(*) as file_count,
    SUM(size) as dir_size
FROM (
    SELECT 'controllers' as dir, * FROM git_tree('git://./app/src/controllers@HEAD')
    UNION ALL
    SELECT 'models' as dir, * FROM git_tree('git://./app/src/models@HEAD') 
    UNION ALL
    SELECT 'views' as dir, * FROM git_tree('git://./app/src/views@HEAD')
) GROUP BY dir;

-- Single file tracking across commits
SELECT commit_hash, commit_date, size, blob_hash
FROM git_tree('git://./repo/package.json@HEAD~10..HEAD')  -- Range support
ORDER BY commit_date DESC;
```

### 5. **LATERAL Join Enhancement**
```sql
-- Enhanced LATERAL joins with absolute paths (Enhancement 1)
WITH repos(name, path) AS (
    VALUES 
        ('frontend', '/projects/frontend'),
        ('backend', '/projects/backend'),
        ('mobile', '/projects/mobile')
)
SELECT r.name, f.text 
FROM repos r, 
     LATERAL git_read_each('git://' || r.path || '/package.json@HEAD') f;
```

### 6. **Consistency**
All DuckDB filesystem functions now work the same way:
- `read_csv('file:///path/to/file.csv')`
- `read_csv('s3://bucket/file.csv')`  
- `read_csv('git:///path/to/repo/file.csv@HEAD')` ← NEW

All git table functions now support git:// URLs:
- `git_tree('git:///path/to/repo@commit')`
- `git_log('git:///path/to/repo@branch')`
- `git_parents('git:///path/to/repo@tag')` ← NEW

## Testing Strategy

### Unit Tests
- ✅ Relative paths continue working
- ✅ Absolute paths parse correctly
- ✅ Edge cases (root directory, empty paths)
- ✅ URL encoding edge cases

### Integration Tests
```sql
-- Test absolute path functionality
SELECT * FROM read_csv('git:///tmp/test-repo/data.csv@HEAD');

-- Test relative path backward compatibility  
SELECT * FROM read_csv('git://data.csv@HEAD');

-- Test with LATERAL joins
WITH repos(path) AS (VALUES ('/tmp/repo1'), ('/tmp/repo2'))
SELECT r.path, f.content 
FROM repos r, LATERAL git_read_each('git://' || r.path || '/README.md@HEAD') f;
```

### Performance Testing
- Verify no performance regression for relative paths
- Test absolute path resolution performance

## Implementation Timeline

This comprehensive enhancement can be implemented in phases:

### Phase 1: Absolute Path Support (2-3 hours)
- **Enhancement 1**: Modify `GitPath::Parse()` in `src/git_filesystem.cpp`
- Update path parsing logic for absolute paths
- Add unit tests for filesystem URL parsing
- Test with read_csv, git_read_each

### Phase 2: Table Function git:// URL Support (4-5 hours)
- **Enhancement 2**: Update git_tree, git_log, git_parents bind functions
- Add git:// URL parameter detection and parsing
- Modify function data structures to handle parsed URLs
- Add unit tests for table function URL parameters

### Phase 3: Smart Commit Resolution (2-3 hours)
- **Enhancement 3**: Implement ResolveCommitRef() function
- Add regex patterns for version/branch/SHA detection  
- Integrate with table function parameter parsing
- Add comprehensive resolution tests

### Phase 4: Path Filtering (3-4 hours)
- **Enhancement 4**: Add path filtering to git_tree function
- Implement directory and single-file filtering logic
- Handle edge cases (non-existent paths, empty results)
- Add filtering tests

### Phase 5: Integration Testing (2 hours)
- Test all enhancements working together
- Verify backward compatibility across all functions
- Test complex LATERAL join scenarios
- Performance testing

### Phase 6: Documentation (1 hour)
- Update README.md examples
- Add advanced examples to docs/
- Update function documentation

**Total effort:** ~14-18 hours of development time

**Phased deployment:** Each enhancement can be shipped independently, allowing incremental rollout.

## Deployment Considerations

### Branch Independence
This change can be implemented on:
- ✅ Current `feature/add-git-tree-and-parents-functions` branch
- ✅ Separate feature branch  
- ✅ Main branch
- ✅ Any existing branch

### Rollout Strategy
1. **Soft launch:** Include in current LATERAL feature branch
2. **Documentation:** Update examples to show new capability
3. **User communication:** Highlight as enhancement, not breaking change

## Conclusion

These comprehensive enhancements transform git:// URI support in duck_tails from a basic filesystem feature into a powerful, intuitive interface for git repository analysis. The improvements include:

✅ **Absolute path support** - matching DuckDB filesystem conventions  
✅ **git:// URLs as table function parameters** - unified interface design  
✅ **Smart commit reference resolution** - eliminates manual tag/branch lookups  
✅ **Path filtering in git_tree()** - focused analysis without full repo scans  

**Key Benefits:**
- **Intuitive**: Users can leverage existing filesystem URL knowledge
- **Powerful**: Enables sophisticated multi-repository and version analysis  
- **Ergonomic**: Reduces boilerplate and manual path construction
- **Consistent**: Unifies git functionality under a single URL scheme

**Risk Assessment:** Minimal breaking changes with substantial user experience improvements.

**Implementation Strategy:** Phased rollout allows incremental delivery while maintaining stability.

**Recommendation:** Implement these enhancements to establish duck_tails as the definitive SQL interface for git repository analysis, providing capabilities that go far beyond traditional git tooling.