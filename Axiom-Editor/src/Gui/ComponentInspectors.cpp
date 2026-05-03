#include <pch.hpp>
#include "Assets/AssetRegistry.hpp"
#include "Gui/ComponentInspectors.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Gui/ThumbnailCache.hpp"

#include "Components/Components.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Texture2D.hpp"
#include "Audio/AudioManager.hpp"
#include "Physics/PhysicsTypes.hpp"
#include <Project/ProjectManager.hpp>
#include <Project/AxiomProject.hpp>
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"

#include <imgui.h>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace Axiom {

	// ── Texture Picker State (for SpriteRenderer inspector) ─────────

	namespace {
		static TextureHandle LoadTextureFromAssetPath(const std::string& path, UUID* outAssetId = nullptr) {
			const uint64_t assetId = AssetRegistry::GetOrCreateAssetUUID(path);
			if (outAssetId) {
				*outAssetId = UUID(assetId);
			}

			if (assetId != 0 && AssetRegistry::IsTexture(assetId)) {
				TextureHandle handle = TextureManager::LoadTextureByUUID(assetId);
				if (handle.IsValid()) {
					return handle;
				}
			}

			return TextureManager::LoadTexture(path);
		}

		// Apply a sprite texture path to a single component instance.
		static void AssignSpriteTexture(SpriteRendererComponent& sprite, const std::string& path) {
			UUID assetId = UUID(0);
			sprite.TextureHandle = LoadTextureFromAssetPath(path, &assetId);
			sprite.TextureAssetId = assetId;
		}

		// Apply a sprite texture path to every entity in the selection.
		static void AssignSpriteTextureForAll(std::span<const Entity> entities, const std::string& path) {
			UUID sharedAssetId = UUID(0);
			TextureHandle sharedHandle = LoadTextureFromAssetPath(path, &sharedAssetId);
			for (const Entity& entity : entities) {
				if (!entity.HasComponent<SpriteRendererComponent>()) continue;
				auto& sprite = const_cast<Entity&>(entity).GetComponent<SpriteRendererComponent>();
				sprite.TextureHandle = sharedHandle;
				sprite.TextureAssetId = sharedAssetId;
			}
		}

		static void AssignAudioClip(AudioSourceComponent& audioSource, const std::string& path) {
			const uint64_t assetId = AssetRegistry::GetOrCreateAssetUUID(path);
			if (assetId != 0 && AssetRegistry::IsAudio(assetId)) {
				audioSource.SetAudioHandle(AudioManager::LoadAudioByUUID(assetId), UUID(assetId));
				return;
			}

			audioSource.SetAudioHandle(AudioManager::LoadAudio(path), UUID(0));
		}

		static void AssignAudioClipForAll(std::span<const Entity> entities, const std::string& path) {
			const uint64_t assetId = AssetRegistry::GetOrCreateAssetUUID(path);
			for (const Entity& entity : entities) {
				if (!entity.HasComponent<AudioSourceComponent>()) continue;
				auto& audioSource = const_cast<Entity&>(entity).GetComponent<AudioSourceComponent>();
				if (assetId != 0 && AssetRegistry::IsAudio(assetId)) {
					audioSource.SetAudioHandle(AudioManager::LoadAudioByUUID(assetId), UUID(assetId));
				}
				else {
					audioSource.SetAudioHandle(AudioManager::LoadAudio(path), UUID(0));
				}
			}
		}

		static void DrawPickerHeaderControls(const char* searchId, char* searchBuffer, std::size_t searchBufferSize, bool& isOpen) {
			const float closeButtonSize = ImGui::GetFrameHeight();
			const float inputWidth = std::max(ImGui::GetContentRegionAvail().x - closeButtonSize - ImGui::GetStyle().ItemSpacing.x, 1.0f);

			ImGui::SetNextItemWidth(inputWidth);
			ImGui::InputTextWithHint(searchId, "Search...", searchBuffer, searchBufferSize);
			ImGui::SameLine();
			if (ImGui::Button("X", ImVec2(closeButtonSize, closeButtonSize))) {
				isOpen = false;
			}
			ImGui::Separator();
		}

		static bool DrawAssetSelectionField(const char* label, const std::string& displayName, const std::string& tooltip = {}) {
			ImGui::PushID(label);
			ImGuiUtils::BeginInspectorFieldRow(label);
			const std::string resolvedDisplayName = displayName.empty() ? std::string("(None)") : displayName;
			const float fieldWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
			bool truncated = false;
			const std::string buttonText = ImGuiUtils::Ellipsize(
				resolvedDisplayName,
				fieldWidth - ImGui::GetStyle().FramePadding.x * 2.0f,
				&truncated);

			const bool clicked = ImGui::Button((buttonText + "##Value").c_str(), ImVec2(fieldWidth, 0.0f));
			if (ImGui::IsItemHovered() && (truncated || !tooltip.empty())) {
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(resolvedDisplayName.c_str());
				if (!tooltip.empty()) {
					ImGui::TextDisabled("%s", tooltip.c_str());
				}
				ImGui::EndTooltip();
			}

			ImGui::PopID();
			return clicked;
		}

		struct TexturePickerEntry {
			std::string RelativePath;
			std::string DisplayName;
			std::string FullPath;
			uint64_t AssetId = 0;
		};

		static bool s_TexturePickerOpen = false;
		static char s_TexturePickerSearch[128] = {};
		static std::vector<TexturePickerEntry> s_TexturePickerEntries;
		static ThumbnailCache s_TexturePickerThumbnails;
		static bool s_TexturePickerThumbnailCacheInitialized = false;
		static std::unordered_set<std::string> s_TexturePickerLoadedPaths;
		// Capture every selected entity so picking applies to all of them, not
		// just the most recently focused one.
		// Stored as full Entity (not bare handle) so the picker can write back to
		// whichever scene the inspector was rendering — the prefab inspector
		// uses a detached scene that isn't SceneManager::GetActiveScene().
		static std::vector<Entity> s_TexturePickerTargets;

		static bool IsImageExtension(const std::string& ext) {
			return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
				|| ext == ".bmp" || ext == ".tga";
		}

		static void CollectTextureFiles(const std::filesystem::path& dir, const std::filesystem::path& root,
			std::vector<TexturePickerEntry>& entries) {
			if (!std::filesystem::exists(dir)) return;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
				if (!entry.is_regular_file()) continue;
				std::string ext = entry.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (!IsImageExtension(ext)) continue;

				std::string rel = std::filesystem::relative(entry.path(), root).string();
				std::string name = entry.path().filename().string();
				entries.push_back({ rel, name, entry.path().string(), AssetRegistry::GetOrCreateAssetUUID(entry.path().string()) });
			}
			std::sort(entries.begin(), entries.end(),
				[](const TexturePickerEntry& a, const TexturePickerEntry& b) {
					if (a.DisplayName == b.DisplayName) {
						return a.RelativePath < b.RelativePath;
					}
					return a.DisplayName < b.DisplayName;
				});
		}

		static void EnsureTexturePickerThumbnailCache() {
			if (!s_TexturePickerThumbnailCacheInitialized) {
				s_TexturePickerThumbnails.Initialize();
				s_TexturePickerThumbnailCacheInitialized = true;
			}
		}

		static void ClearTexturePickerThumbnailCache() {
			if (s_TexturePickerThumbnailCacheInitialized) {
				s_TexturePickerThumbnails.Clear();
			}
			s_TexturePickerLoadedPaths.clear();
		}

		static void ApplyTexturePickerSelection(const TexturePickerEntry* entry) {
			TextureHandle sharedHandle;
			UUID sharedAssetId = UUID(0);
			if (entry) {
				if (entry->AssetId != 0 && AssetRegistry::IsTexture(entry->AssetId)) {
					sharedHandle = TextureManager::LoadTextureByUUID(entry->AssetId);
					sharedAssetId = UUID(entry->AssetId);
				}
				else {
					sharedHandle = TextureManager::LoadTexture(entry->FullPath);
					sharedAssetId = UUID(0);
				}
			}

			// Each target carries its own Scene*, so this works for the active
			// scene AND for the prefab inspector's detached scene.
			for (const Entity& target : s_TexturePickerTargets) {
				Scene* targetScene = const_cast<Entity&>(target).GetScene();
				if (!targetScene) continue;
				const EntityHandle handle = target.GetHandle();
				if (!targetScene->IsValid(handle) || !targetScene->HasComponent<SpriteRendererComponent>(handle)) continue;
				auto& sprite = targetScene->GetComponent<SpriteRendererComponent>(handle);
				sprite.TextureHandle = sharedHandle;
				sprite.TextureAssetId = sharedAssetId;
				targetScene->MarkDirty();
			}

			s_TexturePickerOpen = false;
			s_TexturePickerTargets.clear();
		}

		static void OpenTexturePicker(std::span<const Entity> entities) {
			s_TexturePickerOpen = true;
			s_TexturePickerSearch[0] = '\0';
			s_TexturePickerTargets.clear();
			s_TexturePickerTargets.reserve(entities.size());
			for (const Entity& entity : entities) {
				s_TexturePickerTargets.push_back(entity);
			}
			s_TexturePickerEntries.clear();
			EnsureTexturePickerThumbnailCache();
			ClearTexturePickerThumbnailCache();
			// Force the asset registry to re-scan before we collect — the
			// picker scans the filesystem directly via CollectTextureFiles,
			// but GetOrCreateAssetUUID below routes through AssetRegistry
			// which would otherwise return 0 for assets it hasn't indexed
			// yet (a brand-new texture file wouldn't drag into the picker
			// with a usable AssetId).
			AssetRegistry::MarkDirty();
			AssetRegistry::Sync();

			// Collect from AxiomAssets/Textures (engine) and Assets/Textures (user project)
			std::string axiomTexDir = Path::ResolveAxiomAssets("Textures");
			if (!axiomTexDir.empty()) {
				auto axiomTexPath = std::filesystem::path(axiomTexDir);
				CollectTextureFiles(axiomTexPath, axiomTexPath, s_TexturePickerEntries);
			}
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				CollectTextureFiles(project->AssetsDirectory, project->AssetsDirectory, s_TexturePickerEntries);
			}
			else {
				auto userAssetsDir = std::filesystem::path(Path::ExecutableDir()) / "Assets";
				CollectTextureFiles(userAssetsDir, userAssetsDir, s_TexturePickerEntries);
			}
		}

		static void RenderTexturePicker() {
			if (!s_TexturePickerOpen) {
				ClearTexturePickerThumbnailCache();
				return;
			}

			EnsureTexturePickerThumbnailCache();

			ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("Select Texture", &s_TexturePickerOpen)) {
				ImGui::End();
				if (!s_TexturePickerOpen) {
					ClearTexturePickerThumbnailCache();
				}
				return;
			}

			DrawPickerHeaderControls("##TexSearch", s_TexturePickerSearch, sizeof(s_TexturePickerSearch), s_TexturePickerOpen);

			ImGui::BeginChild("##TexList");

			if (ImGui::Selectable("(None)", false)) {
				ApplyTexturePickerSelection(nullptr);
			}

			std::string filter(s_TexturePickerSearch);
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			std::vector<const TexturePickerEntry*> filteredEntries;
			filteredEntries.reserve(s_TexturePickerEntries.size());
			for (const auto& entry : s_TexturePickerEntries) {
				if (!filter.empty()) {
					std::string lowerName = entry.DisplayName;
					std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
					std::string lowerPath = entry.RelativePath;
					std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
					if (lowerName.find(filter) == std::string::npos && lowerPath.find(filter) == std::string::npos) continue;
				}
				filteredEntries.push_back(&entry);
			}

			if (filteredEntries.empty()) {
				ImGui::TextDisabled("No matching textures");
			}
			else {
				const float thumbnailSize = 48.0f;
				const float rowPadding = 6.0f;
				const float lineHeight = ImGui::GetTextLineHeight();
				const float rowHeight = std::max(thumbnailSize + rowPadding * 2.0f, lineHeight * 2.0f + rowPadding * 2.0f + 2.0f);
				std::unordered_set<std::string> visiblePaths;
				ImDrawList* drawList = ImGui::GetWindowDrawList();

				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(filteredEntries.size()), rowHeight);
				while (clipper.Step()) {
					for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
						const TexturePickerEntry& entry = *filteredEntries[static_cast<std::size_t>(index)];
						visiblePaths.insert(entry.FullPath);
						s_TexturePickerLoadedPaths.insert(entry.FullPath);

						ImGui::PushID(entry.RelativePath.c_str());

						const float rowWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
						const ImVec2 rowMin = ImGui::GetCursorScreenPos();
						const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);

						ImGui::InvisibleButton("##TextureRow", ImVec2(rowWidth, rowHeight));
						const bool hovered = ImGui::IsItemHovered();
						if (hovered) {
							drawList->AddRectFilled(rowMin, rowMax, IM_COL32(70, 78, 92, 120), 4.0f);
						}
						if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
							ApplyTexturePickerSelection(&entry);
						}

						const ImVec2 thumbMin(rowMin.x + rowPadding, rowMin.y + rowPadding);
						const ImVec2 thumbMax(thumbMin.x + thumbnailSize, thumbMin.y + thumbnailSize);
						drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(35, 35, 35, 255), 4.0f);

						const unsigned int thumbnail = s_TexturePickerThumbnails.GetThumbnail(entry.FullPath);
						Texture2D* thumbnailTexture = s_TexturePickerThumbnails.GetCacheEntry(entry.FullPath);
						if (thumbnail != 0 && thumbnailTexture && thumbnailTexture->IsValid()) {
							float drawWidth = thumbnailSize;
							float drawHeight = thumbnailSize;
							const float texWidth = thumbnailTexture->GetWidth();
							const float texHeight = thumbnailTexture->GetHeight();
							if (texWidth > 0.0f && texHeight > 0.0f) {
								const float aspect = texWidth / texHeight;
								if (aspect > 1.0f) {
									drawHeight = thumbnailSize / aspect;
								}
								else {
									drawWidth = thumbnailSize * aspect;
								}
							}

							const ImVec2 imageMin(
								thumbMin.x + (thumbnailSize - drawWidth) * 0.5f,
								thumbMin.y + (thumbnailSize - drawHeight) * 0.5f);
							const ImVec2 imageMax(imageMin.x + drawWidth, imageMin.y + drawHeight);
							drawList->AddImage(
								static_cast<ImTextureID>(static_cast<intptr_t>(thumbnail)),
								imageMin,
								imageMax,
								ImVec2(0.0f, 1.0f),
								ImVec2(1.0f, 0.0f));
						}
						else {
							ThumbnailCache::DrawAssetIcon(AssetType::Image, thumbMin, thumbnailSize);
						}

						const float textX = thumbMax.x + rowPadding * 1.5f;
						const float textWidth = std::max(rowMax.x - textX - rowPadding, 1.0f);
						bool nameTruncated = false;
						bool pathTruncated = false;
						const std::string displayName = ImGuiUtils::Ellipsize(entry.DisplayName, textWidth, &nameTruncated);
						const std::string displayPath = ImGuiUtils::Ellipsize(entry.RelativePath, textWidth, &pathTruncated);
						drawList->AddText(ImVec2(textX, rowMin.y + rowPadding), ImGui::GetColorU32(ImGuiCol_Text), displayName.c_str());
						drawList->AddText(ImVec2(textX, rowMin.y + rowPadding + lineHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled), displayPath.c_str());

						if (hovered && (nameTruncated || pathTruncated)) {
							ImGui::SetTooltip("%s", entry.RelativePath.c_str());
						}

						ImGui::PopID();
					}
				}

				for (auto it = s_TexturePickerLoadedPaths.begin(); it != s_TexturePickerLoadedPaths.end();) {
					if (visiblePaths.find(*it) == visiblePaths.end()) {
						s_TexturePickerThumbnails.Invalidate(*it);
						it = s_TexturePickerLoadedPaths.erase(it);
					}
					else {
						++it;
					}
				}
			}

			ImGui::EndChild();
			ImGui::End();
			if (!s_TexturePickerOpen) {
				ClearTexturePickerThumbnailCache();
			}
		}
	}

	void DrawNameComponentInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::InputTextMulti("##NameValue", entities,
			[](const Entity& e) -> std::string {
				return e.GetComponent<NameComponent>().Name;
			},
			[](const Entity& e, const std::string& v) {
				const_cast<Entity&>(e).GetComponent<NameComponent>().Name = v;
			});
	}

	void DrawTransform2DInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::DragFloatNMulti("Position", entities, 2,
			[](const Entity& e, int c) -> float {
				const auto& t = e.GetComponent<Transform2DComponent>();
				return c == 0 ? t.Position.x : t.Position.y;
			},
			[](const Entity& e, int c, float v) {
				auto& t = const_cast<Entity&>(e).GetComponent<Transform2DComponent>();
				if (c == 0) t.Position.x = v;
				else        t.Position.y = v;
			},
			0.05f);

		ImGuiUtils::DragFloatNMulti("Scale", entities, 2,
			[](const Entity& e, int c) -> float {
				const auto& t = e.GetComponent<Transform2DComponent>();
				return c == 0 ? t.Scale.x : t.Scale.y;
			},
			[](const Entity& e, int c, float v) {
				auto& t = const_cast<Entity&>(e).GetComponent<Transform2DComponent>();
				if (c == 0) t.Scale.x = v;
				else        t.Scale.y = v;
			},
			0.05f, 0.001f, 0.0f);

		ImGuiUtils::DragFloatMulti("Rotation", entities,
			[](const Entity& e) -> float {
				return e.GetComponent<Transform2DComponent>().Rotation;
			},
			[](const Entity& e, float v) {
				const_cast<Entity&>(e).GetComponent<Transform2DComponent>().Rotation = v;
			},
			0.01f);
	}

	void DrawRigidbody2DInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::DragFloatNMulti("Position", entities, 2,
			[](const Entity& e, int c) -> float {
				const Vec2 p = e.GetComponent<Rigidbody2DComponent>().GetPosition();
				return c == 0 ? p.x : p.y;
			},
			[](const Entity& e, int c, float v) {
				auto& rb = const_cast<Entity&>(e).GetComponent<Rigidbody2DComponent>();
				Vec2 p = rb.GetPosition();
				if (c == 0) p.x = v; else p.y = v;
				rb.SetPosition(p);
			},
			0.05f);

		ImGuiUtils::DragFloatNMulti("Velocity", entities, 2,
			[](const Entity& e, int c) -> float {
				const Vec2 v = e.GetComponent<Rigidbody2DComponent>().GetVelocity();
				return c == 0 ? v.x : v.y;
			},
			[](const Entity& e, int c, float v) {
				auto& rb = const_cast<Entity&>(e).GetComponent<Rigidbody2DComponent>();
				Vec2 cur = rb.GetVelocity();
				if (c == 0) cur.x = v; else cur.y = v;
				rb.SetVelocity(cur);
			},
			0.05f);

		ImGuiUtils::DragFloatMulti("Rotation", entities,
			[](const Entity& e) -> float {
				return e.GetComponent<Rigidbody2DComponent>().GetRotation();
			},
			[](const Entity& e, float v) {
				const_cast<Entity&>(e).GetComponent<Rigidbody2DComponent>().SetRotation(v);
			},
			0.01f);

		ImGuiUtils::SliderFloatMulti("Gravity Scale", entities,
			[](const Entity& e) -> float {
				return e.GetComponent<Rigidbody2DComponent>().GetGravityScale();
			},
			[](const Entity& e, float v) {
				const_cast<Entity&>(e).GetComponent<Rigidbody2DComponent>().SetGravityScale(v);
			},
			0.0f, 1.0f);

		ImGuiUtils::EnumComboMulti<BodyType>("Body Type", entities,
			[](const Entity& e) -> BodyType {
				return const_cast<Entity&>(e).GetComponent<Rigidbody2DComponent>().GetBodyType();
			},
			[](const Entity& e, BodyType v) {
				const_cast<Entity&>(e).GetComponent<Rigidbody2DComponent>().SetBodyType(v);
			});
	}

	void DrawSpriteRendererInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::ColorEdit4Multi("Color", entities,
			[](const Entity& e, int c) -> float {
				const Color& col = e.GetComponent<SpriteRendererComponent>().Color;
				return (&col.r)[c];
			},
			[](const Entity& e, int c, float v) {
				Color& col = const_cast<Entity&>(e).GetComponent<SpriteRendererComponent>().Color;
				(&col.r)[c] = v;
			},
			ImGuiColorEditFlags_NoInputs);

		ImGuiUtils::DragScalarMulti<int16_t>("Sorting Order", entities, ImGuiDataType_S16,
			[](const Entity& e) -> int16_t {
				return e.GetComponent<SpriteRendererComponent>().SortingOrder;
			},
			[](const Entity& e, int16_t v) {
				const_cast<Entity&>(e).GetComponent<SpriteRendererComponent>().SortingOrder = v;
			},
			1.0f);

		ImGuiUtils::DragScalarMulti<uint8_t>("Sorting Layer", entities, ImGuiDataType_U8,
			[](const Entity& e) -> uint8_t {
				return e.GetComponent<SpriteRendererComponent>().SortingLayer;
			},
			[](const Entity& e, uint8_t v) {
				const_cast<Entity&>(e).GetComponent<SpriteRendererComponent>().SortingLayer = v;
			},
			1.0f);

		// Texture: only show a uniform display name when every selected entity
		// has the same texture handle; otherwise show "—". Picker-driven edits
		// always apply to the entire selection.
		bool textureUniform = true;
		TextureHandle firstHandle;
		if (!entities.empty()) firstHandle = entities[0].GetComponent<SpriteRendererComponent>().TextureHandle;
		for (std::size_t i = 1; i < entities.size(); ++i) {
			const TextureHandle h = entities[i].GetComponent<SpriteRendererComponent>().TextureHandle;
			if (h != firstHandle) { textureUniform = false; break; }
		}

		std::string textureDisplayName;
		std::string textureTooltip;
		if (!textureUniform) {
			textureDisplayName = "-";
		}
		else if (firstHandle.IsValid()) {
			textureTooltip = TextureManager::GetTextureName(firstHandle);
			if (!textureTooltip.empty()) {
				textureDisplayName = std::filesystem::path(textureTooltip).filename().string();
			}
			else {
				textureDisplayName = "(None)";
			}
		}
		else {
			textureDisplayName = "(None)";
		}

		if (DrawAssetSelectionField("Texture", textureDisplayName, textureTooltip)) {
			OpenTexturePicker(entities);
		}

		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::string ext = std::filesystem::path(droppedPath).extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
					AssignSpriteTextureForAll(entities, droppedPath);
				}
			}
			ImGui::EndDragDropTarget();
		}

		RenderTexturePicker();

		// Texture preview, filter/wrap controls — only meaningful when a single
		// shared texture is selected. Otherwise show a muted hint.
		if (textureUniform && firstHandle.IsValid()) {
			if (Texture2D* texture = TextureManager::GetTexture(firstHandle)) {
				const float texWidth = texture->GetWidth();
				const float texHeight = texture->GetHeight();

				ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texWidth, texHeight);
				ImGui::Text("%.0f x %.0f", texWidth, texHeight);

				ImGui::PushID("TextureSettings");
				ImGuiUtils::DrawEnumCombo<Filter>("Filter", texture->GetFilter(), [&texture](Filter newFilter) {
					texture->SetFilter(newFilter);
					});
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap U", texture->GetWrapU(), [&texture](Wrap wrapU) {
					texture->SetWrapU(wrapU);
					});
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap V", texture->GetWrapV(), [&texture](Wrap wrapV) {
					texture->SetWrapV(wrapV);
					});
				ImGui::PopID();
			}
		}
		else if (!textureUniform) {
			ImGui::TextDisabled("Mixed texture — pick to apply to all");
		}
	}

	void DrawCamera2DInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::DragFloatMulti("Zoom", entities,
			[](const Entity& e) -> float { return e.GetComponent<Camera2DComponent>().GetZoom(); },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<Camera2DComponent>().SetZoom(v); },
			0.01f, 0.01f, 100.0f);

		ImGuiUtils::DragFloatMulti("Orthographic Size", entities,
			[](const Entity& e) -> float { return e.GetComponent<Camera2DComponent>().GetOrthographicSize(); },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<Camera2DComponent>().SetOrthographicSize(v); },
			0.05f, 0.05f, 1000.0f);

		ImGuiUtils::ColorEdit4Multi("Clear Color", entities,
			[](const Entity& e, int c) -> float {
				const Color& col = e.GetComponent<Camera2DComponent>().GetClearColor();
				return (&col.r)[c];
			},
			[](const Entity& e, int c, float v) {
				auto& cam = const_cast<Entity&>(e).GetComponent<Camera2DComponent>();
				Color col = cam.GetClearColor();
				(&col.r)[c] = v;
				cam.SetClearColor(col);
			});

		// Read-only viewports: only meaningful when a single camera is selected;
		// otherwise show muted hints.
		if (entities.size() == 1) {
			const auto& camera = entities[0].GetComponent<Camera2DComponent>();
			ImGuiUtils::DrawVec2ReadOnly("Viewport Size", camera.GetViewport()->GetSize());
			ImGuiUtils::DrawVec2ReadOnly("World Viewport", camera.WorldViewPort());
		}
		else {
			ImGuiUtils::BeginInspectorFieldRow("Viewport Size");
			ImGui::TextDisabled("-  (multiple cameras)");
			ImGuiUtils::BeginInspectorFieldRow("World Viewport");
			ImGui::TextDisabled("-  (multiple cameras)");
		}
	}

	void DrawParticleSystem2DInspector(std::span<const Entity> entities)
	{
		// "Play / Pause" toggle: drive every selected particle system the same
		// way. We base the button label on the "majority" play state — first
		// entity wins in mixed cases, which matches the existing single-edit
		// behavior when applied to all.
		bool firstPlaying = false;
		bool playUniform = true;
		if (!entities.empty()) {
			firstPlaying = entities[0].GetComponent<ParticleSystem2DComponent>().IsPlaying();
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (entities[i].GetComponent<ParticleSystem2DComponent>().IsPlaying() != firstPlaying) {
					playUniform = false;
					break;
				}
			}
		}
		const std::string playbackLabel = (playUniform ? (firstPlaying ? "Pause" : "Play") : "Play / Pause") + std::string("##Value");
		if (ImGuiUtils::DrawInspectorControl("Playback", [&playbackLabel](const char*) {
			return ImGui::Button(playbackLabel.c_str());
		})) {
			for (const Entity& e : entities) {
				auto& ps = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>();
				if (ps.IsPlaying()) ps.Pause();
				else                ps.Play();
			}
		}

		ImGuiUtils::CheckboxMulti("Play On Awake", entities,
			[](const Entity& e) -> bool { return e.GetComponent<ParticleSystem2DComponent>().PlayOnAwake; },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().PlayOnAwake = v; });

		ImGuiUtils::InputFloatMulti("LifeTime", entities,
			[](const Entity& e) -> float { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.LifeTime; },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.LifeTime = v; });

		ImGuiUtils::InputFloatMulti("Scale", entities,
			[](const Entity& e) -> float { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.Scale; },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.Scale = v; });

		ImGuiUtils::InputFloatMulti("Speed", entities,
			[](const Entity& e) -> float { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.Speed; },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.Speed = v; });

		ImGuiUtils::CheckboxMulti("Gravity", entities,
			[](const Entity& e) -> bool { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity; },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity = v; });

		// Gravity Value: enabled only when all entities have UseGravity set.
		bool gravityEnabledUniform = true;
		bool gravityEnabledFirst = false;
		if (!entities.empty()) {
			gravityEnabledFirst = entities[0].GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity;
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (entities[i].GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity != gravityEnabledFirst) {
					gravityEnabledUniform = false;
					break;
				}
			}
		}
		ImGuiUtils::DrawEnabled(gravityEnabledUniform && gravityEnabledFirst, [&]() {
			ImGuiUtils::DragFloatNMulti("Gravity Value", entities, 2,
				[](const Entity& e, int c) -> float {
					const Vec2& g = e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.Gravity;
					return c == 0 ? g.x : g.y;
				},
				[](const Entity& e, int c, float v) {
					Vec2& g = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.Gravity;
					if (c == 0) g.x = v; else g.y = v;
				});
		});

		ImGuiUtils::CheckboxMulti("Random Colors", entities,
			[](const Entity& e) -> bool { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseRandomColors; },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseRandomColors = v; });

		if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::PushID("Emission");
			ImGuiUtils::InputIntMulti("Emit Over Time", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().EmissionSettings.EmitOverTime); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().EmissionSettings.EmitOverTime = static_cast<uint16_t>(std::clamp(v, 0, 65535)); });

			// Shape type combo. If selections have different alternatives,
			// switching writes that alternative to all (defaults match the
			// existing single-edit branch).
			using ShapeType = ParticleSystem2DComponent::ShapeType;
			auto shapeOf = [](const Entity& e) -> ShapeType {
				return std::visit([](auto&& s) -> ShapeType {
					using T = std::decay_t<decltype(s)>;
					if constexpr (std::is_same_v<T, ParticleSystem2DComponent::CircleParams>) return ShapeType::Circle;
					else                                                                       return ShapeType::Square;
					}, e.GetComponent<ParticleSystem2DComponent>().Shape);
			};
			ImGuiUtils::EnumComboMulti<ShapeType>("Shape Type", entities,
				shapeOf,
				[](const Entity& e, ShapeType v) {
					auto& ps = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>();
					if (v == ShapeType::Circle) ps.Shape = ParticleSystem2DComponent::CircleParams{ 1.f, false };
					else                        ps.Shape = ParticleSystem2DComponent::SquareParams{ Vec2{ 1.f, 1.f } };
				});

			// Shape parameters: only render the per-alternative fields when all
			// selected entities currently use the same alternative — otherwise
			// editing them is ambiguous.
			ShapeType firstShape = entities.empty() ? ShapeType::Circle : shapeOf(entities[0]);
			bool shapeUniform = true;
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (shapeOf(entities[i]) != firstShape) { shapeUniform = false; break; }
			}
			if (!shapeUniform) {
				ImGui::TextDisabled("Mixed shape — pick one to apply to all");
			}
			else if (firstShape == ShapeType::Circle) {
				ImGuiUtils::InputFloatMulti("Radius", entities,
					[](const Entity& e) -> float { return std::get<ParticleSystem2DComponent::CircleParams>(e.GetComponent<ParticleSystem2DComponent>().Shape).Radius; },
					[](const Entity& e, float v) { std::get<ParticleSystem2DComponent::CircleParams>(const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().Shape).Radius = v; });
				ImGuiUtils::CheckboxMulti("On Circle Edge", entities,
					[](const Entity& e) -> bool { return std::get<ParticleSystem2DComponent::CircleParams>(e.GetComponent<ParticleSystem2DComponent>().Shape).IsOnCircle; },
					[](const Entity& e, bool v) { std::get<ParticleSystem2DComponent::CircleParams>(const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().Shape).IsOnCircle = v; });
			}
			else {
				ImGuiUtils::DragFloatNMulti("Half Extents", entities, 2,
					[](const Entity& e, int c) -> float {
						const Vec2& v = std::get<ParticleSystem2DComponent::SquareParams>(e.GetComponent<ParticleSystem2DComponent>().Shape).HalfExtends;
						return c == 0 ? v.x : v.y;
					},
					[](const Entity& e, int c, float v) {
						Vec2& he = std::get<ParticleSystem2DComponent::SquareParams>(const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().Shape).HalfExtends;
						if (c == 0) he.x = v; else he.y = v;
					});
			}
			ImGui::PopID();
		}

		if (ImGui::CollapsingHeader("Rendering")) {
			ImGui::PushID("Rendering");
			ImGuiUtils::ColorEdit4Multi("Color", entities,
				[](const Entity& e, int c) -> float {
					const Color& col = e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.Color;
					return (&col.r)[c];
				},
				[](const Entity& e, int c, float v) {
					Color& col = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.Color;
					(&col.r)[c] = v;
				});

			ImGuiUtils::DragIntMulti("Max Particles", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.MaxParticles); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.MaxParticles = static_cast<uint32_t>(std::max(1, v)); },
				1.0f, 1, 10000);

			ImGuiUtils::InputIntMulti("Sorting Order", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingOrder); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingOrder = static_cast<int16_t>(v); });

			ImGuiUtils::InputIntMulti("Sorting Layer", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingLayer); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingLayer = static_cast<uint8_t>(std::clamp(v, 0, 255)); });

			// Texture preview: only meaningful for a single selection.
			if (entities.size() == 1) {
				const auto& ps = entities[0].GetComponent<ParticleSystem2DComponent>();
				TextureHandle previewHandle = ps.GetTextureHandle();
				if (!previewHandle.IsValid()) {
					previewHandle = TextureManager::GetDefaultTexture(DefaultTexture::Square);
				}
				if (Texture2D* texture = TextureManager::GetTexture(previewHandle)) {
					ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texture->GetWidth(), texture->GetHeight());
				}
			}
			ImGui::PopID();
		}
	}

	void DrawBoxCollider2DInspector(std::span<const Entity> entities)
	{
		Scene* scene = SceneManager::Get().GetActiveScene();
		if (!scene) {
			ImGui::TextDisabled("No active scene");
			return;
		}

		// Verify each selected entity has a Transform2D (collider depends on it
		// for world-scale math). If any entity is missing it, show the existing
		// hint and bail.
		for (const Entity& entity : entities) {
			if (!scene->IsValid(entity.GetHandle()) || !scene->HasComponent<Transform2DComponent>(entity.GetHandle())) {
				ImGui::TextDisabled("Box collider requires a valid Transform 2D");
				return;
			}
		}

		ImGuiUtils::DragFloatNMulti("Offset", entities, 2,
			[](const Entity& e, int c) -> float {
				const Vec2 v = e.GetComponent<BoxCollider2DComponent>().GetCenter();
				return c == 0 ? v.x : v.y;
			},
			[scene](const Entity& e, int c, float v) {
				auto& col = const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>();
				Vec2 cur = col.GetCenter();
				if (c == 0) cur.x = v; else cur.y = v;
				col.SetCenter(cur, *scene);
			},
			0.05f);

		// Size (world): per-channel write convertes back through the entity's
		// transform scale to a local-size, matching the existing single-edit path.
		ImGuiUtils::DragFloatNMulti("Size", entities, 2,
			[](const Entity& e, int c) -> float {
				const Vec2 v = e.GetComponent<BoxCollider2DComponent>().GetScale();
				return c == 0 ? v.x : v.y;
			},
			[scene](const Entity& e, int c, float v) {
				auto& col = const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>();
				const auto& transform = scene->GetComponent<Transform2DComponent>(e.GetHandle());
				Vec2 worldSize = col.GetScale();
				Vec2 localSize = col.GetLocalScale(*scene);
				const float clamped = std::max(v, 0.001f);
				if (c == 0) {
					worldSize.x = clamped;
					if (std::fabs(transform.Scale.x) > 0.0001f) localSize.x = clamped / transform.Scale.x;
				}
				else {
					worldSize.y = clamped;
					if (std::fabs(transform.Scale.y) > 0.0001f) localSize.y = clamped / transform.Scale.y;
				}
				col.SetScale(localSize, *scene);
			},
			0.05f, 0.001f, 1000.0f);

		ImGuiUtils::CheckboxMulti("Sensor", entities,
			[](const Entity& e) -> bool { return e.GetComponent<BoxCollider2DComponent>().IsSensor(); },
			[scene](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>().SetSensor(v, *scene); });

		ImGuiUtils::CheckboxMulti("Contact Events", entities,
			[](const Entity& e) -> bool { return e.GetComponent<BoxCollider2DComponent>().CanRegisterContacts(); },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>().SetRegisterContacts(v); });

		ImGuiUtils::CheckboxMulti("Collider Enabled", entities,
			[](const Entity& e) -> bool { return e.GetComponent<BoxCollider2DComponent>().IsEnabled(); },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>().SetEnabled(v); });

		ImGuiUtils::DragFloatMulti("Friction", entities,
			[](const Entity& e) -> float { return e.GetComponent<BoxCollider2DComponent>().GetFriction(); },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>().SetFriction(v); },
			0.01f, 0.0f, 10.0f);

		ImGuiUtils::DragFloatMulti("Bounciness", entities,
			[](const Entity& e) -> float { return e.GetComponent<BoxCollider2DComponent>().GetBounciness(); },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>().SetBounciness(v); },
			0.01f, 0.0f, 1.0f);

		ImGuiUtils::InputScalarMulti<uint64_t>("Layer Mask", entities, ImGuiDataType_U64,
			[](const Entity& e) -> uint64_t { return e.GetComponent<BoxCollider2DComponent>().GetLayer(); },
			[](const Entity& e, uint64_t v) { const_cast<Entity&>(e).GetComponent<BoxCollider2DComponent>().SetLayer(v); });

		// Local Size readout. Per-channel mixed when transforms or sizes differ.
		float localValues[2] = {};
		std::array<bool, 2> localMixed{};
		ImGuiUtils::MultiEdit::SamplePerChannel<2>(entities,
			[scene](const Entity& e, std::size_t c) -> float {
				const Vec2 v = e.GetComponent<BoxCollider2DComponent>().GetLocalScale(*scene);
				return c == 0 ? v.x : v.y;
			},
			localValues, localMixed);
		ImGui::PushID("LocalSize");
		ImGuiUtils::BeginInspectorFieldRow("Local Size");
		ImGuiUtils::MultiEdit::MultiItemRow(2, [&](int c) {
			ImGui::PushID(c);
			ImGui::BeginDisabled();
			float v = localValues[c];
			ImGui::InputFloat("##c", &v, 0.0f, 0.0f, localMixed[c] ? "-" : "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::EndDisabled();
			ImGui::PopID();
			return false;
		});
		ImGui::PopID();
	}

	// ── Audio Picker (same pattern as Texture Picker) ──────────────

	namespace {
		struct AudioPickerEntry {
			std::string DisplayName;
			std::string RelativePath;
			std::string FullPath;
			uint64_t AssetId = 0;
		};

		static bool s_AudioPickerOpen = false;
		static char s_AudioPickerSearch[128] = {};
		static std::vector<AudioPickerEntry> s_AudioPickerEntries;
		// See s_TexturePickerTargets above — Entity (not EntityHandle) so the
		// picker writes back to whichever scene the inspector was rendering,
		// including the prefab inspector's detached scene.
		static std::vector<Entity> s_AudioPickerTargets;

		static void ApplyAudioPickerSelection(const AudioPickerEntry* entry) {
			AudioHandle sharedHandle;
			UUID sharedAssetId = UUID(0);
			if (entry) {
				if (entry->AssetId != 0 && AssetRegistry::IsAudio(entry->AssetId)) {
					sharedHandle = AudioManager::LoadAudioByUUID(entry->AssetId);
					sharedAssetId = UUID(entry->AssetId);
				}
				else {
					sharedHandle = AudioManager::LoadAudio(entry->FullPath);
					sharedAssetId = UUID(0);
				}
			}

			for (const Entity& target : s_AudioPickerTargets) {
				Scene* targetScene = const_cast<Entity&>(target).GetScene();
				if (!targetScene) continue;
				const EntityHandle handle = target.GetHandle();
				if (!targetScene->IsValid(handle) || !targetScene->HasComponent<AudioSourceComponent>(handle)) continue;
				auto& audioSource = targetScene->GetComponent<AudioSourceComponent>(handle);
				audioSource.SetAudioHandle(sharedHandle, sharedAssetId);
				targetScene->MarkDirty();
			}

			s_AudioPickerOpen = false;
			s_AudioPickerTargets.clear();
		}

		static void CollectAudioFiles(const std::string& dir, const std::string& rootDir,
			std::vector<AudioPickerEntry>& out)
		{
			if (!std::filesystem::exists(dir)) return;
			for (auto& entry : std::filesystem::recursive_directory_iterator(dir,
				std::filesystem::directory_options::skip_permission_denied))
			{
				if (!entry.is_regular_file()) continue;
				std::string ext = entry.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
					AudioPickerEntry e;
					e.DisplayName = entry.path().stem().string();
					e.FullPath = entry.path().string();
					e.RelativePath = std::filesystem::relative(entry.path(), rootDir).string();
					e.AssetId = AssetRegistry::GetOrCreateAssetUUID(entry.path().string());
					out.push_back(e);
				}
			}
			std::sort(out.begin(), out.end(), [](const AudioPickerEntry& a, const AudioPickerEntry& b) {
				if (a.DisplayName == b.DisplayName) {
					return a.RelativePath < b.RelativePath;
				}
				return a.DisplayName < b.DisplayName;
			});
		}

		static void OpenAudioPicker(std::span<const Entity> entities) {
			s_AudioPickerOpen = true;
			s_AudioPickerSearch[0] = '\0';
			s_AudioPickerTargets.clear();
			s_AudioPickerTargets.reserve(entities.size());
			for (const Entity& e : entities) {
				s_AudioPickerTargets.push_back(e);
			}
			s_AudioPickerEntries.clear();

			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (project) {
				CollectAudioFiles(project->AssetsDirectory, project->AssetsDirectory, s_AudioPickerEntries);
			}
		}

		static void RenderAudioPicker() {
			if (!s_AudioPickerOpen) return;

			ImGui::SetNextWindowSize(ImVec2(320, 380), ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("Select Audio", &s_AudioPickerOpen)) {
				ImGui::End();
				return;
			}

			DrawPickerHeaderControls("##AudioSearch", s_AudioPickerSearch, sizeof(s_AudioPickerSearch), s_AudioPickerOpen);

			std::string filter(s_AudioPickerSearch);
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			ImGui::BeginChild("AudioList");
			if (ImGui::Selectable("(None)", false)) {
				ApplyAudioPickerSelection(nullptr);
			}

			for (const auto& entry : s_AudioPickerEntries) {
				if (!filter.empty()) {
					std::string lower = entry.DisplayName;
					std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
					std::string lowerPath = entry.RelativePath;
					std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
					if (lower.find(filter) == std::string::npos && lowerPath.find(filter) == std::string::npos) continue;
				}

				bool truncated = false;
				const std::string label = ImGuiUtils::Ellipsize(entry.DisplayName, ImGui::GetContentRegionAvail().x, &truncated) + "##" + entry.RelativePath;
				if (ImGui::Selectable(label.c_str(), false)) {
					ApplyAudioPickerSelection(&entry);
				}
				if (ImGui::IsItemHovered() && (truncated || !entry.RelativePath.empty())) {
					ImGui::SetTooltip("%s", entry.RelativePath.c_str());
				}
			}
			ImGui::EndChild();
			ImGui::End();
		}
	}

	void DrawAudioSourceInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::CheckboxMulti("Play On Awake", entities,
			[](const Entity& e) -> bool { return e.GetComponent<AudioSourceComponent>().GetPlayOnAwake(); },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<AudioSourceComponent>().SetPlayOnAwake(v); });

		ImGuiUtils::SliderFloatMulti("Volume", entities,
			[](const Entity& e) -> float { return e.GetComponent<AudioSourceComponent>().GetVolume(); },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<AudioSourceComponent>().SetVolume(v); },
			0.0f, 1.0f);

		ImGuiUtils::DragFloatMulti("Pitch", entities,
			[](const Entity& e) -> float { return e.GetComponent<AudioSourceComponent>().GetPitch(); },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<AudioSourceComponent>().SetPitch(v); },
			0.01f, 0.1f, 3.0f);

		ImGuiUtils::CheckboxMulti("Loop", entities,
			[](const Entity& e) -> bool { return e.GetComponent<AudioSourceComponent>().IsLooping(); },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<AudioSourceComponent>().SetLoop(v); });

		ImGui::Spacing();

		// Audio Clip uniform check
		bool clipUniform = true;
		AudioHandle firstHandle;
		if (!entities.empty()) firstHandle = entities[0].GetComponent<AudioSourceComponent>().GetAudioHandle();
		for (std::size_t i = 1; i < entities.size(); ++i) {
			if (entities[i].GetComponent<AudioSourceComponent>().GetAudioHandle() != firstHandle) {
				clipUniform = false;
				break;
			}
		}

		std::string audioDisplayName;
		std::string audioPath;
		if (!clipUniform) {
			audioDisplayName = "-";
		}
		else if (firstHandle.IsValid()) {
			audioPath = AudioManager::GetAudioName(firstHandle);
			audioDisplayName = audioPath.empty()
				? std::string("(None)")
				: std::filesystem::path(audioPath).filename().string();
		}
		else {
			audioDisplayName = "(None)";
		}

		if (DrawAssetSelectionField("Clip", audioDisplayName, audioPath)) {
			OpenAudioPicker(entities);
		}

		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::string ext = std::filesystem::path(droppedPath).extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
					AssignAudioClipForAll(entities, droppedPath);
				}
			}
			ImGui::EndDragDropTarget();
		}

		RenderAudioPicker();
	}

	// ── Axiom-Physics Inspectors ─────────────────────────────────────

	void DrawFastBody2DInspector(std::span<const Entity> entities)
	{
		static const char* bodyTypeNames[] = { "Static", "Dynamic", "Kinematic" };
		ImGuiUtils::ComboMulti("Body Type", entities,
			[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<FastBody2DComponent>().Type); },
			[](const Entity& e, int v) {
				auto& body = const_cast<Entity&>(e).GetComponent<FastBody2DComponent>();
				body.Type = static_cast<AxiomPhys::BodyType>(v);
				if (body.m_Body) body.m_Body->SetBodyType(body.Type);
			},
			bodyTypeNames, 3);

		ImGuiUtils::DragFloatMulti("Mass", entities,
			[](const Entity& e) -> float { return e.GetComponent<FastBody2DComponent>().Mass; },
			[](const Entity& e, float v) {
				auto& body = const_cast<Entity&>(e).GetComponent<FastBody2DComponent>();
				body.Mass = v;
				if (body.m_Body) body.m_Body->SetMass(body.Mass);
			},
			0.1f, 0.001f, 10000.0f);

		ImGuiUtils::CheckboxMulti("Use Gravity", entities,
			[](const Entity& e) -> bool { return e.GetComponent<FastBody2DComponent>().UseGravity; },
			[](const Entity& e, bool v) {
				auto& body = const_cast<Entity&>(e).GetComponent<FastBody2DComponent>();
				body.UseGravity = v;
				if (body.m_Body) body.m_Body->SetGravityEnabled(body.UseGravity);
			});

		ImGuiUtils::CheckboxMulti("Boundary Check", entities,
			[](const Entity& e) -> bool { return e.GetComponent<FastBody2DComponent>().BoundaryCheck; },
			[](const Entity& e, bool v) {
				auto& body = const_cast<Entity&>(e).GetComponent<FastBody2DComponent>();
				body.BoundaryCheck = v;
				if (body.m_Body) body.m_Body->SetBoundaryCheckEnabled(body.BoundaryCheck);
			});

		// Runtime values: only meaningful when a single body is live and
		// selected (these are read each frame from the physics body).
		if (entities.size() == 1) {
			const auto& body = entities[0].GetComponent<FastBody2DComponent>();
			if (body.IsValid()) {
				ImGui::Separator();
				ImGui::TextDisabled("Runtime");
				Vec2 vel = body.GetVelocity();
				ImGuiUtils::DrawVec2ReadOnly("Velocity", vel);
				Vec2 pos = body.GetPosition();
				ImGuiUtils::DrawVec2ReadOnly("Position", pos);
			}
		}
	}

	void DrawFastBoxCollider2DInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::DragFloatNMulti("Half Extents", entities, 2,
			[](const Entity& e, int c) -> float {
				const Vec2& v = e.GetComponent<FastBoxCollider2DComponent>().HalfExtents;
				return c == 0 ? v.x : v.y;
			},
			[](const Entity& e, int c, float v) {
				auto& col = const_cast<Entity&>(e).GetComponent<FastBoxCollider2DComponent>();
				if (c == 0) col.HalfExtents.x = v; else col.HalfExtents.y = v;
				if (col.m_Collider) col.m_Collider->SetHalfExtents({ col.HalfExtents.x, col.HalfExtents.y });
			},
			0.05f, 0.01f, 100.0f);
	}

	void DrawFastCircleCollider2DInspector(std::span<const Entity> entities)
	{
		ImGuiUtils::DragFloatMulti("Radius", entities,
			[](const Entity& e) -> float { return e.GetComponent<FastCircleCollider2DComponent>().Radius; },
			[](const Entity& e, float v) {
				auto& col = const_cast<Entity&>(e).GetComponent<FastCircleCollider2DComponent>();
				col.Radius = v;
				if (col.m_Collider) col.m_Collider->SetRadius(col.Radius);
			},
			0.05f, 0.01f, 100.0f);
	}

}
