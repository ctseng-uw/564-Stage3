#include <filesystem>
#include <memory>

#include "catch_amalgamated.hpp"

// clang-format off
#include "page.h"
#include "buf.h"
// clang-format on

BufMgr* bufMgr = nullptr;

TEST_CASE("Buffer manager") {
    const std::string fileBase = "unittestdata.";
    const int bufSize = 50;
    auto bufMgrPtr = std::make_unique<BufMgr>(bufSize);
    bufMgr = bufMgrPtr.get();
    DB db;
    std::array<File*, 3> files;
    for (int i = 0; i < 3; i++) {
        std::string fileName = fileBase + std::to_string(i);
        std::filesystem::remove(fileName);
        REQUIRE(db.createFile(fileName) == OK);
        REQUIRE(db.openFile(fileName, files[i]) == OK);
    }

    SECTION("BufMgr construct successfully") { REQUIRE(bufMgr != nullptr); }

    for (const auto file : files) {
        db.closeFile(file);
    }
    for (int i = 0; i < 3; i++) {
        std::string fileName = fileBase + std::to_string(i);
        std::filesystem::remove(fileName);
    }
}