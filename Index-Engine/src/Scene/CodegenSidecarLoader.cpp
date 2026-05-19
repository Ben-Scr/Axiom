#include "pch.hpp"
#include "Scene/CodegenSidecarLoader.hpp"
#include "Core/Log.hpp"
#include "Serialization/Path.hpp"

#include <filesystem>

#if defined(IDX_PLATFORM_WINDOWS)
#include <windows.h>
#elif defined(IDX_PLATFORM_LINUX)
#include <dlfcn.h>
#endif

namespace Index {

	namespace {

#if defined(IDX_PLATFORM_WINDOWS)
		constexpr const char* k_SidecarFileName = "Index-GameComponents.dll";
#elif defined(IDX_PLATFORM_LINUX)
		constexpr const char* k_SidecarFileName = "libIndex-GameComponents.so";
#else
		constexpr const char* k_SidecarFileName = "Index-GameComponents";
#endif

		constexpr const char* k_ExportedSymbolName = "IndexRegisterCodegenComponents";

		using RegisterFn = void (*)(SceneManager*);

		void* OpenLibrary(const std::filesystem::path& sidecarPath) {
#if defined(IDX_PLATFORM_WINDOWS)
			return static_cast<void*>(LoadLibraryA(sidecarPath.string().c_str()));
#elif defined(IDX_PLATFORM_LINUX)
			return dlopen(sidecarPath.string().c_str(), RTLD_NOW);
#else
			(void)sidecarPath;
			return nullptr;
#endif
		}

		void* ResolveSymbol(void* handle, const char* symbol) {
#if defined(IDX_PLATFORM_WINDOWS)
			return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), symbol));
#elif defined(IDX_PLATFORM_LINUX)
			dlerror();
			return dlsym(handle, symbol);
#else
			(void)handle; (void)symbol;
			return nullptr;
#endif
		}

	}

	void LoadCodegenComponents(SceneManager& sceneManager) {
		const std::filesystem::path exeDir = std::filesystem::path(Path::ExecutableDir());
		const std::filesystem::path sidecarPath = exeDir / k_SidecarFileName;

		std::error_code ec;
		if (!std::filesystem::exists(sidecarPath, ec)) {
			// No sidecar = no user-generated components. Equivalent to the
			// old no-op stub. Info-level only — a real warning would fire
			// on every fresh checkout before the editor's Rebuild Engine
			// flow has produced a sidecar build.
			IDX_CORE_INFO_TAG("Scene",
				"Codegen sidecar not found at '{}'. No user-defined native components will be registered.",
				sidecarPath.string());
			return;
		}

		// The handle is intentionally leaked: SceneManager keeps pointers
		// into the sidecar's read-only data (RTTI names, the registration
		// payload) for the lifetime of the process. FreeLibrary would
		// invalidate those pointers and corrupt later lookups.
		void* handle = OpenLibrary(sidecarPath);
		if (!handle) {
			IDX_CORE_ERROR_TAG("Scene",
				"Failed to load codegen sidecar '{}'. User-defined native components will be skipped.",
				sidecarPath.string());
			return;
		}

		auto registerFn = reinterpret_cast<RegisterFn>(
			ResolveSymbol(handle, k_ExportedSymbolName));
		if (!registerFn) {
			IDX_CORE_ERROR_TAG("Scene",
				"Codegen sidecar '{}' loaded but does not export '{}'. User-defined components will be skipped.",
				sidecarPath.string(), k_ExportedSymbolName);
			return;
		}

		registerFn(&sceneManager);
	}

}
