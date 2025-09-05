#include "git_clone.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include <regex>

namespace duckdb {

// Constructor
GitCloneFunctionData::GitCloneFunctionData(const string &url, const string &local_path, bool local_path_specified)
    : url(url), local_path(local_path), local_path_specified(local_path_specified) {
}

// Helper function to extract repository name from URL
string ExtractRepoNameFromURL(const string &url) {
    // Remove trailing .git if present
    string clean_url = url;
    if (StringUtil::EndsWith(clean_url, ".git")) {
        clean_url = clean_url.substr(0, clean_url.length() - 4);
    }
    
    // Find the last slash
    auto last_slash = clean_url.find_last_of('/');
    if (last_slash == string::npos) {
        // No slash found, use the whole URL as name
        return clean_url;
    }
    
    // Extract the part after the last slash
    string repo_name = clean_url.substr(last_slash + 1);
    if (repo_name.empty()) {
        // URL ends with slash, try the previous segment
        clean_url = clean_url.substr(0, last_slash);
        last_slash = clean_url.find_last_of('/');
        if (last_slash != string::npos) {
            repo_name = clean_url.substr(last_slash + 1);
        } else {
            repo_name = clean_url; // Use the remaining part if no more slashes
        }
    }
    
    return repo_name.empty() ? "repo" : repo_name;
}

// Helper function to check if a directory is a git repository
bool IsGitRepository(const string &path) {
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, path.c_str());
    if (error == 0 && repo) {
        git_repository_free(repo);
        return true;
    }
    return false;
}

