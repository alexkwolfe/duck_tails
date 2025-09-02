# git_tree Enhancement: Array Support and git_tree_each Function

## Overview

Enhance git repository analysis with:
1. **Array support** for `git_tree()` - analyze multiple commits in a single query
2. **New `git_tree_each()` function** - LATERAL join support for dynamic commit analysis
3. **Unified schema** across both functions for seamless multi-commit workflows

This maintains clean separation between static batch operations (`git_tree`) and dynamic row-by-row operations (`git_tree_each`).

## Current Limitations

```sql
-- ❌ This DOESN'T work (static parameter limitation)
SELECT l.commit_hash, t.path, t.size
FROM git_log() l
JOIN git_tree(l.commit_hash) t ON true;  -- ERROR: dynamic parameter

-- ❌ This DOESN'T work (no array support)  
SELECT * FROM git_tree(ARRAY['HEAD', 'HEAD~1', 'HEAD~2']);  -- ERROR: unsupported

-- ❌ Tedious workaround required
SELECT 'HEAD' as commit_hash, * FROM git_tree('HEAD')
UNION ALL
SELECT 'HEAD~1' as commit_hash, * FROM git_tree('HEAD~1');
```

## Proposed Enhancements

### Enhancement 1: Array Support for git_tree()

```sql
-- ✅ Multiple commits (NEW capability)
SELECT * FROM git_tree(ARRAY['HEAD', 'HEAD~1', 'HEAD~2']);

-- ✅ Static arrays only (no dynamic subqueries)
SELECT * FROM git_tree(ARRAY['v1.0', 'v2.0', 'v3.0']);
```

### Enhancement 2: New git_tree_each() Function

```sql
-- ✅ Dynamic single commit (NEW function)
SELECT l.commit_hash, t.path, t.size
FROM git_log() l,
     LATERAL git_tree_each(l.commit_hash) t
WHERE l.author_date > '2024-01-01';

-- ✅ Dynamic commit with repository path
SELECT l.commit_hash, t.path, t.size  
FROM git_log('/path/to/repo') l,
     LATERAL git_tree_each(l.commit_hash, repo_path := '/path/to/repo') t;
```

## Implementation Design

### Enhanced git_tree() Function

```cpp
void RegisterGitTreeFunction(DatabaseInstance &db) {
    TableFunctionSet git_tree_set("git_tree");
    
    // 1. Single commit (existing)
    TableFunction git_tree_single({LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_set.AddFunction(git_tree_single);
    
    // 2. Multiple commits (NEW - array support)
    TableFunction git_tree_array({LogicalType::LIST(LogicalType::VARCHAR)}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_array.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_set.AddFunction(git_tree_array);
    
    // 3. Two-argument version (commit, repo_path)
    TableFunction git_tree_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_set.AddFunction(git_tree_two);
    
    ExtensionUtil::RegisterFunction(db, git_tree_set);
}
```

### New git_tree_each() Function

```cpp
void RegisterGitTreeEachFunction(DatabaseInstance &db) {
    TableFunctionSet git_tree_each_set("git_tree_each");
    
    // Version that takes commit_ref as first parameter (for LATERAL context)
    TableFunction git_tree_each_1({LogicalType::VARCHAR}, nullptr, GitTreeEachBind, nullptr, GitTreeEachLocalInit);
    git_tree_each_1.in_out_function = GitTreeEachFunction;  // Key difference!
    git_tree_each_1.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_each_set.AddFunction(git_tree_each_1);
    
    // Version with commit_ref and repo_path parameters  
    TableFunction git_tree_each_2({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitTreeEachBind, nullptr, GitTreeEachLocalInit);
    git_tree_each_2.in_out_function = GitTreeEachFunction;
    git_tree_each_set.AddFunction(git_tree_each_2);
    
    ExtensionUtil::RegisterFunction(db, git_tree_each_set);
}
```

### Unified Schema

