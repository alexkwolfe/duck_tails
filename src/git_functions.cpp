#include "git_functions.hpp"
#include "git_filesystem.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Git Log Function
//===--------------------------------------------------------------------===//

GitLogFunctionData::GitLogFunctionData(const string &repo_path, const string &resolved_repo_path) 
    : repo_path(repo_path), resolved_repo_path(resolved_repo_path), repo(nullptr), walker(nullptr), initialized(false) {
}

GitLogFunctionData::~GitLogFunctionData() {
    if (walker) {
        git_revwalk_free(walker);
    }
    if (repo) {
        git_repository_free(repo);
    }
}

unique_ptr<FunctionData> GitLogBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
    
    // Parse repository path from arguments
    string repo_path = ".";
    if (!input.inputs.empty()) {
        auto &repo_arg = input.inputs[0];
        if (repo_arg.type().id() == LogicalTypeId::VARCHAR) {
            repo_path = repo_arg.GetValue<string>();
        }
    }
    
    // Use GitPath::Parse for repository discovery
    string resolved_repo_path;
    try {
        auto git_path = GitPath::Parse("git://" + repo_path + "@HEAD");
        resolved_repo_path = git_path.repository_path;
    } catch (const std::exception &e) {
        throw IOException("Failed to resolve repository path '%s': %s", repo_path, e.what());
    }
    
    // Define return schema with repo_path as first column
    return_types = {
        LogicalType::VARCHAR,    // repo_path
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // author_name  
        LogicalType::VARCHAR,    // author_email
        LogicalType::VARCHAR,    // committer_name
        LogicalType::VARCHAR,    // committer_email
        LogicalType::TIMESTAMP,  // author_date
        LogicalType::TIMESTAMP,  // commit_date
        LogicalType::VARCHAR,    // message
        LogicalType::INTEGER,    // parent_count
        LogicalType::VARCHAR     // tree_hash
    };
    
    names = {
        "repo_path", "commit_hash", "author_name", "author_email", "committer_name", "committer_email",
        "author_date", "commit_date", "message", "parent_count", "tree_hash"
    };
    
    return make_uniq<GitLogFunctionData>(repo_path, resolved_repo_path);
}

unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

unique_ptr<LocalTableFunctionState> GitLogLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
    return make_uniq<GitLogLocalState>();
}

unique_ptr<LocalTableFunctionState> GitBranchesLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
    return make_uniq<GitBranchesLocalState>();
}

unique_ptr<LocalTableFunctionState> GitTagsLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
    return make_uniq<GitTagsLocalState>();
}

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &data = (GitLogFunctionData &)*data_p.bind_data;
    
    if (!data.initialized) {
        // Open repository using resolved path
        int error = git_repository_open(&data.repo, data.resolved_repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s", 
                            data.repo_path, e ? e->message : "Unknown error");
        }
        
        // Create revwalk
        error = git_revwalk_new(&data.walker, data.repo);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to create revwalk: %s", e ? e->message : "Unknown error");
        }
        
        // Push HEAD
        error = git_revwalk_push_head(data.walker);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to push HEAD: %s", e ? e->message : "Unknown error");
        }
        
        data.initialized = true;
    }
    
    idx_t count = 0;
    git_oid oid;
    
    while (count < STANDARD_VECTOR_SIZE && git_revwalk_next(&oid, data.walker) == 0) {
        git_commit *commit = nullptr;
        int error = git_commit_lookup(&commit, data.repo, &oid);
        if (error != 0) {
            continue; // Skip invalid commits
        }
        
        // Set repo_path as first column
        output.SetValue(0, count, Value(data.repo_path));
        
        // Get commit hash
        char hash_str[GIT_OID_HEXSZ + 1];
        git_oid_tostr(hash_str, sizeof(hash_str), &oid);
        output.SetValue(1, count, Value(hash_str));
        
        // Get author info
        const git_signature *author = git_commit_author(commit);
        output.SetValue(2, count, Value(author->name ? author->name : ""));
        output.SetValue(3, count, Value(author->email ? author->email : ""));
        
        // Get committer info
        const git_signature *committer = git_commit_committer(commit);
        output.SetValue(4, count, Value(committer->name ? committer->name : ""));
        output.SetValue(5, count, Value(committer->email ? committer->email : ""));
        
        // Get timestamps
        timestamp_t author_ts = Timestamp::FromEpochSeconds(author->when.time);
        timestamp_t commit_ts = Timestamp::FromEpochSeconds(committer->when.time);
        output.SetValue(6, count, Value::TIMESTAMP(author_ts));
        output.SetValue(7, count, Value::TIMESTAMP(commit_ts));
        
        // Get commit message
        const char *message = git_commit_message(commit);
        output.SetValue(8, count, Value(message ? message : ""));
        
        // Get parent count
        unsigned int parent_count = git_commit_parentcount(commit);
        output.SetValue(9, count, Value::INTEGER(parent_count));
        
        // Get tree hash
        const git_oid *tree_oid = git_commit_tree_id(commit);
        char tree_hash[GIT_OID_HEXSZ + 1];
        git_oid_tostr(tree_hash, sizeof(tree_hash), tree_oid);
        output.SetValue(10, count, Value(tree_hash));
        
        git_commit_free(commit);
        count++;
    }
    
    output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Git Branches Function
//===--------------------------------------------------------------------===//

GitBranchesFunctionData::GitBranchesFunctionData(const string &repo_path, const string &resolved_repo_path)
    : repo_path(repo_path), resolved_repo_path(resolved_repo_path), repo(nullptr), iterator(nullptr), initialized(false) {
}

GitBranchesFunctionData::~GitBranchesFunctionData() {
    if (iterator) {
        git_branch_iterator_free(iterator);
    }
    if (repo) {
        git_repository_free(repo);
    }
}

unique_ptr<FunctionData> GitBranchesBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    
    string repo_path = ".";
    if (!input.inputs.empty()) {
        auto &repo_arg = input.inputs[0];
        if (repo_arg.type().id() == LogicalTypeId::VARCHAR) {
            repo_path = repo_arg.GetValue<string>();
        }
    }
    
    // Use GitPath::Parse for repository discovery
    string resolved_repo_path;
    try {
        auto git_path = GitPath::Parse("git://" + repo_path + "@HEAD");
        resolved_repo_path = git_path.repository_path;
    } catch (const std::exception &e) {
        throw IOException("Failed to resolve repository path '%s': %s", repo_path, e.what());
    }
    
    return_types = {
        LogicalType::VARCHAR,  // repo_path
        LogicalType::VARCHAR,  // branch_name
        LogicalType::VARCHAR,  // commit_hash
        LogicalType::BOOLEAN,  // is_current
        LogicalType::BOOLEAN   // is_remote
    };
    
    names = {"repo_path", "branch_name", "commit_hash", "is_current", "is_remote"};
    
    return make_uniq<GitBranchesFunctionData>(repo_path, resolved_repo_path);
}

unique_ptr<GlobalTableFunctionState> GitBranchesInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

void GitBranchesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &data = (GitBranchesFunctionData &)*data_p.bind_data;
    
    if (!data.initialized) {
        int error = git_repository_open(&data.repo, data.resolved_repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s", 
                            data.resolved_repo_path, e ? e->message : "Unknown error");
        }
        
        error = git_branch_iterator_new(&data.iterator, data.repo, GIT_BRANCH_ALL);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to create branch iterator: %s", e ? e->message : "Unknown error");
        }
        
        data.initialized = true;
    }
    
    idx_t count = 0;
    git_reference *ref = nullptr;
    git_branch_t branch_type;
    
    while (count < STANDARD_VECTOR_SIZE && git_branch_next(&ref, &branch_type, data.iterator) == 0) {
        // Set repo_path as first column
        output.SetValue(0, count, Value(data.repo_path));
        
        // Get branch name
        const char *branch_name = nullptr;
        git_branch_name(&branch_name, ref);
        output.SetValue(1, count, Value(branch_name ? branch_name : ""));
        
        // Get commit hash
        const git_oid *oid = git_reference_target(ref);
        if (oid) {
            char hash_str[GIT_OID_HEXSZ + 1];
            git_oid_tostr(hash_str, sizeof(hash_str), oid);
            output.SetValue(2, count, Value(hash_str));
        } else {
            output.SetValue(2, count, Value(""));
        }
        
        // Check if current branch
        bool is_current = git_branch_is_head(ref) == 1;
        output.SetValue(3, count, Value::BOOLEAN(is_current));
        
        // Check if remote branch
        bool is_remote = (branch_type == GIT_BRANCH_REMOTE);
        output.SetValue(4, count, Value::BOOLEAN(is_remote));
        
        git_reference_free(ref);
        count++;
    }
    
    output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Git Tags Function
//===--------------------------------------------------------------------===//

GitTagsFunctionData::GitTagsFunctionData(const string &repo_path, const string &resolved_repo_path)
    : repo_path(repo_path), resolved_repo_path(resolved_repo_path), repo(nullptr), current_index(0), initialized(false) {
}

GitTagsFunctionData::~GitTagsFunctionData() {
    if (repo) {
        git_repository_free(repo);
    }
}

static int tag_foreach_cb(const char *name, git_oid *oid, void *payload) {
    auto *tag_names = static_cast<vector<string>*>(payload);
    if (StringUtil::StartsWith(name, "refs/tags/")) {
        tag_names->push_back(name + 10); // Remove "refs/tags/" prefix
    }
    return 0;
}

unique_ptr<FunctionData> GitTagsBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
    
    string repo_path = ".";
    if (!input.inputs.empty()) {
        auto &repo_arg = input.inputs[0];
        if (repo_arg.type().id() == LogicalTypeId::VARCHAR) {
            repo_path = repo_arg.GetValue<string>();
        }
    }
    
    // Use GitPath::Parse for repository discovery
    string resolved_repo_path;
    try {
        auto git_path = GitPath::Parse("git://" + repo_path + "@HEAD");
        resolved_repo_path = git_path.repository_path;
    } catch (const std::exception &e) {
        throw IOException("Failed to resolve repository path '%s': %s", repo_path, e.what());
    }
    
    return_types = {
        LogicalType::VARCHAR,    // repo_path
        LogicalType::VARCHAR,    // tag_name
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // tagger_name
        LogicalType::TIMESTAMP,  // tagger_date
        LogicalType::VARCHAR,    // message
        LogicalType::BOOLEAN     // is_annotated
    };
    
    names = {"repo_path", "tag_name", "commit_hash", "tagger_name", "tagger_date", "message", "is_annotated"};
    
    return make_uniq<GitTagsFunctionData>(repo_path, resolved_repo_path);
}

