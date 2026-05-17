#include "pch.hpp"
#include "Graphics/Renderer2D.hpp"

#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/RenderApi.hpp"
#include "Graphics/StaticRenderData.hpp"
#include "Graphics/SpriteResources.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Math/Trigonometry.hpp"
#include "Scene/Scene.hpp"
#ifdef INDEX_PROFILER_ENABLED
#include "Profiling/GpuTimer.hpp"
#endif

#include <webgpu/webgpu_cpp.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

// =============================================================================
// Renderer2D — WebGPU (Dawn) implementation.
// -----------------------------------------------------------------------------
// Flow:
//   1. Initialize / Shutdown reference-count WebGPUSpriteResources (shared
//      with GuiRenderer).
//   2. RenderSceneWithVP collects Transform2D + SpriteRenderer pairs, sorts
//      by (SortingLayer, SortingOrder, DrawIndex) so the z-stack contract
//      holds, then issues one wgpu::RenderPipeline + one render pass + N
//      DrawIndexed calls (one per texture run).
//
// Architecture notes:
//   * One persistent instance VBO grown geometrically on demand
//     (queue.WriteBuffer each frame, never frees once grown).
//   * One persistent uniform VBO (64 bytes for the viewProj mat4), updated
//     once per RenderSceneWithVP via queue.WriteBuffer.
//   * Bind groups cached PERSISTENTLY, keyed by texture pool ID. Entries
//     are evicted by a TextureManager::DestroyListener registered in
//     Renderer2D::Initialize, fired BEFORE the underlying GPU resource
//     is torn down by UnloadTexture / UnloadAll / ReloadTexture's
//     move-assign. This avoids the per-frame device.CreateBindGroup
//     storm that the earlier per-frame clear caused on texture-heavy
//     scenes.
//   * One render pass per RenderSceneWithVP call with LoadOp::Load — so a
//     prior RenderApi::Clear's clear-only pass result is preserved. The
//     swap-chain-rendered flag is poked so Present()'s touch-fallback
//     skips its own redundant clear.
//   * Render pipeline is fetched from WebGPUSpriteResources keyed on the
//     current target's color format — covers swap chain (typically
//     BGRA8Unorm on Windows / Linux) and FBO targets (RGBA8Unorm) with
//     two cached pipelines per session.
//
// Still pending:
//   * Particle / external instance contributors — RegisterInstanceContributor
//     stays a stub until that hook is rebuilt.
//   * Premultiplied-alpha and alpha-cutoff per-instance toggles — the WGSL
//     sprite shader's fragment body is the simplest possible texel * color.
//   * GpuTimer integration — pending wgpu::QuerySet plumbing.
// =============================================================================

namespace Index {

	namespace {
		using WebGPUSpriteResources::SpriteInstance;

		// Per-frame CPU scratch (persistent so the heap doesn't churn).
		std::vector<Instance44>     g_InstancesScratch;
		std::vector<TextureHandle>  g_TexturesScratch;
		std::vector<SpriteInstance> g_GpuInstanceScratch;
		std::vector<size_t>         g_SortIndexScratch;
		std::vector<std::pair<uint32_t, Renderer2D::InstanceContributor>> g_InstanceContributors;
		uint32_t g_NextInstanceContributorToken = 1;

		struct StaticSpriteEntry {
			EntityHandle Entity = entt::null;
			AABB Bounds{};
			uint32_t DrawIndex = 0;
		};

		struct StaticSpriteGrid {
			uint64_t Version = 0;
			float CellSize = 512.0f;
			std::vector<StaticSpriteEntry> Entries;
			std::unordered_map<uint32_t, uint32_t> DrawIndices;
			std::unordered_map<uint64_t, std::vector<uint32_t>> Cells;
			std::vector<uint32_t> OverflowEntries;
			std::vector<uint32_t> QueryMarks;
			uint32_t QueryStamp = 1;
		};

		std::unordered_map<const Scene*, StaticSpriteGrid> g_StaticSpriteGrids;

		// Persistent GPU resources — survive across RenderSceneWithVP calls
		// inside a frame and across frames. Reset on Shutdown.
		wgpu::Buffer g_InstanceBuffer;
		uint32_t     g_InstanceBufferCapacity = 0;  // measured in SpriteInstance units

		wgpu::Buffer g_UniformBuffer;

