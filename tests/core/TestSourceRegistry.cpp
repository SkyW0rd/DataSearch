#include "TestFramework.h"

#include "datasearch/core/SourceRegistry.h"

using datasearch::core::SourceEntry;
using datasearch::core::SourceRegistry;

void runSourceRegistryTests() {
    SourceRegistry registry(":memory:");

    DS_CHECK_EQ(registry.all().size(), std::size_t{0});

    registry.upsert(SourceEntry{"C:\\", "/idx/c.sqlite3", 0});
    registry.upsert(SourceEntry{"D:\\", "/idx/d.sqlite3", 0});

    auto all = registry.all();
    DS_CHECK_EQ(all.size(), std::size_t{2});
    DS_CHECK_EQ(all[0].root, std::string("C:\\"));
    DS_CHECK_EQ(all[1].root, std::string("D:\\"));

    // Re-registering the same root updates in place, not duplicates.
    registry.upsert(SourceEntry{"C:\\", "/idx/c-new.sqlite3", 0});
    all = registry.all();
    DS_CHECK_EQ(all.size(), std::size_t{2});

    registry.touchLastFullScan("C:\\", 12345);
    all = registry.all();
    const auto& c = (all[0].root == "C:\\") ? all[0] : all[1];
    DS_CHECK_EQ(c.lastFullScan, std::int64_t{12345});
    DS_CHECK_EQ(c.dbPath, std::string("/idx/c-new.sqlite3"));

    registry.remove("D:\\");
    DS_CHECK_EQ(registry.all().size(), std::size_t{1});
}
