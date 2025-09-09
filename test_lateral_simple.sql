-- Minimal test case for LATERAL join issue
LOAD duck_tails;

-- Test 1: Direct git_tree call (should work)
SELECT file_path, git_uri 
FROM git_tree('HEAD', repo_path => '.')
LIMIT 3;

-- Test 2: Simple LATERAL join (potential segfault)
SELECT t.file_path, COUNT(l.commit_hash) as commit_count
FROM git_tree('HEAD', repo_path => '.') t
CROSS JOIN LATERAL (
    SELECT commit_hash 
    FROM git_log_each(t.git_uri) 
    LIMIT 1
) l
WHERE t.file_path LIKE '%.md'
GROUP BY t.file_path
LIMIT 3;

-- Test 3: Even simpler LATERAL join
SELECT t.file_path
FROM git_tree('HEAD', repo_path => '.') t
CROSS JOIN LATERAL git_log_each(t.git_uri) l
LIMIT 1;