// Helper function to perform git pull/update
GitCloneRow PerformGitPull(ClientContext &context, const string &repo_path, const string &ref, 
                          const GitCloneFunctionData &options) {
    GitCloneRow result;
    result.url = ""; // Not applicable for pull
    result.local_path = repo_path;
    result.status = "error";
    result.action = "error";
    result.commit_time = timestamp_t(0);
    
    git_repository *repo = nullptr;
    git_remote *remote = nullptr;
    git_reference *head_ref = nullptr;
    
    try {
        // Open repository
        int error = git_repository_open(&repo, repo_path.c_str());
        if (error != 0) {
            result.message = "Failed to open repository: " + string(git_error_last()->message);
            return result;
        }
        
        // Get current HEAD commit
        git_oid current_oid;
        error = git_reference_name_to_id(&current_oid, repo, "HEAD");
        if (error == 0) {
            char current_hash_str[GIT_OID_HEXSZ + 1];
            git_oid_fmt(current_hash_str, &current_oid);
            current_hash_str[GIT_OID_HEXSZ] = '\0';
            result.previous_hash = string(current_hash_str);
        }
        
        // Get remote origin
        error = git_remote_lookup(&remote, repo, "origin");
        if (error != 0) {
            result.message = "Failed to lookup origin remote: " + string(git_error_last()->message);
            git_repository_free(repo);
            return result;
        }
        
        // Set up fetch options
        git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
        fetch_opts.callbacks.progress = nullptr;
        
        // Perform fetch
        error = git_remote_fetch(remote, nullptr, &fetch_opts, nullptr);
        if (error != 0) {
            result.message = "Failed to fetch from remote: " + string(git_error_last()->message);
            git_remote_free(remote);
            git_repository_free(repo);
            return result;
        }
        
        // Get the remote head
        const git_remote_head **refs;
        size_t refs_len;
        error = git_remote_ls(&refs, &refs_len, remote);
        if (error != 0 || refs_len == 0) {
            result.message = "Failed to list remote references";
            git_remote_free(remote);
            git_repository_free(repo);
            return result;
        }
        
        // Find the default branch (typically main/master)
        const git_remote_head *target_ref = nullptr;
        for (size_t i = 0; i < refs_len; i++) {
            if (strcmp(refs[i]->name, "refs/heads/main") == 0 || 
                strcmp(refs[i]->name, "refs/heads/master") == 0) {
                target_ref = refs[i];
                break;
            }
        }
        
        if (!target_ref && refs_len > 0) {
            target_ref = refs[0]; // Fall back to first ref
        }
        
        if (!target_ref) {
            result.message = "No suitable remote branch found";
            git_remote_free(remote);
            git_repository_free(repo);
            return result;
        }
        
        // Check if we need to update
        if (error == 0 && git_oid_equal(&current_oid, &target_ref->oid)) {
            result.status = "success";
            result.action = "up_to_date";
            result.message = "Repository is already up to date";
            result.commit_hash = result.previous_hash;
        } else {
            // Perform merge (fast-forward)
            git_annotated_commit *annotated_commit;
            error = git_annotated_commit_from_fetchhead(&annotated_commit, repo, 
                                                       target_ref->name, target_ref->name, &target_ref->oid);
            if (error != 0) {
                result.message = "Failed to create annotated commit: " + string(git_error_last()->message);
            } else {
                git_merge_analysis_t analysis;
                git_merge_preference_t preference;
                error = git_merge_analysis(&analysis, &preference, repo, 
                                         (const git_annotated_commit **)&annotated_commit, 1);
                
                if (error == 0 && (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD)) {
                    // Perform fast-forward merge
                    git_reference *target_ref_obj;
                    error = git_repository_head(&target_ref_obj, repo);
                    if (error == 0) {
                        git_reference *new_ref;
                        error = git_reference_set_target(&new_ref, target_ref_obj, &target_ref->oid, "Fast-forward merge");
                        if (error == 0) {
                            // Update working directory
                            git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
                            checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE;
                            error = git_checkout_head(repo, &checkout_opts);
                            
                            if (error == 0) {
                                result.status = "success";
                                result.action = "updated";
                                result.message = "Repository updated successfully";
                                
                                char new_hash_str[GIT_OID_HEXSZ + 1];
                                git_oid_fmt(new_hash_str, &target_ref->oid);
                                new_hash_str[GIT_OID_HEXSZ] = '\0';
                                result.commit_hash = string(new_hash_str);
                            } else {
                                result.message = "Failed to checkout HEAD after merge: " + string(git_error_last()->message);
                            }
                            git_reference_free(new_ref);
                        } else {
                            result.message = "Failed to update reference: " + string(git_error_last()->message);
                        }
                        git_reference_free(target_ref_obj);
                    } else {
                        result.message = "Failed to get HEAD reference: " + string(git_error_last()->message);
                    }
                } else if (analysis & GIT_MERGE_ANALYSIS_NORMAL) {
                    result.message = "Repository requires manual merge (not supported)";
                } else {
                    result.message = "No merge needed or merge not possible";
                }
                
                git_annotated_commit_free(annotated_commit);
            }
        }
        
    } catch (const std::exception &e) {
        result.message = "Exception during pull: " + string(e.what());
    }
    
    // Cleanup
    if (remote) git_remote_free(remote);
    if (repo) git_repository_free(repo);
    
    return result;
}

