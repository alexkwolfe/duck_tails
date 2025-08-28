#include "git_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Git Log Function
//===--------------------------------------------------------------------===//

GitLogFunctionData::GitLogFunctionData(const string &repo_path) 
    : repo_path(repo_path), repo(nullptr), walker(nullptr), initialized(false) {
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
    
    // Define return schema
    return_types = {
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
        "commit_hash", "author_name", "author_email", "committer_name", "committer_email",
        "author_date", "commit_date", "message", "parent_count", "tree_hash"
    };
    
    return make_uniq<GitLogFunctionData>(repo_path);
}

unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &data = (GitLogFunctionData &)*data_p.bind_data;
    
    if (!data.initialized) {
        // Open repository
        int error = git_repository_open(&data.repo, data.repo_path.c_str());
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
        
        // Get commit hash
        char hash_str[GIT_OID_HEXSZ + 1];
        git_oid_tostr(hash_str, sizeof(hash_str), &oid);
        output.SetValue(0, count, Value(hash_str));
        
        // Get author info
        const git_signature *author = git_commit_author(commit);
        output.SetValue(1, count, Value(author->name ? author->name : ""));
        output.SetValue(2, count, Value(author->email ? author->email : ""));
        
        // Get committer info
        const git_signature *committer = git_commit_committer(commit);
        output.SetValue(3, count, Value(committer->name ? committer->name : ""));
        output.SetValue(4, count, Value(committer->email ? committer->email : ""));
        
        // Get timestamps
        timestamp_t author_ts = Timestamp::FromEpochSeconds(author->when.time);
        timestamp_t commit_ts = Timestamp::FromEpochSeconds(committer->when.time);
        output.SetValue(5, count, Value::TIMESTAMP(author_ts));
        output.SetValue(6, count, Value::TIMESTAMP(commit_ts));
        
        // Get commit message
        const char *message = git_commit_message(commit);
        output.SetValue(7, count, Value(message ? message : ""));
        
        // Get parent count
        unsigned int parent_count = git_commit_parentcount(commit);
        output.SetValue(8, count, Value::INTEGER(parent_count));
        
        // Get tree hash
        const git_oid *tree_oid = git_commit_tree_id(commit);
        char tree_hash[GIT_OID_HEXSZ + 1];
        git_oid_tostr(tree_hash, sizeof(tree_hash), tree_oid);
        output.SetValue(9, count, Value(tree_hash));
        
        git_commit_free(commit);
        count++;
    }
    
    output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Git Branches Function
//===--------------------------------------------------------------------===//

GitBranchesFunctionData::GitBranchesFunctionData(const string &repo_path)
    : repo_path(repo_path), repo(nullptr), iterator(nullptr), initialized(false) {
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
    
    return_types = {
        LogicalType::VARCHAR,  // branch_name
        LogicalType::VARCHAR,  // commit_hash
        LogicalType::BOOLEAN,  // is_current
        LogicalType::BOOLEAN   // is_remote
    };
    
    names = {"branch_name", "commit_hash", "is_current", "is_remote"};
    
    return make_uniq<GitBranchesFunctionData>(repo_path);
}

unique_ptr<GlobalTableFunctionState> GitBranchesInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

void GitBranchesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &data = (GitBranchesFunctionData &)*data_p.bind_data;
    
    if (!data.initialized) {
        int error = git_repository_open(&data.repo, data.repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s", 
                            data.repo_path, e ? e->message : "Unknown error");
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
        // Get branch name
        const char *branch_name = nullptr;
        git_branch_name(&branch_name, ref);
        output.SetValue(0, count, Value(branch_name ? branch_name : ""));
        
        // Get commit hash
        const git_oid *oid = git_reference_target(ref);
        if (oid) {
            char hash_str[GIT_OID_HEXSZ + 1];
            git_oid_tostr(hash_str, sizeof(hash_str), oid);
            output.SetValue(1, count, Value(hash_str));
        } else {
            output.SetValue(1, count, Value(""));
        }
        
        // Check if current branch
        bool is_current = git_branch_is_head(ref) == 1;
        output.SetValue(2, count, Value::BOOLEAN(is_current));
        
        // Check if remote branch
        bool is_remote = (branch_type == GIT_BRANCH_REMOTE);
        output.SetValue(3, count, Value::BOOLEAN(is_remote));
        
        git_reference_free(ref);
        count++;
    }
    
    output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Git Tags Function
