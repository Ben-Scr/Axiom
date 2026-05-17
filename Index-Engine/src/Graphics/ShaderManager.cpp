#include "pch.hpp"
#include "Graphics/ShaderManager.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Log.hpp"
#include "Graphics/ShaderEntry.hpp"

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <utility>

// =============================================================================
// ShaderManager — slot table + handle-generation pool for compiled shader
// programs. Dedup key is the (vsPath, fsPath) pair. Mirrors TextureManager's
// internal layout; see Graphics/TextureManager.cpp for the reference
// implementation and design notes.
// =============================================================================

namespace Index {

    std::vector<ShaderEntry> ShaderManager::s_Shaders;
    std::queue<uint16_t>     ShaderManager::s_FreeIndices;
    bool                     ShaderManager::s_IsInitialized = false;

    namespace {
        struct ProviderEntry {
            uint32_t Token = 0;
            ShaderManager::ReferenceProvider Provider;
        };
        std::vector<ProviderEntry> g_Providers;
        uint32_t g_NextProviderToken = 1;

        std::string CanonicalPathKey(const std::string& path) {
            if (path.empty()) return {};
            std::error_code ec;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
            if (ec) {
                canonical = std::filesystem::absolute(path, ec);
                if (ec) {
                    canonical = path;
                }
            }
            return canonical.lexically_normal().make_preferred().string();
        }
    }

    void ShaderManager::Initialize() {
        if (s_IsInitialized) return;
        s_Shaders.clear();
        while (!s_FreeIndices.empty()) s_FreeIndices.pop();
        s_Shaders.reserve(16);
        s_IsInitialized = true;
    }

    void ShaderManager::Shutdown() {
        UnloadAll();
        s_Shaders.clear();
        while (!s_FreeIndices.empty()) s_FreeIndices.pop();
        g_Providers.clear();
        s_IsInitialized = false;
    }

    ShaderHandle ShaderManager::LoadShader(const std::string_view& vsPath, const std::string_view& fsPath) {
        if (!s_IsInitialized || vsPath.empty() || fsPath.empty()) return ShaderHandle{};

        const std::string vs(vsPath);
        const std::string fs(fsPath);

        // Reuse existing slot when the same pair is already cached.
        ShaderHandle existing = FindShaderByPaths(vs, fs);
        if (IsValid(existing)) return existing;

        // Compile first. Don't claim a slot until we know the shader is
        // valid — that way a failed load leaves the cache untouched.
        auto shader = std::make_unique<Shader>(vs, fs);
        if (!shader->IsValid()) {
            IDX_CORE_WARN_TAG("ShaderManager", "Failed to load shader: vs='{}' fs='{}'", vs, fs);
            return ShaderHandle{};
        }

        uint16_t idx;
        if (!s_FreeIndices.empty()) {
            idx = s_FreeIndices.front();
            s_FreeIndices.pop();
        }
        else {
            idx = static_cast<uint16_t>(s_Shaders.size());
            s_Shaders.emplace_back();
        }
        ShaderEntry& slot = s_Shaders[idx];
        slot.Shader = std::move(shader);
        slot.VsPath = vs;
        slot.FsPath = fs;
        slot.IsValid = true;
        return ShaderHandle{ idx, slot.Generation };
    }

    ShaderHandle ShaderManager::LoadShaderByUUID(uint64_t vsAssetId, uint64_t fsAssetId) {
        if (!s_IsInitialized || vsAssetId == 0 || fsAssetId == 0) return ShaderHandle{};

        auto resolve = [](uint64_t id) -> std::string {
            std::string path = AssetRegistry::ResolvePath(id);
            if (path.empty()) {
                AssetRegistry::MarkDirty();
                AssetRegistry::Sync();
                path = AssetRegistry::ResolvePath(id);
            }
            return path;
        };

        const std::string vs = resolve(vsAssetId);
        const std::string fs = resolve(fsAssetId);
        if (vs.empty() || fs.empty()) return ShaderHandle{};
        return LoadShader(vs, fs);
    }

    void ShaderManager::UnloadShader(ShaderHandle handle) {
        if (!IsValid(handle)) return;
        ShaderEntry& slot = s_Shaders[handle.index];
        slot.Shader.reset();
        slot.VsPath.clear();
        slot.FsPath.clear();
        slot.IsValid = false;
        ++slot.Generation;
        s_FreeIndices.push(handle.index);
    }

    Shader* ShaderManager::GetShader(ShaderHandle handle) {
        if (!IsValid(handle)) return nullptr;
        return s_Shaders[handle.index].Shader.get();
    }

    bool ShaderManager::IsValid(ShaderHandle handle) {
        if (!s_IsInitialized) return false;
        if (handle.index >= s_Shaders.size()) return false;
        const ShaderEntry& slot = s_Shaders[handle.index];
        return slot.IsValid && slot.Generation == handle.generation;
    }

    ShaderHandle ShaderManager::GetShaderHandle(const std::string& vsPath, const std::string& fsPath) {
        return FindShaderByPaths(vsPath, fsPath);
    }

