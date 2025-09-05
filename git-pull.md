# Git Pull Function Specification

## Overview
Add `git_pull` and `git_pull_each` functions to Duck Tails extension, enabling repository updating (fetch + merge/fast-forward) directly from SQL, following the same patterns as other git_* functions.

## Function Signatures

### git_pull
```sql
-- Pull from default remote/branch
git_pull(repo_path VARCHAR) → TABLE

-- Pull from specific ref/branch
git_pull(repo_path VARCHAR, ref VARCHAR) → TABLE

-- With options
git_pull(repo_path VARCHAR, ref VARCHAR, options STRUCT) → TABLE
```

**Parameters:**
- `repo_path`: Path to git repository (supports relative, absolute, and git:// URIs)
- `ref`: Branch/tag/remote to pull from (default: current branch's upstream)
- `options`: Pull configuration options

**Options STRUCT fields** (all optional):
- `strategy` VARCHAR: 'merge', 'rebase', or 'fast_forward_only' (default: 'merge')
- `remote` VARCHAR: Remote name to pull from (default: 'origin')
- `force` BOOLEAN: Force update even with conflicts (default: false)
- `timeout` INTEGER: Timeout in seconds (default: 300 = 5 minutes)
- `autostash` BOOLEAN: Automatically stash/pop local changes (default: false)

**Returns:**
- `repo_path` VARCHAR: Absolute path to repository
- `status` VARCHAR: 'success' or 'error'
- `action` VARCHAR: 'fast_forwarded', 'merged', 'rebased', 'up_to_date', 'conflict', or 'error'
- `message` VARCHAR: Success message or error details
- `previous_hash` VARCHAR: HEAD commit before pull
- `current_hash` VARCHAR: HEAD commit after pull
- `commits_added` INTEGER: Number of new commits
- `files_changed` INTEGER: Number of files modified
- `remote` VARCHAR: Remote that was pulled from
- `branch` VARCHAR: Branch that was updated

### git_pull_each
```sql
-- For LATERAL joins with column references
git_pull_each(repo_path VARCHAR) → TABLE

-- With ref parameter
git_pull_each(repo_path VARCHAR, ref VARCHAR) → TABLE

-- With options
git_pull_each(repo_path VARCHAR, ref VARCHAR, options STRUCT) → TABLE
```

**Key capability**: All parameters can be column references in LATERAL joins:
```sql
-- Pull multiple repositories
WITH repos AS (
    SELECT repo_path, branch FROM project_repos
)
SELECT * FROM repos r, LATERAL git_pull_each(r.repo_path, r.branch);

-- Conditional pull strategies
WITH repo_configs AS (
    SELECT 
        repo_path,
        CASE 
            WHEN is_production THEN {'strategy': 'fast_forward_only'}
            ELSE {'strategy': 'merge'}
        END as options
    FROM repositories
)
SELECT * FROM repo_configs r, LATERAL git_pull_each(r.repo_path, NULL, r.options);
```

**Returns:** Same schema as `git_pull`

## Git URI Support

Both functions support git:// URIs for consistency with other git_* functions:
```sql
-- Pull using git:// URI
SELECT * FROM git_pull('git:///path/to/repo@main');

-- Pull with current directory
SELECT * FROM git_pull('.');

-- Pull with relative path
SELECT * FROM git_pull('../other-repo', 'develop');
```

## Implementation Details

### Pull Workflow
1. **Open repository**: Use `git_repository_open()`
2. **Fetch remote**: Use `git_remote_fetch()`
3. **Analyze merge**: Use `git_merge_analysis()` to determine action needed
4. **Perform update**:
   - Fast-forward: `git_reference_set_target()`
   - Merge: `git_merge()` + `git_commit_create()`
   - Rebase: `git_rebase_init()` + `git_rebase_next()` + `git_rebase_finish()`
5. **Update working directory**: `git_checkout_head()`
6. **Return results**: Include commit counts, file changes, etc.

### Error Handling
1. **Repository not found**: Clear error about missing repository
2. **No upstream configured**: Suggest setting upstream branch
3. **Merge conflicts**: Return conflict status with list of conflicted files
4. **Network errors**: Report connection failures with remote
5. **Authentication required**: Handle private repos (future: token support)
6. **Uncommitted changes**: Option to autostash or error

### Special Cases
1. **Detached HEAD**: Error with helpful message
2. **No remote**: Error suggesting to add remote
3. **Multiple remotes**: Use 'origin' by default or specified remote
4. **Diverged branches**: Follow strategy option (merge/rebase/fail)

## Example Usage

```sql
-- Simple pull from origin
SELECT * FROM git_pull('/path/to/repo');

-- Pull specific branch
SELECT * FROM git_pull('.', 'main');

-- Pull with fast-forward only (safe for production)
SELECT * FROM git_pull(
    '/production/repo',
    'main',
    {'strategy': 'fast_forward_only'}
);

-- Pull multiple repositories
WITH repos AS (
    VALUES 
    ('/repos/project1', 'main'),
    ('/repos/project2', 'develop'),
    ('/repos/project3', NULL)  -- Use default branch
)
SELECT 
    repo_path,
    action,
    commits_added,
    files_changed
FROM repos r(path, branch), 
     LATERAL git_pull_each(r.path, r.branch);

-- Update all repositories in a directory
WITH repo_list AS (
    SELECT DISTINCT repo_path 
    FROM git_tree('.')
    WHERE repo_path IS NOT NULL
)
SELECT * FROM repo_list r, LATERAL git_pull_each(r.repo_path);

-- Pull and analyze changes
WITH pulled AS (
    SELECT * FROM git_pull('/data/repo')
)
SELECT 
    p.*,
    COUNT(*) as new_files
FROM pulled p, 
     LATERAL git_log(p.repo_path, p.previous_hash || '..' || p.current_hash) l
WHERE p.status = 'success'
GROUP BY p.repo_path;

-- Conditional pull based on age
WITH repos_to_update AS (
    SELECT 
        repo_path,
        commit_time,
        CASE 
            WHEN commit_time < CURRENT_TIMESTAMP - INTERVAL '1 day' 
            THEN true 
            ELSE false 
        END as needs_update
    FROM git_log('.') 
    LIMIT 1
)
SELECT * FROM git_pull('.')
WHERE (SELECT needs_update FROM repos_to_update);
```

## Integration with git_clone

The `git_clone` function can leverage `git_pull` internally:
```sql
-- Pseudocode for git_clone behavior
IF directory_exists AND is_git_repo THEN
    -- Use git_pull to update existing repo
    RETURN git_pull(directory, ref, options)
ELSE
    -- Fresh clone
    RETURN git_clone_internal(url, directory, options)
END IF
```

## Testing Strategy

### Unit Tests
```sql
# Test basic pull
statement ok
SELECT * FROM git_pull('/test/repo');

# Test pull with specific branch
statement ok  
SELECT * FROM git_pull('/test/repo', 'develop');

# Test up-to-date repository
query II
SELECT status, action FROM git_pull('/test/repo');
----
success	up_to_date

# Test fast-forward
query III
SELECT status, action, commits_added > 0 as has_commits
FROM git_pull('/test/repo', 'main');
----
success	fast_forwarded	true
```

### Integration Tests
```sql
# Clone Duck Tails, make changes, then pull
WITH cloned AS (
    SELECT * FROM git_clone('https://github.com/teaguesterling/duck_tails.git', '/tmp/dt_pull_test')
)
SELECT * FROM git_pull('/tmp/dt_pull_test', 'main');

# Test git_pull_each with multiple repos
WITH repos AS (
    VALUES
    ('/tmp/repo1', 'main'),
    ('/tmp/repo2', 'develop')
)
SELECT COUNT(*) as pulled
FROM repos r(path, branch),
     LATERAL git_pull_each(r.path, r.branch)
WHERE status = 'success';
```

## File Structure

If implementing separately:
```
src/
├── git_pull.cpp           # Implementation
├── include/
│   └── git_pull.hpp       # Headers and structures
└── git_functions.cpp      # Add registration call

test/
└── sql/
    └── git_pull.test      # Comprehensive test suite
```

Or integrate into existing `git_clone.cpp` since they share similar logic.

## Dependencies

- libgit2 (already available via vcpkg)
- Functions needed:
  - `git_repository_open()`
  - `git_remote_fetch()`
  - `git_merge_analysis()`
  - `git_merge()`
  - `git_reference_set_target()`
  - `git_checkout_head()`
  - `git_rebase_*()` (for rebase strategy)

## Timeline Estimate

1. Core implementation: 2-3 hours
2. Merge/rebase strategies: 1-2 hours
3. Testing and debugging: 1-2 hours
4. Documentation updates: 30 minutes

Total: ~5-7 hours

## Open Questions

1. Should we support `git_fetch` separately (fetch without merge)?
2. How to handle merge conflicts - return conflicted file list?
3. Support for `--prune` option to remove deleted remote branches?
4. Should we expose merge commit message customization?
5. Add support for pulling specific tags?
6. Should pull work on bare repositories?

## Next Steps

1. Review and approve specification
2. Decide on implementation approach (separate or combined with git_clone)
3. Implement core pull logic
4. Add comprehensive tests
5. Update documentation
6. Consider git_fetch as separate function