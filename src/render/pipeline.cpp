// render/pipeline.cpp
// shader loading

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <plume_render_interface.h>
#include <zstd.h>

#include <rex/logging.h>

#include "generated/shader_cache.h"
#include "render/guest_resources.h"
#include "render/render_internal.h"

#ifdef _WIN32
#include <windows.h>
#include <objidl.h>
#include <unknwn.h>
#include <dxcapi.h>
#endif

using namespace plume;

namespace reodyssey::render {

namespace {

std::unique_ptr<uint8_t[]> g_dxilCache;
std::once_flag g_dxilCacheOnce;

std::unique_ptr<uint8_t[]> g_spirvCache;
std::once_flag g_spirvCacheOnce;

#ifdef _WIN32
class DxcRuntime {
 public:
  DxcRuntime() {
    library_ = LoadLibraryW(L"dxcompiler.dll");
    if (library_ == nullptr) {
      REXLOG_ERROR("DXC: failed to load dxcompiler.dll");
      return;
    }

    createInstance_ =
        reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(library_, "DxcCreateInstance"));
    if (createInstance_ == nullptr) {
      REXLOG_ERROR("DXC: failed to get DxcCreateInstance");
      return;
    }

    if (FAILED(createInstance_(CLSID_DxcCompiler, __uuidof(IDxcCompiler3),
                               reinterpret_cast<void**>(&compiler_))) ||
        compiler_ == nullptr) {
      REXLOG_ERROR("DXC: failed to create IDxcCompiler3");
      return;
    }
    if (FAILED(createInstance_(CLSID_DxcUtils, __uuidof(IDxcUtils),
                               reinterpret_cast<void**>(&utils_))) ||
        utils_ == nullptr) {
      REXLOG_ERROR("DXC: failed to create IDxcUtils");
      return;
    }
  }

  ~DxcRuntime() {
    for (auto& [_, blob] : specConstantLibraries_) {
      if (blob != nullptr) {
        blob->Release();
      }
    }
    if (utils_ != nullptr) {
      utils_->Release();
    }
    if (compiler_ != nullptr) {
      compiler_->Release();
    }
    if (library_ != nullptr) {
      FreeLibrary(library_);
    }
  }

  DxcRuntime(const DxcRuntime&) = delete;
  DxcRuntime& operator=(const DxcRuntime&) = delete;

  bool ready() const { return compiler_ != nullptr && utils_ != nullptr; }

  IDxcBlob* GetSpecConstantLibrary(uint32_t specConstants) {
    std::lock_guard lock(mutex_);

    auto cached = specConstantLibraries_.find(specConstants);
    if (cached != specConstantLibraries_.end()) {
      cached->second->AddRef();
      return cached->second;
    }

    char source[128];
    int sourceSize = std::snprintf(source, sizeof(source),
                                   "export uint g_SpecConstants() { return %u; }",
                                   specConstants);
    assert(sourceSize > 0 && sourceSize < static_cast<int>(sizeof(source)));

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source;
    sourceBuffer.Size = static_cast<size_t>(sourceSize);
    sourceBuffer.Encoding = DXC_CP_ACP;

    LPCWSTR args[] = {L"-T lib_6_3"};

    IDxcResult* result = nullptr;
    HRESULT hr = compiler_->Compile(&sourceBuffer, args, std::size(args), nullptr,
                                    __uuidof(IDxcResult),
                                    reinterpret_cast<void**>(&result));
    if (FAILED(hr) || result == nullptr) {
      REXLOG_ERROR("DXC: spec constant library compile failed hr=0x%08X",
                   static_cast<unsigned>(hr));
      return nullptr;
    }

    IDxcBlob* object = GetResultObject(result, "DXC: spec constant library");
    result->Release();
    if (object != nullptr) {
      object->AddRef();
      specConstantLibraries_.emplace(specConstants, object);
    }
    return object;
  }

  IDxcBlobEncoding* CreatePinnedLibraryBlob(const void* dxilData, uint32_t dxilSize) {
    if (!ready()) {
      return nullptr;
    }

    IDxcBlobEncoding* shaderBlob = nullptr;
    HRESULT hr = utils_->CreateBlobFromPinned(dxilData, dxilSize, DXC_CP_ACP, &shaderBlob);
    if (FAILED(hr) || shaderBlob == nullptr) {
      REXLOG_ERROR("DXC: failed to create shader library blob hr=0x%08X",
                   static_cast<unsigned>(hr));
      return nullptr;
    }
    return shaderBlob;
  }