unique_ptr<GlobalTableFunctionState> GitTagsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

void GitTagsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &data = (GitTagsFunctionData &)*data_p.bind_data;
    
    if (!data.initialized) {
        int error = git_repository_open(&data.repo, data.resolved_repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s", 
                            data.resolved_repo_path, e ? e->message : "Unknown error");
        }
        
        // Get all tags
        error = git_tag_foreach(data.repo, tag_foreach_cb, &data.tag_names);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to list tags: %s", e ? e->message : "Unknown error");
        }
        
        data.initialized = true;
    }
    
    idx_t count = 0;
    
    while (count < STANDARD_VECTOR_SIZE && data.current_index < data.tag_names.size()) {
        const string &tag_name = data.tag_names[data.current_index];
        
        // Look up tag reference
        git_reference *tag_ref = nullptr;
        string full_name = "refs/tags/" + tag_name;
        int error = git_reference_lookup(&tag_ref, data.repo, full_name.c_str());
        if (error == 0) {
            // Set repo_path as first column
            output.SetValue(0, count, Value(data.repo_path));
            output.SetValue(1, count, Value(tag_name));
            
            const git_oid *oid = git_reference_target(tag_ref);
            if (oid) {
                char hash_str[GIT_OID_HEXSZ + 1];
                git_oid_tostr(hash_str, sizeof(hash_str), oid);
                output.SetValue(2, count, Value(hash_str));
                
                // Try to get tag object for annotation info
                git_tag *tag_obj = nullptr;
                bool is_annotated = false;
                if (git_tag_lookup(&tag_obj, data.repo, oid) == 0) {
                    is_annotated = true;
                    
                    const git_signature *tagger = git_tag_tagger(tag_obj);
                    if (tagger) {
                        output.SetValue(3, count, Value(tagger->name ? tagger->name : ""));
                        timestamp_t tag_ts = Timestamp::FromEpochSeconds(tagger->when.time);
                        output.SetValue(4, count, Value::TIMESTAMP(tag_ts));
                    } else {
                        output.SetValue(3, count, Value(""));
                        output.SetValue(4, count, Value());
                    }
                    
                    const char *message = git_tag_message(tag_obj);
                    output.SetValue(5, count, Value(message ? message : ""));
                    
                    git_tag_free(tag_obj);
                } else {
                    // Lightweight tag
                    output.SetValue(3, count, Value(""));
                    output.SetValue(4, count, Value());
                    output.SetValue(5, count, Value(""));
                }
                
                output.SetValue(6, count, Value::BOOLEAN(is_annotated));
            }
            
            git_reference_free(tag_ref);
            count++;
        }
        
        data.current_index++;
    }
    
    output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Git Tree Function
//===--------------------------------------------------------------------===//

GitTreeFunctionData::GitTreeFunctionData(const string &ref, const string &repo_path) 
    : mode(GitTreeMode::SINGLE), ref(ref), repo_path(repo_path), current_index(0), is_dynamic(false) {
}

GitTreeFunctionData::GitTreeFunctionData(const vector<string> &commits, const string &repo_path)
    : mode(GitTreeMode::ARRAY), commits(commits), repo_path(repo_path), current_index(0), is_dynamic(false) {
}

GitTreeFunctionData::GitTreeFunctionData(const string &range, const string &repo_path, bool is_range)
    : mode(GitTreeMode::RANGE), commit_range(range), repo_path(repo_path), current_index(0), is_dynamic(false) {
}

static string oid_to_hex(const git_oid *oid) {
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    return string(hex);
}

static bool IsCommitRange(const string &param) {
    // Check for git range syntax
    return param.find("..") != string::npos || 
           param == "--all" || 
           param.find("~") != string::npos ||
           param.find("^") != string::npos;
}

static vector<string> ParseCommitArray(const Value &array_value) {
    vector<string> commits;
    if (array_value.type().id() == LogicalTypeId::LIST) {
        auto children = ListValue::GetChildren(array_value);
        for (const auto &child : children) {
            if (child.type().id() == LogicalTypeId::VARCHAR) {
                commits.push_back(child.GetValue<string>());
            }
        }
    }
    return commits;
}

// Helper function to construct git:// URI for a file
static string BuildGitFileUri(const string &repo_path, const string &file_path, const string &commit_hash) {
    string uri = "git://" + repo_path;
    if (!file_path.empty()) {
        // Add separator if repo_path doesn't end with / and file_path doesn't start with /
        if (!repo_path.empty() && repo_path.back() != '/' && file_path[0] != '/') {
            uri += "/";
        }
        uri += file_path;
    }
    uri += "@" + commit_hash;
    return uri;
}

static void traverse_tree(git_repository *repo, git_tree *tree, const string &base, vector<GitTreeRow> &out, 
                          const string &commit_hash, timestamp_t commit_date, const string &repo_path) {
    const size_t count = git_tree_entrycount(tree);
    for (size_t i = 0; i < count; ++i) {
        const git_tree_entry *entry = git_tree_entry_byindex(tree, i);
        const char *name = git_tree_entry_name(entry);
        git_object_t type = git_tree_entry_type(entry);
        const git_oid *oid = git_tree_entry_id(entry);
        int32_t mode = git_tree_entry_filemode(entry);

        string path = base.empty() ? string(name) : (base + "/" + name);

        if (type == GIT_OBJECT_BLOB) {
            git_blob *blob = nullptr;
            int64_t size = 0;
            if (git_blob_lookup(&blob, repo, oid) == 0) {
                size = static_cast<int64_t>(git_blob_rawsize(blob));
                git_blob_free(blob);
            }
            string git_file_uri = BuildGitFileUri(repo_path, path, commit_hash);
            out.push_back(GitTreeRow{commit_hash, commit_date, path, mode, oid_to_hex(oid), size, git_file_uri});
        } else if (type == GIT_OBJECT_TREE) {
            git_tree *subtree = nullptr;
            if (git_tree_lookup(&subtree, repo, oid) == 0) {
                traverse_tree(repo, subtree, path, out, commit_hash, commit_date, repo_path);
                git_tree_free(subtree);
            }
        }
    }
}

unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
    string repo_path = ".";
    
    // Check for named parameter repo_path first
    if (input.named_parameters.count("repo_path")) {
        repo_path = StringValue::Get(input.named_parameters.at("repo_path"));
    }
    
    // Handle different parameter types
    if (input.inputs.empty()) {
        // Zero arguments - default to HEAD
        names = {"commit_hash", "commit_date", "path", "mode", "blob_hash", "size", "git_file_uri"};
        return_types = {LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR, 
                       LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR};
        return make_uniq<GitTreeFunctionData>("HEAD", repo_path);
    }
    
    auto &first_param = input.inputs[0];
    
    if (first_param.type().id() == LogicalTypeId::VARCHAR) {
        string param = first_param.GetValue<string>();
        
        // Override repo_path if second parameter provided
        if (input.inputs.size() >= 2) {
            repo_path = input.inputs[1].GetValue<string>();
        }
        
        if (IsCommitRange(param)) {
            // Range mode: "HEAD~10..HEAD", "--all", etc.
            names = {"commit_hash", "commit_date", "path", "mode", "blob_hash", "size", "git_file_uri"};
            return_types = {LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR,
                           LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR};
            return make_uniq<GitTreeFunctionData>(param, repo_path, true);
        } else {
            // Single commit mode
            names = {"commit_hash", "commit_date", "path", "mode", "blob_hash", "size", "git_file_uri"};
            return_types = {LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR,
                           LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR};
            return make_uniq<GitTreeFunctionData>(param, repo_path);
        }
    } 
    else if (first_param.type().id() == LogicalTypeId::LIST) {
        // Array mode: ARRAY['HEAD', 'HEAD~1']
        auto commits = ParseCommitArray(first_param);
        if (commits.empty()) {
            throw InternalException("git_tree: empty commit array provided");
        }
        
        names = {"commit_hash", "commit_date", "path", "mode", "blob_hash", "size", "git_file_uri"};
        return_types = {LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR,
                       LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR};
        return make_uniq<GitTreeFunctionData>(commits, repo_path);
    }
    
    throw InternalException("git_tree: unsupported parameter type - expected VARCHAR or LIST");
}

// Helper function to process a single commit
static void ProcessSingleCommit(git_repository *repo, const string &ref, const string &repo_path,
                               vector<GitTreeRow> &rows) {
    git_object *obj = nullptr;
    if (git_revparse_single(&obj, repo, ref.c_str()) != 0) {
        const git_error *e = git_error_last();
        throw IOException("git_tree: failed to resolve ref '%s' in repository '%s': %s", 
                        ref, repo_path, e ? e->message : "Unknown error");
    }

    git_tree *tree = nullptr;
    git_commit *commit = nullptr;
    timestamp_t commit_date = timestamp_t(0);
    string commit_hash;

    if (git_object_type(obj) == GIT_OBJECT_COMMIT) {
        commit = reinterpret_cast<git_commit *>(obj);
        if (git_commit_tree(&tree, commit) != 0) {
            git_object_free(obj);
            throw IOException("git_tree: failed to get tree from commit");
        }
        
        // Get commit info
        const git_signature *sig = git_commit_author(commit);
        commit_date = Timestamp::FromEpochSeconds(sig->when.time);
        commit_hash = oid_to_hex(git_object_id(obj));
    } else if (git_object_type(obj) == GIT_OBJECT_TREE) {
        git_oid oid = *git_tree_id(reinterpret_cast<git_tree *>(obj));
        if (git_tree_lookup(&tree, repo, &oid) != 0) {
            git_object_free(obj);
            throw IOException("git_tree: failed to lookup tree");
        }
        commit_hash = oid_to_hex(&oid);
        commit_date = timestamp_t(0); // No timestamp for direct tree objects
    } else {
        git_object_free(obj);
        throw IOException("git_tree: ref is not a commit or tree");
    }

    traverse_tree(repo, tree, "", rows, commit_hash, commit_date, repo_path);

    git_tree_free(tree);
    git_object_free(obj);
}

