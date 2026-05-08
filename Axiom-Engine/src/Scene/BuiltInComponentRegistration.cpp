#include "pch.hpp"
#include "Scene/BuiltInComponentRegistration.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Audio/AudioManager.hpp"
#include "Components/Components.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/TextureManager.hpp"
#include "Inspector/PropertyRegistration.hpp"
#include "Math/Trigonometry.hpp"
#include "Physics/PhysicsTypes.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"

#include <cmath>

#include <vector>

// Single source of truth for built-in components — names, categories, properties.
namespace Axiom {
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

		template<typename T>
		void RegisterComponent(SceneManager& sceneManager, const std::string& displayName,
			ComponentCategory category = ComponentCategory::Component,
			const std::string& subcategory = "",
			const std::string& serializedName = "",
			std::vector<PropertyDescriptor> properties = {})
		{
			ComponentInfo info{ displayName, subcategory, category };
			info.serializedName = serializedName;
			info.properties = std::move(properties);
			sceneManager.RegisterComponentType<T>(info);
		}

		template<typename A, typename B>
		void DeclareConflict(SceneManager& sceneManager) {
			const std::type_index aId(typeid(A));
			const std::type_index bId(typeid(B));
			sceneManager.GetComponentRegistry().ForEachComponentInfo(
				[&](const std::type_index& id, ComponentInfo& info) {
					if (id == aId) {
						bool present = false;
						for (const auto& c : info.conflictsWith) if (c == bId) { present = true; break; }
						if (!present) info.conflictsWith.push_back(bId);
					}
					else if (id == bId) {
						bool present = false;
						for (const auto& c : info.conflictsWith) if (c == aId) { present = true; break; }
						if (!present) info.conflictsWith.push_back(aId);
					}
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
					[]() { PropertyMetadata m; m.DragSpeed = 1.0f; return m; }()),
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
					}),
			});

		RegisterComponent<ImageComponent>(sceneManager, "Image",
			ComponentCategory::Component, "UI", "Image",
			{
				Properties::Make("Color", "Color", &ImageComponent::Color),
				MakeTextureRefDirect<ImageComponent>("Texture", "Texture",
					[](const ImageComponent& i) { return i.TextureHandle; },
					[](ImageComponent& i, TextureHandle h, UUID assetId) {
						i.TextureHandle = h;
						i.TextureAssetId = assetId;
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.01; m.ClampMax = 100.0; m.DragSpeed = 0.01f; return m; }()),
				Properties::MakeWith<float>("OrthographicSize", "Orthographic Size",
					[](const Entity& e) { return e.GetComponent<Camera2DComponent>().GetOrthographicSize(); },
					[](Entity& e, float v) {
						e.GetComponent<Camera2DComponent>().SetOrthographicSize(v);
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.05; m.ClampMax = 1000.0; m.DragSpeed = 0.05f; return m; }()),
				Properties::MakeWith<Color>("ClearColor", "Clear Color",
					[](const Entity& e) { return e.GetComponent<Camera2DComponent>().GetClearColor(); },
					[](Entity& e, const Color& c) {
						e.GetComponent<Camera2DComponent>().SetClearColor(c);
					}),
			});

		// Custom drawInspector — variant shapes / play-pause don't map to PropertyDescriptors.
		RegisterComponent<ParticleSystem2DComponent>(sceneManager, "Particle System 2D",
			ComponentCategory::Component, "Rendering", "ParticleSystem2D");

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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 1.0; m.ClampMax = 512.0; m.DragSpeed = 0.5f; return m; }()),
				Properties::Make("Color", "Color", &TextRendererComponent::Color),
				Properties::MakeWith<float>("LetterSpacing", "Letter Spacing",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().LetterSpacing; },
					[](Entity& e, float v) {
						e.GetComponent<TextRendererComponent>().LetterSpacing = v;
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = -64.0; m.ClampMax = 64.0; m.DragSpeed = 0.1f; return m; }()),
				Properties::MakeWith<TextAlignment>("Alignment", "Alignment",
					[](const Entity& e) { return e.GetComponent<TextRendererComponent>().HAlign; },
					[](Entity& e, TextAlignment v) {
						e.GetComponent<TextRendererComponent>().HAlign = v;
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
					[]() { PropertyMetadata m; m.ReadOnly = true; return m; }()),
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
					[]() { PropertyMetadata m; m.DragSpeed = 0.01f; m.ReadOnly = true; return m; }()),
				Properties::MakeWith<float>("GravityScale", "Gravity Scale",
					[](const Entity& e) { return e.GetComponent<Rigidbody2DComponent>().GetGravityScale(); },
					[](Entity& e, float v) {
						e.GetComponent<Rigidbody2DComponent>().SetGravityScale(v);
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 1.0; return m; }()),
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
					[]() { PropertyMetadata m; m.DragSpeed = 0.05f; return m; }()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.001; m.ClampMax = 1000.0; m.DragSpeed = 0.05f; return m; }()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 10.0; m.DragSpeed = 0.01f; return m; }()),
				Properties::MakeWith<float>("Bounciness", "Bounciness",
					[](const Entity& e) { return e.GetComponent<BoxCollider2DComponent>().GetBounciness(); },
					[](Entity& e, float v) {
						e.GetComponent<BoxCollider2DComponent>().SetBounciness(v);
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 1.0; m.DragSpeed = 0.01f; return m; }()),
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
					[]() { PropertyMetadata m; m.DragSpeed = 0.05f; return m; }()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.001; m.ClampMax = 1000.0; m.DragSpeed = 0.05f; return m; }()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 10.0; m.DragSpeed = 0.01f; return m; }()),
				Properties::MakeWith<float>("Bounciness", "Bounciness",
					[](const Entity& e) { return e.GetComponent<CircleCollider2DComponent>().GetBounciness(); },
					[](Entity& e, float v) {
						e.GetComponent<CircleCollider2DComponent>().SetBounciness(v);
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 1.0; m.DragSpeed = 0.01f; return m; }()),
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
					[]() { PropertyMetadata m; m.DragSpeed = 0.05f; return m; }()),
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
					[]() {
						PropertyMetadata m;
						m.HasClamp = true;
						m.ClampMin = static_cast<double>(PolygonCollider2DComponent::k_MinVertices);
						m.ClampMax = static_cast<double>(PolygonCollider2DComponent::k_MaxVertices);
						m.DragSpeed = 1.0f;
						return m;
					}()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.001; m.ClampMax = 1000.0; m.DragSpeed = 0.05f; return m; }()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 10.0; m.DragSpeed = 0.01f; return m; }()),
				Properties::MakeWith<float>("Bounciness", "Bounciness",
					[](const Entity& e) { return e.GetComponent<PolygonCollider2DComponent>().GetBounciness(); },
					[](Entity& e, float v) {
						e.GetComponent<PolygonCollider2DComponent>().SetBounciness(v);
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 1.0; m.DragSpeed = 0.01f; return m; }()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.001; m.ClampMax = 10000.0; m.DragSpeed = 0.1f; return m; }()),
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
					[]() { PropertyMetadata m; m.DragSpeed = 0.05f; return m; }()),
			});

		RegisterComponent<FastCircleCollider2DComponent>(sceneManager, "Fast Circle Collider 2D",
			ComponentCategory::Component, "Physics", "FastCircleCollider2D",
			{
				Properties::MakeWith<float>("Radius", "Radius",
					[](const Entity& e) { return e.GetComponent<FastCircleCollider2DComponent>().Radius; },
					[](Entity& e, float v) {
						e.GetComponent<FastCircleCollider2DComponent>().SetRadius(v);
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.01; m.ClampMax = 100.0; m.DragSpeed = 0.05f; return m; }()),
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
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.0; m.ClampMax = 1.0; m.DragSpeed = 0.01f; return m; }()),
				Properties::MakeWith<float>("Pitch", "Pitch",
					[](const Entity& e) { return e.GetComponent<AudioSourceComponent>().GetPitch(); },
					[](Entity& e, float v) {
						e.GetComponent<AudioSourceComponent>().SetPitch(v);
					},
					[]() { PropertyMetadata m; m.HasClamp = true; m.ClampMin = 0.1; m.ClampMax = 3.0; m.DragSpeed = 0.01f; return m; }()),
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
		};
		interactableInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<InteractableComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("interactable", Json::Value(c.Interactable));
			return v;
		};
		interactableInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<InteractableComponent>();
			if (const Json::Value* m = v.FindMember("interactable")) c.Interactable = m->AsBoolOr(c.Interactable);
		};
		sceneManager.RegisterComponentType<InteractableComponent>(interactableInfo);

		ComponentInfo buttonInfo{ "Button", "UI", ComponentCategory::Component };
		buttonInfo.serializedName = "Button";
		buttonInfo.properties = {
			Properties::Make("NormalColor",   "Normal Color",   &ButtonComponent::NormalColor),
			Properties::Make("HoveredColor",  "Hovered Color",  &ButtonComponent::HoveredColor),
			Properties::Make("PressedColor",  "Pressed Color",  &ButtonComponent::PressedColor),
			Properties::Make("DisabledColor", "Disabled Color", &ButtonComponent::DisabledColor),
		};
		buttonInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<ButtonComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("normal",   UIColorToJson(c.NormalColor));
			v.AddMember("hovered",  UIColorToJson(c.HoveredColor));
			v.AddMember("pressed",  UIColorToJson(c.PressedColor));
			v.AddMember("disabled", UIColorToJson(c.DisabledColor));
			return v;
		};
		buttonInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<ButtonComponent>();
			if (const Json::Value* m = v.FindMember("normal"))   c.NormalColor   = UIColorFromJson(*m, c.NormalColor);
			if (const Json::Value* m = v.FindMember("hovered"))  c.HoveredColor  = UIColorFromJson(*m, c.HoveredColor);
			if (const Json::Value* m = v.FindMember("pressed"))  c.PressedColor  = UIColorFromJson(*m, c.PressedColor);
			if (const Json::Value* m = v.FindMember("disabled")) c.DisabledColor = UIColorFromJson(*m, c.DisabledColor);
		};
		sceneManager.RegisterComponentType<ButtonComponent>(buttonInfo);

		ComponentInfo sliderInfo{ "Slider", "UI", ComponentCategory::Component };
		sliderInfo.serializedName = "Slider";
		sliderInfo.properties = {
			Properties::MakeWith<float>("Value", "Value",
				[](const Entity& e) { return e.GetComponent<SliderComponent>().Value; },
				[](Entity& e, float v) { e.GetComponent<SliderComponent>().Value = v; },
				[]() { PropertyMetadata m; m.DragSpeed = 0.01f; return m; }()),
			Properties::Make("MinValue",     "Min Value",     &SliderComponent::MinValue),
			Properties::Make("MaxValue",     "Max Value",     &SliderComponent::MaxValue),
			Properties::Make("WholeNumbers", "Whole Numbers", &SliderComponent::WholeNumbers),
		};
		sliderInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<SliderComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("value",        Json::Value(c.Value));
			v.AddMember("min",          Json::Value(c.MinValue));
			v.AddMember("max",          Json::Value(c.MaxValue));
			v.AddMember("wholeNumbers", Json::Value(c.WholeNumbers));
			return v;
		};
		sliderInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<SliderComponent>();
			if (const Json::Value* m = v.FindMember("value"))        c.Value        = static_cast<float>(m->AsDoubleOr(c.Value));
			if (const Json::Value* m = v.FindMember("min"))          c.MinValue     = static_cast<float>(m->AsDoubleOr(c.MinValue));
			if (const Json::Value* m = v.FindMember("max"))          c.MaxValue     = static_cast<float>(m->AsDoubleOr(c.MaxValue));
			if (const Json::Value* m = v.FindMember("wholeNumbers")) c.WholeNumbers = m->AsBoolOr(c.WholeNumbers);
		};
		sceneManager.RegisterComponentType<SliderComponent>(sliderInfo);

		ComponentInfo inputFieldInfo{ "Input Field", "UI", ComponentCategory::Component };
		inputFieldInfo.serializedName = "InputField";
		inputFieldInfo.properties = {
			Properties::Make("Text",            "Text",            &InputFieldComponent::Text),
			Properties::Make("PlaceholderText", "Placeholder",     &InputFieldComponent::PlaceholderText),
			Properties::Make("CharacterLimit",  "Character Limit", &InputFieldComponent::CharacterLimit),
			Properties::Make("TextColor",       "Text Color",      &InputFieldComponent::TextColor),
			Properties::Make("PlaceholderColor","Placeholder Color", &InputFieldComponent::PlaceholderColor),
		};
		inputFieldInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<InputFieldComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("text",             Json::Value(c.Text));
			v.AddMember("placeholder",      Json::Value(c.PlaceholderText));
			v.AddMember("characterLimit",   Json::Value(c.CharacterLimit));
			v.AddMember("textColor",        UIColorToJson(c.TextColor));
			v.AddMember("placeholderColor", UIColorToJson(c.PlaceholderColor));
			return v;
		};
		inputFieldInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<InputFieldComponent>();
			if (const Json::Value* m = v.FindMember("text"))             c.Text             = m->AsStringOr(c.Text);
			if (const Json::Value* m = v.FindMember("placeholder"))      c.PlaceholderText  = m->AsStringOr(c.PlaceholderText);
			if (const Json::Value* m = v.FindMember("characterLimit"))   c.CharacterLimit   = m->AsIntOr(c.CharacterLimit);
			if (const Json::Value* m = v.FindMember("textColor"))        c.TextColor        = UIColorFromJson(*m, c.TextColor);
			if (const Json::Value* m = v.FindMember("placeholderColor")) c.PlaceholderColor = UIColorFromJson(*m, c.PlaceholderColor);
		};
		sceneManager.RegisterComponentType<InputFieldComponent>(inputFieldInfo);

		ComponentInfo dropdownInfo{ "Dropdown", "UI", ComponentCategory::Component };
		dropdownInfo.serializedName = "Dropdown";
		dropdownInfo.properties = {
			Properties::Make("SelectedIndex", "Selected Index", &DropdownComponent::SelectedIndex),
			Properties::Make("OptionRowHeight", "Row Height", &DropdownComponent::OptionRowHeight),
			Properties::Make("PopupBackgroundColor", "Popup Background", &DropdownComponent::PopupBackgroundColor),
			Properties::Make("OptionTextColor",      "Option Text Color", &DropdownComponent::OptionTextColor),
			Properties::Make("OptionHoverColor",     "Option Hover Color", &DropdownComponent::OptionHoverColor),
		};
		dropdownInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<DropdownComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("selectedIndex",   Json::Value(c.SelectedIndex));
			v.AddMember("rowHeight",       Json::Value(c.OptionRowHeight));
			v.AddMember("popupBackground", UIColorToJson(c.PopupBackgroundColor));
			v.AddMember("optionTextColor", UIColorToJson(c.OptionTextColor));
			v.AddMember("optionHoverColor",UIColorToJson(c.OptionHoverColor));
			Json::Value optionsArr = Json::Value::MakeArray();
			for (const std::string& opt : c.Options) {
				optionsArr.Append(Json::Value(opt));
			}
			v.AddMember("options", std::move(optionsArr));
			return v;
		};
		dropdownInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<DropdownComponent>();
			if (const Json::Value* m = v.FindMember("selectedIndex"))   c.SelectedIndex        = m->AsIntOr(c.SelectedIndex);
			if (const Json::Value* m = v.FindMember("rowHeight"))       c.OptionRowHeight      = static_cast<float>(m->AsDoubleOr(c.OptionRowHeight));
			if (const Json::Value* m = v.FindMember("popupBackground")) c.PopupBackgroundColor = UIColorFromJson(*m, c.PopupBackgroundColor);
			if (const Json::Value* m = v.FindMember("optionTextColor")) c.OptionTextColor      = UIColorFromJson(*m, c.OptionTextColor);
			if (const Json::Value* m = v.FindMember("optionHoverColor"))c.OptionHoverColor     = UIColorFromJson(*m, c.OptionHoverColor);
			if (const Json::Value* m = v.FindMember("options"); m && m->IsArray()) {
				c.Options.clear();
				for (const Json::Value& item : m->GetArray()) {
					c.Options.push_back(item.AsStringOr(""));
				}
			}
		};
		sceneManager.RegisterComponentType<DropdownComponent>(dropdownInfo);

		ComponentInfo toggleInfo{ "Toggle", "UI", ComponentCategory::Component };
		toggleInfo.serializedName = "Toggle";
		toggleInfo.properties = {
			Properties::Make("IsOn", "Is On", &ToggleComponent::IsOn),
		};
		toggleInfo.serialize = [](Entity e) -> Json::Value {
			const auto& c = e.GetComponent<ToggleComponent>();
			Json::Value v = Json::Value::MakeObject();
			v.AddMember("isOn", Json::Value(c.IsOn));
			return v;
		};
		toggleInfo.deserialize = [](Entity e, const Json::Value& v) {
			auto& c = e.GetComponent<ToggleComponent>();
			if (const Json::Value* m = v.FindMember("isOn")) c.IsOn = m->AsBoolOr(c.IsOn);
		};
		sceneManager.RegisterComponentType<ToggleComponent>(toggleInfo);

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

		// Tags (not user-addable)
		RegisterComponent<IdTag>(sceneManager, "Id", ComponentCategory::Tag);
		RegisterComponent<StaticTag>(sceneManager, "Static", ComponentCategory::Tag);
		RegisterComponent<DisabledTag>(sceneManager, "Disabled", ComponentCategory::Tag);
		RegisterComponent<DeadlyTag>(sceneManager, "Deadly", ComponentCategory::Tag);
	}
}