		// Persistent bind-group cache. Keyed by Texture2D::GetHandle() (the
		// raw WGPUTextureView pointer cast to uint64_t under WebGPU). Lives
		// across frames — entries are evicted by a TextureManager
		// DestroyListener (registered in Renderer2D::Initialize) when the
		// underlying texture is unloaded, hot-reloaded, or wiped via
		// UnloadAll. This eliminates the per-frame device.CreateBindGroup
		// storm that the previous per-frame clear caused for scenes with
		// many unique textures.
		std::unordered_map<uint64_t, wgpu::BindGroup> g_BindGroupCache;

		// Token returned from TextureManager::AddDestroyListener so
		// Shutdown can unregister cleanly. 0 = no listener installed.
		uint32_t g_TextureDestroyListenerToken = 0;

		bool Vec2ExactEqual(const Vec2& a, const Vec2& b) {
			return a.x == b.x && a.y == b.y;
		}

		AABB CreateQuadAABB(const Vec2& position, const Vec2& scale, float rotation) {
			if (rotation == 0.0f) {
				const Vec2 halfExtents{ scale.x * 0.5f, scale.y * 0.5f };
				return { position - halfExtents, position + halfExtents };
			}
			if (AABB::IsAxisAligned(rotation)) {
				return AABB::Create(position, Vec2(scale.x * 0.5f, scale.y * 0.5f));
			}
			return AABB::Create(position, scale * 0.5f, Degrees(rotation));
		}

		const AABB& GetStaticSpriteAABB(entt::registry& registry,
			EntityHandle entity,
			const Transform2DComponent& transform)
		{
			StaticRenderData* cache = registry.try_get<StaticRenderData>(entity);
			if (!cache) {
				cache = &registry.emplace<StaticRenderData>(entity);
			}

			if (!cache->Valid
				|| !Vec2ExactEqual(cache->CachedPosition, transform.Position)
				|| !Vec2ExactEqual(cache->CachedScale, transform.Scale)
				|| cache->CachedRotation != transform.Rotation) {
				cache->CachedAABB = CreateQuadAABB(transform.Position, transform.Scale, transform.Rotation);
				cache->CachedPosition = transform.Position;
				cache->CachedScale = transform.Scale;
				cache->CachedRotation = transform.Rotation;
				cache->Valid = true;
			}

			return cache->CachedAABB;
		}

		AABB GetSpriteAABB(entt::registry& registry,
			EntityHandle entity,
			const Transform2DComponent& transform)
		{
			if (registry.all_of<StaticTag>(entity)) {
				return GetStaticSpriteAABB(registry, entity, transform);
			}
			return CreateQuadAABB(transform.Position, transform.Scale, transform.Rotation);
		}

		bool IsFinite(const AABB& bounds) {
			return std::isfinite(bounds.Min.x)
				&& std::isfinite(bounds.Min.y)
				&& std::isfinite(bounds.Max.x)
				&& std::isfinite(bounds.Max.y);
		}

		int32_t CellCoord(float value, float cellSize) {
			const float scaled = value / cellSize;
			if (scaled <= static_cast<float>(std::numeric_limits<int32_t>::min())) {
				return std::numeric_limits<int32_t>::min();
			}
			if (scaled >= static_cast<float>(std::numeric_limits<int32_t>::max())) {
				return std::numeric_limits<int32_t>::max();
			}
			return static_cast<int32_t>(std::floor(scaled));
		}

		uint64_t CellKey(int32_t x, int32_t y) {
			return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
				| static_cast<uint32_t>(y);
		}

		void AddStaticEntryToGrid(StaticSpriteGrid& grid, uint32_t entryIndex) {
			const AABB& bounds = grid.Entries[entryIndex].Bounds;
			if (!IsFinite(bounds)) {
				grid.OverflowEntries.push_back(entryIndex);
				return;
			}

			const int32_t minX = CellCoord(bounds.Min.x, grid.CellSize);
			const int32_t maxX = CellCoord(bounds.Max.x, grid.CellSize);
			const int32_t minY = CellCoord(bounds.Min.y, grid.CellSize);
			const int32_t maxY = CellCoord(bounds.Max.y, grid.CellSize);
			if (maxX < minX || maxY < minY) {
				grid.OverflowEntries.push_back(entryIndex);
				return;
			}

			const int64_t cellsX = static_cast<int64_t>(maxX) - static_cast<int64_t>(minX) + 1;
			const int64_t cellsY = static_cast<int64_t>(maxY) - static_cast<int64_t>(minY) + 1;
			constexpr int64_t k_MaxCellsPerSprite = 256;
			if (cellsX <= 0 || cellsY <= 0 || cellsX * cellsY > k_MaxCellsPerSprite) {
				grid.OverflowEntries.push_back(entryIndex);
				return;
			}

			for (int32_t y = minY; y <= maxY; ++y) {
				for (int32_t x = minX; x <= maxX; ++x) {
					grid.Cells[CellKey(x, y)].push_back(entryIndex);
				}
			}
		}

