#pragma once

#include "Components/UI/InspectorEventBinding.hpp"
#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"

#include <string>
#include <vector>

namespace Axiom {

	class Scene;

	// Fires inspector-bound C# event lists (Button.OnClick and friends).
	// Resolution per binding:
	//   1. Pick the target entity — the binding's owner if TargetEntityUUID
	//      is 0, otherwise Scene::TryResolveEntityRef.
	//   2. Look up the matching ScriptInstance on the target's
	//      ScriptComponent (managed instance, matching ClassName).
	//   3. Invoke the parameterless `void` method by name through
	//      ScriptEngine's managed callback.
	// Missing entity / script / method paths log once per (class, method)
	// key so a stale binding can't spam the console every frame.
	namespace InspectorEvents {

		AXIOM_API bool Fire(Scene& scene, EntityHandle ownerEntity,
			const InspectorEventBinding& binding);

		// Convenience for the host component's dispatch loop. Skips
		// !Enabled rows and returns the count of successful invocations.
		AXIOM_API int FireAll(Scene& scene, EntityHandle ownerEntity,
			const std::vector<InspectorEventBinding>& bindings);

		// "Dynamic-arg" overload — used by widgets like Slider /
		// InputField that have an event-time value (the new slider
		// position, the just-typed text, etc.). When a binding's
		// authored ArgumentKind is Void, the dynamic value is left
		// alone — the bound method is called bare. When it matches the
		// dynamic value's type, the dispatcher overrides the user-
		// authored static value with the live one. Bindings of an
		// unrelated type still fire with their authored static value
		// (handy for "on slider change, also play this string-named
		// sound effect").
		struct DynamicArg {
			InspectorEventArgKind Kind = InspectorEventArgKind::Void;
			std::string Encoded; // same wire format as InspectorEventBinding::ArgumentValue
		};

		AXIOM_API int FireAllWithDynamicArg(Scene& scene, EntityHandle ownerEntity,
			const std::vector<InspectorEventBinding>& bindings,
			const DynamicArg& dynamicArg);

		// Cleared whenever the user assembly reloads — stale class /
		// method names from the previous load shouldn't suppress fresh
		// warnings after the reload.
		AXIOM_API void ResetMissingMethodLog();

		// Editor helper: returns the names of every inspector-invokable
		// method on the given C# class. Empty if the class isn't loaded.
		// Cached internally with the same lifetime as ScriptEngine's
		// class cache (cleared on assembly reload).
		AXIOM_API std::vector<std::string> GetInvokableMethods(const std::string& className);

	}

}
