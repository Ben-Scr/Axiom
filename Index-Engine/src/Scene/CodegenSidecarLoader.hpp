#pragma once

#include "Core/Export.hpp"

namespace Index {

	class SceneManager;

	// Loads Index-GameComponents.dll (the per-project user-component
	// sidecar) and invokes its exported `IndexRegisterCodegenComponents`
	// against the provided SceneManager. The DLL is discovered next to
	// the consumer executable via Path::ExecutableDir().
	//
	// Missing DLL or missing export is NOT an error — it's the same as
	// "the user has not authored any [NativeComponent(..., Generate =
	// true)] components." The function logs an info line and returns
	// without registering anything. This keeps the engine usable on
	// fresh checkouts or when the editor's Rebuild Engine flow has not
	// yet produced a sidecar build.
	//
	// The loaded DLL handle stays resident for the lifetime of the
	// process. Component type registrations hold pointers into the
	// sidecar's read-only data (RTTI names, vtables), so we cannot
	// FreeLibrary it without invalidating those pointers.
	void INDEX_API LoadCodegenComponents(SceneManager& sceneManager);

}
