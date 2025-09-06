#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <git2.h>

namespace duckdb {

// Git log table function
struct GitLogFunctionData : public TableFunctionData {
    explicit GitLogFunctionData(const string &repo_path, const string &resolved_repo_path);
    explicit GitLogFunctionData(const string &ref);  // For LATERAL functions
    ~GitLogFunctionData();
    
    string repo_path;
    string resolved_repo_path;
    string ref;  // For LATERAL functions  
    git_repository *repo;
    git_revwalk *walker;
    bool initialized;
};

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitLogBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<FunctionData> GitLogEachBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git log row structure for LATERAL processing
struct GitLogRow {
    string repo_path;
    string commit_hash;
    string author_name;
    string author_email;
    string committer_name;
    string committer_email;
    timestamp_t author_date;
    timestamp_t commit_date;
    string message;
    uint32_t parent_count;
    string tree_hash;
};

// Local state for git_log_each LATERAL processing
struct GitLogLocalState : public LocalTableFunctionState {
    vector<GitLogRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;  
    bool initialized_row = false;
};

// Local init for git_log_each  
unique_ptr<LocalTableFunctionState> GitLogLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

// Git branches row structure for LATERAL processing
struct GitBranchesRow {
    string repo_path;
    string branch_name;
    string commit_hash;
    bool is_current;
    bool is_remote;
};

// Local state for git_branches_each LATERAL processing
struct GitBranchesLocalState : public LocalTableFunctionState {
    vector<GitBranchesRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;
};

// Local init for git_branches_each
unique_ptr<LocalTableFunctionState> GitBranchesLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

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
unique_ptr<FunctionData> GitBranchesEachBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitBranchesInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git tags row structure for LATERAL processing
struct GitTagsRow {
    string repo_path;
    string tag_name;
    string commit_hash;
    string tagger_name;
    timestamp_t tagger_date;
    string message;
    bool is_annotated;
};

// Local state for git_tags_each LATERAL processing
struct GitTagsLocalState : public LocalTableFunctionState {
    vector<GitTagsRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;
};

// Local init for git_tags_each
unique_ptr<LocalTableFunctionState> GitTagsLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

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
unique_ptr<FunctionData> GitTagsEachBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitTagsInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git tree operation modes
enum class GitTreeMode {
    SINGLE,    // Single commit (static or dynamic)
    RANGE      // Commit range (e.g., HEAD~10..HEAD)
};

// Git tree table function
struct GitTreeFunctionData : public TableFunctionData {
    explicit GitTreeFunctionData(const string &ref, const string &repo_path);
    explicit GitTreeFunctionData(const string &range, const string &repo_path, bool is_range);
    
    GitTreeMode mode;
    string ref;                    // For single commit mode
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
    string git_file_uri;  // Ready-to-use git:// URI for this file
    string file_path;     // Extracted file path from git_file_uri
    string file_ext;      // File extension (e.g., .js, .cpp, .md)
    string ref;          // Extracted ref from git_file_uri
};

// Local state for git_tree_each LATERAL processing
struct GitTreeLocalState : public LocalTableFunctionState {
    vector<GitTreeRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;
};

// Local init for git_tree_each
unique_ptr<LocalTableFunctionState> GitTreeLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<FunctionData> GitTreeEachBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitTreeInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git parents table function
struct GitParentsFunctionData : public TableFunctionData {
    explicit GitParentsFunctionData(const string &ref, const string &repo_path, bool all_refs);
    explicit GitParentsFunctionData(const vector<string> &commits, const string &repo_path, bool all_refs);
    
    string ref;                    // For single commit mode
    vector<string> commits;        // For array mode  
    string repo_path;
    bool all_refs;
    bool is_array_mode;
    vector<struct GitParentsRow> rows;
    idx_t current_index;
};

struct GitParentsRow {
    string commit_hash;
    string parent_hash;
    int32_t parent_index;
};

void GitParentsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);

// Local state for git_parents_each
struct GitParentsLocalState : public LocalTableFunctionState {
    vector<GitParentsRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;
    string current_repo_path;  // Store the repo path for the current row being processed
};

// Bind data for git_parents_each (simpler than regular git_parents)
struct GitParentsEachBindData : public TableFunctionData {
    string repo_path;  // Bind-time repository path
};

// Function declarations for git_parents_each
unique_ptr<FunctionData> GitParentsEachBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<LocalTableFunctionState> GitParentsLocalInit(ExecutionContext &context, TableFunctionInitInput &input, 
                                                       GlobalTableFunctionState *global_state);
OperatorResultType GitParentsEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                         DataChunk &input, DataChunk &output);
unique_ptr<FunctionData> GitParentsBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitParentsInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Forward declarations from git_clone.hpp
struct GitCloneFunctionData;
struct GitCloneLocalState;
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

// Registration functions
void RegisterGitLogFunction(DatabaseInstance &db);
void RegisterGitBranchesFunction(DatabaseInstance &db);  
void RegisterGitTagsFunction(DatabaseInstance &db);
void RegisterGitTreeFunction(DatabaseInstance &db);
void RegisterGitParentsFunction(DatabaseInstance &db);
void RegisterGitCloneFunction(DatabaseInstance &db);
void RegisterGitFunctions(DatabaseInstance &db);

} // namespace duckdb