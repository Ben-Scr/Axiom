#include "pch.hpp"
#include "Scripting/InspectorEventDispatch.hpp"

#include "Components/UI/InspectorEventBinding.hpp"
#include "Core/Log.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptEngine.hpp"

#include <array>
#include <mutex>
#include <unordered_set>

namespace Axiom::InspectorEvents {

	namespace {
		std::mutex g_LogMutex;
		std::unordered_set<std::string> g_LoggedMissing;

		bool ShouldLogMiss(const std::string& key) {
			std::scoped_lock lock(g_LogMutex);
			return g_LoggedMissing.insert(key).second;
		}

		// Look up a managed ScriptInstance on `entity`'s ScriptComponent
		// matching `className`. Returns 0 (== invalid GC handle) if no
		// match or the instance hasn't been bound yet.
		uint32_t FindManagedHandle(Scene& scene, EntityHandle entity, const std::string& className) {
			ScriptComponent* sc = nullptr;
			if (!scene.TryGetComponent<ScriptComponent>(entity, sc) || sc == nullptr) {
				return 0;
			}
			for (const ScriptInstance& s : sc->Scripts) {
				if (s.HasManagedInstance() && s.GetClassName() == className) {
					return s.GetGCHandle();
				}
			}
			return 0;
		}
	}

	bool Fire(Scene& scene, EntityHandle ownerEntity, const InspectorEventBinding& binding) {
		if (binding.ScriptClassName.empty() || binding.MethodName.empty()) {
			return false;
		}

		EntityHandle target = ownerEntity;
		if (binding.TargetEntityUUID != 0) {
			EntityHandle resolved = entt::null;
			if (!scene.TryResolveEntityRef(binding.TargetEntityUUID, resolved)) {
				// Target gone — common in dynamic UIs (entity destroyed
				// between scene tick and click). Silent skip, no log.
				return false;
			}
			target = resolved;
		}

		const uint32_t handle = FindManagedHandle(scene, target, binding.ScriptClassName);
		if (handle == 0) {
			const std::string key = "noinstance:" + binding.ScriptClassName + "." + binding.MethodName;
			if (ShouldLogMiss(key)) {
				AIM_CORE_WARN_TAG("InspectorEvents",
					"OnClick binding '{}.{}' has no live instance on the target entity",
					binding.ScriptClassName, binding.MethodName);
			}
			return false;
		}

		const ManagedCallbacks& callbacks = ScriptEngine::GetCallbacks();
		if (callbacks.InvokeScriptMethodByName == nullptr) {
			// Bridge not initialised yet (or running against an older
			// ScriptCore that pre-dates this slot). Bail quietly.
			return false;
		}

		// Pass the binding's typed argument to the C# side. Void bindings
		// send a null pointer for ArgValue — the C# parser ignores it.
		// All other kinds use the encoded string in ArgumentValue
		// (see InspectorEventBinding for the per-kind format).
		const uint8_t argKind = static_cast<uint8_t>(binding.ArgumentKind);
		const char* argValueCStr =
			binding.ArgumentKind == InspectorEventArgKind::Void
			? nullptr
			: binding.ArgumentValue.c_str();
		const int rc = callbacks.InvokeScriptMethodByName(
			static_cast<int32_t>(handle), binding.MethodName.c_str(),
			argKind, argValueCStr);
		if (rc == 0) {
			const std::string key = "missing:" + binding.ScriptClassName + "." + binding.MethodName;
			if (ShouldLogMiss(key)) {
				AIM_CORE_WARN_TAG("InspectorEvents",
					"Inspector-event binding '{}.{}' — method not found on class",
					binding.ScriptClassName, binding.MethodName);
			}
			return false;
		}
		return true;
	}

	int FireAll(Scene& scene, EntityHandle ownerEntity,
		const std::vector<InspectorEventBinding>& bindings)
	{
		int fired = 0;
		for (const InspectorEventBinding& b : bindings) {
			if (!b.Enabled) continue;
			if (Fire(scene, ownerEntity, b)) {
				++fired;
			}
		}
		return fired;
	}

