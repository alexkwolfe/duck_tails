# Advanced Duck Tails Examples

This document contains advanced usage patterns and complex queries for Duck Tails.

## Repository Structure Analysis

```sql
-- Analyze repository file structure and sizes
SELECT 
    CASE 
        WHEN path LIKE '%.py' THEN 'Python'
        WHEN path LIKE '%.js' THEN 'JavaScript'
        WHEN path LIKE '%.cpp' OR path LIKE '%.hpp' THEN 'C++'
        ELSE 'Other'
    END as file_type,
    COUNT(*) as file_count,
    SUM(size) as total_size,
    AVG(size) as avg_file_size
FROM git_tree('HEAD')
GROUP BY 1
ORDER BY total_size DESC;

-- Find largest files in repository
SELECT path, size, blob_hash
FROM git_tree('HEAD')
WHERE size > 100000  -- Files larger than 100KB
ORDER BY size DESC;

-- Compare file structures across different repositories
SELECT 
    'main-repo' as repo_name,
    COUNT(*) as file_count,
    SUM(size) as total_size
FROM git_tree('HEAD', '/path/to/main/repo')
UNION ALL
SELECT 
    'other-repo' as repo_name,
    COUNT(*) as file_count,
    SUM(size) as total_size  
FROM git_tree('HEAD', '/path/to/other/repo');
```

## Commit Genealogy Analysis

```sql
-- Find merge commits (commits with multiple parents)
SELECT 
    p.commit_hash,
    COUNT(*) as parent_count,
    g.message,
    g.author_name
FROM git_parents('HEAD') p
JOIN git_log() g ON p.commit_hash = g.commit_hash
GROUP BY p.commit_hash, g.message, g.author_name
HAVING COUNT(*) > 1
ORDER BY parent_count DESC;

-- Trace commit ancestry paths
WITH RECURSIVE ancestry AS (
    -- Start from HEAD
    SELECT commit_hash, parent_hash, 0 as generation
    FROM git_parents('HEAD') 
    WHERE parent_index = 0  -- First parent only
    
    UNION ALL
    
    -- Follow the parent chain
    SELECT p.commit_hash, p.parent_hash, a.generation + 1
    FROM git_parents('HEAD') p
    JOIN ancestry a ON p.commit_hash = a.parent_hash
    WHERE p.parent_index = 0 AND a.generation < 10  -- Limit depth
)
SELECT * FROM ancestry ORDER BY generation;
```

## Repository Evolution Tracking

```sql
-- Repository evolution: Track how file sizes change over time
WITH file_history AS (
    SELECT 
        p.commit_hash,
        p.parent_hash,
        g.author_date,
        SUM(t.size) as total_repo_size,
        COUNT(*) as file_count
    FROM git_parents('HEAD') p
    JOIN git_log() g ON p.commit_hash = g.commit_hash
    JOIN git_tree(p.commit_hash) t
    WHERE p.parent_index = 0  -- First parent only
    GROUP BY p.commit_hash, p.parent_hash, g.author_date
)
SELECT 
    commit_hash,
    author_date,
    total_repo_size,
    file_count,
    total_repo_size - LAG(total_repo_size) OVER (ORDER BY author_date) as size_change
FROM file_history 
ORDER BY author_date DESC
LIMIT 10;

-- Find commits that introduced large changes
SELECT 
    g.commit_hash, 
    g.message,
    length(r.diff_text) as change_size
FROM git_log() g
CROSS JOIN read_git_diff('git://src/@' || g.commit_hash || '~1', 
                        'git://src/@' || g.commit_hash) r
WHERE length(r.diff_text) > 1000
ORDER BY change_size DESC
LIMIT 5;

-- Cross-reference file changes with commit structure
SELECT 
    t.path,
    t.size,
    COUNT(p.parent_hash) as times_modified_in_merges
FROM git_tree('HEAD') t
LEFT JOIN git_parents('HEAD') p ON EXISTS (
    SELECT 1 FROM git_tree(p.commit_hash) t2 WHERE t2.path = t.path
)
WHERE p.commit_hash IN (
    SELECT commit_hash FROM git_parents('HEAD') 
    GROUP BY commit_hash HAVING COUNT(*) > 1  -- Merge commits
)
GROUP BY t.path, t.size
ORDER BY times_modified_in_merges DESC;

-- Compare data schema evolution
SELECT 
    'v1.0' as version,
    column_name,
    column_type  
FROM describe(SELECT * FROM read_csv('git://data.csv@v1.0') LIMIT 0)
UNION ALL
SELECT 
    'v2.0' as version,
    column_name,
    column_type
FROM describe(SELECT * FROM read_csv('git://data.csv@v2.0') LIMIT 0);
```

## Configuration Management

```sql
-- Track configuration changes over time
SELECT 
    g.commit_hash,
    g.author_date,
    g.message,
    r.diff_text
FROM git_log() g
CROSS JOIN read_git_diff('git://config.json@' || g.commit_hash || '~1', 
                        'git://config.json@' || g.commit_hash) r
WHERE length(r.diff_text) > 0  -- Only commits that changed config
LIMIT 10;
```