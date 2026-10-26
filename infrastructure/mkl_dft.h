#pragma once

#include <complex>
#include <cstdint>
#include <string>

namespace signalstudio {

// Host-side oneMKL DFTI. The DLL is optional. A missing library leaves the
// caller on the existing handwritten transform.
class MklDft {
 public:
  MklDft();
  ~MklDft();
  MklDft(const MklDft&) = delete;
  MklDft& operator=(const MklDft&) = delete;

  bool prepare(std::uint32_t length, std::string& error);
  bool forward(std::complex<double>* data, std::uint32_t length, std::string& error);
  // Arbitrary length. Missing runtime returns false and sets error to "missing".
  // A committed library failure returns false with another error and must not be
  // retried on the handwritten transform.
  bool transform(std::complex<float>* data, std::uint32_t length, bool inverse, std::string& error);
  bool transform(std::complex<double>* data, std::uint32_t length, bool inverse, std::string& error);
  const std::string& detail() const { return detail_; }

 private:
  void* module_ = nullptr;
  void* descriptor_ = nullptr;
  void* single_ = nullptr;
  void* singleInverse_ = nullptr;
  std::uint32_t length_ = 0;
  std::uint32_t singleLength_ = 0;
  using Create = long(__cdecl*)(void**, int, long);
  using Commit = long(__cdecl*)(void*);
  using Execute = long(__cdecl*)(void*, void*);
  using Release = long(__cdecl*)(void**);
  Create create_ = nullptr;
  Create createSingle_ = nullptr;
  Commit commit_ = nullptr;
  Execute execute_ = nullptr;
  Execute inverse_ = nullptr;
  Release release_ = nullptr;
  std::string detail_;
  bool load(std::string& error);
};

}  // namespace signalstudio
