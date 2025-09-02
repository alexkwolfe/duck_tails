#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <git2.h>

namespace duckdb {

// Git log table function
struct GitLogFunctionData : public TableFunctionData {
    explicit GitLogFunctionData(const string &repo_path, const string &resolved_repo_path);
    ~GitLogFunctionData();
    
    string repo_path;
    string resolved_repo_path;
    git_repository *repo;
    git_revwalk *walker;
    bool initialized;
};

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitLogBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git branches table function  
struct GitBranchesFunctionData : public TableFunctionData {
    explicit GitBranchesFunctionData(const string &repo_path, const string &resolved_repo_path);
    ~GitBranchesFunctionData();
    
    string repo_path;
    string resolved_repo_path;
    git_repository *repo;
    git_branch_iterator *iterator;
    bool initialized;
};

void GitBranchesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitBranchesBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitBranchesInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git tags table function
struct GitTagsFunctionData : public TableFunctionData {
    explicit GitTagsFunctionData(const string &repo_path, const string &resolved_repo_path);
    ~GitTagsFunctionData();
    
    string repo_path;
    string resolved_repo_path;
    git_repository *repo;
    vector<string> tag_names;
    idx_t current_index;
    bool initialized;
};

void GitTagsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitTagsBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitTagsInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git tree operation modes
enum class GitTreeMode {
    SINGLE,    // Single commit (static or dynamic)
    ARRAY,     // Multiple commits from array
    RANGE      // Commit range (e.g., HEAD~10..HEAD)
};

// Git tree table function
struct GitTreeFunctionData : public TableFunctionData {
    explicit GitTreeFunctionData(const string &ref, const string &repo_path);
    explicit GitTreeFunctionData(const vector<string> &commits, const string &repo_path);
    explicit GitTreeFunctionData(const string &range, const string &repo_path, bool is_range);
    
    GitTreeMode mode;
    string ref;                    // For single commit mode
    vector<string> commits;        // For array mode
    string commit_range;           // For range mode
    string repo_path;
    vector<struct GitTreeRow> rows;
    idx_t current_index;
    bool is_dynamic;               // True if parameter comes from LATERAL
};

struct GitTreeRow {
    string commit_hash;   // Added for multi-commit support
    timestamp_t commit_date; // Added context
    string path;
    int32_t mode;
    string blob_hash;
    int64_t size;
};

void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitTreeInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git parents table function
struct GitParentsFunctionData : public TableFunctionData {
    explicit GitParentsFunctionData(const string &ref, const string &repo_path, bool all_refs);
    
    string ref;
    string repo_path;
    bool all_refs;
    vector<struct GitParentsRow> rows;
    idx_t current_index;
};

struct GitParentsRow {
    string commit_hash;
    string parent_hash;
    int32_t parent_index;
};

void GitParentsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitParentsBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitParentsInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Registration functions
void RegisterGitLogFunction(DatabaseInstance &db);
void RegisterGitBranchesFunction(DatabaseInstance &db);  
void RegisterGitTagsFunction(DatabaseInstance &db);
void RegisterGitTreeFunction(DatabaseInstance &db);
void RegisterGitParentsFunction(DatabaseInstance &db);
void RegisterGitFunctions(DatabaseInstance &db);

} // namespace duckdb