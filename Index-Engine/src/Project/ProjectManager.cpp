#include "pch.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/IndexProject.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Serialization/PrefabTemplateCache.hpp"

namespace Index {

	std::unique_ptr<IndexProject> ProjectManager::s_CurrentProject = nullptr;

	void ProjectManager::SetCurrentProject(std::unique_ptr<IndexProject> project) {
		s_CurrentProject = std::move(project);
		AssetRegistry::MarkDirty();
		AssetRegistry::Sync();

		// Re-arm the prefab cache: drop every template baked against the
		// previous project and (in editor builds only) point the file
		// watcher at the new project's assets root so on-disk edits to a
		// .prefab invalidate the cache before the next spawn replays
		// stale bytes. The first-call cost is just the watcher thread
		// spin-up; subsequent project switches replace the watcher.
		PrefabTemplateCache& cache = PrefabTemplateCache::Get();
		cache.InvalidateAll();
#if defined(INDEX_WITH_EDITOR)
		if (IndexProject* live = s_CurrentProject.get(); live != nullptr && !live->AssetsDirectory.empty()) {
			cache.InitializeForProject(live->AssetsDirectory);
		}
		else {
			cache.Shutdown();
		}
#endif
	}

	IndexProject* ProjectManager::GetCurrentProject() {
		return s_CurrentProject.get();
	}

	bool ProjectManager::HasProject() {
		return s_CurrentProject != nullptr;
	}

} // namespace Index
