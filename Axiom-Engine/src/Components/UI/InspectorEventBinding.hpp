#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Axiom {

	// One row in an inspector-bound event list. Resolves to a managed C# script
	// instance + a method on it. The dispatcher fires every row in the list
	// when its host component's trigger condition is met (e.g. ButtonComponent
	// fires its OnClick list on the rising edge of
	// InteractableComponent::IsClicked).
	//
	// TargetEntityUUID == 0 means "self" — the dispatcher uses the host
	// component's owning entity. Otherwise the UUID is resolved through
	// Scene::TryResolveEntityRef just like any other entity reference, so the
	// binding survives scene-load even though runtime EntityHandle values get
	// reallocated.
	//
	// ScriptClassName matches `ScriptInstance::GetClassName()` on the resolved
	// entity's `ScriptComponent`. MethodName is a bare method name; the C# side
	// caches MethodInfo per (class, method) and logs once on miss.

	// First-parameter type the bound method accepts. `Void` means the method
	// takes no arguments — the dispatcher invokes it bare. Every other kind
	// means the method takes exactly one parameter of that type, and
	// `ArgumentValue` carries the user-authored static value the dispatcher
	// passes at fire time. The wire format for `ArgumentValue` is type-keyed:
	//   Bool      "0" / "1"
	//   Int       decimal int  ("-42")
	//   Float     "1.5"
	//   Double    "1.5"
	//   String    raw literal text
	//   Vec2      "x,y"   (two decimal floats)
	//   Color     "r,g,b,a" (four decimal 0-1 floats)
	//   EntityRef UUID as decimal uint64
	enum class InspectorEventArgKind : uint8_t {
		Void = 0,
		Bool = 1,
		Int = 2,
		Float = 3,
		Double = 4,
		String = 5,
		Vec2 = 6,
		Color = 7,
		EntityRef = 8,
	};

	struct InspectorEventBinding {
		uint64_t TargetEntityUUID = 0;
		std::string ScriptClassName;
		std::string MethodName;
		bool Enabled = true;

		// Typed static argument. `ArgumentKind` matches the C# method's
		// first parameter type; `ArgumentValue` is the encoded string the
		// dispatcher hands to the C# side. `Void` ⇒ argument unused.
		InspectorEventArgKind ArgumentKind = InspectorEventArgKind::Void;
		std::string ArgumentValue;
	};

	struct InspectorEventList {
		std::vector<InspectorEventBinding> Bindings;
	};

}