		void RebuildStaticSpriteGrid(Scene& scene, StaticSpriteGrid& grid) {
			grid.Version = scene.GetStaticRenderDataVersion();
			grid.Entries.clear();
			grid.DrawIndices.clear();
			grid.Cells.clear();
			grid.OverflowEntries.clear();
			grid.QueryMarks.clear();
			grid.QueryStamp = 1;

			auto& registry = scene.GetRegistry();
			auto view = registry.view<Transform2DComponent, SpriteRendererComponent>(entt::exclude<DisabledTag>);
			grid.Entries.reserve(registry.view<StaticTag>().size());
			grid.DrawIndices.reserve(registry.view<SpriteRendererComponent>().size());

			uint32_t drawIndex = 0;
			for (auto entity : view) {
				const uint32_t currentDrawIndex = drawIndex++;
				grid.DrawIndices[static_cast<uint32_t>(entity)] = currentDrawIndex;
				if (!registry.all_of<StaticTag>(entity)) {
					continue;
				}

				const auto& transform = view.get<Transform2DComponent>(entity);
				const uint32_t entryIndex = static_cast<uint32_t>(grid.Entries.size());
				grid.Entries.push_back(StaticSpriteEntry{
					entity,
					GetStaticSpriteAABB(registry, entity, transform),
					currentDrawIndex
				});
				AddStaticEntryToGrid(grid, entryIndex);
			}

			grid.QueryMarks.assign(grid.Entries.size(), 0);
		}

		StaticSpriteGrid& ResolveStaticSpriteGrid(Scene& scene) {
			StaticSpriteGrid& grid = g_StaticSpriteGrids[&scene];
			if (grid.Version != scene.GetStaticRenderDataVersion()) {
				RebuildStaticSpriteGrid(scene, grid);
			}
			return grid;
		}

		void AppendStaticSpriteInstance(Scene& scene,
			StaticSpriteGrid& grid,
			uint32_t entryIndex,
			const AABB& viewportAABB,
			std::vector<Instance44>& outInstances)
		{
			if (entryIndex >= grid.Entries.size() || entryIndex >= grid.QueryMarks.size()) return;
			if (grid.QueryMarks[entryIndex] == grid.QueryStamp) return;
			grid.QueryMarks[entryIndex] = grid.QueryStamp;

			const StaticSpriteEntry& entry = grid.Entries[entryIndex];
			if (!AABB::Intersects(viewportAABB, entry.Bounds)) {
				return;
			}

			auto& registry = scene.GetRegistry();
			if (!registry.valid(entry.Entity)
				|| !registry.all_of<Transform2DComponent, SpriteRendererComponent, StaticTag>(entry.Entity)
				|| registry.all_of<DisabledTag>(entry.Entity)) {
				scene.MarkStaticRenderDataDirty();
				return;
			}

			const auto& transform = registry.get<Transform2DComponent>(entry.Entity);
			const auto& sprite = registry.get<SpriteRendererComponent>(entry.Entity);
			outInstances.emplace_back(
				Vec2{ transform.Position.x, transform.Position.y },
				Vec2{ transform.Scale.x, transform.Scale.y },
				transform.Rotation,
				sprite.Color,
				sprite.TextureHandle,
				sprite.SortingOrder,
				sprite.SortingLayer,
				entry.DrawIndex);
		}

