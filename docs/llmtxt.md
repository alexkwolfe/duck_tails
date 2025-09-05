# Duck Tails - Git Functions for DuckDB

Duck Tails is a DuckDB extension that provides SQL access to Git repositories through table functions.

## Installation

```sql
INSTALL duck_tails FROM 'http://duckdb.github.io/community-extensions/v1.1.3';
LOAD duck_tails;
```

## Core Concepts

All functions work with Git repositories on your local filesystem.

Input formats:
- Git URI: `git://path/to/repo/path/in/repo@revision`
- Filesystem path: `/path/to/repo` or `.` (uses HEAD by default)
- Two parameters: `(repo_path, ref)` where ref defaults to 'HEAD'

Functions emit URIs in git:// format with absolute paths.

Every function has an `_each` variant for LATERAL joins with column references.

## Functions

### git_tree

Lists files and directories in a git repository at a specific revision.

Signature:
```sql
git_tree(repo_path VARCHAR, ref VARCHAR DEFAULT 'HEAD') → TABLE
```

Returns:
- path VARCHAR: File path relative to repository root
- mode VARCHAR: Git file mode (e.g., '100644' for regular file)
- type VARCHAR: Entry type ('blob' or 'tree')
- size BIGINT: File size in bytes (-1 for directories)
- blob_hash VARCHAR: Git object hash
- commit_hash VARCHAR: Commit hash where file exists
- commit_time TIMESTAMP: Commit timestamp
- commit_message VARCHAR: Commit message
- commit_author VARCHAR: Commit author
- repo_path VARCHAR: Absolute path to repository
- git_file_uri VARCHAR: URI to access this file

Examples:
```sql
-- List all files at HEAD
SELECT * FROM git_tree('.');

-- List files at specific commit
SELECT * FROM git_tree('/path/to/repo', 'abc123');

-- List files at a branch
SELECT * FROM git_tree('.', 'main');
```

### git_tree_each

LATERAL join variant of git_tree for use with column values.

Signatures:
```sql
git_tree_each(ref VARCHAR) → TABLE
git_tree_each(ref VARCHAR, repo_path VARCHAR) → TABLE
```

Parameters:
- ref: Git reference or git:// URI
- repo_path: Repository path (optional)

**URI Support**: Accepts git:// URIs for full composability.

Returns: Same as git_tree, including git_file_uri for each file

Example:
```sql
-- Count files across multiple branches
WITH refs AS (SELECT 'HEAD' as r UNION SELECT 'main')
SELECT r, COUNT(*) as file_count
FROM refs, LATERAL git_tree_each(r) t
GROUP BY r;

-- Use with git:// URIs
SELECT * FROM git_tree_each('git:///repo@HEAD');
```

### git_log

Returns commit history for a repository.

Signature:
```sql
git_log(repo_path VARCHAR, ref VARCHAR DEFAULT 'HEAD') → TABLE
```

Returns:
- commit_hash VARCHAR: Full commit SHA
- short_hash VARCHAR: Abbreviated commit SHA
- author_name VARCHAR: Author name
- author_email VARCHAR: Author email
- author_time TIMESTAMP: Author timestamp
- committer_name VARCHAR: Committer name
- committer_email VARCHAR: Committer email
- commit_time TIMESTAMP: Commit timestamp
- message VARCHAR: Full commit message
- subject VARCHAR: First line of commit message
- body VARCHAR: Commit message body (after first line)
- parents VARCHAR[]: Array of parent commit hashes
- tree_hash VARCHAR: Tree object hash
- repo_path VARCHAR: Absolute path to repository
- uri VARCHAR: Git URI for this commit

Examples:
```sql
-- Get last 10 commits
SELECT * FROM git_log('.') LIMIT 10;

-- Get commits in a range
SELECT * FROM git_log('main..feature');

-- Three-dot range (commits in either branch but not both)
SELECT * FROM git_log('main...feature');
```

### git_log_each

LATERAL join variant of git_log.

Signature:
```sql
git_log_each(repo_path VARCHAR, ref VARCHAR) → TABLE
```

Returns: Same as git_log

### git_branches

Lists all branches in a repository.

Signature:
```sql
git_branches(repo_path VARCHAR) → TABLE
```

Returns:
- branch_name VARCHAR: Branch name
- full_name VARCHAR: Full reference name (e.g., refs/heads/main)
- commit_hash VARCHAR: Commit hash branch points to
- commit_time TIMESTAMP: Timestamp of branch tip commit
- commit_message VARCHAR: Message of branch tip commit
- is_head BOOLEAN: True if this is the current branch
- is_remote BOOLEAN: True if this is a remote branch
- remote_name VARCHAR: Remote name (for remote branches)
- upstream VARCHAR: Upstream branch name
- repo_path VARCHAR: Absolute path to repository

