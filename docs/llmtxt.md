# Duck Tails - Git Functions for DuckDB

Duck Tails is a DuckDB extension that provides SQL access to Git repositories through table functions.

## Installation

```sql
LOAD duck_tails;
```

Note: Duck Tails is currently a local extension that must be built and loaded directly.

## Core Concepts

All functions work with Git repositories on your local filesystem.

### Git URIs

Duck Tails uses a custom URI format: `git://[repo_path]/[file_path]@[ref]`

Examples:
- `git:///home/user/myrepo@HEAD` - absolute path to repository root at HEAD
- `git:///home/user/myrepo/src/main.cpp@36581c4` - specific file at commit
- `git://./relative/path@main` - relative repository path at branch
- `git://../other_repo@36581c4` - navigate to sibling repository
- `git://../../parent/other_repo@HEAD` - navigate up and access different repo

Functions that work with files return `git_uri` columns that can be:
- Passed to other `_each` functions for chaining
- Used with DuckDB readers: `read_csv()`, `read_json_auto()`, `read_parquet()`

Functions that work with commits return `git_uri` columns for commit-specific URIs.

### URI Construction

The `git_uri()` helper function constructs Git URIs from components:

```sql
git_uri(repo_path VARCHAR, file_path VARCHAR, ref VARCHAR) → VARCHAR
```

Example:
```sql
SELECT git_uri('/home/user/repo', 'src/main.cpp', '36581c4');
-- Returns: git:///home/user/repo/src/main.cpp@36581c4
```

This is useful for programmatically building URIs, though most workflows use the URIs returned directly by other git functions.

### Input Formats & Path Resolution

Functions accept either git URIs or filesystem paths that are automatically resolved:

**Git URIs**: `git://path/to/repo/path/in/repo@revision`
- Repository and file path are explicitly specified
- Revision is embedded in the URI

**Filesystem Paths**: `/path/to/repo` or `/path/to/repo/file.txt`
- Duck Tails discovers the repository by walking up the directory tree from the given path
- The repository root becomes `repo_path`, any additional path becomes `file_path`
- Examples:
    - `/home/user/repo/src/main.cpp` → repo: `/home/user/repo`, file: `src/main.cpp`
    - `./` → repo: `./`, file: `` (empty = repository root)

**Two Parameters**: `(repo_or_file_path, ref)` where ref defaults to 'HEAD'
- Supports range syntax: `main..feature` (two-dot) or `main...feature` (three-dot)
- Special refs: `HEAD~5`, `--all`

### LATERAL Join Functions

Most functions have an `_each` variant designed for LATERAL joins with column references.
These support two main usage patterns:

1. **URI Chaining**: Pass `git_uri` columns from other git functions
2. **Component Assembly**: Pass separate repo_path, file_path, and commit_sha from your datasets

**Important**: Some functions like `git_read_each` are LATERAL-only and do not have standalone versions. These functions must be used with LATERAL joins and cannot be called directly.

## Functions

### git_tree

Lists files and directories in a git repository at a specific revision.

Signatures:
```sql
git_tree(git_uri_or_repo_path VARCHAR) → TABLE
git_tree(repo_path VARCHAR, ref VARCHAR DEFAULT 'HEAD') → TABLE
```

Returns:
- git_uri VARCHAR: Complete git:// URI to access this file
- repo_path VARCHAR: Repository filesystem path
- commit_hash VARCHAR: Git commit hash
- tree_hash VARCHAR: Git tree hash containing the file
- file_path VARCHAR: File path relative to repository root
- file_ext VARCHAR: File extension
- ref VARCHAR: Git reference (branch, tag, etc.)
- blob_hash VARCHAR: Git blob object hash
- commit_date TIMESTAMP: Commit timestamp
- mode INTEGER: Git file mode (permissions)
- size_bytes BIGINT: File size in bytes
- kind VARCHAR: Object kind (blob, tree, etc.)
- is_text BOOLEAN: Whether content is text
- encoding VARCHAR: Text encoding (utf8, binary)

Examples:
```sql
-- List all files at HEAD
SELECT * FROM git_tree('git://.@HEAD');

-- List files in subdirectory at specific commit
SELECT * FROM git_tree('git:///home/user/repo/src@36581c4');

-- List files using git URI (equivalent to above)
SELECT * FROM git_tree('git:///home/user/repo/src@36581c4');

-- List files at a branch using git URI
SELECT * FROM git_tree('git://.@main');
```