```sql
-- Both git_tree() and git_tree_each() return the same columns
git_tree(...) RETURNS TABLE (
  commit_hash VARCHAR,   -- Always included for multi-commit scenarios
  commit_date TIMESTAMP, -- Added context for multi-commit  
  path VARCHAR,
  mode INTEGER,
  blob_hash VARCHAR,
  size BIGINT
);

git_tree_each(...) RETURNS TABLE (
  commit_hash VARCHAR,   -- Same schema as git_tree()
  commit_date TIMESTAMP,
  path VARCHAR,
  mode INTEGER,
  blob_hash VARCHAR,
  size BIGINT
);
```

### git_tree_each() Implementation Details

**Following git_read_each pattern:**

```cpp
// Bind data for git_tree_each
struct GitTreeEachBindData : public TableFunctionData {
    string repo_path;
    
    GitTreeEachBindData(const string& repo_path) : repo_path(repo_path) {}
};

// Local state for git_tree_each (row-by-row processing)
struct GitTreeEachLocalState : public LocalTableFunctionState {
    idx_t current_input_row = 0;
    bool initialized_row = false;
    vector<GitTreeEntry> tree_entries;
    idx_t entry_index = 0;
    
    GitTreeEachLocalState() {}
};

// LATERAL function: processes input DataChunk row-by-row
static OperatorResultType GitTreeEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                            DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitTreeEachBindData>();
    auto &state = data_p.local_state->Cast<GitTreeEachLocalState>();
    
    while (true) {
        if (!state.initialized_row) {
            // Initialize for the current input row
            if (state.current_input_row >= input.size()) {
                // Ran out of input rows
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // Extract commit_ref from input DataChunk
            input.Flatten();
            if (input.ColumnCount() == 0) {
                throw BinderException("git_tree_each: no input columns available");
            }
            
            auto data = FlatVector::GetData<string_t>(input.data[0]);
            if (!data) {
                throw BinderException("git_tree_each: no string data in input column");
            }
            
            auto string_t_value = data[state.current_input_row];
            string commit_ref(string_t_value.GetData(), string_t_value.GetSize());
            
            if (commit_ref.empty()) {
                // Skip NULL/empty commit refs
                state.current_input_row++;
                continue;
            }
            
            // Get tree entries for this commit
            state.tree_entries = GetTreeEntries(bind_data.repo_path, commit_ref);
            state.entry_index = 0;
            state.initialized_row = true;
        }
        
        // Output tree entries for current commit
        idx_t output_count = 0;
        while (state.entry_index < state.tree_entries.size() && output_count < STANDARD_VECTOR_SIZE) {
            auto &entry = state.tree_entries[state.entry_index];
            
            // Set output values
            FlatVector::GetData<string_t>(output.data[0])[output_count] = StringVector::AddString(output, entry.commit_hash);
            FlatVector::GetData<timestamp_t>(output.data[1])[output_count] = entry.commit_date;
            FlatVector::GetData<string_t>(output.data[2])[output_count] = StringVector::AddString(output, entry.path);
            FlatVector::GetData<int32_t>(output.data[3])[output_count] = entry.mode;
            FlatVector::GetData<string_t>(output.data[4])[output_count] = StringVector::AddString(output, entry.blob_hash);
            FlatVector::GetData<int64_t>(output.data[5])[output_count] = entry.size;
            
            output_count++;
            state.entry_index++;
        }
        
        if (output_count > 0) {
            output.SetCardinality(output_count);
            return OperatorResultType::HAVE_MORE_OUTPUT;
        }
        
        // Finished current commit, move to next input row
        state.current_input_row++;
        state.initialized_row = false;
    }
}
```

## Usage Examples

### 1. Single Commit (Current + LATERAL)

```sql
-- Static single commit
SELECT path, size FROM git_tree('HEAD');
SELECT path, size FROM git_tree('git:///path/to/repo@HEAD');

-- Dynamic single commit  
SELECT l.commit_hash, t.path, t.size
FROM git_log() l,
     LATERAL git_tree_each(l.commit_hash) t
WHERE l.author_date > '2024-01-01';

-- Dynamic with git:// URIs
SELECT l.commit_hash, t.path, t.size
FROM git_log('git:///path/to/repo@main') l,
     LATERAL git_tree_each('git:///path/to/repo@' || l.commit_hash) t;
```

### 2. Multiple Commits (Array)