// Core function to perform git clone
GitCloneRow PerformGitClone(ClientContext &context, const string &url, const string &local_path, 
                           const GitCloneFunctionData &options) {
    GitCloneRow result;
    result.url = url;
    result.local_path = local_path;
    result.status = "error";
    result.action = "error";
    result.commit_time = timestamp_t(0);
    
    // Check if local_path already exists and is a git repository
    if (IsGitRepository(local_path)) {
        // Repository exists, perform update instead of clone
        return PerformGitPull(context, local_path, options.branch.empty() ? "HEAD" : options.branch, options);
    }
    
    git_repository *repo = nullptr;
    
    try {
        // Set up clone options
        git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;
        clone_opts.checkout_opts.checkout_strategy = options.no_checkout ? GIT_CHECKOUT_NONE : GIT_CHECKOUT_SAFE;
        clone_opts.bare = options.bare;
        
        // Set up branch if specified
        if (!options.branch.empty()) {
            clone_opts.checkout_branch = options.branch.c_str();
        }
        
        // Set up fetch options for depth
        if (options.depth > 0) {
            clone_opts.fetch_opts.depth = options.depth;
        }
        
        // TODO: Add timeout support when libgit2 supports it
        
        // Perform the clone
        int error = git_clone(&repo, url.c_str(), local_path.c_str(), &clone_opts);
        
        if (error == 0 && repo) {
            // Get HEAD commit info
            git_reference *head_ref = nullptr;
            error = git_repository_head(&head_ref, repo);
            if (error == 0) {
                const git_oid *head_oid = git_reference_target(head_ref);
                if (head_oid) {
                    // Format commit hash
                    char hash_str[GIT_OID_HEXSZ + 1];
                    git_oid_fmt(hash_str, head_oid);
                    hash_str[GIT_OID_HEXSZ] = '\0';
                    result.commit_hash = string(hash_str);
                    
                    // Get commit object for timestamp
                    git_commit *commit = nullptr;
                    error = git_commit_lookup(&commit, repo, head_oid);
                    if (error == 0) {
                        git_time_t commit_time = git_commit_time(commit);
                        result.commit_time = timestamp_t(commit_time * 1000000LL); // Convert to microseconds
                        git_commit_free(commit);
                    }
                }
                git_reference_free(head_ref);
            }
            
            result.status = "success";
            result.action = "cloned";
            result.message = "Repository cloned successfully";
            result.previous_hash = ""; // No previous hash for new clone
            
        } else {
            result.message = "Clone failed: " + string(git_error_last() ? git_error_last()->message : "Unknown error");
        }
        
    } catch (const std::exception &e) {
        result.message = "Exception during clone: " + string(e.what());
    }
    
    // Cleanup
    if (repo) {
        git_repository_free(repo);
    }
    
    return result;
}

// Bind function for git_clone
unique_ptr<FunctionData> GitCloneBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.size() < 1 || input.inputs.size() > 3) {
        throw BinderException("git_clone function requires 1-3 arguments: (url [, local_path] [, options])");
    }
    
    // Parse URL
    string url;
    if (input.inputs[0].IsNull()) {
        throw BinderException("git_clone: url cannot be NULL");
    }
    url = StringValue::Get(input.inputs[0]);
    
    // Parse local_path (optional)
    string local_path;
    bool local_path_specified = false;
    
    if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
        auto &second_arg = input.inputs[1];
        if (second_arg.type().id() == LogicalTypeId::VARCHAR) {
            local_path = StringValue::Get(second_arg);
            local_path_specified = true;
        }
    }
    
    if (!local_path_specified) {
        // Extract repository name from URL
        local_path = ExtractRepoNameFromURL(url);
    }
    
    // Create function data
    auto bind_data = make_uniq<GitCloneFunctionData>(url, local_path, local_path_specified);
    
    // Parse options struct (if provided)
    if (input.inputs.size() >= 2) {
        // Check if last argument is a struct (could be 2nd or 3rd argument)
        Value *options_arg = nullptr;
        if (input.inputs.size() == 2 && input.inputs[1].type().id() == LogicalTypeId::STRUCT) {
            options_arg = &input.inputs[1];
        } else if (input.inputs.size() == 3 && input.inputs[2].type().id() == LogicalTypeId::STRUCT) {
            options_arg = &input.inputs[2];
        }
        
        if (options_arg && !options_arg->IsNull()) {
            // Parse struct options
            auto &struct_value = options_arg->GetValue<list_entry_t>();
            auto &struct_children = ListValue::GetChildren(*options_arg);
            
            for (idx_t i = 0; i < struct_children.size(); i++) {
                auto &child = struct_children[i];
                if (child.IsNull()) continue;
                
                // Get field name from struct type
                auto &struct_type = options_arg->type().AuxInfo()->Cast<StructTypeInfo>();
                if (i >= struct_type.child_names.size()) continue;
                auto &field_name = struct_type.child_names[i];
                
                if (field_name == "branch" && child.type().id() == LogicalTypeId::VARCHAR) {
                    bind_data->branch = StringValue::Get(child);
                } else if (field_name == "depth" && child.type().id() == LogicalTypeId::INTEGER) {
                    bind_data->depth = IntegerValue::Get(child);
                } else if (field_name == "bare" && child.type().id() == LogicalTypeId::BOOLEAN) {
                    bind_data->bare = BooleanValue::Get(child);
                } else if (field_name == "no_checkout" && child.type().id() == LogicalTypeId::BOOLEAN) {
                    bind_data->no_checkout = BooleanValue::Get(child);
                } else if (field_name == "timeout" && child.type().id() == LogicalTypeId::INTEGER) {
                    bind_data->timeout = IntegerValue::Get(child);
                } else if (field_name == "force" && child.type().id() == LogicalTypeId::BOOLEAN) {
                    bind_data->force = BooleanValue::Get(child);
                }
            }
        }
    }
    
    // Set return schema
    return_types = {
        LogicalType::VARCHAR,   // url
        LogicalType::VARCHAR,   // local_path  
        LogicalType::VARCHAR,   // status
        LogicalType::VARCHAR,   // action
        LogicalType::VARCHAR,   // message
        LogicalType::VARCHAR,   // commit_hash
        LogicalType::VARCHAR,   // previous_hash
        LogicalType::TIMESTAMP, // commit_time
        LogicalType::BIGINT     // size_bytes
    };
    
    names = {"url", "local_path", "status", "action", "message", "commit_hash", "previous_hash", "commit_time", "size_bytes"};
    
    return std::move(bind_data);
}

