#include "pch.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Scripting/ScriptBindings.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Input.hpp"
#include "Core/Time.hpp"
#include "Core/Window.hpp"
#include "Core/Log.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/AxiomProject.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/UI/ButtonComponent.hpp"
#include "Components/UI/DropdownComponent.hpp"
#include "Components/UI/InputFieldComponent.hpp"
#include "Components/UI/InteractableComponent.hpp"
#include "Components/UI/SliderComponent.hpp"
#include "Components/UI/ToggleComponent.hpp"
#include "Components/Tags.hpp"
#include "Audio/AudioManager.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/Gizmo.hpp"
#include "Physics/Physics2D.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

namespace Axiom {
	bool ScriptBindings::IsScriptInputEnabled()
	{
		return Application::s_Instance && Application::s_Instance->m_IsScriptInputEnabled;
	}

	EntityHandle ToEntityHandle(uint64_t id)
	{
		return static_cast<EntityHandle>(static_cast<uint32_t>(id));
	}

	uint64_t FromEntityHandle(EntityHandle handle)
	{
		return static_cast<uint64_t>(static_cast<uint32_t>(handle));
	}

	uint64_t GetEntityScriptId(const Scene& scene, EntityHandle handle)
	{
		if (scene.IsValid(handle)) {
			const uint64_t runtimeId = scene.GetRuntimeID(handle);
			if (runtimeId != 0) {
				return runtimeId;
			}
		}

		return FromEntityHandle(handle);
	}

	bool TryResolveEntityByUUID(const Scene& scene, uint64_t entityID, EntityHandle& outHandle)
	{
		if (scene.TryResolveRuntimeID(entityID, outHandle)) {
			return true;
		}

		auto view = scene.GetRegistry().view<UUIDComponent>();
		for (EntityHandle handle : view) {
			if (static_cast<uint64_t>(view.get<UUIDComponent>(handle).Id) == entityID) {
				outHandle = handle;
				return true;
			}
		}

		return false;
	}