	int FireAllWithDynamicArg(Scene& scene, EntityHandle ownerEntity,
		const std::vector<InspectorEventBinding>& bindings,
		const DynamicArg& dynamicArg)
	{
		int fired = 0;
		for (const InspectorEventBinding& b : bindings) {
			if (!b.Enabled) continue;
			// Override the binding's static argument when both kinds
			// match (e.g. Slider.OnValueChanged dispatching to a
			// `void Foo(float)` method — the user wants the live
			// slider value, not whatever placeholder was typed in
			// the inspector). Bindings of any other kind keep their
			// authored static value, which lets the same event list
			// host both "consume the dynamic value" and "kick a
			// constant" handlers side-by-side.
			if (dynamicArg.Kind != InspectorEventArgKind::Void
				&& b.ArgumentKind == dynamicArg.Kind)
			{
				InspectorEventBinding patched = b;
				patched.ArgumentValue = dynamicArg.Encoded;
				if (Fire(scene, ownerEntity, patched)) ++fired;
			}
			else if (Fire(scene, ownerEntity, b)) {
				++fired;
			}
		}
		return fired;
	}

	void ResetMissingMethodLog() {
		std::scoped_lock lock(g_LogMutex);
		g_LoggedMissing.clear();
	}

	std::vector<std::string> GetInvokableMethods(const std::string& className) {
		std::vector<std::string> result;
		if (className.empty()) return result;

		const ManagedCallbacks& callbacks = ScriptEngine::GetCallbacks();
		if (callbacks.GetInvokableMethodsBuffer == nullptr) {
			return result;
		}

		// First call — sized probe. Buffer big enough for typical results;
		// only resize if the C# side reports more.
		std::array<char, 2048> stackBuf{};
		int needed = callbacks.GetInvokableMethodsBuffer(
			className.c_str(), stackBuf.data(), static_cast<int>(stackBuf.size()));

		std::string payload;
		if (needed > 0 && needed <= static_cast<int>(stackBuf.size())) {
			payload.assign(stackBuf.data());
		}
		else if (needed > 0) {
			std::vector<char> heapBuf(static_cast<std::size_t>(needed));
			callbacks.GetInvokableMethodsBuffer(
				className.c_str(), heapBuf.data(), needed);
			payload.assign(heapBuf.data());
		}
		if (payload.empty() || payload == "[]") return result;

		// Tiny JSON-array-of-strings parser. The C# side emits exactly
		// this shape (escaped strings, comma-separated, no whitespace),
		// so we don't need a full JSON parser. Worst case a malformed
		// payload yields an empty list, which the caller already handles.
		std::size_t i = 0;
		const std::size_t n = payload.size();
		auto skipWs = [&]() {
			while (i < n && (payload[i] == ' ' || payload[i] == '\t' || payload[i] == '\n' || payload[i] == '\r')) ++i;
		};
		skipWs();
		if (i >= n || payload[i] != '[') return result;
		++i;
		while (i < n) {
			skipWs();
			if (i < n && payload[i] == ']') break;
			if (i >= n || payload[i] != '"') break;
			++i;
			std::string item;
			while (i < n && payload[i] != '"') {
				if (payload[i] == '\\' && i + 1 < n) {
					const char esc = payload[i + 1];
					if (esc == 'n') item.push_back('\n');
					else if (esc == 'r') item.push_back('\r');
					else if (esc == 't') item.push_back('\t');
					else item.push_back(esc);
					i += 2;
				}
				else {
					item.push_back(payload[i]);
					++i;
				}
			}
			if (i >= n) break;
			++i; // consume closing quote
			result.push_back(std::move(item));
			skipWs();
			if (i < n && payload[i] == ',') ++i;
		}
		return result;
	}

}
