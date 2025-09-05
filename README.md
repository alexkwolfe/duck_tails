# Duck Tails 🦆

A DuckDB extension that brings git-aware data analysis capabilities to your database. Query your git history, access files at any revision, and perform version-aware data analysis - all with SQL.

## ✨ Features

### 📥 Repository Cloning
- **git_clone()**: Clone remote repositories with smart conflict handling
- **Auto-path extraction**: Automatically extracts repository name from URL
- **Smart updates**: Existing repositories get updated instead of erroring
- **LATERAL support**: Clone multiple repositories efficiently 
- **Rich options**: Branch selection, shallow cloning, timeouts

### 🗂️ Git Filesystem
Access any file in your git repository at any commit, branch, or tag using the `git://` protocol with flexible repository path support:

```sql
-- Read a CSV file from the current repository
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Access files from sibling repositories
SELECT * FROM read_csv('git://../other-repo/config.json@HEAD');

-- Work with absolute repository paths
SELECT * FROM read_csv('git:///path/to/project/data.csv@HEAD');

-- Compare data between commits
SELECT * FROM read_csv('git://data/sales.csv@HEAD~1');

-- Access files from specific branches and tags
SELECT * FROM read_csv('git://config.json@feature-branch');
SELECT * FROM read_csv('git://metrics.csv@v1.0.0');
```

### 📊 Git Table Functions
Query your git repository metadata directly with flexible repository path support:

```sql
-- View commit history (defaults to current directory)
SELECT repo_path, commit_hash, author_name, message, author_date 
FROM git_log();

-- List all branches with repository context
SELECT repo_path, branch_name, commit_hash, is_current 
FROM git_branches();

-- Show all tags
SELECT repo_path, tag_name, commit_hash, tagger_date 
FROM git_tags();

-- Explore repository structure
SELECT commit_hash, commit_date, path, mode, blob_hash, size FROM git_tree('HEAD');
SELECT commit_hash, parent_hash, parent_index FROM git_parents('.', 'HEAD');

-- Query different repositories
SELECT * FROM git_log('../other-project');
SELECT * FROM git_log('/absolute/path/to/repo');
SELECT * FROM git_tree('HEAD', repo_path := '/path/to/repo');

-- Multi-repository analysis
SELECT repo_path, COUNT(*) as commit_count 
FROM (
    SELECT * FROM git_log('.')
    UNION ALL 
    SELECT * FROM git_log('../other-repo')
) 
GROUP BY repo_path;
```

### 🔄 Repository Cloning
Clone and manage remote repositories directly from SQL with smart conflict handling:

```sql
-- Basic clone with auto-generated local path
SELECT * FROM git_clone('https://github.com/duckdb/duckdb.git');

-- Clone to specific directory  
SELECT * FROM git_clone('https://github.com/apache/arrow.git', 'my-arrow');

-- Shallow clone with options
SELECT * FROM git_clone('https://github.com/postgres/postgres.git', {
    branch: 'main',
    depth: 1,
    timeout: 600
});

-- Clone multiple repositories in one query
CREATE TABLE repos AS VALUES 
  ('https://github.com/duckdb/duckdb.git'),
  ('https://github.com/apache/arrow.git')
AS t(url);

SELECT r.url, c.status, c.local_path, c.action
FROM repos r, LATERAL git_clone_each(r.url) c;

-- Smart updates: automatically pulls existing repositories
SELECT * FROM git_clone('https://github.com/existing/repo.git');
-- Returns action='updated' if repo exists, 'cloned' if new
```

**Smart Features:**
- 🎯 **Auto-path extraction**: Automatically extracts repository name when `local_path` is omitted
- 🔄 **Smart updates**: Existing repositories are updated via `git pull` instead of erroring
- ⚡ **LATERAL support**: Process multiple URLs efficiently with `git_clone_each`
- 🛠️ **Full options**: Branch selection, shallow cloning, timeouts, and more

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
    c.commit_time,
    (SELECT COUNT(*) FROM read_csv('git://metrics.csv@' || c.commit_hash)) as metric_count
FROM git_log('.') c
WHERE c.commit_time > '2024-01-01'
ORDER BY c.commit_time;
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
Duck Tails includes a comprehensive test suite covering all functionality:

