# Refactoring Danger Zones: Ghost Bug Prevention

## Overview

This document identifies **critical consistency points** where making changes in one place during URI schema work could introduce subtle differences that create ghost bugs. These are the places where common functionality is implemented multiple times and **MUST** stay identical.

## Danger Zone 1: OID to Hex Conversion

**Risk:** Different hex formatting could cause hash mismatches between functions.

### Current Implementations (14 instances):

#### Pattern A: Direct `git_oid_tostr` (10 instances)
```cpp
// git_log function (line 266, 296)
char hash_str[GIT_OID_HEXSZ + 1];
git_oid_tostr(hash_str, sizeof(hash_str), &oid);

// git_branches function (line 383, 489)  
char hash_str[GIT_OID_HEXSZ + 1];
git_oid_tostr(hash_str, sizeof(hash_str), oid);

// git_log_each function (lines 1215, 1242, 1429, 1623)
char hash_str[GIT_OID_HEXSZ + 1];
git_oid_tostr(hash_str, sizeof(hash_str), &commit_oid);

// git_parents_each function (lines 2126, 2138)
char commit_oid_str[GIT_OID_HEXSZ + 1];
git_oid_tostr(commit_oid_str, sizeof(commit_oid_str), git_commit_id(commit));
```

#### Pattern B: Via `oid_to_hex()` helper (4 instances)
```cpp
// git_tree function (lines 720, 727)
commit_hash = oid_to_hex(git_object_id(obj));
commit_hash = oid_to_hex(&oid);

// git_tree traverse_tree (line 641)
out.push_back(GitTreeRow{..., oid_to_hex(oid), ...});

// git_tree ProcessSingleCommit (line 867)
string commit_hash = oid_to_hex(&oid);

// git_parents function (lines 1117, 1118)
rows.push_back(GitParentsRow{
    oid_to_hex(&oid), 
    oid_to_hex(parent_oid), 
    ...
});
```

### **CRITICAL CONSISTENCY REQUIREMENT:**

**All OID conversions MUST produce identical output:**
```cpp
// This helper function exists (line 545):
static string oid_to_hex(const git_oid *oid) {
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    return string(hex);
}
```

**Ghost Bug Risk:** If we change OID formatting in one place (e.g., add prefixes, change case, truncate), other functions will return different hash values.

**Safe Practice During URI Work:**
- **Never modify `oid_to_hex()`** function signature or output format
- **Never change `git_oid_tostr` buffer size** in any location
- **Test hash consistency** across functions after any changes:
```sql
-- Verify same file produces same hashes across functions
SELECT DISTINCT 
  t.blob_hash as tree_hash,
  p.commit_hash as parents_hash,
  l.commit_hash as log_hash
FROM git_tree('HEAD') t
JOIN git_parents('HEAD') p ON t.commit_hash = p.commit_hash  
JOIN git_log('HEAD') l ON t.commit_hash = l.commit_hash
LIMIT 1;
-- All hash columns should be identical for same commits
```

## Danger Zone 2: Git URI Construction

**Risk:** Inconsistent URI format could break URI parsing/comparison.

### Current Implementations:

#### Pattern A: ConstructGitUri function (used in git_tree)
```cpp
// From git_functions.cpp - line ~150
static string ConstructGitUri(const string &repo_path, const string &file_path, const string &revision) {
    string uri = "git://" + repo_path;
    // ... specific construction logic
}
```

#### Pattern B: Manual URI construction (if any exist)
**Need to verify:** Are there other places manually building git:// URIs?

### **CRITICAL CONSISTENCY REQUIREMENT:**

**All git:// URIs MUST follow identical format:**
- Same prefix (`git://`)
- Same path separator handling
- Same revision format (`@revision`)
- Same empty component handling

**Ghost Bug Risk:** Functions returning different URI formats for same file could break:
- URI parsing in other functions
- JOIN operations between URI-returning functions
- User queries expecting consistent format

