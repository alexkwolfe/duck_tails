# AddressSanitizer Memory Corruption Issue

## Executive Summary
After resolving the initial LATERAL join memory safety issue documented in `lateral-join-issue.md`, new AddressSanitizer crashes have emerged with the same characteristic `0xbebebebebebebebe` pattern, but affecting different functions and scenarios. This represents a **separate** memory corruption issue that requires different fixes.

## How This Differs From lateral-join-issue.md

### Previous Issue (lateral-join-issue.md)
- **Root Cause**: Uninitialized struct fields in `GitTreeRow` and `GitReadLocalState::ReadResult`
- **Trigger**: LATERAL joins with git functions in test framework
- **Pattern**: Uninitialized struct constructors causing random memory access
- **Solution**: Added proper constructors to initialize all fields
- **Status**: ✅ **RESOLVED** - Fixed by adding constructors and proper initialization
- **Functions Affected**: Primarily git_tree and git_read functions

### Current Issue (This Document)
- **Root Cause**: Uninitialized memory in output vector handling across multiple git functions
- **Trigger**: Any git function call that outputs string vectors (both direct and LATERAL)
- **Pattern**: Same `0xbebebebebebebebe` but in different code paths (vector memory management)
- **Solution**: Needs output.Reset() pattern analysis and SetValue() fixes across all functions
- **Status**: ❌ **ONGOING** - Multiple functions still affected
- **Functions Affected**: Multiple functions including git_read_each, git_branches_each, git_log_each, etc.

## Current Problem Details

### Environment
- **Extension**: Duck Tails (git functions for DuckDB)
- **DuckDB Version**: v1.3.2
- **Platform**: macOS (Darwin 24.6.0, arm64)
- **Compiler**: AppleClang 17.0.0.17000013
- **Testing Framework**: AddressSanitizer enabled unittest

### Technical Analysis

#### Memory Pattern Recognition
**Characteristic Pattern**: `0xbebebebebebebebe`
- This specific pattern indicates DuckDB's internal memory debugging markers
- Shows up in `x[1]` and `x[19]` registers consistently across all crashes
- Points to uninitialized memory being accessed during string operations

#### Call Stack Analysis
**Common Stack Trace Pattern**:
```
#0 __asan_memcpy+0x98
#1 duckdb::Load<unsigned long long>(unsigned char const*) helper.hpp:228
#2 duckdb::Utf8Proc::Analyze(char const*, unsigned long, duckdb::UnicodeInvalidReason*, unsigned long*)
#3 duckdb::string_t::VerifyUTF8() const
#4 duckdb::Vector::Verify(duckdb::Vector&, duckdb::SelectionVector const&, unsigned long long)
#5 duckdb::DataChunk::Verify()
#6 duckdb::PipelineExecutor::EndOperator()
```

**Key Insight**: The crash occurs during string verification in the DuckDB pipeline executor, suggesting output vectors contain uninitialized string data.

### Functions Currently Affected

#### ✅ Fixed Functions (Schema Issues Only)
- **git_tree_parents.test** - 30 assertions passed
- **git_tree_enhanced.test** - 7 assertions passed  
- **git_each_integration.test** - 24 assertions passed
- **git_uri_function.test** - 21 assertions passed
- **git_tree_each_comprehensive.test** - 14 assertions passed
- **duck_tails_basic_functionality.test** - 7 assertions passed

#### ❌ Still Crashing (AddressSanitizer Issues)
- **git_uri_composability.test** - Memory corruption in git_read_each LATERAL usage
- **git_uri_composability_fixture.test** - Memory corruption in fixture-based tests
- Multiple other tests when run in batch (crashes during parallel execution)

### Root Cause Analysis

#### The git_read_each Success Story
We previously fixed git_read_each by identifying that it was:
1. Calling `output.Reset()` which invalidates vector memory
2. Then directly writing to raw vector pointers
3. **Solution**: Removed `output.Reset()` and used `output.SetCardinality()` + `output.SetValue()`

#### Current Problem: Similar Pattern in Other Functions
The same pattern likely exists in other git functions:
- Functions calling `output.Reset()` or similar vector invalidation
- Then attempting to write to vectors without proper initialization
- Functions using raw pointer access instead of safe `SetValue()` methods

### Specific Crash Scenarios

#### Scenario 1: git_uri_composability.test
**Error Location**: Line 127-128 (git_read_each with LATERAL join)
```sql
SELECT text 
FROM git_read_each(t.git_uri)
```
**Analysis**: Even though we fixed git_read_each, there might be edge cases in LATERAL join contexts.

#### Scenario 2: git_uri_composability_fixture.test
**Error Context**: Fixture-based test execution with CONCAT operations
**Analysis**: Crashes during complex URI construction and parsing operations.

