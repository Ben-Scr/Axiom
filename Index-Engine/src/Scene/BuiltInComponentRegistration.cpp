#include "pch.hpp"
#include "Scene/BuiltInComponentRegistration.hpp"

#include "Scene/ComponentRegistrationHelpers.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Audio/AudioManager.hpp"
#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/PostProcessing2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/TextureManager.hpp"
#include "Inspector/PropertyRegistration.hpp"
#include "Math/Trigonometry.hpp"
#include "Physics/PhysicsTypes.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"

#include <cmath>
#include <type_traits>
#include <variant>
#include <vector>

// Single source of truth for built-in components — names, categories, properties.
namespace Index {
	namespace {
		// File-scope helpers: can be called from non-capturing lambdas
		// that decay into the raw function pointers ComponentInfo's
		// serialize / deserialize slots require.
		Json::Value UIColorToJson(const Color& c) {
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("r", Json::Value(c.r));
			v.AddMember("g", Json::Value(c.g));
			v.AddMember("b", Json::Value(c.b));
			v.AddMember("a", Json::Value(c.a));
			return v;
		}
		Color UIColorFromJson(const Json::Value& v, const Color& fallback) {
			Color c = fallback;
			if (!v.IsObject()) return c;
			if (const Json::Value* x = v.FindMember("r")) c.r = static_cast<float>(x->AsDoubleOr(c.r));
			if (const Json::Value* x = v.FindMember("g")) c.g = static_cast<float>(x->AsDoubleOr(c.g));
			if (const Json::Value* x = v.FindMember("b")) c.b = static_cast<float>(x->AsDoubleOr(c.b));
			if (const Json::Value* x = v.FindMember("a")) c.a = static_cast<float>(x->AsDoubleOr(c.a));
			return c;
		}

		// UUIDs are 64 bits; encode as a string so JSON's double-backed
		// number type doesn't quantise away the low bits. Same approach
		// the SpriteRenderer / Image / Particle paths use in
		// SceneSerializer.cpp.
		Json::Value UIUuidToJson(UUID uuid) {
			return Json::Value(std::to_string(static_cast<uint64_t>(uuid)));
		}
		UUID UIUuidFromJson(const Json::Value& v, UUID fallback) {
			if (!v.IsString()) return fallback;
			const std::string s = v.AsStringOr("");
			if (s.empty()) return fallback;
			try { return UUID(std::stoull(s)); } catch (...) { return fallback; }
		}

		// Save/load an EntityHandle field as a persistent UUID — same
		// approach the editor's reference picker uses, so refs survive
		// scene reload (where runtime EntityHandle values are
		// reallocated). UUID 0 means "not set" both on disk and in RAM.
		Json::Value UIEntityHandleToJson(const Entity& owner, EntityHandle h) {
			uint64_t id = 0;
			if (h != entt::null) {
				if (const Scene* s = owner.GetScene()) {
					id = s->GetEntityPersistentID(h);
				}
			}
			return UIUuidToJson(UUID(id));
		}
		void UIEntityHandleFromJson(Entity& owner, const Json::Value& v, EntityHandle& outRef) {
			const UUID id = UIUuidFromJson(v, UUID(0));
			if (static_cast<uint64_t>(id) == 0) {
				outRef = entt::null;
				return;
			}
			Scene* s = owner.GetScene();
			if (!s) return;

			// Try to resolve right away. Back-refs (target appeared
			// earlier in the scene file) succeed here because their
			// UUIDComponent is already registered.
			EntityHandle resolved = entt::null;
			if (s->TryResolveEntityRef(static_cast<uint64_t>(id), resolved)) {
				outRef = resolved;
				return;
			}

			// Forward ref: the referenced entity hasn't been created
			// yet. Defer to the end of the load batch — by then every
			// entity has its UUIDComponent and the lookup succeeds.
			// Pointer-to-component-field is stable for the duration of
			// the load (entities aren't destroyed mid-deserialize), and
			// SceneSerializer drains the queue before returning.
			const uint64_t persistentId = static_cast<uint64_t>(id);
			EntityHandle* refSlot = &outRef;
			s->DeferEntityRefFixup([s, refSlot, persistentId]() {
				EntityHandle r = entt::null;
				if (s->TryResolveEntityRef(persistentId, r)) {
					*refSlot = r;
				}
			});
		}

		// RegisterComponent<T>, DeclareConflict<A,B>, DeclareDependency<TDependent,TDependency>
		// moved to Scene/ComponentRegistrationHelpers.hpp so the generated
		// Generated/CodegenComponents.cpp (user-defined C# components) can call
		// them via the same templates as the hand-written engine registrations.

		// Save/load an InspectorEventList alongside its host component.
		// Used by every UI widget that owns one or more event lists
		// (Button.OnClick, Slider.OnValueChanged, ...). Each row stores
		// the typed argument as a (kind, value) pair; older scene files
		// without those fields default to Void/empty so the binding
		// behaves identically to the original parameterless flow.
		Json::Value UIEventListToJson(const InspectorEventList& list) {
			Json::Value array = Json::Value::MakeArray();
			for (const InspectorEventBinding& b : list.Bindings) {
				Json::Value row = Json::Value::MakeObject();
				row.AddMember("entity",  UIUuidToJson(UUID(b.TargetEntityUUID)));
				row.AddMember("class",   Json::Value(b.ScriptClassName));
				row.AddMember("method",  Json::Value(b.MethodName));
				row.AddMember("enabled", Json::Value(b.Enabled));
				row.AddMember("argKind", Json::Value(static_cast<int>(b.ArgumentKind)));
				row.AddMember("argValue", Json::Value(b.ArgumentValue));
				array.Append(std::move(row));
			}
			return array;
		}
		void UIEventListFromJson(const Json::Value& v, InspectorEventList& outList) {
			outList.Bindings.clear();
			if (!v.IsArray()) return;
			for (const Json::Value& row : v.GetArray()) {
				if (!row.IsObject()) continue;
				InspectorEventBinding b;
				if (const Json::Value* x = row.FindMember("entity"))
					b.TargetEntityUUID = static_cast<uint64_t>(UIUuidFromJson(*x, UUID(0)));
				if (const Json::Value* x = row.FindMember("class"))    b.ScriptClassName = x->AsStringOr("");
				if (const Json::Value* x = row.FindMember("method"))   b.MethodName      = x->AsStringOr("");
				if (const Json::Value* x = row.FindMember("enabled"))  b.Enabled         = x->AsBoolOr(true);
				if (const Json::Value* x = row.FindMember("argKind")) {
					int k = x->AsIntOr(0);
					if (k < 0 || k > static_cast<int>(InspectorEventArgKind::EntityRef)) k = 0;
					b.ArgumentKind = static_cast<InspectorEventArgKind>(k);
				}
				if (const Json::Value* x = row.FindMember("argValue")) b.ArgumentValue   = x->AsStringOr("");
				if (!b.ScriptClassName.empty() || !b.MethodName.empty()) {
					outList.Bindings.push_back(std::move(b));
				}
			}
		}

		// onAdd hook for UI widgets with a NormalColor field. When the user
		// adds (e.g.) ButtonComponent through the inspector to an entity that
		// already has an ImageComponent with an authored color, copy that
		// color into the widget's NormalColor before the next UIEventSystem
		// pass overwrites Image.Color from the widget's per-state palette.
		// Without this, the widget's default white NormalColor stamps over
		// whatever the user had carefully picked for the image, which is
		// the opposite of the user's intent ("I styled the image, now make
		// it interactive"). Skipped when there's no ImageComponent — those
		// widgets get tinted via their Handle child or simply don't tint at
		// all. Other state colors (Hovered/Pressed/Disabled/Focused) are
		// left at their authored defaults so a user who does want a
		// hover/press contrast doesn't have to re-author them.
		template <typename TComponent>
		void InheritImageColorIntoNormal(Entity e) {
			if (!e.HasComponent<ImageComponent>() || !e.HasComponent<TComponent>()) {
				return;
			}
			const Color imageColor = e.GetComponent<ImageComponent>().Color;
			e.GetComponent<TComponent>().NormalColor = imageColor;
		}

		// Inspector picker for an EntityHandle field on a UI widget.
		// The runtime stores a fast EntityHandle (regenerated each
		// scene load); the inspector / serialization round-trip goes
		// through the persistent UUID provided by Scene, so refs
		// survive reload the same way the rest of the engine treats
		// entity references. memberPtr points at the EntityHandle
		// member to read/write.
		template <typename T>
		PropertyDescriptor MakeUiEntityRef(const std::string& name, const std::string& displayName,
			EntityHandle T::* memberPtr)
		{
			return Properties::MakeEntityRef(name, displayName,
				[memberPtr](const Entity& e) -> uint64_t {
					EntityHandle h = e.GetComponent<T>().*memberPtr;
					if (h == entt::null) return 0;
					const Scene* s = e.GetScene();
					if (!s) return 0;
					return s->GetEntityPersistentID(h);
				},
				[memberPtr](Entity& e, uint64_t persistentId) {
					auto& ref = e.GetComponent<T>().*memberPtr;
					if (persistentId == 0) {
						ref = entt::null;
						return;
					}
					if (Scene* s = e.GetScene()) {
						EntityHandle resolved = entt::null;
						if (s->TryResolveEntityRef(persistentId, resolved)) {
							ref = resolved;
						}
					}
				});
		}

		// Inspector picker for per-state sprite UUIDs on UI widgets.
		// These slots only store the UUID (no resolved TextureHandle) —
		// UIEventSystem's SpriteSwap path looks up the handle on demand
		// via TextureManager when the resolved state actually changes.
		// Setting a uuid here doesn't preload the asset; the registry
		// is consulted again on the first frame the state activates.
		template <typename T>
		PropertyDescriptor MakeUiSpriteRef(const std::string& name, const std::string& displayName,
			UUID T::* memberPtr)
		{
			return Properties::MakeTextureRef(name, displayName,
				[memberPtr](const Entity& e) -> uint64_t {
					return static_cast<uint64_t>(e.GetComponent<T>().*memberPtr);
				},
				[memberPtr](Entity& e, uint64_t uuid) {
					e.GetComponent<T>().*memberPtr = UUID(uuid);
				});
		}

		// ── Texture / audio ref helpers ─────────────────────────────
		template <typename T, typename HandleAccessor, typename HandleAssign>
		PropertyDescriptor MakeTextureRefDirect(const std::string& name, const std::string& displayName,
			HandleAccessor getHandle, HandleAssign setHandle)
		{
			return Properties::MakeTextureRef(name, displayName,
				[getHandle](const Entity& e) -> uint64_t {
					const auto& component = e.GetComponent<T>();
					if constexpr (requires { component.TextureAssetId; }) {
						const uint64_t assetId = static_cast<uint64_t>(component.TextureAssetId);
						if (assetId != 0) {
							return assetId;
						}
					}
					const auto handle = getHandle(component);
					return handle.IsValid() ? TextureManager::GetTextureAssetUUID(handle) : 0ull;
				},
				[setHandle](Entity& e, uint64_t uuid) {
					auto& component = e.GetComponent<T>();
					TextureHandle h = uuid != 0
						? TextureManager::LoadTextureByUUID(uuid)
						: TextureHandle{};
					if (uuid != 0 && !h.IsValid()) {
						AssetRegistry::MarkDirty();
						AssetRegistry::Sync();
						h = TextureManager::LoadTextureByUUID(uuid);
					}
					setHandle(component, h, UUID(uuid));
				});
		}
	}