unique_ptr<GlobalTableFunctionState> GitTreeInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = const_cast<GitTreeFunctionData&>(input.bind_data->Cast<GitTreeFunctionData>());
    
    git_libgit2_init();
    
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, bind_data.repo_path.c_str());
    if (error != 0) {
        const git_error *e = git_error_last();
        git_libgit2_shutdown();
        throw IOException("git_tree: failed to open git repository '%s': %s", 
                        bind_data.repo_path, e ? e->message : "Unknown error");
    }

    vector<GitTreeRow> rows;
    rows.reserve(1024);

    try {
        switch (bind_data.mode) {
            case GitTreeMode::SINGLE: {
                ProcessSingleCommit(repo, bind_data.ref, bind_data.repo_path, rows);
                break;
            }
            case GitTreeMode::ARRAY: {
                for (const auto &commit_ref : bind_data.commits) {
                    ProcessSingleCommit(repo, commit_ref, bind_data.repo_path, rows);
                }
                break;
            }
            case GitTreeMode::RANGE: {
                // For now, implement basic range support
                // TODO: Implement full git range parsing (HEAD~10..HEAD, --all, etc.)
                if (bind_data.commit_range == "--all") {
                    // Process all reachable commits
                    git_revwalk *walk = nullptr;
                    git_revwalk_new(&walk, repo);
                    git_revwalk_push_head(walk);
                    
                    git_oid oid;
                    while (git_revwalk_next(&oid, walk) == 0) {
                        string commit_hash = oid_to_hex(&oid);
                        ProcessSingleCommit(repo, commit_hash, bind_data.repo_path, rows);
                    }
                    git_revwalk_free(walk);
                } else {
                    // For other ranges, just process as single commit for now
                    ProcessSingleCommit(repo, bind_data.commit_range, bind_data.repo_path, rows);
                }
                break;
            }
        }
    } catch (...) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        throw;
    }

    // Store rows in bind_data so they persist
    bind_data.rows = std::move(rows);

    git_repository_free(repo);
    git_libgit2_shutdown();

    return make_uniq<GlobalTableFunctionState>();
}

void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = const_cast<GitTreeFunctionData&>(data_p.bind_data->Cast<GitTreeFunctionData>());
    
    idx_t remaining = bind_data.rows.size() - bind_data.current_index;
    if (remaining == 0) {
        output.SetCardinality(0);
        return;
    }

    const idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
    
    for (idx_t i = 0; i < count; i++) {
        auto &row = bind_data.rows[bind_data.current_index + i];
        output.SetValue(0, i, Value(row.commit_hash));           // commit_hash
        output.SetValue(1, i, Value::TIMESTAMP(row.commit_date)); // commit_date
        output.SetValue(2, i, Value(row.path));                  // path
        output.SetValue(3, i, Value::INTEGER(row.mode));         // mode
        output.SetValue(4, i, Value(row.blob_hash));             // blob_hash
        output.SetValue(5, i, Value::BIGINT(row.size));          // size
        output.SetValue(6, i, Value(row.git_file_uri));          // git_file_uri
    }

    output.SetCardinality(count);
    bind_data.current_index += count;
}

// Git Tree In-Out Function for LATERAL support
struct GitTreeInOutState : public LocalTableFunctionState {
    GitTreeInOutState() = default;
    
    bool initialized_row = false;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    vector<GitTreeRow> current_rows;
    string repo_path;
};

static unique_ptr<LocalTableFunctionState> GitTreeInOutInit(ExecutionContext &context,
                                                           TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_state) {
    auto state = make_uniq<GitTreeInOutState>();
    auto &bind_data = input.bind_data->Cast<GitTreeFunctionData>();
    state->repo_path = bind_data.repo_path;
    return std::move(state);
}

static void ProcessCommitForInOut(const string &commit_hash, const string &repo_path, vector<GitTreeRow> &rows) {
    git_libgit2_init();
    git_repository *repo = nullptr;
    
    try {
        if (git_repository_open(&repo, repo_path.c_str()) != 0) {
            git_libgit2_shutdown();
            return;
        }
        
        git_oid oid;
        if (git_oid_fromstr(&oid, commit_hash.c_str()) != 0) {
            git_repository_free(repo);
            git_libgit2_shutdown();
            return;
        }
        
        git_commit *commit = nullptr;
        if (git_commit_lookup(&commit, repo, &oid) != 0) {
            git_repository_free(repo);
            git_libgit2_shutdown();
            return;
        }
        
        timestamp_t commit_date = Timestamp::FromEpochSeconds(git_commit_time(commit));
        
        git_tree *tree = nullptr;
        if (git_commit_tree(&tree, commit) != 0) {
            git_commit_free(commit);
            git_repository_free(repo);
            git_libgit2_shutdown();
            return;
        }
        
        // Use existing traverse_tree function to populate rows
        traverse_tree(repo, tree, "", rows, commit_hash, commit_date, repo_path);
        
        git_tree_free(tree);
        git_commit_free(commit);
        git_repository_free(repo);
        git_libgit2_shutdown();
        
    } catch (...) {
        if (repo) git_repository_free(repo);
        git_libgit2_shutdown();
    }
}

static OperatorResultType GitTreeInOutFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                              DataChunk &input, DataChunk &output) {
    auto &state = data_p.local_state->Cast<GitTreeInOutState>();
    
    while (true) {
        if (!state.initialized_row) {
            // Initialize for the current input row
            if (state.current_input_row >= input.size()) {
                // Ran out of input rows
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // Get the commit hash for this row
            input.Flatten();
            if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                // Null commit hash - skip this row
                state.current_input_row++;
                continue;
            }
            
            string commit_hash = FlatVector::GetValue<string>(input.data[0], state.current_input_row);
            
            // Process the git tree for this commit
            state.current_rows.clear();
            ProcessCommitForInOut(commit_hash, state.repo_path, state.current_rows);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        if (state.current_output_row >= state.current_rows.size()) {
            // Finished outputting all rows for this input row, move to next
            state.current_input_row++;
            state.initialized_row = false;
            continue;
        }
        
        // Output rows for the current commit
        idx_t remaining = state.current_rows.size() - state.current_output_row;
        idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
        
        for (idx_t i = 0; i < count; i++) {
            auto &row = state.current_rows[state.current_output_row + i];
            output.SetValue(0, i, Value(row.commit_hash));           // commit_hash
            output.SetValue(1, i, Value::TIMESTAMP(row.commit_date)); // commit_date
            output.SetValue(2, i, Value(row.path));                  // path
            output.SetValue(3, i, Value::INTEGER(row.mode));         // mode
            output.SetValue(4, i, Value(row.blob_hash));             // blob_hash
            output.SetValue(5, i, Value::BIGINT(row.size));          // size
            output.SetValue(6, i, Value(row.git_file_uri));          // git_file_uri
        }
        
        output.SetCardinality(count);
        state.current_output_row += count;
        
        return OperatorResultType::HAVE_MORE_OUTPUT;
    }
}

//===--------------------------------------------------------------------===//
// Git Parents Function  
//===--------------------------------------------------------------------===//

GitParentsFunctionData::GitParentsFunctionData(const string &ref, const string &repo_path, bool all_refs)
    : ref(ref), repo_path(repo_path), all_refs(all_refs), current_index(0) {
}

unique_ptr<FunctionData> GitParentsBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
    string ref = "HEAD";
    string repo_path = ".";
    bool all_refs = false;
    
    if (input.inputs.size() >= 1) {
        ref = input.inputs[0].GetValue<string>();
    }
    if (input.inputs.size() >= 2) {
        repo_path = input.inputs[1].GetValue<string>();
    }
    
    // Check for named parameters
    if (input.named_parameters.count("repo_path")) {
        repo_path = StringValue::Get(input.named_parameters.at("repo_path"));
    }
    if (input.named_parameters.count("all_refs")) {
        all_refs = BooleanValue::Get(input.named_parameters.at("all_refs"));
    }
    
    names = {"commit_hash", "parent_hash", "parent_index"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER};
    
    return make_uniq<GitParentsFunctionData>(ref, repo_path, all_refs);
}

unique_ptr<GlobalTableFunctionState> GitParentsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = const_cast<GitParentsFunctionData&>(input.bind_data->Cast<GitParentsFunctionData>());
    
    git_libgit2_init();
    
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, bind_data.repo_path.c_str());
    if (error != 0) {
        const git_error *e = git_error_last();
        git_libgit2_shutdown();
        throw IOException("git_parents: failed to open git repository '%s': %s", 
                        bind_data.repo_path, e ? e->message : "Unknown error");
    }

    git_revwalk *walk = nullptr;
    git_revwalk_new(&walk, repo);
    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);

    if (bind_data.all_refs) {
        git_reference_iterator *it = nullptr;
        git_reference_iterator_new(&it, repo);
        git_reference *ref;
        while (!git_reference_next(&ref, it)) {
            const git_oid *target = git_reference_target(ref);
            if (target) {
                git_revwalk_push(walk, target);
            }
        }
        git_reference_iterator_free(it);
    } else {
        git_object *obj = nullptr;
        if (git_revparse_single(&obj, repo, bind_data.ref.c_str()) != 0) {
            const git_error *e = git_error_last();
            git_revwalk_free(walk);
            git_repository_free(repo);
            git_libgit2_shutdown();
            throw IOException("git_parents: cannot resolve ref '%s' in repository '%s': %s", 
                            bind_data.ref, bind_data.repo_path, e ? e->message : "Unknown error");
        }
        git_oid oid = *git_object_id(obj);
        git_revwalk_push(walk, &oid);
        git_object_free(obj);
    }

    vector<GitParentsRow> rows;
    git_oid oid;
    while (!git_revwalk_next(&oid, walk)) {
        git_commit *commit = nullptr;
        if (git_commit_lookup(&commit, repo, &oid) != 0) {
            continue;
        }
        
        unsigned int parent_count = git_commit_parentcount(commit);
        for (unsigned int i = 0; i < parent_count; i++) {
            const git_oid *parent_oid = git_commit_parent_id(commit, i);
            rows.push_back(GitParentsRow{
                oid_to_hex(&oid), 
                oid_to_hex(parent_oid), 
                static_cast<int32_t>(i)
            });
        }
        git_commit_free(commit);
    }

    // Store rows in bind_data
    bind_data.rows = std::move(rows);

    git_revwalk_free(walk);
    git_repository_free(repo);
    git_libgit2_shutdown();

    return make_uniq<GlobalTableFunctionState>();
}

void GitParentsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = const_cast<GitParentsFunctionData&>(data_p.bind_data->Cast<GitParentsFunctionData>());
    
    idx_t remaining = bind_data.rows.size() - bind_data.current_index;
    if (remaining == 0) {
        output.SetCardinality(0);
        return;
    }

    const idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
    
    for (idx_t i = 0; i < count; i++) {
        auto &row = bind_data.rows[bind_data.current_index + i];
        output.SetValue(0, i, Value(row.commit_hash));
        output.SetValue(1, i, Value(row.parent_hash));
        output.SetValue(2, i, Value::INTEGER(row.parent_index));
    }

    output.SetCardinality(count);
    bind_data.current_index += count;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

