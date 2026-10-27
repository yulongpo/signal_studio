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
std::wstring moduleDirectory() {
  std::wstring image(32768, L'\0');
  const auto written = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
  if (!written || written >= image.size()) return {};
  image.resize(written);
  const auto slash = image.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return image.substr(0, slash);
}

// Probe values never fall through into the deploy directory or the current
// working directory. "off" and "0" keep the handwritten transform.
bool probeOverride(std::wstring& path, std::string& error) {
  const auto env = qgetenv("SS_MKL_DLL");
  if (env.isEmpty()) return false;
  if (env == "off" || env == "0") {
    error = "oneMKL probe disabled; handwritten transform retained";
    return true;
  }
  const QFileInfo info(QString::fromUtf8(env));
  if (!info.isAbsolute()) {
    error = "SS_MKL_DLL is not an absolute oneMKL runtime path";
    return true;
  }
  if (!info.isFile()) {
    error = "oneMKL probe path is missing; handwritten transform retained";
    return true;
  }
  path = info.absoluteFilePath().toStdWString();
  return true;
}

HMODULE loadSibling(const std::wstring& path) {
  if (!QFileInfo(QString::fromStdWString(path)).isFile()) return nullptr;
  // Absolute path plus the DLL directory. This does not search the current
  // directory or PATH; an already-loaded sibling is reused by name.
  return LoadLibraryExW(path.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

void preloadRuntime(const std::wstring& directory) {
  const wchar_t* names[] = {
      L"libimalloc.dll",
      L"libiomp5md.dll",
      L"mkl_core.3.dll",
      L"mkl_def.3.dll",
      L"mkl_avx2.3.dll",
      L"mkl_avx512.3.dll",
      L"mkl_avx10.3.dll",
      L"mkl_mc3.3.dll",
      L"mkl_vml_def.3.dll",
      L"mkl_vml_avx2.3.dll",
      L"mkl_vml_avx512.3.dll",
      L"mkl_vml_avx10.3.dll",
      L"mkl_vml_mc3.3.dll",
      L"mkl_vml_cmpt.3.dll",
      L"mkl_intel_thread.3.dll",
  };
  for (const auto* name : names) loadSibling(directory + L"\\" + name);
}

bool pinOneThread(HMODULE module, std::string& error) {
  using SetInt = void(__cdecl*)(int);
  using SetLocal = int(__cdecl*)(int);
  using GetInt = int(__cdecl*)();
  using SetLayer = int(__cdecl*)(int);
  const auto setLayer = reinterpret_cast<SetLayer>(GetProcAddress(module, "MKL_Set_Threading_Layer"));
  const auto setDynamic = reinterpret_cast<SetInt>(GetProcAddress(module, "MKL_Set_Dynamic"));
  const auto setThreads = reinterpret_cast<SetInt>(GetProcAddress(module, "MKL_Set_Num_Threads"));
  const auto setLocal = reinterpret_cast<SetLocal>(GetProcAddress(module, "MKL_Set_Num_Threads_Local"));
  const auto getThreads = reinterpret_cast<GetInt>(GetProcAddress(module, "MKL_Get_Max_Threads"));
  if (!setLayer || !setThreads || !setLocal || !getThreads) {
    error = "oneMKL thread control export is missing";
    return false;
  }
  // MKL_THREADING_INTEL is 0. Select it before the other service calls so the
  // dispatcher does not jump through an unloaded threading layer.
  if (setLayer(0) < 0) {
    error = "oneMKL Intel threading layer did not select";
    return false;
  }
  if (setDynamic) setDynamic(0);
  setThreads(1);
  if (setLocal(1) < 0) {
    error = "oneMKL local thread pin failed";
    return false;
  }
  const auto observed = getThreads();
  if (observed != 1) {
    error = "oneMKL did not stay on one thread";
    return false;
  }
  return true;
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
  if (module_ && create_ && createSingle_ && commit_ && execute_ && inverse_ && release_) {
    detail_ = runtimeDetail_;
    return true;
  }
  std::wstring path;
  if (probeOverride(path, error)) {
    if (path.empty()) {
      detail_ = error;
      return false;
    }
  } else {
    const auto directory = moduleDirectory();
    path = directory.empty() ? std::wstring{} : directory + L"\\mkl_rt.3.dll";
    if (path.empty() || !QFileInfo(QString::fromStdWString(path)).isFile()) {
      error = "oneMKL runtime is not in the executable deploy directory";
      detail_ = error;
      return false;
    }
  }
  // The runtime reads these at library initialization. Pin them before the
  // DLL load so the default pool cannot make a short STFT window slower.
  SetEnvironmentVariableW(L"MKL_NUM_THREADS", L"1");
  SetEnvironmentVariableW(L"MKL_DYNAMIC", L"FALSE");
  const auto slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) preloadRuntime(path.substr(0, slash));
  // Dependencies were preloaded from this directory. The current directory
  // and PATH are not searched for mkl_rt.3.dll itself.
  auto* module = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
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
  if (!create_ || !createSingle_ || !commit_ || !execute_ || !inverse_ || !release_ ||
      !pinOneThread(module, error)) {
    FreeLibrary(module);
    create_ = createSingle_ = nullptr;
    commit_ = nullptr;
    execute_ = inverse_ = nullptr;
    release_ = nullptr;
    if (error.empty()) error = "oneMKL DFTI exports are missing";
    detail_ = error;
    return false;
  }
  module_ = module;
  runtimeDetail_ = "oneMKL threads=1; " + QString::fromStdWString(path).toStdString();
  detail_ = runtimeDetail_;
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
  detail_ = runtimeDetail_;
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