Examples:
```sql
-- List all local branches
SELECT * FROM git_branches('.')
WHERE NOT is_remote;

-- Find current branch
SELECT branch_name FROM git_branches('.')
WHERE is_head;
```

### git_branches_each

LATERAL join variant of git_branches.

Signature:
```sql
git_branches_each(repo_path VARCHAR) → TABLE
```

Returns: Same as git_branches

### git_tags

Lists all tags in a repository.

Signature:
```sql
git_tags(repo_path VARCHAR) → TABLE
```

Returns:
- tag_name VARCHAR: Tag name
- full_name VARCHAR: Full reference name
- commit_hash VARCHAR: Commit hash tag points to
- commit_time TIMESTAMP: Timestamp of tagged commit
- commit_message VARCHAR: Message of tagged commit
- tag_message VARCHAR: Annotated tag message (if any)
- tagger_name VARCHAR: Name of person who created tag
- tagger_email VARCHAR: Email of tagger
- tag_time TIMESTAMP: When tag was created
- is_annotated BOOLEAN: True if annotated tag
- repo_path VARCHAR: Absolute path to repository

Examples:
```sql
-- List all tags
SELECT * FROM git_tags('.');

-- Find latest semantic version tag
SELECT tag_name FROM git_tags('.')
WHERE tag_name SIMILAR TO 'v[0-9]+\.[0-9]+\.[0-9]+'
ORDER BY tag_time DESC
LIMIT 1;
```

### git_tags_each

LATERAL join variant of git_tags.

Signature:
```sql
git_tags_each(repo_path VARCHAR) → TABLE
```

Returns: Same as git_tags

### git_read

Reads file content from a git repository at a specific revision.

Signature:
```sql
git_read(file_path VARCHAR, ref VARCHAR DEFAULT NULL) → TABLE
```

Returns:
- path VARCHAR: File path
- blob_hash VARCHAR: Git blob hash
- size_bytes BIGINT: File size
- content BLOB: Raw file content
- text VARCHAR: Text content (if UTF-8 compatible)
- repo_path VARCHAR: Absolute path to repository
- revision VARCHAR: Resolved revision
- git_file_uri VARCHAR: URI to this file

Examples:
```sql
-- Read README at HEAD
SELECT text FROM git_read('README.md', 'HEAD');

-- Read file at specific commit
SELECT * FROM git_read('/path/to/file.txt', 'abc123');

-- Read and parse JSON
WITH json_file AS (
  SELECT git_file_uri FROM git_tree('HEAD:data/')
  WHERE path LIKE '%.json'
  LIMIT 1
)
SELECT * FROM json_file, LATERAL read_json_auto(json_file.git_file_uri);
```

### git_read_each

LATERAL join variant of git_read.

Signature:
```sql
git_read_each(file_path VARCHAR, ref VARCHAR) → TABLE
```

Returns: Same as git_read

Example:
```sql
-- Read multiple files
WITH files AS (
  SELECT 'README.md' as f UNION SELECT 'LICENSE'
)
SELECT f, text FROM files, LATERAL git_read_each(f, 'HEAD');
```

### git_parents

Returns parent commits for a given commit.

Signatures:
```sql
git_parents(repo_path VARCHAR, ref VARCHAR DEFAULT 'HEAD', all_refs BOOLEAN DEFAULT FALSE) → TABLE
```

Returns:
- commit_hash VARCHAR: Hash of the commit
- parent_hash VARCHAR: Hash of the parent commit
- parent_index INTEGER: Index of the parent (0-based)

Examples:
```sql
-- Get parents of HEAD
SELECT * FROM git_parents('.', 'HEAD');

-- Get parents of all commits
SELECT * FROM git_parents('.', 'HEAD', true);

-- Find merge commits (commits with multiple parents)
SELECT commit_hash, COUNT(*) as parent_count
FROM git_parents('.', 'HEAD', true)
GROUP BY commit_hash
HAVING COUNT(*) > 1;
```

### git_parents_each

LATERAL join variant of git_parents. Accepts column references to generate parent rows for multiple commits.

Signatures:
```sql
git_parents_each(ref VARCHAR) → TABLE
git_parents_each(ref VARCHAR, repo_path VARCHAR) → TABLE
```

Parameters:
- ref: Commit reference (hash, branch, tag) or git:// URI
- repo_path: Repository path (optional, defaults to current directory)

Returns: Same as git_parents

**URI Support**: All parameters accept git:// URIs for composability with other functions.

Examples:
```sql
-- Get parents for multiple commits
WITH commits AS (
  SELECT commit_hash FROM git_log('.') LIMIT 10
)
SELECT c.commit_hash, p.parent_hash, p.parent_index
FROM commits c, LATERAL git_parents_each(c.commit_hash, '.') p;

-- Use with git:// URIs from other functions
WITH commits AS (
  SELECT 'git://' || '/repo' || '@' || commit_hash as git_uri
  FROM git_log('/repo')
)
SELECT * FROM commits c
CROSS JOIN LATERAL git_parents_each(c.git_uri) p;
```