// Bind function for git_clone_each
unique_ptr<FunctionData> GitCloneEachBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
    // Same schema as git_clone
    return_types = {
        LogicalType::VARCHAR,   // url
        LogicalType::VARCHAR,   // local_path  
        LogicalType::VARCHAR,   // status
        LogicalType::VARCHAR,   // action
        LogicalType::VARCHAR,   // message
        LogicalType::VARCHAR,   // commit_hash
        LogicalType::VARCHAR,   // previous_hash
        LogicalType::TIMESTAMP, // commit_time
        LogicalType::BIGINT     // size_bytes
    };
    
    names = {"url", "local_path", "status", "action", "message", "commit_hash", "previous_hash", "commit_time", "size_bytes"};
    
    return make_uniq<GitCloneFunctionData>("", "", false); // Dummy data for LATERAL function
}

// Global state initialization
unique_ptr<GlobalTableFunctionState> GitCloneInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

// Local state initialization for git_clone_each
unique_ptr<LocalTableFunctionState> GitCloneLocalInit(ExecutionContext &context, TableFunctionInitInput &input, 
                                                     GlobalTableFunctionState *global_state) {
    return make_uniq<GitCloneLocalState>();
}

// LATERAL function for git_clone_each - processes dynamic URLs
OperatorResultType GitCloneEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                       DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitCloneFunctionData>();
    auto &state = data_p.local_state->Cast<GitCloneLocalState>();
    
    while (true) {
        if (!state.initialized_row) {
            // Initialize for the current input row
            if (state.current_input_row >= input.size()) {
                // Ran out of rows
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // LATERAL function: extract URL from input DataChunk
            input.Flatten();
            
            // Check if input has columns and data
            if (input.ColumnCount() == 0) {
                throw BinderException("git_clone_each: no input columns available");
            }
            
            // Check if the input is null
            if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                // Move to next input row if null
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Extract URL from input DataChunk
            auto data = FlatVector::GetData<string_t>(input.data[0]);
            if (!data) {
                throw BinderException("git_clone_each: no string data in input column");
            }
            
            // Get the string_t directly and convert to string
            auto string_t_value = data[state.current_input_row];
            string url(string_t_value.GetData(), string_t_value.GetSize());
            
            if (url.empty()) {
                throw BinderException("git_clone_each: received empty URL from input");
            }
            
            // Extract additional parameters if provided
            string local_path;
            bool local_path_specified = false;
            
            if (input.ColumnCount() >= 2 && !FlatVector::IsNull(input.data[1], state.current_input_row)) {
                auto path_data = FlatVector::GetData<string_t>(input.data[1]);
                if (path_data) {
                    auto path_string_t = path_data[state.current_input_row];
                    local_path = string(path_string_t.GetData(), path_string_t.GetSize());
                    local_path_specified = true;
                }
            }
            
            if (!local_path_specified) {
                // Extract repository name from URL
                local_path = ExtractRepoNameFromURL(url);
            }
            
            // Create function data with the extracted parameters
            GitCloneFunctionData clone_options(url, local_path, local_path_specified);
            // Copy any additional options from bind_data
            clone_options.branch = bind_data.branch;
            clone_options.depth = bind_data.depth;
            clone_options.bare = bind_data.bare;
            clone_options.no_checkout = bind_data.no_checkout;
            clone_options.timeout = bind_data.timeout;
            clone_options.force = bind_data.force;
            
            // Perform the clone operation
            state.current_result = PerformGitClone(context.client, url, local_path, clone_options);
            state.has_result = true;
            state.initialized_row = true;
        }
        
        if (state.has_result) {
            // Output the result
            output.SetCardinality(1);
            
            // Fill output columns with current result
            FlatVector::GetData<string_t>(output.data[0])[0] = StringVector::AddString(output, state.current_result.url);
            FlatVector::GetData<string_t>(output.data[1])[0] = StringVector::AddString(output, state.current_result.local_path);
            FlatVector::GetData<string_t>(output.data[2])[0] = StringVector::AddString(output, state.current_result.status);
            FlatVector::GetData<string_t>(output.data[3])[0] = StringVector::AddString(output, state.current_result.action);
            FlatVector::GetData<string_t>(output.data[4])[0] = StringVector::AddString(output, state.current_result.message);
            FlatVector::GetData<string_t>(output.data[5])[0] = StringVector::AddString(output, state.current_result.commit_hash);
            FlatVector::GetData<string_t>(output.data[6])[0] = StringVector::AddString(output, state.current_result.previous_hash);
            FlatVector::GetData<timestamp_t>(output.data[7])[0] = state.current_result.commit_time;
            FlatVector::GetData<int64_t>(output.data[8])[0] = state.current_result.size_bytes;
            
            // Move to next input row
            state.current_input_row++;
            state.initialized_row = false;
            state.has_result = false;
            
            return OperatorResultType::HAVE_MORE_OUTPUT;
        }
    }
}