	bool ResolveEntityReference(uint64_t entityID, Scene*& outScene, EntityHandle& outHandle)
	{
		outScene = nullptr;
		outHandle = entt::null;

		if (entityID == 0) {
			return false;
		}

		// Never fall back to ToEntityHandle(entityID) — UUIDs would truncate and silently alias entt handles.
		Scene* currentScene = ScriptEngine::GetScene();
		if (currentScene) {
			if (TryResolveEntityByUUID(*currentScene, entityID, outHandle)) {
				outScene = currentScene;
				return true;
			}
		}

		SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
			if (outScene || &scene == currentScene) {
				return;
			}

			EntityHandle resolvedHandle = entt::null;
			if (TryResolveEntityByUUID(scene, entityID, resolvedHandle)) {
				outScene = const_cast<Scene*>(&scene);
				outHandle = resolvedHandle;
			}
		});

		return outScene != nullptr;
	}

	Scene* GetScene()
	{
		Scene* scene = ScriptEngine::GetScene();
		if (scene && scene->IsLoaded()) {
			return scene;
		}

		auto* app = Application::GetInstance();
		if (app && app->GetSceneManager()) {
			return app->GetSceneManager()->GetActiveScene();
		}
		return nullptr;
	}

	// Legacy string callbacks use this per-thread scratch buffer. Managed callers use
	// the Buffer variants below so returned strings are copied into caller-owned memory.
	//
	// HAZARD: the const char* returned by every "no-buffer" binding below points into
	// this thread-local. Managed code that holds the pointer past the next binding
	// call on the same thread reads stale or different content. The Buffer variants
	// (those whose C# signature ends with `out byte[]` or `(IntPtr, int)`) copy the
	// string into caller-owned memory and are safe; the no-Buffer variants are kept
	// only for backwards compatibility with older C# entry points.
	//
	// Migration plan: every legacy entry point should be marked [Obsolete] on the C#
	// side and routed through its Buffer variant. Once no callers remain, delete the
	// legacy thunks. Do not add new legacy-style bindings.
	thread_local std::string s_StringReturnBuffer;

	// C++ exceptions MUST NOT propagate across the C++/C# boundary — managed code
	// invokes these via [UnmanagedCallersOnly] function pointers, and an exception
	// unwinding through CoreCLR-managed frames is undefined behavior. Many bindings
	// allocate (std::string, std::vector, JSON parse), and any of those can throw
	// std::bad_alloc on a near-OOM editor. The macros below give us a uniform way
	// to swallow exceptions at every binding boundary while still logging them.
	//
	// Usage:
	//   static T MyBinding(args...) {
	//       AIM_BINDING_TRY {
	//           ... body that may allocate ...
	//           return result;
	//       } AIM_BINDING_CATCH(default_return_value);
	//   }
	#define AIM_BINDING_TRY try
	#define AIM_BINDING_CATCH(default_return)                                                  \
		catch (const std::exception& _aim_ex) {                                                \
			AIM_CORE_ERROR_TAG("ScriptBinding", "exception in {}: {}", __func__, _aim_ex.what()); \
			return default_return;                                                             \
		}                                                                                      \
		catch (...) {                                                                          \
			AIM_CORE_ERROR_TAG("ScriptBinding", "unknown exception in {}", __func__);          \
			return default_return;                                                             \
		}
	// Void-return variant (no return statement in catch).
	#define AIM_BINDING_CATCH_VOID                                                             \
		catch (const std::exception& _aim_ex) {                                                \
			AIM_CORE_ERROR_TAG("ScriptBinding", "exception in {}: {}", __func__, _aim_ex.what()); \
		}                                                                                      \
		catch (...) {                                                                          \
			AIM_CORE_ERROR_TAG("ScriptBinding", "unknown exception in {}", __func__);          \
		}

	static int CopyStringToBuffer(std::string_view value, char* outBuffer, int capacity)
	{
		const int requiredBytes = static_cast<int>(std::min(
			value.size(),
			static_cast<size_t>(std::numeric_limits<int>::max())));
		if (outBuffer && capacity > 0) {
			const int bytesToCopy = std::min(requiredBytes, capacity - 1);
			if (bytesToCopy > 0) {
				std::memcpy(outBuffer, value.data(), static_cast<size_t>(bytesToCopy));
			}
			outBuffer[bytesToCopy] = '\0';
		}
		return requiredBytes;
	}

	static int CopyCStringToBuffer(const char* value, char* outBuffer, int capacity)
	{
		return CopyStringToBuffer(value ? std::string_view(value) : std::string_view(), outBuffer, capacity);
	}

	void PopulateNonComponentBindings(NativeBindings& b);

	// IsValid re-check: ResolveEntityReference can succeed at lookup time, but a
	// script callback firing between resolution and the registry access may have
	// destroyed the entity. Without IsValid, GetComponent walks a stale handle.
	#define GET_COMPONENT(Type, entityID, failReturn) \
		Scene* scene = nullptr; \
		EntityHandle handle = entt::null; \
		if (!ResolveEntityReference(entityID, scene, handle)) return failReturn; \
		if (!scene->IsValid(handle)) return failReturn; \
		if (!scene->HasComponent<Type>(handle)) return failReturn; \
		auto& comp = scene->GetComponent<Type>(handle)


	// Tries displayName, serializedName, and the +"Component" C# fallbacks for package types.
	static const ComponentInfo* FindComponentByName(const std::string& name) {
		const auto& registry = SceneManager::Get().GetComponentRegistry();
		const ComponentInfo* found = nullptr;
		registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
			if (found) return;
			if (info.displayName == name) { found = &info; return; }
			if (!info.serializedName.empty() && info.serializedName == name) { found = &info; return; }
			if (!info.serializedName.empty() && (info.serializedName + "Component") == name) {
				found = &info;
				return;
			}
			if (!info.displayName.empty()) {
				std::string normalized;
				normalized.reserve(info.displayName.size());
				for (char c : info.displayName) {
					if (c != ' ') normalized.push_back(c);
				}
				if ((normalized + "Component") == name) {
					found = &info;
					return;
				}
			}
		});
		return found;
	}

	static const ComponentInfo* FindComponentByName(const char* name) {
		return FindComponentByName(std::string(name ? name : ""));
	}

	static bool HasManagedComponent(Scene& scene, EntityHandle handle, const std::string& className) {
		if (!scene.IsValid(handle) || !scene.HasComponent<ScriptComponent>(handle)) return false;
		return scene.GetComponent<ScriptComponent>(handle).HasManagedComponent(className);
	}

	static bool AddManagedComponent(Scene& scene, EntityHandle handle, const std::string& className) {
		if (!scene.IsValid(handle) || className.empty()) return false;
		if (!scene.HasComponent<ScriptComponent>(handle)) {
			scene.AddComponent<ScriptComponent>(handle);
		}
		auto& scriptComponent = scene.GetComponent<ScriptComponent>(handle);
		if (scriptComponent.HasManagedComponent(className)) return true;
		scriptComponent.AddManagedComponent(className);
		scene.MarkDirty();
		return true;
	}

	static bool RemoveManagedComponent(Scene& scene, EntityHandle handle, const std::string& className) {
		if (!scene.IsValid(handle) || !scene.HasComponent<ScriptComponent>(handle)) return false;
		const bool removed = scene.GetComponent<ScriptComponent>(handle).RemoveManagedComponent(className);
		if (removed) scene.MarkDirty();
		return removed;
	}

	static const char* Axiom_Entity_GetManagedComponentFields(uint64_t entityID, const char* componentName)
	{
		AIM_BINDING_TRY {
			s_StringReturnBuffer = "{}";
			Scene* scene = nullptr;
			EntityHandle handle = entt::null;
			const std::string className = componentName ? componentName : "";
			if (className.empty() || !ResolveEntityReference(entityID, scene, handle)
				|| !scene->HasComponent<ScriptComponent>(handle)) {
				return s_StringReturnBuffer.c_str();
			}

			const auto& scriptComponent = scene->GetComponent<ScriptComponent>(handle);
			if (!scriptComponent.HasManagedComponent(className)) {
				return s_StringReturnBuffer.c_str();
			}

			Json::Value root = Json::Value::MakeObject();
			const std::string prefix = className + ".";
			for (const auto& [key, value] : scriptComponent.PendingFieldValues) {
				if (key.rfind(prefix, 0) != 0) {
					continue;
				}
				root.AddMember(key.substr(prefix.size()), Json::Value(value));
			}

			s_StringReturnBuffer = Json::Stringify(root, false);
			return s_StringReturnBuffer.c_str();
		} AIM_BINDING_CATCH("{}");
	}

	static int Axiom_Entity_GetManagedComponentFieldsBuffer(uint64_t entityID, const char* componentName, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_Entity_GetManagedComponentFields(entityID, componentName), outBuffer, capacity);
	}

	static int Axiom_Entity_GetIsStatic(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;
		return scene->HasComponent<StaticTag>(handle) ? 1 : 0;
	}

	static void Axiom_Entity_SetIsStatic(uint64_t entityID, int isStatic)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return;

		const bool shouldBeStatic = isStatic != 0;
		const bool currentlyStatic = scene->HasComponent<StaticTag>(handle);
		if (shouldBeStatic == currentlyStatic) return;

		if (shouldBeStatic) {
			scene->AddComponent<StaticTag>(handle);
		}
		else {
			scene->RemoveComponent<StaticTag>(handle);
		}
		scene->MarkDirty();
	}

	static int Axiom_Entity_GetIsEnabled(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;
		return scene->HasComponent<DisabledTag>(handle) ? 0 : 1;
	}

	static void Axiom_Entity_SetIsEnabled(uint64_t entityID, int isEnabled)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return;

		const bool shouldBeEnabled = isEnabled != 0;
		const bool currentlyEnabled = !scene->HasComponent<DisabledTag>(handle);
		if (shouldBeEnabled == currentlyEnabled) return;

		scene->GetEntity(handle).SetEnabled(shouldBeEnabled);
		scene->MarkDirty();
	}

	struct QueryComponentRequirement {
		const ComponentInfo* Native = nullptr;
		std::string ManagedClassName;

		bool Has(Scene& scene, EntityHandle handle, Entity entity) const {
			if (Native && Native->has) {
				return Native->has(entity);
			}
			return HasManagedComponent(scene, handle, ManagedClassName);
		}
	};

	static bool BuildComponentRequirement(const std::string& name, QueryComponentRequirement& out) {
		if (name.empty()) {
			return false;
		}

		const ComponentInfo* info = FindComponentByName(name);
		if (info && info->has) {
			out.Native = info;
			return true;
		}

		out.ManagedClassName = name;
		return true;
	}

	int Axiom_Entity_HasComponent(uint64_t entityID, const char* componentName)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;

		const ComponentInfo* info = FindComponentByName(componentName);
		if (!info || !info->has) return HasManagedComponent(*scene, handle, componentName ? componentName : "") ? 1 : 0;
		return info->has(scene->GetEntity(handle)) ? 1 : 0;
	}

	int Axiom_Entity_AddComponent(uint64_t entityID, const char* componentName)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;

		const ComponentInfo* info = FindComponentByName(componentName);
		if (!info || !info->add) return AddManagedComponent(*scene, handle, componentName ? componentName : "") ? 1 : 0;

		Entity entity = scene->GetEntity(handle);
		if (info->has && info->has(entity)) return 1;
		info->add(entity);
		return 1;
	}

	int Axiom_Entity_RemoveComponent(uint64_t entityID, const char* componentName)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;

		const ComponentInfo* info = FindComponentByName(componentName);
		if (!info || !info->remove) return RemoveManagedComponent(*scene, handle, componentName ? componentName : "") ? 1 : 0;

		Entity entity = scene->GetEntity(handle);
		if (info->has && !info->has(entity)) return 0;
		info->remove(entity);
		return 1;
	}

	uint64_t Axiom_Entity_Clone(uint64_t sourceEntityID)
	{
		Scene* targetScene = GetScene();
		if (!targetScene) return 0;

		Scene* sourceScene = nullptr;
		EntityHandle sourceHandle = entt::null;
		if (!ResolveEntityReference(sourceEntityID, sourceScene, sourceHandle)) return 0;

		Entity source = sourceScene->GetEntity(sourceHandle);
		std::string name = source.GetName();

		Entity clone = targetScene->CreateRuntimeEntity(name + " (Clone)");

		const auto& registry = SceneManager::Get().GetComponentRegistry();
		registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
			if (info.category != ComponentCategory::Component) return;
			if (!info.has(source)) return;
			if (info.copyTo) {
				info.copyTo(source, clone);
			}
		});

		return GetEntityScriptId(*targetScene, clone.GetHandle());
	}

	uint64_t Axiom_Entity_InstantiatePrefab(uint64_t prefabGuid)
	{
		Scene* targetScene = GetScene();
		if (!targetScene) return 0;

		const EntityHandle instance = SceneSerializer::InstantiatePrefab(*targetScene, prefabGuid);
		return instance != entt::null ? GetEntityScriptId(*targetScene, instance) : 0;
	}

	int Axiom_Entity_GetOrigin(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return static_cast<int>(EntityOrigin::Runtime);
		return static_cast<int>(scene->GetEntityOrigin(handle));
	}

	uint64_t Axiom_Entity_GetRuntimeID(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;
		return scene->GetRuntimeID(handle);
	}

	uint64_t Axiom_Entity_GetSceneGUID(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;
		return static_cast<uint64_t>(scene->GetSceneEntityGUID(handle));
	}

	uint64_t Axiom_Entity_GetPrefabGUID(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;
		return static_cast<uint64_t>(scene->GetPrefabGUID(handle));
	}

	// ── Scene ───────────────────────────────────────────────────────────

	static const char* Axiom_Scene_GetActiveSceneName() {
		AIM_BINDING_TRY {
			Scene* scene = GetScene();
			if (!scene) { s_StringReturnBuffer.clear(); return s_StringReturnBuffer.c_str(); }
			s_StringReturnBuffer = scene->GetName();
			return s_StringReturnBuffer.c_str();
		} AIM_BINDING_CATCH("");
	}

	static int Axiom_Scene_GetActiveSceneNameBuffer(char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_Scene_GetActiveSceneName(), outBuffer, capacity);
	}

	static int CountSceneEntities(const Scene& scene) {
		int count = 0;
		auto view = scene.GetRegistry().view<entt::entity>();
		for (EntityHandle entityHandle : view) {
			if (scene.GetRegistry().valid(entityHandle)) {
				++count;
			}
		}

		return count;
	}

	static int Axiom_Scene_GetEntityCount() {
		Scene* scene = GetScene();
		if (!scene) return 0;
		return CountSceneEntities(*scene);
	}

	static int Axiom_Scene_GetEntityCountByName(const char* sceneName) {
		if (!sceneName || sceneName[0] == '\0') return 0;
		auto scene = SceneManager::Get().GetLoadedScene(sceneName).lock();
		if (!scene || !scene->IsLoaded()) return 0;
		return CountSceneEntities(*scene);
	}

	static const char* Axiom_Scene_GetEntityNameByUUID(uint64_t uuid) {
		Scene* scene = GetScene();
		if (!scene) { s_StringReturnBuffer.clear(); return s_StringReturnBuffer.c_str(); }

		Scene* resolvedScene = nullptr;
		EntityHandle resolvedHandle = entt::null;
		if (ResolveEntityReference(uuid, resolvedScene, resolvedHandle)
			&& resolvedScene
			&& resolvedScene->HasComponent<NameComponent>(resolvedHandle)) {
			s_StringReturnBuffer = resolvedScene->GetComponent<NameComponent>(resolvedHandle).Name;
			return s_StringReturnBuffer.c_str();
		}

		auto view = scene->GetRegistry().view<UUIDComponent, NameComponent>();
		for (auto [ent, uuidComp, nameComp] : view.each()) {
			if (static_cast<uint64_t>(uuidComp.Id) == uuid) {
				s_StringReturnBuffer = nameComp.Name;
				return s_StringReturnBuffer.c_str();
			}
		}
		s_StringReturnBuffer.clear();
		return s_StringReturnBuffer.c_str();
	}

	static int Axiom_Scene_GetEntityNameByUUIDBuffer(uint64_t uuid, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_Scene_GetEntityNameByUUID(uuid), outBuffer, capacity);
	}

	static int LoadSceneByName(const char* sceneName, bool additive) {
		auto& sm = SceneManager::Get();
		std::string name(sceneName);
		AxiomProject* project = ProjectManager::GetCurrentProject();
		const bool hasDefinition = sm.HasSceneDefinition(name);

		if (!hasDefinition) {
			auto& definition = sm.RegisterScene(name);
			if (project) {
				const std::string scenePath = project->GetSceneFilePath(name);
				definition.OnLoad([scenePath](Scene& scene) {
					if (File::Exists(scenePath)) {
						SceneSerializer::LoadFromFile(scene, scenePath);
					}
				});
			}
		}

		auto sceneWeak = additive ? sm.LoadSceneAdditive(name) : sm.LoadScene(name);
		auto scenePtr = sceneWeak.lock();
		if (!scenePtr) {
			AIM_CORE_ERROR_TAG("ScriptBindings", "Failed to load scene{}: {}", additive ? " additively" : "", name);
			return 0;
		}

		AIM_INFO_TAG("ScriptBindings", "Loaded scene{}: {}", additive ? " additively" : "", name);
		return 1;
	}

	static int Axiom_Scene_LoadAdditive(const char* sceneName) {
		return LoadSceneByName(sceneName, true);
	}

	static int Axiom_Scene_Load(const char* sceneName) {
		return LoadSceneByName(sceneName, false);
	}

	static void Axiom_Scene_Unload(const char* sceneName) {
		SceneManager::Get().UnloadScene(sceneName);
	}

	static int Axiom_Scene_SetActive(const char* sceneName) {
		return SceneManager::Get().SetActiveScene(sceneName) ? 1 : 0;
	}

	static int Axiom_Scene_Reload(const char* sceneName) {
		auto result = SceneManager::Get().ReloadScene(sceneName);
		return result.lock() ? 1 : 0;
	}

	static int Axiom_Scene_SetGameSystemEnabled(const char* sceneName, const char* className, int enabled) {
		if (!sceneName || !className || sceneName[0] == '\0' || className[0] == '\0') return 0;
		auto scene = SceneManager::Get().GetLoadedScene(sceneName).lock();
		return scene && scene->SetGameSystemEnabled(className, enabled != 0) ? 1 : 0;
	}

	static void Axiom_Scene_SetGlobalSystemEnabled(const char* className, int enabled) {
		ScriptEngine::SetGlobalSystemEnabled(className ? className : "", enabled != 0);
	}

	static int Axiom_Scene_DoesSceneExist(const char* sceneName) {
		if (!sceneName || sceneName[0] == '\0') return 0;
		return SceneManager::Get().HasSceneDefinition(sceneName) ? 1 : 0;
	}

	static int Axiom_Scene_GetLoadedCount() {
		return static_cast<int>(SceneManager::Get().GetLoadedSceneCount());
	}

	static const char* Axiom_Scene_GetLoadedSceneNameAt(int index) {
		if (index < 0) {
			s_StringReturnBuffer.clear();
			return s_StringReturnBuffer.c_str();
		}

		const Scene* scene = SceneManager::Get().GetLoadedSceneAt(static_cast<size_t>(index));
		s_StringReturnBuffer = scene ? scene->GetName() : "";
		return s_StringReturnBuffer.c_str();
	}

	static int Axiom_Scene_GetLoadedSceneNameAtBuffer(int index, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_Scene_GetLoadedSceneNameAt(index), outBuffer, capacity);
	}

	// ── Scene Query ─────────────────────────────────────────────────────

	static Scene* ResolveLoadedSceneForQuery(const char* sceneName) {
		if (!sceneName || sceneName[0] == '\0') {
			return GetScene();
		}

		auto scene = SceneManager::Get().GetLoadedScene(sceneName).lock();
		return scene && scene->IsLoaded() ? scene.get() : nullptr;
	}

	static int QueryEntitiesInScene(Scene* scene, const char* componentNames, uint64_t* outEntityIDs, int maxOut) {
		if (!scene || !componentNames || !outEntityIDs || maxOut <= 0) return 0;
		if (!scene->IsLoaded()) return 0;

		// Parse pipe-delimited component names.
		std::vector<QueryComponentRequirement> requirements;
		std::string names(componentNames);
		size_t start = 0;
		while (start < names.size()) {
			size_t end = names.find('|', start);
			if (end == std::string::npos) end = names.size();
			std::string name = names.substr(start, end - start);
			QueryComponentRequirement requirement;
			if (!BuildComponentRequirement(name, requirement)) return 0;
			requirements.push_back(std::move(requirement));
			start = end + 1;
		}
		if (requirements.empty()) return 0;

		int count = 0;
		auto& registry = scene->GetRegistry();
		auto view = registry.view<entt::entity>();
		for (EntityHandle entityHandle : view) {
			if (!registry.valid(entityHandle)) continue;

			Entity entity = scene->GetEntity(entityHandle);
			bool match = true;
			for (const auto& requirement : requirements) {
				if (!requirement.Has(*scene, entityHandle, entity)) { match = false; break; }
			}
			if (match) {
				if (count < maxOut)
					outEntityIDs[count] = GetEntityScriptId(*scene, entityHandle);
				count++;
			}
		}
		return count;
	}

	static int Axiom_Scene_QueryEntities(const char* componentNames, uint64_t* outEntityIDs, int maxOut) {
		AIM_BINDING_TRY {
			return QueryEntitiesInScene(GetScene(), componentNames, outEntityIDs, maxOut);
		} AIM_BINDING_CATCH(0);
	}

	static int Axiom_Scene_QueryEntitiesInScene(const char* sceneName, const char* componentNames, uint64_t* outEntityIDs, int maxOut) {
		AIM_BINDING_TRY {
			return QueryEntitiesInScene(ResolveLoadedSceneForQuery(sceneName), componentNames, outEntityIDs, maxOut);
		} AIM_BINDING_CATCH(0);
	}

	static int Axiom_Asset_IsValid(uint64_t assetId)
	{
		return AssetRegistry::Exists(assetId) ? 1 : 0;
	}

	static uint64_t Axiom_Asset_GetOrCreateUUIDFromPath(const char* path)
	{
		if (!path || path[0] == '\0') {
			return 0;
		}

		return AssetRegistry::GetOrCreateAssetUUID(path);
	}

	static const char* Axiom_Asset_GetPath(uint64_t assetId)
	{
		AIM_BINDING_TRY {
			s_StringReturnBuffer = AssetRegistry::ResolvePath(assetId);
			return s_StringReturnBuffer.c_str();
		} AIM_BINDING_CATCH("");
	}

	static int Axiom_Asset_GetPathBuffer(uint64_t assetId, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_Asset_GetPath(assetId), outBuffer, capacity);
	}

	static const char* Axiom_Asset_GetDisplayName(uint64_t assetId)
	{
		AIM_BINDING_TRY {
			s_StringReturnBuffer = AssetRegistry::GetDisplayName(assetId);
			return s_StringReturnBuffer.c_str();
		} AIM_BINDING_CATCH("");
	}

	static int Axiom_Asset_GetDisplayNameBuffer(uint64_t assetId, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_Asset_GetDisplayName(assetId), outBuffer, capacity);
	}

	static int Axiom_Asset_GetKind(uint64_t assetId)
	{
		return static_cast<int>(AssetRegistry::GetKind(assetId));
	}

	static const char* Axiom_Asset_FindAll(const char* pathPrefix, int kind)
	{
		AIM_BINDING_TRY {
			Json::Value ids = Json::Value::MakeArray();
			for (const AssetRegistry::Record& record : AssetRegistry::FindAll(
					 static_cast<AssetKind>(kind),
					 pathPrefix ? pathPrefix : "")) {
				ids.Append(Json::Value(std::to_string(record.Id)));
			}

			s_StringReturnBuffer = Json::Stringify(ids, false);
			return s_StringReturnBuffer.c_str();
		} AIM_BINDING_CATCH("[]");
	}

	static int Axiom_Asset_FindAllBuffer(const char* pathPrefix, int kind, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_Asset_FindAll(pathPrefix, kind), outBuffer, capacity);
	}

	static int Axiom_Texture_LoadAsset(uint64_t assetId)
	{
		return TextureManager::LoadTextureByUUID(assetId).IsValid() ? 1 : 0;
	}

	static int Axiom_Texture_GetWidth(uint64_t assetId)
	{
		TextureHandle handle = TextureManager::LoadTextureByUUID(assetId);
		Texture2D* texture = TextureManager::GetTexture(handle);
		return texture ? static_cast<int>(texture->GetWidth()) : 0;
	}

	static int Axiom_Texture_GetHeight(uint64_t assetId)
	{
		TextureHandle handle = TextureManager::LoadTextureByUUID(assetId);
		Texture2D* texture = TextureManager::GetTexture(handle);
		return texture ? static_cast<int>(texture->GetHeight()) : 0;
	}

	static int Axiom_Audio_LoadAsset(uint64_t assetId)
	{
		return AudioManager::LoadAudioByUUID(assetId).IsValid() ? 1 : 0;
	}

	static int Axiom_Font_LoadAsset(uint64_t assetId)
	{
		// Validity check only — DO NOT bake an atlas here. Atlas slots are
		// per (uuid, pixelSize) so picking a default size would waste a GL
		// texture every time `Font.IsValid` is called from script while the
		// consumer's actual FontSize differs. The TextRenderer system bakes
		// at TextRendererComponent::FontSize on first draw.
		return assetId != 0 && AssetRegistry::IsFont(assetId) ? 1 : 0;
	}

	static void Axiom_Audio_PlayOneShotAsset(uint64_t assetId, float volume)
	{
		AudioHandle handle = AudioManager::LoadAudioByUUID(assetId);
		if (handle.IsValid()) {
			AudioManager::PlayOneShot(handle, volume);
		}
	}

	// Helper: parse pipe-delimited names into native or managed component requirements.
	static bool ParseComponentNames(const char* names, std::vector<QueryComponentRequirement>& out) {
		if (!names || names[0] == '\0') return true; // empty is valid (no filter)
		std::string str(names);
		size_t start = 0;
		while (start < str.size()) {
			size_t end = str.find('|', start);
			if (end == std::string::npos) end = str.size();
			std::string name = str.substr(start, end - start);
			if (!name.empty()) {
				QueryComponentRequirement requirement;
				if (!BuildComponentRequirement(name, requirement)) return false;
				out.push_back(std::move(requirement));
			}
			start = end + 1;
		}
		return true;
	}

	static int QueryEntitiesFilteredInScene(
		Scene* scene,
		const char* withComponents,
		const char* withoutComponents,
		const char* mustHaveComponents,
		int enableFilter,
		uint64_t* outEntityIDs, int maxOut)
	{
		if (!scene || !outEntityIDs || maxOut <= 0) return 0;
		if (!scene->IsLoaded()) return 0;

		std::vector<QueryComponentRequirement> withInfos, withoutInfos, mustHaveInfos;
		if (!ParseComponentNames(withComponents, withInfos)) return 0;
		if (!ParseComponentNames(withoutComponents, withoutInfos)) return 0;
		if (!ParseComponentNames(mustHaveComponents, mustHaveInfos)) return 0;
		if (withInfos.empty() && mustHaveInfos.empty()) return 0;

		int count = 0;
		auto& registry = scene->GetRegistry();
		auto view = registry.view<entt::entity>();

		for (auto entityHandle : view) {
			if (!registry.valid(entityHandle)) continue;

			Entity entity = scene->GetEntity(entityHandle);

			// Enable filter: 1 = enabled only, 2 = disabled only
			if (enableFilter == 1 && registry.all_of<DisabledTag>(entityHandle)) continue;
			if (enableFilter == 2 && !registry.all_of<DisabledTag>(entityHandle)) continue;

			// WITH: entity must have all these components
			bool match = true;
			for (const auto& requirement : withInfos) {
				if (!requirement.Has(*scene, entityHandle, entity)) { match = false; break; }
			}
			if (!match) continue;

			// MUST HAVE (With<>): entity must have these too
			for (const auto& requirement : mustHaveInfos) {
				if (!requirement.Has(*scene, entityHandle, entity)) { match = false; break; }
			}
			if (!match) continue;

			// WITHOUT: entity must NOT have any of these
			for (const auto& requirement : withoutInfos) {
				if (requirement.Has(*scene, entityHandle, entity)) { match = false; break; }
			}
			if (!match) continue;

			if (count < maxOut)
				outEntityIDs[count] = GetEntityScriptId(*scene, entityHandle);
			count++;
		}
		return count;
	}

	static int Axiom_Scene_QueryEntitiesFiltered(
		const char* withComponents,
		const char* withoutComponents,
		const char* mustHaveComponents,
		int enableFilter,
		uint64_t* outEntityIDs, int maxOut)
	{
		return QueryEntitiesFilteredInScene(GetScene(), withComponents, withoutComponents, mustHaveComponents, enableFilter, outEntityIDs, maxOut);
	}

	static int Axiom_Scene_QueryEntitiesFilteredInScene(
		const char* sceneName,
		const char* withComponents,
		const char* withoutComponents,
		const char* mustHaveComponents,
		int enableFilter,
		uint64_t* outEntityIDs, int maxOut)
	{
		return QueryEntitiesFilteredInScene(ResolveLoadedSceneForQuery(sceneName), withComponents, withoutComponents, mustHaveComponents, enableFilter, outEntityIDs, maxOut);
	}

	// ── NameComponent ───────────────────────────────────────────────────

	static const char* Axiom_NameComponent_GetName(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle) || !scene->HasComponent<NameComponent>(handle)) {
			s_StringReturnBuffer.clear();
			return s_StringReturnBuffer.c_str();
		}

		s_StringReturnBuffer = scene->GetComponent<NameComponent>(handle).Name;
		return s_StringReturnBuffer.c_str();
	}

	static int Axiom_NameComponent_GetNameBuffer(uint64_t entityID, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_NameComponent_GetName(entityID), outBuffer, capacity);
	}

	static void Axiom_NameComponent_SetName(uint64_t entityID, const char* name)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle) || !scene->HasComponent<NameComponent>(handle)) return;
		scene->GetComponent<NameComponent>(handle).Name = name;
	}

	// ── Transform2D ─────────────────────────────────────────────────────

	static void Axiom_Transform2D_GetPosition(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(Transform2DComponent, entityID, (void)(*outX = 0, *outY = 0));
		*outX = comp.Position.x; *outY = comp.Position.y;
	}

	static void Axiom_Transform2D_SetPosition(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(Transform2DComponent, entityID, );
		comp.SetPosition({ x, y });
	}

	static float Axiom_Transform2D_GetRotation(uint64_t entityID)
	{
		GET_COMPONENT(Transform2DComponent, entityID, 0.0f);
		return comp.Rotation;
	}

	static void Axiom_Transform2D_SetRotation(uint64_t entityID, float rotation)
	{
		GET_COMPONENT(Transform2DComponent, entityID, );
		comp.SetRotation(rotation);
	}

	static void Axiom_Transform2D_GetScale(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(Transform2DComponent, entityID, (void)(*outX = 1, *outY = 1));
		*outX = comp.Scale.x; *outY = comp.Scale.y;
	}

	static void Axiom_Transform2D_SetScale(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(Transform2DComponent, entityID, );
		comp.SetScale({ x, y });
	}

	static uint64_t Axiom_Transform2D_GetEntity(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;
		if (!scene->HasComponent<Transform2DComponent>(handle)) return 0;
		return GetEntityScriptId(*scene, handle);
	}

	static void Axiom_Transform2D_GetLocalPosition(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(Transform2DComponent, entityID, (void)(*outX = 0, *outY = 0));
		*outX = comp.LocalPosition.x; *outY = comp.LocalPosition.y;
	}

	static void Axiom_Transform2D_SetLocalPosition(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(Transform2DComponent, entityID, );
		comp.SetPosition({ x, y });
	}

	static float Axiom_Transform2D_GetLocalRotation(uint64_t entityID)
	{
		GET_COMPONENT(Transform2DComponent, entityID, 0.0f);
		return comp.LocalRotation;
	}

	static void Axiom_Transform2D_SetLocalRotation(uint64_t entityID, float rotation)
	{
		GET_COMPONENT(Transform2DComponent, entityID, );
		comp.SetRotation(rotation);
	}

	static void Axiom_Transform2D_GetLocalScale(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(Transform2DComponent, entityID, (void)(*outX = 1, *outY = 1));
		*outX = comp.LocalScale.x; *outY = comp.LocalScale.y;
	}

	static void Axiom_Transform2D_SetLocalScale(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(Transform2DComponent, entityID, );
		comp.SetScale({ x, y });
	}

	static uint64_t Axiom_Transform2D_GetParent(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;

		Entity parent = scene->GetEntity(handle).GetParent();
		if (!parent.IsValid()) return 0;
		return GetEntityScriptId(*scene, parent.GetHandle());
	}

	static int Axiom_Transform2D_SetParent(uint64_t entityID, uint64_t parentEntityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;

		Entity entity = scene->GetEntity(handle);

		if (parentEntityID == 0) {
			entity.SetParent(Entity::Null);
			return 1;
		}

		Scene* parentScene = nullptr;
		EntityHandle parentHandle = entt::null;
		if (!ResolveEntityReference(parentEntityID, parentScene, parentHandle)) return 0;

		// Cross-scene parenting would silently corrupt the parent's HierarchyComponent
		// (the child's handle wouldn't resolve in the parent's registry), so refuse it.
		if (parentScene != scene) {
			AIM_CORE_WARN_TAG("ScriptBinding", "Transform2D.SetParent across scenes is not supported");
			return 0;
		}

		entity.SetParent(scene->GetEntity(parentHandle));
		return 1;
	}

	static int Axiom_Transform2D_GetChildCount(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;
		return static_cast<int>(scene->GetEntity(handle).GetChildren().size());
	}

	static uint64_t Axiom_Transform2D_GetChildAt(uint64_t entityID, int index)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;

		const auto& children = scene->GetEntity(handle).GetChildren();
		if (index < 0 || static_cast<size_t>(index) >= children.size()) return 0;
		return GetEntityScriptId(*scene, children[static_cast<size_t>(index)]);
	}

	static int Axiom_Transform2D_GetChildren(uint64_t entityID, uint64_t* outIDs, int maxOut)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return 0;

		const auto& children = scene->GetEntity(handle).GetChildren();
		const int total = static_cast<int>(children.size());
		if (!outIDs || maxOut <= 0) return total;

		const int writeCount = std::min(total, maxOut);
		for (int i = 0; i < writeCount; ++i) {
			outIDs[i] = GetEntityScriptId(*scene, children[static_cast<size_t>(i)]);
		}
		return total;
	}

	// ── SpriteRenderer ──────────────────────────────────────────────────

	static void Axiom_SpriteRenderer_GetColor(uint64_t entityID, float* r, float* g, float* b, float* a)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, (void)(*r = 1, *g = 1, *b = 1, *a = 1));
		*r = comp.Color.r; *g = comp.Color.g; *b = comp.Color.b; *a = comp.Color.a;
	}

	static void Axiom_SpriteRenderer_SetColor(uint64_t entityID, float r, float g, float b, float a)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, );
		comp.Color = { r, g, b, a };
	}

	static uint64_t Axiom_SpriteRenderer_GetTexture(uint64_t entityID)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, 0);

		uint64_t assetId = static_cast<uint64_t>(comp.TextureAssetId);
		if (assetId == 0 && TextureManager::IsValid(comp.TextureHandle)) {
			assetId = TextureManager::GetTextureAssetUUID(comp.TextureHandle);
			if (assetId != 0) {
				comp.TextureAssetId = UUID(assetId);
			}
		}

		return assetId;
	}

	static void Axiom_SpriteRenderer_SetTexture(uint64_t entityID, uint64_t assetId)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, );

		if (assetId == 0) {
			comp.TextureHandle = TextureHandle::Invalid();
			comp.TextureAssetId = UUID(0);
			return;
		}

		comp.TextureAssetId = UUID(assetId);
		comp.TextureHandle = TextureManager::LoadTextureByUUID(assetId);
	}

	static int Axiom_SpriteRenderer_GetSortingOrder(uint64_t entityID)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, 0);
		return comp.SortingOrder;
	}

	static void Axiom_SpriteRenderer_SetSortingOrder(uint64_t entityID, int order)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, );
		comp.SortingOrder = static_cast<short>(order);
	}

	static int Axiom_SpriteRenderer_GetSortingLayer(uint64_t entityID)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, 0);
		return comp.SortingLayer;
	}

	static void Axiom_SpriteRenderer_SetSortingLayer(uint64_t entityID, int layer)
	{
		GET_COMPONENT(SpriteRendererComponent, entityID, );
		comp.SortingLayer = static_cast<uint8_t>(layer);
	}

	// ── TextRenderer ────────────────────────────────────────────────────

	static const char* Axiom_TextRenderer_GetText(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle) || !scene->HasComponent<TextRendererComponent>(handle)) {
			s_StringReturnBuffer.clear();
			return s_StringReturnBuffer.c_str();
		}
		s_StringReturnBuffer = scene->GetComponent<TextRendererComponent>(handle).Text;
		return s_StringReturnBuffer.c_str();
	}

	static int Axiom_TextRenderer_GetTextBuffer(uint64_t entityID, char* outBuffer, int capacity)
	{
		return CopyCStringToBuffer(Axiom_TextRenderer_GetText(entityID), outBuffer, capacity);
	}

	static void Axiom_TextRenderer_SetText(uint64_t entityID, const char* text)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );
		comp.Text = text ? text : "";
	}

	static uint64_t Axiom_TextRenderer_GetFont(uint64_t entityID)
	{
		GET_COMPONENT(TextRendererComponent, entityID, 0);

		uint64_t assetId = static_cast<uint64_t>(comp.FontAssetId);
		if (assetId == 0 && FontManager::IsValid(comp.ResolvedFont)) {
			assetId = FontManager::GetFontAssetUUID(comp.ResolvedFont);
			if (assetId != 0) {
				comp.FontAssetId = UUID(assetId);
			}
		}
		return assetId;
	}

	static void Axiom_TextRenderer_SetFont(uint64_t entityID, uint64_t assetId)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );

		comp.FontAssetId = UUID(assetId);
		// Force the renderer to re-resolve next frame at the current FontSize.
		comp.ResolvedFont = FontHandle{};
	}

	static float Axiom_TextRenderer_GetFontSize(uint64_t entityID)
	{
		GET_COMPONENT(TextRendererComponent, entityID, 32.0f);
		return comp.FontSize;
	}

	static void Axiom_TextRenderer_SetFontSize(uint64_t entityID, float size)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );
		comp.FontSize = size;
		// A new pixel size needs a different atlas slot — invalidate the cache.
		comp.ResolvedFont = FontHandle{};
	}

	static void Axiom_TextRenderer_GetColor(uint64_t entityID, float* r, float* g, float* b, float* a)
	{
		GET_COMPONENT(TextRendererComponent, entityID, (void)(*r = 1, *g = 1, *b = 1, *a = 1));
		*r = comp.Color.r; *g = comp.Color.g; *b = comp.Color.b; *a = comp.Color.a;
	}

	static void Axiom_TextRenderer_SetColor(uint64_t entityID, float r, float g, float b, float a)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );
		comp.Color = { r, g, b, a };
	}

	static float Axiom_TextRenderer_GetLetterSpacing(uint64_t entityID)
	{
		GET_COMPONENT(TextRendererComponent, entityID, 0.0f);
		return comp.LetterSpacing;
	}

	static void Axiom_TextRenderer_SetLetterSpacing(uint64_t entityID, float spacing)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );
		comp.LetterSpacing = spacing;
	}

	static int Axiom_TextRenderer_GetHAlign(uint64_t entityID)
	{
		GET_COMPONENT(TextRendererComponent, entityID, 0);
		return static_cast<int>(comp.HAlign);
	}

	static void Axiom_TextRenderer_SetHAlign(uint64_t entityID, int align)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );
		comp.HAlign = static_cast<TextAlignment>(align);
	}

	static int Axiom_TextRenderer_GetSortingOrder(uint64_t entityID)
	{
		GET_COMPONENT(TextRendererComponent, entityID, 0);
		return comp.SortingOrder;
	}

	static void Axiom_TextRenderer_SetSortingOrder(uint64_t entityID, int order)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );
		comp.SortingOrder = static_cast<int16_t>(order);
	}

	static int Axiom_TextRenderer_GetSortingLayer(uint64_t entityID)
	{
		GET_COMPONENT(TextRendererComponent, entityID, 0);
		return comp.SortingLayer;
	}

	static void Axiom_TextRenderer_SetSortingLayer(uint64_t entityID, int layer)
	{
		GET_COMPONENT(TextRendererComponent, entityID, );
		comp.SortingLayer = static_cast<uint8_t>(layer);
	}

	// ── Camera2D ────────────────────────────────────────────────────────

	static float Axiom_Camera2D_GetOrthographicSize(uint64_t entityID)
	{
		GET_COMPONENT(Camera2DComponent, entityID, 5.0f);
		return comp.GetOrthographicSize();
	}

	static void Axiom_Camera2D_SetOrthographicSize(uint64_t entityID, float size)
	{
		GET_COMPONENT(Camera2DComponent, entityID, );
		comp.SetOrthographicSize(size);
	}

	static float Axiom_Camera2D_GetZoom(uint64_t entityID)
	{
		GET_COMPONENT(Camera2DComponent, entityID, 1.0f);
		return comp.GetZoom();
	}

	static void Axiom_Camera2D_SetZoom(uint64_t entityID, float zoom)
	{
		GET_COMPONENT(Camera2DComponent, entityID, );
		comp.SetZoom(zoom);
	}

	static void Axiom_Camera2D_GetClearColor(uint64_t entityID, float* r, float* g, float* b, float* a)
	{
		GET_COMPONENT(Camera2DComponent, entityID, (void)(*r = 0.1f, *g = 0.1f, *b = 0.1f, *a = 1.0f));
		const auto& cc = comp.GetClearColor();
		*r = cc.r; *g = cc.g; *b = cc.b; *a = cc.a;
	}

	static void Axiom_Camera2D_SetClearColor(uint64_t entityID, float r, float g, float b, float a)
	{
		GET_COMPONENT(Camera2DComponent, entityID, );
		comp.SetClearColor(Color(r, g, b, a));
	}

	static void Axiom_Camera2D_ScreenToWorld(uint64_t entityID, float sx, float sy, float* outX, float* outY)
	{
		GET_COMPONENT(Camera2DComponent, entityID, (void)(*outX = 0, *outY = 0));
		Vec2 world = comp.ScreenToWorld({ sx, sy });
		*outX = world.x; *outY = world.y;
	}

	static float Axiom_Camera2D_GetViewportWidth(uint64_t entityID)
	{
		GET_COMPONENT(Camera2DComponent, entityID, 0.0f);
		return comp.ViewportWidth();
	}

	static float Axiom_Camera2D_GetViewportHeight(uint64_t entityID)
	{
		GET_COMPONENT(Camera2DComponent, entityID, 0.0f);
		return comp.ViewportHeight();
	}

	static uint64_t Axiom_Camera2D_GetMainEntity()
	{
		Scene* scene = GetScene();
		if (!scene) return 0;

		const EntityHandle handle = scene->GetMainCameraEntity();
		if (handle == entt::null) return 0;
		return GetEntityScriptId(*scene, handle);
	}

	// ── Rigidbody2D ─────────────────────────────────────────────────────

	static void Axiom_Rigidbody2D_ApplyForce(uint64_t entityID, float forceX, float forceY, int wake)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, );
		b2BodyId bodyId = comp.GetBodyHandle();
		if (b2Body_IsValid(bodyId)) b2Body_ApplyForceToCenter(bodyId, { forceX, forceY }, wake != 0);
	}

	static void Axiom_Rigidbody2D_ApplyImpulse(uint64_t entityID, float impulseX, float impulseY, int wake)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, );
		b2BodyId bodyId = comp.GetBodyHandle();
		if (b2Body_IsValid(bodyId)) b2Body_ApplyLinearImpulseToCenter(bodyId, { impulseX, impulseY }, wake != 0);
	}

	static void Axiom_Rigidbody2D_GetLinearVelocity(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, (void)(*outX = 0, *outY = 0));
		Vec2 vel = comp.GetVelocity(); *outX = vel.x; *outY = vel.y;
	}

	static void Axiom_Rigidbody2D_SetLinearVelocity(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, );
		comp.SetVelocity({ x, y });
	}

	static float Axiom_Rigidbody2D_GetAngularVelocity(uint64_t entityID)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, 0.0f);
		return comp.GetAngularVelocity();
	}

	static void Axiom_Rigidbody2D_SetAngularVelocity(uint64_t entityID, float velocity)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, );
		comp.SetAngularVelocity(velocity);
	}

	static int Axiom_Rigidbody2D_GetBodyType(uint64_t entityID)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, 0);
		return static_cast<int>(comp.GetBodyType());
	}

	static void Axiom_Rigidbody2D_SetBodyType(uint64_t entityID, int type)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, );
		comp.SetBodyType(static_cast<BodyType>(type));
	}

	static float Axiom_Rigidbody2D_GetGravityScale(uint64_t entityID)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, 1.0f);
		return comp.GetGravityScale();
	}

	static void Axiom_Rigidbody2D_SetGravityScale(uint64_t entityID, float scale)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, );
		comp.SetGravityScale(scale);
	}

	static float Axiom_Rigidbody2D_GetMass(uint64_t entityID)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, 1.0f);
		return comp.GetMass();
	}

	static void Axiom_Rigidbody2D_SetMass(uint64_t entityID, float mass)
	{
		GET_COMPONENT(Rigidbody2DComponent, entityID, );
		comp.SetMass(mass);
	}

	// ── BoxCollider2D ───────────────────────────────────────────────────

	static void Axiom_BoxCollider2D_GetScale(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(BoxCollider2DComponent, entityID, (void)(*outX = 1, *outY = 1));
		Vec2 s = comp.GetScale(); *outX = s.x; *outY = s.y;
	}

	static void Axiom_BoxCollider2D_GetCenter(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(BoxCollider2DComponent, entityID, (void)(*outX = 0, *outY = 0));
		Vec2 c = comp.GetCenter(); *outX = c.x; *outY = c.y;
	}

	static void Axiom_BoxCollider2D_SetEnabled(uint64_t entityID, int enabled)
	{
		GET_COMPONENT(BoxCollider2DComponent, entityID, );
		comp.SetEnabled(enabled != 0);
	}

	// ── CircleCollider2D ────────────────────────────────────────────────

	static float Axiom_CircleCollider2D_GetRadius(uint64_t entityID)
	{
		GET_COMPONENT(CircleCollider2DComponent, entityID, 0.0f);
		return comp.GetLocalRadius(*scene);
	}

	static void Axiom_CircleCollider2D_SetRadius(uint64_t entityID, float radius)
	{
		GET_COMPONENT(CircleCollider2DComponent, entityID, );
		comp.SetRadius(radius, *scene);
	}

	static void Axiom_CircleCollider2D_GetCenter(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(CircleCollider2DComponent, entityID, (void)(*outX = 0, *outY = 0));
		Vec2 c = comp.GetCenter(); *outX = c.x; *outY = c.y;
	}

	static void Axiom_CircleCollider2D_SetCenter(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(CircleCollider2DComponent, entityID, );
		comp.SetCenter(Vec2{ x, y }, *scene);
	}

	static void Axiom_CircleCollider2D_SetEnabled(uint64_t entityID, int enabled)
	{
		GET_COMPONENT(CircleCollider2DComponent, entityID, );
		comp.SetEnabled(enabled != 0);
	}

	// ── PolygonCollider2D ───────────────────────────────────────────────

	static int Axiom_PolygonCollider2D_GetVertexCount(uint64_t entityID)
	{
		GET_COMPONENT(PolygonCollider2DComponent, entityID, 0);
		return comp.GetVertexCount();
	}

	static int Axiom_PolygonCollider2D_GetWorldPoints(uint64_t entityID, float* outPoints, int maxOut)
	{
		if (!outPoints || maxOut <= 0) return 0;
		GET_COMPONENT(PolygonCollider2DComponent, entityID, 0);

		std::vector<Vec2> world = comp.GetWorldPoints();
		const int writable = std::min(static_cast<int>(world.size()) * 2, maxOut);
		// Pair pack: index i -> (outPoints[2i], outPoints[2i+1]) so the C# side
		// can read it as a Span<Vector2> without an extra reshape pass.
		for (int i = 0; i < writable / 2; ++i) {
			outPoints[i * 2 + 0] = world[i].x;
			outPoints[i * 2 + 1] = world[i].y;
		}
		return writable / 2;
	}

	static void Axiom_PolygonCollider2D_SetPoints(uint64_t entityID, const float* points, int pointCount)
	{
		if (!points) return;
		GET_COMPONENT(PolygonCollider2DComponent, entityID, );
		if (pointCount < PolygonCollider2DComponent::k_MinVertices ||
			pointCount > PolygonCollider2DComponent::k_MaxVertices) {
			return;
		}

		std::vector<Vec2> localPoints;
		localPoints.reserve(static_cast<size_t>(pointCount));
		for (int i = 0; i < pointCount; ++i) {
			localPoints.push_back(Vec2{ points[i * 2], points[i * 2 + 1] });
		}
		comp.SetPoints(localPoints, *scene);
	}

	static void Axiom_PolygonCollider2D_SetSides(uint64_t entityID, int sides)
	{
		GET_COMPONENT(PolygonCollider2DComponent, entityID, );
		comp.SetSides(sides, *scene);
	}

	static void Axiom_PolygonCollider2D_GetCenter(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(PolygonCollider2DComponent, entityID, (void)(*outX = 0, *outY = 0));
		Vec2 c = comp.GetCenter(); *outX = c.x; *outY = c.y;
	}

	static void Axiom_PolygonCollider2D_SetCenter(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(PolygonCollider2DComponent, entityID, );
		comp.SetCenter(Vec2{ x, y }, *scene);
	}

	static void Axiom_PolygonCollider2D_GetSize(uint64_t entityID, float* outX, float* outY)
	{
		GET_COMPONENT(PolygonCollider2DComponent, entityID, (void)(*outX = 1, *outY = 1));
		Vec2 s = comp.GetSize(); *outX = s.x; *outY = s.y;
	}

	static void Axiom_PolygonCollider2D_SetSize(uint64_t entityID, float x, float y)
	{
		GET_COMPONENT(PolygonCollider2DComponent, entityID, );
		comp.SetSize(Vec2{ x, y }, *scene);
	}

	static void Axiom_PolygonCollider2D_SetEnabled(uint64_t entityID, int enabled)
	{
		GET_COMPONENT(PolygonCollider2DComponent, entityID, );
		comp.SetEnabled(enabled != 0);
	}

	// ── AudioSource ─────────────────────────────────────────────────────

	static void Axiom_AudioSource_Play(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, ); AudioManager::PlayAudioSource(comp); }
	static void Axiom_AudioSource_Pause(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, ); AudioManager::PauseAudioSource(comp); }
	static void Axiom_AudioSource_Stop(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, ); AudioManager::StopAudioSource(comp); }
	static void Axiom_AudioSource_Resume(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, ); AudioManager::ResumeAudioSource(comp); }

	static float Axiom_AudioSource_GetVolume(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, 1.0f); return comp.GetVolume(); }
	static void  Axiom_AudioSource_SetVolume(uint64_t entityID, float volume) { GET_COMPONENT(AudioSourceComponent, entityID, ); comp.SetVolume(volume); }
	static float Axiom_AudioSource_GetPitch(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, 1.0f); return comp.GetPitch(); }
	static void  Axiom_AudioSource_SetPitch(uint64_t entityID, float pitch) { GET_COMPONENT(AudioSourceComponent, entityID, ); comp.SetPitch(pitch); }
	static int   Axiom_AudioSource_GetLoop(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, 0); return comp.IsLooping() ? 1 : 0; }
	static void  Axiom_AudioSource_SetLoop(uint64_t entityID, int loop) { GET_COMPONENT(AudioSourceComponent, entityID, ); comp.SetLoop(loop != 0); }
	static int   Axiom_AudioSource_IsPlaying(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, 0); return comp.IsPlaying() ? 1 : 0; }
	static int   Axiom_AudioSource_IsPaused(uint64_t entityID) { GET_COMPONENT(AudioSourceComponent, entityID, 0); return comp.IsPaused() ? 1 : 0; }

	static uint64_t Axiom_AudioSource_GetAudio(uint64_t entityID)
	{
		GET_COMPONENT(AudioSourceComponent, entityID, 0);

		// Cache UUID resolved from a loaded handle so subsequent calls are O(1).
		uint64_t assetId = static_cast<uint64_t>(comp.GetAudioAssetId());
		if (assetId == 0 && comp.GetAudioHandle().IsValid()) {
			assetId = AudioManager::GetAudioAssetUUID(comp.GetAudioHandle());
			if (assetId != 0) {
				comp.SetAudioAssetId(UUID(assetId));
			}
		}
		return assetId;
	}

	static void Axiom_AudioSource_SetAudio(uint64_t entityID, uint64_t assetId)
	{
		GET_COMPONENT(AudioSourceComponent, entityID, );

		if (assetId == 0) {
			comp.SetAudioHandle(AudioHandle(), UUID(0));
			return;
		}

		comp.SetAudioHandle(AudioManager::LoadAudioByUUID(assetId), UUID(assetId));
	}

	// ── Axiom-Physics ────────────────────────────────────────────────────

	static int Axiom_FastBody2D_GetBodyType(uint64_t entityID) { GET_COMPONENT(FastBody2DComponent, entityID, 1); return static_cast<int>(comp.Type); }
	static void Axiom_FastBody2D_SetBodyType(uint64_t entityID, int type) { GET_COMPONENT(FastBody2DComponent, entityID, ); comp.Type = static_cast<AxiomPhys::BodyType>(type); if (comp.m_Body) comp.m_Body->SetBodyType(comp.Type); }
	static float Axiom_FastBody2D_GetMass(uint64_t entityID) { GET_COMPONENT(FastBody2DComponent, entityID, 1.0f); return comp.Mass; }
	static void Axiom_FastBody2D_SetMass(uint64_t entityID, float mass) { GET_COMPONENT(FastBody2DComponent, entityID, ); comp.Mass = mass; if (comp.m_Body) comp.m_Body->SetMass(mass); }
	static int Axiom_FastBody2D_GetUseGravity(uint64_t entityID) { GET_COMPONENT(FastBody2DComponent, entityID, 1); return comp.UseGravity ? 1 : 0; }
	static void Axiom_FastBody2D_SetUseGravity(uint64_t entityID, int enabled) { GET_COMPONENT(FastBody2DComponent, entityID, ); comp.UseGravity = enabled != 0; if (comp.m_Body) comp.m_Body->SetGravityEnabled(comp.UseGravity); }
	static void Axiom_FastBody2D_GetVelocity(uint64_t entityID, float* outX, float* outY) { GET_COMPONENT(FastBody2DComponent, entityID, (void)(*outX = 0, *outY = 0)); Vec2 v = comp.GetVelocity(); *outX = v.x; *outY = v.y; }
	static void Axiom_FastBody2D_SetVelocity(uint64_t entityID, float x, float y) { GET_COMPONENT(FastBody2DComponent, entityID, ); comp.SetVelocity({ x, y }); }

	static void Axiom_FastBoxCollider2D_GetHalfExtents(uint64_t entityID, float* outX, float* outY) { GET_COMPONENT(FastBoxCollider2DComponent, entityID, (void)(*outX = 0.5f, *outY = 0.5f)); *outX = comp.HalfExtents.x; *outY = comp.HalfExtents.y; }
	static void Axiom_FastBoxCollider2D_SetHalfExtents(uint64_t entityID, float x, float y) { GET_COMPONENT(FastBoxCollider2DComponent, entityID, ); comp.SetHalfExtents({ x, y }); }

	static float Axiom_FastCircleCollider2D_GetRadius(uint64_t entityID) { GET_COMPONENT(FastCircleCollider2DComponent, entityID, 0.5f); return comp.Radius; }
	static void Axiom_FastCircleCollider2D_SetRadius(uint64_t entityID, float radius) { GET_COMPONENT(FastCircleCollider2DComponent, entityID, ); comp.SetRadius(radius); }

	// ── ParticleSystem2D ────────────────────────────────────────────────

	static void Axiom_ParticleSystem2D_Play(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.Play();
	}
	static void Axiom_ParticleSystem2D_Pause(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.Pause();
	}
	static void Axiom_ParticleSystem2D_Stop(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.Stop();
	}
	static int Axiom_ParticleSystem2D_IsPlaying(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, 0);
		return comp.IsPlaying() ? 1 : 0;
	}
	static int Axiom_ParticleSystem2D_GetPlayOnAwake(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, 0);
		return comp.PlayOnAwake ? 1 : 0;
	}
	static void Axiom_ParticleSystem2D_SetPlayOnAwake(uint64_t entityID, int enabled) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.PlayOnAwake = (enabled != 0);
	}
	static void Axiom_ParticleSystem2D_GetColor(uint64_t entityID, float* r, float* g, float* b, float* a) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		*r = comp.RenderingSettings.Color.r; *g = comp.RenderingSettings.Color.g;
		*b = comp.RenderingSettings.Color.b; *a = comp.RenderingSettings.Color.a;
	}
	static void Axiom_ParticleSystem2D_SetColor(uint64_t entityID, float r, float g, float b, float a) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.RenderingSettings.Color = { r, g, b, a };
	}
	static float Axiom_ParticleSystem2D_GetLifeTime(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, 0.0f);
		return comp.ParticleSettings.LifeTime;
	}
	static void Axiom_ParticleSystem2D_SetLifeTime(uint64_t entityID, float lifetime) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.ParticleSettings.LifeTime = lifetime;
	}
	static float Axiom_ParticleSystem2D_GetSpeed(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, 0.0f);
		return comp.ParticleSettings.Speed;
	}
	static void Axiom_ParticleSystem2D_SetSpeed(uint64_t entityID, float speed) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.ParticleSettings.Speed = speed;
	}
	static float Axiom_ParticleSystem2D_GetScale(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, 0.0f);
		return comp.ParticleSettings.Scale;
	}
	static void Axiom_ParticleSystem2D_SetScale(uint64_t entityID, float scale) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.ParticleSettings.Scale = scale;
	}
	static int Axiom_ParticleSystem2D_GetEmitOverTime(uint64_t entityID) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, 0);
		return comp.EmissionSettings.EmitOverTime;
	}
	static void Axiom_ParticleSystem2D_SetEmitOverTime(uint64_t entityID, int rate) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.EmissionSettings.EmitOverTime = static_cast<uint16_t>(rate);
	}
	static void Axiom_ParticleSystem2D_Emit(uint64_t entityID, int count) {
		GET_COMPONENT(ParticleSystem2DComponent, entityID, );
		comp.Emit(static_cast<size_t>(count));
	}

	// ── Gizmos ──────────────────────────────────────────────────────────

	static void Axiom_Gizmo_DrawLine(float x1, float y1, float x2, float y2) { Gizmo::DrawLine({ x1, y1 }, { x2, y2 }); }
	static void Axiom_Gizmo_DrawSquare(float cx, float cy, float sx, float sy, float degrees) { Gizmo::DrawSquare({ cx, cy }, { sx, sy }, degrees); }
	static void Axiom_Gizmo_DrawCircle(float cx, float cy, float radius, int segments) { Gizmo::DrawCircle({ cx, cy }, radius, segments); }
	static void Axiom_Gizmo_SetColor(float r, float g, float b, float a) { Gizmo::SetColor(Color(r, g, b, a)); }
	static void Axiom_Gizmo_GetColor(float* r, float* g, float* b, float* a) { Color c = Gizmo::GetColor(); *r = c.r; *g = c.g; *b = c.b; *a = c.a; }
	static float Axiom_Gizmo_GetLineWidth() { return Gizmo::GetLineWidth(); }
	static void Axiom_Gizmo_SetLineWidth(float width) { Gizmo::SetLineWidth(width); }

	// ── Physics2D ───────────────────────────────────────────────────────

	static int Axiom_Physics2D_Raycast(
		float originX, float originY, float dirX, float dirY, float distance,
		uint64_t* hitEntityID, float* hitX, float* hitY, float* hitNormalX, float* hitNormalY, float* hitDistance)
	{
		if (!hitEntityID || !hitX || !hitY || !hitNormalX || !hitNormalY || !hitDistance) return 0;
		*hitEntityID = 0; *hitX = 0; *hitY = 0; *hitNormalX = 0; *hitNormalY = 0; *hitDistance = 0;

		auto result = Physics2D::Raycast({ originX, originY }, { dirX, dirY }, distance);
		if (result.has_value()) {
			if (result->scene && result->scene->IsValid(result->entity)) {
				*hitEntityID = GetEntityScriptId(*result->scene, result->entity);
			}
			*hitX = result->point.x; *hitY = result->point.y;
			*hitNormalX = result->normal.x; *hitNormalY = result->normal.y;
			*hitDistance = result->distance;
			return 1;
		}
		return 0;
	}

	static uint64_t ToScriptEntityId(const PhysicsBodyRef2D& ref) {
		if (!ref.scene || ref.entity == entt::null || !ref.scene->IsValid(ref.entity)) {
			return 0;
		}
		return GetEntityScriptId(*ref.scene, ref.entity);
	}

	static OverlapMode ToOverlapMode(int mode) {
		return mode == 1 ? OverlapMode::Nearest : OverlapMode::First;
	}

	static std::vector<Vec2> ReadPolygonPoints(const float* points, int pointCount) {
		std::vector<Vec2> result;
		constexpr int maxPolygonVertices = 8;
		if (!points || pointCount < 3 || pointCount > maxPolygonVertices) {
			return result;
		}

		result.reserve(static_cast<size_t>(pointCount));
		for (int i = 0; i < pointCount; ++i) {
			result.push_back({ points[i * 2], points[i * 2 + 1] });
		}
		return result;
	}

	static int WriteOverlapResult(std::optional<PhysicsBodyRef2D> result, uint64_t* entityID) {
		if (!entityID) return 0;
		*entityID = result.has_value() ? ToScriptEntityId(*result) : 0;
		return *entityID != 0 ? 1 : 0;
	}

	static int WriteOverlapResults(const std::vector<PhysicsBodyRef2D>& refs, uint64_t* outEntityIDs, int maxOut) {
		if (!outEntityIDs || maxOut <= 0) return 0;

		int count = 0;
		for (const PhysicsBodyRef2D& ref : refs) {
			uint64_t id = ToScriptEntityId(ref);
			if (id == 0) continue;

			if (count < maxOut) {
				outEntityIDs[count] = id;
			}
			++count;
		}
		return count;
	}

	static int Axiom_Physics2D_OverlapCircle(float originX, float originY, float radius, int mode, uint64_t* entityID) {
		return WriteOverlapResult(
			Physics2D::OverlapCircleRef({ originX, originY }, radius, ToOverlapMode(mode)),
			entityID);
	}

	static int Axiom_Physics2D_OverlapBox(float originX, float originY, float halfX, float halfY, float degrees, int mode, uint64_t* entityID) {
		return WriteOverlapResult(
			Physics2D::OverlapBoxRef({ originX, originY }, { halfX, halfY }, degrees, ToOverlapMode(mode)),
			entityID);
	}

	static int Axiom_Physics2D_OverlapPolygon(float originX, float originY, const float* points, int pointCount, int mode, uint64_t* entityID) {
		std::vector<Vec2> polygon = ReadPolygonPoints(points, pointCount);
		if (polygon.empty()) {
			if (entityID) *entityID = 0;
			return 0;
		}

		return WriteOverlapResult(
			Physics2D::OverlapPolygonRef({ originX, originY }, polygon, ToOverlapMode(mode)),
			entityID);
	}

	static int Axiom_Physics2D_OverlapCircleAll(float originX, float originY, float radius, uint64_t* outEntityIDs, int maxOut) {
		return WriteOverlapResults(
			Physics2D::OverlapCircleAllRefs({ originX, originY }, radius),
			outEntityIDs, maxOut);
	}

	static int Axiom_Physics2D_OverlapBoxAll(float originX, float originY, float halfX, float halfY, float degrees, uint64_t* outEntityIDs, int maxOut) {
		return WriteOverlapResults(
			Physics2D::OverlapBoxAllRefs({ originX, originY }, { halfX, halfY }, degrees),
			outEntityIDs, maxOut);
	}

	static int Axiom_Physics2D_OverlapPolygonAll(float originX, float originY, const float* points, int pointCount, uint64_t* outEntityIDs, int maxOut) {
		std::vector<Vec2> polygon = ReadPolygonPoints(points, pointCount);
		if (polygon.empty()) {
			return 0;
		}

		return WriteOverlapResults(
			Physics2D::OverlapPolygonAllRefs({ originX, originY }, polygon),
			outEntityIDs, maxOut);
	}

	static int Axiom_Physics2D_ContainsPoint(float originX, float originY, int mode, uint64_t* entityID) {
		return WriteOverlapResult(
			Physics2D::ContainsPointRef({ originX, originY }, ToOverlapMode(mode)),
			entityID);
	}

	static int Axiom_Physics2D_ContainsPointAll(float originX, float originY, uint64_t* outEntityIDs, int maxOut) {
		return WriteOverlapResults(
			Physics2D::ContainsPointAllRefs({ originX, originY }),
			outEntityIDs, maxOut);
	}

	// ── UI: RectTransform2D ─────────────────────────────────────────────

	static void Axiom_RectTransform_GetAnchorMin(uint64_t entityID, float* outX, float* outY) {
		GET_COMPONENT(RectTransform2DComponent, entityID, (void)(*outX = 0.5f, *outY = 0.5f));
		*outX = comp.AnchorMin.x; *outY = comp.AnchorMin.y;
	}
	static void Axiom_RectTransform_SetAnchorMin(uint64_t entityID, float x, float y) {
		GET_COMPONENT(RectTransform2DComponent, entityID, );
		comp.AnchorMin = { x, y };
	}
	static void Axiom_RectTransform_GetAnchorMax(uint64_t entityID, float* outX, float* outY) {
		GET_COMPONENT(RectTransform2DComponent, entityID, (void)(*outX = 0.5f, *outY = 0.5f));
		*outX = comp.AnchorMax.x; *outY = comp.AnchorMax.y;
	}
	static void Axiom_RectTransform_SetAnchorMax(uint64_t entityID, float x, float y) {
		GET_COMPONENT(RectTransform2DComponent, entityID, );
		comp.AnchorMax = { x, y };
	}
	static void Axiom_RectTransform_GetPivot(uint64_t entityID, float* outX, float* outY) {
		GET_COMPONENT(RectTransform2DComponent, entityID, (void)(*outX = 0.5f, *outY = 0.5f));
		*outX = comp.Pivot.x; *outY = comp.Pivot.y;
	}
	static void Axiom_RectTransform_SetPivot(uint64_t entityID, float x, float y) {
		GET_COMPONENT(RectTransform2DComponent, entityID, );
		comp.Pivot = { x, y };
	}
	static void Axiom_RectTransform_GetAnchoredPosition(uint64_t entityID, float* outX, float* outY) {
		GET_COMPONENT(RectTransform2DComponent, entityID, (void)(*outX = 0.0f, *outY = 0.0f));
		*outX = comp.AnchoredPosition.x; *outY = comp.AnchoredPosition.y;
	}
	static void Axiom_RectTransform_SetAnchoredPosition(uint64_t entityID, float x, float y) {
		GET_COMPONENT(RectTransform2DComponent, entityID, );
		comp.AnchoredPosition = { x, y };
	}
	static void Axiom_RectTransform_GetSizeDelta(uint64_t entityID, float* outX, float* outY) {
		GET_COMPONENT(RectTransform2DComponent, entityID, (void)(*outX = 100.0f, *outY = 100.0f));
		*outX = comp.SizeDelta.x; *outY = comp.SizeDelta.y;
	}
	static void Axiom_RectTransform_SetSizeDelta(uint64_t entityID, float x, float y) {
		GET_COMPONENT(RectTransform2DComponent, entityID, );
		comp.SizeDelta = { x, y };
	}
	static float Axiom_RectTransform_GetRotation(uint64_t entityID) {
		GET_COMPONENT(RectTransform2DComponent, entityID, 0.0f);
		return comp.Rotation;
	}
	static void Axiom_RectTransform_SetRotation(uint64_t entityID, float rotation) {
		GET_COMPONENT(RectTransform2DComponent, entityID, );
		comp.Rotation = rotation;
	}
	static void Axiom_RectTransform_GetScale(uint64_t entityID, float* outX, float* outY) {
		GET_COMPONENT(RectTransform2DComponent, entityID, (void)(*outX = 1.0f, *outY = 1.0f));
		*outX = comp.Scale.x; *outY = comp.Scale.y;
	}
	static void Axiom_RectTransform_SetScale(uint64_t entityID, float x, float y) {
		GET_COMPONENT(RectTransform2DComponent, entityID, );
		comp.Scale = { x, y };
	}
	static void Axiom_RectTransform_GetResolvedSize(uint64_t entityID, float* outW, float* outH) {
		GET_COMPONENT(RectTransform2DComponent, entityID, (void)(*outW = 0.0f, *outH = 0.0f));
		const Vec2 size = comp.GetSize();
		*outW = size.x; *outH = size.y;
	}

	// ── UI: Image ───────────────────────────────────────────────────────

	static void Axiom_Image_GetColor(uint64_t entityID, float* r, float* g, float* b, float* a) {
		GET_COMPONENT(ImageComponent, entityID, (void)(*r = 1, *g = 1, *b = 1, *a = 1));
		*r = comp.Color.r; *g = comp.Color.g; *b = comp.Color.b; *a = comp.Color.a;
	}
	static void Axiom_Image_SetColor(uint64_t entityID, float r, float g, float b, float a) {
		GET_COMPONENT(ImageComponent, entityID, );
		comp.Color = Color{ r, g, b, a };
	}
	static uint64_t Axiom_Image_GetTexture(uint64_t entityID) {
		GET_COMPONENT(ImageComponent, entityID, 0ull);
		return static_cast<uint64_t>(comp.TextureAssetId);
	}
	static void Axiom_Image_SetTexture(uint64_t entityID, uint64_t assetId) {
		GET_COMPONENT(ImageComponent, entityID, );
		comp.TextureAssetId = UUID(assetId);
		comp.TextureHandle = (assetId != 0)
			? TextureManager::LoadTextureByUUID(assetId)
			: TextureHandle{};
	}

	// ── UI: Interactable ────────────────────────────────────────────────

	static int Axiom_Interactable_GetInteractable(uint64_t entityID) {
		GET_COMPONENT(InteractableComponent, entityID, 0);
		return comp.Interactable ? 1 : 0;
	}
	static void Axiom_Interactable_SetInteractable(uint64_t entityID, int value) {
		GET_COMPONENT(InteractableComponent, entityID, );
		comp.Interactable = value != 0;
	}
	static int Axiom_Interactable_GetIsHovered(uint64_t entityID) {
		GET_COMPONENT(InteractableComponent, entityID, 0);
		return comp.IsHovered ? 1 : 0;
	}
	static int Axiom_Interactable_GetIsClicked(uint64_t entityID) {
		GET_COMPONENT(InteractableComponent, entityID, 0);
		return comp.IsClicked ? 1 : 0;
	}
	static int Axiom_Interactable_GetIsPressed(uint64_t entityID) {
		GET_COMPONENT(InteractableComponent, entityID, 0);
		return comp.IsPressed ? 1 : 0;
	}
	static int Axiom_Interactable_GetIsMouseDown(uint64_t entityID) {
		GET_COMPONENT(InteractableComponent, entityID, 0);
		return comp.IsMouseDown ? 1 : 0;
	}
	static int Axiom_Interactable_GetIsMouseUp(uint64_t entityID) {
		GET_COMPONENT(InteractableComponent, entityID, 0);
		return comp.IsMouseUp ? 1 : 0;
	}

	// ── UI: Button ──────────────────────────────────────────────────────

	#define BUTTON_COLOR_BINDING(MEMBER, GETTER, SETTER) \
		static void GETTER(uint64_t entityID, float* r, float* g, float* b, float* a) { \
			GET_COMPONENT(ButtonComponent, entityID, (void)(*r = 1, *g = 1, *b = 1, *a = 1)); \
			*r = comp.MEMBER.r; *g = comp.MEMBER.g; *b = comp.MEMBER.b; *a = comp.MEMBER.a; \
		} \
		static void SETTER(uint64_t entityID, float r, float g, float b, float a) { \
			GET_COMPONENT(ButtonComponent, entityID, ); \
			comp.MEMBER = Color{ r, g, b, a }; \
		}

	BUTTON_COLOR_BINDING(NormalColor,   Axiom_Button_GetNormalColor,   Axiom_Button_SetNormalColor)
	BUTTON_COLOR_BINDING(HoveredColor,  Axiom_Button_GetHoveredColor,  Axiom_Button_SetHoveredColor)
	BUTTON_COLOR_BINDING(PressedColor,  Axiom_Button_GetPressedColor,  Axiom_Button_SetPressedColor)
	BUTTON_COLOR_BINDING(DisabledColor, Axiom_Button_GetDisabledColor, Axiom_Button_SetDisabledColor)
	#undef BUTTON_COLOR_BINDING

	// ── UI: Slider ──────────────────────────────────────────────────────

	static float Axiom_Slider_GetValue(uint64_t entityID) {
		GET_COMPONENT(SliderComponent, entityID, 0.0f);
		return comp.Value;
	}
	static void Axiom_Slider_SetValue(uint64_t entityID, float value) {
		GET_COMPONENT(SliderComponent, entityID, );
		comp.Value = value;
	}
	static float Axiom_Slider_GetMinValue(uint64_t entityID) {
		GET_COMPONENT(SliderComponent, entityID, 0.0f);
		return comp.MinValue;
	}
	static void Axiom_Slider_SetMinValue(uint64_t entityID, float value) {
		GET_COMPONENT(SliderComponent, entityID, );
		comp.MinValue = value;
	}
	static float Axiom_Slider_GetMaxValue(uint64_t entityID) {
		GET_COMPONENT(SliderComponent, entityID, 1.0f);
		return comp.MaxValue;
	}
	static void Axiom_Slider_SetMaxValue(uint64_t entityID, float value) {
		GET_COMPONENT(SliderComponent, entityID, );
		comp.MaxValue = value;
	}
	static int Axiom_Slider_GetWholeNumbers(uint64_t entityID) {
		GET_COMPONENT(SliderComponent, entityID, 0);
		return comp.WholeNumbers ? 1 : 0;
	}
	static void Axiom_Slider_SetWholeNumbers(uint64_t entityID, int value) {
		GET_COMPONENT(SliderComponent, entityID, );
		comp.WholeNumbers = value != 0;
	}
	static int Axiom_Slider_GetValueChangedThisFrame(uint64_t entityID) {
		GET_COMPONENT(SliderComponent, entityID, 0);
		return comp.ValueChangedThisFrame ? 1 : 0;
	}

	// ── UI: Toggle ──────────────────────────────────────────────────────

	static int Axiom_Toggle_GetIsOn(uint64_t entityID) {
		GET_COMPONENT(ToggleComponent, entityID, 0);
		return comp.IsOn ? 1 : 0;
	}
	static void Axiom_Toggle_SetIsOn(uint64_t entityID, int value) {
		GET_COMPONENT(ToggleComponent, entityID, );
		comp.IsOn = value != 0;
	}
	static int Axiom_Toggle_GetValueChangedThisFrame(uint64_t entityID) {
		GET_COMPONENT(ToggleComponent, entityID, 0);
		return comp.ValueChangedThisFrame ? 1 : 0;
	}

	// ── UI: InputField ──────────────────────────────────────────────────

	static int Axiom_InputField_GetTextBuffer(uint64_t entityID, char* outBuffer, int capacity) {
		GET_COMPONENT(InputFieldComponent, entityID, CopyStringToBuffer({}, outBuffer, capacity));
		return CopyStringToBuffer(comp.Text, outBuffer, capacity);
	}
	static void Axiom_InputField_SetText(uint64_t entityID, const char* text) {
		GET_COMPONENT(InputFieldComponent, entityID, );
		comp.Text = text ? text : "";
	}
	static int Axiom_InputField_GetPlaceholderTextBuffer(uint64_t entityID, char* outBuffer, int capacity) {
		GET_COMPONENT(InputFieldComponent, entityID, CopyStringToBuffer({}, outBuffer, capacity));
		return CopyStringToBuffer(comp.PlaceholderText, outBuffer, capacity);
	}
	static void Axiom_InputField_SetPlaceholderText(uint64_t entityID, const char* text) {
		GET_COMPONENT(InputFieldComponent, entityID, );
		comp.PlaceholderText = text ? text : "";
	}
	static int Axiom_InputField_GetIsFocused(uint64_t entityID) {
		GET_COMPONENT(InputFieldComponent, entityID, 0);
		return comp.IsFocused ? 1 : 0;
	}
	static void Axiom_InputField_SetIsFocused(uint64_t entityID, int value) {
		GET_COMPONENT(InputFieldComponent, entityID, );
		comp.IsFocused = value != 0;
	}
	static int Axiom_InputField_GetSubmittedThisFrame(uint64_t entityID) {
		GET_COMPONENT(InputFieldComponent, entityID, 0);
		return comp.SubmittedThisFrame ? 1 : 0;
	}
	static int Axiom_InputField_GetCharacterLimit(uint64_t entityID) {
		GET_COMPONENT(InputFieldComponent, entityID, 0);
		return comp.CharacterLimit;
	}
	static void Axiom_InputField_SetCharacterLimit(uint64_t entityID, int value) {
		GET_COMPONENT(InputFieldComponent, entityID, );
		comp.CharacterLimit = value;
	}

	// ── UI: Dropdown ────────────────────────────────────────────────────

	static int Axiom_Dropdown_GetSelectedIndex(uint64_t entityID) {
		GET_COMPONENT(DropdownComponent, entityID, 0);
		return comp.SelectedIndex;
	}
	static void Axiom_Dropdown_SetSelectedIndex(uint64_t entityID, int value) {
		GET_COMPONENT(DropdownComponent, entityID, );
		comp.SelectedIndex = value;
	}
	static int Axiom_Dropdown_GetIsOpen(uint64_t entityID) {
		GET_COMPONENT(DropdownComponent, entityID, 0);
		return comp.IsOpen ? 1 : 0;
	}
	static void Axiom_Dropdown_SetIsOpen(uint64_t entityID, int value) {
		GET_COMPONENT(DropdownComponent, entityID, );
		comp.IsOpen = value != 0;
	}
	static int Axiom_Dropdown_GetSelectionChangedThisFrame(uint64_t entityID) {
		GET_COMPONENT(DropdownComponent, entityID, 0);
		return comp.SelectionChangedThisFrame ? 1 : 0;
	}
	static int Axiom_Dropdown_GetOptionCount(uint64_t entityID) {
		GET_COMPONENT(DropdownComponent, entityID, 0);
		return static_cast<int>(comp.Options.size());
	}
	static int Axiom_Dropdown_GetOptionBuffer(uint64_t entityID, int index, char* outBuffer, int capacity) {
		GET_COMPONENT(DropdownComponent, entityID, CopyStringToBuffer({}, outBuffer, capacity));
		if (index < 0 || index >= static_cast<int>(comp.Options.size())) {
			return CopyStringToBuffer({}, outBuffer, capacity);
		}
		return CopyStringToBuffer(comp.Options[index], outBuffer, capacity);
	}
	static void Axiom_Dropdown_SetOption(uint64_t entityID, int index, const char* text) {
		GET_COMPONENT(DropdownComponent, entityID, );
		if (index < 0 || index >= static_cast<int>(comp.Options.size())) return;
		comp.Options[index] = text ? text : "";
	}
	static void Axiom_Dropdown_AddOption(uint64_t entityID, const char* text) {
		GET_COMPONENT(DropdownComponent, entityID, );
		comp.Options.emplace_back(text ? text : "");
	}
	static void Axiom_Dropdown_RemoveOption(uint64_t entityID, int index) {
		GET_COMPONENT(DropdownComponent, entityID, );
		if (index < 0 || index >= static_cast<int>(comp.Options.size())) return;
		comp.Options.erase(comp.Options.begin() + index);
	}
	static void Axiom_Dropdown_ClearOptions(uint64_t entityID) {
		GET_COMPONENT(DropdownComponent, entityID, );
		comp.Options.clear();
	}

	#undef GET_COMPONENT

	// ── Registration ────────────────────────────────────────────────────

	void ScriptBindings::PopulateNativeBindings(NativeBindings& b)
	{
		PopulateNonComponentBindings(b);

		b.Entity_Clone = &Axiom_Entity_Clone;
		b.Entity_InstantiatePrefab = &Axiom_Entity_InstantiatePrefab;
		b.Entity_GetOrigin = &Axiom_Entity_GetOrigin;
		b.Entity_GetRuntimeID = &Axiom_Entity_GetRuntimeID;
		b.Entity_GetSceneGUID = &Axiom_Entity_GetSceneGUID;
		b.Entity_GetPrefabGUID = &Axiom_Entity_GetPrefabGUID;
		b.Entity_HasComponent = &Axiom_Entity_HasComponent;
		b.Entity_AddComponent = &Axiom_Entity_AddComponent;
		b.Entity_RemoveComponent = &Axiom_Entity_RemoveComponent;
		// Deprecated: legacy non-buffer slot returns a pointer to a static thread-local
		// scratch buffer that races across calls. Managed code only uses the *Buffer
		// variant; keep wired commented-out so this trap is never set again.
		// b.Entity_GetManagedComponentFields = &Axiom_Entity_GetManagedComponentFields;
		b.Entity_GetManagedComponentFieldsBuffer = &Axiom_Entity_GetManagedComponentFieldsBuffer;
		b.Entity_GetIsStatic = &Axiom_Entity_GetIsStatic;
		b.Entity_SetIsStatic = &Axiom_Entity_SetIsStatic;
		b.Entity_GetIsEnabled = &Axiom_Entity_GetIsEnabled;
		b.Entity_SetIsEnabled = &Axiom_Entity_SetIsEnabled;

		// Deprecated: legacy non-buffer string slot — see Entity_GetManagedComponentFields above.
		// b.NameComponent_GetName = &Axiom_NameComponent_GetName;
		b.NameComponent_GetNameBuffer = &Axiom_NameComponent_GetNameBuffer;
		b.NameComponent_SetName = &Axiom_NameComponent_SetName;

		b.Transform2D_GetPosition = &Axiom_Transform2D_GetPosition;
		b.Transform2D_SetPosition = &Axiom_Transform2D_SetPosition;
		b.Transform2D_GetRotation = &Axiom_Transform2D_GetRotation;
		b.Transform2D_SetRotation = &Axiom_Transform2D_SetRotation;
		b.Transform2D_GetScale = &Axiom_Transform2D_GetScale;
		b.Transform2D_SetScale = &Axiom_Transform2D_SetScale;
		b.Transform2D_GetEntity = &Axiom_Transform2D_GetEntity;
		b.Transform2D_GetLocalPosition = &Axiom_Transform2D_GetLocalPosition;
		b.Transform2D_SetLocalPosition = &Axiom_Transform2D_SetLocalPosition;
		b.Transform2D_GetLocalRotation = &Axiom_Transform2D_GetLocalRotation;
		b.Transform2D_SetLocalRotation = &Axiom_Transform2D_SetLocalRotation;
		b.Transform2D_GetLocalScale = &Axiom_Transform2D_GetLocalScale;
		b.Transform2D_SetLocalScale = &Axiom_Transform2D_SetLocalScale;
		b.Transform2D_GetParent = &Axiom_Transform2D_GetParent;
		b.Transform2D_SetParent = &Axiom_Transform2D_SetParent;
		b.Transform2D_GetChildCount = &Axiom_Transform2D_GetChildCount;
		b.Transform2D_GetChildAt = &Axiom_Transform2D_GetChildAt;
		b.Transform2D_GetChildren = &Axiom_Transform2D_GetChildren;

		b.SpriteRenderer_GetColor = &Axiom_SpriteRenderer_GetColor;
		b.SpriteRenderer_SetColor = &Axiom_SpriteRenderer_SetColor;
		b.SpriteRenderer_GetTexture = &Axiom_SpriteRenderer_GetTexture;
		b.SpriteRenderer_SetTexture = &Axiom_SpriteRenderer_SetTexture;
		b.SpriteRenderer_GetSortingOrder = &Axiom_SpriteRenderer_GetSortingOrder;
		b.SpriteRenderer_SetSortingOrder = &Axiom_SpriteRenderer_SetSortingOrder;
		b.SpriteRenderer_GetSortingLayer = &Axiom_SpriteRenderer_GetSortingLayer;
		b.SpriteRenderer_SetSortingLayer = &Axiom_SpriteRenderer_SetSortingLayer;

		// Deprecated: legacy non-buffer string slot — see Entity_GetManagedComponentFields above.
		// b.TextRenderer_GetText = &Axiom_TextRenderer_GetText;
		b.TextRenderer_GetTextBuffer = &Axiom_TextRenderer_GetTextBuffer;
		b.TextRenderer_SetText = &Axiom_TextRenderer_SetText;
		b.TextRenderer_GetFont = &Axiom_TextRenderer_GetFont;
		b.TextRenderer_SetFont = &Axiom_TextRenderer_SetFont;
		b.TextRenderer_GetFontSize = &Axiom_TextRenderer_GetFontSize;
		b.TextRenderer_SetFontSize = &Axiom_TextRenderer_SetFontSize;
		b.TextRenderer_GetColor = &Axiom_TextRenderer_GetColor;
		b.TextRenderer_SetColor = &Axiom_TextRenderer_SetColor;
		b.TextRenderer_GetLetterSpacing = &Axiom_TextRenderer_GetLetterSpacing;
		b.TextRenderer_SetLetterSpacing = &Axiom_TextRenderer_SetLetterSpacing;
		b.TextRenderer_GetHAlign = &Axiom_TextRenderer_GetHAlign;
		b.TextRenderer_SetHAlign = &Axiom_TextRenderer_SetHAlign;
		b.TextRenderer_GetSortingOrder = &Axiom_TextRenderer_GetSortingOrder;
		b.TextRenderer_SetSortingOrder = &Axiom_TextRenderer_SetSortingOrder;
		b.TextRenderer_GetSortingLayer = &Axiom_TextRenderer_GetSortingLayer;
		b.TextRenderer_SetSortingLayer = &Axiom_TextRenderer_SetSortingLayer;

		b.Camera2D_GetOrthographicSize = &Axiom_Camera2D_GetOrthographicSize;
		b.Camera2D_SetOrthographicSize = &Axiom_Camera2D_SetOrthographicSize;
		b.Camera2D_GetZoom = &Axiom_Camera2D_GetZoom;
		b.Camera2D_SetZoom = &Axiom_Camera2D_SetZoom;
		b.Camera2D_GetClearColor = &Axiom_Camera2D_GetClearColor;
		b.Camera2D_SetClearColor = &Axiom_Camera2D_SetClearColor;
		b.Camera2D_ScreenToWorld = &Axiom_Camera2D_ScreenToWorld;
		b.Camera2D_GetViewportWidth = &Axiom_Camera2D_GetViewportWidth;
		b.Camera2D_GetViewportHeight = &Axiom_Camera2D_GetViewportHeight;
		b.Camera2D_GetMainEntity = &Axiom_Camera2D_GetMainEntity;

		b.Rigidbody2D_ApplyForce = &Axiom_Rigidbody2D_ApplyForce;
		b.Rigidbody2D_ApplyImpulse = &Axiom_Rigidbody2D_ApplyImpulse;
		b.Rigidbody2D_GetLinearVelocity = &Axiom_Rigidbody2D_GetLinearVelocity;
		b.Rigidbody2D_SetLinearVelocity = &Axiom_Rigidbody2D_SetLinearVelocity;
		b.Rigidbody2D_GetAngularVelocity = &Axiom_Rigidbody2D_GetAngularVelocity;
		b.Rigidbody2D_SetAngularVelocity = &Axiom_Rigidbody2D_SetAngularVelocity;
		b.Rigidbody2D_GetBodyType = &Axiom_Rigidbody2D_GetBodyType;
		b.Rigidbody2D_SetBodyType = &Axiom_Rigidbody2D_SetBodyType;
		b.Rigidbody2D_GetGravityScale = &Axiom_Rigidbody2D_GetGravityScale;
		b.Rigidbody2D_SetGravityScale = &Axiom_Rigidbody2D_SetGravityScale;
		b.Rigidbody2D_GetMass = &Axiom_Rigidbody2D_GetMass;
		b.Rigidbody2D_SetMass = &Axiom_Rigidbody2D_SetMass;

		b.BoxCollider2D_GetScale = &Axiom_BoxCollider2D_GetScale;
		b.BoxCollider2D_GetCenter = &Axiom_BoxCollider2D_GetCenter;
		b.BoxCollider2D_SetEnabled = &Axiom_BoxCollider2D_SetEnabled;

		b.CircleCollider2D_GetRadius = &Axiom_CircleCollider2D_GetRadius;
		b.CircleCollider2D_SetRadius = &Axiom_CircleCollider2D_SetRadius;
		b.CircleCollider2D_GetCenter = &Axiom_CircleCollider2D_GetCenter;
		b.CircleCollider2D_SetCenter = &Axiom_CircleCollider2D_SetCenter;
		b.CircleCollider2D_SetEnabled = &Axiom_CircleCollider2D_SetEnabled;

		b.PolygonCollider2D_GetVertexCount = &Axiom_PolygonCollider2D_GetVertexCount;
		b.PolygonCollider2D_GetWorldPoints = &Axiom_PolygonCollider2D_GetWorldPoints;
		b.PolygonCollider2D_SetPoints = &Axiom_PolygonCollider2D_SetPoints;
		b.PolygonCollider2D_SetSides = &Axiom_PolygonCollider2D_SetSides;
		b.PolygonCollider2D_GetCenter = &Axiom_PolygonCollider2D_GetCenter;
		b.PolygonCollider2D_SetCenter = &Axiom_PolygonCollider2D_SetCenter;
		b.PolygonCollider2D_GetSize = &Axiom_PolygonCollider2D_GetSize;
		b.PolygonCollider2D_SetSize = &Axiom_PolygonCollider2D_SetSize;
		b.PolygonCollider2D_SetEnabled = &Axiom_PolygonCollider2D_SetEnabled;

		b.AudioSource_Play = &Axiom_AudioSource_Play;
		b.AudioSource_Pause = &Axiom_AudioSource_Pause;
		b.AudioSource_Stop = &Axiom_AudioSource_Stop;
		b.AudioSource_Resume = &Axiom_AudioSource_Resume;
		b.AudioSource_GetVolume = &Axiom_AudioSource_GetVolume;
		b.AudioSource_SetVolume = &Axiom_AudioSource_SetVolume;
		b.AudioSource_GetPitch = &Axiom_AudioSource_GetPitch;
		b.AudioSource_SetPitch = &Axiom_AudioSource_SetPitch;
		b.AudioSource_GetLoop = &Axiom_AudioSource_GetLoop;
		b.AudioSource_SetLoop = &Axiom_AudioSource_SetLoop;
		b.AudioSource_IsPlaying = &Axiom_AudioSource_IsPlaying;
		b.AudioSource_IsPaused = &Axiom_AudioSource_IsPaused;
		b.AudioSource_GetAudio = &Axiom_AudioSource_GetAudio;
		b.AudioSource_SetAudio = &Axiom_AudioSource_SetAudio;

		b.FastBody2D_GetBodyType = &Axiom_FastBody2D_GetBodyType;
		b.FastBody2D_SetBodyType = &Axiom_FastBody2D_SetBodyType;
		b.FastBody2D_GetMass = &Axiom_FastBody2D_GetMass;
		b.FastBody2D_SetMass = &Axiom_FastBody2D_SetMass;
		b.FastBody2D_GetUseGravity = &Axiom_FastBody2D_GetUseGravity;
		b.FastBody2D_SetUseGravity = &Axiom_FastBody2D_SetUseGravity;
		b.FastBody2D_GetVelocity = &Axiom_FastBody2D_GetVelocity;
		b.FastBody2D_SetVelocity = &Axiom_FastBody2D_SetVelocity;
		b.FastBoxCollider2D_GetHalfExtents = &Axiom_FastBoxCollider2D_GetHalfExtents;
		b.FastBoxCollider2D_SetHalfExtents = &Axiom_FastBoxCollider2D_SetHalfExtents;
		b.FastCircleCollider2D_GetRadius = &Axiom_FastCircleCollider2D_GetRadius;
		b.FastCircleCollider2D_SetRadius = &Axiom_FastCircleCollider2D_SetRadius;

		// Deprecated: legacy non-buffer string slot — see Entity_GetManagedComponentFields above.
		// b.Scene_GetActiveSceneName = &Axiom_Scene_GetActiveSceneName;
		b.Scene_GetActiveSceneNameBuffer = &Axiom_Scene_GetActiveSceneNameBuffer;
		b.Scene_GetEntityCount = &Axiom_Scene_GetEntityCount;
		b.Scene_GetEntityCountByName = &Axiom_Scene_GetEntityCountByName;
		b.Scene_LoadAdditive = &Axiom_Scene_LoadAdditive;
		b.Scene_Load = &Axiom_Scene_Load;
		b.Scene_Unload = &Axiom_Scene_Unload;
		b.Scene_SetActive = &Axiom_Scene_SetActive;
		b.Scene_Reload = &Axiom_Scene_Reload;
		b.Scene_SetGameSystemEnabled = &Axiom_Scene_SetGameSystemEnabled;
		b.Scene_SetGlobalSystemEnabled = &Axiom_Scene_SetGlobalSystemEnabled;
		b.Scene_DoesSceneExist = &Axiom_Scene_DoesSceneExist;
		b.Scene_GetLoadedCount = &Axiom_Scene_GetLoadedCount;
		b.Scene_GetLoadedSceneNameAt = &Axiom_Scene_GetLoadedSceneNameAt;
		b.Scene_GetLoadedSceneNameAtBuffer = &Axiom_Scene_GetLoadedSceneNameAtBuffer;
		// Deprecated: legacy non-buffer string slot — see Entity_GetManagedComponentFields above.
		// b.Scene_GetEntityNameByUUID = &Axiom_Scene_GetEntityNameByUUID;
		b.Scene_GetEntityNameByUUIDBuffer = &Axiom_Scene_GetEntityNameByUUIDBuffer;
		b.Scene_QueryEntities = &Axiom_Scene_QueryEntities;
		b.Scene_QueryEntitiesFiltered = &Axiom_Scene_QueryEntitiesFiltered;
		b.Scene_QueryEntitiesInScene = &Axiom_Scene_QueryEntitiesInScene;
		b.Scene_QueryEntitiesFilteredInScene = &Axiom_Scene_QueryEntitiesFilteredInScene;

		b.Asset_IsValid = &Axiom_Asset_IsValid;
		b.Asset_GetOrCreateUUIDFromPath = &Axiom_Asset_GetOrCreateUUIDFromPath;
		// Deprecated: legacy non-buffer string slots — see Entity_GetManagedComponentFields above.
		// b.Asset_GetPath = &Axiom_Asset_GetPath;
		b.Asset_GetPathBuffer = &Axiom_Asset_GetPathBuffer;
		// b.Asset_GetDisplayName = &Axiom_Asset_GetDisplayName;
		b.Asset_GetDisplayNameBuffer = &Axiom_Asset_GetDisplayNameBuffer;
		b.Asset_GetKind = &Axiom_Asset_GetKind;
		// b.Asset_FindAll = &Axiom_Asset_FindAll;
		b.Asset_FindAllBuffer = &Axiom_Asset_FindAllBuffer;
		b.Texture_LoadAsset = &Axiom_Texture_LoadAsset;
		b.Texture_GetWidth = &Axiom_Texture_GetWidth;
		b.Texture_GetHeight = &Axiom_Texture_GetHeight;
		b.Audio_LoadAsset = &Axiom_Audio_LoadAsset;
		b.Audio_PlayOneShotAsset = &Axiom_Audio_PlayOneShotAsset;
		b.Font_LoadAsset = &Axiom_Font_LoadAsset;

		b.ParticleSystem2D_Play = &Axiom_ParticleSystem2D_Play;
		b.ParticleSystem2D_Pause = &Axiom_ParticleSystem2D_Pause;
		b.ParticleSystem2D_Stop = &Axiom_ParticleSystem2D_Stop;
		b.ParticleSystem2D_IsPlaying = &Axiom_ParticleSystem2D_IsPlaying;
		b.ParticleSystem2D_GetPlayOnAwake = &Axiom_ParticleSystem2D_GetPlayOnAwake;
		b.ParticleSystem2D_SetPlayOnAwake = &Axiom_ParticleSystem2D_SetPlayOnAwake;
		b.ParticleSystem2D_GetColor = &Axiom_ParticleSystem2D_GetColor;
		b.ParticleSystem2D_SetColor = &Axiom_ParticleSystem2D_SetColor;
		b.ParticleSystem2D_GetLifeTime = &Axiom_ParticleSystem2D_GetLifeTime;
		b.ParticleSystem2D_SetLifeTime = &Axiom_ParticleSystem2D_SetLifeTime;
		b.ParticleSystem2D_GetSpeed = &Axiom_ParticleSystem2D_GetSpeed;
		b.ParticleSystem2D_SetSpeed = &Axiom_ParticleSystem2D_SetSpeed;
		b.ParticleSystem2D_GetScale = &Axiom_ParticleSystem2D_GetScale;
		b.ParticleSystem2D_SetScale = &Axiom_ParticleSystem2D_SetScale;
		b.ParticleSystem2D_GetEmitOverTime = &Axiom_ParticleSystem2D_GetEmitOverTime;
		b.ParticleSystem2D_SetEmitOverTime = &Axiom_ParticleSystem2D_SetEmitOverTime;
		b.ParticleSystem2D_Emit = &Axiom_ParticleSystem2D_Emit;

		b.Gizmo_DrawLine = &Axiom_Gizmo_DrawLine;
		b.Gizmo_DrawSquare = &Axiom_Gizmo_DrawSquare;
		b.Gizmo_DrawCircle = &Axiom_Gizmo_DrawCircle;
		b.Gizmo_SetColor = &Axiom_Gizmo_SetColor;
		b.Gizmo_GetColor = &Axiom_Gizmo_GetColor;
		b.Gizmo_GetLineWidth = &Axiom_Gizmo_GetLineWidth;
		b.Gizmo_SetLineWidth = &Axiom_Gizmo_SetLineWidth;

		b.Physics2D_Raycast = &Axiom_Physics2D_Raycast;
		b.Physics2D_OverlapCircle = &Axiom_Physics2D_OverlapCircle;
		b.Physics2D_OverlapBox = &Axiom_Physics2D_OverlapBox;
		b.Physics2D_OverlapPolygon = &Axiom_Physics2D_OverlapPolygon;
		b.Physics2D_OverlapCircleAll = &Axiom_Physics2D_OverlapCircleAll;
		b.Physics2D_OverlapBoxAll = &Axiom_Physics2D_OverlapBoxAll;
		b.Physics2D_OverlapPolygonAll = &Axiom_Physics2D_OverlapPolygonAll;
		b.Physics2D_ContainsPoint = &Axiom_Physics2D_ContainsPoint;
		b.Physics2D_ContainsPointAll = &Axiom_Physics2D_ContainsPointAll;

		// ── UI ──────────────────────────────────────────────────────
		b.RectTransform_GetAnchorMin        = &Axiom_RectTransform_GetAnchorMin;
		b.RectTransform_SetAnchorMin        = &Axiom_RectTransform_SetAnchorMin;
		b.RectTransform_GetAnchorMax        = &Axiom_RectTransform_GetAnchorMax;
		b.RectTransform_SetAnchorMax        = &Axiom_RectTransform_SetAnchorMax;
		b.RectTransform_GetPivot            = &Axiom_RectTransform_GetPivot;
		b.RectTransform_SetPivot            = &Axiom_RectTransform_SetPivot;
		b.RectTransform_GetAnchoredPosition = &Axiom_RectTransform_GetAnchoredPosition;
		b.RectTransform_SetAnchoredPosition = &Axiom_RectTransform_SetAnchoredPosition;
		b.RectTransform_GetSizeDelta        = &Axiom_RectTransform_GetSizeDelta;
		b.RectTransform_SetSizeDelta        = &Axiom_RectTransform_SetSizeDelta;
		b.RectTransform_GetRotation         = &Axiom_RectTransform_GetRotation;
		b.RectTransform_SetRotation         = &Axiom_RectTransform_SetRotation;
		b.RectTransform_GetScale            = &Axiom_RectTransform_GetScale;
		b.RectTransform_SetScale            = &Axiom_RectTransform_SetScale;
		b.RectTransform_GetResolvedSize     = &Axiom_RectTransform_GetResolvedSize;

		b.Image_GetColor   = &Axiom_Image_GetColor;
		b.Image_SetColor   = &Axiom_Image_SetColor;
		b.Image_GetTexture = &Axiom_Image_GetTexture;
		b.Image_SetTexture = &Axiom_Image_SetTexture;

		b.Interactable_GetInteractable = &Axiom_Interactable_GetInteractable;
		b.Interactable_SetInteractable = &Axiom_Interactable_SetInteractable;
		b.Interactable_GetIsHovered    = &Axiom_Interactable_GetIsHovered;
		b.Interactable_GetIsClicked    = &Axiom_Interactable_GetIsClicked;
		b.Interactable_GetIsPressed    = &Axiom_Interactable_GetIsPressed;
		b.Interactable_GetIsMouseDown  = &Axiom_Interactable_GetIsMouseDown;
		b.Interactable_GetIsMouseUp    = &Axiom_Interactable_GetIsMouseUp;

		b.Button_GetNormalColor   = &Axiom_Button_GetNormalColor;
		b.Button_SetNormalColor   = &Axiom_Button_SetNormalColor;
		b.Button_GetHoveredColor  = &Axiom_Button_GetHoveredColor;
		b.Button_SetHoveredColor  = &Axiom_Button_SetHoveredColor;
		b.Button_GetPressedColor  = &Axiom_Button_GetPressedColor;
		b.Button_SetPressedColor  = &Axiom_Button_SetPressedColor;
		b.Button_GetDisabledColor = &Axiom_Button_GetDisabledColor;
		b.Button_SetDisabledColor = &Axiom_Button_SetDisabledColor;

		b.Slider_GetValue                 = &Axiom_Slider_GetValue;
		b.Slider_SetValue                 = &Axiom_Slider_SetValue;
		b.Slider_GetMinValue              = &Axiom_Slider_GetMinValue;
		b.Slider_SetMinValue              = &Axiom_Slider_SetMinValue;
		b.Slider_GetMaxValue              = &Axiom_Slider_GetMaxValue;
		b.Slider_SetMaxValue              = &Axiom_Slider_SetMaxValue;
		b.Slider_GetWholeNumbers          = &Axiom_Slider_GetWholeNumbers;
		b.Slider_SetWholeNumbers          = &Axiom_Slider_SetWholeNumbers;
		b.Slider_GetValueChangedThisFrame = &Axiom_Slider_GetValueChangedThisFrame;

		b.Toggle_GetIsOn                  = &Axiom_Toggle_GetIsOn;
		b.Toggle_SetIsOn                  = &Axiom_Toggle_SetIsOn;
		b.Toggle_GetValueChangedThisFrame = &Axiom_Toggle_GetValueChangedThisFrame;

		b.InputField_GetTextBuffer            = &Axiom_InputField_GetTextBuffer;
		b.InputField_SetText                  = &Axiom_InputField_SetText;
		b.InputField_GetPlaceholderTextBuffer = &Axiom_InputField_GetPlaceholderTextBuffer;
		b.InputField_SetPlaceholderText       = &Axiom_InputField_SetPlaceholderText;
		b.InputField_GetIsFocused             = &Axiom_InputField_GetIsFocused;
		b.InputField_SetIsFocused             = &Axiom_InputField_SetIsFocused;
		b.InputField_GetSubmittedThisFrame    = &Axiom_InputField_GetSubmittedThisFrame;
		b.InputField_GetCharacterLimit        = &Axiom_InputField_GetCharacterLimit;
		b.InputField_SetCharacterLimit        = &Axiom_InputField_SetCharacterLimit;

		b.Dropdown_GetSelectedIndex             = &Axiom_Dropdown_GetSelectedIndex;
		b.Dropdown_SetSelectedIndex             = &Axiom_Dropdown_SetSelectedIndex;
		b.Dropdown_GetIsOpen                    = &Axiom_Dropdown_GetIsOpen;
		b.Dropdown_SetIsOpen                    = &Axiom_Dropdown_SetIsOpen;
		b.Dropdown_GetSelectionChangedThisFrame = &Axiom_Dropdown_GetSelectionChangedThisFrame;
		b.Dropdown_GetOptionCount               = &Axiom_Dropdown_GetOptionCount;
		b.Dropdown_GetOptionBuffer              = &Axiom_Dropdown_GetOptionBuffer;
		b.Dropdown_SetOption                    = &Axiom_Dropdown_SetOption;
		b.Dropdown_AddOption                    = &Axiom_Dropdown_AddOption;
		b.Dropdown_RemoveOption                 = &Axiom_Dropdown_RemoveOption;
		b.Dropdown_ClearOptions                 = &Axiom_Dropdown_ClearOptions;
	}

} // namespace Axiom
