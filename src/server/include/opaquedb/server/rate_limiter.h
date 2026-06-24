#ifndef OPAQUEDB_SERVER_RATE_LIMITER_H_
#define OPAQUEDB_SERVER_RATE_LIMITER_H_

#include <chrono>
#include <mutex>

// A token-bucket rate limiter. It bounds how fast requests are admitted, so one
// caller cannot flood the node. The capacity and refill rate have permissive
// defaults; exposing them through config is a documented extension point.

namespace opaquedb::server {

class RateLimiter {
public:
  RateLimiter(double tokens_per_second, double burst)
      : rate_(tokens_per_second), burst_(burst), tokens_(burst),
        last_(std::chrono::steady_clock::now()) {}

  // Admits one request if a token is available, refilling by elapsed time.
  bool TryAcquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    tokens_ = std::min(burst_, tokens_ + elapsed * rate_);
    if (tokens_ < 1.0)
      return false;
    tokens_ -= 1.0;
    return true;
  }

private:
  double rate_;
  double burst_;
  double tokens_;
  std::chrono::steady_clock::time_point last_;
  std::mutex mutex_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_RATE_LIMITER_H_
