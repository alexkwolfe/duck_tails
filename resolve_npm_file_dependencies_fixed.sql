-- Fixed version of resolve_npm_file_dependencies macro
-- Breaks complex LATERAL joins into simpler, sequential steps

CREATE OR REPLACE MACRO resolve_npm_file_dependencies(source_repo_uri, source_repo_id) AS TABLE (
  WITH
    -- Step 1: Get basic dependencies (no LATERAL joins yet)
    basic_deps AS (
      SELECT *
      FROM resolve_file_dependencies(source_repo_uri, source_repo_id)
    ),

    -- Step 2: Get package-lock content separately
    package_lock AS (
      SELECT content
      FROM text_files(source_repo_uri, source_repo_id)
      WHERE path = 'package-lock.json'
      LIMIT 1
    ),

    -- Step 3: Extract versions using a simple CROSS JOIN
    dependency_versions AS (
      SELECT
        bd.source_repo_id,
        bd.source_path,
        bd.source_commit_hash,
        bd.dependency_package_name,
        bd.dependency_local_path,
        bd.import_statement,
        bd.dependency_type,
        extract_package_version(pl.content, bd.dependency_package_name) as dependency_version
      FROM basic_deps bd
      CROSS JOIN package_lock pl
      WHERE bd.dependency_local_path IS NOT NULL
    ),

    -- Step 4: Simple table to iterate over for version resolution
    deps_with_versions AS (
      SELECT *
      FROM dependency_versions
      WHERE dependency_version IS NOT NULL
    )

  -- Final SELECT with simplified LATERAL joins
  SELECT
    dwv.source_repo_id,
    dwv.source_path,
    dwv.source_commit_hash,
    dwv.dependency_package_name as dependency_repo_id,
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
    END as dependency_path,
    rvtc.commit_hash as dependency_commit_hash,
    dwv.dependency_package_name,
    dwv.dependency_version,
    rvtc.tag_name as resolved_tag,
    dwv.import_statement,
    dwv.dependency_type
  FROM deps_with_versions dwv
  CROSS JOIN LATERAL resolve_version_to_commit(dwv.dependency_local_path, dwv.dependency_version) rvtc
  CROSS JOIN LATERAL git_read_each(dwv.dependency_local_path || '/package.json', rvtc.commit_hash) gr
  WHERE rvtc.commit_hash IS NOT NULL
    AND gr.text IS NOT NULL
);