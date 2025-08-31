# Duck Tails 🦆

A DuckDB extension that brings git-aware data analysis capabilities to your database. Query your git history, access files at any revision, and perform version-aware data analysis - all with SQL.

## ✨ Features

### 🗂️ Git Filesystem
Access any file in your git repository at any commit, branch, or tag using the `git://` protocol:

```sql
-- Read a CSV file from the current HEAD
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Compare data between commits
SELECT * FROM read_csv('git://data/sales.csv@HEAD~1');

-- Access files from a specific branch
SELECT * FROM read_csv('git://config.json@feature-branch');

-- Load data from a tagged release
SELECT * FROM read_csv('git://metrics.csv@v1.0.0');
```

### 📊 Git Table Functions
Query your git repository metadata directly:

```sql
-- View commit history
SELECT commit_hash, author_name, message, author_date FROM git_log();

-- List all branches and tags  
SELECT branch_name, commit_hash FROM git_branches();
SELECT tag_name, commit_hash, tagger_date FROM git_tags();

-- Explore repository structure
SELECT path, mode, blob_hash, size FROM git_tree('HEAD');
SELECT commit_hash, parent_hash, parent_index FROM git_parents('HEAD');

-- Query different repositories
SELECT * FROM git_log('/path/to/repo');
SELECT * FROM git_tree('HEAD', repo_path := '/path/to/repo');
```

### 🔄 Version-Aware Analysis
Perform sophisticated version comparisons and historical analysis:

```sql
-- Compare record counts across versions
WITH current AS (SELECT COUNT(*) as cnt FROM read_csv('git://data.csv@HEAD')),
     previous AS (SELECT COUNT(*) as cnt FROM read_csv('git://data.csv@HEAD~1'))
SELECT current.cnt - previous.cnt AS records_added
FROM current, previous;

-- Analyze changes over time
SELECT 
    c.commit_hash,
    c.author_date,
    (SELECT COUNT(*) FROM read_csv('git://metrics.csv@' || c.commit_hash)) as metric_count
FROM git_log() c
WHERE c.author_date > '2024-01-01'
ORDER BY c.author_date;
```

### 🧠 Text Diff Analysis
Text diffing capabilities with file integration:

```sql
-- Pure text diffing (string result)
SELECT diff_text('Hello World', 'Hello DuckDB');

-- Text diffing (original function name)
SELECT text_diff('Hello World', 'Hello DuckDB');

-- Single file diff against HEAD (convenient shorthand)
SELECT * FROM read_git_diff('file.txt');

-- File-based diffing with local files
SELECT * FROM read_git_diff('file1.txt', 'file2.txt');

-- Git repository file diffing
SELECT * FROM read_git_diff('git://README.md@HEAD', 'git://README.md@HEAD~1');
```

### 📄 Git Content Reading
```sql
-- Read file content with metadata 
SELECT uri, is_text, encoding, size_bytes, text FROM git_read('git://README.md@HEAD');

-- Read with size limit
SELECT text FROM git_read('git://README.md@HEAD', 1000);
```

## 🚀 Quick Start

### Prerequisites
- DuckDB v1.3.2+
- vcpkg package manager
- libgit2 (automatically installed via vcpkg)

### Building
```bash
# Clone and build
git clone https://github.com/teaguesterling/duck_tails.git
cd duck_tails
make

# Run tests to verify everything works
make test

# Load the extension
./build/release/duckdb -c "LOAD 'duck_tails';"
```

### Basic Usage
```sql
-- Load the extension
LOAD 'duck_tails';

-- Query git history (clean syntax - no arguments needed!)
SELECT * FROM git_log() LIMIT 5;

-- Access version-controlled data
SELECT * FROM read_csv('git://test/data/sales.csv@HEAD');
```

### Testing
Duck Tails includes a comprehensive test suite with **108 test assertions** covering all functionality:

```bash
# Run all tests
make test

# Expected output: All tests passed (108 assertions across 8 test suites)
```

## 📋 Examples

### Historical Data Analysis
```sql
-- Compare sales data between releases
SELECT 
    'v1.0' as version,
    SUM(amount) as total_sales
FROM read_csv('git://sales.csv@v1.0')
UNION ALL
SELECT 
    'v2.0' as version,
    SUM(amount) as total_sales  
FROM read_csv('git://sales.csv@v2.0');
```

### Repository Analytics
```sql
-- Most active contributors
SELECT 
    author_name,
    COUNT(*) as commit_count,
    MIN(author_date) as first_commit,
    MAX(author_date) as latest_commit
FROM git_log()
GROUP BY author_name
ORDER BY commit_count DESC;
```


### Configuration Drift Detection
```sql
-- Compare configuration files across branches
SELECT 
    'main' as branch,
    * 
FROM read_json('git://config.json@main')
UNION ALL
SELECT 
    'develop' as branch,
    *
FROM read_json('git://config.json@develop');
```