		void CollectStaticSpriteInstances(Scene& scene,
			StaticSpriteGrid& grid,
			const AABB& viewportAABB,
			std::vector<Instance44>& outInstances)
		{
			if (grid.Entries.empty()) {
				return;
			}

			++grid.QueryStamp;
			if (grid.QueryStamp == 0) {
				std::fill(grid.QueryMarks.begin(), grid.QueryMarks.end(), 0);
				grid.QueryStamp = 1;
			}

			if (!IsFinite(viewportAABB)) {
				for (uint32_t i = 0; i < static_cast<uint32_t>(grid.Entries.size()); ++i) {
					AppendStaticSpriteInstance(scene, grid, i, viewportAABB, outInstances);
				}
				return;
			}

			const int32_t minX = CellCoord(viewportAABB.Min.x, grid.CellSize);
			const int32_t maxX = CellCoord(viewportAABB.Max.x, grid.CellSize);
			const int32_t minY = CellCoord(viewportAABB.Min.y, grid.CellSize);
			const int32_t maxY = CellCoord(viewportAABB.Max.y, grid.CellSize);

			const int64_t cellsX = static_cast<int64_t>(maxX) - static_cast<int64_t>(minX) + 1;
			const int64_t cellsY = static_cast<int64_t>(maxY) - static_cast<int64_t>(minY) + 1;
			constexpr int64_t k_MaxQueryCells = 4096;
			if (maxX >= minX && maxY >= minY && cellsX > 0 && cellsY > 0 && cellsX * cellsY <= k_MaxQueryCells) {
				for (int32_t y = minY; y <= maxY; ++y) {
					for (int32_t x = minX; x <= maxX; ++x) {
						auto it = grid.Cells.find(CellKey(x, y));
						if (it == grid.Cells.end()) continue;
						for (uint32_t entryIndex : it->second) {
							AppendStaticSpriteInstance(scene, grid, entryIndex, viewportAABB, outInstances);
						}
					}
				}
			}
			else {
				for (uint32_t i = 0; i < static_cast<uint32_t>(grid.Entries.size()); ++i) {
					AppendStaticSpriteInstance(scene, grid, i, viewportAABB, outInstances);
				}
			}

			for (uint32_t entryIndex : grid.OverflowEntries) {
				AppendStaticSpriteInstance(scene, grid, entryIndex, viewportAABB, outInstances);
			}
		}

		// ── CPU collect path ────────────────────────────────────────────────
		size_t CollectSpriteInstances(Scene& scene,
			const AABB& viewportAABB,
			std::vector<Instance44>& outInstances,
			std::vector<TextureHandle>& outTextures)
		{
			outInstances.clear();
			outTextures.clear();
			auto& registry = scene.GetRegistry();
			StaticSpriteGrid* staticGrid = nullptr;
			if (registry.view<StaticTag>().size() > 0) {
				staticGrid = &ResolveStaticSpriteGrid(scene);
			}

			auto view = registry.view<Transform2DComponent, SpriteRendererComponent>(entt::exclude<DisabledTag, StaticTag>);
			// Reserve up-front so the per-frame collect loop doesn't trigger a
			// std::vector grow-and-copy mid-iteration. The capacity stabilises at
			// the high-water mark across frames because outInstances/outTextures
			// are caller-owned scratch buffers.
			const size_t dynamicHint = view.size_hint();
			const size_t staticHint = staticGrid ? staticGrid->Entries.size() : 0;
			outInstances.reserve(dynamicHint + staticHint);
			uint32_t dynamicDrawIndex = 0;
			for (auto entity : view) {
				uint32_t currentDrawIndex = dynamicDrawIndex++;
				if (staticGrid) {
					auto drawIndexIt = staticGrid->DrawIndices.find(static_cast<uint32_t>(entity));
					if (drawIndexIt != staticGrid->DrawIndices.end()) {
						currentDrawIndex = drawIndexIt->second;
					}
				}

				const auto& t = view.get<Transform2DComponent>(entity);
				const auto& s = view.get<SpriteRendererComponent>(entity);
				const AABB bounds = CreateQuadAABB(t.Position, t.Scale, t.Rotation);
				if (!AABB::Intersects(viewportAABB, bounds)) {
					continue;
				}

				outInstances.emplace_back(
					Vec2{ t.Position.x, t.Position.y },
					Vec2{ t.Scale.x, t.Scale.y },
					t.Rotation,
					s.Color,
					s.TextureHandle,
					s.SortingOrder,
					s.SortingLayer,
					currentDrawIndex);
			}

			if (staticGrid) {
				CollectStaticSpriteInstances(scene, *staticGrid, viewportAABB, outInstances);
			}

			auto particleView = scene.GetRegistry().view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>);
			for (auto entity : particleView) {
				const auto& particleSystem = particleView.get<ParticleSystem2DComponent>(entity);
				const TextureHandle texture = particleSystem.GetTextureHandle();
				for (const auto& particle : particleSystem.GetParticles()) {
					Vec2 position = particle.Transform.Position;
					Vec2 scale = particle.Transform.Scale;
					float rotation = particle.Transform.Rotation;
					if (particleSystem.EmissionSettings.EmissionSpace == ParticleSystem2DComponent::Space::Local) {
						const auto& emitterTransform = particleSystem.GetTransform2D();
						position = emitterTransform.TransformPoint(position);
						scale = Vec2{
							scale.x * emitterTransform.Scale.x,
							scale.y * emitterTransform.Scale.y
						};
						rotation += emitterTransform.Rotation;
					}
					if (!AABB::Intersects(viewportAABB, CreateQuadAABB(position, scale, rotation))) {
						continue;
					}
					outInstances.emplace_back(
						position,
						scale,
						rotation,
						particle.Color,
						texture,
						particleSystem.RenderingSettings.SortingOrder,
						particleSystem.RenderingSettings.SortingLayer,
						static_cast<std::uint32_t>(outInstances.size()));
				}
			}

