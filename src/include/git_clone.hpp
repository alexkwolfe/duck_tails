#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <git2.h>

namespace duckdb {

// Git clone function data
struct GitCloneFunctionData : public TableFunctionData {
    GitCloneFunctionData(const string &url, const string &local_path, bool local_path_specified);
    ~GitCloneFunctionData() = default;
    
    string url;
    string local_path;
    bool local_path_specified;
    
    // Options from STRUCT parameter
    string branch;
    int32_t depth = 0;  // 0 means full clone
    bool bare = false;
    bool no_checkout = false;
    int32_t timeout = 300;  // 5 minutes default
    bool force = false;  // For future use
};

// Git clone row structure for result
struct GitCloneRow {
    string url;
    string local_path;
    string status;       // 'success' or 'error'
    string action;       // 'cloned', 'updated', 'up_to_date', or 'error'
    string message;      // Success message or error details
    string commit_hash;  // HEAD commit hash
    string previous_hash; // Previous HEAD (for updates, NULL for clones)
    timestamp_t commit_time;
    int64_t size_bytes = -1;  // Total size (optional, -1 if not available)
};

// Local state for git_clone_each LATERAL processing
struct GitCloneLocalState : public LocalTableFunctionState {
    GitCloneRow current_result;  // Single result for current input row
    idx_t current_input_row = 0;
    bool initialized_row = false;
    bool has_result = false;     // Whether we have a result to output
};

// Function declarations
void GitCloneFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
OperatorResultType GitCloneEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                       DataChunk &input, DataChunk &output);
unique_ptr<FunctionData> GitCloneBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<FunctionData> GitCloneEachBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitCloneInitGlobal(ClientContext &context, TableFunctionInitInput &input);
unique_ptr<LocalTableFunctionState> GitCloneLocalInit(ExecutionContext &context, TableFunctionInitInput &input, 
                                                     GlobalTableFunctionState *global_state);

// Helper functions
string ExtractRepoNameFromURL(const string &url);
GitCloneRow PerformGitClone(ClientContext &context, const string &url, const string &local_path, 
                           const GitCloneFunctionData &options);
bool IsGitRepository(const string &path);
GitCloneRow PerformGitPull(ClientContext &context, const string &repo_path, const string &ref, 
                          const GitCloneFunctionData &options);

// Registration function is now in git_functions.cpp

} // namespace duckdb