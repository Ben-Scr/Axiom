#pragma once

// EntityScriptOps
// ===============
// Free helper templates that route Entity::AddComponent<TScript>() /
// HasComponent<TScript>() / GetComponent<TScript>() / TryGetComponent /
// RemoveComponent calls to the engine's native-script subsystem
// (NativeScriptHost + ScriptComponent storage), so script types and
// component types share one API on Entity.
//
// Cycle-safe by construction: this header pulls in NO heavy script /
// scene machinery. The templates take a forward-declared `Scene*` and
// route through small non-template helpers in `EntityScriptOps.cpp`
// where Scene + NativeScriptHost + ScriptComponent are all complete.
// The template bodies themselves use only:
//   - NativeScriptRegistry::NameOfType (header-only, leaf include)
//   - dynamic_cast<T*>(NativeScript*) — requires NativeScript complete
//     at the call site (i.e. wherever a user instantiates these
//     templates), which is satisfied because the user's script type
//     itself derives from NativeScript and so transitively includes
//     NativeScript.hpp.
//
// Why this matters: Entity.hpp ends with `#include "EntityScriptOps.hpp"`,
// and Scene.hpp includes Entity.hpp at its top. If THIS header brought in
// Scene.hpp or NativeScript.hpp, the chain `Scene.hpp -> Entity.hpp ->
// EntityScriptOps.hpp -> NativeScript.hpp -> Scene.hpp (guard hit, no-op
// but Scene still incomplete)` would leave Scene's class members
// referenced in inline bodies undefined at compile time.

#include "Scene/EntityHandle.hpp"
#include "Scripting/NativeScriptRegistry.hpp"

#include <type_traits>
#include <typeinfo>

namespace Index {
	class Scene;
	class NativeScript;
}

namespace Index::EntityScriptOps::detail {
	// Non-template low-level helpers. Defined in EntityScriptOps.cpp
	// where Scene + NativeScriptHost + ScriptComponent are complete.
	// The template wrappers below resolve T -> name string via
	// NativeScriptRegistry (registered by REGISTER_SCRIPT), then
	// bounce through these. Pointer return is null when the entity
	// is invalid / not in the right state for the operation.
	NativeScript* AddScriptByName(Scene* scene, EntityHandle entity, const char* name);
	NativeScript* GetNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name);
	bool HasNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name);
	bool RemoveNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name);
}

namespace Index::EntityScriptOps {

	// Helper: lookup the registered string name for T. Returns nullptr
	// when T was never registered via REGISTER_SCRIPT — the call site
	// short-circuits without touching ScriptComponent or the host.
	template<typename T>
	const char* ResolveScriptName() {
		return NativeScriptRegistry::NameOfType(typeid(T));
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	T* AddScriptToEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return nullptr;
		NativeScript* ns = detail::AddScriptByName(scene, entity, name);
		return ns ? dynamic_cast<T*>(ns) : nullptr;
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	T* GetScriptOnEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return nullptr;
		NativeScript* ns = detail::GetNativeScriptOnEntity(scene, entity, name);
		return ns ? dynamic_cast<T*>(ns) : nullptr;
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	bool HasScriptOnEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return false;
		return detail::HasNativeScriptOnEntity(scene, entity, name);
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	bool TryGetScriptOnEntity(Scene* scene, EntityHandle entity, T*& out) {
		out = GetScriptOnEntity<T>(scene, entity);
		return out != nullptr;
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	bool RemoveScriptFromEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return false;
		return detail::RemoveNativeScriptOnEntity(scene, entity, name);
	}

} // namespace Index::EntityScriptOps
