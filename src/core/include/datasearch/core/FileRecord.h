#pragma once

#include <cstdint>
#include <string>

namespace datasearch::core {

// One indexed file — the "document" in Elasticsearch terms (see ТЗ п.2).
struct FileRecord {
    std::string path;
    std::string name;
    std::string extension;
    std::uint64_t size = 0;
    std::int64_t createdTime = 0;   // seconds since Unix epoch, UTC; 0 if unknown
    std::int64_t modifiedTime = 0;  // seconds since Unix epoch, UTC
};

} // namespace datasearch::core
