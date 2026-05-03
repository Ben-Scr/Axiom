#pragma once
#include "Scene/EntityHandle.hpp"
#include <memory>
#include <string>

namespace Axiom {

	class Scene;
	namespace Json { class Value; }

	// Inspector body for a `.prefab` asset. Owns a detached editor-preview
	// `Scene` (see `Scene::CreateDetachedEditorScene`) into which the prefab's
	// root entity is deserialized for editing. Saving re-serializes the
	// detached entity back to disk via `SceneSerializer::SaveEntityToFile`.
	//
	// Lifetime: the detached scene is held by `m_PrefabScene` and torn down
	// when `Close()` is called or when a different prefab is `Open`'d. The
	// scene's destructor fires component-destroy hooks; those that touch
	// global subsystems are gated by `Scene::IsEditorPreview()`.
	//
	// Dirty tracking: any component-drawer interaction on the prefab window
	// marks the detached scene dirty via `Scene::MarkDirty`. `HasUnsavedChanges`
	// reads back through `m_PrefabScene->IsDirty()`.
	class PrefabInspector {
	public:
		PrefabInspector();
		~PrefabInspector();

		PrefabInspector(const PrefabInspector&) = delete;
		PrefabInspector& operator=(const PrefabInspector&) = delete;

		// Load `prefabPath` into a fresh detached scene. Replaces any prefab
		// currently open without prompting — the caller is responsible for
		// driving the save/discard prompt via `HasUnsavedChanges()` first.
		void Open(const std::string& prefabPath);

		// Tear down the detached scene. Caller should check `HasUnsavedChanges`.
		void Close();

		bool IsOpen() const { return m_PrefabScene != nullptr; }
		const std::string& GetCurrentPath() const { return m_PrefabPath; }
		bool HasUnsavedChanges() const;

		// Render the inspector body for the prefab's root entity. Returns true
		// if the user edited anything this frame (caller can use as a UX hint).
		bool Render();

		// Re-serialize the detached entity back to the .prefab file via
		// `SceneSerializer::SaveEntityToFile`. Returns true on success and
		// triggers propagation to live instances of this prefab in any open
		// scene (see `PropagateToLiveInstances`).
		bool Save();

	private:
		void PropagateToLiveInstances(uint64_t prefabGuid, const Json::Value& previousSourceEntity);

		std::unique_ptr<Scene> m_PrefabScene;
		EntityHandle m_RootEntity = entt::null;
		std::string m_PrefabPath;
	};

}
