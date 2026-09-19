#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/storage/disk_manager.h"
#include "kerndb/telemetry.h"

namespace kerndb::storage {

inline constexpr std::size_t kDefaultBufferPoolFrames = 16U;

class BufferPoolManager;

// A move-only pin on one buffer frame. The referenced Page remains resident
// until Release() or destruction, so callers cannot keep an evictable raw page
// pointer. Calling mutable_page() marks the frame dirty automatically.
class PinnedPage {
public:
    PinnedPage() = delete;
    ~PinnedPage();

    PinnedPage(PinnedPage&& other) noexcept;
    PinnedPage& operator=(PinnedPage&& other) noexcept;
    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;

    [[nodiscard]] const Page& page() const;
    [[nodiscard]] Page& mutable_page();
    [[nodiscard]] PageId page_id() const noexcept;
    [[nodiscard]] bool pinned() const noexcept;

    // Explicitly releases the pin. Destruction performs the same operation and
    // deliberately ignores an internal release error because destructors cannot
    // report failure.
    [[nodiscard]] Status Release();

private:
    friend class BufferPoolManager;

    PinnedPage(BufferPoolManager& manager, PageId page_id, Page& page) noexcept;
    void Reset() noexcept;

    BufferPoolManager* manager_{nullptr};
    PageId page_id_{};
    Page* page_{nullptr};
    bool dirty_{false};
};

class BufferPoolManager {
public:
    [[nodiscard]] static Result<std::unique_ptr<BufferPoolManager>> Open(
        const std::filesystem::path& path,
        bool create_if_missing,
        std::size_t frame_count = kDefaultBufferPoolFrames,
        MetricsRegistry* metrics = nullptr);

    ~BufferPoolManager();

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;
    BufferPoolManager(BufferPoolManager&&) = delete;
    BufferPoolManager& operator=(BufferPoolManager&&) = delete;

    [[nodiscard]] Result<PinnedPage> FetchPage(PageId page_id);
    [[nodiscard]] Result<PinnedPage> NewPage(PageType type);
    [[nodiscard]] Status FlushPage(PageId page_id);
    [[nodiscard]] Status FlushAllPages();
    [[nodiscard]] Status DeletePage(PageId page_id);
    [[nodiscard]] Result<std::uint64_t> PageCount() const;
    [[nodiscard]] std::size_t frame_count() const noexcept;

private:
    friend class PinnedPage;

    struct Frame {
        std::optional<Page> page;
        std::uint32_t pin_count{0U};
        bool dirty{false};
    };

    BufferPoolManager(
        std::unique_ptr<DiskManager> disk_manager,
        std::size_t frame_count,
        MetricsRegistry* metrics);

    [[nodiscard]] Result<std::size_t> AcquireFrame();
    [[nodiscard]] Status PrepareFrameForUse(std::size_t frame_id);
    [[nodiscard]] Status FlushFrame(std::size_t frame_id);
    [[nodiscard]] Status UnpinPage(PageId page_id, bool dirty);
    [[nodiscard]] PinnedPage PinFrame(std::size_t frame_id);
    void RemoveFromReplacer(std::size_t frame_id);
    void AddToReplacer(std::size_t frame_id);
    void IncrementMetric(std::string_view name) const;

    std::unique_ptr<DiskManager> disk_manager_;
    std::vector<Frame> frames_;
    std::map<std::uint64_t, std::size_t> page_table_;
    std::list<std::size_t> unpinned_lru_;
    MetricsRegistry* metrics_{nullptr};
};

}  // namespace kerndb::storage
