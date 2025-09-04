# GIT_URI Custom Type Design

## Overview
Implement a custom `GIT_URI` type in DuckDB to provide type safety and semantic meaning for git URIs.

## Benefits of Custom Type

### 1. Type Safety
- Validate URI format at insert/cast time
- Prevent invalid URIs from being stored
- Clear semantic meaning in schemas

### 2. Automatic Parsing
- Parse components once, store efficiently
- Direct access to repo_path, file_path, and revision
- No repeated string parsing

### 3. Better User Experience
```sql
-- Current (string-based)
CREATE TABLE git_files (
    uri VARCHAR  -- Could be anything
);

-- With custom type
CREATE TABLE git_files (
    uri GIT_URI  -- Clearly a git URI, validated
);
```

### 4. Optimized Storage
- Store parsed components instead of full string
- Potentially compress common prefixes (git://)
- Index-friendly structure

## Implementation Approaches

### Structured Type 
Create GIT_URI as a STRUCT with parsed components:

```cpp
// Define as struct type (simplified view - actual implementation uses sub-types)
LogicalType GIT_URI = LogicalType::STRUCT({
    {"uri", LogicalType::VARCHAR},
    {"repo_path", LogicalType::VARCHAR},
    {"file_path", LogicalType::VARCHAR},
    {"ref", GIT_REF_TYPE},  // Sub-type for reference information
    {"repo_sha", LogicalType::VARCHAR},
    {"blob_sha", LogicalType::VARCHAR}
});
```

### 1. Type Definitions

#### GIT_REF Sub-Type
First, define a sub-type for the git reference portion (after @):

```sql
CREATE TYPE GIT_REF AS STRUCT(
    ref_string VARCHAR,          -- Original string (e.g., "HEAD~1", "v1.0..v2.0")
    ref_type VARCHAR,            -- 'simple', 'relative', 'range_two_dot', 'range_three_dot'
    base_ref VARCHAR,            -- Base reference (e.g., "HEAD" from "HEAD~1", "v1.0" from "v1.0..v2.0")
    modifier VARCHAR,            -- Modifier (e.g., "~1" from "HEAD~1", NULL for simple refs)
    target_ref VARCHAR,          -- For ranges: target reference (e.g., "v2.0" from "v1.0..v2.0")
    commit_sha VARCHAR,          -- Resolved commit SHA (when available from git functions)
    base_sha VARCHAR,            -- For ranges: base commit SHA
    target_sha VARCHAR           -- For ranges: target commit SHA
);
```

#### GIT_URI Main Type
```sql
CREATE TYPE GIT_URI AS STRUCT(
    uri VARCHAR,                 -- Constructed URI string (e.g., 'git://repo/file.txt@HEAD')
    repo_path VARCHAR,           -- Absolute path to repository (resolved from discovery)
    file_path VARCHAR,           -- Path within repository (can be empty)
    ref GIT_REF,                 -- Parsed reference information
    repo_sha VARCHAR,            -- Repository identifier hash (when available)
    blob_sha VARCHAR             -- File blob SHA (when available from git functions)
);
```

**Note on the `uri` field**: This contains the normalized/constructed URI string, not necessarily the original input. For example:
- Input: `git_uri('.', 'file.txt', 'HEAD')`
- `uri` field: `'git:///absolute/path/to/repo/file.txt@HEAD'` (after repository discovery)
- This ensures the URI is always valid and can be used with other git functions

### 2. Parser Functions

#### Parse GIT_REF
```cpp
GIT_REF ParseGitRef(const string &ref_string) {
    GIT_REF result;
    result.ref_string = ref_string;
    
    // Check for range operators
    size_t three_dot = ref_string.find("...");
    size_t two_dot = ref_string.find("..");
    
    if (three_dot != string::npos) {
        result.ref_type = "range_three_dot";
        result.base_ref = ref_string.substr(0, three_dot);
        result.target_ref = ref_string.substr(three_dot + 3);
    } else if (two_dot != string::npos) {
        result.ref_type = "range_two_dot";
        result.base_ref = ref_string.substr(0, two_dot);
        result.target_ref = ref_string.substr(two_dot + 2);
    } else if (ref_string.find("~") != string::npos || ref_string.find("^") != string::npos) {
        // Parse relative refs like HEAD~1, main^2
        result.ref_type = "relative";
        size_t mod_pos = ref_string.find_first_of("~^");
        result.base_ref = ref_string.substr(0, mod_pos);
        result.modifier = ref_string.substr(mod_pos);
    } else {
        result.ref_type = "simple";
        result.base_ref = ref_string;
    }
    
    return result;
}
```

### 3. Constructor Functions

#### Scalar Functions (for single URIs)
```sql
-- Overload 1: Pass through existing GIT_URI
SELECT git_uri(existing_uri::GIT_URI);  -- Returns: GIT_URI unchanged

-- Overload 2: Parse git:// URI with optional ref override
SELECT git_uri('git://repo/file.txt@main');  -- Returns: GIT_URI struct
SELECT git_uri('git://repo/file.txt@main', 'feature');  -- Returns: GIT_URI with ref='feature'
SELECT git_uri('git://repo/file.txt@main', 'feature'::GIT_REF);  -- Returns: GIT_URI with GIT_REF

-- Overload 3: Filesystem path with ref
SELECT git_uri('/path/to/repo/file.txt', 'HEAD');  -- Returns: GIT_URI struct
SELECT git_uri('/path/to/repo/file.txt', 'v1.0..v2.0'::GIT_REF);  -- Returns: GIT_URI with range

-- Overload 4: Three components (legacy compatibility)
SELECT git_uri('repo', 'file.txt', 'HEAD');  -- Returns: GIT_URI struct

-- Access the string representation
SELECT git_uri('/path/to/file', 'main').uri;  -- Returns: 'git:///path/to/repo/file@main'

-- Cast to VARCHAR when needed
SELECT git_uri('/path/to/file', 'HEAD')::VARCHAR;  -- Returns: 'git:///path/to/repo/file@HEAD'
```

**Signature Summary**:
```cpp
// Overload signatures matching git function patterns
git_uri(uri::GIT_URI) → GIT_URI                           // Pass through
git_uri(uri::VARCHAR) → GIT_URI                           // Parse git:// URI
git_uri(uri::VARCHAR, ref::VARCHAR) → GIT_URI             // URI with ref override
git_uri(uri::VARCHAR, ref::GIT_REF) → GIT_URI             // URI with GIT_REF override
git_uri(fs_path::VARCHAR, ref::VARCHAR) → GIT_URI         // Filesystem path + ref
git_uri(fs_path::VARCHAR, ref::GIT_REF) → GIT_URI         // Filesystem path + GIT_REF
git_uri(repo::VARCHAR, file::VARCHAR, ref::VARCHAR) → GIT_URI  // Three components

// Ref override behavior:
// - If URI contains @ref and ref parameter provided, parameter takes precedence
// - If URI lacks @ref and ref parameter provided, ref is added
// - Warning issued if refs conflict (e.g., 'git://file@main' with ref='feature')
```

#### Table Functions (for bulk operations with resolution)
```sql
-- Same signature patterns as scalar function, but returns table with resolved hashes
SELECT * FROM git_uri_each(existing_uri::GIT_URI);
SELECT * FROM git_uri_each('git://repo/file.txt@main');
SELECT * FROM git_uri_each('git://repo/file.txt@main', 'feature');  -- Override ref
SELECT * FROM git_uri_each('/path/to/file.txt', 'HEAD');

-- Column reference support for batch operations
SELECT * FROM git_uri_each(
    SELECT uri_column FROM my_table
);

SELECT * FROM git_uri_each(
    SELECT path_column, ref_column FROM my_table
);

SELECT * FROM git_uri_each(
    SELECT repo_col, file_col, ref_col FROM my_table  -- Three column version
);

-- Returns table with columns:
-- - uri: GIT_URI struct with all hashes populated
-- - commit_sha: The resolved commit SHA (convenience column)
-- - blob_sha: The file blob SHA if applicable (convenience column)

-- Example: Resolve multiple URIs with LATERAL join
WITH uris AS (
    SELECT 
        'git://repo/file1.txt@main'::VARCHAR as uri_path,  -- VARCHAR
        'data1'::VARCHAR as data
    UNION ALL
    SELECT 
        'git://repo/file2.txt@feature'::VARCHAR as uri_path,  -- VARCHAR
        'data2'::VARCHAR as data
)
SELECT 
    u.data,           -- VARCHAR from table
    r.*               -- GIT_URI with resolved hashes from function
FROM uris u           -- u is table alias, not a GIT_URI
JOIN LATERAL git_uri_each(u.uri_path, 'v1.0') r ON TRUE;  -- Override all to v1.0

-- Example: Process existing table with mixed paths and URIs
SELECT r.*            -- GIT_URI with resolved hashes
FROM file_references f
JOIN LATERAL git_uri_each(
    f.path,           -- VARCHAR: filesystem path or git:// URI
    f.ref             -- VARCHAR: git ref like 'HEAD' or 'main'
) r ON TRUE
WHERE f.needs_resolution;

-- Example: Process with GIT_URI column
SELECT r.*            -- GIT_URI with resolved hashes
FROM git_operations o
JOIN LATERAL git_uri_each(
    o.uri             -- GIT_URI: already a struct, needs hash resolution
) r ON TRUE;
```

### 4. Cast Functions
```sql
-- String to GIT_URI (with validation and parsing)
CREATE CAST (VARCHAR AS GIT_URI) 
    WITH FUNCTION parse_git_uri;

-- GIT_URI to String
CREATE CAST (GIT_URI AS VARCHAR) 
    WITH FUNCTION git_uri_to_string;

-- String to GIT_REF
CREATE CAST (VARCHAR AS GIT_REF)
    WITH FUNCTION parse_git_ref;
```

### 5. Accessor Functions
```sql
-- Direct struct access
SELECT uri.repo_path FROM my_table;
SELECT uri.file_path FROM my_table;
SELECT uri.ref.ref_string FROM my_table;
SELECT uri.ref.ref_type FROM my_table;

-- Helper functions for common queries
SELECT git_uri_is_range(uri) FROM my_table;  -- Returns TRUE for range refs
SELECT git_uri_base_ref(uri) FROM my_table;  -- Returns base reference
SELECT git_uri_target_ref(uri) FROM my_table; -- Returns target for ranges, NULL otherwise
```

### 6. Comparison and Query Operators
```sql
-- Equality comparison
SELECT * WHERE uri1 = uri2;

-- Component matching
SELECT * WHERE uri.repo_path = '/specific/repo';
SELECT * WHERE uri.ref.ref_type = 'range_two_dot';

-- Find all URIs pointing to a specific branch
SELECT * WHERE uri.ref.base_ref = 'main';

-- Find all range queries
SELECT * WHERE uri.ref.ref_type IN ('range_two_dot', 'range_three_dot');
```

## Usage Examples

### Creating Tables
```sql
CREATE TABLE git_operations (
    id INTEGER,
    source_uri GIT_URI,
    operation VARCHAR,
    timestamp TIMESTAMP
);
```

### Inserting Data
```sql
-- Automatic parsing and validation
INSERT INTO git_operations VALUES 
    (1, 'git://repo/file.txt@main', 'read', now()),
    (2, 'git://repo/dir/file.py@v1.0', 'write', now());

-- Would fail validation
INSERT INTO git_operations VALUES 
    (3, 'not-a-git-uri', 'read', now());  -- ERROR: Invalid GIT_URI format
```

### Querying with Sub-Types
```sql
-- Find all operations on a specific repo
SELECT * FROM git_operations 
WHERE source_uri.repo_path = 'repo';

-- Find all operations on main branch (exact match)
SELECT * FROM git_operations 
WHERE source_uri.ref.base_ref = 'main' 
  AND source_uri.ref.ref_type = 'simple';

-- Find all operations that involve ranges
SELECT * FROM git_operations 
WHERE source_uri.ref.ref_type LIKE 'range_%';

-- Find operations between specific versions
SELECT * FROM git_operations 
WHERE source_uri.ref.base_ref = 'v1.0' 
  AND source_uri.ref.target_ref = 'v2.0';

-- Join operations on the same base branch
SELECT o1.*, o2.*
FROM git_operations o1
JOIN git_operations o2 
  ON o1.source_uri.repo_path = o2.source_uri.repo_path
  AND o1.source_uri.ref.base_ref = o2.source_uri.ref.base_ref;
```

### With Git Functions
```sql
-- Functions could accept GIT_URI type
SELECT * FROM git_read(source_uri) 
FROM git_operations 
WHERE id = 1;

-- Type safety in function signatures
CREATE FUNCTION process_git_file(uri GIT_URI) 
RETURNS TABLE(...);
```

## Integration with Existing Functions

### Update Function Signatures
```cpp
// Current
TableFunction git_read({LogicalType::VARCHAR}, ...);

// With GIT_URI type  
TableFunction git_read({GIT_URI_TYPE}, ...);
// Also keep VARCHAR overload for compatibility
```

### Update Return Types
```cpp
// git_tree returns GIT_URI with populated hashes
DefineGitTreeSchema(...) {
    return_types = {
        LogicalType::VARCHAR,    // repo_path
        // ...
        GIT_URI_TYPE            // git_file_uri with commit_sha and blob_sha
    };
}

// When creating the URI in git_tree
GIT_URI uri;
uri.uri = ConstructGitUri(repo_path, file_path, commit_sha);
uri.repo_path = repo_path;
uri.file_path = file_path;
uri.ref.ref_string = commit_sha;
uri.ref.ref_type = "simple";
uri.ref.base_ref = commit_sha;
uri.ref.commit_sha = commit_sha;  // Already have this from tree walk
uri.blob_sha = blob_hash;          // Already have this from tree entry
```

## Implementation Steps

### Phase 1: Basic Type and Functions
1. Define GIT_URI and GIT_REF as struct types
2. Update `git_uri()` to return GIT_URI struct (without hashes)
3. Add `git_uri_each()` table function returning GIT_URI (with resolved hashes)
4. Implement parse_git_uri cast function
5. Add GIT_URI to VARCHAR cast for string representation
6. Add validation logic
7. Create tests

### Phase 2: Integration
1. Add GIT_URI overloads to existing git functions
2. Update git_tree to optionally return GIT_URI column with hashes
3. Update git_log to optionally return GIT_URI with commit hashes
4. Ensure backward compatibility with VARCHAR versions

### Phase 3: Optimization
1. Add comparison operators
2. Optimize storage if needed
3. Add indexing support
4. Consider caching strategies for resolved hashes

## Testing

### Validation Tests
```sql
-- Valid URIs with different ref types
SELECT 'git://repo/file@HEAD'::GIT_URI;           -- Simple ref
SELECT 'git://repo/file@HEAD~1'::GIT_URI;         -- Relative ref
SELECT 'git://repo/file@v1.0..v2.0'::GIT_URI;     -- Two-dot range
SELECT 'git://repo/file@main...feature'::GIT_URI;  -- Three-dot range

-- Invalid URIs  
SELECT 'not-a-uri'::GIT_URI;                      -- ERROR: Invalid format
SELECT 'git://missing-at'::GIT_URI;               -- ERROR: Missing @ separator
```

### Component Access Tests
```sql
-- Test simple ref
WITH test AS (
    SELECT 'git://my-repo/path/file.txt@v1.0'::GIT_URI as uri
)
SELECT 
    uri.repo_path = 'my-repo',
    uri.file_path = 'path/file.txt',
    uri.ref.ref_string = 'v1.0',
    uri.ref.ref_type = 'simple',
    uri.ref.base_ref = 'v1.0'
FROM test;

-- Test relative ref
WITH test AS (
    SELECT 'git://repo/file.txt@HEAD~2'::GIT_URI as uri
)
SELECT 
    uri.ref.ref_type = 'relative',
    uri.ref.base_ref = 'HEAD',
    uri.ref.modifier = '~2'
FROM test;

-- Test range ref
WITH test AS (
    SELECT 'git://repo/file.txt@v1.0..v2.0'::GIT_URI as uri
)
SELECT 
    uri.ref.ref_type = 'range_two_dot',
    uri.ref.base_ref = 'v1.0',
    uri.ref.target_ref = 'v2.0'
FROM test;
```

### Performance Tests
- Compare parsing overhead
- Measure storage impact
- Benchmark query performance

## Benefits of Sub-Type Approach

### 1. Semantic Clarity
The GIT_REF sub-type makes it clear what portion of the URI represents the git reference, improving code readability and maintainability.

### 2. Enhanced Query Capabilities
With structured ref information, queries become more powerful:
- Easily filter by ref type (simple, relative, range)
- Query specific range types (two-dot vs three-dot)
- Join on base references across different URI variations

### 3. Validation at Multiple Levels
- URI-level validation ensures overall format correctness
- REF-level validation ensures git reference syntax is valid
- Can add ref-type-specific validation rules

### 4. Future Extensibility
The sub-type approach allows for future enhancements:
- Support additional git ref syntax (e.g., @{yesterday})
- Add ref-specific metadata (is_tag, is_branch, etc.)
- Provide resolution functions that convert refs to SHAs

## Implementation Considerations

### Hash Population by Git Functions
When git functions return GIT_URI values, they can populate the hash fields since they already have repository access:

```sql
-- git_tree returns GIT_URIs with populated hashes
SELECT 
    git_file_uri,                    -- Full GIT_URI struct
    git_file_uri.ref.commit_sha,     -- Already resolved by git_tree
    git_file_uri.blob_sha            -- Blob hash from the tree entry
FROM git_tree('HEAD');

-- git_log could return URIs with commit SHAs
SELECT 
    commit_uri,                      -- GIT_URI for the commit
    commit_uri.ref.commit_sha        -- The actual commit SHA
FROM git_log();

-- Functions that read files can populate blob SHAs
SELECT 
    source_uri,
    source_uri.blob_sha              -- SHA of the file content
FROM git_read('git://repo/file.txt@main');
```

### Dynamic Resolution for User-Created URIs
User-created URIs would use the `_each` pattern for resolution:
```sql
-- User creates URIs without hashes using scalar function
INSERT INTO git_operations (uri) VALUES 
    (git_uri('repo', 'file.txt', 'main')),
    (git_uri('repo', 'other.txt', 'v1.0'));

-- Resolve using git_uri_each with LATERAL join
SELECT 
    o.id,
    o.operation,
    r.*  -- Fully resolved GIT_URI with all hashes
FROM git_operations o
JOIN LATERAL git_uri_each(
    o.uri.repo_path, 
    o.uri.file_path, 
    o.uri.ref.ref_string
) r ON TRUE
WHERE o.uri.ref.commit_sha IS NULL;

-- Or resolve in batch
WITH unresolved AS (
    SELECT repo_path, file_path, ref.ref_string as ref
    FROM git_operations
    WHERE uri.ref.commit_sha IS NULL
)
SELECT * FROM git_uri_each(unresolved);

-- Note: You cannot update struct fields in-place in DuckDB
-- Resolution always creates new structs
```

**Key Insight**: Git functions that already access the repository can populate hash fields at creation time, providing immediate access to resolved values without additional repository lookups.

### Type Conversion Functions
```sql
-- Extract just the ref portion
SELECT git_uri_to_ref('git://repo/file@v1.0..v2.0') AS ref;
-- Returns: GIT_REF with parsed components

-- Construct URI from components
SELECT make_git_uri_from_parts(
    'repo', 
    'file.txt',
    make_git_ref_range('v1.0', 'v2.0', 'two_dot')
) AS uri;
```

## Open Questions

1. **Storage Format**: Should we store the full URI string + components, or just components?
   - Recommendation: Store both for flexibility (display vs query optimization)

2. **Validation Strictness**: How strict should validation be? Allow partial URIs?
   - Recommendation: Strict for GIT_URI, but provide separate functions for partial parsing

3. **Backwards Compatibility**: How to handle existing code expecting VARCHAR?
   - Recommendation: Provide automatic casting from GIT_URI to VARCHAR via the `.uri` field

4. **NULL Handling**: How to handle NULL components (empty file_path, etc.)?
   - Recommendation: Allow NULL file_path for repo-level operations

5. **Case Sensitivity**: Should repo_path be case-sensitive?
   - Recommendation: Preserve case but provide case-insensitive comparison operators

6. **Ref Resolution**: When should refs be resolved to SHAs?
   - Recommendation: At function return time for git functions, via explicit table/scalar functions for user-created URIs
   
7. **Struct Immutability**: How to handle the fact that DuckDB structs are immutable?
   - Recommendation: Resolution functions return new structs rather than modifying existing ones

## Alternative: Domain Type
Instead of a full custom type, we could use a DOMAIN (if DuckDB supports it):

```sql
CREATE DOMAIN GIT_URI AS VARCHAR
    CHECK (validate_git_uri(VALUE));
```

This provides validation but not component access.