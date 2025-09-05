# Claude URI Design: Pragmatic Git URIs for Duck Tails (v2)

## Executive Summary

This design standardizes Git URIs in Duck Tails to use `git+file://` with fragment syntax, supports ultrashort refspecs (e.g., just `HEAD`), and provides a clean migration path from the current `git://` format. The approach prioritizes simplicity, backward compatibility, and user ergonomics while avoiding unnecessary complexity.

## Core Design Principles

1. **String-based API** - No exposed SQL types, everything is VARCHAR
2. **Gradual migration** - Accept both old and new formats
3. **Ultrashort support** - Allow `HEAD` to work with cwd as repo
4. **Fragment syntax** - Use `#revspec[:path]` for cleaner parsing
5. **Single output format** - Always emit `git+file://` with absolute paths
6. **Default to current branch** - When no revspec given, use checked-out branch (not HEAD)
7. **All functions have URI overload** - Every git function accepts a URI as first parameter

## URI Format Specification

### Emitted Format (Output from Functions)
Functions **always emit** the canonical format:
```
git+file://<absolute-repo-path>#<revspec>[:<path>]
```

Examples:
- `git+file:///Users/alex/Dev/duck_tails#HEAD:README.md`
- `git+file:///Users/alex/Dev/duck_tails#v1.0..v2.0`
- `git+file:///Users/alex/Dev/duck_tails#main...feature:src/`

### Accepted Input Formats

#### 1. Full URIs
- **Preferred**: `git+file://repo#revspec[:path]`
- **Legacy**: `git://repo/path@revspec` (accepted, never emitted)

#### 2. Ultrashort Refspecs (NEW)
When input doesn't start with `git://` or `git+file://` and contains git revision syntax:
- `HEAD` → `git+file://<cwd>#HEAD`
- `HEAD:README.md` → `git+file://<cwd>#HEAD:README.md`
- `main..feature` → `git+file://<cwd>#main..feature`
- `v1.0...v2.0:src/` → `git+file://<cwd>#v1.0...v2.0:src/`

#### 3. Filesystem Paths
- `path/to/repo` → discovers repo, returns `git+file://<abs-repo>#<current-branch>`
- `.` or empty → uses current directory as repo with current branch
- `/abs/path` → absolute path to repo with current branch

### Default Branch Behavior

**Important:** When no revspec is provided, the default is the **currently checked-out branch**, not `HEAD`:
- This requires opening the repository to determine the current branch
- Cannot be determined in scalar functions (must be done at execution time)
- If detached HEAD state, falls back to the commit SHA

### Parsing Rules

#### Fragment Syntax (`#revspec[:path]`)
- Split on **first** `:` after `#` to separate revspec from path
- All subsequent `:` belong to the path
- Example: `#HEAD:docs/a:b:c.md` → revspec=`HEAD`, path=`docs/a:b:c.md`

#### Legacy @ Syntax (backward compatibility)
- Split on **last** `@` to handle rare `@` in paths
- Example: `git://repo/p@th/file@HEAD` → path=`repo/p@th/file`, rev=`HEAD`

#### Revspec Detection for Ultrashort
Input is treated as ultrashort refspec if it:
1. Doesn't start with `git://` or `git+file://` or `/` (not absolute path)
2. Contains one of these patterns:
   - Git refs: `HEAD`, `main`, `master`, `develop`
   - Relative refs: contains `~`, `^`, `@{`
   - Ranges: contains `..` or `...`
   - SHA prefix: matches `/^[0-9a-f]{6,40}$/i`
   - Has `:` suggesting `revspec:path` format

## Internal Architecture

### GitUri Structure (Internal Only)
```cpp
struct GitUri {
    string abs_repo_path;   // Always absolute
    string revspec;         // Can be ref or range (empty means use current branch)
    optional<string> path;  // File/directory within repo
    
    // Computed lazily
    bool is_range;          // Contains .. or ...
    string range_type;      // "two-dot" or "three-dot"
};
```

### Current Branch Resolution
```cpp
string GetCurrentBranch(git_repository *repo) {
    git_reference *head = nullptr;
    if (git_repository_head(&head, repo) != 0) {
        // Detached HEAD or error - fall back to SHA
        return GetHeadCommitSha(repo);
    }
    
    const char *branch = git_reference_shorthand(head);
    string result(branch);
    git_reference_free(head);
    return result;
}
```