### git_tree_each

LATERAL join variant of git_tree for use with column values.

Signature:
```sql
git_tree_each(git_uri VARCHAR) → TABLE
```

Parameters:
- git_uri: Git URI (git://path/to/repo@ref) from another function's output

**URI Support**: Accepts git:// URIs for full composability.

Returns: Same as git_tree, including git_uri for each file

Example:
```sql
-- Count files across multiple branches using git URIs
WITH refs AS (
  SELECT 'git://.@HEAD' as r 
  UNION 
  SELECT 'git://.@main'
)
SELECT r, COUNT(*) as file_count
FROM refs, LATERAL git_tree_each(r) t
GROUP BY r;

-- Use with git:// URIs
SELECT * FROM git_tree_each('git://<repo_path>/<file_path>@<ref>');
```

### git_log

Returns commit history for a repository.

Signature:
```sql
git_log(git_uri_or_repo_path VARCHAR) → TABLE
```

Returns:
- repo_path VARCHAR: Repository filesystem path
- commit_hash VARCHAR: Full commit SHA
- author_name VARCHAR: Author name
- author_email VARCHAR: Author email
- committer_name VARCHAR: Committer name
- committer_email VARCHAR: Committer email
- author_date TIMESTAMP: Author timestamp
- commit_date TIMESTAMP: Commit timestamp
- message VARCHAR: Full commit message
- parent_count INTEGER: Number of parent commits
- tree_hash VARCHAR: Tree object hash

Examples:
```sql
-- Get last 10 commits
SELECT * FROM git_log('git://.@HEAD') LIMIT 10;

-- Get commits using git URI
SELECT * FROM git_log('git:///home/user/repo@36581c4');

-- Get commits in a two-dot range (commits in feature not in main)
SELECT * FROM git_log('git://.@main..feature');

-- Get commits in a three-dot range (commits since common ancestor)
SELECT * FROM git_log('git://.@main...feature');

-- Get commits from subdirectory at specific commit
SELECT * FROM git_log('git:///home/user/repo/src@main');
```

### git_log_each

LATERAL join variant of git_log.

Signature:
```sql
git_log_each(git_uri VARCHAR) → TABLE
```

Returns: Same as git_log

### git_branches

Lists all branches in a repository.

Signature:
```sql
git_branches(git_uri_or_repo_path VARCHAR) → TABLE
```

Note: git_branches lists all branches in a repository - it doesn't accept a ref parameter since it shows branch information, not content at a specific revision.

Returns:
- repo_path VARCHAR: Repository filesystem path
- branch_name VARCHAR: Branch name
- commit_hash VARCHAR: Commit hash branch points to
- is_current BOOLEAN: True if this is the current branch
- is_remote BOOLEAN: True if this is a remote branch

Examples:
```sql
-- List all local branches
SELECT * FROM git_branches('git://.@HEAD');

-- List branches using git URI
SELECT * FROM git_branches('git:///home/user/repo@HEAD');

-- Find current branch
SELECT branch_name FROM git_branches('git://.@HEAD')
WHERE is_current;
```

### git_branches_each

LATERAL join variant of git_branches.

Signature:
```sql
git_branches_each(git_uri_or_path VARCHAR) → TABLE
```

Returns: Same as git_branches

### git_tags

Lists all tags in a repository.

Signature:
```sql
git_tags(git_uri_or_repo_path VARCHAR) → TABLE
```

Returns:
- repo_path VARCHAR: Repository filesystem path
- tag_name VARCHAR: Tag name
- commit_hash VARCHAR: Commit hash tag points to
- tagger_name VARCHAR: Name of person who created tag
- tagger_date TIMESTAMP: When tag was created
- message VARCHAR: Annotated tag message (if any)
- is_annotated BOOLEAN: True if annotated tag

Examples:
```sql
-- List all tags
SELECT * FROM git_tags('git://.@HEAD');

-- List tags using git URI
SELECT * FROM git_tags('git:///home/user/repo@HEAD');

-- Find semantic version tags
SELECT tag_name FROM git_tags('git://.@HEAD')
WHERE tag_name SIMILAR TO 'v[0-9]+\.[0-9]+\.[0-9]+';
```

### git_tags_each

LATERAL join variant of git_tags.

Signature:
```sql
git_tags_each(git_uri_or_path VARCHAR) → TABLE
```

Returns: Same as git_tags

### git_read

**Note**: `git_read` is a standalone function that does not accept column references. For LATERAL join usage with column references, use `git_read_each`.

Signature:
```sql
git_read(git_uri VARCHAR) → TABLE
```

Returns:
- git_uri VARCHAR: Complete git:// URI
- repo_path VARCHAR: Repository filesystem path  
- commit_hash VARCHAR: Git commit hash
- tree_hash VARCHAR: Git tree hash containing the file
- file_path VARCHAR: File path relative to repository root
- file_ext VARCHAR: File extension
- ref VARCHAR: Git reference (branch, tag, etc.)
- blob_hash VARCHAR: Git blob object hash
- mode INTEGER: Git file mode (permissions)
- kind VARCHAR: Object kind (blob/tree/etc)
- is_text BOOLEAN: Whether content is text
- encoding VARCHAR: Text encoding
- size_bytes BIGINT: File size in bytes
- truncated BOOLEAN: Whether content was truncated
- text VARCHAR: Text content (if text file)
- blob BLOB: Raw file content

Examples:
```sql
-- Read README using git URI
SELECT text FROM git_read('git://.@HEAD/README.md');

-- Read file at specific commit
SELECT * FROM git_read('git:///home/user/repo/config.json@36581c4');
```

### git_read_each

**LATERAL-only function**: This function can only be used with LATERAL joins and column references. It cannot be called directly.

Signature:
```sql
git_read_each(git_uri VARCHAR) → TABLE
```

Returns: Same as git_read (17 columns including git_uri, repo_path, commit_hash, tree_hash, file_path, file_ext, ref, blob_hash, mode, kind, is_text, encoding, size_bytes, truncated, text, blob)

Example:
```sql
-- Read multiple files using git URIs from another function
SELECT t.file_path, r.text
FROM git_tree('git://.@HEAD') t,
LATERAL git_read_each(t.git_uri) r
WHERE t.file_path LIKE '%.md';
```

### git_parents

Returns parent commits for a given commit.

Signatures:
```sql
git_parents(git_uri_or_ref VARCHAR, all_refs BOOLEAN DEFAULT FALSE) → TABLE
git_parents(repo_path VARCHAR, ref VARCHAR DEFAULT 'HEAD', all_refs BOOLEAN DEFAULT FALSE) → TABLE
```

Returns:
- repo_path VARCHAR: Repository filesystem path
- commit_hash VARCHAR: Hash of the commit
- parent_hash VARCHAR: Hash of the parent commit
- parent_index INTEGER: Index of the parent (0-based)

Examples:
```sql
-- Get parents of HEAD
SELECT * FROM git_parents('git://.@HEAD');

-- Get parents using git URI
SELECT * FROM git_parents('git://.@36581c4');

-- Get parents of all commits
SELECT * FROM git_parents('git:///home/user/repo@HEAD', true);

-- Find merge commits (commits with multiple parents)
SELECT commit_hash, COUNT(*) as parent_count
FROM git_parents('git://.@HEAD', true)
GROUP BY commit_hash
HAVING COUNT(*) > 1;
```

### git_parents_each

LATERAL join variant of git_parents. Accepts column references to generate parent rows for multiple commits.

Signature:
```sql
git_parents_each(git_uri VARCHAR) → TABLE
```

Parameters:
- git_uri: Git URI (git://path/to/repo@ref) from another function's output

Returns: Same as git_parents

**URI Support**: All parameters accept git:// URIs for composability with other functions.

Examples:
```sql
-- Get parents for multiple commits using URIs
WITH commits AS (
  SELECT 'git://.@' || commit_hash as commit_uri
  FROM git_log('git://.@HEAD') LIMIT 10
)
SELECT c.commit_uri, p.parent_hash, p.parent_index
FROM commits c, LATERAL git_parents_each(c.commit_uri) p;

-- Chain with other functions using git URIs
WITH commits AS (
  SELECT 'git:///repo@' || commit_hash as git_uri
  FROM git_log('git:///repo@HEAD')
)
SELECT * FROM commits c
CROSS JOIN LATERAL git_parents_each(c.uri) p;
```

### git_uri

Helper function to construct Git URIs.

Signature:
```sql
git_uri(repo_path VARCHAR, file_path VARCHAR, ref VARCHAR) → VARCHAR
```

Returns: Git URI string in git:// format

Examples:
```sql
-- Basic URI construction
SELECT git_uri('/path/to/repo', 'src/main.cpp', 'HEAD');
-- Returns: git:///path/to/repo/src/main.cpp@HEAD

-- Construct URIs from your data
WITH my_files AS (
  SELECT '/home/user/project' as repo,
         'config.json' as file,
         'main' as branch
  UNION ALL
  SELECT '/home/user/other-project' as repo,
         'package.json' as file,  
         'v1.2.3' as branch
)
SELECT 
  repo,
  file,
  branch,
  git_uri(repo, file, branch) as constructed_uri
FROM my_files;

-- Use git_uri to read files from different repositories and revisions
WITH file_specs AS (
  SELECT '/repo1' as repo_path, 'README.md' as file_path, 'main' as ref
  UNION ALL  
  SELECT '/repo2' as repo_path, 'config.yml' as file_path, 'develop' as ref
)
SELECT 
  fs.repo_path,
  fs.file_path,
  r.text
FROM file_specs fs,
LATERAL git_read_each(git_uri(fs.repo_path, fs.file_path, fs.ref)) r;

-- Dynamically build URIs for commit analysis
WITH commit_analysis AS (
  SELECT 
    '/my/repo' as repo_path,
    commit_hash,
    'src/' as directory_filter
  FROM git_log('git:///my/repo@HEAD')
  LIMIT 5
)
SELECT 
  ca.commit_hash,
  t.file_path,
  t.size_bytes
FROM commit_analysis ca,
LATERAL git_tree_each(git_uri(ca.repo_path, ca.directory_filter, ca.commit_hash)) t
WHERE t.kind = 'blob';
```

## Common Patterns

### LATERAL Join Usage Scenarios

#### Scenario 1: URI Chaining from git functions
Use `git_uri` or `uri` columns from git functions to chain operations:

```sql
-- Read files discovered by git_tree
SELECT t.file_path, r.text
FROM git_tree('git://.@HEAD') t,
LATERAL git_read_each(t.git_uri) r
WHERE t.file_path LIKE '%.json';

-- Get commit history for files
SELECT t.file_path, l.commit_hash
FROM git_tree('git://.@HEAD') t,
LATERAL git_log_each(t.git_uri) l;
```

#### Scenario 2: Component Assembly from datasets using git_uri()
Use git_uri() to combine separate repo_path, file_path, and commit columns from your data:

```sql
-- Your dataset with file versions
WITH file_versions AS (
  SELECT '/home/user/repo' as repo_path,
         'config.json' as file_path, 
         '36581c4' as commit_sha
  UNION ALL
  SELECT '/home/user/other_repo' as repo_path,
         'package.json' as file_path,
         'main' as commit_sha
)
-- Read each file using git_uri construction
SELECT fv.repo_path, fv.file_path, r.text
FROM file_versions fv,
LATERAL git_read_each(git_uri(fv.repo_path, fv.file_path, fv.commit_sha)) r;

-- Another example: analyze configuration files across multiple repos and branches
WITH config_matrix AS (
  SELECT unnest(['/repo1', '/repo2', '/repo3']) as repo,
         unnest(['config.yml', 'settings.json']) as config_file,
         unnest(['main', 'develop', 'staging']) as branch
)
SELECT 
  cm.repo,
  cm.config_file, 
  cm.branch,
  LENGTH(r.text) as config_size
FROM config_matrix cm,
LATERAL git_read_each(git_uri(cm.repo, cm.config_file, cm.branch)) r
WHERE r.text IS NOT NULL;

-- Build URIs for bulk file operations across commit history
WITH commit_files AS (
  SELECT 
    l.commit_hash,
    unnest(['package.json', 'README.md', 'config.yml']) as target_file
  FROM git_log('git:///my/project@HEAD') l
  LIMIT 10
)
SELECT 
  cf.commit_hash,
  cf.target_file,
  r.size_bytes,
  r.is_text
FROM commit_files cf,
LATERAL git_read_each(git_uri('/my/project', cf.target_file, cf.commit_hash)) r;
```

#### Scenario 3: Multi-Level Function Chaining
Chain 3+ functions together using URIs for deep analysis:

```sql
-- Chain git_tree → git_log_each → git_parents_each
SELECT t.file_path, l.commit_hash, p.parent_hash
FROM git_tree('git://.@HEAD') t,
LATERAL git_log_each(t.git_uri) l,
LATERAL git_parents_each(l.git_uri) p
WHERE t.file_path LIKE '%.cpp'
LIMIT 100;
```

### URI Composability with LATERAL Joins

All `_each` functions support git:// URIs, enabling powerful composability:

```sql
-- Chain git_tree → git_log_each to get history for each file
SELECT 
  t.file_path, 
  COUNT(l.commit_hash) as commits
FROM git_tree('git:///repo@HEAD') t
CROSS JOIN LATERAL git_log_each(t.git_uri) l
GROUP BY t.file_path;

-- Chain multiple functions using URIs
WITH tree_files AS (
  SELECT file_path, git_uri 
  FROM git_tree('git:///repo@HEAD')
),
file_commits AS (
  SELECT 
    tf.file_path,
    'git:///repo@' || l.commit_hash as commit_uri,
    l.commit_hash
  FROM tree_files tf
  CROSS JOIN LATERAL git_log_each(tf.git_uri) l
)
SELECT 
  fc.file_path,
  fc.commit_hash,
  p.parent_hash
FROM file_commits fc
CROSS JOIN LATERAL git_parents_each(fc.commit_uri) p;
```

### File History

```sql
-- Get history of a specific file
SELECT commit_hash, commit_date, message
FROM git_log('git://.@HEAD')
WHERE EXISTS (
  SELECT 1 FROM git_tree('git://.@' || commit_hash)
  WHERE file_path = 'src/important.cpp'
)
ORDER BY commit_date DESC;
```

### Compare Branches

```sql
-- Files changed between branches
WITH main_files AS (
  SELECT file_path, blob_hash FROM git_tree('git://.@main')
),
feature_files AS (
  SELECT file_path, blob_hash FROM git_tree('git://.@feature')
)
SELECT 
  COALESCE(m.file_path, f.file_path) as file_path,
  CASE 
    WHEN m.blob_hash IS NULL THEN 'added'
    WHEN f.blob_hash IS NULL THEN 'deleted'
    ELSE 'modified'
  END as status
FROM main_files m
FULL OUTER JOIN feature_files f ON m.file_path = f.file_path
WHERE m.blob_hash IS DISTINCT FROM f.blob_hash;
```

### Repository Statistics

```sql
-- Count files by extension
SELECT 
  regexp_extract(file_path, '\.([^.]+)$', 1) as extension,
  COUNT(*) as file_count,
  SUM(size_bytes) as total_bytes
FROM git_tree('git://.@HEAD')
WHERE kind = 'blob'
GROUP BY extension
ORDER BY file_count DESC;
```

### Working with CSVs in Git

```sql
-- Analyze CSV files in repository
WITH csv_files AS (
  SELECT git_uri, file_path
  FROM git_tree('git://.@HEAD')
  WHERE file_path LIKE 'data/%.csv'
)
SELECT 
  cf.file_path,
  COUNT(*) as row_count
FROM csv_files cf, LATERAL read_csv(cf.git_uri) data
GROUP BY cf.file_path;
```

### Tag-based Releases

```sql
-- Find files changed between releases
WITH v1_files AS (
  SELECT file_path, blob_hash FROM git_tree('git://.@v1.0.0')
),
v2_files AS (
  SELECT file_path, blob_hash FROM git_tree('git://.@v2.0.0')
)
SELECT COUNT(*) as changed_files
FROM v1_files v1
JOIN v2_files v2 ON v1.file_path = v2.file_path
WHERE v1.blob_hash != v2.blob_hash;
```

## Integration with DuckDB Readers

The git_uri output can be passed to DuckDB's built-in readers:

```sql
-- Read CSV from git using git URIs
WITH data_file AS (
  SELECT git_uri FROM git_tree('git://.@HEAD')
  WHERE file_path = 'data/sales.csv'
)
SELECT * FROM data_file, LATERAL read_csv(data_file.git_uri);

-- Read JSON from git
WITH config AS (
  SELECT git_uri FROM git_tree('git://.@HEAD')
  WHERE file_path = 'config.json'
)
SELECT * FROM config, LATERAL read_json_auto(config.git_uri);

-- Read Parquet from git
WITH dataset AS (
  SELECT git_uri FROM git_tree('git://.@main')
  WHERE file_path LIKE 'datasets/%.parquet'
)
SELECT * FROM dataset, LATERAL read_parquet(dataset.git_uri);
```

### git_clone

Clones Git repositories from remote URLs with smart conflict handling and automatic path extraction.

Signature:
```sql
git_clone(url VARCHAR) → TABLE
git_clone(url VARCHAR, local_path VARCHAR) → TABLE
git_clone(url VARCHAR, options STRUCT) → TABLE
git_clone(url VARCHAR, local_path VARCHAR, options STRUCT) → TABLE
```

Returns:
- url VARCHAR: The source repository URL
- local_path VARCHAR: The actual path where repository was cloned
- status VARCHAR: 'success' or 'error'
- action VARCHAR: 'cloned', 'updated', 'up_to_date', or 'error'
- message VARCHAR: Success message or error details
- commit_hash VARCHAR: HEAD commit hash of cloned/updated repository
- previous_hash VARCHAR: Previous HEAD commit (for updates, NULL for clones)
- commit_time TIMESTAMP: HEAD commit timestamp
- size_bytes BIGINT: Total repository size (-1 if not available)

Options STRUCT fields (all optional):
- branch VARCHAR: Specific branch to clone (default: default branch)
- depth INTEGER: Shallow clone depth (default: NULL = full clone)
- bare BOOLEAN: Create bare repository (default: false)
- no_checkout BOOLEAN: Skip checkout after clone (default: false)
- timeout INTEGER: Timeout in seconds (default: 300 = 5 minutes)
- force BOOLEAN: Overwrite existing directory (default: false)

Smart Behavior:
- **Auto-path extraction**: When local_path is omitted, automatically extracts repository name from URL
- **Smart updates**: If repository already exists, performs `git pull` instead of erroring
- **Conflict resolution**: Returns 'updated', 'up_to_date', or 'error' for existing repositories

Examples:
```sql
-- Basic clone with auto-generated local path
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git');

-- Clone to specific directory
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git', 'my-duckdb');

-- Shallow clone with options
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git', {
    branch: 'main',
    depth: 1,
    timeout: 600
});

-- Clone multiple repositories (LATERAL join)
SELECT r.name, c.status, c.local_path
FROM repo_list r, LATERAL git_clone_each(r.url) c
WHERE c.status = 'success';
```

### git_clone_each

LATERAL version of git_clone for processing multiple URLs from input rows.

Signature:
```sql
git_clone_each(url VARCHAR) → TABLE
git_clone_each(url VARCHAR, local_path VARCHAR) → TABLE
git_clone_each(url VARCHAR, options STRUCT) → TABLE
git_clone_each(url VARCHAR, local_path VARCHAR, options STRUCT) → TABLE
```

Returns the same schema as git_clone.

Examples:
```sql
-- Create table with repository URLs
CREATE TABLE repositories AS VALUES
  ('https://github.com/duckdb/duckdb.git'),
  ('https://github.com/apache/arrow.git'),
  ('https://github.com/postgres/postgres.git')
AS t(url);

-- Clone all repositories
SELECT r.url, c.status, c.local_path, c.action
FROM repositories r, LATERAL git_clone_each(r.url) c;

-- Clone with custom paths
SELECT r.url, c.status, c.local_path
FROM repositories r, LATERAL git_clone_each(r.url, 'repos/' || RIGHT(r.url, 20)) c;
```

## Limitations

- Git clone requires network connectivity for remote repositories
- SSH authentication not yet supported (HTTPS only)
- No support for git submodules in cloned repositories
- Binary file content returned as BLOB, text extraction depends on encoding
- Maximum file size limited by available memory

## Performance Considerations

- Repository discovery walks up directory tree, cache results when possible
- Large repositories may be slow to traverse, use path filters
- Range queries (main..feature) must walk commit history
- File content is loaded into memory, be careful with large files
- Use LIMIT clauses when exploring unfamiliar repositories

## Error Handling

Common errors:
- "Not a git repository": Directory is not inside a git repository
- "Failed to open repository": Path exists but is not a valid git repository
- "Reference not found": Branch, tag, or commit hash doesn't exist
- "Path not found": File or directory doesn't exist in the repository at given revision

## Future Features (Planned)

- Git diff functions for comparing revisions
- Support for git submodules
- Remote repository support
- Commit creation and repository modification functions