			for (const auto& [token, contributor] : g_InstanceContributors) {
				(void)token;
				if (contributor) {
					contributor(scene, viewportAABB, outInstances);
				}
			}

			outTextures.reserve(outInstances.size());
			for (const Instance44& instance : outInstances) {
				outTextures.push_back(instance.TextureHandle);
			}
			return outInstances.size();
		}

		void SortInstancesInPlace(size_t n) {
			if (n < 2) return;
			auto sortLess = [](const Instance44& a, const Instance44& b) {
				if (a.SortingLayer != b.SortingLayer) return a.SortingLayer < b.SortingLayer;
				if (a.SortingOrder != b.SortingOrder) return a.SortingOrder < b.SortingOrder;
				return a.DrawIndex < b.DrawIndex;
			};

			bool alreadySorted = true;
			for (size_t k = 1; k < n; ++k) {
				if (sortLess(g_InstancesScratch[k], g_InstancesScratch[k - 1])) {
					alreadySorted = false;
					break;
				}
			}
			if (alreadySorted) return;

			// Indirect sort then in-place permutation following the cycle
			// chains of the resulting index array. The previous implementation
			// allocated two sorted scratch vectors and copied every element
			// once into them, then swapped — that's 3× the memory traffic of
			// what's actually required. This version uses one temporary per
			// cycle (two registers, effectively) and visits each element of
			// each parallel array exactly once during the rearrangement.
			g_SortIndexScratch.resize(n);
			for (size_t k = 0; k < n; ++k) g_SortIndexScratch[k] = k;
			std::sort(g_SortIndexScratch.begin(), g_SortIndexScratch.end(),
				[](size_t a, size_t b) {
					const auto& ia = g_InstancesScratch[a];
					const auto& ib = g_InstancesScratch[b];
					if (ia.SortingLayer != ib.SortingLayer) return ia.SortingLayer < ib.SortingLayer;
					if (ia.SortingOrder != ib.SortingOrder) return ia.SortingOrder < ib.SortingOrder;
					return ia.DrawIndex < ib.DrawIndex;
				});

			// Apply the permutation g_SortIndexScratch to both parallel
			// arrays in-place. std::sort produces READ-direction indices
			// (sorted[k] = src[idx[k]]), so we walk each cycle once,
			// pulling values backwards into position. The swap-as-you-go
			// form is shorter but is the WRITE-direction permutation and
			// would silently mis-sort here.
			//
			// Two moves per element across the whole array (one stash, one
			// final write) plus one assignment per intra-cycle hop —
			// strictly fewer copies than the previous double-scratch
			// implementation, and no auxiliary vector allocation.
			for (size_t i = 0; i < n; ++i) {
				if (g_SortIndexScratch[i] == i) continue;
				Instance44    holdInst = g_InstancesScratch[i];
				TextureHandle holdTex  = g_TexturesScratch[i];
				size_t j = i;
				while (g_SortIndexScratch[j] != i) {
					const size_t src = g_SortIndexScratch[j];
					g_InstancesScratch[j] = g_InstancesScratch[src];
					g_TexturesScratch[j]  = g_TexturesScratch[src];
					g_SortIndexScratch[j] = j; // mark settled
					j = src;
				}
				g_InstancesScratch[j] = holdInst;
				g_TexturesScratch[j]  = holdTex;
				g_SortIndexScratch[j] = j;
			}
		}

		// ── GPU resource helpers ────────────────────────────────────────────

		bool EnsureUniformBuffer(wgpu::Device device) {
			if (g_UniformBuffer) return true;
			wgpu::BufferDescriptor desc{};
			desc.size  = 64;  // mat4x4<f32>
			desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
			desc.label = "renderer2d-viewproj-ubo";
			g_UniformBuffer = device.CreateBuffer(&desc);
			if (!g_UniformBuffer) {
				IDX_CORE_ERROR_TAG("Renderer2D", "Failed to create uniform buffer");
				return false;
			}
			return true;
		}

