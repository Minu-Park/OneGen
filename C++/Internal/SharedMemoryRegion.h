#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace onegen {

/**
 * @brief Host-created OS-backed frame region.
 *
 * The host maps the region read-only and the worker opens it read/write. The
 * name is random and only the same OS user can open it; the worker never
 * publishes a writable view to the host.
 */
class SharedMemoryRegion {
public:
  ~SharedMemoryRegion() { release(); }
  SharedMemoryRegion(const SharedMemoryRegion&) = delete;
  SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;

  /** @brief Creates an unguessable region and maps it read-only for the host. */
  static std::shared_ptr<SharedMemoryRegion> create(std::size_t bytes, std::string& error);
  /** @brief Opens the host-created region read/write in the worker. */
  static std::shared_ptr<SharedMemoryRegion> open(const std::string& name, std::size_t bytes,
                                                  std::string& error);

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  /** @brief Read-only host or worker view; valid while this shared owner is alive. */
  [[nodiscard]] const std::uint8_t* data() const noexcept
  {
    return static_cast<const std::uint8_t*>(view_);
  }
  /** @brief Worker-only writable view; null on the host mapping. */
  [[nodiscard]] std::uint8_t* writable() noexcept
  {
    return writable_ ? static_cast<std::uint8_t*>(view_) : nullptr;
  }

private:
  SharedMemoryRegion() = default;
  void release() noexcept;
  static std::string make_name();

  std::string name_;
  std::size_t size_ = 0;
  void* view_ = nullptr;
  bool writable_ = false;
  bool owner_ = false;
#ifdef _WIN32
  HANDLE mapping_ = nullptr;
#else
  int fd_ = -1;
#endif
};

inline std::string SharedMemoryRegion::make_name()
{
  std::random_device random;
  const auto high = (static_cast<std::uint64_t>(random()) << 32) ^ random();
  const auto low = (static_cast<std::uint64_t>(random()) << 32) ^ random();
  char suffix[32] = {};
  std::snprintf(suffix, sizeof(suffix), "og-%016llx%08llx",
      static_cast<unsigned long long>(high),
      static_cast<unsigned long long>(low & 0xffffffffULL));
#ifdef _WIN32
  return std::string("Local\\") + suffix;
#else
  return std::string("/") + suffix;
#endif
}

inline std::shared_ptr<SharedMemoryRegion> SharedMemoryRegion::create(std::size_t bytes,
                                                                      std::string& error)
{
  if (!bytes) {
    error = "frame pool size is zero";
    return {};
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto region = std::shared_ptr<SharedMemoryRegion>(new SharedMemoryRegion());
    region->name_ = make_name();
    region->size_ = bytes;
    region->owner_ = true;
#ifdef _WIN32
    const auto wide_name = std::wstring(region->name_.begin(), region->name_.end());
    region->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes & 0xffffffffULL),
        wide_name.c_str());
    if (!region->mapping_) {
      error = "CreateFileMapping failed: " + std::to_string(GetLastError());
      return {};
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(region->mapping_);
      region->mapping_ = nullptr;
      continue;
    }
    region->view_ = MapViewOfFile(region->mapping_, FILE_MAP_READ, 0, 0, bytes);
    if (!region->view_) {
      error = "MapViewOfFile failed: " + std::to_string(GetLastError());
      return {};
    }
    return region;
#else
    region->fd_ = shm_open(region->name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (region->fd_ < 0) {
      if (errno == EEXIST) continue;
      error = "shm_open failed: " + std::to_string(errno);
      return {};
    }
    if (ftruncate(region->fd_, static_cast<off_t>(bytes)) != 0) {
      error = "ftruncate failed: " + std::to_string(errno);
      return {};
    }
    region->view_ = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, region->fd_, 0);
    if (region->view_ == MAP_FAILED) {
      region->view_ = nullptr;
      error = "mmap failed: " + std::to_string(errno);
      return {};
    }
    return region;
#endif
  }
  error = "could not allocate a unique frame pool name";
  return {};
}

inline std::shared_ptr<SharedMemoryRegion> SharedMemoryRegion::open(const std::string& name,
                                                                    std::size_t bytes,
                                                                    std::string& error)
{
  if (!bytes) {
    error = "frame pool size is zero";
    return {};
  }
  auto region = std::shared_ptr<SharedMemoryRegion>(new SharedMemoryRegion());
  region->name_ = name;
  region->size_ = bytes;
  region->writable_ = true;
#ifdef _WIN32
  const auto wide_name = std::wstring(name.begin(), name.end());
  region->mapping_ = OpenFileMappingW(FILE_MAP_WRITE, FALSE, wide_name.c_str());
  if (!region->mapping_) {
    error = "OpenFileMapping failed: " + std::to_string(GetLastError());
    return {};
  }
  region->view_ = MapViewOfFile(region->mapping_, FILE_MAP_WRITE, 0, 0, bytes);
  if (!region->view_) {
    error = "MapViewOfFile failed: " + std::to_string(GetLastError());
    return {};
  }
  return region;
#else
  region->fd_ = shm_open(name.c_str(), O_RDWR, 0);
  if (region->fd_ < 0) {
    error = "shm_open failed: " + std::to_string(errno);
    return {};
  }
  struct stat status {};
  if (fstat(region->fd_, &status) != 0
      || static_cast<std::size_t>(status.st_size) < bytes) {
    error = "shared frame region is smaller than requested";
    return {};
  }
  region->view_ = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, region->fd_, 0);
  if (region->view_ == MAP_FAILED) {
    region->view_ = nullptr;
    error = "mmap failed: " + std::to_string(errno);
    return {};
  }
  return region;
#endif
}

inline void SharedMemoryRegion::release() noexcept
{
#ifdef _WIN32
  if (view_) UnmapViewOfFile(view_);
  if (mapping_) CloseHandle(mapping_);
  view_ = nullptr;
  mapping_ = nullptr;
#else
  if (view_) munmap(view_, size_);
  if (fd_ >= 0) close(fd_);
  if (owner_ && !name_.empty()) shm_unlink(name_.c_str());
  view_ = nullptr;
  fd_ = -1;
#endif
  size_ = 0;
  writable_ = false;
  owner_ = false;
}

} // namespace onegen
