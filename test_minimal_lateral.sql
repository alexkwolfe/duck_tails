-- Minimal test to isolate LATERAL join issue
LOAD duck_tails;

-- Test 1: Direct git_tree_each call (should work)
SELECT COUNT(*) FROM git_tree_each('git://.@HEAD');

-- Test 2: Simple LATERAL join without WHERE clause
SELECT COUNT(*)
FROM git_tree_each('git://.@HEAD') t
CROSS JOIN LATERAL git_log_each(t.git_uri) l;

-- Test 3: Even simpler - just one row
SELECT t.file_path, l.commit_hash
FROM git_tree_each('git://.@HEAD') t
CROSS JOIN LATERAL git_log_each(t.git_uri) l
LIMIT 1;