// Helper function for LATERAL git_log_each processing - processes entire git log for a repository path
static void ProcessLogCommitForInOut(const string &input_repo_path, const string &bind_repo_path, vector<GitLogRow> &rows) {
    // Use input_repo_path from LATERAL context, or bind_repo_path as fallback
    string repo_path = input_repo_path.empty() ? bind_repo_path : input_repo_path;
    
    git_libgit2_init();
    
    // Use GitPath::Parse to resolve repository path  
    string resolved_repo_path;
    try {
        auto git_path = GitPath::Parse("git://" + repo_path + "@HEAD");  
        resolved_repo_path = git_path.repository_path;
    } catch (const std::exception &e) {
        throw IOException("Failed to resolve repository path '%s': %s", repo_path, e.what());
    }
    
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, resolved_repo_path.c_str());
    if (error != 0) {
        const git_error *e = git_error_last();
        throw IOException("Failed to open repository '%s': %s", 
                        repo_path, e ? e->message : "Unknown error");
    }
    
    // Create revwalk
    git_revwalk *walker = nullptr;
    error = git_revwalk_new(&walker, repo);
    if (error != 0) {
        git_repository_free(repo);
        const git_error *e = git_error_last();
        throw IOException("Failed to create revwalk: %s", e ? e->message : "Unknown error");
    }
    
    // Push HEAD (like git_log does) - process entire log
    error = git_revwalk_push_head(walker);
    if (error != 0) {
        git_revwalk_free(walker);
        git_repository_free(repo);
        const git_error *e = git_error_last();
        throw IOException("Failed to push HEAD: %s", e ? e->message : "Unknown error");
        return;
    }
    
    // Walk commits
    git_oid commit_oid;
    while (git_revwalk_next(&commit_oid, walker) == 0) {
        git_commit *commit = nullptr;
        error = git_commit_lookup(&commit, repo, &commit_oid);
        if (error != 0) {
            continue; // Skip invalid commits
        }
        
        GitLogRow row;
        row.repo_path = repo_path;
        
        // Get commit hash
        char hash_str[GIT_OID_HEXSZ + 1];
        git_oid_tostr(hash_str, sizeof(hash_str), &commit_oid);
        row.commit_hash = hash_str;
        
        // Get author info
        const git_signature *author = git_commit_author(commit);
        row.author_name = author->name ? author->name : "";
        row.author_email = author->email ? author->email : "";
        
        // Get committer info
        const git_signature *committer = git_commit_committer(commit);
        row.committer_name = committer->name ? committer->name : "";
        row.committer_email = committer->email ? committer->email : "";
        
        // Get timestamps
        row.author_date = Timestamp::FromEpochSeconds(author->when.time);
        row.commit_date = Timestamp::FromEpochSeconds(committer->when.time);
        
        // Get commit message
        const char *message = git_commit_message(commit);
        row.message = message ? message : "";
        
        // Get parent count
        row.parent_count = git_commit_parentcount(commit);
        
        // Get tree hash
        const git_oid *tree_oid = git_commit_tree_id(commit);
        char tree_hash[GIT_OID_HEXSZ + 1];
        git_oid_tostr(tree_hash, sizeof(tree_hash), tree_oid);
        row.tree_hash = tree_hash;
        
        rows.push_back(row);
        git_commit_free(commit);
    }
    
    git_revwalk_free(walker);
    git_repository_free(repo);
}

// LATERAL git_log_each function - processes dynamic commit refs
static OperatorResultType GitLogEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                           DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitLogFunctionData>();
    auto &state = data_p.local_state->Cast<GitLogLocalState>();
    
    while (true) {
        if (!state.initialized_row) {
            // initialize for the current input row
            if (state.current_input_row >= input.size()) {
                // ran out of rows
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // LATERAL function: ALWAYS extract commit ref from input DataChunk
            input.Flatten();
            
            // Check if input has columns and data
            if (input.ColumnCount() == 0) {
                throw BinderException("git_log_each: no input columns available");
            }
            
            // Check if the input is null
            if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                // Move to next input row if null
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Extract commit ref from input DataChunk - direct string_t access
            auto data = FlatVector::GetData<string_t>(input.data[0]);
            if (!data) {
                throw BinderException("git_log_each: no string data in input column");
            }
            
            // Get the string_t directly and convert to string
            auto string_t_value = data[state.current_input_row];
            string input_repo_path(string_t_value.GetData(), string_t_value.GetSize());
            
            if (input_repo_path.empty()) {
                throw BinderException("git_log_each: received empty repository path from input");
            }
            
            // Process the git log for this repository using the DRY shared logic
            state.current_rows.clear();
            ProcessLogCommitForInOut(input_repo_path, bind_data.repo_path, state.current_rows);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        // Output rows for current input
        idx_t output_count = 0;
        while (output_count < STANDARD_VECTOR_SIZE && 
               state.current_output_row < state.current_rows.size()) {
            
            auto &row = state.current_rows[state.current_output_row];
            
            // Fill output row with git log data
            output.SetValue(0, output_count, Value(row.repo_path));
            output.SetValue(1, output_count, Value(row.commit_hash));
            output.SetValue(2, output_count, Value(row.author_name));  
            output.SetValue(3, output_count, Value(row.author_email));
            output.SetValue(4, output_count, Value(row.committer_name));
            output.SetValue(5, output_count, Value(row.committer_email));
            output.SetValue(6, output_count, Value::TIMESTAMP(row.author_date));
            output.SetValue(7, output_count, Value::TIMESTAMP(row.commit_date));
            output.SetValue(8, output_count, Value(row.message));
            output.SetValue(9, output_count, Value::INTEGER(row.parent_count));
            output.SetValue(10, output_count, Value(row.tree_hash));
            
            output_count++;
            state.current_output_row++;
        }
        
        output.SetCardinality(output_count);
        
        // Check if we're done with current input row
        if (state.current_output_row >= state.current_rows.size()) {
            state.current_input_row++;
            state.initialized_row = false;
        }
        
        return OperatorResultType::HAVE_MORE_OUTPUT;
    }
}

// Helper function for LATERAL git_branches_each processing - processes entire git branches for a repository path
static void ProcessBranchesForInOut(const string &input_repo_path, const string &bind_repo_path, vector<GitBranchesRow> &rows) {
    // Use input_repo_path from LATERAL context, or bind_repo_path as fallback
    string repo_path = input_repo_path.empty() ? bind_repo_path : input_repo_path;
    
    // Use GitPath::Parse for repository discovery
    string resolved_repo_path;
    try {
        auto git_path = GitPath::Parse("git://" + repo_path + "@HEAD");
        resolved_repo_path = git_path.repository_path;
    } catch (const std::exception &e) {
        throw IOException("Failed to resolve repository path '%s': %s", repo_path, e.what());
    }
    
    git_repository *repo = nullptr;
    git_branch_iterator *iterator = nullptr;
    
    try {
        // Open repository
        int error = git_repository_open(&repo, resolved_repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s", 
                            resolved_repo_path, e ? e->message : "Unknown error");
        }
        
        // Create branch iterator
        error = git_branch_iterator_new(&iterator, repo, GIT_BRANCH_ALL);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to create branch iterator: %s", e ? e->message : "Unknown error");
        }
        
        // Iterate through branches
        git_reference *ref = nullptr;
        git_branch_t branch_type;
        
        while (git_branch_next(&ref, &branch_type, iterator) == 0) {
            GitBranchesRow row;
            row.repo_path = repo_path;
            
            // Get branch name
            const char *branch_name = nullptr;
            git_branch_name(&branch_name, ref);
            row.branch_name = branch_name ? branch_name : "";
            
            // Get commit hash
            const git_oid *oid = git_reference_target(ref);
            if (oid) {
                char hash_str[GIT_OID_HEXSZ + 1];
                git_oid_tostr(hash_str, sizeof(hash_str), oid);
                row.commit_hash = hash_str;
            } else {
                row.commit_hash = "";
            }
            
            // Check if current branch
            row.is_current = (git_branch_is_head(ref) == 1);
            
            // Check if remote branch
            row.is_remote = (branch_type == GIT_BRANCH_REMOTE);
            
            rows.push_back(row);
            
            git_reference_free(ref);
            ref = nullptr;
        }
        
    } catch (...) {
        // Cleanup on exception
        if (iterator) {
            git_branch_iterator_free(iterator);
            iterator = nullptr;
        }
        if (repo) {
            git_repository_free(repo);
            repo = nullptr;
        }
        throw;
    }
    
    // Normal cleanup
    if (iterator) {
        git_branch_iterator_free(iterator);
        iterator = nullptr;
    }
    if (repo) {
        git_repository_free(repo);
        repo = nullptr;
    }
}

// LATERAL git_branches_each function - processes dynamic repository paths
static OperatorResultType GitBranchesEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                                 DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitBranchesFunctionData>();
    auto &state = data_p.local_state->Cast<GitBranchesLocalState>();
    
    while (true) {
        if (!state.initialized_row) {
            if (state.current_input_row >= input.size()) {
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            input.Flatten();
            
            string input_repo_path;
            if (input.ColumnCount() == 0) {
                // Zero-argument static call: use bind_data repo_path (defaults to ".")
                input_repo_path = bind_data.repo_path.empty() ? "." : bind_data.repo_path;
            } else {
                // LATERAL call: extract repo_path from input column
                if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                    state.current_input_row++;
                    state.initialized_row = false;
                    continue;
                }
                
                auto data = FlatVector::GetData<string_t>(input.data[0]);
                if (!data) {
                    throw BinderException("git_branches_each: no string data in input column");
                }
                
                auto string_t_value = data[state.current_input_row];
                input_repo_path = string(string_t_value.GetData(), string_t_value.GetSize());
                
                if (input_repo_path.empty()) {
                    throw BinderException("git_branches_each: received empty repository path from input");
                }
            }
            
            state.current_rows.clear();
            ProcessBranchesForInOut(input_repo_path, bind_data.repo_path, state.current_rows);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        idx_t output_count = 0;
        while (output_count < STANDARD_VECTOR_SIZE && 
               state.current_output_row < state.current_rows.size()) {
            
            auto &row = state.current_rows[state.current_output_row];
            
            output.SetValue(0, output_count, Value(row.repo_path));
            output.SetValue(1, output_count, Value(row.branch_name));
            output.SetValue(2, output_count, Value(row.commit_hash));
            output.SetValue(3, output_count, Value(row.is_current));
            output.SetValue(4, output_count, Value(row.is_remote));
            
            output_count++;
            state.current_output_row++;
        }
        
        output.SetCardinality(output_count);
        
        if (state.current_output_row >= state.current_rows.size()) {
            state.current_input_row++;
            state.initialized_row = false;
        }
        
        return OperatorResultType::HAVE_MORE_OUTPUT;
    }
}