    bool ShaderManager::ReloadShader(ShaderHandle handle) {
        if (!IsValid(handle)) return false;

        ShaderEntry& slot = s_Shaders[handle.index];
        if (slot.VsPath.empty() || slot.FsPath.empty()) return false;

        // Build into a temporary; only swap on success so a syntax-error
        // reload doesn't drop the renderer's currently-bound shader.
        auto reloaded = std::make_unique<Shader>(slot.VsPath, slot.FsPath);
        if (!reloaded->IsValid()) {
            IDX_CORE_WARN_TAG("ShaderManager", "Hot reload failed: vs='{}' fs='{}'", slot.VsPath, slot.FsPath);
            return false;
        }

        slot.Shader = std::move(reloaded);
        IDX_CORE_INFO_TAG("ShaderManager", "Reloaded shader: vs='{}' fs='{}'", slot.VsPath, slot.FsPath);
        return true;
    }

    size_t ShaderManager::ReloadShaderPath(const std::string& path) {
        if (!s_IsInitialized || path.empty()) return 0;
        const std::string targetKey = CanonicalPathKey(path);
        size_t count = 0;
        for (size_t i = 0; i < s_Shaders.size(); ++i) {
            const ShaderEntry& slot = s_Shaders[i];
            if (!slot.IsValid) continue;
            const bool vsMatch = !slot.VsPath.empty() && CanonicalPathKey(slot.VsPath) == targetKey;
            const bool fsMatch = !slot.FsPath.empty() && CanonicalPathKey(slot.FsPath) == targetKey;
            if (!vsMatch && !fsMatch) continue;
            if (ReloadShader(ShaderHandle{ static_cast<uint16_t>(i), slot.Generation })) {
                ++count;
            }
        }
        return count;
    }

    size_t ShaderManager::ReloadShadersFromDisk() {
        if (!s_IsInitialized) return 0;
        size_t count = 0;
        for (size_t i = 0; i < s_Shaders.size(); ++i) {
            const ShaderEntry& slot = s_Shaders[i];
            if (!slot.IsValid) continue;
            if (ReloadShader(ShaderHandle{ static_cast<uint16_t>(i), slot.Generation })) {
                ++count;
            }
        }
        return count;
    }

    std::vector<ShaderHandle> ShaderManager::GetLoadedHandles() {
        std::vector<ShaderHandle> out;
        out.reserve(s_Shaders.size());
        for (size_t i = 0; i < s_Shaders.size(); ++i) {
            if (s_Shaders[i].IsValid) {
                out.push_back(ShaderHandle{ static_cast<uint16_t>(i), s_Shaders[i].Generation });
            }
        }
        return out;
    }

    void ShaderManager::UnloadAll() {
        for (size_t i = 0; i < s_Shaders.size(); ++i) {
            ShaderEntry& slot = s_Shaders[i];
            if (!slot.IsValid) continue;
            slot.Shader.reset();
            slot.VsPath.clear();
            slot.FsPath.clear();
            slot.IsValid = false;
            ++slot.Generation;
            s_FreeIndices.push(static_cast<uint16_t>(i));
        }
    }

    size_t ShaderManager::PurgeUnreferenced() {
        if (!s_IsInitialized) return 0;

        std::unordered_set<ShaderHandle> live;
        const auto emit = [&live](ShaderHandle h) {
            if (h.IsValid()) live.insert(h);
        };
        for (const ProviderEntry& p : g_Providers) {
            if (p.Provider) p.Provider(emit);
        }

        size_t freed = 0;
        for (size_t i = 0; i < s_Shaders.size(); ++i) {
            ShaderEntry& slot = s_Shaders[i];
            if (!slot.IsValid) continue;
            ShaderHandle h{ static_cast<uint16_t>(i), slot.Generation };
            if (live.find(h) != live.end()) continue;
            slot.Shader.reset();
            slot.VsPath.clear();
            slot.FsPath.clear();
            slot.IsValid = false;
            ++slot.Generation;
            s_FreeIndices.push(static_cast<uint16_t>(i));
            ++freed;
        }
        return freed;
    }

    uint32_t ShaderManager::AddReferenceProvider(ReferenceProvider provider) {
        if (!provider) return 0;
        const uint32_t token = g_NextProviderToken++;
        g_Providers.push_back({ token, std::move(provider) });
        return token;
    }

    void ShaderManager::RemoveReferenceProvider(uint32_t token) {
        auto it = std::remove_if(g_Providers.begin(), g_Providers.end(),
            [token](const ProviderEntry& e) { return e.Token == token; });
        g_Providers.erase(it, g_Providers.end());
    }

    ShaderHandle ShaderManager::FindShaderByPaths(const std::string& vsPath, const std::string& fsPath) {
        for (size_t i = 0; i < s_Shaders.size(); ++i) {
            const ShaderEntry& e = s_Shaders[i];
            if (!e.IsValid) continue;
            if (e.VsPath == vsPath && e.FsPath == fsPath) {
                return ShaderHandle{ static_cast<uint16_t>(i), e.Generation };
            }
        }
        return ShaderHandle{};
    }

} // namespace Index