#### Scenario 3: Batch Test Execution
**Error Context**: Running multiple tests causes crashes in worker threads
**Analysis**: Parallel execution revealing thread-safety issues in vector management.

## Investigation Steps Needed

### 1. Audit All Git Functions for output.Reset() Pattern
Search for functions that might have the same issue we fixed in git_read_each:

```bash
grep -n "output\.Reset\|FlatVector::GetData.*output" src/git_functions.cpp
```

### 2. Identify Functions Using Raw Vector Pointers
Look for unsafe vector access patterns:
```cpp
// UNSAFE - what we need to find and fix
FlatVector::GetData<string_t>(output.data[0])[i] = StringVector::AddString(...);

// SAFE - what we need to convert to
output.SetValue(0, i, Value(string_value));
```

### 3. Test Each Function Individually
Isolate which specific functions are causing crashes:
```bash
# Test each _each function individually
./build/debug/test/unittest test/sql/git_log_each_comprehensive.test
./build/debug/test/unittest test/sql/git_branches_each_comprehensive.test  
./build/debug/test/unittest test/sql/git_tags_each_comprehensive.test
```

## Comparison With Previous Issue

| Aspect | lateral-join-issue.md | Current Issue |
|--------|----------------------|---------------|
| **Symptom** | AddressSanitizer crashes with 0xbebebebe | Same crashes, same pattern |
| **Root Cause** | Uninitialized struct constructors | Uninitialized output vectors |
| **Location** | Struct field access during computation | Vector memory during output |
| **Fix Applied** | Added constructors to structs | Removed output.Reset() from git_read_each |
| **Current Status** | ✅ Resolved | ❌ Ongoing - multiple functions affected |
| **Scope** | Specific to git_tree and git_read | Multiple git functions |
| **Test Impact** | Some LATERAL tests failed | Multiple tests failing |

## Next Steps

### Immediate Actions Required
1. **Audit all git functions** for output.Reset() + raw pointer usage patterns
2. **Apply git_read_each fix pattern** to other affected functions:
   - Remove `output.Reset()` calls
   - Use `output.SetCardinality(count)` for initialization  
   - Use `output.SetValue(col, row, Value(data))` for safe writing
3. **Test each function individually** to identify specific culprits

### Pattern to Apply (Based on git_read_each Success)
```cpp
// BEFORE (causes crash)
output.Reset();
// ... later ...
FlatVector::GetData<string_t>(output.data[0])[i] = StringVector::AddString(output.data[0], str);

// AFTER (safe)
idx_t count = /* calculate count */;
output.SetCardinality(count);
// ... later ...  
output.SetValue(0, i, Value(str));
```

### Validation Strategy
1. Fix one function at a time
2. Test individually after each fix
3. Confirm fix with AddressSanitizer enabled
4. Move to next function

## Impact Assessment

| Audience | Impact | Urgency |
|----------|--------|---------|
| **Production Users** | None - functions work correctly in real usage | Low |
| **Development Team** | Cannot run full test suite reliably | High |
| **CI/CD Pipeline** | Test failures blocking development | High |
| **Code Quality** | Memory safety issues need resolution | High |

## Technical Debt

This issue represents **technical debt** from the URI schema standardization work:
- Schema changes were implemented correctly
- But vector output patterns weren't updated to match DuckDB's safe patterns
- Multiple functions need the same fix we applied to git_read_each

## Success Metrics

### Definition of Done
- [ ] All git functions use safe vector output patterns
- [ ] All tests pass with AddressSanitizer enabled
- [ ] No more 0xbebebebe memory corruption crashes
- [ ] Batch test execution works reliably

### Test Coverage
- [ ] Individual function tests: git_log_each, git_branches_each, git_tags_each, git_parents_each
- [ ] LATERAL join composability tests  
- [ ] Fixture-based tests
- [ ] Batch execution tests

## Documentation Updates Required

Once resolved, update:
1. **lateral-join-issue.md** - Mark as resolved, reference this document
2. **README.md** - Update testing instructions
3. **Code comments** - Document safe vector output patterns

## Conclusion

This is a **different but related** memory safety issue from the one documented in `lateral-join-issue.md`. While both show the same AddressSanitizer pattern, they have different root causes:

- **Previous issue**: Uninitialized struct fields → **FIXED** with constructors
- **Current issue**: Uninitialized vector memory → **NEEDS FIX** with output pattern updates

The solution path is clear based on our git_read_each success: systematically update all git functions to use safe vector output patterns instead of raw pointer access.

---
*Document created: 2025-01-09*  
*Status: ONGOING INVESTIGATION*