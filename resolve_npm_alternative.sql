-- Alternative: Break into multiple functions/views if LATERAL joins still fail

-- Step 1: Create a simple function for version resolution
CREATE OR REPLACE MACRO get_dependency_commits(deps_table_name) AS TABLE (
  SELECT 
    source_repo_id,
    source_path,
    source_commit_hash,
    dependency_package_name,
    dependency_local_path,
    dependency_version,
    import_statement,
    dependency_type,
    rvtc.commit_hash as dependency_commit_hash,
    rvtc.tag_name as resolved_tag
  FROM query_table(deps_table_name) dv,
       LATERAL resolve_version_to_commit(dv.dependency_local_path, dv.dependency_version) rvtc
  WHERE dv.dependency_version IS NOT NULL
);

-- Step 2: Create function for entry point resolution  
CREATE OR REPLACE MACRO get_entry_points(commits_table_name) AS TABLE (
  SELECT
    dc.*,
    CASE
      WHEN json_extract_string(gr.text, '$.main') LIKE './%' 
      THEN SUBSTR(json_extract_string(gr.text, '$.main'), 3)
      WHEN json_extract_string(gr.text, '$.main') LIKE '/%' 
      THEN SUBSTR(json_extract_string(gr.text, '$.main'), 2)
      ELSE COALESCE(json_extract_string(gr.text, '$.main'), 'index')
    END || CASE
      WHEN json_extract_string(gr.text, '$.main') NOT LIKE '%.js'
       AND json_extract_string(gr.text, '$.main') NOT LIKE '%.mjs'
       AND json_extract_string(gr.text, '$.main') NOT LIKE '%.ts'
      THEN '.js'
      ELSE ''
    END as dependency_path
  FROM query_table(commits_table_name) dc,
       LATERAL git_read_each(dc.dependency_local_path || '/package.json', dc.dependency_commit_hash) gr
  WHERE dc.dependency_commit_hash IS NOT NULL
    AND gr.text IS NOT NULL
);

-- Main function using the helper functions
CREATE OR REPLACE MACRO resolve_npm_file_dependencies_v2(source_repo_uri, source_repo_id) AS TABLE (
  WITH
    basic_deps AS (
      SELECT *
      FROM resolve_file_dependencies(source_repo_uri, source_repo_id)
    ),
    package_lock AS (
      SELECT content
      FROM text_files(source_repo_uri, source_repo_id)
      WHERE path = 'package-lock.json'
      LIMIT 1
    ),
    dependency_versions AS (
      SELECT
        bd.*,
        extract_package_version(pl.content, bd.dependency_package_name) as dependency_version
      FROM basic_deps bd
      CROSS JOIN package_lock pl
      WHERE bd.dependency_local_path IS NOT NULL
    )
  SELECT
    source_repo_id,
    source_path, 
    source_commit_hash,
    dependency_package_name as dependency_repo_id,
    dependency_path,
    dependency_commit_hash,
    dependency_package_name,
    dependency_version,
    resolved_tag,
    import_statement,
    dependency_type
  FROM get_entry_points(get_dependency_commits(dependency_versions))
);