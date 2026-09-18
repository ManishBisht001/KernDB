#include <cstddef>
#include <string>

#include "kerndb/result.h"
#include "test_framework.h"

KERNDB_TEST(StatusCapturesErrorCodeMessageAndContext) {
    const kerndb::Status status = kerndb::Status::Error(
                                      kerndb::ErrorCode::kInvalidArgument,
                                      "invalid page size")
                                      .WithContext("requested", "1024")
                                      .WithContext("minimum", "4096");

    KERNDB_EXPECT(!status.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kInvalidArgument, status.code());
    KERNDB_EXPECT_EQ(std::string("invalid page size"), status.message());
    KERNDB_EXPECT_EQ(std::size_t{2U}, status.context().size());
    KERNDB_EXPECT(status.ToString().find("requested=1024") != std::string::npos);
}

KERNDB_TEST(ResultStoresValuesAndErrors) {
    const kerndb::Result<std::string> value_result{std::string("Ada")};
    KERNDB_EXPECT(value_result.ok());
    KERNDB_EXPECT_EQ(std::string("Ada"), value_result.value());
    KERNDB_EXPECT(value_result.status().ok());

    const kerndb::Result<std::string> error_result = kerndb::Status::Error(
        kerndb::ErrorCode::kNotFound,
        "table does not exist");
    KERNDB_EXPECT(!error_result.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kNotFound, error_result.status().code());

    const kerndb::Result<void> successful_void_result;
    KERNDB_EXPECT(successful_void_result.ok());
}