```sql
-- Specific commits with current repo
SELECT commit_hash, path, size 
FROM git_tree(ARRAY['HEAD', 'HEAD~1', 'v1.0.0'])
WHERE path LIKE '%.cpp';

-- Specific commits with absolute repo path
SELECT commit_hash, path, size 
FROM git_tree(ARRAY['git:///path/to/repo@HEAD', 'git:///path/to/repo@HEAD~1', 'git:///path/to/repo@v1.0.0'])
WHERE path LIKE '%.cpp';

-- Path filtering with git:// URLs
SELECT commit_hash, path, size
FROM git_tree('git:///path/to/repo/src@HEAD')
ORDER BY size DESC;
```

### 3. LATERAL Join with git_tree_each()

```sql
-- Dynamic single commit analysis
SELECT l.commit_hash, t.path, t.size
FROM git_log() l,
     LATERAL git_tree_each(l.commit_hash) t
WHERE l.author_date > '2024-01-01';

-- File evolution tracking
SELECT l.commit_hash, t.path, t.size, l.author_date
FROM git_log() l,
     LATERAL git_tree_each(l.commit_hash) t 
WHERE t.path LIKE '%.cpp'
ORDER BY t.path, l.author_date;
```

## Implementation Strategy

### Parameter Type Detection

```cpp
unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
    auto &first_param = input.inputs[0];
    
    if (first_param.type().id() == LogicalTypeId::VARCHAR) {
        string param = first_param.GetValue<string>();
        // Handle single commit: "HEAD", "abc123", etc.
        return make_uniq<GitTreeSingleData>(param);
    } 
    else if (first_param.type().id() == LogicalTypeId::LIST) {
        // Handle array: ARRAY['HEAD', 'HEAD~1']
        auto commits = ListValue::GetChildren(first_param);
        return make_uniq<GitTreeArrayData>(commits);
    }
    
    throw InvalidInputError("git_tree: unsupported parameter type");
}
```

### Unified Output Schema

```cpp
// Always return the same schema
return_types = {
    LogicalType::VARCHAR,  // commit_hash
    LogicalType::VARCHAR,  // path  
    LogicalType::INTEGER,  // mode
    LogicalType::VARCHAR,  // blob_hash
    LogicalType::BIGINT,   // size
    LogicalType::TIMESTAMP // commit_date (for context)
};

names = {"commit_hash", "path", "mode", "blob_hash", "size", "commit_date"};
```

## Smart Optimizations

### 1. Automatic Batching

```cpp
// When processing arrays, batch git operations
void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitTreeBindData>();
    
    switch (bind_data.mode) {
        case GitTreeMode::SINGLE:
            ProcessSingleCommit(bind_data, output);
            break;
        case GitTreeMode::ARRAY:
            ProcessCommitArray(bind_data, output);  // Batch optimization
            break;
    }
}
```

### 2. Deduplication

```cpp
// Automatically deduplicate when same commit appears multiple times
if (bind_data.mode == GitTreeMode::ARRAY) {
    auto unique_commits = DeduplicateCommits(bind_data.commits);
    // Process unique commits only
}
```

## Benefits of Single Function Approach

### 1. Simpler API
- **One function name** to remember
- **Consistent return schema** 
- **Natural parameter polymorphism**

### 2. Better Performance
- **Shared code paths** for git operations
- **Automatic batching** for multiple commits
- **Connection reuse** across commits

### 3. Flexible Usage

```sql
-- Static operations use git_tree:
SELECT * FROM git_tree('HEAD');                                    -- Single static (current repo)
SELECT * FROM git_tree('git:///path/to/repo@HEAD');               -- Single static (absolute repo)
SELECT * FROM git_tree(ARRAY['HEAD', 'HEAD~1']);                  -- Multiple static (current repo)
SELECT * FROM git_tree('git:///path/to/repo/src@HEAD');           -- Path filtered

-- Dynamic operations use git_tree_each:
SELECT l.commit_hash, t.* FROM git_log() l,
   LATERAL git_tree_each(l.commit_hash) t;                        -- Single dynamic (current repo)
SELECT l.commit_hash, t.* FROM git_log('git:///path/to/repo@main') l,
   LATERAL git_tree_each('git:///path/to/repo@' || l.commit_hash) t;  -- Single dynamic (absolute repo)
```