### Parser Enhancement
```cpp
GitUri ParseGitUri(const string &input) {
    // Priority order:
    
    // 1. Full URIs with schemes
    if (starts_with(input, "git+file://") || starts_with(input, "git://")) {
        return ParseFullUri(input);
    }
    
    // 2. Absolute filesystem paths
    if (starts_with(input, "/")) {
        return ParseFilesystemPath(input);
    }
    
    // 3. Check for ultrashort refspec patterns
    if (IsUltrashortRefspec(input)) {
        // Use current working directory as repo
        char cwd[PATH_MAX];
        getcwd(cwd, sizeof(cwd));
        string repo = FindGitRepository(cwd);
        
        // Parse revspec[:path]
        size_t colon = input.find(':');
        if (colon != string::npos) {
            return GitUri{repo, input.substr(0, colon), input.substr(colon+1)};
        } else {
            return GitUri{repo, input, nullopt};
        }
    }
    
    // 4. Relative filesystem path
    return ParseFilesystemPath(input);
}

bool IsUltrashortRefspec(const string &input) {
    // Common git refs
    if (input == "HEAD" || input == "main" || input == "master") return true;
    
    // Contains revision syntax
    if (contains_any(input, "~^@{")) return true;
    
    // Range syntax
    if (contains(input, "..")) return true;
    
    // Looks like SHA
    if (regex_match(input, "^[0-9a-f]{6,40}")) return true;
    
    // Has colon suggesting revspec:path
    if (contains(input, ":") && !contains(input, "://")) return true;
    
    return false;
}
```

## Function Behavior

### Input Handling - All Functions Have URI Overload
All git functions accept these input patterns:

```sql
-- Full URIs (all functions accept this as first parameter)
SELECT * FROM git_tree('git+file:///repo#HEAD:src/');
SELECT * FROM git_log('git+file:///repo#main..feature');
SELECT * FROM git_branches('git+file:///repo#main');
SELECT * FROM git_tags('git+file:///repo#v1.0');
SELECT * FROM git_read('git+file:///repo#HEAD:README.md');

-- Ultrashort (NEW) - uses cwd as repo
SELECT * FROM git_tree('HEAD');                 
SELECT * FROM git_tree('main:src/');            
SELECT * FROM git_log('v1.0..v2.0');           

-- Filesystem paths (defaults to current branch)
SELECT * FROM git_tree('/abs/path/to/repo');    
SELECT * FROM git_tree('relative/path');        
SELECT * FROM git_tree('.');                    

-- Two-parameter form (repo, ref)
SELECT * FROM git_tree('/repo', 'HEAD');        
SELECT * FROM git_log('.', 'main..feature');   
```

### git_tree and git_tree_each Behavior

Both `git_tree` and `git_tree_each` return **multiple rows per tree** - one row for each file/directory entry:

```sql
-- git_tree returns multiple rows (entire tree listing)
SELECT * FROM git_tree('HEAD');
-- Returns: multiple rows, one per file in HEAD

-- git_tree with path prefix (filters tree to that path)
SELECT * FROM git_tree('HEAD:src/');
-- Returns: multiple rows, only files under src/

-- git_tree_each also returns multiple rows per input
WITH refs AS (SELECT 'HEAD' as r UNION SELECT 'main')
SELECT * FROM refs, LATERAL git_tree_each(r) t;
-- Returns: multiple rows per ref (entire tree for each)
```

**Path Resolution:**
- If URI contains `:path`, the tree is filtered to that subtree
- The path uses existing discovery rules (relative to repo root)
- Empty path or no path means entire repository tree

### Output Format
Functions **always emit** `git+file://` URIs:

```sql
SELECT git_file_uri FROM git_tree('HEAD');
-- Returns: git+file:///Users/alex/Dev/duck_tails#abc123:src/main.cpp

SELECT uri FROM git_log('main..feature');  
-- Returns: git+file:///Users/alex/Dev/duck_tails#def456
```

### Function Signatures and Overloads

Every git function has these overloads:

#### git_tree / git_tree_each
```sql
-- URI overload (first parameter)
git_tree(uri VARCHAR) → TABLE(...)
git_tree_each(uri VARCHAR) → TABLE(...)

-- Traditional two-parameter
git_tree(repo_path VARCHAR, ref VARCHAR) → TABLE(...)
git_tree_each(repo_path VARCHAR, ref VARCHAR) → TABLE(...)
```

#### git_log / git_log_each
```sql
-- URI overload
git_log(uri VARCHAR) → TABLE(...)
git_log_each(uri VARCHAR) → TABLE(...)

-- Traditional
git_log(repo_path VARCHAR, ref VARCHAR) → TABLE(...)
git_log_each(repo_path VARCHAR, ref VARCHAR) → TABLE(...)
```

#### git_branches / git_branches_each
```sql
-- URI overload
git_branches(uri VARCHAR) → TABLE(...)
git_branches_each(uri VARCHAR) → TABLE(...)

-- Traditional
git_branches(repo_path VARCHAR) → TABLE(...)
git_branches_each(repo_path VARCHAR) → TABLE(...)
```