// Helper function for LATERAL git_tags_each processing - processes entire git tags for a repository path  
static void ProcessTagsForInOut(const string &input_repo_path, const string &bind_repo_path, vector<GitTagsRow> &rows) {
    // Use input_repo_path from LATERAL context, or bind_repo_path as fallback
    string repo_path = input_repo_path.empty() ? bind_repo_path : input_repo_path;
    
    // Use GitPath::Parse for repository discovery
    string resolved_repo_path;
    try {
        auto git_path = GitPath::Parse("git://" + repo_path + "@HEAD");
        resolved_repo_path = git_path.repository_path;
    } catch (const std::exception &e) {
        throw IOException("Failed to resolve repository path '%s': %s", repo_path, e.what());
    }
    
    git_repository *repo = nullptr;
    vector<string> tag_names;
    
    try {
        // Open repository
        int error = git_repository_open(&repo, resolved_repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s", 
                            resolved_repo_path, e ? e->message : "Unknown error");
        }
        
        // Get all tags
        error = git_tag_foreach(repo, tag_foreach_cb, &tag_names);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to list tags: %s", e ? e->message : "Unknown error");
        }
        
        // Process each tag
        for (const string &tag_name : tag_names) {
            GitTagsRow row;
            row.repo_path = repo_path;
            row.tag_name = tag_name;
            
            // Look up tag reference
            git_reference *tag_ref = nullptr;
            string full_name = "refs/tags/" + tag_name;
            error = git_reference_lookup(&tag_ref, repo, full_name.c_str());
            if (error == 0) {
                const git_oid *oid = git_reference_target(tag_ref);
                if (oid) {
                    char hash_str[GIT_OID_HEXSZ + 1];
                    git_oid_tostr(hash_str, sizeof(hash_str), oid);
                    row.commit_hash = hash_str;
                    
                    // Try to get tag object for annotation info
                    git_tag *tag_obj = nullptr;
                    bool is_annotated = false;
                    if (git_tag_lookup(&tag_obj, repo, oid) == 0) {
                        is_annotated = true;
                        
                        const git_signature *tagger = git_tag_tagger(tag_obj);
                        if (tagger) {
                            row.tagger_name = tagger->name ? tagger->name : "";
                            row.tagger_date = Timestamp::FromEpochSeconds(tagger->when.time);
                        } else {
                            row.tagger_name = "";
                            row.tagger_date = timestamp_t(0);
                        }
                        
                        const char *message = git_tag_message(tag_obj);
                        row.message = message ? message : "";
                        
                        git_tag_free(tag_obj);
                        tag_obj = nullptr;
                    } else {
                        // Lightweight tag
                        row.tagger_name = "";
                        row.tagger_date = timestamp_t(0);
                        row.message = "";
                    }
                    
                    row.is_annotated = is_annotated;
                } else {
                    row.commit_hash = "";
                    row.tagger_name = "";
                    row.tagger_date = timestamp_t(0);
                    row.message = "";
                    row.is_annotated = false;
                }
                
                rows.push_back(row);
                git_reference_free(tag_ref);
                tag_ref = nullptr;
            }
        }
        
    } catch (...) {
        // Cleanup on exception
        if (repo) {
            git_repository_free(repo);
            repo = nullptr;
        }
        throw;
    }
    
    // Normal cleanup
    if (repo) {
        git_repository_free(repo);
        repo = nullptr;
    }
}

// LATERAL git_tags_each function - processes dynamic repository paths
static OperatorResultType GitTagsEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                             DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitTagsFunctionData>();
    auto &state = data_p.local_state->Cast<GitTagsLocalState>();
    
    while (true) {
        if (!state.initialized_row) {
            if (state.current_input_row >= input.size()) {
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            input.Flatten();
            
            string input_repo_path;
            if (input.ColumnCount() == 0) {
                // Zero-argument static call: use bind_data repo_path (defaults to ".")
                input_repo_path = bind_data.repo_path.empty() ? "." : bind_data.repo_path;
            } else {
                // LATERAL call: extract repo_path from input column
                if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                    state.current_input_row++;
                    state.initialized_row = false;
                    continue;
                }
                
                auto data = FlatVector::GetData<string_t>(input.data[0]);
                if (!data) {
                    throw BinderException("git_tags_each: no string data in input column");
                }
                
                auto string_t_value = data[state.current_input_row];
                input_repo_path = string(string_t_value.GetData(), string_t_value.GetSize());
                
                if (input_repo_path.empty()) {
                    throw BinderException("git_tags_each: received empty repository path from input");
                }
            }
            
            state.current_rows.clear();
            ProcessTagsForInOut(input_repo_path, bind_data.repo_path, state.current_rows);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        idx_t output_count = 0;
        while (output_count < STANDARD_VECTOR_SIZE && 
               state.current_output_row < state.current_rows.size()) {
            
            auto &row = state.current_rows[state.current_output_row];
            
            output.SetValue(0, output_count, Value(row.repo_path));
            output.SetValue(1, output_count, Value(row.tag_name));
            output.SetValue(2, output_count, Value(row.commit_hash));
            output.SetValue(3, output_count, Value(row.tagger_name));
            output.SetValue(4, output_count, Value::TIMESTAMP(row.tagger_date));
            output.SetValue(5, output_count, Value(row.message));
            output.SetValue(6, output_count, Value(row.is_annotated));
            
            output_count++;
            state.current_output_row++;
        }
        
        output.SetCardinality(output_count);
        
        if (state.current_output_row >= state.current_rows.size()) {
            state.current_input_row++;
            state.initialized_row = false;
        }
        
        return OperatorResultType::HAVE_MORE_OUTPUT;
    }
}

void RegisterGitLogFunction(DatabaseInstance &db) {
    // Single-argument version (existing)
    TableFunction git_log_func("git_log", {LogicalType::VARCHAR}, GitLogFunction, GitLogBind, GitLogInitGlobal);
    git_log_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_log_func);
    
    // Zero-argument version (defaults to current directory)
    TableFunction git_log_func_zero("git_log", {}, GitLogFunction, GitLogBind, GitLogInitGlobal);
    git_log_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_log_func_zero);
    
    // LATERAL git_log_each function (commit ref comes from LATERAL context) - ONLY for dynamic input
    TableFunctionSet git_log_each_set("git_log_each");
    
    // Version that takes commit ref as first parameter (for LATERAL context)
    TableFunction git_log_each_single({LogicalType::VARCHAR}, nullptr, GitLogBind, nullptr, GitLogLocalInit);
    git_log_each_single.in_out_function = GitLogEachFunction;
    git_log_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_log_each_set.AddFunction(git_log_each_single);
    
    // Two-argument version (ref, repo_path)
    TableFunction git_log_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitLogBind, nullptr, GitLogLocalInit);
    git_log_each_two.in_out_function = GitLogEachFunction;
    git_log_each_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_log_each_set.AddFunction(git_log_each_two);
    
    ExtensionUtil::RegisterFunction(db, git_log_each_set);
}

void RegisterGitBranchesFunction(DatabaseInstance &db) {
    // Single-argument version (existing)
    TableFunction git_branches_func("git_branches", {LogicalType::VARCHAR}, GitBranchesFunction, GitBranchesBind, GitBranchesInitGlobal);
    git_branches_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_branches_func);
    
    // Zero-argument version (defaults to current directory)
    TableFunction git_branches_func_zero("git_branches", {}, GitBranchesFunction, GitBranchesBind, GitBranchesInitGlobal);
    git_branches_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_branches_func_zero);
    
    // LATERAL git_branches_each function (repository path comes from LATERAL context) - ONLY for dynamic input
    TableFunctionSet git_branches_each_set("git_branches_each");
    
    // Zero-argument version (defaults to current directory)
    TableFunction git_branches_each_zero({}, nullptr, GitBranchesBind, nullptr, GitBranchesLocalInit);
    git_branches_each_zero.in_out_function = GitBranchesEachFunction;
    git_branches_each_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_branches_each_set.AddFunction(git_branches_each_zero);
    
    // Version that takes repository path as first parameter (for LATERAL context)
    TableFunction git_branches_each_single({LogicalType::VARCHAR}, nullptr, GitBranchesBind, nullptr, GitBranchesLocalInit);
    git_branches_each_single.in_out_function = GitBranchesEachFunction;
    git_branches_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_branches_each_set.AddFunction(git_branches_each_single);
    
    // Two-argument version (repo_path, repo_path)
    TableFunction git_branches_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitBranchesBind, nullptr, GitBranchesLocalInit);
    git_branches_each_two.in_out_function = GitBranchesEachFunction;
    git_branches_each_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_branches_each_set.AddFunction(git_branches_each_two);
    
    ExtensionUtil::RegisterFunction(db, git_branches_each_set);
}

void RegisterGitTagsFunction(DatabaseInstance &db) {
    // Single-argument version (existing)
    TableFunction git_tags_func("git_tags", {LogicalType::VARCHAR}, GitTagsFunction, GitTagsBind, GitTagsInitGlobal);
    git_tags_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_tags_func);
    
    // Zero-argument version (defaults to current directory)
    TableFunction git_tags_func_zero("git_tags", {}, GitTagsFunction, GitTagsBind, GitTagsInitGlobal);
    git_tags_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_tags_func_zero);
    
    // LATERAL git_tags_each function (repository path comes from LATERAL context) - ONLY for dynamic input
    TableFunctionSet git_tags_each_set("git_tags_each");
    
    // Zero-argument version (defaults to current directory)
    TableFunction git_tags_each_zero({}, nullptr, GitTagsBind, nullptr, GitTagsLocalInit);
    git_tags_each_zero.in_out_function = GitTagsEachFunction;
    git_tags_each_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tags_each_set.AddFunction(git_tags_each_zero);
    
    // Version that takes repository path as first parameter (for LATERAL context)
    TableFunction git_tags_each_single({LogicalType::VARCHAR}, nullptr, GitTagsBind, nullptr, GitTagsLocalInit);
    git_tags_each_single.in_out_function = GitTagsEachFunction;
    git_tags_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tags_each_set.AddFunction(git_tags_each_single);
    
    // Two-argument version (repo_path, repo_path)
    TableFunction git_tags_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitTagsBind, nullptr, GitTagsLocalInit);
    git_tags_each_two.in_out_function = GitTagsEachFunction;
    git_tags_each_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tags_each_set.AddFunction(git_tags_each_two);
    
    ExtensionUtil::RegisterFunction(db, git_tags_each_set);
}

