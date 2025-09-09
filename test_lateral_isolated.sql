-- Test each LATERAL join individually to see which ones work
LOAD duck_tails;

-- Test 1: git_log_each alone (should work - smaller struct)
SELECT COUNT(*) FROM git_log_each('git://.@HEAD');

-- Test 2: git_tree_each alone (should work)
SELECT COUNT(*) FROM git_tree_each('git://.@HEAD');

-- Test 3: Simple git_log LATERAL (should work - 11 fields, 8 strings)
WITH data AS (SELECT 'git://.@HEAD' as uri)
SELECT COUNT(*)
FROM data d
CROSS JOIN LATERAL git_log_each(d.uri) l;

-- Test 4: Simple git_parents LATERAL (should work - 3 fields, 2 strings)
WITH data AS (SELECT 'git://.@HEAD' as uri)
SELECT COUNT(*)
FROM data d  
CROSS JOIN LATERAL git_parents_each(d.uri) p;

-- Test 5: Simple git_tree LATERAL (this is the problem - 14 fields, 10 strings)
WITH data AS (SELECT 'git://.@HEAD' as uri)
SELECT COUNT(*)
FROM data d
CROSS JOIN LATERAL git_tree_each(d.uri) t;

-- Test 6: git_tree -> git_log LATERAL (the failing case)
SELECT COUNT(*)
FROM git_tree_each('git://.@HEAD') t
CROSS JOIN LATERAL git_log_each(t.git_uri) l
LIMIT 10;