#include "pch.hpp"
#include "EntityHelper.hpp"

#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/UI/ButtonComponent.hpp"
#include "Components/UI/DropdownComponent.hpp"
#include "Components/UI/InputFieldComponent.hpp"
#include "Components/UI/InteractableComponent.hpp"
#include "Components/UI/SliderComponent.hpp"
#include "Components/UI/ToggleComponent.hpp"

namespace Axiom {
	void EntityHelper::SetEnabled(Entity entity, bool enabled) {
		entity.SetEnabled(enabled);
	}
	bool EntityHelper::IsEnabled(Entity entity) {
		return !entity.HasComponent<DisabledTag>();
	}

	std::size_t EntityHelper::EntitiesCount() {
		std::size_t count = 0;

		SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
			count += scene.GetRegistry().view<EntityHandle>().size();
			});

		return count;
	}

	Entity EntityHelper::CreateCamera2DEntity(Scene& scene) {
		Entity entity = CreateWith<Transform2DComponent, Camera2DComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Camera 2D"));
		return entity;
	}

	Entity EntityHelper::CreateCamera2DEntity() {
		Entity entity = CreateWith<Transform2DComponent, Camera2DComponent>();
		entity.AddComponent<NameComponent>(NameComponent("Camera 2D"));
		return entity;
	}

	Entity EntityHelper::CreateSpriteEntity(Scene& scene) {
		return CreateWith<Transform2DComponent, SpriteRendererComponent>(scene);
	}

	Entity EntityHelper::CreateSpriteEntity() {
		return CreateWith<Transform2DComponent, SpriteRendererComponent >();
	}

	Entity EntityHelper::CreateImageEntity(Scene& scene) {
		Entity e = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		e.AddComponent<NameComponent>(NameComponent("Image"));
		auto& rect = e.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 100.0f, 100.0f };
		return e;
	}

	Entity EntityHelper::CreateImageEntity() {
		Entity e = CreateWith<RectTransform2DComponent, ImageComponent>();
		e.AddComponent<NameComponent>(NameComponent("Image"));
		auto& rect = e.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 100.0f, 100.0f };
		return e;
	}

	// ── UI presets ──────────────────────────────────────────────────────
	// Each preset configures a "parent" entity (the widget root) plus
	// optional child entities for visuals that need their own rect /
	// renderer. Sizes are in framebuffer pixels (UIRenderer is screen-
	// space at 1 unit per pixel). UIEventSystem auto-resolves the
	// cross-entity references each frame, so the child structure here
	// is also what survives scene reload.

	Entity EntityHelper::CreateUIPanel(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Panel"));
		auto& img = entity.GetComponent<ImageComponent>();
		img.Color = Color{ 0.18f, 0.18f, 0.20f, 0.92f };
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 320.0f, 220.0f };
		return entity;
	}

	Entity EntityHelper::CreateUIButton(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, ButtonComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Button"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 180.0f, 44.0f };

		// Pleasant default palette (looks good on a dark editor view).
		auto& btn = entity.GetComponent<ButtonComponent>();
		btn.NormalColor   = Color{ 0.30f, 0.55f, 0.95f, 1.0f };
		btn.HoveredColor  = Color{ 0.42f, 0.65f, 1.00f, 1.0f };
		btn.PressedColor  = Color{ 0.20f, 0.40f, 0.80f, 1.0f };
		btn.DisabledColor = Color{ 0.45f, 0.45f, 0.45f, 0.5f };

		// Image starts at NormalColor; UIEventSystem retints from
		// ButtonComponent every frame, so the explicit color is just
		// the value at rest before the first system tick.
		entity.GetComponent<ImageComponent>().Color = btn.NormalColor;

		// Label child stretches to fill the parent so the text reflows
		// when the button is resized. Centred alignment makes it look
		// right at the default size too.
		Entity label = CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
		label.AddComponent<NameComponent>(NameComponent("Label"));
		auto& labelRect = label.GetComponent<RectTransform2DComponent>();
		labelRect.AnchorMin = Vec2{ 0.0f, 0.0f };
		labelRect.AnchorMax = Vec2{ 1.0f, 1.0f };
		labelRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		labelRect.SizeDelta = Vec2{ 0.0f, 0.0f };
		auto& labelText = label.GetComponent<TextRendererComponent>();
		labelText.Text = "Button";
		labelText.Color = Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		labelText.HAlign = TextAlignment::Center;
		labelText.FontSize = 18.0f;
		label.SetParent(entity);

		return entity;
	}

	Entity EntityHelper::CreateUISlider(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, SliderComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Slider"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 220.0f, 20.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.18f, 0.20f, 0.24f, 1.0f };

		// Fill child — auto-resized by UIEventSystem to span [0, t] of
		// the track. We anchor it stretched-vertically and pin to the
		// left edge so it always grows from the left.
		Entity fill = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		fill.AddComponent<NameComponent>(NameComponent("Fill"));
		auto& fillRect = fill.GetComponent<RectTransform2DComponent>();
		fillRect.AnchorMin = Vec2{ 0.0f, 0.0f };
		fillRect.AnchorMax = Vec2{ 0.5f, 1.0f };
		fillRect.Pivot = Vec2{ 0.0f, 0.5f };
		fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		fillRect.SizeDelta = Vec2{ 0.0f, 0.0f };
		fill.GetComponent<ImageComponent>().Color = Color{ 0.30f, 0.55f, 0.95f, 1.0f };
		fill.SetParent(entity);

		// Handle child — the bit the user visually drags. Width is
		// fixed; UIEventSystem repositions it along the track every
		// frame from SliderComponent::Value.
		Entity handle = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		handle.AddComponent<NameComponent>(NameComponent("Handle"));
		auto& handleRect = handle.GetComponent<RectTransform2DComponent>();
		handleRect.SizeDelta = Vec2{ 18.0f, 28.0f };
		handle.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };
		handle.SetParent(entity);

		auto& slider = entity.GetComponent<SliderComponent>();
		slider.HandleEntity = handle.GetHandle();
		slider.FillEntity = fill.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIInputField(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, InputFieldComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Input Field"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 240.0f, 36.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };

		// Text child stretches to fill the parent so the placeholder /
		// entered text always sits inside the box. Slight left-pad via
		// alignment.
		Entity textChild = CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
		textChild.AddComponent<NameComponent>(NameComponent("Text"));
		auto& textRect = textChild.GetComponent<RectTransform2DComponent>();
		textRect.AnchorMin = Vec2{ 0.0f, 0.0f };
		textRect.AnchorMax = Vec2{ 1.0f, 1.0f };
		textRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		textRect.SizeDelta = Vec2{ 0.0f, 0.0f };
		auto& tc = textChild.GetComponent<TextRendererComponent>();
		tc.Text = "Enter text...";
		tc.Color = Color{ 0.55f, 0.55f, 0.55f, 1.0f };
		tc.FontSize = 16.0f;
		tc.HAlign = TextAlignment::Left;
		textChild.SetParent(entity);

		entity.GetComponent<InputFieldComponent>().TextEntity = textChild.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIDropdown(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, DropdownComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Dropdown"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 220.0f, 36.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };

		auto& dd = entity.GetComponent<DropdownComponent>();
		dd.Options = { "Option A", "Option B", "Option C" };
		dd.SelectedIndex = 0;

		Entity labelChild = CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
		labelChild.AddComponent<NameComponent>(NameComponent("Label"));
		auto& labelRect = labelChild.GetComponent<RectTransform2DComponent>();
		labelRect.AnchorMin = Vec2{ 0.0f, 0.0f };
		labelRect.AnchorMax = Vec2{ 1.0f, 1.0f };
		labelRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		labelRect.SizeDelta = Vec2{ 0.0f, 0.0f };
		auto& tc = labelChild.GetComponent<TextRendererComponent>();
		tc.Text = dd.Options.empty() ? "" : dd.Options[0];
		tc.Color = Color{ 0.10f, 0.10f, 0.10f, 1.0f };
		tc.FontSize = 16.0f;
		tc.HAlign = TextAlignment::Left;
		labelChild.SetParent(entity);

		dd.LabelEntity = labelChild.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIToggle(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, ToggleComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Toggle"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 28.0f, 28.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };

		// Checkmark child — anchored to the box's centre with a small
		// margin so the on-state shows a smaller filled square.
		// UIEventSystem flips it enabled/disabled from ToggleComponent::IsOn.
		Entity check = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		check.AddComponent<NameComponent>(NameComponent("Checkmark"));
		auto& checkRect = check.GetComponent<RectTransform2DComponent>();
		checkRect.AnchorMin = Vec2{ 0.0f, 0.0f };
		checkRect.AnchorMax = Vec2{ 1.0f, 1.0f };
		checkRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		// Negative SizeDelta = inset by 8px on each side (4 per edge).
		checkRect.SizeDelta = Vec2{ -8.0f, -8.0f };
		check.GetComponent<ImageComponent>().Color = Color{ 0.30f, 0.65f, 0.30f, 1.0f };
		check.AddComponent<DisabledTag>();
		check.SetParent(entity);

		entity.GetComponent<ToggleComponent>().CheckmarkEntity = check.GetHandle();
		return entity;
	}
}
