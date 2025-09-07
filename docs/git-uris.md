# Git URI Specification

## Format

Git URIs in Duck Tails follow this format:
```
git://[repo_path/]file_path@revision
```

## Components

### Prefix
- `git://` - Required prefix that identifies this as a git URI

### Repo Path (optional)
- Path to the git repository (relative or absolute)
- If omitted, uses repository discovery from the file path
- Examples: `./`, `test/tmp/main-repo`, `/absolute/path/to/repo`

### File Path
- Path to the file within the repository
- Can be empty for repository root operations
- Examples: `README.md`, `src/main.cpp`, `docs/guide.md`

### Revision
- Git reference after the `@` symbol
- Defaults to `HEAD` if omitted
- Types:
  - Commit SHA: `@abc123`
  - Branch: `@main`, `@feature/new-thing`
  - Tag: `@v1.0.0`
  - Relative refs: `@HEAD~1`, `@main^2`
  - Ranges: `@v1.0..v2.0` (two-dot: commits in v2.0 but not v1.0) or `@main...feature` (three-dot: commits since merge-base)
  - Special refs: `@HEAD~5` (5 commits before HEAD), `@--all` (all reachable commits)

## Repository Discovery

When a git URI is parsed, the system discovers the repository by:
1. Starting from the specified path
2. Walking up the directory tree looking for `.git`
3. Using the first (deepest) repository found

### Examples
- `git://README.md@HEAD` → Discovers repo from current directory
- `git://src/file.cpp@main` → Discovers repo from `src/` upward
- `git://test/tmp/repo/file.md@v1.0` → Finds nested repository at `test/tmp/repo`

## Edge Cases

### Path Normalization
- Handles `.` and `..` components: `git://./src/../README.md@HEAD`
- Trailing slashes are normalized: `git://repo/` becomes `git://repo`

### Non-existent Files
- Files don't need to exist in the working tree
- Only needs to exist in the specified git revision
- Example: Query deleted files from history: `git://deleted-file.txt@old-commit`

### Special Characters
- Spaces and special characters in paths should be escaped or quoted when used in SQL
- Internal handling preserves paths as-is

### Nested Repositories
- Discovers the most specific (deepest) repository first
- `git://outer/inner/file.txt` uses `inner/` repo if it exists, otherwise `outer/`

### Empty Components
- `git://@HEAD` - Invalid (no file path)
- `git://file.txt@` - Valid (defaults to HEAD)
- `git://file.txt` - Invalid (missing @ separator)

## The git_uri() Function

Constructs a properly formatted git URI from components:

```sql
git_uri(repo_path, file_path, revision) → VARCHAR
```

### Parameters
- `repo_path` (VARCHAR): Repository path (can be `.` for current)
- `file_path` (VARCHAR): File path within repository
- `revision` (VARCHAR): Git revision (commit/branch/tag)

### Examples
```sql
-- Basic usage
SELECT git_uri('.', 'README.md', 'HEAD');
-- Returns: 'git://./README.md@HEAD'

-- With nested repository
SELECT git_uri('test/tmp/main-repo', 'src/main.cpp', 'v1.0');
-- Returns: 'git://test/tmp/main-repo/src/main.cpp@v1.0'

-- Repository root
SELECT git_uri('/abs/path/repo', '', 'main');
-- Returns: 'git:///abs/path/repo@main'
```

## Usage in Duck Tails Functions

### Reading Files
```sql
-- Read file from git
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Using git_read
SELECT * FROM git_read('git://README.md@main');
```

### Tree Operations
```sql
-- git_tree outputs git_file_uri column
SELECT git_file_uri FROM git_tree('HEAD');
-- Returns URIs like: 'git://./src/main.cpp@abc123'

-- Using URIs to read specific files
SELECT * FROM git_tree('HEAD') t
JOIN LATERAL git_read(t.git_file_uri) r ON TRUE;
```

### Diff Operations
```sql
-- Two-dot range (changes in v2.0 not in v1.0)
SELECT * FROM read_git_diff('git://file.txt@v1.0..v2.0');

-- Three-dot range (changes since merge-base)
SELECT * FROM read_git_diff('git://file.txt@main...feature');
```

## Function Parameter Conventions

All Duck Tails git functions accept their first parameter in two forms:

### 1. Filesystem Path (repo_path)
When passing a filesystem path, you can optionally provide a revision as the second parameter:
```sql
-- With explicit revision
SELECT * FROM git_log('path/to/repo', 'main');
SELECT * FROM git_tree('.', 'v1.0.0');

-- Without revision (defaults to HEAD)
SELECT * FROM git_log('.');
SELECT * FROM git_tree('path/to/repo');
```

### 2. Git URI
When passing a git:// URI, the revision is embedded in the URI:
```sql
-- Revision specified in URI
SELECT * FROM git_log('git://path/to/repo@main');
SELECT * FROM git_tree('git://.@v1.0.0');

-- Note: Cannot pass revision parameter when using git:// URI
-- This would error: git_log('git://repo@main', 'other-branch')
```

### Default Revisions
- Filesystem paths without revision parameter: defaults to `HEAD`
- Git URIs without `@revision`: invalid (must include @ separator)
- Empty revision after @: defaults to `HEAD` (e.g., `git://file.txt@`)

## Implementation Notes

- All URI construction should use `git_uri()` function for consistency
- Repository discovery happens at parse time via `GitPath::Parse`
- URIs are normalized to absolute repository paths internally
- The `git_uri` column in git functions provides ready-to-use URIs
- Functions validate that git:// URIs don't conflict with revision parameters

## Schema Standardization

**⚠️ Breaking Changes Coming:** Duck Tails functions are being standardized for consistent URI schemas. See `uri-clarity.md` for details.

**Key changes:**
- All URI-returning functions will have consistent first 8 columns
- Column names standardized: `git_uri` as first column for all functions
- Enhanced metadata and cross-function compatibility

**Migration guide** and timeline available in the URI clarity plan.