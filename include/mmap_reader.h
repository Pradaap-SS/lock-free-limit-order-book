#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>

// RAII wrapper around mmap(2) for read-only file access.
//
// Maps the entire file into virtual address space in one call — no buffering,
// no read() syscalls on the hot path. The OS page-faults data in lazily as
// needed. MADV_SEQUENTIAL tells the kernel to prefetch ahead.
//
// Usage:
//   MmapReader f("data/AAPL.itch");
//   const uint8_t* p = f.data();
//   std::size_t    n = f.size();

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

class MmapReader {
public:
    explicit MmapReader(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("MmapReader: cannot open: ") + path);
        }

        struct stat st{};
        if (::fstat(fd_, &st) < 0) {
            ::close(fd_);
            throw std::runtime_error("MmapReader: fstat failed");
        }
        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ == 0) {
            // mmap(0 bytes) is undefined — leave data_ null; caller checks size()
            return;
        }

        data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            ::close(fd_);
            throw std::runtime_error("MmapReader: mmap failed");
        }

        // Sequential access hint — kernel will read-ahead pages
        ::madvise(data_, size_, MADV_SEQUENTIAL);
    }

    ~MmapReader() {
        if (data_) ::munmap(data_, size_);
        if (fd_ >= 0) ::close(fd_);
    }

    // Non-copyable, movable
    MmapReader(const MmapReader&) = delete;
    MmapReader& operator=(const MmapReader&) = delete;

    MmapReader(MmapReader&& o) noexcept
        : data_(o.data_), size_(o.size_), fd_(o.fd_) {
        o.data_ = nullptr;
        o.size_ = 0;
        o.fd_   = -1;
    }

    [[nodiscard]] const uint8_t* data() const noexcept {
        return static_cast<const uint8_t*>(data_);
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }

private:
    void*       data_{nullptr};
    std::size_t size_{0};
    int         fd_{-1};
};
