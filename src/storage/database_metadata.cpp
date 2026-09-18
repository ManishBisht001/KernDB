#include "kerndb/storage/database_metadata.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "kerndb/byte_codec.h"
#include "kerndb/options.h"

namespace kerndb::storage {
namespace {

constexpr std::uint32_t kMetadataMagic = 0x4B44424DU;
constexpr std::size_t kMetadataSize = 32U;

[[nodiscard]] Status MetadataError(
    ErrorCode code,
    const std::filesystem::path& path,
    std::string message) {
    return Status::Error(code, std::move(message)).WithContext("path", path.string());
}

[[nodiscard]] std::uint64_t NewDatabaseId() {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return static_cast<std::uint64_t>(now) ^ 0x4B45524E4442554FULL;
}

[[nodiscard]] Status CreateMetadata(const std::filesystem::path& path) {
    ByteWriter writer(kMetadataSize);
    writer.WriteUInt32(kMetadataMagic);
    writer.WriteUInt32(kPersistentFormatVersion);
    writer.WriteUInt32(static_cast<std::uint32_t>(kInitialPageSizeBytes));
    writer.WriteUInt64(NewDatabaseId());
    writer.WriteUInt64(0U);
    writer.WriteUInt32(0U);

    const std::vector<std::byte> bytes = std::move(writer).TakeBytes();
    if (bytes.size() != kMetadataSize) {
        return Status::Error(ErrorCode::kInternal, "database metadata encoder produced an invalid size");
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return MetadataError(ErrorCode::kIo, path, "failed to create database metadata");
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file.good()) {
        return MetadataError(ErrorCode::kIo, path, "failed to write database metadata");
    }
    return Status::Ok();
}

[[nodiscard]] Result<DatabaseMetadata> ReadMetadata(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return MetadataError(ErrorCode::kIo, path, "failed to inspect database metadata")
            .WithContext("error", filesystem_error.message());
    }
    if (file_size != kMetadataSize) {
        return MetadataError(ErrorCode::kCorruption, path, "database metadata has an invalid file size")
            .WithContext("actual_size", std::to_string(file_size));
    }

    std::vector<std::byte> bytes(kMetadataSize);
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return MetadataError(ErrorCode::kIo, path, "failed to open database metadata");
    }
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (file.gcount() != static_cast<std::streamsize>(bytes.size()) || !file.good()) {
        return MetadataError(ErrorCode::kIo, path, "failed to read database metadata");
    }

    ByteReader reader(bytes);
    const auto magic = reader.ReadUInt32();
    const auto version = reader.ReadUInt32();
    const auto page_size = reader.ReadUInt32();
    const auto database_id = reader.ReadUInt64();
    const auto catalog_root = reader.ReadUInt64();
    const auto reserved = reader.ReadUInt32();
    static_cast<void>(catalog_root);
    static_cast<void>(reserved);
    if (!magic.ok() || !version.ok() || !page_size.ok() || !database_id.ok()) {
        return MetadataError(ErrorCode::kCorruption, path, "database metadata is truncated");
    }
    if (magic.value() != kMetadataMagic) {
        return MetadataError(ErrorCode::kCorruption, path, "database metadata magic is invalid");
    }
    if (version.value() != kPersistentFormatVersion) {
        return MetadataError(ErrorCode::kUnsupported, path, "database format version is unsupported")
            .WithContext("version", std::to_string(version.value()));
    }
    if (page_size.value() != kInitialPageSizeBytes) {
        return MetadataError(ErrorCode::kUnsupported, path, "database page size is incompatible")
            .WithContext("page_size", std::to_string(page_size.value()));
    }
    if (database_id.value() == 0U) {
        return MetadataError(ErrorCode::kCorruption, path, "database metadata ID is invalid");
    }
    return DatabaseMetadata{
        .database_id = database_id.value(),
    };
}

}  // namespace

Result<DatabaseMetadata> OpenOrCreateDatabaseMetadata(
    const std::filesystem::path& database_directory) {
    std::error_code error;
    if (!std::filesystem::exists(database_directory, error)) {
        if (!std::filesystem::create_directories(database_directory, error)) {
            return MetadataError(ErrorCode::kIo, database_directory, "failed to create database directory")
                .WithContext("error", error.message());
        }
    } else if (error || !std::filesystem::is_directory(database_directory, error)) {
        return MetadataError(ErrorCode::kIo, database_directory, "database path is not a directory");
    }

    const std::filesystem::path metadata_path = database_directory / "database.meta";
    if (!std::filesystem::exists(metadata_path, error)) {
        if (error) {
            return MetadataError(ErrorCode::kIo, metadata_path, "failed to inspect database metadata")
                .WithContext("error", error.message());
        }
        const Status create_status = CreateMetadata(metadata_path);
        if (!create_status.ok()) {
            return create_status;
        }
    }
    return ReadMetadata(metadata_path);
}

}  // namespace kerndb::storage
