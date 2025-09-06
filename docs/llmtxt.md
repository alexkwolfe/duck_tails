# Duck Tails - Git Functions for DuckDB

Duck Tails is a DuckDB extension that provides SQL access to Git repositories through table functions.

## Installation

```sql
INSTALL duck_tails FROM 'http://duckdb.github.io/community-extensions/v1.1.3';
LOAD duck_tails;
```

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

Functions that work with files return `git_file_uri` columns that can be:
- Passed to other `_each` functions for chaining
- Used with DuckDB readers: `read_csv()`, `read_json_auto()`, `read_parquet()`

Functions that work with commits return `uri` columns for commit-specific URIs.

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

Every function has an `_each` variant designed for LATERAL joins with column references.
These support two main usage patterns:

1. **URI Chaining**: Pass `git_file_uri` or `uri` columns from other git functions
2. **Component Assembly**: Pass separate repo_path, file_path, and commit_sha from your datasets

Both approaches enable powerful data analysis across git repositories.

## Functions

### git_tree

Lists files and directories in a git repository at a specific revision.

Signatures:
```sql
git_tree(git_uri_or_repo_path VARCHAR) → TABLE
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
-- List all files at HEAD (traditional syntax)
SELECT * FROM git_tree('.');

-- List files in subdirectory at specific commit
SELECT * FROM git_tree('/home/user/repo/src', '36581c4');

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

Returns: Same as git_tree, including git_file_uri for each file

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
-- Get last 10 commits (traditional syntax)
SELECT * FROM git_log('.') LIMIT 10;

-- Get commits using git URI
SELECT * FROM git_log('git:///home/user/repo@36581c4');

-- Get commits in a two-dot range (commits in feature not in main)
SELECT * FROM git_log('.', 'main..feature');

-- Get commits in a three-dot range (commits since common ancestor)
SELECT * FROM git_log('.', 'main...feature');

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
-- List all local branches (traditional syntax)
SELECT * FROM git_branches('.')
WHERE NOT is_remote;

-- List branches using git URI
SELECT * FROM git_branches('git:///home/user/repo@HEAD');

-- Find current branch
SELECT branch_name FROM git_branches('git://.@HEAD')
WHERE is_head;
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
-- List all tags (traditional syntax)
SELECT * FROM git_tags('.');

-- List tags using git URI
SELECT * FROM git_tags('git:///home/user/repo@HEAD');

-- Find latest semantic version tag
SELECT tag_name FROM git_tags('git://.@HEAD')
WHERE tag_name SIMILAR TO 'v[0-9]+\.[0-9]+\.[0-9]+'
ORDER BY tag_time DESC
LIMIT 1;
```

### git_tags_each

LATERAL join variant of git_tags.

Signature:
```sql
git_tags_each(git_uri_or_path VARCHAR) → TABLE
```

Returns: Same as git_tags

### git_read

Reads file content from a git repository at a specific revision.

Signatures:
```sql
git_read(git_uri_or_file_path VARCHAR) → TABLE
git_read(file_path VARCHAR, ref VARCHAR) → TABLE
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
-- Read README using git URI
SELECT text FROM git_read('git://.@HEAD/README.md');

-- Read file at specific commit (traditional syntax)
SELECT * FROM git_read('/home/user/repo/config.json', '36581c4');

-- Read file using git URI (equivalent to above)
SELECT * FROM git_read('git:///home/user/repo/config.json@36581c4');

-- Read and parse JSON
WITH json_file AS (
  SELECT git_file_uri FROM git_tree('.', 'HEAD')
  WHERE path LIKE '%.json'
  LIMIT 1
)
SELECT * FROM json_file, LATERAL read_json_auto(json_file.git_file_uri);
```

### git_read_each

LATERAL join variant of git_read.

Signature:
```sql
git_read_each(git_uri VARCHAR) → TABLE
```

Returns: Same as git_read

