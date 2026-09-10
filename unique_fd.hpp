#pragma once

#include <unistd.h>

namespace evi {

class unique_fd {
 public:
  unique_fd() = default;
  explicit unique_fd(int fd) : fd_(fd) {}
  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  unique_fd(unique_fd&& o) noexcept : fd_(o.release()) {}
  unique_fd& operator=(unique_fd&& o) noexcept {
    reset(o.release());
    return *this;
  }
  ~unique_fd() { reset(); }
  int get() const { return fd_; }
  int release() {
    int f = fd_;
    fd_ = -1;
    return f;
  }
  void reset(int fd = -1) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = fd;
  }
  explicit operator bool() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

}  // namespace evi