  IDxcBlob* LinkShaderLibrary(IDxcBlob* shaderBlob, uint32_t dxilOffset, ResourceType shaderType,
                              uint32_t specConstants) {
    if (!ready()) {
      return nullptr;
    }

    IDxcBlob* specBlob = GetSpecConstantLibrary(specConstants);
    if (specBlob == nullptr) {
      return nullptr;
    }

    IDxcLinker* linker = nullptr;
    HRESULT hr = createInstance_(CLSID_DxcLinker, __uuidof(IDxcLinker),
                                 reinterpret_cast<void**>(&linker));
    if (FAILED(hr) || linker == nullptr) {
      REXLOG_ERROR("DXC: failed to create IDxcLinker hr=0x%08X", static_cast<unsigned>(hr));
      specBlob->Release();
      return nullptr;
    }

    wchar_t specConstantsLibName[64];
    swprintf_s(specConstantsLibName, L"SpecConstants_%u", specConstants);
    wchar_t shaderLibName[64];
    swprintf_s(shaderLibName, L"Shader_%u", dxilOffset);

    bool registered =
        SUCCEEDED(linker->RegisterLibrary(specConstantsLibName, specBlob)) &&
        SUCCEEDED(linker->RegisterLibrary(shaderLibName, shaderBlob));
    specBlob->Release();
    if (!registered) {
      REXLOG_ERROR("DXC: failed to register shader libraries");
      linker->Release();
      return nullptr;
    }

    const wchar_t* libraries[] = {specConstantsLibName, shaderLibName};
    const wchar_t* profile =
        shaderType == ResourceType::VertexShader ? L"vs_6_0" : L"ps_6_0";
    IDxcOperationResult* linkResult = nullptr;
    hr = linker->Link(L"shaderMain", profile, libraries, std::size(libraries), nullptr, 0,
                      &linkResult);
    linker->Release();
    if (FAILED(hr) || linkResult == nullptr) {
      REXLOG_ERROR("DXC: shader link failed hr=0x%08X", static_cast<unsigned>(hr));
      return nullptr;
    }

    IDxcBlob* object = GetOperationResultObject(linkResult, "DXC: shader link");
    linkResult->Release();
    return object;
  }

 private:
  static IDxcBlob* GetResultObject(IDxcResult* result, const char* label) {
    HRESULT status = E_FAIL;
    HRESULT hr = result->GetStatus(&status);
    if (FAILED(hr) || FAILED(status)) {
      LogResultErrors(result, label);
      return nullptr;
    }

    IDxcBlob* object = nullptr;
    hr = result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&object),
                           nullptr);
    if (FAILED(hr) || object == nullptr) {
      REXLOG_ERROR("%s: failed to get object hr=0x%08X", label, static_cast<unsigned>(hr));
      return nullptr;
    }
    return object;
  }

  static IDxcBlob* GetOperationResultObject(IDxcOperationResult* result, const char* label) {
    HRESULT status = E_FAIL;
    HRESULT hr = result->GetStatus(&status);
    if (FAILED(hr) || FAILED(status)) {
      IDxcBlobEncoding* errors = nullptr;
      if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors != nullptr) {
        REXLOG_ERROR("%s: %.*s", label, static_cast<int>(errors->GetBufferSize()),
                     static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
      } else {
        REXLOG_ERROR("%s: failed status hr=0x%08X", label, static_cast<unsigned>(status));
      }
      return nullptr;
    }

    IDxcBlob* object = nullptr;
    hr = result->GetResult(&object);
    if (FAILED(hr) || object == nullptr) {
      REXLOG_ERROR("%s: failed to get linked object hr=0x%08X", label,
                   static_cast<unsigned>(hr));
      return nullptr;
    }
    return object;
  }

  static void LogResultErrors(IDxcResult* result, const char* label) {
    IDxcBlobUtf8* errors = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8),
                                    reinterpret_cast<void**>(&errors), nullptr)) &&
        errors != nullptr) {
      REXLOG_ERROR("%s: %s", label, errors->GetStringPointer());
      errors->Release();
    } else {
      REXLOG_ERROR("%s: failed", label);
    }
  }

  HMODULE library_ = nullptr;
  DxcCreateInstanceProc createInstance_ = nullptr;
  IDxcCompiler3* compiler_ = nullptr;
  IDxcUtils* utils_ = nullptr;
  std::mutex mutex_;
  std::unordered_map<uint32_t, IDxcBlob*> specConstantLibraries_;
};

