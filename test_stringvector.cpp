#include "duckdb.hpp"
#include "duckdb/common/types/vector.hpp"

using namespace duckdb;

void test_stringvector() {
    DataChunk output;
    output.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
    
    // This should work
    StringVector::AddString(output.data[0], "test");
    
    // This should fail 
    // StringVector::AddString(output, "test");
}