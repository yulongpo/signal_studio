#include "infrastructure/mkl_dft.h"

#include <QByteArray>
#include <QFileInfo>
#include <QString>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace signalstudio {
namespace {
std::string dllPath() {
  const auto env = qgetenv("SS_MKL_DLL");
  if (!env.isEmpty()) return env.toStdString();
  return {};
}
}  // namespace

MklDft::MklDft() = default;
MklDft::~MklDft() {
  if (descriptor_ && release_) release_(&descriptor_);
  if (single_ && release_) release_(&single_);
  if (singleInverse_ && release_) release_(&singleInverse_);
  if (module_) FreeLibrary(static_cast<HMODULE>(module_));
}

bool MklDft::load(std::string& error) {
  if (module_ && create_ && createSingle_ && commit_ && execute_ && inverse_ && release_) return true;
  const auto path = dllPath();
  if (path.empty() || !QFileInfo(QString::fromStdString(path)).isAbsolute()) {
    error = "SS_MKL_DLL is not an absolute oneMKL runtime path";
    detail_ = error;
    return false;
  }
  auto* module = LoadLibraryW(QString::fromStdString(path).toStdWString().c_str());
  if (!module) {
    error = "oneMKL runtime did not load";
    detail_ = error;
    return false;
  }
  create_ = reinterpret_cast<Create>(GetProcAddress(module, "DftiCreateDescriptor_d_1d"));
  createSingle_ = reinterpret_cast<Create>(GetProcAddress(module, "DftiCreateDescriptor_s_1d"));
  commit_ = reinterpret_cast<Commit>(GetProcAddress(module, "DftiCommitDescriptor"));
  execute_ = reinterpret_cast<Execute>(GetProcAddress(module, "DftiComputeForward"));
  inverse_ = reinterpret_cast<Execute>(GetProcAddress(module, "DftiComputeBackward"));
  release_ = reinterpret_cast<Release>(GetProcAddress(module, "DftiFreeDescriptor"));
  if (!create_ || !createSingle_ || !commit_ || !execute_ || !inverse_ || !release_) {
    FreeLibrary(module);
    error = "oneMKL DFTI exports are missing";
    detail_ = error;
    return false;
  }
  module_ = module;
  detail_.clear();
  return true;
}

bool MklDft::prepare(std::uint32_t length, std::string& error) {
  if (length < 2048 || (length & (length - 1))) {
    error = "oneMKL STFT length is below 2048 or is not a power of two";
    return false;
  }
  if (!load(error)) return false;
  if (descriptor_ && length_ == length) return true;
  if (descriptor_) {
    release_(&descriptor_);
    descriptor_ = nullptr;
    length_ = 0;
  }
  // DFTI_COMPLEX == 32. Forward scale stays at the library default of 1.
  if (create_(&descriptor_, 32, static_cast<long>(length)) != 0 || !descriptor_ ||
      commit_(descriptor_) != 0) {
    if (descriptor_) release_(&descriptor_);
    descriptor_ = nullptr;
    error = "oneMKL DFTI plan failed";
    detail_ = error;
    return false;
  }
  length_ = length;
  detail_.clear();
  return true;
}

bool MklDft::forward(std::complex<double>* data, std::uint32_t length, std::string& error) {
  if (!descriptor_ || length != length_ || !data) {
    error = "oneMKL DFTI plan does not match the frame";
    return false;
  }
  if (execute_(descriptor_, data) != 0) {
    error = "oneMKL DFTI execute failed";
    detail_ = error;
    return false;
  }
  return true;
}

bool MklDft::transform(std::complex<float>* data, std::uint32_t length, bool inverse, std::string& error) {
  if (!length || !data) { error = "oneMKL float transform is empty"; return false; }
  if (!load(error)) { error = "missing"; return false; }
  if (!single_ || !singleInverse_ || singleLength_ != length) {
    if (single_) release_(&single_);
    if (singleInverse_) release_(&singleInverse_);
    single_ = singleInverse_ = nullptr;
    singleLength_ = 0;
    if (createSingle_(&single_, 32, static_cast<long>(length)) != 0 || !single_ || commit_(single_) != 0 ||
        createSingle_(&singleInverse_, 32, static_cast<long>(length)) != 0 || !singleInverse_ ||
        commit_(singleInverse_) != 0) {
      if (single_) release_(&single_);
      if (singleInverse_) release_(&singleInverse_);
      single_ = singleInverse_ = nullptr;
      error = "oneMKL float DFTI plan failed";
      detail_ = error;
      return false;
    }
    singleLength_ = length;
  }
  auto* desc = inverse ? singleInverse_ : single_;
  if ((inverse ? inverse_ : execute_)(desc, data) != 0) {
    error = "oneMKL float DFTI execute failed";
    detail_ = error;
    return false;
  }
  if (inverse) {
    const float scale = 1.f / static_cast<float>(length);
    for (std::uint32_t i = 0; i < length; ++i) data[i] *= scale;
  }
  return true;
}

bool MklDft::transform(std::complex<double>* data, std::uint32_t length, bool inverse, std::string& error) {
  if (!length || !data) { error = "oneMKL double transform is empty"; return false; }
  if (!load(error)) { error = "missing"; return false; }
  if (!descriptor_ || length_ != length) {
    if (descriptor_) release_(&descriptor_);
    descriptor_ = nullptr;
    length_ = 0;
    if (create_(&descriptor_, 32, static_cast<long>(length)) != 0 || !descriptor_ || commit_(descriptor_) != 0) {
      if (descriptor_) release_(&descriptor_);
      descriptor_ = nullptr;
      error = "oneMKL double DFTI plan failed";
      detail_ = error;
      return false;
    }
    length_ = length;
  }
  if ((inverse ? inverse_ : execute_)(descriptor_, data) != 0) {
    error = "oneMKL double DFTI execute failed";
    detail_ = error;
    return false;
  }
  if (inverse) {
    const double scale = 1.0 / static_cast<double>(length);
    for (std::uint32_t i = 0; i < length; ++i) data[i] *= scale;
  }
  return true;
}
}  // namespace signalstudio