	void RegisterBuiltInComponents(SceneManager& sceneManager) {
		using Index::Codegen::RegisterComponent;
		using Index::Codegen::DeclareConflict;
		using Index::Codegen::DeclareDependency;

		// ── General ─────────────────────────────────────────────────

		// The inspector edits the authored Local* values (Unity-style).
		// For root entities Local matches World, so no visible behavior
		// change; for children, the inspector now correctly shows the
		// offset relative to the parent rather than the world snapshot.
		RegisterComponent<Transform2DComponent>(sceneManager, "Transform 2D",
			ComponentCategory::Component, "General", "Transform2D",
			{
				Properties::MakeWith<Vec2>("Position", "Position",
					[](const Entity& e) { return e.GetComponent<Transform2DComponent>().LocalPosition; },
					[](Entity& e, const Vec2& v) {
						e.GetComponent<Transform2DComponent>().SetPosition(v);
					}),
				Properties::MakeWith<float>("Rotation", "Rotation",
					[](const Entity& e) {
						return Degrees(e.GetComponent<Transform2DComponent>().LocalRotation);
					},
					[](Entity& e, float v) {
						e.GetComponent<Transform2DComponent>().SetRotation(Radians(v));
					},
					Properties::Meta::DragSpeed(1.0f)),
				Properties::MakeWith<Vec2>("Scale", "Scale",
					[](const Entity& e) { return e.GetComponent<Transform2DComponent>().LocalScale; },
					[](Entity& e, const Vec2& v) {
						e.GetComponent<Transform2DComponent>().SetScale(v);
					}),
			});

		// Inspector is fully custom (DrawRectTransform2DInspector) so this
		// component declares no PropertyDescriptors — they would be ignored
		// anyway since custom drawInspector wins over properties.
		RegisterComponent<RectTransform2DComponent>(sceneManager, "Rect Transform 2D",
			ComponentCategory::Component, "UI", "RectTransform2D");

		RegisterComponent<NameComponent>(sceneManager, "Name",
			ComponentCategory::Component, "General", "name",
			{
				Properties::Make("Name", "Name", &NameComponent::Name),
			});

		RegisterComponent<UUIDComponent>(sceneManager, "UUID", ComponentCategory::Tag);
		RegisterComponent<EntityMetaDataComponent>(sceneManager, "Entity Metadata", ComponentCategory::Tag);
		RegisterComponent<PrefabInstanceComponent>(sceneManager, "Prefab Instance", ComponentCategory::Tag);

		// ── Rendering ───────────────────────────────────────────────

		RegisterComponent<SpriteRendererComponent>(sceneManager, "Sprite Renderer",
			ComponentCategory::Component, "Rendering", "SpriteRenderer",
			{
				Properties::Make("Color", "Color", &SpriteRendererComponent::Color),
				Properties::MakeWith<int16_t>("SortingOrder", "Sorting Order",
					[](const Entity& e) { return e.GetComponent<SpriteRendererComponent>().SortingOrder; },
					[](Entity& e, int16_t v) {
						e.GetComponent<SpriteRendererComponent>().SortingOrder = v;
					}),
				Properties::MakeWith<uint8_t>("SortingLayer", "Sorting Layer",
					[](const Entity& e) { return e.GetComponent<SpriteRendererComponent>().SortingLayer; },
					[](Entity& e, uint8_t v) {
						e.GetComponent<SpriteRendererComponent>().SortingLayer = v;
					}),
				MakeTextureRefDirect<SpriteRendererComponent>("Texture", "Texture",
					[](const SpriteRendererComponent& s) { return s.TextureHandle; },
					[](SpriteRendererComponent& s, TextureHandle h, UUID assetId) {
						s.TextureHandle = h;
						s.TextureAssetId = assetId;
						if (auto* tex = TextureManager::GetTexture(h); tex) {
							tex->SetFilter(s.FilterMode);
						}
					}),
				Properties::MakeWith<Filter>("FilterMode", "Filter Mode",
					[](const Entity& e) { return e.GetComponent<SpriteRendererComponent>().FilterMode; },
					[](Entity& e, Filter v) {
						auto& sprite = e.GetComponent<SpriteRendererComponent>();
						sprite.FilterMode = v;
						if (auto* tex = TextureManager::GetTexture(sprite.TextureHandle); tex) {
							tex->SetFilter(v);
						}
					}),
			});

		RegisterComponent<ImageComponent>(sceneManager, "Image",
			ComponentCategory::Component, "UI", "Image",
			{
				Properties::Make("Color", "Color", &ImageComponent::Color),
				Properties::MakeWith<int16_t>("SortingOrder", "Sorting Order",
					[](const Entity& e) { return e.GetComponent<ImageComponent>().SortingOrder; },
					[](Entity& e, int16_t v) {
						e.GetComponent<ImageComponent>().SortingOrder = v;
					}),
				Properties::MakeWith<uint8_t>("SortingLayer", "Sorting Layer",
					[](const Entity& e) { return e.GetComponent<ImageComponent>().SortingLayer; },
					[](Entity& e, uint8_t v) {
						e.GetComponent<ImageComponent>().SortingLayer = v;
					}),
				MakeTextureRefDirect<ImageComponent>("Texture", "Texture",
					[](const ImageComponent& i) { return i.TextureHandle; },
					[](ImageComponent& i, TextureHandle h, UUID assetId) {
						i.TextureHandle = h;
						i.TextureAssetId = assetId;
						if (auto* tex = TextureManager::GetTexture(h); tex) {
							tex->SetFilter(i.FilterMode);
						}
					}),
				Properties::MakeWith<Filter>("FilterMode", "Filter Mode",
					[](const Entity& e) { return e.GetComponent<ImageComponent>().FilterMode; },
					[](Entity& e, Filter v) {
						auto& image = e.GetComponent<ImageComponent>();
						image.FilterMode = v;
						if (auto* tex = TextureManager::GetTexture(image.TextureHandle); tex) {
							tex->SetFilter(v);
						}
					}),
			});

		RegisterComponent<Camera2DComponent>(sceneManager, "Camera 2D",
			ComponentCategory::Component, "Rendering", "Camera2D",
			{
				Properties::MakeWith<float>("Zoom", "Zoom",
					[](const Entity& e) { return e.GetComponent<Camera2DComponent>().GetZoom(); },
					[](Entity& e, float v) {
						e.GetComponent<Camera2DComponent>().SetZoom(v);
					},
					Properties::Meta::Clamp(0.01, 100.0, 0.01f)),
				Properties::MakeWith<float>("OrthographicSize", "Orthographic Size",
					[](const Entity& e) { return e.GetComponent<Camera2DComponent>().GetOrthographicSize(); },
					[](Entity& e, float v) {
						e.GetComponent<Camera2DComponent>().SetOrthographicSize(v);
					},
					Properties::Meta::Clamp(0.05, 1000.0, 0.05f)),
				Properties::MakeWith<Color>("ClearColor", "Clear Color",
					[](const Entity& e) { return e.GetComponent<Camera2DComponent>().GetClearColor(); },
					[](Entity& e, const Color& c) {
						e.GetComponent<Camera2DComponent>().SetClearColor(c);
					}),
			});

		// Particle System 2D — fully declarative now via the unified API.
		// Shape is a variant (Circle vs Square) driven by
		// Properties::MakeVariantWith over the std::variant<CircleParams,
		// SquareParams> field; "Gravity Value" uses Meta::EnabledIf so
		// it greys out when UseGravity is false. The Play / Pause button
		// + texture preview live in the editor-only inspector extension
		// (DrawParticleSystem2DInspector renders those after the
		// auto-drawer fallback handles every property below).
		using PSC = ParticleSystem2DComponent;
		using ShapeType = PSC::ShapeType;
		using CircleParams = PSC::CircleParams;
		using SquareParams = PSC::SquareParams;

		PropertyMetadata gravityEnabledMeta = Properties::Meta::EnabledIf<PSC>(
			[](const PSC& ps) { return ps.ParticleSettings.UseGravity; })
			.WithDragSpeed(0.05f);

		std::vector<PropertyDescriptor> particleProperties;
		particleProperties.push_back(Properties::Make("PlayOnAwake", "Play On Awake",
			&PSC::PlayOnAwake));
		particleProperties.push_back(Properties::MakeWith<float>("LifeTime", "Life Time",
			[](const Entity& e) { return e.GetComponent<PSC>().ParticleSettings.LifeTime; },
			[](Entity& e, float v) { e.GetComponent<PSC>().ParticleSettings.LifeTime = v; },
			Properties::Meta::Clamp(0.01, 600.0, 0.05f)));
		particleProperties.push_back(Properties::MakeWith<float>("Scale", "Scale",
			[](const Entity& e) { return e.GetComponent<PSC>().ParticleSettings.Scale; },
			[](Entity& e, float v) { e.GetComponent<PSC>().ParticleSettings.Scale = v; },
			Properties::Meta::Clamp(0.0, 100.0, 0.05f)));
		particleProperties.push_back(Properties::MakeWith<float>("Speed", "Speed",
			[](const Entity& e) { return e.GetComponent<PSC>().ParticleSettings.Speed; },
			[](Entity& e, float v) { e.GetComponent<PSC>().ParticleSettings.Speed = v; },
			Properties::Meta::DragSpeed(0.1f)));
		particleProperties.push_back(Properties::MakeWith<bool>("Gravity", "Use Gravity",
			[](const Entity& e) { return e.GetComponent<PSC>().ParticleSettings.UseGravity; },
			[](Entity& e, bool v) { e.GetComponent<PSC>().ParticleSettings.UseGravity = v; }));
		particleProperties.push_back(Properties::MakeWith<Vec2>("GravityValue", "Gravity Value",
			[](const Entity& e) { return e.GetComponent<PSC>().ParticleSettings.Gravity; },
			[](Entity& e, const Vec2& v) { e.GetComponent<PSC>().ParticleSettings.Gravity = v; },
			gravityEnabledMeta));
		particleProperties.push_back(Properties::MakeWith<bool>("RandomColors", "Random Colors",
			[](const Entity& e) { return e.GetComponent<PSC>().ParticleSettings.UseRandomColors; },
			[](Entity& e, bool v) { e.GetComponent<PSC>().ParticleSettings.UseRandomColors = v; }));
		particleProperties.push_back(Properties::MakeWith<uint16_t>("EmitOverTime", "Emit Over Time",
			[](const Entity& e) { return e.GetComponent<PSC>().EmissionSettings.EmitOverTime; },
			[](Entity& e, uint16_t v) { e.GetComponent<PSC>().EmissionSettings.EmitOverTime = v; },
			Properties::Meta::Clamp(0.0, 65535.0, 1.0f).WithHeader("Emission")));
		// Variant: Shape Type → Circle / Square branches.
		particleProperties.push_back(Properties::MakeVariantWith<ShapeType>("ShapeType", "Shape Type",
			[](const Entity& e) -> ShapeType {
				return std::visit([](auto&& s) -> ShapeType {
					using T = std::decay_t<decltype(s)>;
					if constexpr (std::is_same_v<T, CircleParams>) return ShapeType::Circle;
					else                                            return ShapeType::Square;
				}, e.GetComponent<PSC>().Shape);
			},
			[](Entity& e, ShapeType v) {
				auto& ps = e.GetComponent<PSC>();
				if (v == ShapeType::Circle) ps.Shape = CircleParams{ 1.0f, false };
				else                        ps.Shape = SquareParams{ Vec2{ 1.0f, 1.0f } };
			},
			{
				Properties::Branch(ShapeType::Circle, {
					Properties::MakeWith<float>("Radius", "Radius",
						[](const Entity& e) {
							return std::get<CircleParams>(e.GetComponent<PSC>().Shape).Radius;
						},
						[](Entity& e, float v) {
							std::get<CircleParams>(e.GetComponent<PSC>().Shape).Radius = v;
						},
						Properties::Meta::Clamp(0.0, 1000.0, 0.05f)),
					Properties::MakeWith<bool>("OnCircleEdge", "On Circle Edge",
						[](const Entity& e) {
							return std::get<CircleParams>(e.GetComponent<PSC>().Shape).IsOnCircle;
						},
						[](Entity& e, bool v) {
							std::get<CircleParams>(e.GetComponent<PSC>().Shape).IsOnCircle = v;
						}),
				}),
				Properties::Branch(ShapeType::Square, {
					Properties::MakeWith<Vec2>("HalfExtents", "Half Extents",
						[](const Entity& e) {
							return std::get<SquareParams>(e.GetComponent<PSC>().Shape).HalfExtends;
						},
						[](Entity& e, const Vec2& v) {
							std::get<SquareParams>(e.GetComponent<PSC>().Shape).HalfExtends = v;
						},
						Properties::Meta::Clamp(0.0, 10000.0, 0.05f)),
				}),
			}));
		particleProperties.push_back(Properties::MakeWith<Color>("Color", "Color",
			[](const Entity& e) { return e.GetComponent<PSC>().RenderingSettings.Color; },
			[](Entity& e, const Color& c) { e.GetComponent<PSC>().RenderingSettings.Color = c; },
			Properties::Meta::Header("Rendering")));
		particleProperties.push_back(Properties::MakeWith<int32_t>("MaxParticles", "Max Particles",
			[](const Entity& e) { return static_cast<int32_t>(e.GetComponent<PSC>().RenderingSettings.MaxParticles); },
			[](Entity& e, int32_t v) {
				e.GetComponent<PSC>().RenderingSettings.MaxParticles = static_cast<uint32_t>(std::max(1, v));
			},
			Properties::Meta::Clamp(1.0, 10000.0, 1.0f)));
		particleProperties.push_back(Properties::MakeWith<int16_t>("SortingOrder", "Sorting Order",
			[](const Entity& e) { return e.GetComponent<PSC>().RenderingSettings.SortingOrder; },
			[](Entity& e, int16_t v) { e.GetComponent<PSC>().RenderingSettings.SortingOrder = v; }));
		particleProperties.push_back(Properties::MakeWith<uint8_t>("SortingLayer", "Sorting Layer",
			[](const Entity& e) { return e.GetComponent<PSC>().RenderingSettings.SortingLayer; },
			[](Entity& e, uint8_t v) { e.GetComponent<PSC>().RenderingSettings.SortingLayer = v; }));

		RegisterComponent<ParticleSystem2DComponent>(sceneManager, "Particle System 2D",
			ComponentCategory::Component, "Rendering", "ParticleSystem2D",
			std::move(particleProperties));

		// Custom copyTo: ParticleSystem2DComponent caches m_EmitterScene/m_EmitterEntity
		// during on_construct. The default value-copy would carry the source entity's
		// pointers across, leaving the duplicate's emitter pointing at a different
		// (possibly destroyed) entity in (possibly) a different scene. After the
		// member-wise copy, RebindEmitter re-points the destination at its own scene
		// and entity so the next Update() targets the right transform.
		sceneManager.GetComponentRegistry().ForEachComponentInfo(
			[](const std::type_index& id, ComponentInfo& info) {
				if (id == std::type_index(typeid(ParticleSystem2DComponent))) {
					info.copyTo = [](Entity src, Entity dst) {
						if (!src.HasComponent<ParticleSystem2DComponent>()) return;
						const auto& srcComp = src.GetComponent<ParticleSystem2DComponent>();
						if (dst.HasComponent<ParticleSystem2DComponent>()) {
							dst.GetComponent<ParticleSystem2DComponent>() = srcComp;
						} else {
							dst.AddComponent<ParticleSystem2DComponent>(srcComp);
						}
						dst.GetComponent<ParticleSystem2DComponent>()
							.RebindEmitter(dst.GetScene(), dst.GetHandle());
					};
				}
			});

		// Post Processing 2D — settings only. The renderer doesn't read these
		// yet; this slice locks in the data shape and inspector UX so scenes
		// can be authored against it before the FBO + blit pipeline lands.
		{
			using PP = PostProcessing2DComponent;

			PropertyMetadata cgEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.ColorGrading.Enabled; });
			PropertyMetadata vgEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.Vignette.Enabled; });
			PropertyMetadata caEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.ChromaticAberration.Enabled; });
			PropertyMetadata grEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.Grain.Enabled; });
			PropertyMetadata blEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.Bloom.Enabled; });
			PropertyMetadata ldEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.LensDistortion.Enabled; });

			std::vector<PropertyDescriptor> ppProps;

			// Color Grading
			ppProps.push_back(Properties::MakeWith<bool>("CG_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().ColorGrading.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().ColorGrading.Enabled = v; },
				Properties::Meta::Header("Color Grading").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().ColorGrading = PP::ColorGradingSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<float>("CG_Exposure", "Exposure",
				[](const Entity& e) { return e.GetComponent<PP>().ColorGrading.Exposure; },
				[](Entity& e, float v) { e.GetComponent<PP>().ColorGrading.Exposure = v; },
				PropertyMetadata{ cgEnabled }.WithClamp(-10.0, 10.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<float>("CG_Contrast", "Contrast",
				[](const Entity& e) { return e.GetComponent<PP>().ColorGrading.Contrast; },
				[](Entity& e, float v) { e.GetComponent<PP>().ColorGrading.Contrast = v; },
				PropertyMetadata{ cgEnabled }.WithClamp(0.0, 4.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<float>("CG_Saturation", "Saturation",
				[](const Entity& e) { return e.GetComponent<PP>().ColorGrading.Saturation; },
				[](Entity& e, float v) { e.GetComponent<PP>().ColorGrading.Saturation = v; },
				PropertyMetadata{ cgEnabled }.WithClamp(0.0, 4.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<float>("CG_Temperature", "Temperature",
				[](const Entity& e) { return e.GetComponent<PP>().ColorGrading.Temperature; },
				[](Entity& e, float v) { e.GetComponent<PP>().ColorGrading.Temperature = v; },
				PropertyMetadata{ cgEnabled }.WithClamp(-1.0, 1.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<float>("CG_Tint", "Tint",
				[](const Entity& e) { return e.GetComponent<PP>().ColorGrading.Tint; },
				[](Entity& e, float v) { e.GetComponent<PP>().ColorGrading.Tint = v; },
				PropertyMetadata{ cgEnabled }.WithClamp(-1.0, 1.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<Color>("CG_ColorFilter", "Color Filter",
				[](const Entity& e) { return e.GetComponent<PP>().ColorGrading.ColorFilter; },
				[](Entity& e, const Color& c) { e.GetComponent<PP>().ColorGrading.ColorFilter = c; },
				cgEnabled));

			// Vignette
			ppProps.push_back(Properties::MakeWith<bool>("VG_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().Vignette.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().Vignette.Enabled = v; },
				Properties::Meta::Header("Vignette").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().Vignette = PP::VignetteSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<Color>("VG_Color", "Color",
				[](const Entity& e) { return e.GetComponent<PP>().Vignette.Color; },
				[](Entity& e, const Color& c) { e.GetComponent<PP>().Vignette.Color = c; },
				vgEnabled));
			ppProps.push_back(Properties::MakeWith<float>("VG_Intensity", "Intensity",
				[](const Entity& e) { return e.GetComponent<PP>().Vignette.Intensity; },
				[](Entity& e, float v) { e.GetComponent<PP>().Vignette.Intensity = v; },
				PropertyMetadata{ vgEnabled }.WithClamp(0.0, 1.0).WithDragSpeed(0.005f)));
			ppProps.push_back(Properties::MakeWith<float>("VG_Smoothness", "Smoothness",
				[](const Entity& e) { return e.GetComponent<PP>().Vignette.Smoothness; },
				[](Entity& e, float v) { e.GetComponent<PP>().Vignette.Smoothness = v; },
				PropertyMetadata{ vgEnabled }.WithClamp(0.0, 1.0).WithDragSpeed(0.005f)));
			ppProps.push_back(Properties::MakeWith<float>("VG_Roundness", "Roundness",
				[](const Entity& e) { return e.GetComponent<PP>().Vignette.Roundness; },
				[](Entity& e, float v) { e.GetComponent<PP>().Vignette.Roundness = v; },
				PropertyMetadata{ vgEnabled }.WithClamp(0.0, 1.0).WithDragSpeed(0.005f)));
			ppProps.push_back(Properties::MakeWith<Vec2>("VG_Center", "Center",
				[](const Entity& e) { return e.GetComponent<PP>().Vignette.Center; },
				[](Entity& e, const Vec2& v) { e.GetComponent<PP>().Vignette.Center = v; },
				vgEnabled));

			// Chromatic Aberration
			ppProps.push_back(Properties::MakeWith<bool>("CA_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().ChromaticAberration.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().ChromaticAberration.Enabled = v; },
				Properties::Meta::Header("Chromatic Aberration").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().ChromaticAberration = PP::ChromaticAberrationSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<float>("CA_Intensity", "Intensity",
				[](const Entity& e) { return e.GetComponent<PP>().ChromaticAberration.Intensity; },
				[](Entity& e, float v) { e.GetComponent<PP>().ChromaticAberration.Intensity = v; },
				PropertyMetadata{ caEnabled }.WithClamp(0.0, 1.0).WithDragSpeed(0.005f)));

			// Grain
			ppProps.push_back(Properties::MakeWith<bool>("GR_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().Grain.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().Grain.Enabled = v; },
				Properties::Meta::Header("Grain").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().Grain = PP::GrainSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<float>("GR_Intensity", "Intensity",
				[](const Entity& e) { return e.GetComponent<PP>().Grain.Intensity; },
				[](Entity& e, float v) { e.GetComponent<PP>().Grain.Intensity = v; },
				PropertyMetadata{ grEnabled }.WithClamp(0.0, 1.0).WithDragSpeed(0.005f)));
			ppProps.push_back(Properties::MakeWith<float>("GR_Size", "Size",
				[](const Entity& e) { return e.GetComponent<PP>().Grain.Size; },
				[](Entity& e, float v) { e.GetComponent<PP>().Grain.Size = v; },
				PropertyMetadata{ grEnabled }.WithClamp(0.3, 3.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<bool>("GR_Colored", "Colored",
				[](const Entity& e) { return e.GetComponent<PP>().Grain.Colored; },
				[](Entity& e, bool v) { e.GetComponent<PP>().Grain.Colored = v; },
				grEnabled));

			// Bloom
			ppProps.push_back(Properties::MakeWith<bool>("BL_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().Bloom.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().Bloom.Enabled = v; },
				Properties::Meta::Header("Bloom").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().Bloom = PP::BloomSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<int32_t>("BL_Taps", "Taps",
				[](const Entity& e) { return static_cast<int32_t>(e.GetComponent<PP>().Bloom.Taps); },
				[](Entity& e, int32_t v) {
					e.GetComponent<PP>().Bloom.Taps = std::max(7, std::min(500, v));
				},
				PropertyMetadata{ blEnabled }.WithClamp(7.0, 500.0).WithDragSpeed(1.0f)));
			ppProps.push_back(Properties::MakeWith<float>("BL_Threshold", "Threshold",
				[](const Entity& e) { return e.GetComponent<PP>().Bloom.Threshold; },
				[](Entity& e, float v) { e.GetComponent<PP>().Bloom.Threshold = v; },
				PropertyMetadata{ blEnabled }.WithClamp(0.0, 10.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<float>("BL_Intensity", "Intensity",
				[](const Entity& e) { return e.GetComponent<PP>().Bloom.Intensity; },
				[](Entity& e, float v) { e.GetComponent<PP>().Bloom.Intensity = v; },
				PropertyMetadata{ blEnabled }.WithClamp(0.0, 10.0).WithDragSpeed(0.01f)));
			ppProps.push_back(Properties::MakeWith<float>("BL_Scatter", "Scatter",
				[](const Entity& e) { return e.GetComponent<PP>().Bloom.Scatter; },
				[](Entity& e, float v) { e.GetComponent<PP>().Bloom.Scatter = v; },
				PropertyMetadata{ blEnabled }.WithClamp(0.0, 1.0).WithDragSpeed(0.005f)));
			ppProps.push_back(Properties::MakeWith<Color>("BL_Tint", "Tint",
				[](const Entity& e) { return e.GetComponent<PP>().Bloom.Tint; },
				[](Entity& e, const Color& c) { e.GetComponent<PP>().Bloom.Tint = c; },
				blEnabled));

			// Lens Distortion
			ppProps.push_back(Properties::MakeWith<bool>("LD_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().LensDistortion.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().LensDistortion.Enabled = v; },
				Properties::Meta::Header("Lens Distortion").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().LensDistortion = PP::LensDistortionSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<float>("LD_Intensity", "Intensity",
				[](const Entity& e) { return e.GetComponent<PP>().LensDistortion.Intensity; },
				[](Entity& e, float v) { e.GetComponent<PP>().LensDistortion.Intensity = v; },
				PropertyMetadata{ ldEnabled }.WithClamp(-1.0, 1.0).WithDragSpeed(0.005f)));
			ppProps.push_back(Properties::MakeWith<float>("LD_Scale", "Scale",
				[](const Entity& e) { return e.GetComponent<PP>().LensDistortion.Scale; },
				[](Entity& e, float v) { e.GetComponent<PP>().LensDistortion.Scale = v; },
				PropertyMetadata{ ldEnabled }.WithClamp(0.5, 1.5).WithDragSpeed(0.005f)));
			ppProps.push_back(Properties::MakeWith<Vec2>("LD_Center", "Center",
				[](const Entity& e) { return e.GetComponent<PP>().LensDistortion.Center; },
				[](Entity& e, const Vec2& v) { e.GetComponent<PP>().LensDistortion.Center = v; },
				ldEnabled));

			// Pixelated
			PropertyMetadata pxEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.Pixelated.Enabled; });
			PropertyMetadata pxPaletteEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.Pixelated.Enabled && p.Pixelated.QuantizeColor; });
			ppProps.push_back(Properties::MakeWith<bool>("PX_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().Pixelated.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().Pixelated.Enabled = v; },
				Properties::Meta::Header("Pixelated").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().Pixelated = PP::PixelatedSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<float>("PX_BlockSize", "Block Size",
				[](const Entity& e) { return e.GetComponent<PP>().Pixelated.BlockSize; },
				[](Entity& e, float v) { e.GetComponent<PP>().Pixelated.BlockSize = v; },
				PropertyMetadata{ pxEnabled }.WithClamp(1.0, 64.0).WithDragSpeed(0.1f)));
			ppProps.push_back(Properties::MakeWith<bool>("PX_QuantizeColor", "Quantize Color",
				[](const Entity& e) { return e.GetComponent<PP>().Pixelated.QuantizeColor; },
				[](Entity& e, bool v) { e.GetComponent<PP>().Pixelated.QuantizeColor = v; },
				pxEnabled));
			ppProps.push_back(Properties::MakeWith<int32_t>("PX_PaletteSteps", "Palette Steps",
				[](const Entity& e) { return static_cast<int32_t>(e.GetComponent<PP>().Pixelated.PaletteSteps); },
				[](Entity& e, int32_t v) {
					e.GetComponent<PP>().Pixelated.PaletteSteps = std::max(2, std::min(256, v));
				},
				PropertyMetadata{ pxPaletteEnabled }.WithClamp(2.0, 256.0).WithDragSpeed(1.0f)));

			// Gaussian Blur
			PropertyMetadata gbEnabled = Properties::Meta::EnabledIf<PP>(
				[](const PP& p) { return p.GaussianBlur.Enabled; });
			ppProps.push_back(Properties::MakeWith<bool>("GB_Enabled", "Enabled",
				[](const Entity& e) { return e.GetComponent<PP>().GaussianBlur.Enabled; },
				[](Entity& e, bool v) { e.GetComponent<PP>().GaussianBlur.Enabled = v; },
				Properties::Meta::Header("Gaussian Blur").WithResetSection(
					[](Entity& e) {
						e.GetComponent<PP>().GaussianBlur = PP::GaussianBlurSettings{};
					})));
			ppProps.push_back(Properties::MakeWith<int32_t>("GB_Taps", "Taps",
				[](const Entity& e) { return static_cast<int32_t>(e.GetComponent<PP>().GaussianBlur.Taps); },
				[](Entity& e, int32_t v) {
					e.GetComponent<PP>().GaussianBlur.Taps = std::max(7, std::min(500, v));
				},
				PropertyMetadata{ gbEnabled }.WithClamp(7.0, 500.0).WithDragSpeed(1.0f)));
			ppProps.push_back(Properties::MakeWith<float>("GB_Radius", "Radius",
				[](const Entity& e) { return e.GetComponent<PP>().GaussianBlur.Radius; },
				[](Entity& e, float v) { e.GetComponent<PP>().GaussianBlur.Radius = v; },
				PropertyMetadata{ gbEnabled }.WithClamp(0.0, 1.0).WithDragSpeed(0.005f)));

			ComponentInfo ppInfo{ "Post Processing", "Rendering", ComponentCategory::Component };
			ppInfo.serializedName = "PostProcessing2D";
			ppInfo.properties = std::move(ppProps);

			ppInfo.serialize = [](Entity e) -> Json::Value {
				const auto& p = e.GetComponent<PP>();
				Json::Value v = Json::Value::MakeObject();

				Json::Value cg = Json::Value::MakeObject();
				cg.AddMember("enabled",     Json::Value(p.ColorGrading.Enabled));
				cg.AddMember("exposure",    Json::Value(p.ColorGrading.Exposure));
				cg.AddMember("contrast",    Json::Value(p.ColorGrading.Contrast));
				cg.AddMember("saturation",  Json::Value(p.ColorGrading.Saturation));
				cg.AddMember("temperature", Json::Value(p.ColorGrading.Temperature));
				cg.AddMember("tint",        Json::Value(p.ColorGrading.Tint));
				cg.AddMember("colorFilter", UIColorToJson(p.ColorGrading.ColorFilter));
				v.AddMember("colorGrading", std::move(cg));

				Json::Value vg = Json::Value::MakeObject();
				vg.AddMember("enabled",    Json::Value(p.Vignette.Enabled));
				vg.AddMember("color",      UIColorToJson(p.Vignette.Color));
				vg.AddMember("intensity",  Json::Value(p.Vignette.Intensity));
				vg.AddMember("smoothness", Json::Value(p.Vignette.Smoothness));
				vg.AddMember("roundness",  Json::Value(p.Vignette.Roundness));
				vg.AddMember("centerX",    Json::Value(p.Vignette.Center.x));
				vg.AddMember("centerY",    Json::Value(p.Vignette.Center.y));
				v.AddMember("vignette", std::move(vg));

				Json::Value ca = Json::Value::MakeObject();
				ca.AddMember("enabled",   Json::Value(p.ChromaticAberration.Enabled));
				ca.AddMember("intensity", Json::Value(p.ChromaticAberration.Intensity));
				v.AddMember("chromaticAberration", std::move(ca));

				Json::Value gr = Json::Value::MakeObject();
				gr.AddMember("enabled",   Json::Value(p.Grain.Enabled));
				gr.AddMember("intensity", Json::Value(p.Grain.Intensity));
				gr.AddMember("size",      Json::Value(p.Grain.Size));
				gr.AddMember("colored",   Json::Value(p.Grain.Colored));
				v.AddMember("grain", std::move(gr));

				Json::Value bl = Json::Value::MakeObject();
				bl.AddMember("enabled",   Json::Value(p.Bloom.Enabled));
				bl.AddMember("threshold", Json::Value(p.Bloom.Threshold));
				bl.AddMember("intensity", Json::Value(p.Bloom.Intensity));
				bl.AddMember("scatter",   Json::Value(p.Bloom.Scatter));
				bl.AddMember("taps",      Json::Value(p.Bloom.Taps));
				bl.AddMember("tint",      UIColorToJson(p.Bloom.Tint));
				v.AddMember("bloom", std::move(bl));

				Json::Value ld = Json::Value::MakeObject();
				ld.AddMember("enabled",   Json::Value(p.LensDistortion.Enabled));
				ld.AddMember("intensity", Json::Value(p.LensDistortion.Intensity));
				ld.AddMember("scale",     Json::Value(p.LensDistortion.Scale));
				ld.AddMember("centerX",   Json::Value(p.LensDistortion.Center.x));
				ld.AddMember("centerY",   Json::Value(p.LensDistortion.Center.y));
				v.AddMember("lensDistortion", std::move(ld));

				Json::Value px = Json::Value::MakeObject();
				px.AddMember("enabled",       Json::Value(p.Pixelated.Enabled));
				px.AddMember("blockSize",     Json::Value(p.Pixelated.BlockSize));
				px.AddMember("paletteSteps",  Json::Value(p.Pixelated.PaletteSteps));
				px.AddMember("quantizeColor", Json::Value(p.Pixelated.QuantizeColor));
				v.AddMember("pixelated", std::move(px));

				Json::Value gb = Json::Value::MakeObject();
				gb.AddMember("enabled", Json::Value(p.GaussianBlur.Enabled));
				gb.AddMember("radius",  Json::Value(p.GaussianBlur.Radius));
				gb.AddMember("taps",    Json::Value(p.GaussianBlur.Taps));
				v.AddMember("gaussianBlur", std::move(gb));

				return v;
			};

			ppInfo.deserialize = [](Entity e, const Json::Value& v) {
				auto& p = e.GetComponent<PP>();

				// Reset signal: SceneSerializer::ResetComponent calls us with
				// an empty JSON object to mean "revert to defaults". The
				// hardcoded whitelist in SceneSerializerDeserialize.cpp's
				// DeserializeComponent doesn't include this component, so
				// without the check below the per-key FindMember calls would
				// all return null and nothing would actually reset. Detect
				// the empty-object case here and default-construct the whole
				// component before falling through to the (now no-op) patch
				// path. Non-empty inputs (real save files / prefab overrides)
				// keep the existing partial-deserialize semantics.
				const bool isEmptyReset = v.IsObject()
					&& !v.FindMember("colorGrading")
					&& !v.FindMember("vignette")
					&& !v.FindMember("chromaticAberration")
					&& !v.FindMember("grain")
					&& !v.FindMember("bloom")
					&& !v.FindMember("lensDistortion")
					&& !v.FindMember("pixelated")
					&& !v.FindMember("gaussianBlur");
				if (isEmptyReset) {
					p = PP{};
					return;
				}

				if (const Json::Value* cg = v.FindMember("colorGrading")) {
					if (const Json::Value* m = cg->FindMember("enabled"))     p.ColorGrading.Enabled     = m->AsBoolOr(p.ColorGrading.Enabled);
					if (const Json::Value* m = cg->FindMember("exposure"))    p.ColorGrading.Exposure    = static_cast<float>(m->AsDoubleOr(p.ColorGrading.Exposure));
					if (const Json::Value* m = cg->FindMember("contrast"))    p.ColorGrading.Contrast    = static_cast<float>(m->AsDoubleOr(p.ColorGrading.Contrast));
					if (const Json::Value* m = cg->FindMember("saturation"))  p.ColorGrading.Saturation  = static_cast<float>(m->AsDoubleOr(p.ColorGrading.Saturation));
					if (const Json::Value* m = cg->FindMember("temperature")) p.ColorGrading.Temperature = static_cast<float>(m->AsDoubleOr(p.ColorGrading.Temperature));
					if (const Json::Value* m = cg->FindMember("tint"))        p.ColorGrading.Tint        = static_cast<float>(m->AsDoubleOr(p.ColorGrading.Tint));
					if (const Json::Value* m = cg->FindMember("colorFilter")) p.ColorGrading.ColorFilter = UIColorFromJson(*m, p.ColorGrading.ColorFilter);
				}

				if (const Json::Value* vg = v.FindMember("vignette")) {
					if (const Json::Value* m = vg->FindMember("enabled"))    p.Vignette.Enabled    = m->AsBoolOr(p.Vignette.Enabled);
					if (const Json::Value* m = vg->FindMember("color"))      p.Vignette.Color      = UIColorFromJson(*m, p.Vignette.Color);
					if (const Json::Value* m = vg->FindMember("intensity"))  p.Vignette.Intensity  = static_cast<float>(m->AsDoubleOr(p.Vignette.Intensity));
					if (const Json::Value* m = vg->FindMember("smoothness")) p.Vignette.Smoothness = static_cast<float>(m->AsDoubleOr(p.Vignette.Smoothness));
					if (const Json::Value* m = vg->FindMember("roundness"))  p.Vignette.Roundness  = static_cast<float>(m->AsDoubleOr(p.Vignette.Roundness));
					if (const Json::Value* m = vg->FindMember("centerX"))    p.Vignette.Center.x   = static_cast<float>(m->AsDoubleOr(p.Vignette.Center.x));
					if (const Json::Value* m = vg->FindMember("centerY"))    p.Vignette.Center.y   = static_cast<float>(m->AsDoubleOr(p.Vignette.Center.y));
				}

				if (const Json::Value* ca = v.FindMember("chromaticAberration")) {
					if (const Json::Value* m = ca->FindMember("enabled"))   p.ChromaticAberration.Enabled   = m->AsBoolOr(p.ChromaticAberration.Enabled);
					if (const Json::Value* m = ca->FindMember("intensity")) p.ChromaticAberration.Intensity = static_cast<float>(m->AsDoubleOr(p.ChromaticAberration.Intensity));
				}

				if (const Json::Value* gr = v.FindMember("grain")) {
					if (const Json::Value* m = gr->FindMember("enabled"))   p.Grain.Enabled   = m->AsBoolOr(p.Grain.Enabled);
					if (const Json::Value* m = gr->FindMember("intensity")) p.Grain.Intensity = static_cast<float>(m->AsDoubleOr(p.Grain.Intensity));
					if (const Json::Value* m = gr->FindMember("size"))      p.Grain.Size      = static_cast<float>(m->AsDoubleOr(p.Grain.Size));
					if (const Json::Value* m = gr->FindMember("colored"))   p.Grain.Colored   = m->AsBoolOr(p.Grain.Colored);
				}

				if (const Json::Value* bl = v.FindMember("bloom")) {
					if (const Json::Value* m = bl->FindMember("enabled"))   p.Bloom.Enabled   = m->AsBoolOr(p.Bloom.Enabled);
					if (const Json::Value* m = bl->FindMember("threshold")) p.Bloom.Threshold = static_cast<float>(m->AsDoubleOr(p.Bloom.Threshold));
					if (const Json::Value* m = bl->FindMember("intensity")) p.Bloom.Intensity = static_cast<float>(m->AsDoubleOr(p.Bloom.Intensity));
					if (const Json::Value* m = bl->FindMember("scatter"))   p.Bloom.Scatter   = static_cast<float>(m->AsDoubleOr(p.Bloom.Scatter));
					if (const Json::Value* m = bl->FindMember("taps")) {
						p.Bloom.Taps = std::max(7, std::min(500, m->AsIntOr(p.Bloom.Taps)));
					}
					// Legacy migration: pre-slider scenes wrote a "quality"
					// enum int (0/1/2 = Low/Medium/High = 11/21/33 taps).
					// Only consumed when "taps" is absent so newer saves
					// always take precedence.
					else if (const Json::Value* m = bl->FindMember("quality")) {
						const int q = m->AsIntOr(1);
						p.Bloom.Taps = (q == 0) ? 11 : (q == 2) ? 33 : 21;
					}
					if (const Json::Value* m = bl->FindMember("tint"))      p.Bloom.Tint      = UIColorFromJson(*m, p.Bloom.Tint);
				}

				if (const Json::Value* ld = v.FindMember("lensDistortion")) {
					if (const Json::Value* m = ld->FindMember("enabled"))   p.LensDistortion.Enabled   = m->AsBoolOr(p.LensDistortion.Enabled);
					if (const Json::Value* m = ld->FindMember("intensity")) p.LensDistortion.Intensity = static_cast<float>(m->AsDoubleOr(p.LensDistortion.Intensity));
					if (const Json::Value* m = ld->FindMember("scale"))     p.LensDistortion.Scale     = static_cast<float>(m->AsDoubleOr(p.LensDistortion.Scale));
					if (const Json::Value* m = ld->FindMember("centerX"))   p.LensDistortion.Center.x  = static_cast<float>(m->AsDoubleOr(p.LensDistortion.Center.x));
					if (const Json::Value* m = ld->FindMember("centerY"))   p.LensDistortion.Center.y  = static_cast<float>(m->AsDoubleOr(p.LensDistortion.Center.y));
				}

				if (const Json::Value* px = v.FindMember("pixelated")) {
					if (const Json::Value* m = px->FindMember("enabled"))       p.Pixelated.Enabled       = m->AsBoolOr(p.Pixelated.Enabled);
					if (const Json::Value* m = px->FindMember("blockSize"))     p.Pixelated.BlockSize     = static_cast<float>(m->AsDoubleOr(p.Pixelated.BlockSize));
					if (const Json::Value* m = px->FindMember("paletteSteps"))  p.Pixelated.PaletteSteps  = m->AsIntOr(p.Pixelated.PaletteSteps);
					if (const Json::Value* m = px->FindMember("quantizeColor")) p.Pixelated.QuantizeColor = m->AsBoolOr(p.Pixelated.QuantizeColor);
				}

				if (const Json::Value* gb = v.FindMember("gaussianBlur")) {
					if (const Json::Value* m = gb->FindMember("enabled")) p.GaussianBlur.Enabled = m->AsBoolOr(p.GaussianBlur.Enabled);
					if (const Json::Value* m = gb->FindMember("radius"))  p.GaussianBlur.Radius  = static_cast<float>(m->AsDoubleOr(p.GaussianBlur.Radius));
					if (const Json::Value* m = gb->FindMember("taps")) {
						p.GaussianBlur.Taps = std::max(7, std::min(500, m->AsIntOr(p.GaussianBlur.Taps)));
					}
					// Legacy migration from the pre-slider quality enum.
					else if (const Json::Value* m = gb->FindMember("quality")) {
						const int q = m->AsIntOr(1);
						p.GaussianBlur.Taps = (q == 0) ? 11 : (q == 2) ? 33 : 21;
					}
				}
			};

			sceneManager.RegisterComponentType<PostProcessing2DComponent>(ppInfo);
		}

		RegisterComponent<TextRendererComponent>(sceneManager, "Text Renderer",
			ComponentCategory::Component, "UI", "TextRenderer",
			{
				Properties::Make("Text", "Text", &TextRendererComponent::Text),
				Properties::MakeFontRef("Font", "Font",
					[](const Entity& e) -> uint64_t {
						return static_cast<uint64_t>(e.GetComponent<TextRendererComponent>().FontAssetId);
					},
					[](Entity& e, uint64_t uuid) {
						auto& text = e.GetComponent<TextRendererComponent>();
						text.FontAssetId = UUID(uuid);
						// Force re-resolve next frame — atlas might need
						// a different size if FontSize changed at the same
						// time, but ResolvedFont = invalid is enough either way.
						text.ResolvedFont = FontHandle{};
					}),
				Properties::MakeWith<float>("FontSize", "Font Size",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().FontSize; },
					[](Entity& e, float v) {
						auto& text = e.GetComponent<TextRendererComponent>();
						text.FontSize = v;
						// Different px size means different atlas slot.
						text.ResolvedFont = FontHandle{};
					},
					Properties::Meta::Clamp(1.0, 512.0, 0.5f)),
				Properties::Make("Color", "Color", &TextRendererComponent::Color),
				Properties::MakeWith<float>("LetterSpacing", "Letter Spacing",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().LetterSpacing; },
					[](Entity& e, float v) {
						e.GetComponent<TextRendererComponent>().LetterSpacing = v;
					},
					Properties::Meta::Clamp(-64.0, 64.0, 0.1f)),
				Properties::MakeWith<TextAlignment>("Alignment", "Alignment",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().HAlign; },
					[](Entity& e, TextAlignment v) {
						e.GetComponent<TextRendererComponent>().HAlign = v;
					}),
				Properties::MakeWith<TextWrapMode>("WrapMode", "Wrap Mode",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().WrapMode; },
					[](Entity& e, TextWrapMode v) {
						e.GetComponent<TextRendererComponent>().WrapMode = v;
					}),
				// WrapWidth was removed — wrap area now comes entirely
				// from the host rect's width minus Margin (.x + .z). The
				// inspector field is no longer needed because Margin's
				// inner-rect gizmo already shows / drags the same number.
				// (Left, Top, Right, Bottom) inset in pixels. Drives the
				// editor-view margin gizmo; reduces wrap width in
				// GuiRenderer/TextRenderer so wrapped lines respect the
				// authored insets.
				Properties::MakeWith<Vec4>("Margin", "Margin",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().Margin; },
					[](Entity& e, const Vec4& v) {
						e.GetComponent<TextRendererComponent>().Margin = v;
					}),
				Properties::MakeWith<int16_t>("SortingOrder", "Sorting Order",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().SortingOrder; },
					[](Entity& e, int16_t v) {
						e.GetComponent<TextRendererComponent>().SortingOrder = v;
					}),
				Properties::MakeWith<uint8_t>("SortingLayer", "Sorting Layer",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().SortingLayer; },
					[](Entity& e, uint8_t v) {
						e.GetComponent<TextRendererComponent>().SortingLayer = v;
					}),
			});

		DeclareConflict<SpriteRendererComponent, TextRendererComponent>(sceneManager);
		DeclareConflict<ImageComponent, TextRendererComponent>(sceneManager);
		DeclareConflict<ParticleSystem2DComponent, TextRendererComponent>(sceneManager);

		// ── Physics (Box2D-backed) ──────────────────────────────────

		RegisterComponent<Rigidbody2DComponent>(sceneManager, "Rigidbody 2D",
			ComponentCategory::Component, "Physics", "Rigidbody2D",
			{
				Properties::MakeWith<Vec2>("Position", "Position",
					[](const Entity& e) { return e.GetComponent<Rigidbody2DComponent>().GetPosition(); },
					[](Entity& e, const Vec2& v) {
						e.GetComponent<Rigidbody2DComponent>().SetPosition(v);
					},
					Properties::Meta::ReadOnly()),
				Properties::MakeWith<Vec2>("Velocity", "Velocity",
					[](const Entity& e) { return e.GetComponent<Rigidbody2DComponent>().GetVelocity(); },
					[](Entity& e, const Vec2& v) {
						e.GetComponent<Rigidbody2DComponent>().SetVelocity(v);
					}),
				Properties::MakeWith<float>("Rotation", "Rotation",
					[](const Entity& e) { return e.GetComponent<Rigidbody2DComponent>().GetRotation(); },
					[](Entity& e, float v) {
						e.GetComponent<Rigidbody2DComponent>().SetRotation(v);
					},
					Properties::Meta::ReadOnly().WithDragSpeed(0.01f)),
				Properties::MakeWith<float>("GravityScale", "Gravity Scale",
					[](const Entity& e) { return e.GetComponent<Rigidbody2DComponent>().GetGravityScale(); },
					[](Entity& e, float v) {
						e.GetComponent<Rigidbody2DComponent>().SetGravityScale(v);
					},
					Properties::Meta::Clamp(0.0, 1.0)),
				Properties::MakeWith<BodyType>("BodyType", "Body Type",
					[](const Entity& e) {
						return e.GetComponent<Rigidbody2DComponent>().GetBodyType();
					},
					[](Entity& e, BodyType v) {
						e.GetComponent<Rigidbody2DComponent>().SetBodyType(v);
					}),
			});

		RegisterComponent<BoxCollider2DComponent>(sceneManager, "Box Collider 2D",
			ComponentCategory::Component, "Physics", "BoxCollider2D",
			{
				Properties::MakeWith<Vec2>("Offset", "Offset",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().GetCenter(); },
					[](Entity& e, const Vec2& v) {
						if (Scene* s = e.GetScene()) {
							e.GetComponent<BoxCollider2DComponent>().SetCenter(v, *s);
						}
					},
					Properties::Meta::DragSpeed(0.05f)),
				Properties::MakeWith<Vec2>("Size", "Size",
					[](const Entity& e) {
						const Scene* s = e.GetScene();
						if (!s) return Vec2{ 1.0f, 1.0f };
						return e.GetComponent<BoxCollider2DComponent>().GetLocalScale(*s);
					},
					[](Entity& e, const Vec2& localSize) {
						Scene* s = e.GetScene();
						if (!s) return;
						const Vec2 clamped{ std::max(localSize.x, 0.001f), std::max(localSize.y, 0.001f) };
						e.GetComponent<BoxCollider2DComponent>().SetScale(clamped, *s);
					},
					Properties::Meta::Clamp(0.001, 1000.0, 0.05f)),
				Properties::MakeWith<bool>("Sensor", "Sensor",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().IsSensor(); },
					[](Entity& e, bool v) {
						auto& col = e.GetComponent<BoxCollider2DComponent>();
						if (Scene* s = e.GetScene()) col.SetSensor(v, *s);
					}),
				Properties::MakeWith<bool>("ContactEvents", "Contact Events",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().CanRegisterContacts(); },
					[](Entity& e, bool v) {
						e.GetComponent<BoxCollider2DComponent>().SetRegisterContacts(v);
					}),
				Properties::MakeWith<bool>("Enabled", "Collider Enabled",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().IsEnabled(); },
					[](Entity& e, bool v) {
						e.GetComponent<BoxCollider2DComponent>().SetEnabled(v);
					}),
				Properties::MakeWith<float>("Friction", "Friction",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().GetFriction(); },
					[](Entity& e, float v) {
						e.GetComponent<BoxCollider2DComponent>().SetFriction(v);
					},
					Properties::Meta::Clamp(0.0, 10.0, 0.01f)),
				Properties::MakeWith<float>("Bounciness", "Bounciness",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().GetBounciness(); },
					[](Entity& e, float v) {
						e.GetComponent<BoxCollider2DComponent>().SetBounciness(v);
					},
					Properties::Meta::Clamp(0.0, 1.0, 0.01f)),
				Properties::MakeWith<uint64_t>("LayerMask", "Layer Mask",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().GetLayer(); },
					[](Entity& e, uint64_t v) {
						e.GetComponent<BoxCollider2DComponent>().SetLayer(v);
					}),
			});

		RegisterComponent<CircleCollider2DComponent>(sceneManager, "Circle Collider 2D",
			ComponentCategory::Component, "Physics", "CircleCollider2D",
			{
				Properties::MakeWith<Vec2>("Offset", "Offset",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().GetCenter(); },
					[](Entity& e, const Vec2& v) {
						if (Scene* s = e.GetScene()) {
							e.GetComponent<CircleCollider2DComponent>().SetCenter(v, *s);
						}
					},
					Properties::Meta::DragSpeed(0.05f)),
				Properties::MakeWith<float>("Radius", "Radius",
					[](const Entity& e) {
						const Scene* s = e.GetScene();
						if (!s) return 0.5f;
						return e.GetComponent<CircleCollider2DComponent>().GetLocalRadius(*s);
					},
					[](Entity& e, float radius) {
						Scene* s = e.GetScene();
						if (!s) return;
						const float clamped = std::max(radius, 0.001f);
						e.GetComponent<CircleCollider2DComponent>().SetRadius(clamped, *s);
					},
					Properties::Meta::Clamp(0.001, 1000.0, 0.05f)),
				Properties::MakeWith<bool>("Sensor", "Sensor",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().IsSensor(); },
					[](Entity& e, bool v) {
						auto& col = e.GetComponent<CircleCollider2DComponent>();
						if (Scene* s = e.GetScene()) col.SetSensor(v, *s);
					}),
				Properties::MakeWith<bool>("ContactEvents", "Contact Events",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().CanRegisterContacts(); },
					[](Entity& e, bool v) {
						e.GetComponent<CircleCollider2DComponent>().SetRegisterContacts(v);
					}),
				Properties::MakeWith<bool>("Enabled", "Collider Enabled",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().IsEnabled(); },
					[](Entity& e, bool v) {
						e.GetComponent<CircleCollider2DComponent>().SetEnabled(v);
					}),
				Properties::MakeWith<float>("Friction", "Friction",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().GetFriction(); },
					[](Entity& e, float v) {
						e.GetComponent<CircleCollider2DComponent>().SetFriction(v);
					},
					Properties::Meta::Clamp(0.0, 10.0, 0.01f)),
				Properties::MakeWith<float>("Bounciness", "Bounciness",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().GetBounciness(); },
					[](Entity& e, float v) {
						e.GetComponent<CircleCollider2DComponent>().SetBounciness(v);
					},
					Properties::Meta::Clamp(0.0, 1.0, 0.01f)),
				Properties::MakeWith<uint64_t>("LayerMask", "Layer Mask",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().GetLayer(); },
					[](Entity& e, uint64_t v) {
						e.GetComponent<CircleCollider2DComponent>().SetLayer(v);
					}),
			});

		RegisterComponent<PolygonCollider2DComponent>(sceneManager, "Polygon Collider 2D",
			ComponentCategory::Component, "Physics", "PolygonCollider2D",
			{
				Properties::MakeWith<Vec2>("Offset", "Offset",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().GetCenter(); },
					[](Entity& e, const Vec2& v) {
						if (Scene* s = e.GetScene()) {
							e.GetComponent<PolygonCollider2DComponent>().SetCenter(v, *s);
						}
					},
					Properties::Meta::DragSpeed(0.05f)),
				Properties::MakeWith<int>("Sides", "Sides",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().GetSides(); },
					[](Entity& e, int v) {
						Scene* s = e.GetScene();
						if (!s) return;
						const int clamped = std::clamp(v,
							PolygonCollider2DComponent::k_MinVertices,
							PolygonCollider2DComponent::k_MaxVertices);
						e.GetComponent<PolygonCollider2DComponent>().SetSides(clamped, *s);
					},
					Properties::Meta::Clamp(
						static_cast<double>(PolygonCollider2DComponent::k_MinVertices),
						static_cast<double>(PolygonCollider2DComponent::k_MaxVertices),
						1.0f)),
				Properties::MakeWith<Vec2>("Size", "Size",
					[](const Entity& e) {
						const Scene* s = e.GetScene();
						if (!s) return Vec2{ 1.0f, 1.0f };
						return e.GetComponent<PolygonCollider2DComponent>().GetLocalSize(*s);
					},
					[](Entity& e, const Vec2& localSize) {
						Scene* s = e.GetScene();
						if (!s) return;
						const Vec2 clamped{ std::max(localSize.x, 0.001f), std::max(localSize.y, 0.001f) };
						e.GetComponent<PolygonCollider2DComponent>().SetSize(clamped, *s);
					},
					Properties::Meta::Clamp(0.001, 1000.0, 0.05f)),
				Properties::MakeWith<bool>("Sensor", "Sensor",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().IsSensor(); },
					[](Entity& e, bool v) {
						auto& col = e.GetComponent<PolygonCollider2DComponent>();
						if (Scene* s = e.GetScene()) col.SetSensor(v, *s);
					}),
				Properties::MakeWith<bool>("ContactEvents", "Contact Events",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().CanRegisterContacts(); },
					[](Entity& e, bool v) {
						e.GetComponent<PolygonCollider2DComponent>().SetRegisterContacts(v);
					}),
				Properties::MakeWith<bool>("Enabled", "Collider Enabled",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().IsEnabled(); },
					[](Entity& e, bool v) {
						e.GetComponent<PolygonCollider2DComponent>().SetEnabled(v);
					}),
				Properties::MakeWith<float>("Friction", "Friction",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().GetFriction(); },
					[](Entity& e, float v) {
						e.GetComponent<PolygonCollider2DComponent>().SetFriction(v);
					},
					Properties::Meta::Clamp(0.0, 10.0, 0.01f)),
				Properties::MakeWith<float>("Bounciness", "Bounciness",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().GetBounciness(); },
					[](Entity& e, float v) {
						e.GetComponent<PolygonCollider2DComponent>().SetBounciness(v);
					},
					Properties::Meta::Clamp(0.0, 1.0, 0.01f)),
				Properties::MakeWith<uint64_t>("LayerMask", "Layer Mask",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().GetLayer(); },
					[](Entity& e, uint64_t v) {
						e.GetComponent<PolygonCollider2DComponent>().SetLayer(v);
					}),
			});

		// ── Physics (Axiom-Physics, lightweight AABB) ──────────────

		RegisterComponent<FastBody2DComponent>(sceneManager, "Fast Body 2D",
			ComponentCategory::Component, "Physics", "FastBody2D",
			{
				Properties::MakeWith<AxiomPhys::BodyType>("Type", "Body Type",
					[](const Entity& e) { return e.GetComponent<FastBody2DComponent>().Type; },
					[](Entity& e, AxiomPhys::BodyType v) {
						auto& body = e.GetComponent<FastBody2DComponent>();
						body.Type = v;
						if (body.m_Body) body.m_Body->SetBodyType(v);
					}),
				Properties::MakeWith<float>("Mass", "Mass",
					[](const Entity& e) { return e.GetComponent<FastBody2DComponent>().Mass; },
					[](Entity& e, float v) {
						auto& body = e.GetComponent<FastBody2DComponent>();
						body.Mass = v;
						if (body.m_Body) body.m_Body->SetMass(v);
					},
					Properties::Meta::Clamp(0.001, 10000.0, 0.1f)),
				Properties::MakeWith<bool>("UseGravity", "Use Gravity",
					[](const Entity& e) { return e.GetComponent<FastBody2DComponent>().UseGravity; },
					[](Entity& e, bool v) {
						auto& body = e.GetComponent<FastBody2DComponent>();
						body.UseGravity = v;
						if (body.m_Body) body.m_Body->SetGravityEnabled(v);
					}),
				Properties::MakeWith<bool>("BoundaryCheck", "Boundary Check",
					[](const Entity& e) { return e.GetComponent<FastBody2DComponent>().BoundaryCheck; },
					[](Entity& e, bool v) {
						auto& body = e.GetComponent<FastBody2DComponent>();
						body.BoundaryCheck = v;
						if (body.m_Body) body.m_Body->SetBoundaryCheckEnabled(v);
					}),
			});

		RegisterComponent<FastBoxCollider2DComponent>(sceneManager, "Fast Box Collider 2D",
			ComponentCategory::Component, "Physics", "FastBoxCollider2D",
			{
				Properties::MakeWith<Vec2>("HalfExtents", "Half Extents",
					[](const Entity& e) { return e.GetComponent<FastBoxCollider2DComponent>().HalfExtents; },
					[](Entity& e, const Vec2& v) {
						e.GetComponent<FastBoxCollider2DComponent>().SetHalfExtents(v);
					},
					Properties::Meta::DragSpeed(0.05f)),
			});

		RegisterComponent<FastCircleCollider2DComponent>(sceneManager, "Fast Circle Collider 2D",
			ComponentCategory::Component, "Physics", "FastCircleCollider2D",
			{
				Properties::MakeWith<float>("Radius", "Radius",
					[](const Entity& e) { return e.GetComponent<FastCircleCollider2DComponent>().Radius; },
					[](Entity& e, float v) {
						e.GetComponent<FastCircleCollider2DComponent>().SetRadius(v);
					},
					Properties::Meta::Clamp(0.01, 100.0, 0.05f)),
			});

		// ── Audio ───────────────────────────────────────────────────

		RegisterComponent<AudioSourceComponent>(sceneManager, "Audio Source",
			ComponentCategory::Component, "Audio", "AudioSource",
			{
				Properties::MakeWith<bool>("PlayOnAwake", "Play On Awake",
					[](const Entity& e) { return e.GetComponent<AudioSourceComponent>().GetPlayOnAwake(); },
					[](Entity& e, bool v) {
						e.GetComponent<AudioSourceComponent>().SetPlayOnAwake(v);
					}),
				Properties::MakeWith<float>("Volume", "Volume",
					[](const Entity& e) { return e.GetComponent<AudioSourceComponent>().GetVolume(); },
					[](Entity& e, float v) {
						e.GetComponent<AudioSourceComponent>().SetVolume(v);
					},
					Properties::Meta::Clamp(0.0, 1.0, 0.01f)),
				Properties::MakeWith<float>("Pitch", "Pitch",
					[](const Entity& e) { return e.GetComponent<AudioSourceComponent>().GetPitch(); },
					[](Entity& e, float v) {
						e.GetComponent<AudioSourceComponent>().SetPitch(v);
					},
					Properties::Meta::Clamp(0.1, 3.0, 0.01f)),
				Properties::MakeWith<bool>("Loop", "Loop",
					[](const Entity& e) { return e.GetComponent<AudioSourceComponent>().IsLooping(); },
					[](Entity& e, bool v) {
						e.GetComponent<AudioSourceComponent>().SetLoop(v);
					}),
				Properties::MakeAudioRef("Clip", "Clip",
					[](const Entity& e) -> uint64_t {
						const auto& src = e.GetComponent<AudioSourceComponent>();
						const AudioHandle h = src.GetAudioHandle();
						return h.IsValid() ? AudioManager::GetAudioAssetUUID(h) : 0ull;
					},
					[](Entity& e, uint64_t uuid) {
						auto& src = e.GetComponent<AudioSourceComponent>();
						const AudioHandle h = uuid != 0
							? AudioManager::LoadAudioByUUID(uuid)
							: AudioHandle{};
						src.SetAudioHandle(h, UUID(uuid));
					}),
			});

		// ── UI widgets ──────────────────────────────────────────────
		// All show up in the inspector's "UI" tab. The Interactable
		// component is the input-state primitive; Button/Slider/etc.
		// are visual presets that read it. Each carries a registry-driven
		// serialize / deserialize callback so they round-trip via .scene
		// without bloating SceneSerializerDeserialize.cpp.

		// Color (de)serialize uses the free helpers UIColorToJson /
		// UIColorFromJson at file scope — keeps the lambdas below
		// non-capturing so they decay to the raw function pointers
		// ComponentInfo::serialize / deserialize require.

		ComponentInfo interactableInfo{ "Interactable", "UI", ComponentCategory::Component };
		interactableInfo.serializedName = "Interactable";
		interactableInfo.properties = {
			Properties::Make("Interactable", "Interactable", &InteractableComponent::Interactable),
			// Opt-in keyboard / controller navigation. Defaults to false
			// so existing scenes are not silently included in Tab order.
			Properties::Make("Focusable",    "Focusable",    &InteractableComponent::Focusable),
		};
		interactableInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<InteractableComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("interactable", Json::Value(c.Interactable));
			v.AddMember("focusable",    Json::Value(c.Focusable));
			return v;
		};
		interactableInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<InteractableComponent>();
			if (const Json::Value* m = v.FindMember("interactable")) c.Interactable = m->AsBoolOr(c.Interactable);
			if (const Json::Value* m = v.FindMember("focusable"))    c.Focusable    = m->AsBoolOr(c.Focusable);
		};
		sceneManager.RegisterComponentType<InteractableComponent>(interactableInfo);

		ComponentInfo buttonInfo{ "Button", "UI", ComponentCategory::Component };
		buttonInfo.serializedName = "Button";
		buttonInfo.properties = {
			MakeUiEntityRef<ButtonComponent>("TargetGraphic", "Target Graphic",
				&ButtonComponent::TargetGraphic),
			// Variant: TransitionMode picks which per-state slots show.
			// Alpha == 0 on FocusedColor = no focus tint (see ButtonComponent.hpp).
			Properties::MakeVariant("TransitionMode", "Transition Mode",
				&ButtonComponent::TransitionMode,
				{
					Properties::Branch(UITransitionMode::ColorTint, {
						Properties::Make("NormalColor",   "Normal Color",   &ButtonComponent::NormalColor),
						Properties::Make("HoveredColor",  "Hovered Color",  &ButtonComponent::HoveredColor),
						Properties::Make("PressedColor",  "Pressed Color",  &ButtonComponent::PressedColor),
						Properties::Make("DisabledColor", "Disabled Color", &ButtonComponent::DisabledColor),
						Properties::Make("FocusedColor",  "Focused Color",  &ButtonComponent::FocusedColor),
					}),
					Properties::Branch(UITransitionMode::SpriteSwap, {
						MakeUiSpriteRef<ButtonComponent>("NormalSprite",   "Normal Sprite",   &ButtonComponent::NormalSprite),
						MakeUiSpriteRef<ButtonComponent>("HoveredSprite",  "Hovered Sprite",  &ButtonComponent::HoveredSprite),
						MakeUiSpriteRef<ButtonComponent>("PressedSprite",  "Pressed Sprite",  &ButtonComponent::PressedSprite),
						MakeUiSpriteRef<ButtonComponent>("DisabledSprite", "Disabled Sprite", &ButtonComponent::DisabledSprite),
						MakeUiSpriteRef<ButtonComponent>("FocusedSprite",  "Focused Sprite",  &ButtonComponent::FocusedSprite),
					}),
					Properties::Branch(UITransitionMode::None, {}),
				}),
		};
		buttonInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<ButtonComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("targetGraphic",  UIEntityHandleToJson(e, c.TargetGraphic));
			v.AddMember("transitionMode", Json::Value(static_cast<int>(c.TransitionMode)));
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			v.AddMember("focused",  UIColorToJson(c.FocusedColor));
			v.AddMember("normalSprite",   UIUuidToJson(c.NormalSprite));
			v.AddMember("hoveredSprite",  UIUuidToJson(c.HoveredSprite));
			v.AddMember("pressedSprite",  UIUuidToJson(c.PressedSprite));
			v.AddMember("disabledSprite", UIUuidToJson(c.DisabledSprite));
			v.AddMember("focusedSprite",  UIUuidToJson(c.FocusedSprite));

			// OnClick event list. UUID 0 == "self", same convention as
			// UIEntityHandleToJson at file scope.
			v.AddMember("onClick", UIEventListToJson(c.OnClick));
			return v;
		};
		buttonInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<ButtonComponent>();
			if (const Json::Value* m = v.FindMember("targetGraphic")) UIEntityHandleFromJson(e, *m, c.TargetGraphic);
			if (const Json::Value* m = v.FindMember("transitionMode")) c.TransitionMode = static_cast<UITransitionMode>(m->AsIntOr(static_cast<int>(c.TransitionMode)));
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
			if (const Json::Value* m = v.FindMember("focused"))  c.FocusedColor  = UIColorFromJson(*m, c.FocusedColor);
			if (const Json::Value* m = v.FindMember("normalSprite"))   c.NormalSprite   = UIUuidFromJson(*m, c.NormalSprite);
			if (const Json::Value* m = v.FindMember("hoveredSprite"))  c.HoveredSprite  = UIUuidFromJson(*m, c.HoveredSprite);
			if (const Json::Value* m = v.FindMember("pressedSprite"))  c.PressedSprite  = UIUuidFromJson(*m, c.PressedSprite);
			if (const Json::Value* m = v.FindMember("disabledSprite")) c.DisabledSprite = UIUuidFromJson(*m, c.DisabledSprite);
			if (const Json::Value* m = v.FindMember("focusedSprite"))  c.FocusedSprite  = UIUuidFromJson(*m, c.FocusedSprite);

			if (const Json::Value* m = v.FindMember("onClick")) {
				UIEventListFromJson(*m, c.OnClick);
			}
		};
		buttonInfo.onAdd = &InheritImageColorIntoNormal<ButtonComponent>;
		sceneManager.RegisterComponentType<ButtonComponent>(buttonInfo);

		ComponentInfo sliderInfo{ "Slider", "UI", ComponentCategory::Component };
		sliderInfo.serializedName = "Slider";
		sliderInfo.properties = {
			Properties::MakeWith<float>("Value", "Value",
				[](const Entity& e) { return e.GetComponent<SliderComponent>().Value; },
				[](Entity& e, float v) { e.GetComponent<SliderComponent>().Value = v; },
				Properties::Meta::DragSpeed(0.01f)),
			Properties::Make("Direction",    "Direction",     &SliderComponent::Direction),
			Properties::Make("MinValue",     "Min Value",     &SliderComponent::MinValue),
			Properties::Make("MaxValue",     "Max Value",     &SliderComponent::MaxValue),
			Properties::Make("WholeNumbers", "Whole Numbers", &SliderComponent::WholeNumbers),
			Properties::Make("IsReadOnly",   "Read Only",     &SliderComponent::IsReadOnly),
			MakeUiEntityRef<SliderComponent>("BackgroundEntity", "Background Rect", &SliderComponent::BackgroundEntity),
			MakeUiEntityRef<SliderComponent>("FillEntity",       "Fill Rect",       &SliderComponent::FillEntity),
			MakeUiEntityRef<SliderComponent>("HandleEntity",     "Handle Rect",     &SliderComponent::HandleEntity),
			MakeUiEntityRef<SliderComponent>("LabelEntity",      "Label",           &SliderComponent::LabelEntity),
			Properties::MakeVariant("TransitionMode", "Transition Mode",
				&SliderComponent::TransitionMode,
				{
					Properties::Branch(UITransitionMode::ColorTint, {
						Properties::Make("NormalColor",   "Normal Color",   &SliderComponent::NormalColor),
						Properties::Make("HoveredColor",  "Hovered Color",  &SliderComponent::HoveredColor),
						Properties::Make("PressedColor",  "Pressed Color",  &SliderComponent::PressedColor),
						Properties::Make("DisabledColor", "Disabled Color", &SliderComponent::DisabledColor),
						Properties::Make("FocusedColor",  "Focused Color",  &SliderComponent::FocusedColor),
					}),
					Properties::Branch(UITransitionMode::SpriteSwap, {
						MakeUiSpriteRef<SliderComponent>("NormalSprite",   "Normal Sprite",   &SliderComponent::NormalSprite),
						MakeUiSpriteRef<SliderComponent>("HoveredSprite",  "Hovered Sprite",  &SliderComponent::HoveredSprite),
						MakeUiSpriteRef<SliderComponent>("PressedSprite",  "Pressed Sprite",  &SliderComponent::PressedSprite),
						MakeUiSpriteRef<SliderComponent>("DisabledSprite", "Disabled Sprite", &SliderComponent::DisabledSprite),
						MakeUiSpriteRef<SliderComponent>("FocusedSprite",  "Focused Sprite",  &SliderComponent::FocusedSprite),
					}),
					Properties::Branch(UITransitionMode::None, {}),
				}),
		};
		sliderInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<SliderComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("value",        Json::Value(c.Value));
			v.AddMember("direction",    Json::Value(static_cast<int>(c.Direction)));
			v.AddMember("min",          Json::Value(c.MinValue));
			v.AddMember("max",          Json::Value(c.MaxValue));
			v.AddMember("wholeNumbers", Json::Value(c.WholeNumbers));
			v.AddMember("isReadOnly",   Json::Value(c.IsReadOnly));
			v.AddMember("backgroundEntity", UIEntityHandleToJson(e, c.BackgroundEntity));
			v.AddMember("fillEntity",       UIEntityHandleToJson(e, c.FillEntity));
			v.AddMember("handleEntity",     UIEntityHandleToJson(e, c.HandleEntity));
			v.AddMember("labelEntity",      UIEntityHandleToJson(e, c.LabelEntity));
			v.AddMember("transitionMode", Json::Value(static_cast<int>(c.TransitionMode)));
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			v.AddMember("focused",  UIColorToJson(c.FocusedColor));
			v.AddMember("normalSprite",   UIUuidToJson(c.NormalSprite));
			v.AddMember("hoveredSprite",  UIUuidToJson(c.HoveredSprite));
			v.AddMember("pressedSprite",  UIUuidToJson(c.PressedSprite));
			v.AddMember("disabledSprite", UIUuidToJson(c.DisabledSprite));
			v.AddMember("focusedSprite",  UIUuidToJson(c.FocusedSprite));
			v.AddMember("onValueChanged", UIEventListToJson(c.OnValueChanged));
			return v;
		};
		sliderInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<SliderComponent>();
			if (const Json::Value* m = v.FindMember("value"))        c.Value        = static_cast<float>(m->AsDoubleOr(c.Value));
			if (const Json::Value* m = v.FindMember("direction"))    c.Direction    = static_cast<SliderDirection>(m->AsIntOr(static_cast<int>(c.Direction)));
			if (const Json::Value* m = v.FindMember("min"))          c.MinValue     = static_cast<float>(m->AsDoubleOr(c.MinValue));
			if (const Json::Value* m = v.FindMember("max"))          c.MaxValue     = static_cast<float>(m->AsDoubleOr(c.MaxValue));
			if (const Json::Value* m = v.FindMember("wholeNumbers")) c.WholeNumbers = m->AsBoolOr(c.WholeNumbers);
			if (const Json::Value* m = v.FindMember("isReadOnly"))   c.IsReadOnly   = m->AsBoolOr(c.IsReadOnly);
			if (const Json::Value* m = v.FindMember("backgroundEntity")) UIEntityHandleFromJson(e, *m, c.BackgroundEntity);
			if (const Json::Value* m = v.FindMember("fillEntity"))       UIEntityHandleFromJson(e, *m, c.FillEntity);
			if (const Json::Value* m = v.FindMember("handleEntity"))     UIEntityHandleFromJson(e, *m, c.HandleEntity);
			if (const Json::Value* m = v.FindMember("labelEntity"))      UIEntityHandleFromJson(e, *m, c.LabelEntity);
			if (const Json::Value* m = v.FindMember("transitionMode")) c.TransitionMode = static_cast<UITransitionMode>(m->AsIntOr(static_cast<int>(c.TransitionMode)));
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
			if (const Json::Value* m = v.FindMember("focused"))  c.FocusedColor  = UIColorFromJson(*m, c.FocusedColor);
			if (const Json::Value* m = v.FindMember("normalSprite"))   c.NormalSprite   = UIUuidFromJson(*m, c.NormalSprite);
			if (const Json::Value* m = v.FindMember("hoveredSprite"))  c.HoveredSprite  = UIUuidFromJson(*m, c.HoveredSprite);
			if (const Json::Value* m = v.FindMember("pressedSprite"))  c.PressedSprite  = UIUuidFromJson(*m, c.PressedSprite);
			if (const Json::Value* m = v.FindMember("disabledSprite")) c.DisabledSprite = UIUuidFromJson(*m, c.DisabledSprite);
			if (const Json::Value* m = v.FindMember("focusedSprite"))  c.FocusedSprite  = UIUuidFromJson(*m, c.FocusedSprite);
			if (const Json::Value* m = v.FindMember("onValueChanged")) UIEventListFromJson(*m, c.OnValueChanged);
		};
		sliderInfo.onAdd = &InheritImageColorIntoNormal<SliderComponent>;
		sceneManager.RegisterComponentType<SliderComponent>(sliderInfo);

		ComponentInfo inputFieldInfo{ "Input Field", "UI", ComponentCategory::Component };
		inputFieldInfo.serializedName = "InputField";
		PropertyMetadata caretBlinkMeta = Properties::Meta::Clamp(0.0, 5.0, 0.05f);
		PropertyMetadata caretWidthMeta = Properties::Meta::Clamp(1.0, 5.0, 0.05f);
		inputFieldInfo.properties = {
			Properties::Make("Text",            "Text",            &InputFieldComponent::Text),
			Properties::Make("PlaceholderText", "Placeholder",     &InputFieldComponent::PlaceholderText),
			MakeUiEntityRef<InputFieldComponent>("TextEntity", "Text Entity", &InputFieldComponent::TextEntity),
			Properties::Make("CharacterLimit",  "Character Limit", &InputFieldComponent::CharacterLimit),
			Properties::Make("ContentType",     "Content Type",    &InputFieldComponent::ContentType),
			Properties::Make("IsSecret",        "Secret",          &InputFieldComponent::IsSecret),
			Properties::Make("IsReadOnly",      "Read Only",       &InputFieldComponent::IsReadOnly),
			Properties::Make("CaretBlinkRate",  "Caret Blink Rate (Hz)", &InputFieldComponent::CaretBlinkRate, caretBlinkMeta),
			Properties::Make("CaretWidth",      "Caret Width",     &InputFieldComponent::CaretWidth, caretWidthMeta),
			Properties::Make("TextColor",       "Text Color",      &InputFieldComponent::TextColor),
			Properties::Make("PlaceholderColor","Placeholder Color", &InputFieldComponent::PlaceholderColor),
			Properties::Make("CaretColor",      "Caret Color",     &InputFieldComponent::CaretColor),
			Properties::Make("SelectionColor",  "Selection Color", &InputFieldComponent::SelectionColor),
			Properties::MakeVariant("TransitionMode", "Transition Mode",
				&InputFieldComponent::TransitionMode,
				{
					Properties::Branch(UITransitionMode::ColorTint, {
						Properties::Make("NormalColor",   "Normal Color",   &InputFieldComponent::NormalColor),
						Properties::Make("HoveredColor",  "Hovered Color",  &InputFieldComponent::HoveredColor),
						Properties::Make("PressedColor",  "Pressed Color",  &InputFieldComponent::PressedColor),
						Properties::Make("DisabledColor", "Disabled Color", &InputFieldComponent::DisabledColor),
						Properties::Make("FocusedColor",  "Focused Color",  &InputFieldComponent::FocusedColor),
					}),
					Properties::Branch(UITransitionMode::SpriteSwap, {
						MakeUiSpriteRef<InputFieldComponent>("NormalSprite",   "Normal Sprite",   &InputFieldComponent::NormalSprite),
						MakeUiSpriteRef<InputFieldComponent>("HoveredSprite",  "Hovered Sprite",  &InputFieldComponent::HoveredSprite),
						MakeUiSpriteRef<InputFieldComponent>("PressedSprite",  "Pressed Sprite",  &InputFieldComponent::PressedSprite),
						MakeUiSpriteRef<InputFieldComponent>("DisabledSprite", "Disabled Sprite", &InputFieldComponent::DisabledSprite),
						MakeUiSpriteRef<InputFieldComponent>("FocusedSprite",  "Focused Sprite",  &InputFieldComponent::FocusedSprite),
					}),
					Properties::Branch(UITransitionMode::None, {}),
				}),
		};
		inputFieldInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<InputFieldComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("text",             Json::Value(c.Text));
			v.AddMember("placeholder",      Json::Value(c.PlaceholderText));
			v.AddMember("textEntity",       UIEntityHandleToJson(e, c.TextEntity));
			v.AddMember("characterLimit",   Json::Value(c.CharacterLimit));
			v.AddMember("contentType",      Json::Value(static_cast<int>(c.ContentType)));
			v.AddMember("isSecret",         Json::Value(c.IsSecret));
			v.AddMember("isReadOnly",       Json::Value(c.IsReadOnly));
			v.AddMember("caretBlinkRate",   Json::Value(c.CaretBlinkRate));
			v.AddMember("caretWidth",       Json::Value(c.CaretWidth));
			v.AddMember("textColor",        UIColorToJson(c.TextColor));
			v.AddMember("placeholderColor", UIColorToJson(c.PlaceholderColor));
			v.AddMember("caretColor",       UIColorToJson(c.CaretColor));
			v.AddMember("selectionColor",   UIColorToJson(c.SelectionColor));
			v.AddMember("transitionMode", Json::Value(static_cast<int>(c.TransitionMode)));
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			v.AddMember("focused",  UIColorToJson(c.FocusedColor));
			v.AddMember("normalSprite",   UIUuidToJson(c.NormalSprite));
			v.AddMember("hoveredSprite",  UIUuidToJson(c.HoveredSprite));
			v.AddMember("pressedSprite",  UIUuidToJson(c.PressedSprite));
			v.AddMember("disabledSprite", UIUuidToJson(c.DisabledSprite));
			v.AddMember("focusedSprite",  UIUuidToJson(c.FocusedSprite));
			v.AddMember("onValueChanged", UIEventListToJson(c.OnValueChanged));
			v.AddMember("onSubmitted",    UIEventListToJson(c.OnSubmitted));
			return v;
		};
		inputFieldInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<InputFieldComponent>();
			if (const Json::Value* m = v.FindMember("text"))             c.Text             = m->AsStringOr(c.Text);
			if (const Json::Value* m = v.FindMember("placeholder"))      c.PlaceholderText  = m->AsStringOr(c.PlaceholderText);
			if (const Json::Value* m = v.FindMember("textEntity"))       UIEntityHandleFromJson(e, *m, c.TextEntity);
			if (const Json::Value* m = v.FindMember("characterLimit"))   c.CharacterLimit   = m->AsIntOr(c.CharacterLimit);
			if (const Json::Value* m = v.FindMember("contentType"))      c.ContentType      = static_cast<InputContentType>(m->AsIntOr(static_cast<int>(c.ContentType)));
			if (const Json::Value* m = v.FindMember("isSecret"))         c.IsSecret         = m->AsBoolOr(c.IsSecret);
			if (const Json::Value* m = v.FindMember("isReadOnly"))       c.IsReadOnly       = m->AsBoolOr(c.IsReadOnly);
			if (const Json::Value* m = v.FindMember("caretBlinkRate"))   c.CaretBlinkRate   = static_cast<float>(m->AsDoubleOr(c.CaretBlinkRate));
			if (const Json::Value* m = v.FindMember("caretWidth"))       c.CaretWidth       = static_cast<float>(m->AsDoubleOr(c.CaretWidth));
			if (const Json::Value* m = v.FindMember("textColor"))        c.TextColor        = UIColorFromJson(*m, c.TextColor);
			if (const Json::Value* m = v.FindMember("placeholderColor")) c.PlaceholderColor = UIColorFromJson(*m, c.PlaceholderColor);
			if (const Json::Value* m = v.FindMember("caretColor"))       c.CaretColor       = UIColorFromJson(*m, c.CaretColor);
			if (const Json::Value* m = v.FindMember("selectionColor"))   c.SelectionColor   = UIColorFromJson(*m, c.SelectionColor);
			if (const Json::Value* m = v.FindMember("transitionMode")) c.TransitionMode = static_cast<UITransitionMode>(m->AsIntOr(static_cast<int>(c.TransitionMode)));
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
			if (const Json::Value* m = v.FindMember("focused"))  c.FocusedColor  = UIColorFromJson(*m, c.FocusedColor);
			if (const Json::Value* m = v.FindMember("normalSprite"))   c.NormalSprite   = UIUuidFromJson(*m, c.NormalSprite);
			if (const Json::Value* m = v.FindMember("hoveredSprite"))  c.HoveredSprite  = UIUuidFromJson(*m, c.HoveredSprite);
			if (const Json::Value* m = v.FindMember("pressedSprite"))  c.PressedSprite  = UIUuidFromJson(*m, c.PressedSprite);
			if (const Json::Value* m = v.FindMember("disabledSprite")) c.DisabledSprite = UIUuidFromJson(*m, c.DisabledSprite);
			if (const Json::Value* m = v.FindMember("focusedSprite"))  c.FocusedSprite  = UIUuidFromJson(*m, c.FocusedSprite);
			if (const Json::Value* m = v.FindMember("onValueChanged")) UIEventListFromJson(*m, c.OnValueChanged);
			if (const Json::Value* m = v.FindMember("onSubmitted"))    UIEventListFromJson(*m, c.OnSubmitted);
		};
		inputFieldInfo.onAdd = &InheritImageColorIntoNormal<InputFieldComponent>;
		sceneManager.RegisterComponentType<InputFieldComponent>(inputFieldInfo);

		ComponentInfo dropdownInfo{ "Dropdown", "UI", ComponentCategory::Component };
		dropdownInfo.serializedName = "Dropdown";
		dropdownInfo.properties = {
			Properties::Make("SelectedIndex", "Selected Index", &DropdownComponent::SelectedIndex),
			// Real list-of-strings editor — each entry gets its own
			// row with a delete button + an "Add" button. Replaces
			// the older "join on newlines" hack.
			Properties::MakeStringList("Options", "Options",
				[](const Entity& e) -> std::vector<std::string> {
					return e.GetComponent<DropdownComponent>().Options;
				},
				[](Entity& e, const std::vector<std::string>& items) {
					auto& dd = e.GetComponent<DropdownComponent>();
					dd.Options = items;
					// Keep SelectedIndex inside the new range so the
					// closed header doesn't display a stale row, and
					// don't crash the rendering path on empty lists.
					if (dd.Options.empty()) {
						dd.SelectedIndex = 0;
					}
					else {
						if (dd.SelectedIndex < 0) dd.SelectedIndex = 0;
						if (dd.SelectedIndex >= static_cast<int>(dd.Options.size())) {
							dd.SelectedIndex = static_cast<int>(dd.Options.size()) - 1;
						}
					}
				}),
			Properties::Make("IsReadOnly",      "Read Only",      &DropdownComponent::IsReadOnly),
			MakeUiEntityRef<DropdownComponent>("LabelEntity", "Label Entity", &DropdownComponent::LabelEntity),
			Properties::Make("OptionRowHeight", "Row Height", &DropdownComponent::OptionRowHeight),
			Properties::Make("PopupBackgroundColor", "Popup Background", &DropdownComponent::PopupBackgroundColor),
			Properties::Make("OptionTextColor",      "Option Text Color", &DropdownComponent::OptionTextColor),
			Properties::Make("OptionNormalColor",    "Option Normal Color (override)",  &DropdownComponent::OptionNormalColor),
			Properties::Make("OptionHoverColor",     "Option Hover Color",   &DropdownComponent::OptionHoverColor),
			Properties::Make("OptionPressedColor",   "Option Pressed Color (override)", &DropdownComponent::OptionPressedColor),
			Properties::Make("OptionSelectedColor",  "Option Selected Color (override)",&DropdownComponent::OptionSelectedColor),
			Properties::MakeVariant("TransitionMode", "Transition Mode",
				&DropdownComponent::TransitionMode,
				{
					Properties::Branch(UITransitionMode::ColorTint, {
						Properties::Make("NormalColor",   "Normal Color",   &DropdownComponent::NormalColor),
						Properties::Make("HoveredColor",  "Hovered Color",  &DropdownComponent::HoveredColor),
						Properties::Make("PressedColor",  "Pressed Color",  &DropdownComponent::PressedColor),
						Properties::Make("DisabledColor", "Disabled Color", &DropdownComponent::DisabledColor),
						Properties::Make("FocusedColor",  "Focused Color",  &DropdownComponent::FocusedColor),
					}),
					Properties::Branch(UITransitionMode::SpriteSwap, {
						MakeUiSpriteRef<DropdownComponent>("NormalSprite",   "Normal Sprite",   &DropdownComponent::NormalSprite),
						MakeUiSpriteRef<DropdownComponent>("HoveredSprite",  "Hovered Sprite",  &DropdownComponent::HoveredSprite),
						MakeUiSpriteRef<DropdownComponent>("PressedSprite",  "Pressed Sprite",  &DropdownComponent::PressedSprite),
						MakeUiSpriteRef<DropdownComponent>("DisabledSprite", "Disabled Sprite", &DropdownComponent::DisabledSprite),
						MakeUiSpriteRef<DropdownComponent>("FocusedSprite",  "Focused Sprite",  &DropdownComponent::FocusedSprite),
					}),
					Properties::Branch(UITransitionMode::None, {}),
				}),
		};
		dropdownInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<DropdownComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("selectedIndex",   Json::Value(c.SelectedIndex));
			v.AddMember("isReadOnly",      Json::Value(c.IsReadOnly));
			v.AddMember("labelEntity",     UIEntityHandleToJson(e, c.LabelEntity));
			v.AddMember("rowHeight",       Json::Value(c.OptionRowHeight));
			v.AddMember("popupBackground", UIColorToJson(c.PopupBackgroundColor));
			v.AddMember("optionTextColor", UIColorToJson(c.OptionTextColor));
			v.AddMember("optionNormalColor",   UIColorToJson(c.OptionNormalColor));
			v.AddMember("optionHoverColor",    UIColorToJson(c.OptionHoverColor));
			v.AddMember("optionPressedColor",  UIColorToJson(c.OptionPressedColor));
			v.AddMember("optionSelectedColor", UIColorToJson(c.OptionSelectedColor));
			v.AddMember("transitionMode", Json::Value(static_cast<int>(c.TransitionMode)));
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			v.AddMember("focused",  UIColorToJson(c.FocusedColor));
			v.AddMember("normalSprite",   UIUuidToJson(c.NormalSprite));
			v.AddMember("hoveredSprite",  UIUuidToJson(c.HoveredSprite));
			v.AddMember("pressedSprite",  UIUuidToJson(c.PressedSprite));
			v.AddMember("disabledSprite", UIUuidToJson(c.DisabledSprite));
			v.AddMember("focusedSprite",  UIUuidToJson(c.FocusedSprite));
			Json::Value optionsArr = Json::Value::MakeArray();
			for (const std::string& opt : c.Options) {
				optionsArr.Append(Json::Value(opt));
			}
			v.AddMember("options", std::move(optionsArr));
			v.AddMember("onValueChanged", UIEventListToJson(c.OnValueChanged));
			return v;
		};
		dropdownInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<DropdownComponent>();
			if (const Json::Value* m = v.FindMember("selectedIndex"))   c.SelectedIndex        = m->AsIntOr(c.SelectedIndex);
			if (const Json::Value* m = v.FindMember("isReadOnly"))      c.IsReadOnly           = m->AsBoolOr(c.IsReadOnly);
			if (const Json::Value* m = v.FindMember("labelEntity"))     UIEntityHandleFromJson(e, *m, c.LabelEntity);
			if (const Json::Value* m = v.FindMember("rowHeight"))       c.OptionRowHeight      = static_cast<float>(m->AsDoubleOr(c.OptionRowHeight));
			if (const Json::Value* m = v.FindMember("popupBackground")) c.PopupBackgroundColor = UIColorFromJson(*m, c.PopupBackgroundColor);
			if (const Json::Value* m = v.FindMember("optionTextColor")) c.OptionTextColor      = UIColorFromJson(*m, c.OptionTextColor);
			if (const Json::Value* m = v.FindMember("optionNormalColor"))   c.OptionNormalColor   = UIColorFromJson(*m, c.OptionNormalColor);
			if (const Json::Value* m = v.FindMember("optionHoverColor"))    c.OptionHoverColor    = UIColorFromJson(*m, c.OptionHoverColor);
			if (const Json::Value* m = v.FindMember("optionPressedColor"))  c.OptionPressedColor  = UIColorFromJson(*m, c.OptionPressedColor);
			if (const Json::Value* m = v.FindMember("optionSelectedColor"))c.OptionSelectedColor  = UIColorFromJson(*m, c.OptionSelectedColor);
			if (const Json::Value* m = v.FindMember("transitionMode")) c.TransitionMode = static_cast<UITransitionMode>(m->AsIntOr(static_cast<int>(c.TransitionMode)));
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
			if (const Json::Value* m = v.FindMember("focused"))  c.FocusedColor  = UIColorFromJson(*m, c.FocusedColor);
			if (const Json::Value* m = v.FindMember("normalSprite"))   c.NormalSprite   = UIUuidFromJson(*m, c.NormalSprite);
			if (const Json::Value* m = v.FindMember("hoveredSprite"))  c.HoveredSprite  = UIUuidFromJson(*m, c.HoveredSprite);
			if (const Json::Value* m = v.FindMember("pressedSprite"))  c.PressedSprite  = UIUuidFromJson(*m, c.PressedSprite);
			if (const Json::Value* m = v.FindMember("disabledSprite")) c.DisabledSprite = UIUuidFromJson(*m, c.DisabledSprite);
			if (const Json::Value* m = v.FindMember("focusedSprite"))  c.FocusedSprite  = UIUuidFromJson(*m, c.FocusedSprite);
			if (const Json::Value* m = v.FindMember("options"); m && m->IsArray()) {
				c.Options.clear();
				for (const Json::Value& item : m->GetArray()) {
					c.Options.push_back(item.AsStringOr(""));
				}
			}
			if (const Json::Value* m = v.FindMember("onValueChanged")) UIEventListFromJson(*m, c.OnValueChanged);
		};
		dropdownInfo.onAdd = &InheritImageColorIntoNormal<DropdownComponent>;
		sceneManager.RegisterComponentType<DropdownComponent>(dropdownInfo);

		ComponentInfo toggleInfo{ "Toggle", "UI", ComponentCategory::Component };
		toggleInfo.serializedName = "Toggle";
		toggleInfo.properties = {
			Properties::Make("IsOn",       "Is On",     &ToggleComponent::IsOn),
			Properties::Make("IsReadOnly", "Read Only", &ToggleComponent::IsReadOnly),
			MakeUiEntityRef<ToggleComponent>("CheckmarkEntity", "Graphic Entity", &ToggleComponent::CheckmarkEntity),
			Properties::MakeVariant("TransitionMode", "Transition Mode",
				&ToggleComponent::TransitionMode,
				{
					Properties::Branch(UITransitionMode::ColorTint, {
						Properties::Make("NormalColor",   "Normal Color",   &ToggleComponent::NormalColor),
						Properties::Make("HoveredColor",  "Hovered Color",  &ToggleComponent::HoveredColor),
						Properties::Make("PressedColor",  "Pressed Color",  &ToggleComponent::PressedColor),
						Properties::Make("DisabledColor", "Disabled Color", &ToggleComponent::DisabledColor),
						Properties::Make("FocusedColor",  "Focused Color",  &ToggleComponent::FocusedColor),
					}),
					Properties::Branch(UITransitionMode::SpriteSwap, {
						MakeUiSpriteRef<ToggleComponent>("NormalSprite",   "Normal Sprite",   &ToggleComponent::NormalSprite),
						MakeUiSpriteRef<ToggleComponent>("HoveredSprite",  "Hovered Sprite",  &ToggleComponent::HoveredSprite),
						MakeUiSpriteRef<ToggleComponent>("PressedSprite",  "Pressed Sprite",  &ToggleComponent::PressedSprite),
						MakeUiSpriteRef<ToggleComponent>("DisabledSprite", "Disabled Sprite", &ToggleComponent::DisabledSprite),
						MakeUiSpriteRef<ToggleComponent>("FocusedSprite",  "Focused Sprite",  &ToggleComponent::FocusedSprite),
					}),
					Properties::Branch(UITransitionMode::None, {}),
				}),
		};
		toggleInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<ToggleComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("isOn",        Json::Value(c.IsOn));
			v.AddMember("isReadOnly",  Json::Value(c.IsReadOnly));
			v.AddMember("checkmarkEntity", UIEntityHandleToJson(e, c.CheckmarkEntity));
			v.AddMember("transitionMode", Json::Value(static_cast<int>(c.TransitionMode)));
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			v.AddMember("focused",  UIColorToJson(c.FocusedColor));
			v.AddMember("normalSprite",   UIUuidToJson(c.NormalSprite));
			v.AddMember("hoveredSprite",  UIUuidToJson(c.HoveredSprite));
			v.AddMember("pressedSprite",  UIUuidToJson(c.PressedSprite));
			v.AddMember("disabledSprite", UIUuidToJson(c.DisabledSprite));
			v.AddMember("focusedSprite",  UIUuidToJson(c.FocusedSprite));
			v.AddMember("onValueChanged", UIEventListToJson(c.OnValueChanged));
			return v;
		};
		toggleInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<ToggleComponent>();
			if (const Json::Value* m = v.FindMember("isOn"))       c.IsOn       = m->AsBoolOr(c.IsOn);
			if (const Json::Value* m = v.FindMember("isReadOnly")) c.IsReadOnly = m->AsBoolOr(c.IsReadOnly);
			if (const Json::Value* m = v.FindMember("checkmarkEntity")) UIEntityHandleFromJson(e, *m, c.CheckmarkEntity);
			if (const Json::Value* m = v.FindMember("transitionMode")) c.TransitionMode = static_cast<UITransitionMode>(m->AsIntOr(static_cast<int>(c.TransitionMode)));
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
			if (const Json::Value* m = v.FindMember("focused"))  c.FocusedColor  = UIColorFromJson(*m, c.FocusedColor);
			if (const Json::Value* m = v.FindMember("normalSprite"))   c.NormalSprite   = UIUuidFromJson(*m, c.NormalSprite);
			if (const Json::Value* m = v.FindMember("hoveredSprite"))  c.HoveredSprite  = UIUuidFromJson(*m, c.HoveredSprite);
			if (const Json::Value* m = v.FindMember("pressedSprite"))  c.PressedSprite  = UIUuidFromJson(*m, c.PressedSprite);
			if (const Json::Value* m = v.FindMember("disabledSprite")) c.DisabledSprite = UIUuidFromJson(*m, c.DisabledSprite);
			if (const Json::Value* m = v.FindMember("focusedSprite"))  c.FocusedSprite  = UIUuidFromJson(*m, c.FocusedSprite);
			if (const Json::Value* m = v.FindMember("onValueChanged")) UIEventListFromJson(*m, c.OnValueChanged);
		};
		toggleInfo.onAdd = &InheritImageColorIntoNormal<ToggleComponent>;
		sceneManager.RegisterComponentType<ToggleComponent>(toggleInfo);

		ComponentInfo scrollbarInfo{ "Scrollbar", "UI", ComponentCategory::Component };
		scrollbarInfo.serializedName = "Scrollbar";
		PropertyMetadata scrollbarValueMeta = Properties::Meta::Clamp(0.0, 1.0, 0.01f);
		PropertyMetadata scrollbarSizeMeta  = Properties::Meta::Clamp(0.0, 1.0, 0.01f);
		PropertyMetadata scrollbarStepsMeta = Properties::Meta::Clamp(0.0, 11.0);
		scrollbarInfo.properties = {
			MakeUiEntityRef<ScrollbarComponent>("HandleEntity", "Handle Rect", &ScrollbarComponent::HandleEntity),
			Properties::Make("Direction",     "Direction",      &ScrollbarComponent::Direction),
			Properties::Make("Value",         "Value",          &ScrollbarComponent::Value, scrollbarValueMeta),
			Properties::Make("Size",          "Size",           &ScrollbarComponent::Size, scrollbarSizeMeta),
			Properties::Make("NumberOfSteps", "Number Of Steps",&ScrollbarComponent::NumberOfSteps, scrollbarStepsMeta),
			Properties::Make("IsReadOnly",    "Read Only",      &ScrollbarComponent::IsReadOnly),
			Properties::MakeVariant("TransitionMode", "Transition Mode",
				&ScrollbarComponent::TransitionMode,
				{
					Properties::Branch(UITransitionMode::ColorTint, {
						Properties::Make("NormalColor",   "Normal Color",   &ScrollbarComponent::NormalColor),
						Properties::Make("HoveredColor",  "Hovered Color",  &ScrollbarComponent::HoveredColor),
						Properties::Make("PressedColor",  "Pressed Color",  &ScrollbarComponent::PressedColor),
						Properties::Make("DisabledColor", "Disabled Color", &ScrollbarComponent::DisabledColor),
						Properties::Make("FocusedColor",  "Focused Color",  &ScrollbarComponent::FocusedColor),
					}),
					Properties::Branch(UITransitionMode::SpriteSwap, {
						MakeUiSpriteRef<ScrollbarComponent>("NormalSprite",   "Normal Sprite",   &ScrollbarComponent::NormalSprite),
						MakeUiSpriteRef<ScrollbarComponent>("HoveredSprite",  "Hovered Sprite",  &ScrollbarComponent::HoveredSprite),
						MakeUiSpriteRef<ScrollbarComponent>("PressedSprite",  "Pressed Sprite",  &ScrollbarComponent::PressedSprite),
						MakeUiSpriteRef<ScrollbarComponent>("DisabledSprite", "Disabled Sprite", &ScrollbarComponent::DisabledSprite),
						MakeUiSpriteRef<ScrollbarComponent>("FocusedSprite",  "Focused Sprite",  &ScrollbarComponent::FocusedSprite),
					}),
					Properties::Branch(UITransitionMode::None, {}),
				}),
		};
		scrollbarInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<ScrollbarComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("handleEntity", UIEntityHandleToJson(e, c.HandleEntity));
			v.AddMember("direction",      Json::Value(static_cast<int>(c.Direction)));
			v.AddMember("value",          Json::Value(c.Value));
			v.AddMember("size",           Json::Value(c.Size));
			v.AddMember("numberOfSteps",  Json::Value(c.NumberOfSteps));
			v.AddMember("isReadOnly",     Json::Value(c.IsReadOnly));
			v.AddMember("transitionMode", Json::Value(static_cast<int>(c.TransitionMode)));
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			v.AddMember("focused",  UIColorToJson(c.FocusedColor));
			v.AddMember("normalSprite",   UIUuidToJson(c.NormalSprite));
			v.AddMember("hoveredSprite",  UIUuidToJson(c.HoveredSprite));
			v.AddMember("pressedSprite",  UIUuidToJson(c.PressedSprite));
			v.AddMember("disabledSprite", UIUuidToJson(c.DisabledSprite));
			v.AddMember("focusedSprite",  UIUuidToJson(c.FocusedSprite));
			return v;
		};
		scrollbarInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<ScrollbarComponent>();
			if (const Json::Value* m = v.FindMember("handleEntity")) UIEntityHandleFromJson(e, *m, c.HandleEntity);
			if (const Json::Value* m = v.FindMember("direction"))     c.Direction     = static_cast<ScrollbarDirection>(m->AsIntOr(static_cast<int>(c.Direction)));
			if (const Json::Value* m = v.FindMember("value"))         c.Value         = static_cast<float>(m->AsDoubleOr(c.Value));
			if (const Json::Value* m = v.FindMember("size"))          c.Size          = static_cast<float>(m->AsDoubleOr(c.Size));
			if (const Json::Value* m = v.FindMember("numberOfSteps")) c.NumberOfSteps = m->AsIntOr(c.NumberOfSteps);
			if (const Json::Value* m = v.FindMember("isReadOnly"))    c.IsReadOnly    = m->AsBoolOr(c.IsReadOnly);
			if (const Json::Value* m = v.FindMember("transitionMode")) c.TransitionMode = static_cast<UITransitionMode>(m->AsIntOr(static_cast<int>(c.TransitionMode)));
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
			if (const Json::Value* m = v.FindMember("focused"))  c.FocusedColor  = UIColorFromJson(*m, c.FocusedColor);
			if (const Json::Value* m = v.FindMember("normalSprite"))   c.NormalSprite   = UIUuidFromJson(*m, c.NormalSprite);
			if (const Json::Value* m = v.FindMember("hoveredSprite"))  c.HoveredSprite  = UIUuidFromJson(*m, c.HoveredSprite);
			if (const Json::Value* m = v.FindMember("pressedSprite"))  c.PressedSprite  = UIUuidFromJson(*m, c.PressedSprite);
			if (const Json::Value* m = v.FindMember("disabledSprite")) c.DisabledSprite = UIUuidFromJson(*m, c.DisabledSprite);
			if (const Json::Value* m = v.FindMember("focusedSprite"))  c.FocusedSprite  = UIUuidFromJson(*m, c.FocusedSprite);
		};
		scrollbarInfo.onAdd = &InheritImageColorIntoNormal<ScrollbarComponent>;
		sceneManager.RegisterComponentType<ScrollbarComponent>(scrollbarInfo);

		ComponentInfo scrollRectInfo{ "Scroll Rect", "UI", ComponentCategory::Component };
		scrollRectInfo.serializedName = "ScrollRect";
		PropertyMetadata scrollRectElasticityMeta   = Properties::Meta::Clamp(0.0, 1.0, 0.01f);
		PropertyMetadata scrollRectDecelerationMeta = Properties::Meta::Clamp(0.0, 1.0, 0.005f);
		scrollRectInfo.properties = {
			MakeUiEntityRef<ScrollRectComponent>("Content",  "Content",  &ScrollRectComponent::Content),
			Properties::Make("Horizontal",        "Horizontal",         &ScrollRectComponent::Horizontal),
			Properties::Make("Vertical",          "Vertical",           &ScrollRectComponent::Vertical),
			Properties::Make("MovementType",      "Movement Type",      &ScrollRectComponent::MovementType),
			Properties::Make("Elasticity",        "Elasticity",         &ScrollRectComponent::Elasticity, scrollRectElasticityMeta),
			Properties::Make("Inertia",           "Inertia",            &ScrollRectComponent::Inertia),
			Properties::Make("DecelerationRate",  "Deceleration Rate",  &ScrollRectComponent::DecelerationRate, scrollRectDecelerationMeta),
			Properties::Make("ScrollSensitivity", "Scroll Sensitivity", &ScrollRectComponent::ScrollSensitivity),
			MakeUiEntityRef<ScrollRectComponent>("Viewport", "Viewport", &ScrollRectComponent::Viewport),
			MakeUiEntityRef<ScrollRectComponent>("HorizontalScrollbar", "Horizontal Scrollbar", &ScrollRectComponent::HorizontalScrollbar),
			Properties::Make("HorizontalScrollbarVisibility", "Horizontal Visibility", &ScrollRectComponent::HorizontalScrollbarVisibility),
			Properties::Make("HorizontalScrollbarSpacing",    "Horizontal Spacing",    &ScrollRectComponent::HorizontalScrollbarSpacing),
			MakeUiEntityRef<ScrollRectComponent>("VerticalScrollbar",   "Vertical Scrollbar",   &ScrollRectComponent::VerticalScrollbar),
			Properties::Make("VerticalScrollbarVisibility",   "Vertical Visibility",   &ScrollRectComponent::VerticalScrollbarVisibility),
			Properties::Make("VerticalScrollbarSpacing",      "Vertical Spacing",      &ScrollRectComponent::VerticalScrollbarSpacing),
		};
		scrollRectInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<ScrollRectComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("content",            UIEntityHandleToJson(e, c.Content));
			v.AddMember("viewport",           UIEntityHandleToJson(e, c.Viewport));
			v.AddMember("horizontal",         Json::Value(c.Horizontal));
			v.AddMember("vertical",           Json::Value(c.Vertical));
			v.AddMember("movementType",       Json::Value(static_cast<int>(c.MovementType)));
			v.AddMember("elasticity",         Json::Value(c.Elasticity));
			v.AddMember("inertia",            Json::Value(c.Inertia));
			v.AddMember("decelerationRate",   Json::Value(c.DecelerationRate));
			v.AddMember("scrollSensitivity",  Json::Value(c.ScrollSensitivity));
			v.AddMember("horizontalScrollbar", UIEntityHandleToJson(e, c.HorizontalScrollbar));
			v.AddMember("verticalScrollbar",   UIEntityHandleToJson(e, c.VerticalScrollbar));
			v.AddMember("horizontalScrollbarVisibility", Json::Value(static_cast<int>(c.HorizontalScrollbarVisibility)));
			v.AddMember("verticalScrollbarVisibility",   Json::Value(static_cast<int>(c.VerticalScrollbarVisibility)));
			v.AddMember("horizontalScrollbarSpacing",    Json::Value(c.HorizontalScrollbarSpacing));
			v.AddMember("verticalScrollbarSpacing",      Json::Value(c.VerticalScrollbarSpacing));
			Json::Value np = Json::Value::MakeObject();
			np.AddMember("x", Json::Value(c.NormalizedPosition.x));
			np.AddMember("y", Json::Value(c.NormalizedPosition.y));
			v.AddMember("normalizedPosition", std::move(np));
			return v;
		};
		scrollRectInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<ScrollRectComponent>();
			if (const Json::Value* m = v.FindMember("content"))  UIEntityHandleFromJson(e, *m, c.Content);
			if (const Json::Value* m = v.FindMember("viewport")) UIEntityHandleFromJson(e, *m, c.Viewport);
			if (const Json::Value* m = v.FindMember("horizontal"))         c.Horizontal         = m->AsBoolOr(c.Horizontal);
			if (const Json::Value* m = v.FindMember("vertical"))           c.Vertical           = m->AsBoolOr(c.Vertical);
			if (const Json::Value* m = v.FindMember("movementType"))       c.MovementType       = static_cast<ScrollRectMovementType>(m->AsIntOr(static_cast<int>(c.MovementType)));
			if (const Json::Value* m = v.FindMember("elasticity"))         c.Elasticity         = static_cast<float>(m->AsDoubleOr(c.Elasticity));
			if (const Json::Value* m = v.FindMember("inertia"))            c.Inertia            = m->AsBoolOr(c.Inertia);
			if (const Json::Value* m = v.FindMember("decelerationRate"))   c.DecelerationRate   = static_cast<float>(m->AsDoubleOr(c.DecelerationRate));
			if (const Json::Value* m = v.FindMember("scrollSensitivity"))  c.ScrollSensitivity  = static_cast<float>(m->AsDoubleOr(c.ScrollSensitivity));
			if (const Json::Value* m = v.FindMember("horizontalScrollbar")) UIEntityHandleFromJson(e, *m, c.HorizontalScrollbar);
			if (const Json::Value* m = v.FindMember("verticalScrollbar"))   UIEntityHandleFromJson(e, *m, c.VerticalScrollbar);
			if (const Json::Value* m = v.FindMember("horizontalScrollbarVisibility")) c.HorizontalScrollbarVisibility = static_cast<ScrollbarVisibility>(m->AsIntOr(static_cast<int>(c.HorizontalScrollbarVisibility)));
			if (const Json::Value* m = v.FindMember("verticalScrollbarVisibility"))   c.VerticalScrollbarVisibility   = static_cast<ScrollbarVisibility>(m->AsIntOr(static_cast<int>(c.VerticalScrollbarVisibility)));
			if (const Json::Value* m = v.FindMember("horizontalScrollbarSpacing"))    c.HorizontalScrollbarSpacing    = static_cast<float>(m->AsDoubleOr(c.HorizontalScrollbarSpacing));
			if (const Json::Value* m = v.FindMember("verticalScrollbarSpacing"))      c.VerticalScrollbarSpacing      = static_cast<float>(m->AsDoubleOr(c.VerticalScrollbarSpacing));
			if (const Json::Value* m = v.FindMember("normalizedPosition"); m && m->IsObject()) {
				if (const Json::Value* x = m->FindMember("x")) c.NormalizedPosition.x = static_cast<float>(x->AsDoubleOr(c.NormalizedPosition.x));
				if (const Json::Value* y = m->FindMember("y")) c.NormalizedPosition.y = static_cast<float>(y->AsDoubleOr(c.NormalizedPosition.y));
			}
		};
		sceneManager.RegisterComponentType<ScrollRectComponent>(scrollRectInfo);

		// ── Mask ─────────────────────────────────────────────────────
		// Clip descendants' rendering to this entity's resolved rect.
		// Used by ScrollView's Viewport so off-rect content doesn't
		// bleed into surrounding UI.
		ComponentInfo maskInfo{ "Mask", "UI", ComponentCategory::Component };
		maskInfo.serializedName = "Mask";
		maskInfo.properties = {
			Properties::Make("ShowMaskGraphic", "Show Mask Graphic", &MaskComponent::ShowMaskGraphic),
		};
		maskInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<MaskComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("showMaskGraphic", Json::Value(c.ShowMaskGraphic));
			return v;
		};
		maskInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<MaskComponent>();
			if (const Json::Value* m = v.FindMember("showMaskGraphic")) c.ShowMaskGraphic = m->AsBoolOr(c.ShowMaskGraphic);
		};
		sceneManager.RegisterComponentType<MaskComponent>(maskInfo);

		// ── Circular Slider ──────────────────────────────────────────
		// Ring-shaped value control. Render is procedural (GuiRenderer
		// emits a fan of rotated quads forming the arc); hit-test is an
		// annulus the system enforces in its hover loop. Mirrors the
		// linear Slider's value contract (Value/Min/Max/WholeNumbers/
		// IsReadOnly + ValueChangedThisFrame edge flag) so script code
		// that polls the slider doesn't have to special-case the shape.
		ComponentInfo csInfo{ "Circular Slider", "UI", ComponentCategory::Component };
		csInfo.serializedName = "CircularSlider";
		csInfo.properties = {
			Properties::Make("Value",        "Value",         &CircularSliderComponent::Value,
				Properties::Meta::DragSpeed(0.01f)),
			Properties::Make("MinValue",     "Min Value",     &CircularSliderComponent::MinValue),
			Properties::Make("MaxValue",     "Max Value",     &CircularSliderComponent::MaxValue),
			Properties::Make("WholeNumbers", "Whole Numbers", &CircularSliderComponent::WholeNumbers),
			Properties::Make("IsReadOnly",   "Read Only",     &CircularSliderComponent::IsReadOnly),
			Properties::Make("StartAngleDegrees", "Start Angle", &CircularSliderComponent::StartAngleDegrees,
				Properties::Meta::Header("Geometry")),
			Properties::Make("SweepDegrees",   "Sweep",         &CircularSliderComponent::SweepDegrees,
				Properties::Meta::Clamp(1.0, 360.0, 1.0f)),
			Properties::Make("Clockwise",      "Clockwise",     &CircularSliderComponent::Clockwise),
			Properties::Make("RingThickness",  "Ring Thickness",&CircularSliderComponent::RingThickness,
				Properties::Meta::Clamp(0.5, 1024.0, 0.5f)),
			Properties::Make("RingSegments",   "Ring Segments", &CircularSliderComponent::RingSegments,
				Properties::Meta::Clamp(8.0, 256.0, 1.0f)),
			Properties::Make("BackgroundColor", "Background", &CircularSliderComponent::BackgroundColor,
				Properties::Meta::Header("Visual")),
			Properties::Make("FillColor",       "Fill",       &CircularSliderComponent::FillColor),
			MakeUiEntityRef<CircularSliderComponent>("HandleEntity", "Handle Rect", &CircularSliderComponent::HandleEntity),
			Properties::Make("TransitionMode", "Transition",   &CircularSliderComponent::TransitionMode,
				Properties::Meta::Header("Handle Transition")),
			Properties::Make("NormalColor",   "Normal",   &CircularSliderComponent::NormalColor),
			Properties::Make("HoveredColor",  "Hovered",  &CircularSliderComponent::HoveredColor),
			Properties::Make("PressedColor",  "Pressed",  &CircularSliderComponent::PressedColor),
			Properties::Make("DisabledColor", "Disabled", &CircularSliderComponent::DisabledColor),
			Properties::Make("FocusedColor",  "Focused",  &CircularSliderComponent::FocusedColor),
			MakeUiSpriteRef<CircularSliderComponent>("NormalSprite",   "Normal Sprite",   &CircularSliderComponent::NormalSprite),
			MakeUiSpriteRef<CircularSliderComponent>("HoveredSprite",  "Hovered Sprite",  &CircularSliderComponent::HoveredSprite),
			MakeUiSpriteRef<CircularSliderComponent>("PressedSprite",  "Pressed Sprite",  &CircularSliderComponent::PressedSprite),
			MakeUiSpriteRef<CircularSliderComponent>("DisabledSprite", "Disabled Sprite", &CircularSliderComponent::DisabledSprite),
			MakeUiSpriteRef<CircularSliderComponent>("FocusedSprite",  "Focused Sprite",  &CircularSliderComponent::FocusedSprite),
		};
		csInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<CircularSliderComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("value",        Json::Value(c.Value));
			v.AddMember("min",          Json::Value(c.MinValue));
			v.AddMember("max",          Json::Value(c.MaxValue));
			v.AddMember("wholeNumbers", Json::Value(c.WholeNumbers));
			v.AddMember("isReadOnly",   Json::Value(c.IsReadOnly));
			v.AddMember("startAngle",   Json::Value(c.StartAngleDegrees));
			v.AddMember("sweep",        Json::Value(c.SweepDegrees));
			v.AddMember("clockwise",    Json::Value(c.Clockwise));
			v.AddMember("ringThickness",Json::Value(c.RingThickness));
			v.AddMember("ringSegments", Json::Value(c.RingSegments));
			v.AddMember("background",   UIColorToJson(c.BackgroundColor));
			v.AddMember("fill",         UIColorToJson(c.FillColor));
			v.AddMember("handleEntity", UIEntityHandleToJson(e, c.HandleEntity));
			v.AddMember("transitionMode", Json::Value(static_cast<int>(c.TransitionMode)));
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			v.AddMember("focused",  UIColorToJson(c.FocusedColor));
			v.AddMember("normalSprite",   UIUuidToJson(c.NormalSprite));
			v.AddMember("hoveredSprite",  UIUuidToJson(c.HoveredSprite));
			v.AddMember("pressedSprite",  UIUuidToJson(c.PressedSprite));
			v.AddMember("disabledSprite", UIUuidToJson(c.DisabledSprite));
			v.AddMember("focusedSprite",  UIUuidToJson(c.FocusedSprite));
			return v;
		};
		csInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<CircularSliderComponent>();
			if (const Json::Value* m = v.FindMember("value"))         c.Value         = static_cast<float>(m->AsDoubleOr(c.Value));
			if (const Json::Value* m = v.FindMember("min"))           c.MinValue      = static_cast<float>(m->AsDoubleOr(c.MinValue));
			if (const Json::Value* m = v.FindMember("max"))           c.MaxValue      = static_cast<float>(m->AsDoubleOr(c.MaxValue));
			if (const Json::Value* m = v.FindMember("wholeNumbers"))  c.WholeNumbers  = m->AsBoolOr(c.WholeNumbers);
			if (const Json::Value* m = v.FindMember("isReadOnly"))    c.IsReadOnly    = m->AsBoolOr(c.IsReadOnly);
			if (const Json::Value* m = v.FindMember("startAngle"))    c.StartAngleDegrees = static_cast<float>(m->AsDoubleOr(c.StartAngleDegrees));
			if (const Json::Value* m = v.FindMember("sweep"))         c.SweepDegrees  = static_cast<float>(m->AsDoubleOr(c.SweepDegrees));
			if (const Json::Value* m = v.FindMember("clockwise"))     c.Clockwise     = m->AsBoolOr(c.Clockwise);
			if (const Json::Value* m = v.FindMember("ringThickness")) c.RingThickness = static_cast<float>(m->AsDoubleOr(c.RingThickness));
			if (const Json::Value* m = v.FindMember("ringSegments"))  c.RingSegments  = m->AsIntOr(c.RingSegments);
			if (const Json::Value* m = v.FindMember("background"))    c.BackgroundColor = UIColorFromJson(*m, c.BackgroundColor);
			if (const Json::Value* m = v.FindMember("fill"))          c.FillColor       = UIColorFromJson(*m, c.FillColor);
			if (const Json::Value* m = v.FindMember("handleEntity"))  UIEntityHandleFromJson(e, *m, c.HandleEntity);
			if (const Json::Value* m = v.FindMember("transitionMode")) c.TransitionMode = static_cast<UITransitionMode>(m->AsIntOr(static_cast<int>(c.TransitionMode)));
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
			if (const Json::Value* m = v.FindMember("focused"))  c.FocusedColor  = UIColorFromJson(*m, c.FocusedColor);
			if (const Json::Value* m = v.FindMember("normalSprite"))   c.NormalSprite   = UIUuidFromJson(*m, c.NormalSprite);
			if (const Json::Value* m = v.FindMember("hoveredSprite"))  c.HoveredSprite  = UIUuidFromJson(*m, c.HoveredSprite);
			if (const Json::Value* m = v.FindMember("pressedSprite"))  c.PressedSprite  = UIUuidFromJson(*m, c.PressedSprite);
			if (const Json::Value* m = v.FindMember("disabledSprite")) c.DisabledSprite = UIUuidFromJson(*m, c.DisabledSprite);
			if (const Json::Value* m = v.FindMember("focusedSprite"))  c.FocusedSprite  = UIUuidFromJson(*m, c.FocusedSprite);
		};
		sceneManager.RegisterComponentType<CircularSliderComponent>(csInfo);

		// ── Content Size Fitter ──────────────────────────────────────
		// Resizes the entity's RectTransform2D SizeDelta along enabled
		// axes to fit the AABB of its direct children. UILayoutSystem
		// applies this before parent layout-group passes consume the
		// resized rect.
		ComponentInfo csfInfo{ "Content Size Fitter", "UI", ComponentCategory::Component };
		csfInfo.serializedName = "ContentSizeFitter";
		csfInfo.properties = {
			Properties::Make("HorizontalFit", "Horizontal Fit", &ContentSizeFitterComponent::HorizontalFit),
			Properties::Make("VerticalFit",   "Vertical Fit",   &ContentSizeFitterComponent::VerticalFit),
			Properties::Make("PaddingLeft",   "Left",   &ContentSizeFitterComponent::PaddingLeft,
				Properties::Meta::Header("Padding")),
			Properties::Make("PaddingRight",  "Right",  &ContentSizeFitterComponent::PaddingRight),
			Properties::Make("PaddingTop",    "Top",    &ContentSizeFitterComponent::PaddingTop),
			Properties::Make("PaddingBottom", "Bottom", &ContentSizeFitterComponent::PaddingBottom),
		};
		csfInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<ContentSizeFitterComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("horizontalFit", Json::Value(c.HorizontalFit));
			v.AddMember("verticalFit",   Json::Value(c.VerticalFit));
			v.AddMember("paddingLeft",   Json::Value(c.PaddingLeft));
			v.AddMember("paddingRight",  Json::Value(c.PaddingRight));
			v.AddMember("paddingTop",    Json::Value(c.PaddingTop));
			v.AddMember("paddingBottom", Json::Value(c.PaddingBottom));
			return v;
		};
		csfInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<ContentSizeFitterComponent>();
			if (const Json::Value* m = v.FindMember("horizontalFit")) c.HorizontalFit = m->AsBoolOr(c.HorizontalFit);
			if (const Json::Value* m = v.FindMember("verticalFit"))   c.VerticalFit   = m->AsBoolOr(c.VerticalFit);
			if (const Json::Value* m = v.FindMember("paddingLeft"))   c.PaddingLeft   = static_cast<float>(m->AsDoubleOr(c.PaddingLeft));
			if (const Json::Value* m = v.FindMember("paddingRight"))  c.PaddingRight  = static_cast<float>(m->AsDoubleOr(c.PaddingRight));
			if (const Json::Value* m = v.FindMember("paddingTop"))    c.PaddingTop    = static_cast<float>(m->AsDoubleOr(c.PaddingTop));
			if (const Json::Value* m = v.FindMember("paddingBottom")) c.PaddingBottom = static_cast<float>(m->AsDoubleOr(c.PaddingBottom));
		};
		sceneManager.RegisterComponentType<ContentSizeFitterComponent>(csfInfo);

		// Width Constraint —
		// Clamps RectTransform2D SizeDelta.x to [MinWidth, MaxWidth]
		// each layout pass. Negative bound = side disabled. Runs after
		// ContentSizeFitter so a fitter-driven width gets clamped before
		// any parent layout-group reads the SizeDelta.
		ComponentInfo wcInfo{ "Width Constraint", "UI", ComponentCategory::Component };
		wcInfo.serializedName = "WidthConstraint";
		wcInfo.properties = {
			Properties::Make("MinWidth", "Min Width", &WidthConstraintComponent::MinWidth),
			Properties::Make("MaxWidth", "Max Width", &WidthConstraintComponent::MaxWidth),
		};
		wcInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<WidthConstraintComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("minWidth", Json::Value(c.MinWidth));
			v.AddMember("maxWidth", Json::Value(c.MaxWidth));
			return v;
		};
		wcInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<WidthConstraintComponent>();
			if (const Json::Value* m = v.FindMember("minWidth")) c.MinWidth = static_cast<float>(m->AsDoubleOr(c.MinWidth));
			if (const Json::Value* m = v.FindMember("maxWidth")) c.MaxWidth = static_cast<float>(m->AsDoubleOr(c.MaxWidth));
		};
		sceneManager.RegisterComponentType<WidthConstraintComponent>(wcInfo);

		// ── Layout groups ────────────────────────────────────────────
		// Reposition (and optionally resize) child rects each frame.
		// All three share the same padding / spacing / alignment shape.
		// EnabledIf gates: ChildForceExpand* and UseChildScale* require
		// ControlChild{Width,Height} to be on, mirroring Unity's
		// HorizontalLayoutGroup inspector. Disabling Control* greys out
		// the dependent rows so users don't toggle a flag that has no
		// effect.
		auto registerHorizontalLayout = [&]() {
			ComponentInfo info{ "Horizontal Layout Group", "UI", ComponentCategory::Component };
			info.serializedName = "HorizontalLayoutGroup";
			info.properties = {
				Properties::Make("PaddingLeft",   "Left",   &HorizontalLayoutGroupComponent::PaddingLeft,
					Properties::Meta::Header("Padding")),
				Properties::Make("PaddingRight",  "Right",  &HorizontalLayoutGroupComponent::PaddingRight),
				Properties::Make("PaddingTop",    "Top",    &HorizontalLayoutGroupComponent::PaddingTop),
				Properties::Make("PaddingBottom", "Bottom", &HorizontalLayoutGroupComponent::PaddingBottom),
				Properties::Make("Spacing",            "Spacing",             &HorizontalLayoutGroupComponent::Spacing,
					Properties::Meta::Header("Layout")),
				Properties::Make("ChildAlignment",     "Child Alignment",     &HorizontalLayoutGroupComponent::ChildAlignment),
				Properties::Make("ReverseArrangement", "Reverse Arrangement", &HorizontalLayoutGroupComponent::ReverseArrangement),
				Properties::Make("ControlChildWidth",  "Control Child Width",  &HorizontalLayoutGroupComponent::ControlChildWidth,
					Properties::Meta::Header("Children")),
				Properties::Make("ControlChildHeight", "Control Child Height", &HorizontalLayoutGroupComponent::ControlChildHeight),
				Properties::Make("UseChildScaleWidth",  "Use Child Scale Width",  &HorizontalLayoutGroupComponent::UseChildScaleWidth,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<HorizontalLayoutGroupComponent>().ControlChildWidth;
					})),
				Properties::Make("UseChildScaleHeight", "Use Child Scale Height", &HorizontalLayoutGroupComponent::UseChildScaleHeight,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<HorizontalLayoutGroupComponent>().ControlChildHeight;
					})),
				Properties::Make("ChildForceExpandWidth",  "Child Force Expand Width",  &HorizontalLayoutGroupComponent::ChildForceExpandWidth,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<HorizontalLayoutGroupComponent>().ControlChildWidth;
					})),
				Properties::Make("ChildForceExpandHeight", "Child Force Expand Height", &HorizontalLayoutGroupComponent::ChildForceExpandHeight,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<HorizontalLayoutGroupComponent>().ControlChildHeight;
					})),
			};
			info.serialize = [](Entity e) -> Json::Value {
				const auto& c = e.GetComponent<HorizontalLayoutGroupComponent>();
				Json::Value v = Json::Value::MakeObject();
				v.AddMember("paddingLeft",   Json::Value(c.PaddingLeft));
				v.AddMember("paddingRight",  Json::Value(c.PaddingRight));
				v.AddMember("paddingTop",    Json::Value(c.PaddingTop));
				v.AddMember("paddingBottom", Json::Value(c.PaddingBottom));
				v.AddMember("spacing",       Json::Value(c.Spacing));
				v.AddMember("childAlignment", Json::Value(static_cast<int>(c.ChildAlignment)));
				v.AddMember("reverseArrangement", Json::Value(c.ReverseArrangement));
				v.AddMember("controlChildWidth",  Json::Value(c.ControlChildWidth));
				v.AddMember("controlChildHeight", Json::Value(c.ControlChildHeight));
				v.AddMember("useChildScaleWidth",  Json::Value(c.UseChildScaleWidth));
				v.AddMember("useChildScaleHeight", Json::Value(c.UseChildScaleHeight));
				v.AddMember("childForceExpandWidth",  Json::Value(c.ChildForceExpandWidth));
				v.AddMember("childForceExpandHeight", Json::Value(c.ChildForceExpandHeight));
				return v;
			};
			info.deserialize = [](Entity e, const Json::Value& v) {
				auto& c = e.GetComponent<HorizontalLayoutGroupComponent>();
				if (const Json::Value* m = v.FindMember("paddingLeft"))   c.PaddingLeft   = static_cast<float>(m->AsDoubleOr(c.PaddingLeft));
				if (const Json::Value* m = v.FindMember("paddingRight"))  c.PaddingRight  = static_cast<float>(m->AsDoubleOr(c.PaddingRight));
				if (const Json::Value* m = v.FindMember("paddingTop"))    c.PaddingTop    = static_cast<float>(m->AsDoubleOr(c.PaddingTop));
				if (const Json::Value* m = v.FindMember("paddingBottom")) c.PaddingBottom = static_cast<float>(m->AsDoubleOr(c.PaddingBottom));
				if (const Json::Value* m = v.FindMember("spacing"))       c.Spacing       = static_cast<float>(m->AsDoubleOr(c.Spacing));
				if (const Json::Value* m = v.FindMember("childAlignment"))     c.ChildAlignment     = static_cast<UIAlignment>(m->AsIntOr(static_cast<int>(c.ChildAlignment)));
				if (const Json::Value* m = v.FindMember("reverseArrangement")) c.ReverseArrangement = m->AsBoolOr(c.ReverseArrangement);
				if (const Json::Value* m = v.FindMember("controlChildWidth"))  c.ControlChildWidth  = m->AsBoolOr(c.ControlChildWidth);
				if (const Json::Value* m = v.FindMember("controlChildHeight")) c.ControlChildHeight = m->AsBoolOr(c.ControlChildHeight);
				if (const Json::Value* m = v.FindMember("useChildScaleWidth"))  c.UseChildScaleWidth  = m->AsBoolOr(c.UseChildScaleWidth);
				if (const Json::Value* m = v.FindMember("useChildScaleHeight")) c.UseChildScaleHeight = m->AsBoolOr(c.UseChildScaleHeight);
				if (const Json::Value* m = v.FindMember("childForceExpandWidth"))  c.ChildForceExpandWidth  = m->AsBoolOr(c.ChildForceExpandWidth);
				if (const Json::Value* m = v.FindMember("childForceExpandHeight")) c.ChildForceExpandHeight = m->AsBoolOr(c.ChildForceExpandHeight);
			};
			sceneManager.RegisterComponentType<HorizontalLayoutGroupComponent>(info);
		};
		registerHorizontalLayout();

		auto registerVerticalLayout = [&]() {
			ComponentInfo info{ "Vertical Layout Group", "UI", ComponentCategory::Component };
			info.serializedName = "VerticalLayoutGroup";
			info.properties = {
				Properties::Make("PaddingLeft",   "Left",   &VerticalLayoutGroupComponent::PaddingLeft,
					Properties::Meta::Header("Padding")),
				Properties::Make("PaddingRight",  "Right",  &VerticalLayoutGroupComponent::PaddingRight),
				Properties::Make("PaddingTop",    "Top",    &VerticalLayoutGroupComponent::PaddingTop),
				Properties::Make("PaddingBottom", "Bottom", &VerticalLayoutGroupComponent::PaddingBottom),
				Properties::Make("Spacing",            "Spacing",             &VerticalLayoutGroupComponent::Spacing,
					Properties::Meta::Header("Layout")),
				Properties::Make("ChildAlignment",     "Child Alignment",     &VerticalLayoutGroupComponent::ChildAlignment),
				Properties::Make("ReverseArrangement", "Reverse Arrangement", &VerticalLayoutGroupComponent::ReverseArrangement),
				Properties::Make("ControlChildWidth",  "Control Child Width",  &VerticalLayoutGroupComponent::ControlChildWidth,
					Properties::Meta::Header("Children")),
				Properties::Make("ControlChildHeight", "Control Child Height", &VerticalLayoutGroupComponent::ControlChildHeight),
				Properties::Make("UseChildScaleWidth",  "Use Child Scale Width",  &VerticalLayoutGroupComponent::UseChildScaleWidth,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<VerticalLayoutGroupComponent>().ControlChildWidth;
					})),
				Properties::Make("UseChildScaleHeight", "Use Child Scale Height", &VerticalLayoutGroupComponent::UseChildScaleHeight,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<VerticalLayoutGroupComponent>().ControlChildHeight;
					})),
				Properties::Make("ChildForceExpandWidth",  "Child Force Expand Width",  &VerticalLayoutGroupComponent::ChildForceExpandWidth,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<VerticalLayoutGroupComponent>().ControlChildWidth;
					})),
				Properties::Make("ChildForceExpandHeight", "Child Force Expand Height", &VerticalLayoutGroupComponent::ChildForceExpandHeight,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<VerticalLayoutGroupComponent>().ControlChildHeight;
					})),
			};
			info.serialize = [](Entity e) -> Json::Value {
				const auto& c = e.GetComponent<VerticalLayoutGroupComponent>();
				Json::Value v = Json::Value::MakeObject();
				v.AddMember("paddingLeft",   Json::Value(c.PaddingLeft));
				v.AddMember("paddingRight",  Json::Value(c.PaddingRight));
				v.AddMember("paddingTop",    Json::Value(c.PaddingTop));
				v.AddMember("paddingBottom", Json::Value(c.PaddingBottom));
				v.AddMember("spacing",       Json::Value(c.Spacing));
				v.AddMember("childAlignment", Json::Value(static_cast<int>(c.ChildAlignment)));
				v.AddMember("reverseArrangement", Json::Value(c.ReverseArrangement));
				v.AddMember("controlChildWidth",  Json::Value(c.ControlChildWidth));
				v.AddMember("controlChildHeight", Json::Value(c.ControlChildHeight));
				v.AddMember("useChildScaleWidth",  Json::Value(c.UseChildScaleWidth));
				v.AddMember("useChildScaleHeight", Json::Value(c.UseChildScaleHeight));
				v.AddMember("childForceExpandWidth",  Json::Value(c.ChildForceExpandWidth));
				v.AddMember("childForceExpandHeight", Json::Value(c.ChildForceExpandHeight));
				return v;
			};
			info.deserialize = [](Entity e, const Json::Value& v) {
				auto& c = e.GetComponent<VerticalLayoutGroupComponent>();
				if (const Json::Value* m = v.FindMember("paddingLeft"))   c.PaddingLeft   = static_cast<float>(m->AsDoubleOr(c.PaddingLeft));
				if (const Json::Value* m = v.FindMember("paddingRight"))  c.PaddingRight  = static_cast<float>(m->AsDoubleOr(c.PaddingRight));
				if (const Json::Value* m = v.FindMember("paddingTop"))    c.PaddingTop    = static_cast<float>(m->AsDoubleOr(c.PaddingTop));
				if (const Json::Value* m = v.FindMember("paddingBottom")) c.PaddingBottom = static_cast<float>(m->AsDoubleOr(c.PaddingBottom));
				if (const Json::Value* m = v.FindMember("spacing"))       c.Spacing       = static_cast<float>(m->AsDoubleOr(c.Spacing));
				if (const Json::Value* m = v.FindMember("childAlignment"))     c.ChildAlignment     = static_cast<UIAlignment>(m->AsIntOr(static_cast<int>(c.ChildAlignment)));
				if (const Json::Value* m = v.FindMember("reverseArrangement")) c.ReverseArrangement = m->AsBoolOr(c.ReverseArrangement);
				if (const Json::Value* m = v.FindMember("controlChildWidth"))  c.ControlChildWidth  = m->AsBoolOr(c.ControlChildWidth);
				if (const Json::Value* m = v.FindMember("controlChildHeight")) c.ControlChildHeight = m->AsBoolOr(c.ControlChildHeight);
				if (const Json::Value* m = v.FindMember("useChildScaleWidth"))  c.UseChildScaleWidth  = m->AsBoolOr(c.UseChildScaleWidth);
				if (const Json::Value* m = v.FindMember("useChildScaleHeight")) c.UseChildScaleHeight = m->AsBoolOr(c.UseChildScaleHeight);
				if (const Json::Value* m = v.FindMember("childForceExpandWidth"))  c.ChildForceExpandWidth  = m->AsBoolOr(c.ChildForceExpandWidth);
				if (const Json::Value* m = v.FindMember("childForceExpandHeight")) c.ChildForceExpandHeight = m->AsBoolOr(c.ChildForceExpandHeight);
			};
			sceneManager.RegisterComponentType<VerticalLayoutGroupComponent>(info);
		};
		registerVerticalLayout();

		auto registerGridLayout = [&]() {
			ComponentInfo info{ "Grid Layout Group", "UI", ComponentCategory::Component };
			info.serializedName = "GridLayoutGroup";
			info.properties = {
				Properties::Make("PaddingLeft",   "Left",   &GridLayoutGroupComponent::PaddingLeft,
					Properties::Meta::Header("Padding")),
				Properties::Make("PaddingRight",  "Right",  &GridLayoutGroupComponent::PaddingRight),
				Properties::Make("PaddingTop",    "Top",    &GridLayoutGroupComponent::PaddingTop),
				Properties::Make("PaddingBottom", "Bottom", &GridLayoutGroupComponent::PaddingBottom),
				Properties::Make("CellSize", "Cell Size", &GridLayoutGroupComponent::CellSize,
					Properties::Meta::Header("Cells")),
				Properties::Make("Spacing",  "Spacing",   &GridLayoutGroupComponent::Spacing),
				Properties::Make("StartCorner",    "Start Corner",    &GridLayoutGroupComponent::StartCorner,
					Properties::Meta::Header("Layout")),
				Properties::Make("StartAxis",      "Start Axis",      &GridLayoutGroupComponent::StartAxis),
				Properties::Make("ChildAlignment", "Child Alignment", &GridLayoutGroupComponent::ChildAlignment),
				Properties::Make("Constraint",     "Constraint",      &GridLayoutGroupComponent::Constraint),
				// Gate: ConstraintCount only matters for the fixed-count
				// modes. Greys out for the Flexible layout where the
				// row/column count is derived from cell+rect size.
				Properties::Make("ConstraintCount", "Constraint Count", &GridLayoutGroupComponent::ConstraintCount,
					PropertyMetadata{}.WithEnabledIf([](const Entity& e) {
						return e.GetComponent<GridLayoutGroupComponent>().Constraint != GridLayoutConstraint::Flexible;
					})),
				Properties::Make("Reverse", "Reverse", &GridLayoutGroupComponent::Reverse),
			};
			info.serialize = [](Entity e) -> Json::Value {
				const auto& c = e.GetComponent<GridLayoutGroupComponent>();
				Json::Value v = Json::Value::MakeObject();
				v.AddMember("paddingLeft",   Json::Value(c.PaddingLeft));
				v.AddMember("paddingRight",  Json::Value(c.PaddingRight));
				v.AddMember("paddingTop",    Json::Value(c.PaddingTop));
				v.AddMember("paddingBottom", Json::Value(c.PaddingBottom));
				Json::Value cellSize = Json::Value::MakeObject();
				cellSize.AddMember("x", Json::Value(c.CellSize.x));
				cellSize.AddMember("y", Json::Value(c.CellSize.y));
				v.AddMember("cellSize", std::move(cellSize));
				Json::Value spacing = Json::Value::MakeObject();
				spacing.AddMember("x", Json::Value(c.Spacing.x));
				spacing.AddMember("y", Json::Value(c.Spacing.y));
				v.AddMember("spacing", std::move(spacing));
				v.AddMember("startCorner",     Json::Value(static_cast<int>(c.StartCorner)));
				v.AddMember("startAxis",       Json::Value(static_cast<int>(c.StartAxis)));
				v.AddMember("childAlignment",  Json::Value(static_cast<int>(c.ChildAlignment)));
				v.AddMember("constraint",      Json::Value(static_cast<int>(c.Constraint)));
				v.AddMember("constraintCount", Json::Value(c.ConstraintCount));
				v.AddMember("reverse",         Json::Value(c.Reverse));
				return v;
			};
			info.deserialize = [](Entity e, const Json::Value& v) {
				auto& c = e.GetComponent<GridLayoutGroupComponent>();
				if (const Json::Value* m = v.FindMember("paddingLeft"))   c.PaddingLeft   = static_cast<float>(m->AsDoubleOr(c.PaddingLeft));
				if (const Json::Value* m = v.FindMember("paddingRight"))  c.PaddingRight  = static_cast<float>(m->AsDoubleOr(c.PaddingRight));
				if (const Json::Value* m = v.FindMember("paddingTop"))    c.PaddingTop    = static_cast<float>(m->AsDoubleOr(c.PaddingTop));
				if (const Json::Value* m = v.FindMember("paddingBottom")) c.PaddingBottom = static_cast<float>(m->AsDoubleOr(c.PaddingBottom));
				if (const Json::Value* m = v.FindMember("cellSize"); m && m->IsObject()) {
					if (const Json::Value* x = m->FindMember("x")) c.CellSize.x = static_cast<float>(x->AsDoubleOr(c.CellSize.x));
					if (const Json::Value* y = m->FindMember("y")) c.CellSize.y = static_cast<float>(y->AsDoubleOr(c.CellSize.y));
				}
				if (const Json::Value* m = v.FindMember("spacing"); m && m->IsObject()) {
					if (const Json::Value* x = m->FindMember("x")) c.Spacing.x = static_cast<float>(x->AsDoubleOr(c.Spacing.x));
					if (const Json::Value* y = m->FindMember("y")) c.Spacing.y = static_cast<float>(y->AsDoubleOr(c.Spacing.y));
				}
				if (const Json::Value* m = v.FindMember("startCorner"))     c.StartCorner     = static_cast<GridLayoutStartCorner>(m->AsIntOr(static_cast<int>(c.StartCorner)));
				if (const Json::Value* m = v.FindMember("startAxis"))       c.StartAxis       = static_cast<GridLayoutStartAxis>(m->AsIntOr(static_cast<int>(c.StartAxis)));
				if (const Json::Value* m = v.FindMember("childAlignment"))  c.ChildAlignment  = static_cast<UIAlignment>(m->AsIntOr(static_cast<int>(c.ChildAlignment)));
				if (const Json::Value* m = v.FindMember("constraint"))      c.Constraint      = static_cast<GridLayoutConstraint>(m->AsIntOr(static_cast<int>(c.Constraint)));
				if (const Json::Value* m = v.FindMember("constraintCount")) c.ConstraintCount = m->AsIntOr(c.ConstraintCount);
				if (const Json::Value* m = v.FindMember("reverse"))         c.Reverse         = m->AsBoolOr(c.Reverse);
			};
			sceneManager.RegisterComponentType<GridLayoutGroupComponent>(info);
		};
		registerGridLayout();

		// HierarchyComponent is real but managed via Entity::SetParent;
		// users don't add it from the Add Component popup. Tagged so the
		// inspector hides it from the picker.
		RegisterComponent<HierarchyComponent>(sceneManager, "Hierarchy", ComponentCategory::Tag);

		// ── Scripting ───────────────────────────────────────────────
		// Engine-side registration so non-editor builds (Runtime, Launcher) include
		// ScriptComponent in the registry. Without this, generic registry-driven
		// flows (CopyComponents, future serializer hooks) silently skip scripts
		// outside the editor. The editor still calls AttachInspector to layer in
		// the per-script field UI; ComponentRegistry::Register merges drawInspector
		// from a prior entry so the inspector survives.
		RegisterComponent<ScriptComponent>(sceneManager, "Scripts",
			ComponentCategory::Component, "Scripting", "Scripts");

		// ── Conflicts ───────────────────────────────────────────────
		// One renderer per entity (Tilemap2D adds its own conflicts in its package init).
		DeclareConflict<SpriteRendererComponent, ParticleSystem2DComponent>(sceneManager);
		DeclareConflict<SpriteRendererComponent, ImageComponent>(sceneManager);
		DeclareConflict<ImageComponent, ParticleSystem2DComponent>(sceneManager);

		// One spatial transform per entity — UI rects and world-space transforms
		// occupy the same role and should not coexist on a single entity.
		DeclareConflict<Transform2DComponent, RectTransform2DComponent>(sceneManager);

		// Box2D and Axiom-Physics stacks must not be mixed per-entity.
		DeclareConflict<Rigidbody2DComponent, FastBody2DComponent>(sceneManager);
		DeclareConflict<BoxCollider2DComponent, FastBoxCollider2DComponent>(sceneManager);
		DeclareConflict<BoxCollider2DComponent, FastCircleCollider2DComponent>(sceneManager);
		DeclareConflict<CircleCollider2DComponent, FastBoxCollider2DComponent>(sceneManager);
		DeclareConflict<CircleCollider2DComponent, FastCircleCollider2DComponent>(sceneManager);
		DeclareConflict<PolygonCollider2DComponent, FastBoxCollider2DComponent>(sceneManager);
		DeclareConflict<PolygonCollider2DComponent, FastCircleCollider2DComponent>(sceneManager);

		// One Box2D shape per entity — the rigidbody adopts whichever collider
		// is on the entity, and Scene::OnRigidBody2DComponentConstruct only
		// reaches for one. Layering two would leak a body.
		DeclareConflict<BoxCollider2DComponent, CircleCollider2DComponent>(sceneManager);
		DeclareConflict<BoxCollider2DComponent, PolygonCollider2DComponent>(sceneManager);
		DeclareConflict<CircleCollider2DComponent, PolygonCollider2DComponent>(sceneManager);

		// Within Axiom-Physics, only one shape per body is supported.
		DeclareConflict<FastBoxCollider2DComponent, FastCircleCollider2DComponent>(sceneManager);

		// Only one layout group per entity — they all rewrite the same
		// children's RectTransforms, so layering would produce frame-by-
		// frame fights between layout passes.
		DeclareConflict<HorizontalLayoutGroupComponent, VerticalLayoutGroupComponent>(sceneManager);
		DeclareConflict<HorizontalLayoutGroupComponent, GridLayoutGroupComponent>(sceneManager);
		DeclareConflict<VerticalLayoutGroupComponent, GridLayoutGroupComponent>(sceneManager);

		// ── Dependencies ────────────────────────────────────────────
		// UI widgets need an InteractableComponent (input flags) and a
		// RectTransform2DComponent (rect for hit-testing) to work. The
		// inspector / paste / scripting AddComponent paths auto-add
		// these alongside the widget so the user gets a working button
		// in one click. Removal stays unrestricted — if the user
		// strips the Interactable later, UIEventSystem just sees the
		// widget go inert.
		DeclareDependency<ButtonComponent,     InteractableComponent>(sceneManager);
		DeclareDependency<ButtonComponent,     RectTransform2DComponent>(sceneManager);
		DeclareDependency<InputFieldComponent, InteractableComponent>(sceneManager);
		DeclareDependency<InputFieldComponent, RectTransform2DComponent>(sceneManager);
		DeclareDependency<ToggleComponent,     InteractableComponent>(sceneManager);
		DeclareDependency<ToggleComponent,     RectTransform2DComponent>(sceneManager);
		DeclareDependency<SliderComponent,     InteractableComponent>(sceneManager);
		DeclareDependency<SliderComponent,     RectTransform2DComponent>(sceneManager);
		DeclareDependency<DropdownComponent,   InteractableComponent>(sceneManager);
		DeclareDependency<DropdownComponent,   RectTransform2DComponent>(sceneManager);
		DeclareDependency<MaskComponent,       RectTransform2DComponent>(sceneManager);

		// Tags (not user-addable)
		RegisterComponent<IdTag>(sceneManager, "Id", ComponentCategory::Tag);
		RegisterComponent<StaticTag>(sceneManager, "Static", ComponentCategory::Tag);
		RegisterComponent<DisabledTag>(sceneManager, "Disabled", ComponentCategory::Tag);
		RegisterComponent<InheritedDisabledTag>(sceneManager, "Inherited Disabled", ComponentCategory::Tag);
		RegisterComponent<DeadlyTag>(sceneManager, "Deadly", ComponentCategory::Tag);

		// User-defined C# components are registered at runtime via
		// DynamicComponentRegistrar (Index-ScriptCore) — see
		// ComponentRegistry::RegisterDynamic. The engine no longer touches
		// any per-project codegen output; each user .cs file annotated
		// [NativeComponent(..., Generate = true)] is reflected and
		// registered when the user assembly loads.

		// Debug-only sanity check: every conflictsWith edge must be symmetric.
		// DeclareConflict<A, B> already adds both directions, but a manual
		// edit to one info's conflictsWith vector would silently slip through
		// the bidirectional HasConflict() lookups. Catch the asymmetry now,
		// not when a user reports a half-working "Add Component" filter.
		sceneManager.GetComponentRegistry().ValidateConflictSymmetry();
	}
}