		bool EnsureInstanceBuffer(wgpu::Device device, uint32_t neededInstances) {
			if (g_InstanceBufferCapacity >= neededInstances && g_InstanceBuffer) return true;

			// Grow with 2x headroom so a slowly growing scene doesn't recreate
			// the buffer every frame. 256 is a reasonable starting capacity
			// for typical 2D scenes; tune if profiling shows churn.
			uint32_t newCapacity = g_InstanceBufferCapacity > 0 ? g_InstanceBufferCapacity : 256;
			while (newCapacity < neededInstances) newCapacity *= 2;

			wgpu::BufferDescriptor desc{};
			desc.size  = static_cast<uint64_t>(newCapacity) * sizeof(SpriteInstance);
			desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
			desc.label = "renderer2d-instance-vbo";
			wgpu::Buffer buf = device.CreateBuffer(&desc);
			if (!buf) {
				IDX_CORE_ERROR_TAG("Renderer2D",
					"Failed to create instance buffer (cap={})", newCapacity);
				return false;
			}
			g_InstanceBuffer         = std::move(buf);
			g_InstanceBufferCapacity = newCapacity;
			return true;
		}

		wgpu::BindGroup ResolveBindGroup(wgpu::Device device, TextureHandle handle) {
			Texture2D* tex = TextureManager::GetTexture(handle);
			if (!tex || !tex->IsValid()) return nullptr;
			const uint64_t poolId = tex->GetHandle();

			auto cacheIt = g_BindGroupCache.find(poolId);
			if (cacheIt != g_BindGroupCache.end()) return cacheIt->second;

			const auto lookup = WebGPUBackend::LookupTexture2D(poolId);
			if (!lookup.Valid || !lookup.View || !lookup.Sampler) {
				return nullptr;
			}

			wgpu::BindGroupEntry entries[3] = {};
			entries[0].binding = 0;
			entries[0].buffer  = g_UniformBuffer;
			entries[0].offset  = 0;
			entries[0].size    = 64;
			entries[1].binding     = 1;
			entries[1].textureView = lookup.View;
			entries[2].binding = 2;
			entries[2].sampler = lookup.Sampler;

			wgpu::BindGroupDescriptor desc{};
			desc.layout     = WebGPUSpriteResources::GetBindGroupLayout();
			desc.entryCount = 3;
			desc.entries    = entries;
			desc.label      = "renderer2d-sprite-bindgroup";

			wgpu::BindGroup bg = device.CreateBindGroup(&desc);
			if (!bg) return nullptr;
			g_BindGroupCache.emplace(poolId, bg);
			return bg;
		}
	}

	// ── Renderer2D ──────────────────────────────────────────────────────────

	Renderer2D::Renderer2D() = default;
	Renderer2D::~Renderer2D() = default;

	void Renderer2D::Initialize() {
		WebGPUSpriteResources::Acquire();
		// Hook texture destruction so the persistent bind-group cache evicts
		// entries whose underlying GPU texture is about to disappear. Without
		// this, a stale bind group would hold a freed TextureView and trigger
		// a use-after-free on the next draw using the same poolId.
		if (g_TextureDestroyListenerToken == 0) {
			g_TextureDestroyListenerToken = TextureManager::AddDestroyListener(
				[](TextureHandle handle) {
					Texture2D* tex = TextureManager::GetTexture(handle);
					if (!tex) return;
					g_BindGroupCache.erase(tex->GetHandle());
				});
		}
		m_IsInitialized = true;
	}

	void Renderer2D::Shutdown() {
		if (!m_IsInitialized) return;
		WebGPUSpriteResources::Release();

		if (g_TextureDestroyListenerToken != 0) {
			TextureManager::RemoveDestroyListener(g_TextureDestroyListenerToken);
			g_TextureDestroyListenerToken = 0;
		}

		// Persistent GPU resources — drop now so the device is fully idle by
		// the time WebGPUApi::Shutdown unconfigures the surface.
		g_BindGroupCache.clear();
		g_StaticSpriteGrids.clear();
		g_InstanceBuffer         = nullptr;
		g_InstanceBufferCapacity = 0;
		g_UniformBuffer          = nullptr;

		m_IsInitialized = false;
	}

	void Renderer2D::ClearSceneCache(const Scene* scene) {
		if (!scene) {
			g_StaticSpriteGrids.clear();
			return;
		}
		g_StaticSpriteGrids.erase(scene);
	}

	void Renderer2D::BeginFrame() {
		m_DrawCallsCount = 0;
		m_RenderLoopDuration = 0.0f;
		// Bind-group cache is persistent across frames now — entries are
		// evicted by the TextureManager DestroyListener registered in
		// Initialize() whenever the underlying texture is unloaded or
		// hot-reloaded. No per-frame clear here.

		// Runtime auto-render path. Editors set SkipBeginFrameRender=true
		// and drive RenderSceneWithVP themselves into per-panel FBOs; the
		// runtime (Index-Runtime / Index-Launcher) leaves the flag false
		// and expects BeginFrame to do the work against the swap chain.
		// Without this, the Index-Runtime window stays whatever colour
		// `g_ClearColor` defaulted to at Init and no SpriteRenderer2D
		// quads ever appear — the symptom of the renderer's auto-path
		// being a leftover stub from the bgfx era.
		if (m_SkipBeginFrameRender) return;
		if (!m_SceneProvider) return;

		Camera2DComponent* cam = Camera2DComponent::Main();
		if (!cam || !cam->IsValid()) return;

		RenderApi::BindDefaultFramebuffer();

		// Pull the per-frame clear colour from the active main camera so
		// the runtime visually matches what the Game View panel shows in
		// the editor (which threads the same camera's GetClearColor()
		// into RenderSceneIntoFBO).
		RenderApi::SetClearColor(cam->GetClearColor());
		RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);

		const glm::mat4 vp = cam->GetViewProjectionMatrix();
		const AABB viewAABB = cam->GetViewportAABB();
		m_SceneProvider([&](Scene& scene) {
			RenderSceneWithVP(scene, vp, viewAABB);
		});
	}

	void Renderer2D::EndFrame() {
		// All submission already happened in RenderSceneWithVP; Present()
		// (from Window::SwapBuffers) finishes the encoder + surface.Present.
	}

	void Renderer2D::RenderScene(Scene& /*scene*/) {
		// Touch-equivalent already happened in RenderApi::Clear (or will
		// happen in Present's touch-fallback), so this is genuinely a
		// no-op when no VP / no instances are available.
	}

	void Renderer2D::RenderSceneWithVP(Scene& scene,
		const glm::mat4& vp, const AABB& viewportAABB)
	{
		if (!m_IsInitialized) return;
		if (!WebGPUSpriteResources::IsReady()) return;

		const auto renderStart = std::chrono::steady_clock::now();
		auto finishTiming = [&]() {
			m_RenderLoopDuration = std::chrono::duration<float, std::milli>(
				std::chrono::steady_clock::now() - renderStart).count();
			};

		const size_t n = CollectSpriteInstances(scene, viewportAABB, g_InstancesScratch, g_TexturesScratch);
		m_RenderedInstancesCount = n;
		if (n == 0) {
			finishTiming();
			return;
		}

		SortInstancesInPlace(n);

		auto target = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!target.Valid) {
			finishTiming();
			return;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();
		if (!device || !queue) {
			finishTiming();
			return;
		}

		const bool wireframePass = RenderApi::GetPolygonMode() == PolygonMode::Wireframe;
		const bool forceBlack = RenderApi::IsColorLogicOpClearEnabled();
		const auto pipelineMode = wireframePass
			? WebGPUSpriteResources::SpritePipelineMode::Wireframe
			: WebGPUSpriteResources::SpritePipelineMode::Filled;
		wgpu::RenderPipeline pipeline = WebGPUSpriteResources::GetSpritePipeline(
			target.ColorFormat, target.HasDepth, pipelineMode);
		if (!pipeline) {
			IDX_CORE_WARN_TAG("Renderer2D",
				"No pipeline for color-format {} (hasDepth={}) — skipping submit",
				static_cast<int>(target.ColorFormat), target.HasDepth);
			finishTiming();
			return;
		}

		if (!EnsureUniformBuffer(device)) {
			finishTiming();
			return;
		}
		queue.WriteBuffer(g_UniformBuffer, 0, glm::value_ptr(vp), 64);

		if (!EnsureInstanceBuffer(device, static_cast<uint32_t>(n))) {
			finishTiming();
			return;
		}
		g_GpuInstanceScratch.resize(n);
		for (size_t k = 0; k < n; ++k) {
			WebGPUSpriteResources::EncodeInstance44(g_InstancesScratch[k], g_GpuInstanceScratch[k]);
			if (forceBlack) {
				g_GpuInstanceScratch[k].Color[0] = 0.0f;
				g_GpuInstanceScratch[k].Color[1] = 0.0f;
				g_GpuInstanceScratch[k].Color[2] = 0.0f;
				g_GpuInstanceScratch[k].Color[3] = 1.0f;
			}
		}
		queue.WriteBuffer(g_InstanceBuffer, 0,
			g_GpuInstanceScratch.data(),
			n * sizeof(SpriteInstance));

		wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
		if (!encoder) {
			finishTiming();
			return;
		}

		// Open the sprite render pass — Load semantics so a prior
		// RenderApi::Clear's result is preserved instead of double-cleared.
		wgpu::RenderPassColorAttachment colorAtt{};
		colorAtt.view       = target.ColorView;
		colorAtt.loadOp     = wgpu::LoadOp::Load;
		colorAtt.storeOp    = wgpu::StoreOp::Store;
		colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

		wgpu::RenderPassDepthStencilAttachment depthAtt{};
		if (target.HasDepth) {
			depthAtt.view              = target.DepthView;
			depthAtt.depthLoadOp       = wgpu::LoadOp::Load;
			depthAtt.depthStoreOp      = wgpu::StoreOp::Store;
			depthAtt.stencilLoadOp     = wgpu::LoadOp::Load;
			depthAtt.stencilStoreOp    = wgpu::StoreOp::Store;
		}

		wgpu::RenderPassDescriptor passDesc{};
		passDesc.label                  = "renderer2d-sprites";
		passDesc.colorAttachmentCount   = 1;
		passDesc.colorAttachments       = &colorAtt;
		passDesc.depthStencilAttachment = target.HasDepth ? &depthAtt : nullptr;

		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);

		// Set common state once. The pipeline carries vertex layout / blend /
		// primitive state — only what varies per batch (bind group +
		// firstInstance offset) gets re-set inside the loop.
		pass.SetPipeline(pipeline);
		pass.SetVertexBuffer(0, WebGPUSpriteResources::GetQuadVertexBuffer());
		pass.SetVertexBuffer(1, g_InstanceBuffer);
		pass.SetIndexBuffer(WebGPUSpriteResources::GetQuadIndexBuffer(pipelineMode),
			wgpu::IndexFormat::Uint16);

		// Group instances by resolved texture and issue one DrawIndexed
		// per run with firstInstance offsetting into the same instance
		// buffer — uses WebGPU's firstInstance parameter rather than
		// per-batch buffer rebinding.
		const TextureHandle defaultTexture = TextureManager::GetDefaultTexture(DefaultTexture::Square);
		auto resolveHandle = [&](TextureHandle h) {
			if (forceBlack) return defaultTexture;
			return TextureManager::IsValid(h) ? h : defaultTexture;
		};
		const std::uint32_t indexCount = WebGPUSpriteResources::GetQuadIndexCount(pipelineMode);

		size_t i = 0;
		while (i < n) {
			const TextureHandle runHandle = resolveHandle(g_TexturesScratch[i]);
			size_t runEnd = i + 1;
			while (runEnd < n
				&& resolveHandle(g_TexturesScratch[runEnd]).index == runHandle.index
				&& resolveHandle(g_TexturesScratch[runEnd]).generation == runHandle.generation)
			{
				++runEnd;
			}
			const uint32_t count = static_cast<uint32_t>(runEnd - i);

			wgpu::BindGroup bg = ResolveBindGroup(device, runHandle);
			if (!bg) {
				// Default texture also missing — engine-shutdown edge case,
				// or IndexAssets not on disk. Skip the run rather than
				// crash; warning is already logged by TextureManager.
				i = runEnd;
				continue;
			}
			pass.SetBindGroup(0, bg);
			pass.DrawIndexed(/*indexCount=*/indexCount,
				/*instanceCount=*/count,
				/*firstIndex=*/0,
				/*baseVertex=*/0,
				/*firstInstance=*/static_cast<uint32_t>(i));
			++m_DrawCallsCount;

			i = runEnd;
		}

		pass.End();

		if (target.IsSwapChain) {
			WebGPUBackend::MarkSwapChainRendered();
		}
		finishTiming();
	}

	void Renderer2D::RenderScenes() {
		// SceneProvider iteration scaffolding — a no-op until SceneProvider
		// is wired into the WebGPU renderer in a later stage.
	}

	void Renderer2D::CollectAndRenderInstances(Scene& /*scene*/,
		const glm::mat4& /*vp*/, const AABB& /*viewportAABB*/)
	{
	}

	uint32_t Renderer2D::RegisterInstanceContributor(InstanceContributor contributor) {
		if (!contributor) return 0;
		const uint32_t token = g_NextInstanceContributorToken++;
		if (g_NextInstanceContributorToken == 0) {
			g_NextInstanceContributorToken = 1;
		}
		g_InstanceContributors.emplace_back(token, std::move(contributor));
		return token;
	}

	void Renderer2D::UnregisterInstanceContributor(uint32_t token) {
		std::erase_if(g_InstanceContributors,
			[token](const auto& entry) { return entry.first == token; });
	}

}  // namespace Index