**Safe Practice During URI Work:**
- **Always use `ConstructGitUri()`** - never build URIs manually
- **Never modify URI format** without updating all consumers
- **Test URI format consistency:**
```sql
-- Verify URI format consistency
SELECT DISTINCT 
  SUBSTRING(git_uri, 1, 6) as uri_prefix,
  POSITION('@' IN git_uri) > 0 as has_revision
FROM git_tree('HEAD') 
LIMIT 5;
-- All should have 'git://' prefix and '@' revision separator
```

## Danger Zone 3: Error Handling Patterns

**Risk:** Inconsistent error messages/types could break error handling assumptions.

### Current Error Patterns:
```bash
# Found via grep:
rg -n "IOException.*git|git_error_last" src/
```

**Need to map:** What are the current error handling patterns across functions?

### **CRITICAL CONSISTENCY REQUIREMENT:**

**All git operations MUST handle errors consistently:**
- Same exception types for same error conditions
- Consistent error message format
- Same cleanup/resource handling

**Ghost Bug Risk:** Different error handling could cause:
- Uncaught exceptions in some code paths
- Resource leaks in error conditions  
- Inconsistent user error messages

## Danger Zone 4: Repository Path Resolution

**Risk:** Different path resolution could cause same repository to appear as different repositories.

### Current GitPath Usage:
```bash
# Found via grep:
rg -A5 -B5 "GitPath::Parse" src/
```

**Need to verify:** How does GitPath resolve relative paths, handle `.git` discovery, etc.?

### **CRITICAL CONSISTENCY REQUIREMENT:**

**Repository path resolution MUST be identical:**
- Same relative path handling
- Same `.git` discovery logic
- Same absolute path normalization

**Ghost Bug Risk:** Same repository appearing as different repos could cause:
- Duplicate entries in results
- JOIN failures between functions
- Cache misses/inefficiencies

## Testing Strategy: Ghost Bug Detection

### Before Any URI Schema Changes:
```sql
-- Capture baseline outputs
CREATE TABLE baseline_hashes AS 
SELECT 'git_log' as source, commit_hash, tree_hash FROM git_log('HEAD') LIMIT 10
UNION ALL
SELECT 'git_tree' as source, commit_hash, tree_hash FROM git_tree('HEAD') LIMIT 10
UNION ALL  
SELECT 'git_parents' as source, commit_hash, '' as tree_hash FROM git_parents('HEAD') LIMIT 10;

CREATE TABLE baseline_uris AS
SELECT git_uri FROM git_tree('HEAD') LIMIT 10;
```

### After Each URI Schema Change:
```sql
-- Verify hash consistency
SELECT source, commit_hash, COUNT(*) 
FROM baseline_hashes 
GROUP BY source, commit_hash 
HAVING COUNT(*) > 1;
-- Should return empty - no duplicate hashes per source

-- Verify cross-function hash consistency
SELECT commit_hash, COUNT(DISTINCT source)
FROM baseline_hashes 
WHERE commit_hash != ''
GROUP BY commit_hash
HAVING COUNT(DISTINCT source) < 2;
-- Should return empty - same commits should appear in multiple functions

-- Verify URI format consistency
SELECT 
  CASE WHEN git_uri LIKE 'git://%@%' THEN 'valid' ELSE 'invalid' END as format_check,
  COUNT(*)
FROM git_tree('HEAD') 
GROUP BY format_check;
-- Should only show 'valid' URIs
```

## Emergency Revert Strategy

If ghost bugs are detected:

1. **Immediately revert the specific change** that broke consistency
2. **Run full baseline comparison** to identify all affected functions
3. **Fix all instances together** rather than piecemeal
4. **Re-run ghost bug detection tests** before proceeding

---

## Summary: Stay Paranoid

**The most dangerous bugs during URI schema work will be the subtle ones:**
- Functions returning slightly different hash formats
- URI construction variations
- Path resolution differences

**Prevention is cheaper than debugging:**
- Document every consistency point before starting
- Test consistency after every change
- When in doubt, don't change common functionality

*Ghost bugs in hash/URI processing can take days to debug because they're intermittent and context-dependent.*