DxcRuntime& GetDxcRuntime() {
  static DxcRuntime runtime;
  return runtime;
}
#endif

void EnsureDxilCache() {
  std::call_once(g_dxilCacheOnce, [] {
    g_dxilCache = std::make_unique<uint8_t[]>(g_dxilCacheDecompressedSize);
    ZSTD_decompress(g_dxilCache.get(), g_dxilCacheDecompressedSize, g_compressedDxilCache,
                    g_dxilCacheCompressedSize);
  });
}

void EnsureSpirvCache() {
  std::call_once(g_spirvCacheOnce, [] {
    g_spirvCache = std::make_unique<uint8_t[]>(g_spirvCacheDecompressedSize);
    ZSTD_decompress(g_spirvCache.get(), g_spirvCacheDecompressedSize, g_compressedSpirvCache,
                    g_spirvCacheCompressedSize);
  });
}

}  // namespace

#ifdef _WIN32
GuestShader::~GuestShader() {
  if (dxilLibraryBlob != nullptr) {
    dxilLibraryBlob->Release();
  }
}
#else
GuestShader::~GuestShader() = default;
#endif

RenderShader* LoadShader(GuestShader* guestShader, uint32_t specConstants) {
  if (guestShader == nullptr) {
    return nullptr;
  }

  if (guestShader->shaderCacheEntry == nullptr) {
    return nullptr;
  }

  const ShaderCacheEntry* entry = guestShader->shaderCacheEntry;
  RenderShaderFormat fmt = Interface()->getCapabilities().shaderFormat;
  if (fmt == RenderShaderFormat::SPIRV) {
    if (guestShader->shader != nullptr) {
      return guestShader->shader.get();
    }
    EnsureSpirvCache();
    guestShader->shader = Device()->createShader(
        g_spirvCache.get() + entry->spirv_offset,
        entry->spirv_size, "main", RenderShaderFormat::SPIRV);
    return guestShader->shader.get();
  }

#ifdef _WIN32
  EnsureDxilCache();
  if (entry->spec_constants_mask == 0) {
    if (guestShader->shader != nullptr) {
      return guestShader->shader.get();
    }
    guestShader->shader = Device()->createShader(g_dxilCache.get() + entry->dxil_offset,
                                                 entry->dxil_size, "main",
                                                 RenderShaderFormat::DXIL);
    return guestShader->shader.get();
  }

  uint32_t specializedValue = specConstants & entry->spec_constants_mask;
  {
    std::lock_guard lock(guestShader->mutex);
    auto cached = guestShader->specializedShaders.find(specializedValue);
    if (cached != guestShader->specializedShaders.end()) {
      return cached->second.get();
    }
    if (guestShader->dxilLibraryBlob == nullptr) {
      guestShader->dxilLibraryBlob = GetDxcRuntime().CreatePinnedLibraryBlob(
          g_dxilCache.get() + entry->dxil_offset, entry->dxil_size);
    }
  }

  IDxcBlobEncoding* libraryBlob = nullptr;
  {
    std::lock_guard lock(guestShader->mutex);
    libraryBlob = guestShader->dxilLibraryBlob;
    if (libraryBlob != nullptr) {
      libraryBlob->AddRef();
    }
  }
  if (libraryBlob == nullptr) {
    return nullptr;
  }

  IDxcBlob* linkedBlob = GetDxcRuntime().LinkShaderLibrary(
      libraryBlob, entry->dxil_offset, guestShader->type, specializedValue);
  libraryBlob->Release();
  if (linkedBlob == nullptr) {
    return nullptr;
  }

  std::unique_ptr<RenderShader> shader = Device()->createShader(
      linkedBlob->GetBufferPointer(), linkedBlob->GetBufferSize(), "main",
      RenderShaderFormat::DXIL);
  linkedBlob->Release();

  RenderShader* shaderPtr = shader.get();
  {
    std::lock_guard lock(guestShader->mutex);
    auto [it, inserted] =
        guestShader->specializedShaders.emplace(specializedValue, std::move(shader));
    shaderPtr = it->second.get();
  }
  return shaderPtr;
#else
  return nullptr;
#endif
}

}  // namespace reodyssey::render