### git_uri

Helper function to construct Git URIs.

Signature:
```sql
git_uri(repo_path VARCHAR, file_path VARCHAR, ref VARCHAR) → VARCHAR
```

Returns: Git URI string in git:// format

Example:
```sql
SELECT git_uri('/path/to/repo', 'src/main.cpp', 'HEAD');
-- Returns: git:///path/to/repo/src/main.cpp@HEAD
```

## Common Patterns

### URI Composability with LATERAL Joins

All `_each` functions support git:// URIs, enabling powerful composability:

```sql
-- Chain git_tree → git_log_each to get history for each file
SELECT 
  t.path, 
  COUNT(l.commit_hash) as commits
FROM git_tree('HEAD', repo_path => '/repo') t
CROSS JOIN LATERAL git_log_each(t.git_file_uri) l
GROUP BY t.path;

-- Chain multiple functions using URIs
WITH tree_files AS (
  SELECT path, git_file_uri 
  FROM git_tree('HEAD', repo_path => '/repo')
),
file_commits AS (
  SELECT 
    tf.path,
    'git://' || '/repo' || '@' || l.commit_hash as commit_uri,
    l.commit_hash
  FROM tree_files tf
  CROSS JOIN LATERAL git_log_each(tf.git_file_uri) l
)
SELECT 
  fc.path,
  fc.commit_hash,
  p.parent_hash
FROM file_commits fc
CROSS JOIN LATERAL git_parents_each(fc.commit_uri) p;
```

### File History

```sql
-- Get history of a specific file
SELECT commit_hash, commit_time, commit_message
FROM git_log('.')
WHERE EXISTS (
  SELECT 1 FROM git_tree('.', commit_hash)
  WHERE path = 'src/important.cpp'
)
ORDER BY commit_time DESC;
```

### Compare Branches

```sql
-- Files changed between branches
WITH main_files AS (
  SELECT path, blob_hash FROM git_tree('.', 'main')
),
feature_files AS (
  SELECT path, blob_hash FROM git_tree('.', 'feature')
)
SELECT 
  COALESCE(m.path, f.path) as path,
  CASE 
    WHEN m.blob_hash IS NULL THEN 'added'
    WHEN f.blob_hash IS NULL THEN 'deleted'
    ELSE 'modified'
  END as status
FROM main_files m
FULL OUTER JOIN feature_files f ON m.path = f.path
WHERE m.blob_hash IS DISTINCT FROM f.blob_hash;
```

### Repository Statistics

```sql
-- Count files by extension
SELECT 
  regexp_extract(path, '\.([^.]+)$', 1) as extension,
  COUNT(*) as file_count,
  SUM(size) as total_bytes
FROM git_tree('.')
WHERE type = 'blob'
GROUP BY extension
ORDER BY file_count DESC;
```

### Working with CSVs in Git

```sql
-- Analyze CSV files in repository
WITH csv_files AS (
  SELECT git_file_uri, path
  FROM git_tree('HEAD:data/')
  WHERE path LIKE '%.csv'
)
SELECT 
  cf.path,
  COUNT(*) as row_count
FROM csv_files cf, LATERAL read_csv(cf.git_file_uri) data
GROUP BY cf.path;
```

### Tag-based Releases

```sql
-- Find files changed between releases
WITH v1_files AS (
  SELECT path, blob_hash FROM git_tree('.', 'v1.0.0')
),
v2_files AS (
  SELECT path, blob_hash FROM git_tree('.', 'v2.0.0')
)
SELECT COUNT(*) as changed_files
FROM v1_files v1
JOIN v2_files v2 ON v1.path = v2.path
WHERE v1.blob_hash != v2.blob_hash;
```

## Integration with DuckDB Readers

The git_file_uri output can be passed to DuckDB's built-in readers:

```sql
-- Read CSV from git
WITH data_file AS (
  SELECT git_file_uri FROM git_tree('HEAD:data/')
  WHERE path = 'sales.csv'
)
SELECT * FROM data_file, LATERAL read_csv(data_file.git_file_uri);

-- Read JSON from git
WITH config AS (
  SELECT git_file_uri FROM git_tree('HEAD')
  WHERE path = 'config.json'
)
SELECT * FROM config, LATERAL read_json_auto(config.git_file_uri);

-- Read Parquet from git
WITH dataset AS (
  SELECT git_file_uri FROM git_tree('main:datasets/')
  WHERE path LIKE '%.parquet'
)
SELECT * FROM dataset, LATERAL read_parquet(dataset.git_file_uri);
```

## Performance Considerations

- Repository discovery walks up directory tree, cache results when possible
- Large repositories may be slow to traverse, use path filters
- Range queries (main..feature) must walk commit history
- File content is loaded into memory, be careful with large files
- Use LIMIT clauses when exploring unfamiliar repositories

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