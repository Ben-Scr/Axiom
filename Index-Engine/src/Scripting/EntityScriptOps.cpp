#include "pch.hpp"
#include "Scripting/EntityScriptOps.hpp"

#include "Scene/Scene.hpp"
#include "Scripting/NativeScript.hpp"
#include "Scripting/NativeScriptHost.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptInstance.hpp"
#include "Scripting/ScriptSystem.hpp"

namespace Index::EntityScriptOps::detail {

	NativeScript* AddScriptByName(Scene* scene, EntityHandle entity, const char* name) {
		if (!scene || entity == entt::null || !name) return nullptr;
		entt::registry& registry = scene->GetRegistry();
		if (!registry.valid(entity)) return nullptr;

		ScriptComponent& sc = registry.all_of<ScriptComponent>(entity)
			? registry.get<ScriptComponent>(entity)
			: registry.emplace<ScriptComponent>(entity);

		// Return the existing native instance if one is already
		// attached. The caller dynamic_casts to the requested T to
		// confirm the concrete derived type matches.
		for (auto& inst : sc.Scripts) {
			if (inst.GetType() != ScriptType::Native) continue;
			if (inst.GetClassName() != name) continue;
			return inst.GetNativePtr();
		}

		sc.AddScript(name, ScriptType::Native);
		ScriptInstance* added = nullptr;
		for (auto& inst : sc.Scripts) {
			if (inst.GetClassName() == name && inst.GetType() == ScriptType::Native) {
				added = &inst;
				break;
			}
		}
		if (!added) return nullptr;

		added->Bind(entity);
		NativeScript* native = ScriptSystem::GetNativeHost().CreateInstance(name, entity, scene);
		if (!native) return nullptr;
		added->SetNativePtr(native);

		// Run Start() inline so the caller sees a live script immediately
		// after AddComponent returns. ScriptSystem::Update honours
		// MarkStarted() so this Start() won't run again.
		try {
			native->Start();
		}
		catch (...) {
			// Swallow — same policy as ScriptSystem's lifecycle sweep.
		}
		added->MarkStarted();
		return native;
	}

	NativeScript* GetNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name) {
		if (!scene || entity == entt::null || !name) return nullptr;
		entt::registry& registry = scene->GetRegistry();
		if (!registry.valid(entity)) return nullptr;
		auto* sc = registry.try_get<ScriptComponent>(entity);
		if (!sc) return nullptr;
		for (auto& inst : sc->Scripts) {
			if (inst.GetType() != ScriptType::Native) continue;
			if (inst.GetClassName() != name) continue;
			return inst.GetNativePtr();
		}
		return nullptr;
	}

	bool HasNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name) {
		return GetNativeScriptOnEntity(scene, entity, name) != nullptr;
	}

	bool RemoveNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name) {
		if (!scene || entity == entt::null || !name) return false;
		entt::registry& registry = scene->GetRegistry();
		if (!registry.valid(entity)) return false;
		auto* sc = registry.try_get<ScriptComponent>(entity);
		if (!sc) return false;
		for (auto it = sc->Scripts.begin(); it != sc->Scripts.end(); ++it) {
			if (it->GetType() != ScriptType::Native) continue;
			if (it->GetClassName() != name) continue;
			NativeScript* ns = it->GetNativePtr();
			if (ns) {
				ScriptSystem::GetNativeHost().DestroyInstance(ns);
			}
			sc->Scripts.erase(it);
			return true;
		}
		return false;
	}

} // namespace Index::EntityScriptOps::detail
