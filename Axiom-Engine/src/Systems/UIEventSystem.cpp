#include "pch.hpp"
#include "Systems/UIEventSystem.hpp"

#include "Collections/Viewport.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/UI/ButtonComponent.hpp"
#include "Components/UI/DropdownComponent.hpp"
#include "Components/UI/InputFieldComponent.hpp"
#include "Components/UI/InteractableComponent.hpp"
#include "Components/UI/CircularSliderComponent.hpp"
#include "Components/UI/ScrollRectComponent.hpp"
#include "Components/UI/ScrollbarComponent.hpp"
#include "Components/UI/SliderComponent.hpp"
#include "Components/UI/ToggleComponent.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/Application.hpp"
#include "Core/Input.hpp"
#include "Core/KeyCodes.hpp"
#include "Core/Log.hpp"
#include "Core/Time.hpp"
#include "Core/UUID.hpp"
#include "Core/Window.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/UIDrawOrder.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/InspectorEventDispatch.hpp"
#include "Scripting/ScriptEngine.hpp"

#include <unordered_map>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace Axiom {

	namespace {

		// Convert raw GLFW pixel coords (top-left origin, +Y down) to the
		// centred-origin, +Y-up screen space the layout system uses.
		Vec2 ScreenPixelToUiSpace(const Vec2& mouseRaw, int viewportWidth, int viewportHeight) {
			return Vec2{
				mouseRaw.x - static_cast<float>(viewportWidth) * 0.5f,
				static_cast<float>(viewportHeight) * 0.5f - mouseRaw.y
			};
		}

		// Generic state-color resolver used by every widget tint pass.
		// Precedence (highest first): disabled > pressed > focused >
		// hovered > normal. Focused only applies when the widget opted
		// into navigation (Focusable + IsFocused) AND its FocusedColor
		// has a non-zero alpha — the alpha == 0 sentinel means "no
		// focus tint, fall through" and is the default for every
		// widget preset, so existing scenes never gain a focus border
		// they didn't ask for.
		Color ResolveStateTint(const Color& normal, const Color& hovered,
			const Color& pressed, const Color& disabled,
			const Color& focused,
			const InteractableComponent& interact)
		{
			if (!interact.Interactable) return disabled;
			if (interact.IsPressed)     return pressed;
			if (interact.Focusable && interact.IsFocused && focused.a > 0.0f) {
				return focused;
			}
			if (interact.IsHovered)     return hovered;
			return normal;
		}

		Color ResolveButtonTint(const ButtonComponent& btn,
			const InteractableComponent& interact)
		{
			return ResolveStateTint(btn.NormalColor, btn.HoveredColor,
				btn.PressedColor, btn.DisabledColor, btn.FocusedColor, interact);
		}

		// Pick the per-state sprite UUID for the current Interactable
		// state, with the same precedence as ResolveStateTint. UUID{0}
		// in any per-state slot means "unset, fall back to NormalSprite".
		// If NormalSprite is also unset, the helper returns 0 — callers
		// treat that as "leave the texture alone".
		UUID ResolveStateSprite(UUID normal, UUID hovered, UUID pressed,
			UUID disabled, UUID focused,
			const InteractableComponent& interact)
		{
			UUID candidate{ 0 };
			if (!interact.Interactable)                                  candidate = disabled;
			else if (interact.IsPressed)                                 candidate = pressed;
			else if (interact.Focusable && interact.IsFocused)           candidate = focused;
			else if (interact.IsHovered)                                 candidate = hovered;
			else                                                         candidate = normal;
			if (static_cast<uint64_t>(candidate) == 0) candidate = normal;
			return candidate;
		}

		// Swap the image's texture to `desired` if it isn't already
		// pointing there. Skips the swap when desired == 0 so callers
		// can use 0 as "no override; keep authored texture". Loads the
		// runtime handle on demand via TextureManager so SpriteSwap
		// works the same way Image_SetTexture does in scripts.
		void ApplySpriteIfChanged(ImageComponent& image, UUID desired) {
			if (static_cast<uint64_t>(desired) == 0) return;
			if (image.TextureAssetId == desired) return;
			image.TextureAssetId = desired;
			image.TextureHandle = TextureManager::LoadTextureByUUID(
				static_cast<uint64_t>(desired));
		}

		// Apply the per-state visual to a widget's resolved image based
		// on TransitionMode:
		//   ColorTint  → write image.Color, leave the texture alone.
		//   SpriteSwap → swap image.TextureAssetId / TextureHandle for
		//                the resolved per-state sprite, leave Color alone.
		//   None       → no-op; the user owns the visual.
		// Centralising the dispatch here means every widget's tint pass
		// stays a single line, and adding a new transition mode is one
		// switch case rather than five.
		void ApplyWidgetVisualState(ImageComponent& image,
			const InteractableComponent& interact,
			UITransitionMode mode,
			const Color& normalC, const Color& hoveredC, const Color& pressedC,
			const Color& disabledC, const Color& focusedC,
			UUID normalS, UUID hoveredS, UUID pressedS,
			UUID disabledS, UUID focusedS)
		{
			switch (mode) {
			case UITransitionMode::ColorTint:
				image.Color = ResolveStateTint(normalC, hoveredC, pressedC,
					disabledC, focusedC, interact);
				return;
			case UITransitionMode::SpriteSwap: {
				const UUID id = ResolveStateSprite(normalS, hoveredS, pressedS,
					disabledS, focusedS, interact);
				ApplySpriteIfChanged(image, id);
				return;
			}
			case UITransitionMode::None:
				return;
			}
		}

		// Resolve a target entity's visual component (Image preferred,
		// TextRenderer as fallback) and apply the per-state visual the
		// same way ApplyWidgetVisualState does. Used by Button so a
		// button can sit on any entity that has either an Image or a
		// TextRenderer — the preset is one valid setup, not a
		// requirement. SpriteSwap on a text-only target is a no-op
		// (text glyphs aren't textures) so the colour path still runs
		// when ColorTint is the active mode.
		void ApplyWidgetVisualToEntity(entt::registry& registry, EntityHandle target,
			const InteractableComponent& interact,
			UITransitionMode mode,
			const Color& normalC, const Color& hoveredC, const Color& pressedC,
			const Color& disabledC, const Color& focusedC,
			UUID normalS, UUID hoveredS, UUID pressedS,
			UUID disabledS, UUID focusedS)
		{
			if (target == entt::null || !registry.valid(target)) return;
			if (auto* img = registry.try_get<ImageComponent>(target)) {
				ApplyWidgetVisualState(*img, interact, mode,
					normalC, hoveredC, pressedC, disabledC, focusedC,
					normalS, hoveredS, pressedS, disabledS, focusedS);
				return;
			}
			if (auto* txt = registry.try_get<TextRendererComponent>(target)) {
				if (mode == UITransitionMode::ColorTint) {
					txt->Color = ResolveStateTint(normalC, hoveredC, pressedC,
						disabledC, focusedC, interact);
				}
				// SpriteSwap and None are no-ops for text targets.
			}
		}

		void SetEntityEnabled(Scene& scene, EntityHandle entity, bool enabled) {
			if (!scene.IsValid(entity)) return;
			scene.GetEntity(entity).SetEnabled(enabled);
		}

		// ── UTF-8 helpers ───────────────────────────────────────────
		// All input-field caret math is byte-indexed (matches std::string),
		// but UTF-8 multi-byte codepoints must move as a unit so we don't
		// land mid-sequence and corrupt the string.

		// Byte length of the codepoint that starts at `s[idx]`. Treats a
		// stray continuation byte (10xxxxxx) as length 1 so callers always
		// make forward progress.
		int Utf8CodepointLength(std::string_view s, int idx) {
			if (idx < 0 || idx >= static_cast<int>(s.size())) return 0;
			const unsigned char c = static_cast<unsigned char>(s[idx]);
			if (c < 0x80) return 1;
			if ((c & 0xE0) == 0xC0) return 2;
			if ((c & 0xF0) == 0xE0) return 3;
			if ((c & 0xF8) == 0xF0) return 4;
			return 1;
		}

		// Decode the codepoint starting at `s[idx]`. Returns false past the
		// end. Handles malformed bytes by yielding one byte and treating it
		// as a Latin-1 codepoint.
		bool Utf8Decode(std::string_view s, int idx, uint32_t& outCp, int& outLen) {
			if (idx < 0 || idx >= static_cast<int>(s.size())) {
				outCp = 0; outLen = 0; return false;
			}
			const unsigned char b = static_cast<unsigned char>(s[idx]);
			if (b < 0x80) { outCp = b; outLen = 1; return true; }
			int len; uint32_t cp;
			if ((b & 0xE0) == 0xC0)      { len = 2; cp = b & 0x1F; }
			else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; }
			else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; }
			else { outCp = b; outLen = 1; return true; }
			if (idx + len > static_cast<int>(s.size())) {
				outCp = b; outLen = 1; return true;
			}
			for (int i = 1; i < len; ++i) {
				cp = (cp << 6) | (static_cast<unsigned char>(s[idx + i]) & 0x3F);
			}
			outCp = cp; outLen = len; return true;
		}

		// Move one codepoint to the left, walking past continuation bytes
		// (10xxxxxx). Returns 0 when already at the start.
		int Utf8PrevByte(std::string_view s, int byteIdx) {
			const int n = static_cast<int>(s.size());
			if (byteIdx <= 0) return 0;
			if (byteIdx > n) byteIdx = n;
			--byteIdx;
			while (byteIdx > 0) {
				const unsigned char c = static_cast<unsigned char>(s[byteIdx]);
				if ((c & 0xC0) != 0x80) break;
				--byteIdx;
			}
			return byteIdx;
		}

		// Move one codepoint to the right.
		int Utf8NextByte(std::string_view s, int byteIdx) {
			const int n = static_cast<int>(s.size());
			if (byteIdx < 0) return 0;
			if (byteIdx >= n) return n;
			return std::min(byteIdx + Utf8CodepointLength(s, byteIdx), n);
		}

		int Utf8CountCodepoints(std::string_view s) {
			int n = 0;
			for (int i = 0; i < static_cast<int>(s.size()); ) {
				int len = Utf8CodepointLength(s, i);
				if (len <= 0) break;
				i += len;
				++n;
			}
			return n;
		}

		// One '*' per codepoint in `src`. Used by both the visual sync
		// (UIEventSystem writes the masked string into the child
		// TextRenderer when InputField.IsSecret is true) and by
		// hit-testing helpers that need to project byte offsets from
		// the real Text into the rendered mask.
		std::string MaskTextForSecret(std::string_view src) {
			std::string out;
			out.reserve(src.size());
			int idx = 0;
			const int n = static_cast<int>(src.size());
			while (idx < n) {
				int len = Utf8CodepointLength(src, idx);
				if (len <= 0) break;
				out.push_back('*');
				idx += len;
			}
			return out;
		}

		// ── ContentType filtering ───────────────────────────────────
		// Decide whether a single codepoint is allowed under `type`,
		// given the current `existing` text and the `caretByte` insertion
		// point. `existing`/`caretByte` only matter for numeric types
		// (where '-' is restricted to the start and decimal-numbers
		// allow at most one '.').

		bool ContentTypeAllowsCodepoint(InputContentType type, std::uint32_t cp,
			std::string_view existing, int caretByte)
		{
			// Reject control characters (newlines, tabs) outright — even
			// in Standard mode an input field is a single line.
			if (cp < 0x20) return false;
			if (cp == 0x7F) return false; // DEL

			switch (type) {
			case InputContentType::Standard:
				return true;
			case InputContentType::AlphaNumeric:
				return (cp >= '0' && cp <= '9')
					|| (cp >= 'A' && cp <= 'Z')
					|| (cp >= 'a' && cp <= 'z');
			case InputContentType::Alpha:
				return (cp >= 'A' && cp <= 'Z')
					|| (cp >= 'a' && cp <= 'z');
			case InputContentType::IntegerNumber: {
				if (cp >= '0' && cp <= '9') return true;
				if (cp == '-') {
					const bool atStart = (caretByte == 0);
					const bool noExistingMinus = existing.empty() || existing[0] != '-';
					return atStart && noExistingMinus;
				}
				return false;
			}
			case InputContentType::DecimalNumber: {
				if (cp >= '0' && cp <= '9') return true;
				if (cp == '-') {
					const bool atStart = (caretByte == 0);
					const bool noExistingMinus = existing.empty() || existing[0] != '-';
					return atStart && noExistingMinus;
				}
				if (cp == '.') {
					return existing.find('.') == std::string_view::npos;
				}
				return false;
			}
			}
			return true;
		}

		// Walk `src` codepoint-by-codepoint and return only those that
		// pass ContentTypeAllowsCodepoint when inserted at the caret of
		// `existing`. Used both for typed text and clipboard paste —
		// numeric fields need this to keep the insert order valid (e.g.
		// pasting "1.2.3" into a DecimalNumber field becomes "1.23").
		std::string FilterByContentType(InputContentType type, std::string_view src,
			std::string_view existing, int caretByte)
		{
			if (type == InputContentType::Standard) {
				// Still strip control chars so newlines from clipboard
				// can't sneak in and break the single-line layout.
				std::string out;
				out.reserve(src.size());
				int idx = 0;
				const int n = static_cast<int>(src.size());
				while (idx < n) {
					std::uint32_t cp; int len;
					if (!Utf8Decode(src, idx, cp, len)) break;
					if (idx + len > n) break;
					if (cp >= 0x20 && cp != 0x7F) {
						out.append(src.data() + idx, len);
					}
					idx += len;
				}
				return out;
			}

			std::string simulated{ existing };
			int simCaret = caretByte;
			std::string out;
			out.reserve(src.size());

			int idx = 0;
			const int n = static_cast<int>(src.size());
			while (idx < n) {
				std::uint32_t cp; int len;
				if (!Utf8Decode(src, idx, cp, len)) break;
				if (idx + len > n) break;

				if (ContentTypeAllowsCodepoint(type, cp, simulated, simCaret)) {
					out.append(src.data() + idx, len);
					simulated.insert(simCaret, src.data() + idx, len);
					simCaret += len;
				}
				idx += len;
			}
			return out;
		}

		// ── InputField selection helpers ────────────────────────────

		std::pair<int, int> SelectionRange(const InputFieldComponent& field) {
			const int n = static_cast<int>(field.Text.size());
			int lo = std::min(field.CaretBytePos, field.SelectionAnchorBytePos);
			int hi = std::max(field.CaretBytePos, field.SelectionAnchorBytePos);
			lo = std::clamp(lo, 0, n);
			hi = std::clamp(hi, 0, n);
			return { lo, hi };
		}

		bool HasSelection(const InputFieldComponent& field) {
			auto [lo, hi] = SelectionRange(field);
			return lo != hi;
		}

		void ClampCaret(InputFieldComponent& field) {
			const int n = static_cast<int>(field.Text.size());
			field.CaretBytePos = std::clamp(field.CaretBytePos, 0, n);
			field.SelectionAnchorBytePos = std::clamp(field.SelectionAnchorBytePos, 0, n);
		}

		// Wipe the active selection. No-op when caret == anchor.
		void DeleteSelection(InputFieldComponent& field) {
			auto [lo, hi] = SelectionRange(field);
			if (lo == hi) return;
			field.Text.erase(lo, hi - lo);
			field.CaretBytePos = lo;
			field.SelectionAnchorBytePos = lo;
		}

		// Insert UTF-8 text at the caret, replacing any active selection.
		// Truncates `text` to fit CharacterLimit (counted in codepoints,
		// so a 4-byte emoji costs 1 character not 4).
		void InsertAtCaret(InputFieldComponent& field, std::string_view text) {
			if (text.empty()) return;
			DeleteSelection(field);

			std::string toInsert{ text };
			if (field.CharacterLimit > 0) {
				const int currentCount = Utf8CountCodepoints(field.Text);
				const int allowed = field.CharacterLimit - currentCount;
				if (allowed <= 0) return;
				int kept = 0;
				int i = 0;
				while (i < static_cast<int>(toInsert.size()) && kept < allowed) {
					int len = Utf8CodepointLength(toInsert, i);
					if (len <= 0) break;
					if (i + len > static_cast<int>(toInsert.size())) break;
					i += len;
					++kept;
				}
				toInsert.resize(i);
			}
			if (toInsert.empty()) return;

			field.Text.insert(field.CaretBytePos, toInsert);
			field.CaretBytePos += static_cast<int>(toInsert.size());
			field.SelectionAnchorBytePos = field.CaretBytePos;
		}

		// Backspace = delete selection if any, else codepoint left of caret.
		void BackspaceOnce(InputFieldComponent& field) {
			if (HasSelection(field)) { DeleteSelection(field); return; }
			if (field.CaretBytePos <= 0) return;
			const int prev = Utf8PrevByte(field.Text, field.CaretBytePos);
			field.Text.erase(prev, field.CaretBytePos - prev);
			field.CaretBytePos = prev;
			field.SelectionAnchorBytePos = prev;
		}

		// Forward delete = selection or codepoint right of caret.
		void DeleteOnce(InputFieldComponent& field) {
			if (HasSelection(field)) { DeleteSelection(field); return; }
			const int n = static_cast<int>(field.Text.size());
			if (field.CaretBytePos >= n) return;
			const int next = Utf8NextByte(field.Text, field.CaretBytePos);
			field.Text.erase(field.CaretBytePos, next - field.CaretBytePos);
		}

		// ── Glyph metrics for caret/selection geometry ──────────────

		// Width in atlas units up to (but not including) `targetByte`. The
		// caller multiplies by the font scale to land in screen pixels.
		// Mirrors the per-glyph advance/kerning/letter-spacing accumulation
		// that EmitText uses, so the caret X stays aligned with the actual
		// rendered text down to the pixel.
		float MeasureUpToByte(const Font& font, std::string_view text, int targetByte,
			float letterSpacing)
		{
			if (targetByte <= 0) return 0.0f;
			const int n = static_cast<int>(text.size());
			if (targetByte > n) targetByte = n;
			float w = 0.0f;
			uint32_t prev = 0;
			int glyphCount = 0;
			int idx = 0;
			while (idx < targetByte) {
				uint32_t cp; int len;
				if (!Utf8Decode(text, idx, cp, len)) break;
				if (idx + len > targetByte) break;
				const GlyphMetrics* g = font.GetGlyph(cp);
				if (g) {
					if (prev != 0) w += font.GetKerning(prev, cp);
					w += g->XAdvance;
					if (glyphCount > 0) w += letterSpacing;
					++glyphCount;
					prev = cp;
				}
				else {
					prev = 0;
				}
				idx += len;
			}
			return w;
		}

		// Inverse: byte index whose preceding glyph midpoint sits at or to
		// the left of `relX` (screen pixels relative to text origin). Used
		// for click-to-place-caret and drag-to-extend-selection.
		int ByteFromRelativeX(const Font& font, std::string_view text,
			float relX, float scale, float letterSpacing)
		{
			if (text.empty() || relX <= 0.0f) return 0;
			float accX = 0.0f;
			uint32_t prev = 0;
			int glyphCount = 0;
			int idx = 0;
			const int n = static_cast<int>(text.size());
			while (idx < n) {
				uint32_t cp; int len;
				if (!Utf8Decode(text, idx, cp, len)) break;
				const GlyphMetrics* g = font.GetGlyph(cp);
				float advance = 0.0f;
				if (g) {
					if (prev != 0) advance += font.GetKerning(prev, cp);
					advance += g->XAdvance;
					if (glyphCount > 0) advance += letterSpacing;
				}
				const float advanceScreen = advance * scale;
				const float midX = accX + advanceScreen * 0.5f;
				if (relX < midX) return idx;
				accX += advanceScreen;
				if (g) { ++glyphCount; prev = cp; }
				idx += len;
			}
			return n;
		}

		// Resolve the text child's screen-space origin + font + scale so
		// caret math stays consistent with what GuiRenderer paints. Returns
		// nullptr-bearing layout when the text child or its font isn't
		// available; callers fall back to caret = 0 in that case.
		struct InputTextLayout {
			bool Valid = false;
			float OriginX = 0.0f;
			float Scale = 1.0f;
			float LetterSpacing = 0.0f;
			Font* FontPtr = nullptr;
			TextAlignment Align = TextAlignment::Left;
		};

		InputTextLayout ResolveInputTextLayout(entt::registry& registry,
			const InputFieldComponent& field)
		{
			InputTextLayout out{};
			if (field.TextEntity == entt::null || !registry.valid(field.TextEntity)) return out;
			if (!registry.all_of<RectTransform2DComponent, TextRendererComponent>(field.TextEntity)) return out;

			auto& rect = registry.get<RectTransform2DComponent>(field.TextEntity);
			auto& tc = registry.get<TextRendererComponent>(field.TextEntity);
			Font* font = TextRenderer::ResolveFont(tc);
			if (!font || !font->IsLoaded()) return out;

			// Mirror GuiRenderer's text-scale rule: the rect's world scale
			// grows the rendered glyph metrics, so click-to-caret has to
			// measure against the same scaled glyphs or hit-testing lands
			// on the wrong byte for any non-1.0 scale.
			const float uniformScale = rect.Scale.x;
			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const float w = tr.x - bl.x;
			float originX;
			switch (tc.HAlign) {
			case TextAlignment::Center: originX = bl.x + w * 0.5f;         break;
			case TextAlignment::Right:  originX = tr.x - 4.0f * uniformScale; break;
			case TextAlignment::Left:
			default:                    originX = bl.x + 4.0f * uniformScale; break;
			}

			const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : tc.FontSize;
			out.Valid = true;
			out.OriginX = originX;
			out.Scale = (tc.FontSize / bakedSize) * uniformScale;
			out.LetterSpacing = tc.LetterSpacing;
			out.FontPtr = font;
			out.Align = tc.HAlign;
			return out;
		}

		int ByteFromMouseX(entt::registry& registry, const InputFieldComponent& field, Vec2 mouseUi) {
			InputTextLayout layout = ResolveInputTextLayout(registry, field);
			if (!layout.Valid) return 0;

			// Secret fields render one '*' per codepoint; click-to-
			// caret has to measure against that masked string so the
			// glyph widths match what the user actually sees, then the
			// resulting byte index (which is in the mask's coordinate
			// space) is mapped back into field.Text by counting one
			// codepoint per mask byte.
			std::string maskBuffer;
			std::string_view measureView{ field.Text };
			if (field.IsSecret && !field.Text.empty()) {
				maskBuffer = MaskTextForSecret(field.Text);
				measureView = maskBuffer;
			}

			float relX = mouseUi.x - layout.OriginX;
			if (layout.Align == TextAlignment::Center) {
				const float lineW = MeasureUpToByte(*layout.FontPtr, measureView,
					static_cast<int>(measureView.size()), layout.LetterSpacing) * layout.Scale;
				relX += lineW * 0.5f;
			}
			else if (layout.Align == TextAlignment::Right) {
				const float lineW = MeasureUpToByte(*layout.FontPtr, measureView,
					static_cast<int>(measureView.size()), layout.LetterSpacing) * layout.Scale;
				relX += lineW;
			}
			const int byteInMeasure = ByteFromRelativeX(*layout.FontPtr, measureView, relX,
				layout.Scale, layout.LetterSpacing);

			if (!field.IsSecret) return byteInMeasure;

			// Mask bytes correspond 1:1 to field.Text codepoints, so
			// walk that many codepoints in field.Text to land on the
			// matching byte offset.
			int idx = 0;
			int count = 0;
			const int targetCount = byteInMeasure;
			const int n = static_cast<int>(field.Text.size());
			while (idx < n && count < targetCount) {
				int len = Utf8CodepointLength(field.Text, idx);
				if (len <= 0) break;
				idx += len;
				++count;
			}
			return idx;
		}

		// Tunables for hold-to-repeat. Values match common OS conventions:
		// initial wait of ~400ms before auto-fire kicks in, then ~25 hz.
		constexpr float k_KeyHoldDelay = 0.4f;
		constexpr float k_KeyHoldRate = 0.04f;

		// Cursor must travel this far (in UI-space pixels) after pressing
		// a slider handle before drag tracking begins. Below this, a press
		// holds the current value steady — keeps a tap from snapping the
		// value to the cursor's exact position when the click landed
		// slightly off-centre on the handle.
		constexpr float k_SliderDragThresholdPx = 4.0f;

		// Find the first child of `parent` matching the predicate. Used
		// to auto-resolve cross-entity refs (Slider's HandleEntity,
		// Toggle's CheckmarkEntity, InputField's TextEntity, Dropdown's
		// LabelEntity) so they survive scene reload without explicit
		// UUID round-tripping. Skip a child whose UUID matches `skip`
		// — that lets a slider with a Fill child also have a separate
		// Handle child without resolving both to the same entity.
		template <typename Pred>
		EntityHandle FindFirstChildWith(entt::registry& registry,
			EntityHandle parent, Pred&& predicate, EntityHandle skip = entt::null)
		{
			if (!registry.valid(parent)) return entt::null;
			auto* hierarchy = registry.try_get<HierarchyComponent>(parent);
			if (!hierarchy) return entt::null;
			for (EntityHandle child : hierarchy->Children) {
				if (!registry.valid(child)) continue;
				if (child == skip) continue;
				if (predicate(child)) return child;
			}
			return entt::null;
		}

		// First-by-name variant. Falls through to the predicate-only
		// FindFirstChildWith when no child has the requested name. The
		// presets in EntityHelper.cpp tag children with stable names
		// ("Handle", "Fill", "Text", "Label", "Checkmark") so the
		// post-reload auto-resolve picks the right child even when a
		// component has multiple children of the same shape (the slider
		// case: two Image children — one fill, one handle).
		template <typename Pred, typename... SkipEntities>
		EntityHandle FindFirstChildByNameOrPredicate(entt::registry& registry,
			EntityHandle parent, std::string_view preferredName,
			Pred&& predicate, SkipEntities... skipEntities)
		{
			if (!registry.valid(parent)) return entt::null;
			auto* hierarchy = registry.try_get<HierarchyComponent>(parent);
			if (!hierarchy) return entt::null;

			// Variadic skip list — caller can pass 0..N entities to
			// avoid claiming the same child twice when resolving multiple
			// reference fields on the same parent (e.g. Slider's Fill /
			// Handle / Background all live among the same image
			// children).
			auto isSkipped = [&]([[maybe_unused]] EntityHandle child) {
				// Fold over the parameter pack — empty pack collapses
				// to `false`, so MSVC would otherwise flag `child` as
				// unused when no skip args are passed.
				return ((child == skipEntities) || ...);
			};

			// First pass: name match wins.
			for (EntityHandle child : hierarchy->Children) {
				if (!registry.valid(child)) continue;
				if (isSkipped(child)) continue;
				if (auto* name = registry.try_get<NameComponent>(child)) {
					if (name->Name == preferredName && predicate(child)) {
						return child;
					}
				}
			}
			// Second pass: shape match (any child satisfying the predicate).
			for (EntityHandle child : hierarchy->Children) {
				if (!registry.valid(child)) continue;
				if (isSkipped(child)) continue;
				if (predicate(child)) return child;
			}
			return entt::null;
		}

		// Slider visual sync: write the slider's Fill child SizeDelta and
		// Handle child position so they reflect (Value - MinValue) / range.
		// Pulled out of UIEventSystem::Update so OnPreRender can call it
		// in edit mode — that's the path that drives the editor preview
		// when the inspector tweaks Slider.Value/MinValue/MaxValue without
		// entering play mode. Pure visual-state writes; doesn't touch
		// `slider.Value` aside from a defensive clamp into the authored
		// range, so it's safe to call multiple times per frame.
		//
		// True when the slider's value axis runs along X. Vertical
		// directions (BottomToTop / TopToBottom) walk along Y.
		bool IsSliderHorizontal(SliderDirection dir) {
			return dir == SliderDirection::LeftToRight
				|| dir == SliderDirection::RightToLeft;
		}

		// True when the slider's Value=MinValue lives on the visual
		// end of the axis (Right or Top). Used to flip the handle/fill
		// geometry while keeping the value contract the same.
		bool IsSliderReversed(SliderDirection dir) {
			return dir == SliderDirection::RightToLeft
				|| dir == SliderDirection::TopToBottom;
		}

		// Rewrite the slider's Fill and Handle child rects so they
		// reflect (Value - MinValue) / range along the active axis.
		// The fill grows from the start edge of the chosen Direction
		// regardless of the fill's authored pivot — the slider system
		// owns the fill's geometry. The handle is centered at the
		// track edge corresponding to the current Value.
		void ApplySliderVisuals(entt::registry& registry,
			SliderComponent& slider, const RectTransform2DComponent& rect)
		{
			slider.Value = std::clamp(slider.Value,
				std::min(slider.MinValue, slider.MaxValue),
				std::max(slider.MinValue, slider.MaxValue));

			const float range = slider.MaxValue - slider.MinValue;
			const float t = (range != 0.0f) ? (slider.Value - slider.MinValue) / range : 0.0f;
			// Track size is the slider's own authored SizeDelta — not its
			// resolved size — so the fill / handle layout stays stable
			// even before the first layout pass writes ResolvedMin/Max.
			const float trackWidth  = rect.SizeDelta.x;
			const float trackHeight = rect.SizeDelta.y;

			const bool horizontal = IsSliderHorizontal(slider.Direction);
			const bool reversed   = IsSliderReversed(slider.Direction);

			// Visual t flips for reversed directions so Value=Min still
			// lands at the visual start of the chosen direction.
			const float visualT = reversed ? (1.0f - t) : t;

			if (slider.HandleEntity != entt::null
				&& registry.valid(slider.HandleEntity)
				&& registry.all_of<RectTransform2DComponent>(slider.HandleEntity))
			{
				auto& handleRect = registry.get<RectTransform2DComponent>(slider.HandleEntity);
				if (horizontal) {
					// Handle's centre tracks the track's centred axis from
					// left edge (visualT=0) to right edge (visualT=1).
					handleRect.AnchoredPosition.x = -trackWidth * 0.5f + trackWidth * visualT;
				}
				else {
					handleRect.AnchoredPosition.y = -trackHeight * 0.5f + trackHeight * visualT;
				}
			}

			if (slider.FillEntity != entt::null
				&& registry.valid(slider.FillEntity)
				&& registry.all_of<RectTransform2DComponent>(slider.FillEntity))
			{
				auto& fillRect = registry.get<RectTransform2DComponent>(slider.FillEntity);
				// Pin the fill to the start edge of the active direction.
				// Anchor + Pivot are written each frame so the fill grows
				// the right way regardless of how the user authored them.
				// The other axis fills the track (Anchor=0..1) so authored
				// height/width on that axis is preserved via Stretch + the
				// SizeDelta-only-sizes layout rule (no padding added).
				switch (slider.Direction) {
				case SliderDirection::LeftToRight:
					fillRect.AnchorMin = Vec2{ 0.0f, 0.5f };
					fillRect.AnchorMax = Vec2{ 0.0f, 0.5f };
					fillRect.Pivot = Vec2{ 0.0f, 0.5f };
					fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
					fillRect.SizeDelta = Vec2{ trackWidth * t, trackHeight };
					break;
				case SliderDirection::RightToLeft:
					fillRect.AnchorMin = Vec2{ 1.0f, 0.5f };
					fillRect.AnchorMax = Vec2{ 1.0f, 0.5f };
					fillRect.Pivot = Vec2{ 1.0f, 0.5f };
					fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
					fillRect.SizeDelta = Vec2{ trackWidth * t, trackHeight };
					break;
				case SliderDirection::BottomToTop:
					fillRect.AnchorMin = Vec2{ 0.5f, 0.0f };
					fillRect.AnchorMax = Vec2{ 0.5f, 0.0f };
					fillRect.Pivot = Vec2{ 0.5f, 0.0f };
					fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
					fillRect.SizeDelta = Vec2{ trackWidth, trackHeight * t };
					break;
				case SliderDirection::TopToBottom:
					fillRect.AnchorMin = Vec2{ 0.5f, 1.0f };
					fillRect.AnchorMax = Vec2{ 0.5f, 1.0f };
					fillRect.Pivot = Vec2{ 0.5f, 1.0f };
					fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
					fillRect.SizeDelta = Vec2{ trackWidth, trackHeight * t };
					break;
				}
			}

			// Optional percent label — drives the Progress Bar preset.
			// Writes the integer-rounded normalised value as "{N}%". Only
			// touches the text when it would actually change so we don't
			// dirty the renderer on every frame for static sliders.
			if (slider.LabelEntity != entt::null
				&& registry.valid(slider.LabelEntity)
				&& registry.all_of<TextRendererComponent>(slider.LabelEntity))
			{
				auto& tc = registry.get<TextRendererComponent>(slider.LabelEntity);
				const int percent = static_cast<int>(std::round(t * 100.0f));
				std::string next = std::to_string(percent) + "%";
				if (tc.Text != next) {
					tc.Text = std::move(next);
				}
			}
		}

		// Same auto-resolve pass UIEventSystem::Update runs at step 0.
		// OnPreRender needs it independently because in edit mode Update
		// never ran this frame, so HandleEntity / FillEntity may still be
		// null (e.g. right after scene load before any tick).
		void ResolveSliderChildrenForPreview(entt::registry& registry)
		{
			const auto childHasImage = [&registry](EntityHandle e) {
				return registry.all_of<RectTransform2DComponent, ImageComponent>(e);
			};
			auto sliderResolveView = registry.view<SliderComponent>();
			for (auto&& [entity, slider] : sliderResolveView.each()) {
				if (slider.FillEntity == entt::null || !registry.valid(slider.FillEntity)) {
					slider.FillEntity = FindFirstChildByNameOrPredicate(
						registry, entity, "Fill", childHasImage, slider.HandleEntity);
				}
				if (slider.HandleEntity == entt::null || !registry.valid(slider.HandleEntity)) {
					slider.HandleEntity = FindFirstChildByNameOrPredicate(
						registry, entity, "Handle", childHasImage, slider.FillEntity);
				}
			}
		}

		// Sync a dropdown's optional LabelEntity to the currently-selected
		// option text. Mirror of the inline pass at the end of
		// UIEventSystem::Update so the OnPreRender preview stays in lock-
		// step with the play-mode behaviour.
		void ApplyDropdownVisuals(entt::registry& registry, const DropdownComponent& dd)
		{
			if (dd.LabelEntity == entt::null
				|| !registry.valid(dd.LabelEntity)
				|| !registry.all_of<TextRendererComponent>(dd.LabelEntity))
			{
				return;
			}
			auto& tc = registry.get<TextRendererComponent>(dd.LabelEntity);
			if (!dd.Options.empty()) {
				const int idx = std::clamp(dd.SelectedIndex, 0,
					static_cast<int>(dd.Options.size()) - 1);
				tc.Text = dd.Options[idx];
			}
			else {
				tc.Text.clear();
			}
		}

		// Cursor must travel this far (in UI-space pixels) on the scrollbar's
		// value axis before drag tracking begins. Same idea as the slider
		// threshold but tracked per-scrollbar.
		constexpr float k_ScrollbarDragThresholdPx = 4.0f;

		// True when the scrollbar's value axis runs along X. Vertical
		// scrollbars (BottomToTop / TopToBottom) walk along Y.
		bool IsScrollbarHorizontal(ScrollbarDirection dir) {
			return dir == ScrollbarDirection::LeftToRight
				|| dir == ScrollbarDirection::RightToLeft;
		}

		// True when the scrollbar's Value=0 lives on the visual end of
		// the axis (Right or Top). Used to flip the geometry math while
		// keeping the value contract the same.
		bool IsScrollbarReversed(ScrollbarDirection dir) {
			return dir == ScrollbarDirection::RightToLeft
				|| dir == ScrollbarDirection::TopToBottom;
		}

		// Round Value to the nearest of (NumberOfSteps - 1) equal divisions.
		// Pass-through when NumberOfSteps <= 1.
		float SnapScrollbarValue(float value, int numberOfSteps) {
			if (numberOfSteps <= 1) return std::clamp(value, 0.0f, 1.0f);
			const int divisions = numberOfSteps - 1;
			const float snapped = std::round(value * static_cast<float>(divisions)) / static_cast<float>(divisions);
			return std::clamp(snapped, 0.0f, 1.0f);
		}

		// Rewrite the scrollbar's HandleEntity rect so it covers
		// [t, t + Size] of the track on the active axis. Mirrors the
		// slider's ApplySliderVisuals contract but covers four directions.
		//
		// Children no longer inherit parent width/height (see
		// UILayoutSystem ResolveRect), so the handle is point-anchored at
		// the appropriate edge of the track and sized via SizeDelta.
		//
		// Auto-orients the scrollbar's own SizeDelta to match Direction:
		// a vertical Direction on a wide rect (or horizontal on a tall
		// rect) swaps width/height so the track image renders along the
		// scrollbar's value axis. Idempotent — once the rect's longer
		// side matches the value axis the swap is a no-op.
		void ApplyScrollbarVisuals(entt::registry& registry,
			ScrollbarComponent& sb, RectTransform2DComponent& rect)
		{
			sb.Value = std::clamp(sb.Value, 0.0f, 1.0f);
			sb.Size  = std::clamp(sb.Size, 0.0f, 1.0f);

			const bool horizontal = IsScrollbarHorizontal(sb.Direction);
			const bool reversed   = IsScrollbarReversed(sb.Direction);

			// Auto-rotate the track's SizeDelta to match the active axis.
			// A user who flipped Direction to BottomToTop on a horizontally
			// authored scrollbar gets the rect rotated automatically so the
			// track is tall instead of wide.
			if (horizontal && rect.SizeDelta.y > rect.SizeDelta.x) {
				std::swap(rect.SizeDelta.x, rect.SizeDelta.y);
			}
			else if (!horizontal && rect.SizeDelta.x > rect.SizeDelta.y) {
				std::swap(rect.SizeDelta.x, rect.SizeDelta.y);
			}

			if (sb.HandleEntity == entt::null
				|| !registry.valid(sb.HandleEntity)
				|| !registry.all_of<RectTransform2DComponent>(sb.HandleEntity))
			{
				return;
			}
			auto& handle = registry.get<RectTransform2DComponent>(sb.HandleEntity);

			// Track dimensions in the scrollbar's own authored frame
			// (LocalScale=1 typical case). Using SizeDelta directly keeps
			// the layout stable even before the first ResolveRect tick.
			const float trackWidth  = rect.SizeDelta.x;
			const float trackHeight = rect.SizeDelta.y;

			const float visualValue = reversed ? (1.0f - sb.Value) : sb.Value;
			const float spanStart = visualValue * (1.0f - sb.Size);

			if (horizontal) {
				// Anchor handle at the track's left-centre, pivot the
				// handle at its own left-centre. With both at the same
				// edge, AnchoredPosition is a pure offset along the
				// track from its left edge — no centering term.
				handle.AnchorMin = Vec2{ 0.0f, 0.5f };
				handle.AnchorMax = Vec2{ 0.0f, 0.5f };
				handle.Pivot = Vec2{ 0.0f, 0.5f };
				handle.AnchoredPosition = Vec2{ trackWidth * spanStart, 0.0f };
				handle.SizeDelta = Vec2{ trackWidth * sb.Size, trackHeight };
			}
			else {
				// Vertical: anchor at track's bottom-centre, pivot at
				// the handle's bottom-centre, AnchoredPosition.y moves
				// the handle up. visualValue handles the BottomToTop /
				// TopToBottom flip.
				handle.AnchorMin = Vec2{ 0.5f, 0.0f };
				handle.AnchorMax = Vec2{ 0.5f, 0.0f };
				handle.Pivot = Vec2{ 0.5f, 0.0f };
				handle.AnchoredPosition = Vec2{ 0.0f, trackHeight * spanStart };
				handle.SizeDelta = Vec2{ trackWidth, trackHeight * sb.Size };
			}
		}

		// Sync an input field's child TextEntity to its current Text /
		// PlaceholderText. Edit-mode mirror of the per-frame block in
		// UIEventSystem::Update — minus the focus / caret bits, which are
		// per-frame state the editor preview doesn't try to fake.
		// Honors IsSecret so masked fields preview correctly in the
		// editor too.
		void ApplyInputFieldVisuals(entt::registry& registry, const InputFieldComponent& field)
		{
			if (field.TextEntity == entt::null
				|| !registry.valid(field.TextEntity)
				|| !registry.all_of<TextRendererComponent>(field.TextEntity))
			{
				return;
			}
			auto& tc = registry.get<TextRendererComponent>(field.TextEntity);
			const bool useText = !field.Text.empty();
			if (useText && field.IsSecret) {
				tc.Text = MaskTextForSecret(field.Text);
			}
			else {
				tc.Text = useText ? field.Text : field.PlaceholderText;
			}
			tc.Color = useText ? field.TextColor : field.PlaceholderColor;
		}

	} // namespace

	void UIEventSystem::Update(Scene& scene) {
		Application* app = Application::GetInstance();
		if (!app) return;
		Input& input = app->GetInput();

		// Prefer the editor-published UI panel region when active so
		// hit-tests resolve in the same coordinate space the panel was
		// rendered in (panel-relative pixels). For standalone runtime
		// builds the region stays unset and we fall back to the OS
		// window viewport, which is also where mouse coords originate.
		const Window::UIRegion uiRegion = Window::GetUIRegion();
		Vec2 mouseRaw = input.GetMousePosition();
		int vpW = 0;
		int vpH = 0;
		if (uiRegion.IsActive()) {
			mouseRaw.x -= static_cast<float>(uiRegion.OffsetX);
			mouseRaw.y -= static_cast<float>(uiRegion.OffsetY);
			vpW = uiRegion.Width;
			vpH = uiRegion.Height;
		}
		else {
			Viewport* viewport = Window::GetMainViewport();
			if (!viewport || viewport->GetWidth() <= 0 || viewport->GetHeight() <= 0) return;
			vpW = viewport->GetWidth();
			vpH = viewport->GetHeight();
		}

		const Vec2 mouseUi = ScreenPixelToUiSpace(mouseRaw, vpW, vpH);
		const bool mouseDownThisFrame = input.GetMouseDown(MouseButton::Left);
		const bool mouseUpThisFrame   = input.GetMouseUp(MouseButton::Left);
		const bool mouseHeld          = input.GetMouse(MouseButton::Left);

		auto& registry = scene.GetRegistry();

		// ── 0. Auto-resolve cross-entity references each frame ──────
		// Cross-entity refs aren't serialized — they're resolved by
		// finding the first child of the right shape. This survives
		// scene reload (refs default to entt::null after deserialize)
		// and editor flows that copy entities (refs become invalid;
		// re-resolve next frame). Explicit user-set refs survive too:
		// we only re-resolve when the field is null or refers to an
		// entity that no longer exists.

		const auto childHasImage = [&registry](EntityHandle e) {
			return registry.all_of<RectTransform2DComponent, ImageComponent>(e);
		};
		const auto childHasText = [&registry](EntityHandle e) {
			return registry.all_of<RectTransform2DComponent, TextRendererComponent>(e);
		};

		// Sliders: prefer name-matched children ("Fill", "Handle") so
		// the slider keeps working after scene reload even when both
		// cross-entity refs have to be re-resolved from scratch. Resolve
		// fill first, then handle excluding fill — guarantees the two
		// refs end up on different entities even with two image children.
		auto sliderResolveView = registry.view<SliderComponent>();
		for (auto&& [entity, slider] : sliderResolveView.each()) {
			if (slider.FillEntity == entt::null || !registry.valid(slider.FillEntity)) {
				slider.FillEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Fill", childHasImage, slider.HandleEntity);
			}
			if (slider.HandleEntity == entt::null || !registry.valid(slider.HandleEntity)) {
				slider.HandleEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Handle", childHasImage, slider.FillEntity);
			}
			if (slider.BackgroundEntity == entt::null || !registry.valid(slider.BackgroundEntity)) {
				// Skip the fill / handle children when probing — same
				// guard the other two slots use so we don't accidentally
				// triple-claim a single image child.
				slider.BackgroundEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Background", childHasImage,
					slider.FillEntity, slider.HandleEntity);
			}
		}

		// Scrollbars: HandleEntity (image, named "Handle" by default —
		// resolves the same way the slider's Handle does).
		auto scrollbarResolveView = registry.view<ScrollbarComponent>();
		for (auto&& [entity, sb] : scrollbarResolveView.each()) {
			if (sb.HandleEntity == entt::null || !registry.valid(sb.HandleEntity)) {
				sb.HandleEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Handle", childHasImage);
			}
		}

		// Scroll Rects: Content (named "Content"), optional Viewport
		// (named "Viewport"), optional scrollbars by name.
		auto scrollRectResolveView = registry.view<ScrollRectComponent>();
		for (auto&& [entity, sr] : scrollRectResolveView.each()) {
			if (sr.Content == entt::null || !registry.valid(sr.Content)) {
				sr.Content = FindFirstChildByNameOrPredicate(
					registry, entity, "Content",
					[&](EntityHandle e) { return registry.all_of<RectTransform2DComponent>(e); });
			}
			if (sr.Viewport == entt::null || !registry.valid(sr.Viewport)) {
				sr.Viewport = FindFirstChildByNameOrPredicate(
					registry, entity, "Viewport",
					[&](EntityHandle e) { return registry.all_of<RectTransform2DComponent>(e); });
			}
			if (sr.HorizontalScrollbar == entt::null || !registry.valid(sr.HorizontalScrollbar)) {
				sr.HorizontalScrollbar = FindFirstChildByNameOrPredicate(
					registry, entity, "Scrollbar Horizontal",
					[&](EntityHandle e) { return registry.all_of<ScrollbarComponent>(e); });
			}
			if (sr.VerticalScrollbar == entt::null || !registry.valid(sr.VerticalScrollbar)) {
				sr.VerticalScrollbar = FindFirstChildByNameOrPredicate(
					registry, entity, "Scrollbar Vertical",
					[&](EntityHandle e) { return registry.all_of<ScrollbarComponent>(e); });
			}
		}

		// Toggles: CheckmarkEntity (image, named "Checkmark" by default).
		auto toggleResolveView = registry.view<ToggleComponent>();
		for (auto&& [entity, toggle] : toggleResolveView.each()) {
			if (toggle.CheckmarkEntity == entt::null || !registry.valid(toggle.CheckmarkEntity)) {
				toggle.CheckmarkEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Checkmark", childHasImage);
			}
		}

		// Input fields: TextEntity (text renderer, named "Text" by default).
		auto inputResolveView = registry.view<InputFieldComponent>();
		for (auto&& [entity, field] : inputResolveView.each()) {
			if (field.TextEntity == entt::null || !registry.valid(field.TextEntity)) {
				field.TextEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Text", childHasText);
			}
		}

		// Dropdowns: LabelEntity (text renderer, named "Label" by default).
		auto dropdownResolveView = registry.view<DropdownComponent>();
		for (auto&& [entity, dropdown] : dropdownResolveView.each()) {
			if (dropdown.LabelEntity == entt::null || !registry.valid(dropdown.LabelEntity)) {
				dropdown.LabelEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Label", childHasText);
			}
		}

		// ── 1. Resolve dropdown popup hits FIRST ─────────────────────
		// Open dropdowns extend a popup below their button. When the
		// cursor is inside any popup row, that hit consumes the click —
		// rects underneath shouldn't react. We scan dropdowns up-front,
		// remember which row (if any) is hovered and, on click, mutate
		// the dropdown selection / IsOpen state directly.
		struct DropdownHit {
			EntityHandle Entity = entt::null;
			int RowIndex = -1; // -1 = no popup hit
		};
		DropdownHit dropdownHit;

		auto dropdownView = registry.view<RectTransform2DComponent, DropdownComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, rect, dd] : dropdownView.each()) {
			dd.SelectionChangedThisFrame = false;
			if (!dd.SelectionObserved) {
				dd.LastObservedSelectedIndex = dd.SelectedIndex;
				dd.SelectionObserved = true;
			}
			if (!dd.IsOpen || dd.Options.empty()) continue;

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const float width = tr.x - bl.x;
			const float topOfPopup = bl.y;

			for (int i = 0; i < static_cast<int>(dd.Options.size()); ++i) {
				const float rowTop = topOfPopup - dd.OptionRowHeight * static_cast<float>(i);
				const float rowBottom = rowTop - dd.OptionRowHeight;
				if (mouseUi.x >= bl.x && mouseUi.x <= bl.x + width
					&& mouseUi.y >= rowBottom && mouseUi.y <= rowTop)
				{
					dropdownHit.Entity = entity;
					dropdownHit.RowIndex = i;
					// Don't break — last (front-most) dropdown wins. Today
					// only one dropdown is typically open at a time; if
					// multiple are, the iteration order acts as tiebreak.
				}
			}
		}

		// ── 2. Hit-test interactable rects (skip when popup consumed) ─
		// Front-most wins: an Interactable rect that paints ON TOP of
		// another Interactable consumes the click and blocks anything
		// behind it. Mirrors GuiRenderer's z-stack — same hierarchy walk,
		// same (SortingLayer, SortingOrder, DrawIndex) tiebreak — so a
		// panel layered over widgets actually shields them from input
		// instead of the registry-iteration-order entity winning.
		EntityHandle hovered = entt::null;
		const bool popupConsumes = dropdownHit.Entity != entt::null;

		auto hitView = registry.view<RectTransform2DComponent, InteractableComponent>(entt::exclude<DisabledTag>);
		if (!popupConsumes) {
			std::vector<std::pair<EntityHandle, int>> uiDrawOrder;
			uiDrawOrder.reserve(64);
			UIDrawOrder::Build(registry, uiDrawOrder);

			std::unordered_map<EntityHandle, int> drawIndexByEntity;
			drawIndexByEntity.reserve(uiDrawOrder.size());
			for (const auto& [entity, di] : uiDrawOrder) {
				drawIndexByEntity.emplace(entity, di);
			}

			// Effective sort key — same fields GuiRenderer reads. Image
			// wins over Text when both are present (image paints first
			// in the renderer too); a bare RectTransform without either
			// falls back to (0, 0, drawIndex) so hierarchy order alone
			// still orders it.
			struct SortKey {
				int Layer = 0;
				int Order = 0;
				int DrawIndex = 0;
			};
			auto keyFor = [&](EntityHandle entity, int drawIndex) {
				SortKey k;
				k.DrawIndex = drawIndex;
				if (auto* image = registry.try_get<ImageComponent>(entity)) {
					k.Layer = image->SortingLayer;
					k.Order = image->SortingOrder;
				} else if (auto* text = registry.try_get<TextRendererComponent>(entity)) {
					k.Layer = text->SortingLayer;
					k.Order = text->SortingOrder;
				}
				return k;
			};
			auto less = [](const SortKey& a, const SortKey& b) {
				if (a.Layer != b.Layer) return a.Layer < b.Layer;
				if (a.Order != b.Order) return a.Order < b.Order;
				return a.DrawIndex < b.DrawIndex;
			};

			SortKey bestKey;
			bool haveHit = false;
			for (auto&& [entity, rect, interact] : hitView.each()) {
				if (!interact.Interactable) continue;
				if (!rect.ContainsPoint(mouseUi)) continue;

				// CircularSlider's clickable area is the ring annulus,
				// not the bounding rect. Reject hits that fall in the
				// donut hole or outside the ring so the hover doesn't
				// land on an empty centre.
				if (const auto* cs = registry.try_get<CircularSliderComponent>(entity)) {
					const Vec2 size = rect.GetSize();
					const float outerR = std::min(size.x, size.y) * 0.5f;
					const float innerR = std::max(0.0f, outerR - cs->RingThickness);
					// Hit-test against the rect's geometric centre — the
					// renderer paints the disc there, so a slider authored
					// with non-(0.5, 0.5) pivot would otherwise hit-test
					// off-axis from the visible ring.
					const Vec2 c = rect.GetCenter();
					const float dx = mouseUi.x - c.x;
					const float dy = mouseUi.y - c.y;
					const float distSq = dx * dx + dy * dy;
					if (distSq < innerR * innerR || distSq > outerR * outerR) continue;
				}

				auto it = drawIndexByEntity.find(entity);
				if (it == drawIndexByEntity.end()) continue;

				SortKey key = keyFor(entity, it->second);
				if (!haveHit || less(bestKey, key)) {
					hovered = entity;
					bestKey = key;
					haveHit = true;
				}
			}
		}

		// ── 3. Per-entity interaction state machine ──────────────────
		for (auto&& [entity, rect, interact] : hitView.each()) {
			interact.IsHovered     = (entity == hovered);
			interact.IsMouseDown   = false;
			interact.IsMouseUp     = false;
			interact.IsClicked     = false;

			if (!interact.Interactable) {
				interact.IsPressed = false;
				continue;
			}

			if (interact.IsHovered && mouseDownThisFrame) {
				interact.IsMouseDown = true;
				interact.IsPressed = true;
				m_PressedEntity = entity;
			}

			if (!mouseHeld) {
				interact.IsPressed = false;
			}

			if (mouseUpThisFrame) {
				if (interact.IsHovered) {
					interact.IsMouseUp = true;
					// Revalidate m_PressedEntity before consulting it — a
					// mouse-down on entity X followed by X being destroyed
					// (or losing its InteractableComponent) before the matching
					// mouse-up would otherwise leave a stale handle whose
					// integer value can be reused by a freshly-created entity,
					// firing a phantom click on something the user never
					// pressed.
					if (m_PressedEntity == entity
						&& registry.valid(m_PressedEntity)
						&& registry.all_of<InteractableComponent>(m_PressedEntity)) {
						interact.IsClicked = true;
						// Inspector-bound OnClick handlers fire on the
						// rising edge — same frame the engine detects the
						// click. Dispatch lives here (and in the synthetic
						// keyboard / controller activate block below) so
						// every IsClicked rising edge fans out to the
						// component's bindings, not just mouse-driven ones.
						if (auto* btn = registry.try_get<ButtonComponent>(entity)) {
							if (!btn->OnClick.Bindings.empty()) {
								InspectorEvents::FireAll(scene, entity, btn->OnClick.Bindings);
							}
						}
					}
				}
			}
		}

		if (mouseUpThisFrame) {
			m_PressedEntity = entt::null;
		}

		// ── 3a. Cursor swap (UI hover variant) ──────────────────────
		// Window hosts two cursor slots — default + UI — and switches
		// between them per-frame based on whether the cursor sits over
		// an INTERACTABLE Axiom UI element. We scope to "actually
		// interactable" so a disabled button doesn't trigger the hover
		// cursor; the same flag drives hover/press visual state above.
		// Skipping the whole call when no UI cursor was loaded preserves
		// the OS-default look for projects that didn't author one.
		if (Window* win = Application::GetWindow()) {
			bool overInteractable = false;
			if (hovered != entt::null) {
				if (auto* h = registry.try_get<InteractableComponent>(hovered)) {
					overInteractable = h->Interactable;
				}
			}
			win->SetCursorOverUI(overInteractable);
		}

		// ── 3b. Synthesise a click for keyboard / controller activate ──
		// UIFocusSystem set ActivatedThisFrame on the focused entity if
		// the user pressed Enter / Space / Gamepad-A this frame. Stamp
		// the same edge flags a real mouse click would have produced so
		// every widget reaction below (Toggle flip, Dropdown open,
		// Button-via-dispatcher, InputField submit) just works without
		// each having to opt into a focus pathway. The flag is one-frame
		// transient — clear after consumption.
		for (auto&& [entity, rect, interact] : hitView.each()) {
			if (interact.ActivatedThisFrame) {
				if (interact.Interactable) {
					interact.IsClicked   = true;
					interact.IsMouseDown = true;
					interact.IsPressed   = true;
					// Mirror the mouse-driven dispatch above so a
					// keyboard / controller activation also fires the
					// inspector-bound OnClick handlers.
					if (auto* btn = registry.try_get<ButtonComponent>(entity)) {
						if (!btn->OnClick.Bindings.empty()) {
							InspectorEvents::FireAll(scene, entity, btn->OnClick.Bindings);
						}
					}
				}
				interact.ActivatedThisFrame = false;
			}
		}

		// ── 4. Dropdown popup actions ────────────────────────────────
		// Done before button/etc. so that closing a dropdown via outside
		// click doesn't also trigger a click on whatever's underneath.
		if (mouseDownThisFrame) {
			// Click outside any open dropdown closes it (without changing
			// selection). This matches OS-typical combo-box behaviour.
			if (!popupConsumes) {
				for (auto&& [entity, rect, dd] : dropdownView.each()) {
					if (dd.IsOpen && !rect.ContainsPoint(mouseUi)) {
						dd.IsOpen = false;
					}
				}
			}
		}
		if (mouseUpThisFrame && popupConsumes) {
			if (registry.valid(dropdownHit.Entity)
				&& registry.all_of<DropdownComponent>(dropdownHit.Entity))
			{
				auto& dd = registry.get<DropdownComponent>(dropdownHit.Entity);
				// Read-only dropdowns ignore selection clicks. We still
				// close the popup so it isn't stuck open after a stray
				// click — UX matches "you can look but can't pick".
				if (!dd.IsReadOnly
					&& dropdownHit.RowIndex >= 0
					&& dropdownHit.RowIndex < static_cast<int>(dd.Options.size()))
				{
					dd.SelectedIndex = dropdownHit.RowIndex;
				}
				dd.IsOpen = false;
			}
		}

		// Diff every dropdown's SelectedIndex against the last broadcast
		// so inspector edits and programmatic writes also fan out to
		// OnSelectedIndexChange. C# Dropdown setters / SetSelectedIndex
		// update LastObservedSelectedIndex on their immediate-fire path
		// (Dropdown_MarkSelectedIndexObserved) so we don't double-fire here.
		for (auto&& [entity, rect, dd] : dropdownView.each()) {
			if (dd.SelectedIndex != dd.LastObservedSelectedIndex) {
				dd.SelectionChangedThisFrame = true;
				dd.LastObservedSelectedIndex = dd.SelectedIndex;

				if (!dd.OnValueChanged.Bindings.empty()) {
					// Methods with `int` get the index; methods with
					// `string` get the option's text. Picking int as
					// the dynamic kind matches Unity's
					// Dropdown.OnValueChanged convention; string-typed
					// bindings still fire with their authored static
					// argument (the FireAllWithDynamicArg fallback path).
					InspectorEvents::DynamicArg dyn;
					dyn.Kind = InspectorEventArgKind::Int;
					dyn.Encoded = std::to_string(dd.SelectedIndex);
					InspectorEvents::FireAllWithDynamicArg(scene, entity,
						dd.OnValueChanged.Bindings, dyn);
				}
			}
		}

		// ── 5. Widget visual state (color tint OR sprite swap) ──────
		// Each widget's TransitionMode picks between the two paths:
		// ColorTint writes per-state Color, SpriteSwap rewrites the
		// ImageComponent's TextureAssetId / TextureHandle for the
		// resolved state. None opts out entirely so user code can
		// drive the visual.
		//
		// Buttons specifically can target either an Image or a
		// TextRenderer (or a referenced child via TargetGraphic). The
		// view iterates Button + Interactable only — we resolve the
		// graphic per-entity inside the loop so a button on a
		// text-only entity ("Submit", "Cancel" labels) isn't silently
		// skipped.
		// The Target Graphic can own its own InteractableComponent — when
		// the button itself has no Interactable, hover/press state must
		// be read from the graphic for the visual swap to react. Iterate
		// every Button (no Interactable requirement on the button entity)
		// and resolve the interactable from (a) the Button entity, then
		// (b) the TargetGraphic. Without the fall-through, a button
		// authored as "Button on a wrapper, Image+Interactable on a
		// child" stayed locked at NormalColor regardless of cursor state.
		auto buttonView = registry.view<ButtonComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, btn] : buttonView.each()) {
			const EntityHandle target = (btn.TargetGraphic != entt::null && registry.valid(btn.TargetGraphic))
				? btn.TargetGraphic : entity;
			InteractableComponent* interact = registry.try_get<InteractableComponent>(entity);
			if (!interact && target != entity && registry.valid(target)) {
				interact = registry.try_get<InteractableComponent>(target);
			}
			if (!interact) continue;
			ApplyWidgetVisualToEntity(registry, target, *interact, btn.TransitionMode,
				btn.NormalColor, btn.HoveredColor, btn.PressedColor, btn.DisabledColor, btn.FocusedColor,
				btn.NormalSprite, btn.HoveredSprite, btn.PressedSprite, btn.DisabledSprite, btn.FocusedSprite);
		}
		auto toggleTintView = registry.view<InteractableComponent, ToggleComponent, ImageComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, interact, toggle, image] : toggleTintView.each()) {
			ApplyWidgetVisualState(image, interact, toggle.TransitionMode,
				toggle.NormalColor, toggle.HoveredColor, toggle.PressedColor, toggle.DisabledColor, toggle.FocusedColor,
				toggle.NormalSprite, toggle.HoveredSprite, toggle.PressedSprite, toggle.DisabledSprite, toggle.FocusedSprite);
		}
		auto inputTintView = registry.view<InteractableComponent, InputFieldComponent, ImageComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, interact, field, image] : inputTintView.each()) {
			ApplyWidgetVisualState(image, interact, field.TransitionMode,
				field.NormalColor, field.HoveredColor, field.PressedColor, field.DisabledColor, field.FocusedColor,
				field.NormalSprite, field.HoveredSprite, field.PressedSprite, field.DisabledSprite, field.FocusedSprite);
		}
		auto dropdownTintView = registry.view<InteractableComponent, DropdownComponent, ImageComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, interact, dd, image] : dropdownTintView.each()) {
			ApplyWidgetVisualState(image, interact, dd.TransitionMode,
				dd.NormalColor, dd.HoveredColor, dd.PressedColor, dd.DisabledColor, dd.FocusedColor,
				dd.NormalSprite, dd.HoveredSprite, dd.PressedSprite, dd.DisabledSprite, dd.FocusedSprite);
		}

		// ── 5c. Slider handle tint ──────────────────────────────────
		// The slider's draggable surface lives on HandleEntity by default
		// (preset puts the InteractableComponent there too). Mirror the
		// resolution rule used below for drag tracking: tint the handle
		// when it has both an Image + Interactable, otherwise fall back
		// to the slider parent's image tinted from the parent's own
		// Interactable. This keeps older "track-only" sliders working.
		auto sliderTintView = registry.view<SliderComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, slider] : sliderTintView.each()) {
			ImageComponent* targetImage = nullptr;
			InteractableComponent* targetInteract = nullptr;
			if (slider.HandleEntity != entt::null
				&& registry.valid(slider.HandleEntity))
			{
				targetImage = registry.try_get<ImageComponent>(slider.HandleEntity);
				targetInteract = registry.try_get<InteractableComponent>(slider.HandleEntity);
			}
			if (!targetImage || !targetInteract) {
				targetImage = registry.try_get<ImageComponent>(entity);
				targetInteract = registry.try_get<InteractableComponent>(entity);
			}
			if (targetImage && targetInteract) {
				ApplyWidgetVisualState(*targetImage, *targetInteract, slider.TransitionMode,
					slider.NormalColor, slider.HoveredColor, slider.PressedColor, slider.DisabledColor, slider.FocusedColor,
					slider.NormalSprite, slider.HoveredSprite, slider.PressedSprite, slider.DisabledSprite, slider.FocusedSprite);
			}
		}

		// ── 6. Sliders ───────────────────────────────────────────────
		// The default preset puts the InteractableComponent on the handle
		// child, so the draggable surface is the thumb. We prefer the
		// handle's interactable when present and fall back to one on the
		// slider parent so older scenes (or hand-authored sliders without
		// a handle child) keep working.
		auto sliderView = registry.view<SliderComponent, RectTransform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, slider, rect] : sliderView.each()) {
			slider.ValueChangedThisFrame = false;
			// Sync the diff baseline on first observation so a scene-load
			// deserialised Value doesn't fire a spurious event against the
			// default-init LastObservedValue.
			if (!slider.ValueObserved) {
				slider.LastObservedValue = slider.Value;
				slider.ValueObserved = true;
			}

			InteractableComponent* dragInteract = nullptr;
			if (slider.HandleEntity != entt::null
				&& registry.valid(slider.HandleEntity))
			{
				dragInteract = registry.try_get<InteractableComponent>(slider.HandleEntity);
			}
			if (!dragInteract) {
				dragInteract = registry.try_get<InteractableComponent>(entity);
			}

			// Read-only sliders skip the entire drag block — Value
			// stays put even while the user clicks-and-holds on the
			// handle. Hover / press flags still update on the
			// Interactable so visual state remains responsive.
			if (slider.IsReadOnly) {
				slider.IsDragging = false;
			}
			else if (dragInteract && dragInteract->Interactable && dragInteract->IsPressed) {
				const bool sliderHorizontal = IsSliderHorizontal(slider.Direction);
				const bool sliderReversed   = IsSliderReversed(slider.Direction);
				const float mouseAxis = sliderHorizontal ? mouseUi.x : mouseUi.y;

				// Down-edge of the press: snapshot the cursor and value so
				// drag tracking can run relative to where the user grabbed
				// the handle, not the absolute cursor coord. Reset
				// IsDragging so the threshold has to be re-cleared each press.
				if (dragInteract->IsMouseDown) {
					slider.PressMouseAxis = mouseAxis;
					slider.PressValue = slider.Value;
					slider.IsDragging = false;
				}

				if (!slider.IsDragging
					&& std::abs(mouseAxis - slider.PressMouseAxis) >= k_SliderDragThresholdPx)
				{
					slider.IsDragging = true;
				}

				if (slider.IsDragging) {
					const Vec2 bl = rect.GetBottomLeft();
					const Vec2 tr = rect.GetTopRight();
					const float trackLen = sliderHorizontal ? (tr.x - bl.x) : (tr.y - bl.y);
					if (trackLen > 0.0f) {
						const float delta = mouseAxis - slider.PressMouseAxis;
						const float range = slider.MaxValue - slider.MinValue;
						const float signedDelta = sliderReversed ? -delta : delta;
						float newValue = slider.PressValue + (signedDelta / trackLen) * range;
						const float lo = std::min(slider.MinValue, slider.MaxValue);
						const float hi = std::max(slider.MinValue, slider.MaxValue);
						newValue = std::clamp(newValue, lo, hi);
						if (slider.WholeNumbers) {
							newValue = std::round(newValue);
						}
						slider.Value = newValue;
					}
				}
			}
			else {
				slider.IsDragging = false;
			}

			// Visual sync (clamp + handle/fill geometry) is shared with
			// OnPreRender's edit-mode preview path so a single helper owns
			// the slider's visual contract.
			ApplySliderVisuals(registry, slider, rect);

			// Diff against the last broadcast value so every source of
			// change — drag (above), inspector edit, programmatic Value
			// = X — fans out to OnValueChanged. C# SetValue updates
			// LastObservedValue itself (Slider_MarkValueObserved) when
			// it raises the event immediately, so this branch doesn't
			// double-fire.
			if (slider.Value != slider.LastObservedValue) {
				slider.ValueChangedThisFrame = true;
				slider.LastObservedValue = slider.Value;

				// Fan the inspector-bound list out on the same edge.
				// Methods with a `float` parameter receive the new
				// value as the dynamic argument; bindings of any
				// other type keep their authored static value.
				if (!slider.OnValueChanged.Bindings.empty()) {
					InspectorEvents::DynamicArg dyn;
					dyn.Kind = InspectorEventArgKind::Float;
					char buf[32];
					std::snprintf(buf, sizeof(buf), "%g", slider.Value);
					dyn.Encoded = buf;
					InspectorEvents::FireAllWithDynamicArg(scene, entity,
						slider.OnValueChanged.Bindings, dyn);
				}
			}
		}

		// ── 6a. Circular sliders ─────────────────────────────────────
		// Polar drag: cursor angle around the ring centre maps to a
		// position in [0, SweepDegrees]. We snapshot (PressMouseAngle,
		// PressValue) on mouse-down and apply the angular delta to
		// PressValue rather than slamming Value to whatever the cursor's
		// absolute angle is — that mirrors the linear slider's "drag
		// follows the cursor's delta from the press point" feel and
		// avoids the handle teleporting on initial click.
		//
		// Hit-test (the annulus check above) gates IsHovered to actual
		// ring contacts, so the parent's InteractableComponent flags
		// already reflect ring-only interaction. We don't auto-resolve
		// HandleEntity / FillEntity by name like the linear slider does
		// because the ring is procedural — the only optional child here
		// is HandleEntity, which the user wires explicitly.
		{
			constexpr float k_CircularSliderDragThresholdRad = 0.005f; // ~0.3°
			constexpr float k_Pi = 3.14159265358979323846f;
			constexpr float k_Deg2Rad = k_Pi / 180.0f;
			auto circularView = registry.view<CircularSliderComponent, RectTransform2DComponent, InteractableComponent>(entt::exclude<DisabledTag>);
			for (auto&& [entity, cs, rect, interact] : circularView.each()) {
				cs.ValueChangedThisFrame = false;
				if (!cs.ValueObserved) {
					cs.LastObservedValue = cs.Value;
					cs.ValueObserved = true;
				}

				// Drag can be initiated by clicking the handle OR the ring
				// — both produce a usable cursor angle. Without the handle
				// branch, a click that landed squarely on the handle never
				// reached the ring's annulus hit-test (the handle hovered
				// first, suppressing the ring) and the slider sat inert
				// while the user was clearly trying to drag the thumb.
				InteractableComponent* handleInteract = nullptr;
				if (cs.HandleEntity != entt::null && registry.valid(cs.HandleEntity)) {
					handleInteract = registry.try_get<InteractableComponent>(cs.HandleEntity);
				}
				const bool handlePressed = handleInteract && handleInteract->Interactable && handleInteract->IsPressed;
				const bool ringPressed   = interact.Interactable && interact.IsPressed;
				const bool isPressed     = handlePressed || ringPressed;
				const bool isMouseDown   = (handleInteract && handleInteract->IsMouseDown) || interact.IsMouseDown;

				// Centre the polar drag math on the rect's geometric centre
				// (where GuiRenderer paints the disc) rather than on
				// ResolvedPivot — sliders authored with non-(0.5, 0.5) pivot
				// would otherwise read cursor angles from the pivot offset
				// rather than from the visible ring centre.
				const Vec2 centre = rect.GetCenter();
				const float dx = mouseUi.x - centre.x;
				const float dy = mouseUi.y - centre.y;
				const float cursorAngle = std::atan2(dy, dx);

				if (cs.IsReadOnly) {
					cs.IsDragging = false;
				}
				else if (isPressed) {
					if (isMouseDown) {
						cs.PressMouseAngle = cursorAngle;
						cs.PressValue = cs.Value;
						cs.IsDragging = false;
					}

					// Wrap angular delta into [-π, π] so a press near 179°
					// followed by a small drag past 180° doesn't read as a
					// near-full-circle jump in the other direction.
					float delta = cursorAngle - cs.PressMouseAngle;
					while (delta >  k_Pi) delta -= 2.0f * k_Pi;
					while (delta < -k_Pi) delta += 2.0f * k_Pi;

					if (!cs.IsDragging && std::abs(delta) >= k_CircularSliderDragThresholdRad) {
						cs.IsDragging = true;
					}
					if (cs.IsDragging) {
						const float sweepRad = cs.SweepDegrees * k_Deg2Rad;
						if (sweepRad > 0.0f) {
							const float range = cs.MaxValue - cs.MinValue;
							const float signedDelta = cs.Clockwise ? -delta : delta;
							float newValue = cs.PressValue + (signedDelta / sweepRad) * range;
							const float lo = std::min(cs.MinValue, cs.MaxValue);
							const float hi = std::max(cs.MinValue, cs.MaxValue);
							newValue = std::clamp(newValue, lo, hi);
							if (cs.WholeNumbers) {
								newValue = std::round(newValue);
							}
							cs.Value = newValue;
						}
					}
				}
				else {
					cs.IsDragging = false;
				}

				// Visual handle position. Optional child whose RectTransform2D
				// AnchoredPosition gets rewritten each frame to sit on the
				// ring at the value angle. The handle's other RectTransform
				// fields (Pivot, AnchorMin/Max, SizeDelta) are left to the
				// user — typical setup is a 0.5/0.5 anchored point with a
				// small Size so the dot rides the ring centre.
				if (cs.HandleEntity != entt::null
					&& registry.valid(cs.HandleEntity)
					&& registry.all_of<RectTransform2DComponent>(cs.HandleEntity))
				{
					const float range = cs.MaxValue - cs.MinValue;
					const float t = (range != 0.0f)
						? std::clamp((cs.Value - cs.MinValue) / range, 0.0f, 1.0f)
						: 0.0f;
					const float startRad = cs.StartAngleDegrees * k_Deg2Rad;
					const float sweepRad = cs.SweepDegrees * k_Deg2Rad
						* (cs.Clockwise ? -1.0f : 1.0f);
					const float angle = startRad + sweepRad * t;
					const Vec2 size = rect.GetSize();
					const float outerRadius = std::min(size.x, size.y) * 0.5f;
					const float thickness   = std::min(cs.RingThickness, outerRadius);
					const float meanRadius  = outerRadius - thickness * 0.5f;
					auto& hr = registry.get<RectTransform2DComponent>(cs.HandleEntity);
					hr.AnchoredPosition = Vec2{
						std::cos(angle) * meanRadius,
						std::sin(angle) * meanRadius
					};
				}

				if (cs.Value != cs.LastObservedValue) {
					cs.ValueChangedThisFrame = true;
					cs.LastObservedValue = cs.Value;
				}

				// Handle visual feedback. Mirror the linear slider's
				// tinting pattern: prefer the handle's own InteractableComponent
				// (so hover/press is computed from cursor-on-handle, not
				// cursor-on-ring), fall back to the parent's interactable
				// when the handle has none. This is what gives a circular
				// slider's thumb the same hover/press color swap as a
				// linear slider's thumb — without it, the handle's
				// ImageComponent.Color stayed locked to whatever the
				// authored value was.
				if (cs.HandleEntity != entt::null
					&& registry.valid(cs.HandleEntity))
				{
					ImageComponent* handleImage = registry.try_get<ImageComponent>(cs.HandleEntity);
					InteractableComponent* handleInteract = registry.try_get<InteractableComponent>(cs.HandleEntity);
					if (handleImage) {
						InteractableComponent& effectiveInteract = handleInteract ? *handleInteract : interact;
						ApplyWidgetVisualState(*handleImage, effectiveInteract, cs.TransitionMode,
							cs.NormalColor, cs.HoveredColor, cs.PressedColor, cs.DisabledColor, cs.FocusedColor,
							cs.NormalSprite, cs.HoveredSprite, cs.PressedSprite, cs.DisabledSprite, cs.FocusedSprite);
					}
				}
			}
		}

		// ── 6b. Scrollbars ───────────────────────────────────────────
		// Drag the handle to set Value; click empty track to "page" the
		// handle by one Size in the direction of the click. The drag
		// surface is the handle's InteractableComponent (preset puts one
		// there) — we fall back to the parent's Interactable so older
		// scenes still work, mirroring the slider's resolution rule.
		auto scrollbarView = registry.view<ScrollbarComponent, RectTransform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, sb, rect] : scrollbarView.each()) {
			sb.ValueChangedThisFrame = false;
			if (!sb.ValueObserved) {
				sb.LastObservedValue = sb.Value;
				sb.ValueObserved = true;
			}

			InteractableComponent* handleInteract = nullptr;
			if (sb.HandleEntity != entt::null && registry.valid(sb.HandleEntity)) {
				handleInteract = registry.try_get<InteractableComponent>(sb.HandleEntity);
			}
			InteractableComponent* trackInteract = registry.try_get<InteractableComponent>(entity);

			const bool horizontal = IsScrollbarHorizontal(sb.Direction);
			const bool reversed   = IsScrollbarReversed(sb.Direction);
			const float mouseAxis = horizontal ? mouseUi.x : mouseUi.y;
			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const float trackLen = horizontal ? (tr.x - bl.x) : (tr.y - bl.y);

			if (sb.IsReadOnly) {
				sb.IsDragging = false;
			}
			else if (handleInteract && handleInteract->Interactable && handleInteract->IsPressed) {
				if (handleInteract->IsMouseDown) {
					sb.PressMouseAxis = mouseAxis;
					sb.PressValue = sb.Value;
					sb.IsDragging = false;
				}
				if (!sb.IsDragging
					&& std::abs(mouseAxis - sb.PressMouseAxis) >= k_ScrollbarDragThresholdPx)
				{
					sb.IsDragging = true;
				}
				if (sb.IsDragging && trackLen > 0.0f) {
					const float effectiveLen = trackLen * std::max(0.0001f, 1.0f - sb.Size);
					const float delta = mouseAxis - sb.PressMouseAxis;
					float newValue = sb.PressValue + (reversed ? -delta : delta) / effectiveLen;
					if (!horizontal) newValue = sb.PressValue + (reversed ? -delta : delta) / effectiveLen;
					newValue = std::clamp(newValue, 0.0f, 1.0f);
					sb.Value = SnapScrollbarValue(newValue, sb.NumberOfSteps);
				}
			}
			else {
				sb.IsDragging = false;
				// Page-click: pressing on the track (not the handle)
				// jumps Value by one Size in the direction of the click.
				if (trackInteract && trackInteract->IsMouseDown && trackInteract->Interactable
					&& (!handleInteract || !handleInteract->IsHovered))
				{
					if (trackLen > 0.0f) {
						float t = horizontal
							? (mouseAxis - bl.x) / trackLen
							: (mouseAxis - bl.y) / trackLen;
						t = std::clamp(t, 0.0f, 1.0f);
						if (reversed) t = 1.0f - t;
						const float page = std::max(0.05f, sb.Size);
						float newValue = (t > sb.Value) ? sb.Value + page : sb.Value - page;
						newValue = std::clamp(newValue, 0.0f, 1.0f);
						sb.Value = SnapScrollbarValue(newValue, sb.NumberOfSteps);
					}
				}
			}

			ApplyScrollbarVisuals(registry, sb, rect);

			if (sb.Value != sb.LastObservedValue) {
				sb.ValueChangedThisFrame = true;
				sb.LastObservedValue = sb.Value;
			}
		}

		// ── 6c. Scrollbar handle tint ────────────────────────────────
		auto scrollbarTintView = registry.view<ScrollbarComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, sb] : scrollbarTintView.each()) {
			ImageComponent* targetImage = nullptr;
			InteractableComponent* targetInteract = nullptr;
			if (sb.HandleEntity != entt::null && registry.valid(sb.HandleEntity)) {
				targetImage = registry.try_get<ImageComponent>(sb.HandleEntity);
				targetInteract = registry.try_get<InteractableComponent>(sb.HandleEntity);
			}
			if (!targetImage || !targetInteract) {
				targetImage = registry.try_get<ImageComponent>(entity);
				targetInteract = registry.try_get<InteractableComponent>(entity);
			}
			if (targetImage && targetInteract) {
				ApplyWidgetVisualState(*targetImage, *targetInteract, sb.TransitionMode,
					sb.NormalColor, sb.HoveredColor, sb.PressedColor, sb.DisabledColor, sb.FocusedColor,
					sb.NormalSprite, sb.HoveredSprite, sb.PressedSprite, sb.DisabledSprite, sb.FocusedSprite);
			}
		}

		// ── 6d. Scroll Rects ─────────────────────────────────────────
		// Drag inside the viewport scrolls the content. Mouse wheel above
		// the viewport scrolls vertically (and horizontally with Shift,
		// matching the rest of the engine's wheel convention). Inertia
		// keeps the content drifting after release; Elastic mode rubber-
		// bands content past edges back into bounds.
		auto scrollRectView = registry.view<ScrollRectComponent, RectTransform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, sr, viewportRect] : scrollRectView.each()) {
			sr.ValueChangedThisFrame = false;
			if (!sr.ValueObserved) {
				sr.LastObservedNormalizedPosition = sr.NormalizedPosition;
				sr.ValueObserved = true;
			}

			if (sr.Content == entt::null || !registry.valid(sr.Content)
				|| !registry.all_of<RectTransform2DComponent>(sr.Content))
			{
				continue;
			}
			auto& contentRect = registry.get<RectTransform2DComponent>(sr.Content);

			// Resolve viewport rect: explicit Viewport entity wins, else
			// the ScrollRect entity itself acts as the viewport.
			const RectTransform2DComponent* effectiveViewport = &viewportRect;
			if (sr.Viewport != entt::null && registry.valid(sr.Viewport)
				&& registry.all_of<RectTransform2DComponent>(sr.Viewport))
			{
				effectiveViewport = &registry.get<RectTransform2DComponent>(sr.Viewport);
			}

			const Vec2 vbl = effectiveViewport->GetBottomLeft();
			const Vec2 vtr = effectiveViewport->GetTopRight();
			const float viewportW = vtr.x - vbl.x;
			const float viewportH = vtr.y - vbl.y;
			const float contentW = contentRect.SizeDelta.x;
			const float contentH = contentRect.SizeDelta.y;
			const float maxScrollX = std::max(0.0f, contentW - viewportW);
			const float maxScrollY = std::max(0.0f, contentH - viewportH);

			InteractableComponent* viewInteract = registry.try_get<InteractableComponent>(entity);
			const bool hovered = viewInteract && viewInteract->IsHovered;

			// ── Drag inside viewport ────────────────────────────────
			if (viewInteract && viewInteract->Interactable) {
				if (viewInteract->IsMouseDown) {
					sr.IsDragging = true;
					sr.PressMouseUi = mouseUi;
					sr.PressContentPosition = contentRect.AnchoredPosition;
					sr.Velocity = Vec2{ 0.0f, 0.0f };
				}
			}
			if (sr.IsDragging) {
				if (mouseHeld) {
					const Vec2 delta{ mouseUi.x - sr.PressMouseUi.x,
					                  mouseUi.y - sr.PressMouseUi.y };
					Vec2 next = sr.PressContentPosition;
					if (sr.Horizontal) next.x = sr.PressContentPosition.x + delta.x;
					if (sr.Vertical)   next.y = sr.PressContentPosition.y + delta.y;
					contentRect.AnchoredPosition = next;
				}
				else {
					sr.IsDragging = false;
				}
			}

			// ── Mouse wheel ─────────────────────────────────────────
			if (hovered) {
				const float wheel = input.ScrollValue();
				if (wheel != 0.0f) {
					// ScrollSensitivity is exposed in inspector-friendly
					// whole numbers (default 5). Divide by 100 here so a
					// user-typed "5" produces the engine's calibrated
					// default speed; "10" doubles it, "1" gives a slow
					// crawl. Without this scale, raw inspector values
					// produced runaway-fast scrolling for any reasonable
					// integer.
					const float pixels = wheel * 60.0f * (sr.ScrollSensitivity * 0.01f);
					if (sr.Vertical && maxScrollY > 0.0f) {
						contentRect.AnchoredPosition.y -= pixels;
						sr.Velocity.y = -pixels / std::max(0.0001f, app->GetTime().GetUnscaledDeltaTime());
					}
					else if (sr.Horizontal && maxScrollX > 0.0f) {
						contentRect.AnchoredPosition.x += pixels;
						sr.Velocity.x = pixels / std::max(0.0001f, app->GetTime().GetUnscaledDeltaTime());
					}
				}
			}

			// Track velocity from drag for inertia + elastic rebound.
			const float dtScroll = app->GetTime().GetUnscaledDeltaTime();
			if (sr.IsDragging && dtScroll > 0.0f) {
				const Vec2 dragV{
					(contentRect.AnchoredPosition.x - sr.PreviousContentPosition.x) / dtScroll,
					(contentRect.AnchoredPosition.y - sr.PreviousContentPosition.y) / dtScroll
				};
				sr.Velocity = dragV;
			}
			sr.PreviousContentPosition = contentRect.AnchoredPosition;

			// ── Apply movement type clamping ────────────────────────
			// Compute current "anchored" reference: Content's
			// AnchoredPosition is in parent-local pixels; Unity's
			// convention is content.x in [-(contentW-viewportW), 0]
			// when scrolling horizontally with anchor pinned to top-
			// left. We treat AnchoredPosition.x ∈ [-maxScrollX, 0] as
			// the canonical "in bounds" range for horizontal, and
			// AnchoredPosition.y ∈ [0, maxScrollY] for vertical.
			Vec2 pos = contentRect.AnchoredPosition;

			// Inertia drift — apply when not dragging.
			if (!sr.IsDragging && sr.Inertia) {
				const float decay = std::pow(sr.DecelerationRate, dtScroll);
				sr.Velocity.x *= decay;
				sr.Velocity.y *= decay;
				if (sr.Horizontal) pos.x += sr.Velocity.x * dtScroll;
				if (sr.Vertical)   pos.y += sr.Velocity.y * dtScroll;
				if (std::abs(sr.Velocity.x) < 1.0f) sr.Velocity.x = 0.0f;
				if (std::abs(sr.Velocity.y) < 1.0f) sr.Velocity.y = 0.0f;
			}

			auto clampAxis = [](float v, float lo, float hi, ScrollRectMovementType mode, float elasticity, float dt, float& vel) {
				if (mode == ScrollRectMovementType::Unrestricted) return v;
				if (mode == ScrollRectMovementType::Clamped) return std::clamp(v, lo, hi);
				// Elastic: rubber-band toward the nearest edge.
				if (v < lo) {
					const float overshoot = lo - v;
					const float pull = overshoot * (1.0f - std::pow(elasticity, dt));
					v += pull;
					vel *= 0.5f;
				}
				else if (v > hi) {
					const float overshoot = v - hi;
					const float pull = overshoot * (1.0f - std::pow(elasticity, dt));
					v -= pull;
					vel *= 0.5f;
				}
				return v;
			};
			if (sr.Horizontal && maxScrollX > 0.0f) {
				pos.x = clampAxis(pos.x, -maxScrollX, 0.0f, sr.MovementType, sr.Elasticity, dtScroll, sr.Velocity.x);
			}
			else {
				pos.x = 0.0f;
			}
			if (sr.Vertical && maxScrollY > 0.0f) {
				pos.y = clampAxis(pos.y, 0.0f, maxScrollY, sr.MovementType, sr.Elasticity, dtScroll, sr.Velocity.y);
			}
			else {
				pos.y = 0.0f;
			}
			contentRect.AnchoredPosition = pos;
			sr.PreviousContentPosition = pos;

			// ── Compute NormalizedPosition from current offset ──────
			Vec2 normalized = sr.NormalizedPosition;
			normalized.x = (maxScrollX > 0.0f) ? std::clamp(-pos.x / maxScrollX, 0.0f, 1.0f) : 0.0f;
			normalized.y = (maxScrollY > 0.0f) ? std::clamp(1.0f - (pos.y / maxScrollY), 0.0f, 1.0f) : 1.0f;
			sr.NormalizedPosition = normalized;

			// ── Mirror state to attached scrollbars ─────────────────
			auto syncScrollbar = [&](EntityHandle sbEntity, bool isHorizontal) {
				if (sbEntity == entt::null || !registry.valid(sbEntity)) return;
				auto* sb = registry.try_get<ScrollbarComponent>(sbEntity);
				if (!sb) return;
				const float visible = isHorizontal
					? ((contentW > 0.0f) ? std::clamp(viewportW / contentW, 0.05f, 1.0f) : 1.0f)
					: ((contentH > 0.0f) ? std::clamp(viewportH / contentH, 0.05f, 1.0f) : 1.0f);
				sb->Size = visible;
				const float target = isHorizontal ? sr.NormalizedPosition.x : (1.0f - sr.NormalizedPosition.y);
				// If the user is dragging the scrollbar, let it drive
				// the content instead of the other way around.
				if (sb->IsDragging) {
					if (isHorizontal && maxScrollX > 0.0f) {
						const float driven = std::clamp(sb->Value, 0.0f, 1.0f);
						contentRect.AnchoredPosition.x = -driven * maxScrollX;
					}
					else if (!isHorizontal && maxScrollY > 0.0f) {
						// Drag-driven write must be the inverse of the sync
						// direction (target = pos.y / maxScrollY, see above).
						// An extra `1 - driven` was inverting twice — content
						// went the wrong way and the next non-drag tick
						// recomputed Value from the wrong position, snapping
						// the handle back to the top.
						const float driven = std::clamp(sb->Value, 0.0f, 1.0f);
						contentRect.AnchoredPosition.y = driven * maxScrollY;
					}
				}
				else if (std::abs(sb->Value - target) > 0.0005f) {
					sb->Value = target;
					sb->LastObservedValue = target;
				}
			};
			syncScrollbar(sr.HorizontalScrollbar, true);
			syncScrollbar(sr.VerticalScrollbar, false);

			// ── Visibility rules for attached scrollbars ────────────
			auto applyVisibility = [&](EntityHandle sbEntity, ScrollbarVisibility vis, bool isHorizontal) {
				if (sbEntity == entt::null || !registry.valid(sbEntity)) return;
				const bool needed = isHorizontal ? (maxScrollX > 0.0f) : (maxScrollY > 0.0f);
				bool show = true;
				if (vis == ScrollbarVisibility::AutoHide
					|| vis == ScrollbarVisibility::AutoHideAndExpandViewport)
				{
					show = needed;
				}
				if (scene.IsValid(sbEntity)) {
					scene.GetEntity(sbEntity).SetEnabled(show);
				}
			};
			applyVisibility(sr.HorizontalScrollbar, sr.HorizontalScrollbarVisibility, true);
			applyVisibility(sr.VerticalScrollbar,   sr.VerticalScrollbarVisibility,   false);

			if (sr.NormalizedPosition.x != sr.LastObservedNormalizedPosition.x
				|| sr.NormalizedPosition.y != sr.LastObservedNormalizedPosition.y)
			{
				sr.ValueChangedThisFrame = true;
				sr.LastObservedNormalizedPosition = sr.NormalizedPosition;
			}
		}

		// ── 7. Toggles ───────────────────────────────────────────────
		auto toggleView = registry.view<InteractableComponent, ToggleComponent>(entt::exclude<DisabledTag>);
		std::vector<std::pair<EntityHandle, bool>> deferredCheckmarkEnable;
		for (auto&& [entity, interact, toggle] : toggleView.each()) {
			toggle.ValueChangedThisFrame = false;
			if (!toggle.ValueObserved) {
				toggle.LastObservedIsOn = toggle.IsOn;
				toggle.ValueObserved = true;
			}
			// Read-only toggles still receive hover / press visuals
			// (so the widget feels alive) but their IsOn never flips
			// from user input. Programmatic writes still work.
			if (interact.IsClicked && !toggle.IsReadOnly) {
				toggle.IsOn = !toggle.IsOn;
			}

			// Diff catches click-driven flips AND inspector / programmatic
			// IsOn writes. C# SetValue calls Toggle_MarkIsOnObserved when
			// it raises the event immediately so we don't double-fire.
			if (toggle.IsOn != toggle.LastObservedIsOn) {
				toggle.ValueChangedThisFrame = true;
				toggle.LastObservedIsOn = toggle.IsOn;

				if (!toggle.OnValueChanged.Bindings.empty()) {
					InspectorEvents::DynamicArg dyn;
					dyn.Kind = InspectorEventArgKind::Bool;
					dyn.Encoded = toggle.IsOn ? "1" : "0";
					InspectorEvents::FireAllWithDynamicArg(scene, entity,
						toggle.OnValueChanged.Bindings, dyn);
				}
			}

			if (toggle.CheckmarkEntity != entt::null && registry.valid(toggle.CheckmarkEntity)) {
				deferredCheckmarkEnable.emplace_back(toggle.CheckmarkEntity, toggle.IsOn);
			}
		}
		for (const auto& [checkmark, desiredEnabled] : deferredCheckmarkEnable) {
			SetEntityEnabled(scene, checkmark, desiredEnabled);
		}

		// ── 8. Input fields ──────────────────────────────────────────
		// Focus follows mouse-down (so click-to-place-caret feels live),
		// then keyboard shortcuts edit the focused field. Selection state
		// is byte-indexed into Text — typing or pasting replaces the
		// selection if any, otherwise inserts at the caret. Backspace and
		// Delete repeat-fire while held so the editor behaves like every
		// other text input on the OS.
		auto inputView = registry.view<InteractableComponent, InputFieldComponent>(entt::exclude<DisabledTag>);
		const float dt = app->GetTime().GetUnscaledDeltaTime();
		const bool ctrlDown = input.GetKey(KeyCode::LeftControl) || input.GetKey(KeyCode::RightControl);
		const bool shiftDown = input.GetKey(KeyCode::LeftShift) || input.GetKey(KeyCode::RightShift);

		// First pass: figure out which field (if any) was just clicked.
		EntityHandle clickedField = entt::null;
		for (auto&& [entity, interact, field] : inputView.each()) {
			field.SubmittedThisFrame = false;
			if (interact.IsMouseDown) {
				clickedField = entity;
			}
		}

		// Apply focus / start-of-drag for the clicked field, defocus the
		// rest. A click anywhere outside any field also defocuses all.
		if (clickedField != entt::null) {
			for (auto&& [entity, interact, field] : inputView.each()) {
				if (entity == clickedField) {
					field.IsFocused = true;
					ClampCaret(field);
					const int byte = ByteFromMouseX(registry, field, mouseUi);
					field.CaretBytePos = byte;
					if (!shiftDown) {
						field.SelectionAnchorBytePos = byte;
					}
					field.MouseSelecting = true;
				}
				else {
					field.IsFocused = false;
					field.MouseSelecting = false;
				}
			}
		}
		else if (mouseDownThisFrame) {
			for (auto&& [entity, interact, field] : inputView.each()) {
				field.IsFocused = false;
				field.MouseSelecting = false;
			}
		}

		// Drag-to-select: while the mouse is held and we started inside the
		// field's rect, every frame's cursor position updates the caret.
		// The anchor stays put, so the selection grows / shrinks naturally.
		for (auto&& [entity, interact, field] : inputView.each()) {
			if (field.MouseSelecting && mouseHeld) {
				const int byte = ByteFromMouseX(registry, field, mouseUi);
				field.CaretBytePos = byte;
			}
			if (!mouseHeld) {
				field.MouseSelecting = false;
			}
		}

		// Per-frame typed text + special keys. We drain the char buffer
		// once per frame and only apply it to the focused field; the same
		// goes for shortcut keys.
		const std::string typedText = input.GetTypedTextUtf8();
		const bool enterPressed = input.GetKeyDown(KeyCode::Enter)
			|| input.GetKeyDown(KeyCode::KpEnter);

		for (auto&& [entity, interact, field] : inputView.each()) {
			if (!field.IsFocused) {
				// Reset hold-to-repeat timers so refocusing the field
				// doesn't immediately fire a leftover repeat.
				field.BackspaceHoldTime = 0.0f;
				field.BackspaceRepeatAccumulator = 0.0f;
				field.DeleteHoldTime = 0.0f;
				field.DeleteRepeatAccumulator = 0.0f;
				field.LeftHoldTime = 0.0f;
				field.LeftRepeatAccumulator = 0.0f;
				field.RightHoldTime = 0.0f;
				field.RightRepeatAccumulator = 0.0f;
				continue;
			}

			ClampCaret(field);

			// ── Shortcuts ─────────────────────────────────────────
			if (ctrlDown) {
				if (input.GetKeyDown(KeyCode::A)) {
					field.SelectionAnchorBytePos = 0;
					field.CaretBytePos = static_cast<int>(field.Text.size());
				}
				if (input.GetKeyDown(KeyCode::C) || input.GetKeyDown(KeyCode::X)) {
					auto [lo, hi] = SelectionRange(field);
					if (lo != hi) {
						if (Window* w = Application::GetWindow()) {
							w->SetClipboardString(field.Text.substr(lo, hi - lo));
						}
						// Cut mutates Text — read-only fields stay
						// copy-only.
						if (input.GetKeyDown(KeyCode::X) && !field.IsReadOnly) {
							DeleteSelection(field);
						}
					}
				}
				if (input.GetKeyDown(KeyCode::V) && !field.IsReadOnly) {
					if (Window* w = Application::GetWindow()) {
						const std::string clip = w->GetClipboardString();
						if (!clip.empty()) {
							// Strip CR / LF — single-line input fields
							// shouldn't accept newlines from clipboard.
							std::string sanitized;
							sanitized.reserve(clip.size());
							for (char c : clip) {
								if (c != '\r' && c != '\n') sanitized.push_back(c);
							}
							// Reject codepoints that don't pass the
							// active ContentType filter. With selection
							// active the existing-text reference is the
							// post-deletion string so e.g. pasting "1.2"
							// over a selection containing "." in a
							// DecimalNumber field still accepts the new
							// dot.
							auto [lo, hi] = SelectionRange(field);
							std::string existingAfterDelete = field.Text;
							int caretAfterDelete = field.CaretBytePos;
							if (lo != hi) {
								existingAfterDelete.erase(lo, hi - lo);
								caretAfterDelete = lo;
							}
							const std::string filtered = FilterByContentType(
								field.ContentType, sanitized,
								existingAfterDelete, caretAfterDelete);
							if (!filtered.empty()) {
								InsertAtCaret(field, filtered);
							}
						}
					}
				}
			}

			// ── Caret navigation ──────────────────────────────────
			// Down-edge moves once and resets the hold timer; while the
			// arrow stays held we wait k_KeyHoldDelay then auto-repeat at
			// k_KeyHoldRate. Mirrors Backspace / Delete below — without
			// it, holding Left/Right only nudged the caret a single
			// codepoint per press regardless of duration.
			auto moveLeftOnce = [&]() {
				if (HasSelection(field) && !shiftDown) {
					const int lo = std::min(field.CaretBytePos, field.SelectionAnchorBytePos);
					field.CaretBytePos = lo;
					field.SelectionAnchorBytePos = lo;
				}
				else {
					field.CaretBytePos = Utf8PrevByte(field.Text, field.CaretBytePos);
					if (!shiftDown) field.SelectionAnchorBytePos = field.CaretBytePos;
				}
			};
			auto moveRightOnce = [&]() {
				if (HasSelection(field) && !shiftDown) {
					const int hi = std::max(field.CaretBytePos, field.SelectionAnchorBytePos);
					field.CaretBytePos = hi;
					field.SelectionAnchorBytePos = hi;
				}
				else {
					field.CaretBytePos = Utf8NextByte(field.Text, field.CaretBytePos);
					if (!shiftDown) field.SelectionAnchorBytePos = field.CaretBytePos;
				}
			};

			if (input.GetKeyDown(KeyCode::Left)) {
				moveLeftOnce();
				field.LeftHoldTime = 0.0f;
				field.LeftRepeatAccumulator = 0.0f;
			}
			else if (input.GetKey(KeyCode::Left)) {
				field.LeftHoldTime += dt;
				if (field.LeftHoldTime >= k_KeyHoldDelay) {
					field.LeftRepeatAccumulator += dt;
					while (field.LeftRepeatAccumulator >= k_KeyHoldRate) {
						field.LeftRepeatAccumulator -= k_KeyHoldRate;
						moveLeftOnce();
					}
				}
			}
			else {
				field.LeftHoldTime = 0.0f;
				field.LeftRepeatAccumulator = 0.0f;
			}

			if (input.GetKeyDown(KeyCode::Right)) {
				moveRightOnce();
				field.RightHoldTime = 0.0f;
				field.RightRepeatAccumulator = 0.0f;
			}
			else if (input.GetKey(KeyCode::Right)) {
				field.RightHoldTime += dt;
				if (field.RightHoldTime >= k_KeyHoldDelay) {
					field.RightRepeatAccumulator += dt;
					while (field.RightRepeatAccumulator >= k_KeyHoldRate) {
						field.RightRepeatAccumulator -= k_KeyHoldRate;
						moveRightOnce();
					}
				}
			}
			else {
				field.RightHoldTime = 0.0f;
				field.RightRepeatAccumulator = 0.0f;
			}
			if (input.GetKeyDown(KeyCode::Home)) {
				field.CaretBytePos = 0;
				if (!shiftDown) field.SelectionAnchorBytePos = 0;
			}
			if (input.GetKeyDown(KeyCode::End)) {
				field.CaretBytePos = static_cast<int>(field.Text.size());
				if (!shiftDown) field.SelectionAnchorBytePos = field.CaretBytePos;
			}

			// ── Backspace (one-shot + hold-to-repeat) ─────────────
			// Down-edge fires once and resets the hold timer; while the
			// key stays held we wait k_KeyHoldDelay then auto-fire at
			// k_KeyHoldRate. Releasing clears the timer so the next press
			// starts fresh. Read-only fields swallow the keystroke
			// entirely so the caret + selection state still feel live
			// without mutating Text.
			if (input.GetKeyDown(KeyCode::Backspace)) {
				if (!field.IsReadOnly) BackspaceOnce(field);
				field.BackspaceHoldTime = 0.0f;
				field.BackspaceRepeatAccumulator = 0.0f;
			}
			else if (input.GetKey(KeyCode::Backspace) && !field.IsReadOnly) {
				field.BackspaceHoldTime += dt;
				if (field.BackspaceHoldTime >= k_KeyHoldDelay) {
					field.BackspaceRepeatAccumulator += dt;
					while (field.BackspaceRepeatAccumulator >= k_KeyHoldRate) {
						field.BackspaceRepeatAccumulator -= k_KeyHoldRate;
						BackspaceOnce(field);
					}
				}
			}
			else {
				field.BackspaceHoldTime = 0.0f;
				field.BackspaceRepeatAccumulator = 0.0f;
			}

			// ── Delete (mirror of Backspace) ──────────────────────
			if (input.GetKeyDown(KeyCode::Delete)) {
				if (!field.IsReadOnly) DeleteOnce(field);
				field.DeleteHoldTime = 0.0f;
				field.DeleteRepeatAccumulator = 0.0f;
			}
			else if (input.GetKey(KeyCode::Delete) && !field.IsReadOnly) {
				field.DeleteHoldTime += dt;
				if (field.DeleteHoldTime >= k_KeyHoldDelay) {
					field.DeleteRepeatAccumulator += dt;
					while (field.DeleteRepeatAccumulator >= k_KeyHoldRate) {
						field.DeleteRepeatAccumulator -= k_KeyHoldRate;
						DeleteOnce(field);
					}
				}
			}
			else {
				field.DeleteHoldTime = 0.0f;
				field.DeleteRepeatAccumulator = 0.0f;
			}

			// ── Typed characters ──────────────────────────────────
			// GLFW's char callback already fires for held keys at the
			// OS repeat rate, so typed text just appends at the caret.
			// Skip while Ctrl is held so chord shortcuts (Ctrl+C/V/X/A)
			// don't double up by also inserting the bare character.
			// Read-only fields drop the buffered text entirely.
			if (!typedText.empty() && !ctrlDown && !field.IsReadOnly) {
				// ContentType filtering walks the buffered codepoints
				// against the post-selection-delete state so e.g. an
				// IntegerNumber field still accepts a leading '-' when
				// the user is overwriting an existing one.
				auto [lo, hi] = SelectionRange(field);
				std::string existingAfterDelete = field.Text;
				int caretAfterDelete = field.CaretBytePos;
				if (lo != hi) {
					existingAfterDelete.erase(lo, hi - lo);
					caretAfterDelete = lo;
				}
				const std::string filtered = FilterByContentType(
					field.ContentType, typedText,
					existingAfterDelete, caretAfterDelete);
				if (!filtered.empty()) {
					InsertAtCaret(field, filtered);
				}
			}

			if (enterPressed) {
				field.SubmittedThisFrame = true;
			}

			ClampCaret(field);
		}

		// ── Input field event dispatch ────────────────────────────────
		// OnValueChanged fires whenever Text mutates — covers user
		// typing, paste, clipboard cut, programmatic writes, scene-load
		// edits, anything. Diffed against LastObservedText so a freshly-
		// deserialised field with non-empty Text doesn't fire on the
		// first tick (ValueObserved gates the baseline). OnSubmitted
		// fires once when the user pressed Enter while focused.
		for (auto&& [entity, interact, field] : inputView.each()) {
			if (!field.ValueObserved) {
				field.LastObservedText = field.Text;
				field.ValueObserved = true;
			}

			if (field.Text != field.LastObservedText) {
				if (!field.OnValueChanged.Bindings.empty()) {
					InspectorEvents::DynamicArg dyn;
					dyn.Kind = InspectorEventArgKind::String;
					dyn.Encoded = field.Text;
					InspectorEvents::FireAllWithDynamicArg(scene, entity,
						field.OnValueChanged.Bindings, dyn);
				}
				field.LastObservedText = field.Text;
			}

			if (field.SubmittedThisFrame && !field.OnSubmitted.Bindings.empty()) {
				InspectorEvents::DynamicArg dyn;
				dyn.Kind = InspectorEventArgKind::String;
				dyn.Encoded = field.Text;
				InspectorEvents::FireAllWithDynamicArg(scene, entity,
					field.OnSubmitted.Bindings, dyn);
			}
		}

		// Sync the child TextRenderer for every input field so it shows
		// either the entered text (or placeholder when empty + unfocused).
		// The caret + selection highlight are NOT inserted into the text
		// string here — GuiRenderer paints them as separate quads, so
		// caret movement doesn't shift the underlying glyph layout.
		// Secret mode masks Text into one '*' per codepoint for display
		// only; field.Text itself stays untouched.
		for (auto&& [entity, interact, field] : inputView.each()) {
			if (field.TextEntity == entt::null || !registry.valid(field.TextEntity)) continue;
			if (!registry.all_of<TextRendererComponent>(field.TextEntity)) continue;
			auto& tc = registry.get<TextRendererComponent>(field.TextEntity);

			const bool hasContent = !field.Text.empty();
			const bool useText = hasContent || field.IsFocused;
			if (useText && hasContent && field.IsSecret) {
				tc.Text = MaskTextForSecret(field.Text);
			}
			else {
				tc.Text = useText ? field.Text : field.PlaceholderText;
			}
			tc.Color = useText ? field.TextColor : field.PlaceholderColor;
		}

		// ── 9. Dropdowns: open/close, sync label ─────────────────────
		// Open/close on click is handled in the action block above (and
		// outside-click closing happens before #5). Here we just sync
		// the optional LabelEntity to display the selected option.
		for (auto&& [entity, rect, dd] : dropdownView.each()) {
			InteractableComponent* interact = registry.try_get<InteractableComponent>(entity);
			// Read-only dropdowns refuse to open — header still tints
			// on hover / press (visual feedback that this is "live")
			// but the popup never appears via user input. Programmatic
			// writes to IsOpen still work for editor-driven previews.
			if (interact && interact->IsClicked && !popupConsumes && !dd.IsReadOnly) {
				dd.IsOpen = !dd.IsOpen;
			}

			if (dd.LabelEntity != entt::null && registry.valid(dd.LabelEntity)
				&& registry.all_of<TextRendererComponent>(dd.LabelEntity))
			{
				auto& tc = registry.get<TextRendererComponent>(dd.LabelEntity);
				if (!dd.Options.empty()) {
					const int idx = std::clamp(dd.SelectedIndex, 0,
						static_cast<int>(dd.Options.size()) - 1);
					tc.Text = dd.Options[idx];
				}
				else {
					tc.Text.clear();
				}
			}
		}

		// ── 10. Fan out UI events to managed subscribers ─────────────
		// Every transient flag we set above is good for exactly this
		// frame; the managed dispatcher reads them now and fires the
		// matching static UI events (Button.OnClick, Slider.OnValueChanged,
		// etc.). Doing this at the tail of Update means engine-detected
		// transitions reach script handlers the same frame they happen.
		ScriptEngine::RaiseUiEventDispatch();
	}

	void UIEventSystem::OnPreRender(Scene& scene) {
		// Play mode runs Update every frame, which already refreshes
		// every widget's derived visuals. Re-running here would just be
		// wasted work, and worse — would overwrite per-frame input-state
		// reactions (button hover tints, input-field caret) with their
		// edit-mode steady-state values mid-frame.
		if (Application* app = Application::GetInstance(); app && app->GetIsPlaying()) {
			return;
		}

		// Event-driven rebuild: only do work when something actually
		// changed since the last preview pass. Scene::MarkDirty (called
		// on every inspector edit, hierarchy reparent, entity create /
		// destroy, component add / remove) flags this; we clear it
		// after the rebuild so subsequent idle frames cost nothing.
		if (!scene.IsUIDirty()) {
			return;
		}

		auto& registry = scene.GetRegistry();

		// UI is opt-in. When a scene has no UI widgets at all, clear the
		// dirty pulse and bail before iterating any of the per-widget
		// views — otherwise a non-UI scene pays for six empty view scans
		// every time anything else in the scene marks dirty.
		if (registry.view<SliderComponent>().size() == 0
			&& registry.view<ToggleComponent>().size() == 0
			&& registry.view<DropdownComponent>().size() == 0
			&& registry.view<InputFieldComponent>().size() == 0
			&& registry.view<ScrollbarComponent>().size() == 0
			&& registry.view<ButtonComponent>().size() == 0)
		{
			scene.ClearUIDirty();
			return;
		}

		// Cross-entity refs (Fill / Handle / Checkmark / Text / Label)
		// aren't serialized — Update normally re-resolves them every
		// frame. In edit mode Update never ran, so the preview path has
		// to do the same resolution itself. Without it, a slider /
		// toggle / etc. created via the editor wouldn't have its
		// children wired up until the user entered play mode at least
		// once.
		const auto childHasImage = [&registry](EntityHandle e) {
			return registry.all_of<RectTransform2DComponent, ImageComponent>(e);
		};
		const auto childHasText = [&registry](EntityHandle e) {
			return registry.all_of<RectTransform2DComponent, TextRendererComponent>(e);
		};

		ResolveSliderChildrenForPreview(registry);
		for (auto&& [entity, toggle] : registry.view<ToggleComponent>().each()) {
			if (toggle.CheckmarkEntity == entt::null || !registry.valid(toggle.CheckmarkEntity)) {
				toggle.CheckmarkEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Checkmark", childHasImage);
			}
		}
		for (auto&& [entity, field] : registry.view<InputFieldComponent>().each()) {
			if (field.TextEntity == entt::null || !registry.valid(field.TextEntity)) {
				field.TextEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Text", childHasText);
			}
		}
		for (auto&& [entity, dd] : registry.view<DropdownComponent>().each()) {
			if (dd.LabelEntity == entt::null || !registry.valid(dd.LabelEntity)) {
				dd.LabelEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Label", childHasText);
			}
		}

		// ── Sliders: Fill stretch + Handle position from Value ────
		auto sliderView = registry.view<SliderComponent, RectTransform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, slider, rect] : sliderView.each()) {
			ApplySliderVisuals(registry, slider, rect);
		}

		// ── Scrollbars: Handle position + size from Value / Size ──
		for (auto&& [entity, sb] : registry.view<ScrollbarComponent>().each()) {
			if (sb.HandleEntity == entt::null || !registry.valid(sb.HandleEntity)) {
				sb.HandleEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Handle", childHasImage);
			}
		}
		auto scrollbarView = registry.view<ScrollbarComponent, RectTransform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, sb, rect] : scrollbarView.each()) {
			ApplyScrollbarVisuals(registry, sb, rect);
		}

		// ── Toggles: Checkmark visibility from IsOn ───────────────
		auto toggleView = registry.view<ToggleComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, toggle] : toggleView.each()) {
			if (toggle.CheckmarkEntity != entt::null && registry.valid(toggle.CheckmarkEntity)) {
				scene.GetEntity(toggle.CheckmarkEntity).SetEnabled(toggle.IsOn);
			}
		}

		// ── Dropdowns: Label text from SelectedIndex ──────────────
		auto dropdownView = registry.view<DropdownComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, dd] : dropdownView.each()) {
			ApplyDropdownVisuals(registry, dd);
		}

		// ── Input fields: TextEntity content from Text/Placeholder ─
		auto inputView = registry.view<InputFieldComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, field] : inputView.each()) {
			ApplyInputFieldVisuals(registry, field);
		}

		// ── Buttons: NormalColor preview tint ─────────────────────
		// Edit mode has no hover/press/disabled state to react to, so
		// the inspector should show what the button looks like at rest.
		// ButtonComponent owns the colour palette; ImageComponent.Color
		// is the rendered tint, derived in play mode every frame from
		// (interactable, isHovered, isPressed) → palette.
		//
		// Mirrors the play-mode resolver: if the button has an explicit
		// TargetGraphic, the preview tint goes there (image OR text);
		// otherwise it tints the button entity's own image. Without the
		// TargetGraphic respect here, edit-mode previews painted onto
		// the wrong entity (the button itself) instead of its child
		// label/background — visually broken for any button whose
		// graphic lives on a child entity.
		// Edit-mode preview mirrors the play-mode resolver above: walk
		// every Button (Interactable not required on the button entity),
		// and read the Interactable from the button OR its TargetGraphic.
		// Required so a button authored without its own InteractableComponent
		// previews the right palette in the inspector.
		auto buttonView = registry.view<ButtonComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, btn] : buttonView.each()) {
			InteractableComponent* interact = registry.try_get<InteractableComponent>(entity);
			const EntityHandle target = (btn.TargetGraphic != entt::null && registry.valid(btn.TargetGraphic))
				? btn.TargetGraphic : entity;
			if (!interact && target != entity && registry.valid(target)) {
				interact = registry.try_get<InteractableComponent>(target);
			}
			const bool interactable = interact ? interact->Interactable : true;
			const Color tint = interactable ? btn.NormalColor : btn.DisabledColor;
			if (auto* targetImage = registry.try_get<ImageComponent>(target)) {
				targetImage->Color = tint;
			}
			else if (auto* targetText = registry.try_get<TextRendererComponent>(target)) {
				targetText->Color = tint;
			}
		}

		// ── Toggle / InputField / Dropdown: NormalColor preview ───
		// Same contract as the button preview above — show Normal at
		// rest, Disabled when Interactable is off, so authoring the
		// palette in the inspector gives immediate feedback.
		auto togglePrevView = registry.view<ToggleComponent, ImageComponent, InteractableComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, toggle, image, interact] : togglePrevView.each()) {
			image.Color = interact.Interactable ? toggle.NormalColor : toggle.DisabledColor;
		}
		auto inputPrevView = registry.view<InputFieldComponent, ImageComponent, InteractableComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, field, image, interact] : inputPrevView.each()) {
			image.Color = interact.Interactable ? field.NormalColor : field.DisabledColor;
		}
		auto dropdownPrevView = registry.view<DropdownComponent, ImageComponent, InteractableComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, dd, image, interact] : dropdownPrevView.each()) {
			image.Color = interact.Interactable ? dd.NormalColor : dd.DisabledColor;
		}

		// ── Slider handle: NormalColor preview ────────────────────
		// Mirror the play-mode resolver: tint the handle's image when
		// the handle owns the InteractableComponent, otherwise fall back
		// to the parent. Same precedence as the play-mode pass.
		auto sliderPrevView = registry.view<SliderComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, slider] : sliderPrevView.each()) {
			ImageComponent* targetImage = nullptr;
			InteractableComponent* targetInteract = nullptr;
			if (slider.HandleEntity != entt::null
				&& registry.valid(slider.HandleEntity))
			{
				targetImage = registry.try_get<ImageComponent>(slider.HandleEntity);
				targetInteract = registry.try_get<InteractableComponent>(slider.HandleEntity);
			}
			if (!targetImage || !targetInteract) {
				targetImage = registry.try_get<ImageComponent>(entity);
				targetInteract = registry.try_get<InteractableComponent>(entity);
			}
			if (targetImage && targetInteract) {
				targetImage->Color = targetInteract->Interactable ? slider.NormalColor : slider.DisabledColor;
			}
		}

		// ── Circular slider handle: NormalColor preview ──────────
		// Same edit-time pattern as the linear slider above: tint the
		// handle so the user sees the authored palette without entering
		// play mode. Falls back to the slider's parent interactable when
		// the handle has none (older scenes that didn't add one).
		auto circularPrevView = registry.view<CircularSliderComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, cs] : circularPrevView.each()) {
			if (cs.HandleEntity == entt::null
				|| !registry.valid(cs.HandleEntity)) continue;
			ImageComponent* targetImage = registry.try_get<ImageComponent>(cs.HandleEntity);
			if (!targetImage) continue;
			InteractableComponent* targetInteract = registry.try_get<InteractableComponent>(cs.HandleEntity);
			if (!targetInteract) targetInteract = registry.try_get<InteractableComponent>(entity);
			if (!targetInteract) continue;
			targetImage->Color = targetInteract->Interactable ? cs.NormalColor : cs.DisabledColor;
		}

		scene.ClearUIDirty();
	}

}
