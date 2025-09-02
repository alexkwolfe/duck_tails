## Summary

Enhances Duck Tails' existing multi-repository capabilities with flexible relative path support (`../other-repo`), smart libgit2-based repository discovery, and repository context visibility.

## Key Improvements

### 1. Flexible Repository Path Support
**Before:** Only absolute paths and current directory (`.`)
```sql
SELECT commit_hash, author_name FROM git_log('/absolute/path/to/repo');
SELECT * FROM read_csv('git://data.csv@HEAD');  -- current repo only
```

**After:** Relative paths (`../other-repo`) and proper `./` handling
```sql  
SELECT repo_path, commit_hash, author_name FROM git_log('../other-project');
SELECT * FROM read_csv('git://../data-repo/sales.csv@HEAD');
SELECT * FROM read_csv('git://./subdir/file.csv@HEAD');  
```

### 2. Smart Repository Discovery
**Before:** Simple path assumption, no validation - could fail silently
**After:** libgit2-based discovery that actually finds `.git` directories and provides clear error messages

### 3. Repository Context Visibility  
**Before:** No way to know which repo each row came from
```sql
SELECT commit_hash FROM git_log('/path/to/repo');  -- which repo is this from?
```

**After:** `repo_path` column shows repository context
```sql
SELECT repo_path, commit_hash FROM git_log('/path/to/repo'); 
-- Returns: repo_path="/path/to/repo", commit_hash="abc123..."
```

## New `repo_path` Column

All git functions (`git_log`, `git_branches`, `git_tags`) now include `repo_path` as the **first column**, showing which repository each row comes from:

```sql
SELECT repo_path, branch_name FROM git_branches('../other-repo');
-- Returns: repo_path="../other-repo", branch_name="main" 
```

This makes multi-repository analysis more reliable and visible:
```sql
SELECT repo_path, COUNT(*) as commit_count 
FROM (SELECT * FROM git_log('.') UNION ALL SELECT * FROM git_log('../other-repo'))
GROUP BY repo_path;
```

## Repository Discovery Algorithm

Uses libgit2-powered discovery that walks up directory trees to find `.git` locations:

1. **Path Normalization**: Resolves `../` and `./` components to absolute paths
2. **Non-existent Path Handling**: If path doesn't exist, walks up to first existing directory
3. **Repository Search**: From starting directory, walks up checking each directory with libgit2's `git_repository_open()`
4. **Discovery**: Finds closest `.git` directory/file (handles submodules, worktrees)
5. **File Path Calculation**: Computes relative file path from discovered repository root

**Example**: `git://../../other-project/deep/data.csv@HEAD`
- Normalizes to `/absolute/path/other-project/deep/data.csv`  
- Walks up: `/absolute/path/other-project/deep` → `/absolute/path/other-project` ← **finds .git here**
- Result: `repo_path="/absolute/path/other-project"`, `file_path="deep/data.csv"`

### 4. Enhanced Path Handling
**Before:** Path normalization issues, especially with relative paths like `./subdir/file.csv`  
**After:** Consistent handling of `../` and `./` patterns with proper path resolution

### 5. Better Error Messages
**Before:** Misleading errors - `"No files found that match the pattern"` when repository doesn't exist  
**After:** Clear errors - `"No git repository found for path: /nonexistent/repo"`

## Test Coverage
176 assertions across 10 test cases with comprehensive edge case testing.

## Breaking Changes  
**None** - All existing functionality preserved. The `repo_path` column is an additive enhancement.

## Main Benefit
This PR makes **multi-repository analysis more reliable, intuitive, and visible** - enhancing Duck Tails' existing multi-repository capabilities rather than adding them from scratch.