```bash
# Run all tests
make test

# Expected output: All tests passed
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
    repo_path,
    author_name,
    COUNT(*) as commit_count,
    MIN(commit_time) as first_commit,
    MAX(commit_time) as latest_commit
FROM git_log('.')
GROUP BY repo_path, author_name
ORDER BY commit_count DESC;

-- Cross-repository activity comparison
SELECT 
    repo_path,
    COUNT(*) as total_commits,
    COUNT(DISTINCT author_name) as contributor_count
FROM (
    SELECT * FROM git_log('.')
    UNION ALL
    SELECT * FROM git_log('../other-project')
) 
GROUP BY repo_path;
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
    g.commit_time,
    g.message,
    r.diff_text
FROM git_log('.') g
CROSS JOIN read_git_diff('git://config.json@' || g.commit_hash || '~1', 
                        'git://config.json@' || g.commit_hash) r
WHERE length(r.diff_text) > 0  -- Only commits that changed config
LIMIT 10;
```

## 📚 Documentation

- **[Function Reference](docs/llmtxt.md)** - Complete reference for all Duck Tails functions
- **[Database Schema](docs/db.md)** - Detailed table structures and column descriptions  
- **[Data Flow](docs/ingestion.md)** - How data flows from Git to SQL results
- **[Advanced Examples](docs/advanced-examples.md)** - Complex query patterns and use cases
- **[Git URIs](docs/git-uris.md)** - Understanding the git:// protocol syntax

## 🏗️ Architecture

Duck Tails implements a custom DuckDB FileSystem that intercepts `git://` URLs and translates them into libgit2 operations:

- **GitFileSystem**: Handles git:// protocol registration and file access
- **GitFileHandle**: Memory-backed file handles for git blob content with seek operations
- **GitPath**: Parser for git://path@revision syntax supporting branches, tags, and commit hashes
- **Git Table Functions**: Direct repository metadata access with full commit history
- **Repository Cloning**: Remote repository cloning with smart conflict resolution
- **TextDiff Engine**: Advanced line-by-line diff computation with multiple output formats
- **Real File Integration**: Seamless access to local files, git:// files, and mixed scenarios
- **vcpkg Integration**: Robust dependency management for cross-platform libgit2 builds

### Key Technical Features
- **Flexible Repository Paths**: Support for relative (`../other-repo`), absolute (`/path/to/repo`), and current directory access
- **Smart Repository Discovery**: Automatic git repository detection using libgit2 with proper error handling
- **Repository Context**: All git functions include `repo_path` column showing the absolute repository path
- **Memory Efficient**: Files loaded on-demand into memory for fast access
- **Seek Support**: Full random access within git blob content
- **RAII Design**: Smart pointer usage throughout for memory safety
- **Error Resilient**: Clear error messages ("No git repository found") with comprehensive edge case handling
- **Mixed File Systems**: Support for local + git://, S3 + git://, and other combinations
- **Consistent Arguments**: All functions take repository path as first argument
- **Comprehensive Test Coverage**: Full test suite ensuring functionality

## 🛣️ Roadmap

### ✅ Current Implementation
- Git filesystem access with git:// protocol support
- **Flexible repository path support** - relative, absolute, and current directory paths
- **Smart repository discovery** - automatic git repository detection with proper error handling
- Git repository metadata queries (git_log, git_branches, git_tags, git_tree, git_parents, git_read) with repository context
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
- **Repository Path Support**: Flexible relative (`../repo`), absolute (`/path/to/repo`), and current directory access
- **Smart Repository Discovery**: Automatic git repository detection using libgit2 with clear error messages
- **Table Functions**: Repository metadata access (`git_log`, `git_branches`, `git_tags`, `git_tree`, `git_parents`, `git_read`, `git_clone`) with repository context
- **Repository Structure**: File tree exploration and commit genealogy analysis
- **Text Diff Engine**: Diff computation with multiple output formats
- **File Integration**: Support for local files, git:// files, and mixed scenarios
- **Memory Management**: Efficient blob loading with seek operations
- **Error Handling**: Comprehensive edge case handling with user-friendly error messages
- **Comprehensive Test Coverage**: Full test suite with extensive assertions

### 📊 Technical Details
- **7 core components**: GitFileSystem, GitFileHandle, GitPath, Table Functions, Repository Cloning, TextDiff, File Integration
- **7 table functions**: git_log, git_branches, git_tags, git_tree, git_read, git_parents, git_clone (plus _each variants)
- **3 diff functions**: diff_text, text_diff, read_git_diff with multiple output formats
- **Repository path discovery**: Automatic git repository detection with relative/absolute path support
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