//===--------------------------------------------------------------------===//
// Git Tree Each LATERAL Function (for dynamic parameters)
//===--------------------------------------------------------------------===//

struct GitTreeLocalState : public LocalTableFunctionState {
    vector<GitTreeRow> current_rows;
    idx_t current_output_row = 0;
    idx_t current_input_row = 0;
    string repo_path;
    bool initialized_row = false;
};

static unique_ptr<LocalTableFunctionState> GitTreeLocalInit(ExecutionContext &context,
                                                           TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_state) {
    return make_uniq<GitTreeLocalState>();
}

// Helper function for LATERAL git_tree_each processing
static void ProcessTreeCommitForInOut(const string &commit_ref, const string &repo_path, vector<GitTreeRow> &rows) {
    git_libgit2_init();
    git_repository *repo = nullptr;
    
    try {
        if (git_repository_open(&repo, repo_path.c_str()) != 0) {
            git_libgit2_shutdown();
            return;
        }
        
        ProcessSingleCommit(repo, commit_ref, repo_path, rows);
        git_repository_free(repo);
    } catch (...) {
        if (repo) {
            git_repository_free(repo);
        }
    }
    
    git_libgit2_shutdown();
}

static OperatorResultType GitTreeEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                              DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitTreeFunctionData>();
    auto &state = data_p.local_state->Cast<GitTreeLocalState>();
    
    while (true) {
        if (!state.initialized_row) {
            // initialize for the current input row
            if (state.current_input_row >= input.size()) {
                // ran out of rows
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // LATERAL function: ALWAYS extract commit ref from input DataChunk
            input.Flatten();
            
            // Check if input has columns and data
            if (input.ColumnCount() == 0) {
                throw BinderException("git_tree_each: no input columns available");
            }
            
            // Check if the input is null
            if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                // Move to next input row if null
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Extract commit ref from input DataChunk - direct string_t access
            auto data = FlatVector::GetData<string_t>(input.data[0]);
            if (!data) {
                throw BinderException("git_tree_each: no string data in input column");
            }
            
            // Get the string_t directly and convert to string
            auto string_t_value = data[state.current_input_row];
            string commit_ref(string_t_value.GetData(), string_t_value.GetSize());
            
            if (commit_ref.empty()) {
                throw BinderException("git_tree_each: received empty commit reference from input");
            }
            
            // Process the git tree for this commit using the DRY shared logic
            state.current_rows.clear();
            ProcessTreeCommitForInOut(commit_ref, bind_data.repo_path, state.current_rows);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        if (state.current_output_row >= state.current_rows.size()) {
            // Finished outputting all rows for this input row, move to next
            state.current_input_row++;
            state.initialized_row = false;
            continue;
        }
        
        // Output rows for current input
        idx_t remaining = state.current_rows.size() - state.current_output_row;
        idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
        
        for (idx_t i = 0; i < count; i++) {
            auto &row = state.current_rows[state.current_output_row + i];
            output.SetValue(0, i, Value(row.commit_hash));           // commit_hash
            output.SetValue(1, i, Value::TIMESTAMP(row.commit_date)); // commit_date
            output.SetValue(2, i, Value(row.path));                  // path
            output.SetValue(3, i, Value::INTEGER(row.mode));         // mode
            output.SetValue(4, i, Value(row.blob_hash));             // blob_hash
            output.SetValue(5, i, Value::BIGINT(row.size));          // size
            output.SetValue(6, i, Value(row.git_file_uri));          // git_file_uri
        }
        
        output.SetCardinality(count);
        state.current_output_row += count;
        
        if (state.current_output_row >= state.current_rows.size()) {
            // Finished this input row, setup to move to next
            state.current_input_row++;
            state.initialized_row = false;
        }
        
        return OperatorResultType::HAVE_MORE_OUTPUT;
    }
}

void RegisterGitTreeFunction(DatabaseInstance &db) {
    TableFunctionSet git_tree_set("git_tree");
    
    // Enhanced single commit version
    TableFunction git_tree_single({LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_set.AddFunction(git_tree_single);
    
    // Two-argument version (ref, repo_path)
    TableFunction git_tree_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_set.AddFunction(git_tree_two);
    
    // Array version (multiple commits) - WORKING!
    TableFunction git_tree_array({LogicalType::LIST(LogicalType::VARCHAR)}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_array.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_set.AddFunction(git_tree_array);
    
    // Zero-argument version (defaults to HEAD and current directory)
    TableFunction git_tree_zero({}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_set.AddFunction(git_tree_zero);
    
    ExtensionUtil::RegisterFunction(db, git_tree_set);
    
    // LATERAL git_tree_each function (commit ref comes from LATERAL context) - ONLY for dynamic input
    TableFunctionSet git_tree_each_set("git_tree_each");
    
    // Version that takes commit ref as first parameter (for LATERAL context)
    TableFunction git_tree_each_single({LogicalType::VARCHAR}, nullptr, GitTreeBind, nullptr, GitTreeLocalInit);
    git_tree_each_single.in_out_function = GitTreeEachFunction;
    git_tree_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_each_set.AddFunction(git_tree_each_single);
    
    // Two-argument version (ref, repo_path)
    TableFunction git_tree_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitTreeBind, nullptr, GitTreeLocalInit);
    git_tree_each_two.in_out_function = GitTreeEachFunction;
    git_tree_each_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_each_set.AddFunction(git_tree_each_two);
    
    // Array version (multiple commits)
    TableFunction git_tree_each_array({LogicalType::LIST(LogicalType::VARCHAR)}, nullptr, GitTreeBind, nullptr, GitTreeLocalInit);
    git_tree_each_array.in_out_function = GitTreeEachFunction;
    git_tree_each_array.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_tree_each_set.AddFunction(git_tree_each_array);
    
    ExtensionUtil::RegisterFunction(db, git_tree_each_set);
}

void RegisterGitParentsFunction(DatabaseInstance &db) {
    // Single-argument version (ref only, for current directory)
    TableFunction git_parents_func_one("git_parents", {LogicalType::VARCHAR}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func_one.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    ExtensionUtil::RegisterFunction(db, git_parents_func_one);
    
    // Two-parameter version (ref, repo_path)
    TableFunction git_parents_func_two("git_parents", {LogicalType::VARCHAR, LogicalType::VARCHAR}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func_two.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    ExtensionUtil::RegisterFunction(db, git_parents_func_two);
    
    // Array version (multiple commits) - for consistency with git_tree
    TableFunction git_parents_func_array("git_parents", {LogicalType::LIST(LogicalType::VARCHAR)}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func_array.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    ExtensionUtil::RegisterFunction(db, git_parents_func_array);
    
    // Zero-argument version (defaults to HEAD and current directory)
    TableFunction git_parents_func_zero("git_parents", {}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func_zero.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    ExtensionUtil::RegisterFunction(db, git_parents_func_zero);
}

//===--------------------------------------------------------------------===//
// Git Read Functions (both static and LATERAL support for reading blob content)
//===--------------------------------------------------------------------===//

// Common bind data for both static and LATERAL git_read functions
struct GitReadBindData : public TableFunctionData {
    int64_t max_bytes;
    string decode_base64;
    string transcode;
    string filters;
    string repo_path;
    string uri;  // For static git_read function
    
    GitReadBindData(int64_t max_bytes, const string& decode_base64, const string& transcode, 
                   const string& filters, const string& repo_path, const string& uri = "")
        : max_bytes(max_bytes), decode_base64(decode_base64), transcode(transcode), 
          filters(filters), repo_path(repo_path), uri(uri) {}
};

// Global state for static git_read function
struct GitReadGlobalState : public GlobalTableFunctionState {
    GitReadGlobalState() : finished(false) {}
    bool finished;
};

struct GitReadLocalState : public LocalTableFunctionState {
    GitReadLocalState() = default;
    
    bool initialized_row = false;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    
    git_repository *repo = nullptr;
    
    struct ReadResult {
        string uri;
        int32_t mode;
        string kind;
        bool is_text;
        string encoding;
        int64_t size_bytes;
        bool truncated;
        string text;
        string blob;
    };
    
    vector<ReadResult> current_results;
    
    ~GitReadLocalState() {
        if (repo) {
            git_repository_free(repo);
        }
    }
};

// Init functions
static unique_ptr<GlobalTableFunctionState> GitReadInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GitReadGlobalState>();
}

// Helper function to parse git:// URI format
static bool ParseGitURI(const string& uri, string& path, string& commit_hash) {
    // Expected format: git://path/to/file@commit_hash
    if (!StringUtil::StartsWith(uri, "git://")) {
        return false;
    }
    
    auto at_pos = uri.find_last_of('@');
    if (at_pos == string::npos) {
        return false;
    }
    
    path = uri.substr(6, at_pos - 6);  // Remove "git://" prefix
    commit_hash = uri.substr(at_pos + 1);
    
    return true;
}

// Helper function to detect if content is text
static bool IsTextContent(const char* data, size_t size) {
    // Check for null bytes (strong indicator of binary data)
    for (size_t i = 0; i < size; i++) {
        if (data[i] == 0) {
            return false;
        }
    }
    
    // Check for valid UTF-8 sequences and reasonable control characters
    size_t i = 0;
    while (i < size) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        
        // ASCII printable characters and common whitespace
        if (c < 128) {
            // Allow printable ASCII and common control chars: tab, newline, carriage return
            if (c >= 32 || c == 9 || c == 10 || c == 13) {
                i++;
                continue;
            } else {
                return false;  // Unusual control character
            }
        }
        
        // Check for valid UTF-8 multi-byte sequences
        int bytes_in_sequence = 0;
        if ((c & 0xE0) == 0xC0) {  // 110xxxxx - 2 byte sequence
            bytes_in_sequence = 2;
        } else if ((c & 0xF0) == 0xE0) {  // 1110xxxx - 3 byte sequence
            bytes_in_sequence = 3;
        } else if ((c & 0xF8) == 0xF0) {  // 11110xxx - 4 byte sequence
            bytes_in_sequence = 4;
        } else {
            return false;  // Invalid UTF-8 start byte
        }
        
        // Validate the continuation bytes
        if (i + bytes_in_sequence > size) {
            return false;  // Incomplete sequence
        }
        
        for (int j = 1; j < bytes_in_sequence; j++) {
            unsigned char continuation = static_cast<unsigned char>(data[i + j]);
            if ((continuation & 0xC0) != 0x80) {  // Must be 10xxxxxx
                return false;  // Invalid continuation byte
            }
        }
        
        i += bytes_in_sequence;
    }
    
    return true;
}

// Helper function to process a git:// URI and extract content
static void ProcessGitURI(const string& uri, const GitReadBindData& bind_data, 
                         GitReadLocalState::ReadResult& result) {
    result.uri = uri;
    result.mode = 0;
    result.kind = "unknown";
    result.is_text = false;
    result.encoding = "unknown";
    result.size_bytes = 0;
    result.truncated = false;
    result.text = "";
    result.blob = "";
    
    string path, commit_hash;
    if (!ParseGitURI(uri, path, commit_hash)) {
        throw BinderException("git_read: invalid git:// URI format '%s'", uri);
    }
    
    git_repository *repo = nullptr;
    git_commit *commit = nullptr;
    git_tree *tree = nullptr;
    git_tree_entry *entry = nullptr;
    git_blob *blob = nullptr;
    
    try {
        // Open repository
        int error = git_repository_open(&repo, bind_data.repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("git_read: failed to open git repository '%s': %s", 
                            bind_data.repo_path, e ? e->message : "Unknown error");
        }
        
        // Resolve commit (handle both SHA hashes and references like HEAD)
        git_oid commit_oid;
        git_object *obj = nullptr;
        error = git_revparse_single(&obj, repo, commit_hash.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            git_repository_free(repo); repo = nullptr;
            throw IOException("git_read: failed to resolve commit reference '%s': %s", 
                            commit_hash, e ? e->message : "Unknown error");
        }
        
        const git_oid *oid = git_object_id(obj);
        git_oid_cpy(&commit_oid, oid);
        git_object_free(obj);
        
        error = git_commit_lookup(&commit, repo, &commit_oid);
        if (error != 0) {
            const git_error *e = git_error_last();
            git_repository_free(repo); repo = nullptr;
            throw IOException("git_read: commit not found '%s': %s", 
                            commit_hash, e ? e->message : "Unknown error");
        }
        
        // Get tree from commit
        error = git_commit_tree(&tree, commit);
        if (error != 0) {
            const git_error *e = git_error_last();
            git_commit_free(commit); commit = nullptr;
            git_repository_free(repo); repo = nullptr;
            throw IOException("git_read: failed to get commit tree: %s", 
                            e ? e->message : "Unknown error");
        }
        
        // Find the file in the tree
        error = git_tree_entry_bypath(&entry, tree, path.c_str());
        if (error != 0) {
            git_tree_free(tree); tree = nullptr;
            git_commit_free(commit); commit = nullptr;
            git_repository_free(repo); repo = nullptr;
            throw IOException("git_read: file not found '%s' in commit '%s'", path, commit_hash);
        }
        
        // Get file mode and kind
        git_filemode_t filemode = git_tree_entry_filemode(entry);
        result.mode = static_cast<int32_t>(filemode);
        
        switch (filemode) {
            case GIT_FILEMODE_BLOB:
            case GIT_FILEMODE_BLOB_EXECUTABLE:
                result.kind = "file";
                break;
            case GIT_FILEMODE_LINK:
                result.kind = "symlink";
                break;
            case GIT_FILEMODE_TREE:
                result.kind = "tree";
                git_tree_entry_free(entry); entry = nullptr;
                git_tree_free(tree); tree = nullptr;
                git_commit_free(commit); commit = nullptr;
                git_repository_free(repo); repo = nullptr;
                return;
            case GIT_FILEMODE_COMMIT:
                result.kind = "submodule";
                git_tree_entry_free(entry); entry = nullptr;
                git_tree_free(tree); tree = nullptr;
                git_commit_free(commit); commit = nullptr;
                git_repository_free(repo); repo = nullptr;
                return;
            default:
                git_tree_entry_free(entry); entry = nullptr;
                git_tree_free(tree); tree = nullptr;
                git_commit_free(commit); commit = nullptr;
                git_repository_free(repo); repo = nullptr;
                throw IOException("git_read: unsupported file mode %d", static_cast<int>(filemode));
        }
        
        // Get the blob
        const git_oid *blob_oid = git_tree_entry_id(entry);
        error = git_blob_lookup(&blob, repo, blob_oid);
        if (error != 0) {
            const git_error *e = git_error_last();
            git_tree_entry_free(entry); entry = nullptr;
            git_tree_free(tree); tree = nullptr;
            git_commit_free(commit); commit = nullptr;
            git_repository_free(repo); repo = nullptr;
            throw IOException("git_read: failed to load blob: %s", 
                            e ? e->message : "Unknown error");
        }
        
        // Get blob content
        const void *raw_content = git_blob_rawcontent(blob);
        git_off_t raw_size = git_blob_rawsize(blob);
        
        result.size_bytes = static_cast<int64_t>(raw_size);
        
        // Apply max_bytes limit
        size_t content_size = static_cast<size_t>(raw_size);
        if (bind_data.max_bytes > 0 && content_size > static_cast<size_t>(bind_data.max_bytes)) {
            content_size = static_cast<size_t>(bind_data.max_bytes);
            result.truncated = true;
        }
        
        if (content_size > 0) {
            // Determine if content is text or binary
            bool is_text = IsTextContent(static_cast<const char*>(raw_content), content_size);
            result.is_text = is_text;
            
            if (is_text) {
                result.encoding = "utf8";
                result.text = string(static_cast<const char*>(raw_content), content_size);
            } else {
                result.encoding = "binary";
                result.blob = string(static_cast<const char*>(raw_content), content_size);
            }
        }
        
        // Clean up
        git_blob_free(blob); blob = nullptr;
        git_tree_entry_free(entry); entry = nullptr;
        git_tree_free(tree); tree = nullptr;
        git_commit_free(commit); commit = nullptr;
        git_repository_free(repo); repo = nullptr;
        
    } catch (...) {
        // Clean up on exception
        if (blob) git_blob_free(blob);
        if (entry) git_tree_entry_free(entry);
        if (tree) git_tree_free(tree);
        if (commit) git_commit_free(commit);
        if (repo) git_repository_free(repo);
        throw; // Re-throw the exception
    }
}

// Bind function for static git_read (single URI parameter)
static unique_ptr<FunctionData> GitReadBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
    
    // Extract URI parameter
    if (input.inputs.empty()) {
        throw BinderException("git_read requires at least one parameter: the URI");
    }
    
    string uri = input.inputs[0].GetValue<string>();
    
    // Set default parameters
    int64_t max_bytes = -1;  // No limit by default
    string decode_base64 = "auto";
    string transcode = "utf8";
    string filters = "raw";
    string repo_path = ".";
    
    // Parse optional parameters
    if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
        max_bytes = input.inputs[1].GetValue<int64_t>();
    }
    if (input.inputs.size() >= 3 && !input.inputs[2].IsNull()) {
        decode_base64 = input.inputs[2].GetValue<string>();
    }
    if (input.inputs.size() >= 4 && !input.inputs[3].IsNull()) {
        transcode = input.inputs[3].GetValue<string>();
    }
    if (input.inputs.size() >= 5 && !input.inputs[4].IsNull()) {
        filters = input.inputs[4].GetValue<string>();
    }
    
    // Check for repo_path named parameter
    for (const auto &kv : input.named_parameters) {
        if (kv.first == "repo_path") {
            repo_path = kv.second.GetValue<string>();
        }
    }
    
    // Define return schema
    return_types = {
        LogicalType::VARCHAR,  // uri
        LogicalType::INTEGER,  // mode
        LogicalType::VARCHAR,  // kind
        LogicalType::BOOLEAN,  // is_text
        LogicalType::VARCHAR,  // encoding
        LogicalType::BIGINT,   // size_bytes
        LogicalType::BOOLEAN,  // truncated
        LogicalType::VARCHAR,  // text
        LogicalType::BLOB      // blob
    };
    
    names = {"uri", "mode", "kind", "is_text", "encoding", "size_bytes", 
             "truncated", "text", "blob"};
    
    return make_uniq<GitReadBindData>(max_bytes, decode_base64, transcode, filters, repo_path, uri);
}

// Static git_read execution function (processes single URI from bind data)
static void GitReadFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &bind_data = input.bind_data->Cast<GitReadBindData>();
    auto &gstate = input.global_state->Cast<GitReadGlobalState>();
    
    if (gstate.finished) {
        output.SetCardinality(0);
        return;
    }
    
    // Process the URI from bind data
    GitReadLocalState::ReadResult result;
    ProcessGitURI(bind_data.uri, bind_data, result);
    
    // Fill output row
    FlatVector::GetData<string_t>(output.data[0])[0] = 
        StringVector::AddString(output.data[0], result.uri);
    FlatVector::GetData<int32_t>(output.data[1])[0] = result.mode;
    FlatVector::GetData<string_t>(output.data[2])[0] = 
        StringVector::AddString(output.data[2], result.kind);
    FlatVector::GetData<bool>(output.data[3])[0] = result.is_text;
    FlatVector::GetData<string_t>(output.data[4])[0] = 
        StringVector::AddString(output.data[4], result.encoding);
    FlatVector::GetData<int64_t>(output.data[5])[0] = result.size_bytes;
    FlatVector::GetData<bool>(output.data[6])[0] = result.truncated;
    
    if (!result.text.empty()) {
        FlatVector::GetData<string_t>(output.data[7])[0] = 
            StringVector::AddString(output.data[7], result.text);
    } else {
        FlatVector::SetNull(output.data[7], 0, true);
    }
    
    if (!result.blob.empty()) {
        FlatVector::GetData<string_t>(output.data[8])[0] = 
            StringVector::AddStringOrBlob(output.data[8], result.blob);
    } else {
        FlatVector::SetNull(output.data[8], 0, true);
    }
    
    output.SetCardinality(1);
    gstate.finished = true;
}

// Bind function for git_read_each (Pure LATERAL function)
static unique_ptr<FunctionData> GitReadEachBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
    
    // Set default parameters - URI comes from input DataChunk at runtime
    int64_t max_bytes = -1;  // No limit by default
    string decode_base64 = "auto";
    string transcode = "utf8";
    string filters = "raw";
    string repo_path = ".";
    
    // Handle optional parameters (URI is NOT a bind parameter - comes from LATERAL context)
    // First parameter is URI from LATERAL, so optional params start at index 1
    if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
        max_bytes = input.inputs[1].GetValue<int64_t>();
    }
    if (input.inputs.size() >= 3 && !input.inputs[2].IsNull()) {
        decode_base64 = input.inputs[2].GetValue<string>();
    }
    if (input.inputs.size() >= 4 && !input.inputs[3].IsNull()) {
        transcode = input.inputs[3].GetValue<string>();
    }
    if (input.inputs.size() >= 5 && !input.inputs[4].IsNull()) {
        filters = input.inputs[4].GetValue<string>();
    }
    
    // Check for repo_path named parameter
    for (const auto &kv : input.named_parameters) {
        if (kv.first == "repo_path") {
            repo_path = kv.second.GetValue<string>();
        }
    }
    
    // Define return schema
    return_types = {
        LogicalType::VARCHAR,  // uri
        LogicalType::INTEGER,  // mode
        LogicalType::VARCHAR,  // kind
        LogicalType::BOOLEAN,  // is_text
        LogicalType::VARCHAR,  // encoding
        LogicalType::BIGINT,   // size_bytes
        LogicalType::BOOLEAN,  // truncated
        LogicalType::VARCHAR,  // text
        LogicalType::BLOB      // blob
    };
    
    names = {"uri", "mode", "kind", "is_text", "encoding", "size_bytes", 
             "truncated", "text", "blob"};
    
    // Don't store URI in bind_data - it always comes from input DataChunk for LATERAL functions
    return make_uniq<GitReadBindData>(max_bytes, decode_base64, transcode, filters, repo_path, "");
}

static unique_ptr<LocalTableFunctionState> GitReadLocalInit(ExecutionContext &context, 
                                                           TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_state) {
    return make_uniq<GitReadLocalState>();
}

static OperatorResultType GitReadEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                        DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitReadBindData>();
    auto &state = data_p.local_state->Cast<GitReadLocalState>();
    
    while (true) {
        if (!state.initialized_row) {
            // initialize for the current input row
            if (state.current_input_row >= input.size()) {
                // ran out of rows
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // LATERAL function: ALWAYS extract URI from input DataChunk
            input.Flatten();
            
            // Check if input has columns and data
            if (input.ColumnCount() == 0) {
                throw BinderException("git_read_each: no input columns available");
            }
            
            // Check if the input is null
            if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                // Move to next input row if null
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Extract URI from input DataChunk - direct string_t access
            auto data = FlatVector::GetData<string_t>(input.data[0]);
            if (!data) {
                throw BinderException("git_read_each: no string data in input column");
            }
            
            // Get the string_t directly and convert to string
            auto string_t_value = data[state.current_input_row];
            string uri(string_t_value.GetData(), string_t_value.GetSize());
            
            if (uri.empty()) {
                throw BinderException("git_read_each: received empty URI from input");
            }
            
            // Process the URI and extract content
            state.current_results.clear();
            GitReadLocalState::ReadResult result;
            ProcessGitURI(uri, bind_data, result);
            state.current_results.push_back(result);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        // Output results for current input row
        idx_t output_count = 0;
        while (state.current_output_row < state.current_results.size() && output_count < STANDARD_VECTOR_SIZE) {
            auto &result = state.current_results[state.current_output_row];
            
            // Fill output columns
            FlatVector::GetData<string_t>(output.data[0])[output_count] = 
                StringVector::AddString(output.data[0], result.uri);
            FlatVector::GetData<int32_t>(output.data[1])[output_count] = result.mode;
            FlatVector::GetData<string_t>(output.data[2])[output_count] = 
                StringVector::AddString(output.data[2], result.kind);
            FlatVector::GetData<bool>(output.data[3])[output_count] = result.is_text;
            FlatVector::GetData<string_t>(output.data[4])[output_count] = 
                StringVector::AddString(output.data[4], result.encoding);
            FlatVector::GetData<int64_t>(output.data[5])[output_count] = result.size_bytes;
            FlatVector::GetData<bool>(output.data[6])[output_count] = result.truncated;
            
            if (!result.text.empty()) {
                FlatVector::GetData<string_t>(output.data[7])[output_count] = 
                    StringVector::AddString(output.data[7], result.text);
            } else {
                FlatVector::SetNull(output.data[7], output_count, true);
            }
            
            if (!result.blob.empty()) {
                FlatVector::GetData<string_t>(output.data[8])[output_count] = 
                    StringVector::AddStringOrBlob(output.data[8], result.blob);
            } else {
                FlatVector::SetNull(output.data[8], output_count, true);
            }
            
            output_count++;
            state.current_output_row++;
        }
        
        if (output_count > 0) {
            output.SetCardinality(output_count);
            
            if (state.current_output_row >= state.current_results.size()) {
                // Done with this input row, move to next
                state.current_input_row++;
                state.initialized_row = false;
            }
            
            return OperatorResultType::HAVE_MORE_OUTPUT;
        }
        
        // Move to next input row
        state.current_input_row++;
        state.initialized_row = false;
    }
}

void RegisterGitReadFunction(DatabaseInstance &db) {
    // Static git_read function (processes literal URIs)
    // Correct pattern: TableFunction(name, args, function, bind, init_global)
    TableFunctionSet git_read_set("git_read");
    
    TableFunction git_read_1({LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_1.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_1);
    
    TableFunction git_read_2({LogicalType::VARCHAR, LogicalType::BIGINT}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_2.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_2);
    
    TableFunction git_read_3({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_3.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_3);
    
    TableFunction git_read_4({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_4.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_4);
    
    TableFunction git_read_5({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_5.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_5);
    
    ExtensionUtil::RegisterFunction(db, git_read_set);
    
    // LATERAL git_read_each function (URI comes from LATERAL context)
    TableFunctionSet git_read_each_set("git_read_each");
    
    // Version that takes URI as first parameter (for LATERAL context)
    TableFunction git_read_each_1({LogicalType::VARCHAR}, nullptr, GitReadEachBind, nullptr, GitReadLocalInit);
    git_read_each_1.in_out_function = GitReadEachFunction;
    git_read_each_1.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_each_set.AddFunction(git_read_each_1);
    
    // Version with URI and max_bytes parameters
    TableFunction git_read_each_2({LogicalType::VARCHAR, LogicalType::BIGINT}, nullptr, GitReadEachBind, nullptr, GitReadLocalInit);
    git_read_each_2.in_out_function = GitReadEachFunction;
    git_read_each_2.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_each_set.AddFunction(git_read_each_2);
    
    ExtensionUtil::RegisterFunction(db, git_read_each_set);
}

//===--------------------------------------------------------------------===//
// git_uri() Scalar Function - Helper for URI construction
//===--------------------------------------------------------------------===//

static void GitUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    // Handle case where all inputs are constant - create constant output
    if (args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR &&
        args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR &&
        args.data[2].GetVectorType() == VectorType::CONSTANT_VECTOR) {
        
        auto repo_path_value = ConstantVector::GetData<string_t>(args.data[0]);
        auto file_path_value = ConstantVector::GetData<string_t>(args.data[1]);
        auto commit_ref_value = ConstantVector::GetData<string_t>(args.data[2]);
        
        if (ConstantVector::IsNull(args.data[0]) || 
            ConstantVector::IsNull(args.data[1]) ||
            ConstantVector::IsNull(args.data[2])) {
            ConstantVector::SetNull(result, true);
            return;
        }
        
        string repo_path = repo_path_value->GetString();
        string file_path = file_path_value->GetString();  
        string commit_ref = commit_ref_value->GetString();
        
        // Construct git:// URI: git://<repo_path>/<file_path>@<commit_ref>
        string uri = "git://" + repo_path;
        if (!file_path.empty()) {
            // Add separator if repo_path doesn't end with / and file_path doesn't start with /
            if (!repo_path.empty() && repo_path.back() != '/' && file_path[0] != '/') {
                uri += "/";
            }
            uri += file_path;
        }
        uri += "@" + commit_ref;
        
        result.SetVectorType(VectorType::CONSTANT_VECTOR);
        auto result_data = ConstantVector::GetData<string_t>(result);
        *result_data = StringVector::AddString(result, uri);
        return;
    }
    
    // Handle general case with flat vectors
    auto &repo_path_vector = args.data[0];
    auto &file_path_vector = args.data[1]; 
    auto &commit_ref_vector = args.data[2];
    
    UnifiedVectorFormat repo_path_format, file_path_format, commit_ref_format;
    repo_path_vector.ToUnifiedFormat(args.size(), repo_path_format);
    file_path_vector.ToUnifiedFormat(args.size(), file_path_format);
    commit_ref_vector.ToUnifiedFormat(args.size(), commit_ref_format);
    
    auto repo_path_data = UnifiedVectorFormat::GetData<string_t>(repo_path_format);
    auto file_path_data = UnifiedVectorFormat::GetData<string_t>(file_path_format);
    auto commit_ref_data = UnifiedVectorFormat::GetData<string_t>(commit_ref_format);
    
    auto result_data = FlatVector::GetData<string_t>(result);
    for (idx_t i = 0; i < args.size(); i++) {
        auto repo_idx = repo_path_format.sel->get_index(i);
        auto file_idx = file_path_format.sel->get_index(i);
        auto commit_idx = commit_ref_format.sel->get_index(i);
        
        if (!repo_path_format.validity.RowIsValid(repo_idx) || 
            !file_path_format.validity.RowIsValid(file_idx) ||
            !commit_ref_format.validity.RowIsValid(commit_idx)) {
            FlatVector::SetNull(result, i, true);
            continue;
        }
        
        string repo_path = repo_path_data[repo_idx].GetString();
        string file_path = file_path_data[file_idx].GetString();  
        string commit_ref = commit_ref_data[commit_idx].GetString();
        
        // Construct git:// URI: git://<repo_path>/<file_path>@<commit_ref>
        string uri = "git://" + repo_path;
        if (!file_path.empty()) {
            // Add separator if repo_path doesn't end with / and file_path doesn't start with /
            if (!repo_path.empty() && repo_path.back() != '/' && file_path[0] != '/') {
                uri += "/";
            }
            uri += file_path;
        }
        uri += "@" + commit_ref;
        
        result_data[i] = StringVector::AddString(result, uri);
    }
}

static void RegisterGitUriFunction(DatabaseInstance &db) {
    auto git_uri_func = ScalarFunction("git_uri",
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        LogicalType::VARCHAR,
        GitUriFunction);
    ExtensionUtil::RegisterFunction(db, git_uri_func);
}

void RegisterGitFunctions(DatabaseInstance &db) {
    RegisterGitLogFunction(db);
    RegisterGitBranchesFunction(db);
    RegisterGitTagsFunction(db);
    RegisterGitTreeFunction(db);
    RegisterGitParentsFunction(db);
    RegisterGitReadFunction(db);
    RegisterGitUriFunction(db);
}

} // namespace duckdb