// Main table function for git_clone
void GitCloneFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitCloneFunctionData>();
    
    if (output.size() > 0) {
        return; // Already executed
    }
    
    // Perform the clone operation
    GitCloneRow result = PerformGitClone(context, bind_data.url, bind_data.local_path, bind_data);
    
    // Add single row to output
    output.SetCardinality(1);
    
    // Fill output columns
    FlatVector::GetData<string_t>(output.data[0])[0] = StringVector::AddString(output, result.url);
    FlatVector::GetData<string_t>(output.data[1])[0] = StringVector::AddString(output, result.local_path);
    FlatVector::GetData<string_t>(output.data[2])[0] = StringVector::AddString(output, result.status);
    FlatVector::GetData<string_t>(output.data[3])[0] = StringVector::AddString(output, result.action);
    FlatVector::GetData<string_t>(output.data[4])[0] = StringVector::AddString(output, result.message);
    FlatVector::GetData<string_t>(output.data[5])[0] = StringVector::AddString(output, result.commit_hash);
    FlatVector::GetData<string_t>(output.data[6])[0] = StringVector::AddString(output, result.previous_hash);
    FlatVector::GetData<timestamp_t>(output.data[7])[0] = result.commit_time;
    FlatVector::GetData<int64_t>(output.data[8])[0] = result.size_bytes;
}

// Registration function is now in git_functions.cpp

} // namespace duckdb