### Code Change Analysis
```sql
-- Analyze file changes between versions
SELECT 
    diff_text,
    length(diff_text) as diff_size
FROM read_git_diff('git://src/main.py@HEAD~1', 'git://src/main.py@HEAD');

-- Track configuration changes over time
SELECT 
    g.commit_hash,
    g.author_date,
    g.message,
    r.diff_text
FROM git_log() g
CROSS JOIN read_git_diff('git://config.json@' || g.commit_hash || '~1', 
                        'git://config.json@' || g.commit_hash) r
WHERE length(r.diff_text) > 0  -- Only commits that changed config
LIMIT 10;
```

More complex examples are available in [docs/advanced-examples.md](docs/advanced-examples.md).

## 🏗️ Architecture

Duck Tails implements a custom DuckDB FileSystem that intercepts `git://` URLs and translates them into libgit2 operations:

- **GitFileSystem**: Handles git:// protocol registration and file access
- **GitFileHandle**: Memory-backed file handles for git blob content with seek operations
- **GitPath**: Parser for git://path@revision syntax supporting branches, tags, and commit hashes
- **Git Table Functions**: Direct repository metadata access with full commit history
- **TextDiff Engine**: Advanced line-by-line diff computation with multiple output formats
- **Real File Integration**: Seamless access to local files, git:// files, and mixed scenarios
- **vcpkg Integration**: Robust dependency management for cross-platform libgit2 builds

### Key Technical Features
- **Memory Efficient**: Files loaded on-demand into memory for fast access
- **Seek Support**: Full random access within git blob content
- **RAII Design**: Smart pointer usage throughout for memory safety
- **Error Resilient**: Comprehensive error handling for missing repos/revisions
- **Mixed File Systems**: Support for local + git://, S3 + git://, and other combinations
- **Zero-Argument Functions**: Clean syntax defaulting to current directory
- **Test Coverage**: 108 test assertions ensuring functionality

## 🛣️ Roadmap

### ✅ Current Implementation
- Git filesystem access with git:// protocol support
- Git repository metadata queries (git_log, git_branches, git_tags, git_tree, git_parents, git_read)
- Text diff analysis with multiple output formats
- Mixed file system support (local + git:// files)

### 🔮 Future Enhancements
- **Semantic Code Intelligence**: AST-aware diff analysis and function tracking
- **Development Workflow Integration**: Pull request analytics and code review intelligence
- **Advanced Analytics**: Development velocity metrics and team insights

## 🤝 Contributing

Duck Tails is built with modern C++, DuckDB's extension framework, and libgit2. 

### Development Setup
```bash
# Clone with DuckDB submodule
git clone --recursive https://github.com/teaguesterling/duck_tails.git
cd duck_tails

# Build and test
make
make test
```

### Key Technologies
- **DuckDB Extension API**: FileSystem and table function registration
- **libgit2**: Git repository access and blob content loading  
- **vcpkg**: Dependency management for cross-platform builds
- **RAII**: Smart pointer usage throughout for memory safety

### Test-Driven Development
All new features should include comprehensive tests. Our test suite is designed to be resilient to repository changes and uses flexible assertions that won't break with new commits.

## 🏆 Current Status

### ✅ Implemented Features
- **Git Filesystem**: `git://` protocol implementation with revision support
- **Table Functions**: Repository metadata access (`git_log`, `git_branches`, `git_tags`, `git_tree`, `git_parents`, `git_read`)
- **Repository Structure**: File tree exploration and commit genealogy analysis
- **Text Diff Engine**: Diff computation with multiple output formats
- **File Integration**: Support for local files, git:// files, and mixed scenarios
- **Memory Management**: Efficient blob loading with seek operations
- **Error Handling**: Robust error handling for edge cases
- **Test Coverage**: 108 comprehensive test assertions across 8 test suites

### 📊 Technical Details
- **8 test suites** with 108 assertions covering all functionality
- **6 core components**: GitFileSystem, GitFileHandle, GitPath, Table Functions, TextDiff, File Integration
- **17 functions implemented**: git_log, git_branches, git_tags, git_read (multiple arg variants), git_tree, git_parents (multiple arg variants), diff_text, text_diff, read_git_diff (1 and 2 arg), text_diff_lines, text_diff_stats
- **libgit2 integration** via vcpkg dependency management

## 📜 License

[License details to be added]

## 🙏 Acknowledgments

Built with ❤️ using:
- [DuckDB](https://duckdb.org/) - Fast analytical database
- [libgit2](https://libgit2.org/) - Portable git implementation
- [vcpkg](https://vcpkg.io/) - C++ package manager

---

*Duck Tails: Where data analysis meets version control* 🦆✨