//===--------------------------------------------------------------------===//

GitTagsFunctionData::GitTagsFunctionData(const string &repo_path)
    : repo_path(repo_path), repo(nullptr), current_index(0), initialized(false) {
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
    
    return_types = {
        LogicalType::VARCHAR,    // tag_name
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // tagger_name
        LogicalType::TIMESTAMP,  // tagger_date
        LogicalType::VARCHAR,    // message
        LogicalType::BOOLEAN     // is_annotated
    };
    
    names = {"tag_name", "commit_hash", "tagger_name", "tagger_date", "message", "is_annotated"};
    
    return make_uniq<GitTagsFunctionData>(repo_path);
}

unique_ptr<GlobalTableFunctionState> GitTagsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

void GitTagsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &data = (GitTagsFunctionData &)*data_p.bind_data;
    
    if (!data.initialized) {
        int error = git_repository_open(&data.repo, data.repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s", 
                            data.repo_path, e ? e->message : "Unknown error");
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
            output.SetValue(0, count, Value(tag_name));
            
            const git_oid *oid = git_reference_target(tag_ref);
            if (oid) {
                char hash_str[GIT_OID_HEXSZ + 1];
                git_oid_tostr(hash_str, sizeof(hash_str), oid);
                output.SetValue(1, count, Value(hash_str));
                
                // Try to get tag object for annotation info
                git_tag *tag_obj = nullptr;
                bool is_annotated = false;
                if (git_tag_lookup(&tag_obj, data.repo, oid) == 0) {
                    is_annotated = true;
                    
                    const git_signature *tagger = git_tag_tagger(tag_obj);
                    if (tagger) {
                        output.SetValue(2, count, Value(tagger->name ? tagger->name : ""));
                        timestamp_t tag_ts = Timestamp::FromEpochSeconds(tagger->when.time);
                        output.SetValue(3, count, Value::TIMESTAMP(tag_ts));
                    } else {
                        output.SetValue(2, count, Value(""));
                        output.SetValue(3, count, Value());
                    }
                    
                    const char *message = git_tag_message(tag_obj);
                    output.SetValue(4, count, Value(message ? message : ""));
                    
                    git_tag_free(tag_obj);
                } else {
                    // Lightweight tag
                    output.SetValue(2, count, Value(""));
                    output.SetValue(3, count, Value());
                    output.SetValue(4, count, Value(""));
                }
                
                output.SetValue(5, count, Value::BOOLEAN(is_annotated));
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
    : ref(ref), repo_path(repo_path), current_index(0) {
}

static string oid_to_hex(const git_oid *oid) {
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    return string(hex);
}

static void traverse_tree(git_repository *repo, git_tree *tree, const string &base, vector<GitTreeRow> &out) {
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
            out.push_back(GitTreeRow{path, mode, oid_to_hex(oid), size});
        } else if (type == GIT_OBJECT_TREE) {
            git_tree *subtree = nullptr;
            if (git_tree_lookup(&subtree, repo, oid) == 0) {
                traverse_tree(repo, subtree, path, out);
                git_tree_free(subtree);
            }
        }
    }
}

unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
    string ref = "HEAD";
    string repo_path = ".";
    
    if (input.inputs.size() >= 1) {
        ref = input.inputs[0].GetValue<string>();
    }
    if (input.inputs.size() >= 2) {
        repo_path = input.inputs[1].GetValue<string>();
    }
    
    // Check for named parameter repo_path
    if (input.named_parameters.count("repo_path")) {
        repo_path = StringValue::Get(input.named_parameters.at("repo_path"));
    }
    
    names = {"path", "mode", "blob_hash", "size"};
    return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT};
    
    return make_uniq<GitTreeFunctionData>(ref, repo_path);
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

    git_object *obj = nullptr;
    if (git_revparse_single(&obj, repo, bind_data.ref.c_str()) != 0) {
        const git_error *e = git_error_last();
        git_repository_free(repo);
        git_libgit2_shutdown();
        throw IOException("git_tree: failed to resolve ref '%s' in repository '%s': %s", 
                        bind_data.ref, bind_data.repo_path, e ? e->message : "Unknown error");
    }

    git_tree *tree = nullptr;
    if (git_object_type(obj) == GIT_OBJECT_COMMIT) {
        auto *commit = reinterpret_cast<git_commit *>(obj);
        if (git_commit_tree(&tree, commit) != 0) {
            git_object_free(obj);
            git_repository_free(repo);
            git_libgit2_shutdown();
            throw IOException("git_tree: failed to get tree from commit");
        }
    } else if (git_object_type(obj) == GIT_OBJECT_TREE) {
        git_oid oid = *git_tree_id(reinterpret_cast<git_tree *>(obj));
        if (git_tree_lookup(&tree, repo, &oid) != 0) {
            git_object_free(obj);
            git_repository_free(repo);
            git_libgit2_shutdown();
            throw IOException("git_tree: failed to lookup tree");
        }
    } else {
        git_object_free(obj);
        git_repository_free(repo);
        git_libgit2_shutdown();
        throw IOException("git_tree: ref is not a commit or tree");
    }

    vector<GitTreeRow> rows;
    rows.reserve(1024);
    traverse_tree(repo, tree, "", rows);

    // Store rows in bind_data so they persist
    bind_data.rows = std::move(rows);

    git_tree_free(tree);
    git_object_free(obj);
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
        output.SetValue(0, i, Value(row.path));
        output.SetValue(1, i, Value::INTEGER(row.mode));
        output.SetValue(2, i, Value(row.blob_hash));
        output.SetValue(3, i, Value::BIGINT(row.size));
    }

    output.SetCardinality(count);
    bind_data.current_index += count;
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

void RegisterGitLogFunction(DatabaseInstance &db) {
    // Single-argument version (existing)
    TableFunction git_log_func("git_log", {LogicalType::VARCHAR}, GitLogFunction, GitLogBind, GitLogInitGlobal);
    git_log_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_log_func);
    
    // Zero-argument version (defaults to current directory)
    TableFunction git_log_func_zero("git_log", {}, GitLogFunction, GitLogBind, GitLogInitGlobal);
    git_log_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_log_func_zero);
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
}

void RegisterGitTreeFunction(DatabaseInstance &db) {
    // Two-argument version (ref, repo_path)
    TableFunction git_tree_func_two("git_tree", {LogicalType::VARCHAR, LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_func_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_tree_func_two);
    
    // Single-argument version (ref only, defaults to current directory)
    TableFunction git_tree_func_one("git_tree", {LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_func_one.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_tree_func_one);
    
    // Zero-argument version (defaults to HEAD and current directory)
    TableFunction git_tree_func_zero("git_tree", {}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(db, git_tree_func_zero);
}

void RegisterGitParentsFunction(DatabaseInstance &db) {
    // Two-argument version (ref, repo_path)
    TableFunction git_parents_func_two("git_parents", {LogicalType::VARCHAR, LogicalType::VARCHAR}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_parents_func_two.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    ExtensionUtil::RegisterFunction(db, git_parents_func_two);
    
    // Single-argument version (ref only, defaults to current directory)
    TableFunction git_parents_func_one("git_parents", {LogicalType::VARCHAR}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func_one.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_parents_func_one.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    ExtensionUtil::RegisterFunction(db, git_parents_func_one);
    
    // Zero-argument version (defaults to HEAD and current directory)
    TableFunction git_parents_func_zero("git_parents", {}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_parents_func_zero.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    ExtensionUtil::RegisterFunction(db, git_parents_func_zero);
}

void RegisterGitFunctions(DatabaseInstance &db) {
    RegisterGitLogFunction(db);
    RegisterGitBranchesFunction(db);
    RegisterGitTagsFunction(db);
    RegisterGitTreeFunction(db);
    RegisterGitParentsFunction(db);
}

} // namespace duckdb