Example:
```sql
-- Read multiple files using git URIs
WITH files AS (
  SELECT 'git://.@HEAD/README.md' as git_uri 
  UNION 
  SELECT 'git://.@HEAD/LICENSE'
)
SELECT git_uri, text 
FROM files, LATERAL git_read_each(git_uri);
```

### git_parents

Returns parent commits for a given commit.

Signatures:
```sql
git_parents(git_uri_or_ref VARCHAR, all_refs BOOLEAN DEFAULT FALSE) → TABLE
git_parents(repo_path VARCHAR, ref VARCHAR DEFAULT 'HEAD', all_refs BOOLEAN DEFAULT FALSE) → TABLE
```

Returns:
- commit_hash VARCHAR: Hash of the commit
- parent_hash VARCHAR: Hash of the parent commit
- parent_index INTEGER: Index of the parent (0-based)

Examples:
```sql
-- Get parents of HEAD (traditional syntax)
SELECT * FROM git_parents('.', 'HEAD');

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
  FROM git_log('.') LIMIT 10
)
SELECT c.commit_uri, p.parent_hash, p.parent_index
FROM commits c, LATERAL git_parents_each(c.commit_uri) p;

-- Chain with other functions using git URIs
WITH commits AS (
  SELECT 'git:///repo@' || commit_hash as git_uri
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

### LATERAL Join Usage Scenarios

#### Scenario 1: URI Chaining from git functions
Use `git_file_uri` or `uri` columns from git functions to chain operations:

```sql
-- Read files discovered by git_tree
SELECT t.path, r.text
FROM git_tree('.', 'HEAD') t,
LATERAL git_read_each(t.git_file_uri) r
WHERE t.type = 'blob' AND t.path LIKE '%.json';

-- Get commit history for files
SELECT t.path, l.commit_hash, l.commit_message
FROM git_tree('.', 'HEAD') t,
LATERAL git_log_each(t.git_file_uri) l
WHERE t.type = 'blob';
```

#### Scenario 2: Component Assembly from datasets
Use separate repo_path, file_path, and commit columns from your data:

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
```

#### Scenario 3: Multi-Level Function Chaining
Chain 3+ functions together using URIs for deep analysis:

```sql
-- Chain git_tree → git_log_each → git_parents_each
SELECT t.path, l.commit_hash, p.parent_hash
FROM git_tree('.', 'HEAD') t,
LATERAL git_log_each(t.git_file_uri) l,
LATERAL git_parents_each(l.uri) p
WHERE t.type = 'blob' AND t.path LIKE '%.cpp'
LIMIT 100;
```

### URI Composability with LATERAL Joins

All `_each` functions support git:// URIs, enabling powerful composability:

```sql
-- Chain git_tree → git_log_each to get history for each file
SELECT 
  t.path, 
  COUNT(l.commit_hash) as commits
FROM git_tree('/repo', 'HEAD') t
CROSS JOIN LATERAL git_log_each(t.git_file_uri) l
GROUP BY t.path;

-- Chain multiple functions using URIs
WITH tree_files AS (
  SELECT path, git_file_uri 
  FROM git_tree('/repo', 'HEAD')
),
file_commits AS (
  SELECT 
    tf.path,
    'git:///repo@' || l.commit_hash as commit_uri,
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
  FROM git_tree('.', 'HEAD')
  WHERE path LIKE 'data/%.csv'
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
-- Read CSV from git using git URIs
WITH data_file AS (
  SELECT git_file_uri FROM git_tree('.', 'HEAD')
  WHERE path = 'data/sales.csv'
)
SELECT * FROM data_file, LATERAL read_csv(data_file.git_file_uri);

-- Read JSON from git
WITH config AS (
  SELECT git_file_uri FROM git_tree('.', 'HEAD')
  WHERE path = 'config.json'
)
SELECT * FROM config, LATERAL read_json_auto(config.git_file_uri);

-- Read Parquet from git
WITH dataset AS (
  SELECT git_file_uri FROM git_tree('.', 'main')
  WHERE path LIKE 'datasets/%.parquet'
)
SELECT * FROM dataset, LATERAL read_parquet(dataset.git_file_uri);
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