#### git_tags / git_tags_each
```sql
-- URI overload
git_tags(uri VARCHAR) → TABLE(...)
git_tags_each(uri VARCHAR) → TABLE(...)

-- Traditional
git_tags(repo_path VARCHAR) → TABLE(...)
git_tags_each(repo_path VARCHAR) → TABLE(...)
```

#### git_read / git_read_each
```sql
-- URI overload (always includes path in URI)
git_read(uri VARCHAR) → TABLE(...)
git_read_each(uri VARCHAR) → TABLE(...)

-- Traditional
git_read(file_path VARCHAR, ref VARCHAR) → TABLE(...)
git_read_each(file_path VARCHAR, ref VARCHAR) → TABLE(...)
```

## Migration Strategy

### Phase 1: Immediate Switch (Current Release)
- Accept both `git://` and `git+file://` input for compatibility
- Add ultrashort refspec support
- **Start emitting `git+file://` format immediately** from all functions
- Document the change prominently

### Phase 2: Deprecation (Future)
- Add deprecation warnings for `git://` input
- Provide migration guide and tools
- Eventually remove `git://` input support

## Implementation Checklist

### Parser Updates
- [ ] Extend `GitPath::Parse` to handle `git+file://` scheme
- [ ] Add fragment (`#revspec[:path]`) parsing
- [ ] Implement ultrashort refspec detection
- [ ] Support both `:` and `@` separators

### Function Updates
- [ ] Update `ConstructGitUri` to emit `git+file://` format
- [ ] Modify bind functions to handle ultrashort input
- [ ] Ensure all functions use unified parser

### Testing
- [ ] Ultrashort refspecs with cwd
- [ ] Fragment syntax with colons in paths
- [ ] Mixed format compatibility
- [ ] Range specifications
- [ ] LATERAL joins with new format

## Examples

### Basic Usage
```sql
-- Ultrashort with current directory (defaults to current branch)
SELECT * FROM git_tree('.');            -- entire tree of current branch
SELECT * FROM git_tree('HEAD');         -- entire tree at HEAD
SELECT * FROM git_tree('HEAD:src/');    -- src/ subtree at HEAD
SELECT * FROM git_log('main..feature'); -- commits in range
SELECT * FROM git_read('HEAD:README.md'); -- specific file

-- Explicit repository
SELECT * FROM git_tree('/path/to/repo'); -- uses current branch of that repo
SELECT * FROM git_tree('git+file:///abs/repo#v1.0:src/');

-- LATERAL joins (both return multiple rows)
WITH refs AS (SELECT 'HEAD' as r UNION SELECT 'main')
SELECT r, COUNT(*) as file_count 
FROM refs, LATERAL git_tree_each(r) t
GROUP BY r;

-- Direct reader integration (requires storing URI in variable or using LATERAL)
WITH csv_file AS (
    SELECT git_file_uri FROM git_tree('HEAD:data/') 
    WHERE path = 'sales.csv'
    LIMIT 1
)
SELECT * FROM csv_file, LATERAL read_csv(csv_file.git_file_uri);
```

### Advanced Patterns
```sql
-- Three-dot ranges (merge-base)
SELECT * FROM git_log('main...feature');

-- Complex paths with colons
SELECT * FROM git_read('HEAD:docs/time:12:30.md');

-- Path filtering with git_tree
SELECT * FROM git_tree('main:src/components/');  -- only components subtree

-- Pipeline URIs through readers
WITH files AS (
    SELECT git_file_uri as uri 
    FROM git_tree('main:data/')
    WHERE path LIKE '%.json'
)
SELECT * FROM files, LATERAL read_json_auto(uri);

-- Multiple refs with multiple rows each
WITH commits AS (
    SELECT commit_hash FROM git_log('HEAD~5..HEAD')
)
SELECT c.commit_hash, COUNT(*) as file_count
FROM commits c, LATERAL git_tree_each('.', c.commit_hash) t
GROUP BY c.commit_hash;
```

## Benefits Over Current Implementation

- **Cleaner syntax** - Fragment notation with `#`
- **Better git alignment** - `revspec:path` is git-native
- **Standardized output** - Always `git+file://` 
- **Ultrashort ergonomics** - Less typing for common cases

## Summary

This design provides a pragmatic path forward that:
1. **Improves ergonomics** with ultrashort refspecs
2. **Standardizes output** to `git+file://` format
3. **Maintains compatibility** with existing code
4. **Avoids complexity** of type system changes
5. **Enables gradual migration** without breaking users

The implementation is straightforward, requiring only parser updates and output format changes, while keeping the core git operations unchanged. Total effort: **2-3 days** for core implementation, **1 week** including comprehensive testing.