## Real-World Use Cases Enabled

### Repository Growth Over Time

```sql
WITH tree_evolution AS (
  SELECT l.commit_hash, l.author_date, t.path, t.size
  FROM git_log() l,
       LATERAL git_tree_each(l.commit_hash) t
  WHERE l.author_date > '2024-01-01'
)
SELECT 
  DATE_TRUNC('week', author_date) as week,
  COUNT(DISTINCT path) as file_count,
  SUM(size) as total_size
FROM tree_evolution
GROUP BY week
ORDER BY week;
```

### Find When Files Were Added/Removed

```sql
-- Track specific file across history  
SELECT 
  l.commit_hash,
  l.author_date,
  l.message,
  CASE WHEN t.path IS NULL THEN 'deleted' ELSE 'present' END as status
FROM git_log() l
LEFT JOIN LATERAL git_tree_each(l.commit_hash) t ON (t.path = 'src/important.cpp')
WHERE l.author_date > '2024-01-01'
ORDER BY l.author_date;
```

### Repository Structure Analysis Across Multiple Commits

```sql
-- Compare file structures across recent commits using static array
SELECT 
  commit_hash,
  CASE 
    WHEN path LIKE '%.py' THEN 'Python'
    WHEN path LIKE '%.js' THEN 'JavaScript'
    WHEN path LIKE '%.cpp' OR path LIKE '%.hpp' THEN 'C++'
    ELSE 'Other'
  END as file_type,
  COUNT(*) as file_count,
  SUM(size) as total_size
FROM git_tree(ARRAY['HEAD', 'HEAD~1', 'HEAD~2', 'HEAD~3', 'HEAD~4'])
GROUP BY commit_hash, file_type
ORDER BY commit_hash, total_size DESC;

-- Or use LATERAL for dynamic analysis
SELECT 
  l.commit_hash,
  CASE 
    WHEN t.path LIKE '%.py' THEN 'Python'
    WHEN t.path LIKE '%.js' THEN 'JavaScript' 
    WHEN t.path LIKE '%.cpp' OR t.path LIKE '%.hpp' THEN 'C++'
    ELSE 'Other'
  END as file_type,
  COUNT(*) as file_count,
  SUM(t.size) as total_size
FROM (SELECT commit_hash FROM git_log() WHERE author_date > CURRENT_DATE - INTERVAL '30 days' LIMIT 10) l,
     LATERAL git_tree_each(l.commit_hash) t
GROUP BY l.commit_hash, file_type
ORDER BY l.commit_hash, total_size DESC;
```

## Migration Path

### Phase 1: Enhance Current git_tree()
- Add array support for multiple static commits
- Add `commit_hash` column to output schema
- Keep existing single commit behavior intact

### Phase 2: Add git_tree_each() Function
- Create new function for LATERAL join support
- Implement in_out_function pattern for row-by-row processing
- Enable dynamic single commit analysis

## Implementation Requirements

### Core Changes
1. **Modify GitTreeFunctionData** to support array operation mode
2. **Update bind logic** to detect parameter types (single vs array)
3. **Add commit_hash column** to output schema
4. **Implement batch processing** for arrays
5. **Create git_tree_each() function** with in_out_function for LATERAL support

### Testing Requirements
1. **Backward compatibility** - existing git_tree() queries continue to work
2. **LATERAL JOIN functionality** - git_tree_each() dynamic single commit support
3. **Array parameter handling** - multiple commits with deduplication
4. **Performance benchmarks** - ensure batching provides benefits
5. **Error handling** - invalid commits, repository paths, parameter types

### Documentation Updates
1. **README examples** showing new capabilities
2. **Test cases** demonstrating all usage patterns
3. **Performance characteristics** documentation

## Success Criteria

- **Backward compatible** - all existing `git_tree()` usage continues to work
- **LATERAL JOIN support** - git_tree_each() enables dynamic single commit queries
- **Efficient bulk operations** - array and range modes faster than individual calls
- **Intuitive API** - single function handles all use cases naturally
- **Robust error handling** - clear errors for invalid commits or parameters

This enhancement transforms `git_tree()` from a static analysis tool into a powerful, flexible repository analysis function that can handle real-world analytical workflows.