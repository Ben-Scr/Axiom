#include "pch.hpp"
#include "Serialization/JsonArchive.hpp"

#include "Scene/ComponentRegistry.hpp"

#include <charconv>
#include <string>

namespace Index::Serialization {

	namespace {
		// UUID round-trip via a base-10 string: matches the existing
		// UIUuidToJson / UIUuidFromJson convention so legacy scenes load
		// without conversion (and so values > 2^53 don't get crushed into
		// double precision the way a raw Json::Value(uint64_t) → Number
		// path would when re-read on the JS-tooling side).
		std::string UuidToString(std::uint64_t value) {
			return std::to_string(value);
		}

		std::uint64_t UuidFromString(const std::string& text, std::uint64_t fallback) {
			if (text.empty()) return fallback;
			std::uint64_t result = 0;
			auto first = text.data();
			auto last = text.data() + text.size();
			auto [ptr, ec] = std::from_chars(first, last, result);
			if (ec != std::errc()) return fallback;
			return result;
		}
	}

	JsonArchive::JsonArchive(ArchiveMode mode, Json::Value* target, const Json::Value* source,
		const ComponentRegistry* registry)
		: IArchive(mode)
		, m_Registry(registry) {
		Scope root;
		if (mode == ArchiveMode::Write) {
			root.write = target;
		} else {
			root.read = source;
		}
		m_ScopeStack.push_back(root);
	}

	JsonArchive JsonArchive::ForWriting(Json::Value& target, const ComponentRegistry& registry) {
		return JsonArchive(ArchiveMode::Write, &target, nullptr, &registry);
	}

	JsonArchive JsonArchive::ForReading(const Json::Value& source, const ComponentRegistry& registry) {
		return JsonArchive(ArchiveMode::Read, nullptr, &source, &registry);
	}

	void JsonArchive::BeginComponent(std::uint64_t nameHash, std::uint16_t version) {
		m_CurrentComponentVersion = version;

		// Resolve the JSON key from the hash. SceneSerializer is the only
		// caller and always pulls the hash straight from a live ComponentInfo,
		// so an unresolved hash means a stale call — push an empty scope
		// instead of crashing so the surrounding loop stays well-formed.
		const ComponentInfo* info = (m_Registry != nullptr) ? m_Registry->FindByHash(nameHash) : nullptr;
		if (info == nullptr || info->serializedName.empty()) {
			Scope empty;
			m_ScopeStack.push_back(empty);
			return;
		}

		if (IsWriting()) {
			Json::Value* parent = CurrentWrite();
			Scope scope;
			if (parent != nullptr) {
				Json::Value& added = parent->AddMember(info->serializedName, Json::Value::MakeObject());
				scope.write = &added;
			}
			m_ScopeStack.push_back(scope);
		} else {
			const Json::Value* parent = CurrentRead();
			Scope scope;
			if (parent != nullptr) {
				scope.read = parent->FindMember(info->serializedName);
			}
			m_ScopeStack.push_back(scope);
		}
	}

	void JsonArchive::EndComponent() {
		if (m_ScopeStack.size() > 1) {
			m_ScopeStack.pop_back();
		}
	}

	void JsonArchive::Field(std::string_view name, bool& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(v));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) v = m->AsBoolOr(v);
			}
		}
	}

	void JsonArchive::Field(std::string_view name, std::int32_t& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(static_cast<std::int64_t>(v)));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) v = m->AsIntOr(v);
			}
		}
	}

	void JsonArchive::Field(std::string_view name, std::int64_t& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(v));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) v = m->AsInt64Or(v);
			}
		}
	}

	void JsonArchive::Field(std::string_view name, std::uint32_t& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(static_cast<std::uint64_t>(v)));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name))
					v = static_cast<std::uint32_t>(m->AsUInt64Or(v));
			}
		}
	}

	void JsonArchive::Field(std::string_view name, std::uint64_t& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(v));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) v = m->AsUInt64Or(v);
			}
		}
	}

	void JsonArchive::Field(std::string_view name, float& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(static_cast<double>(v)));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) {
					if (m->IsNumber()) {
						v = static_cast<float>(m->AsDoubleOr(static_cast<double>(v)));
					}
					else {
						// Type mismatch — keep the default and warn so a
						// hand-edited or stale-format scene doesn't load
						// with silent data loss. Missing fields stay quiet
						// (the FindMember above gates this branch).
						IDX_CORE_WARN_TAG("JsonArchive",
							"Field '{}': expected number for float, got non-number — keeping default",
							name);
					}
				}
			}
		}
	}

	void JsonArchive::Field(std::string_view name, double& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(v));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) v = m->AsDoubleOr(v);
			}
		}
	}

	void JsonArchive::Field(std::string_view name, std::string& v) {
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(v));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) v = m->AsStringOr(v);
			}
		}
	}

	void JsonArchive::Field(std::string_view name, UUID& v) {
		// String encoding matches UIUuidToJson — preserves 64-bit precision
		// across JS/JSON tooling and avoids the NumberKind drift the legacy
		// path was forced to introduce when raw uint64 values exceeded 2^53.
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				scope->AddMember(std::string(name), Json::Value(UuidToString(static_cast<std::uint64_t>(v))));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) {
					if (m->IsString()) {
						v = UUID(UuidFromString(m->AsStringOr(""), static_cast<std::uint64_t>(v)));
					} else if (m->IsNumber()) {
						// Tolerate legacy values written as raw numbers.
						v = UUID(m->AsUInt64Or(static_cast<std::uint64_t>(v)));
					}
				}
			}
		}
	}

	void JsonArchive::Field(std::string_view name, Color& v) {
		// Layout {r,g,b,a} matches UIColorToJson — already what every existing
		// color field on disk uses.
		if (IsWriting()) {
			if (Json::Value* scope = CurrentWrite()) {
				Json::Value obj = Json::Value::MakeObject();
				obj.AddMember("r", Json::Value(static_cast<double>(v.r)));
				obj.AddMember("g", Json::Value(static_cast<double>(v.g)));
				obj.AddMember("b", Json::Value(static_cast<double>(v.b)));
				obj.AddMember("a", Json::Value(static_cast<double>(v.a)));
				scope->AddMember(std::string(name), std::move(obj));
			}
		} else {
			if (const Json::Value* scope = CurrentRead()) {
				if (const Json::Value* m = scope->FindMember(name)) {
					if (m->IsObject()) {
						if (const Json::Value* r = m->FindMember("r")) v.r = static_cast<float>(r->AsDoubleOr(v.r));
						if (const Json::Value* g = m->FindMember("g")) v.g = static_cast<float>(g->AsDoubleOr(v.g));
						if (const Json::Value* b = m->FindMember("b")) v.b = static_cast<float>(b->AsDoubleOr(v.b));
						if (const Json::Value* a = m->FindMember("a")) v.a = static_cast<float>(a->AsDoubleOr(v.a));
					}
					else if (m->IsNumber()) {
						// Legacy fallback: a single number is treated as
						// grayscale with alpha=1. Prevents an old scene
						// from silently propagating wrong colors when its
						// Color was authored before the {r,g,b,a} schema.
						IDX_CORE_WARN_TAG("JsonArchive",
							"Field '{}': legacy single-number color form; treating as grayscale",
							name);
						const float gray = static_cast<float>(m->AsDoubleOr(0.0));
						v.r = gray; v.g = gray; v.b = gray; v.a = 1.0f;
					}
					else {
						IDX_CORE_WARN_TAG("JsonArchive",
							"Field '{}': expected {{r,g,b,a}} object or number, got unsupported JSON kind — keeping default",
							name);
					}
				}
			}
		}
	}

	void JsonArchive::Object(std::string_view name, void* userData,
		void (*fn)(IArchive&, void*)) {
		if (IsWriting()) {
			Json::Value* parent = CurrentWrite();
			if (parent == nullptr) {
				fn(*this, userData); // keep the trampoline well-formed; writes go nowhere
				return;
			}
			Json::Value& added = parent->AddMember(std::string(name), Json::Value::MakeObject());
			Scope scope;
			scope.write = &added;
			m_ScopeStack.push_back(scope);
			fn(*this, userData);
			m_ScopeStack.pop_back();
		} else {
			const Json::Value* parent = CurrentRead();
			const Json::Value* child = (parent != nullptr) ? parent->FindMember(name) : nullptr;
			if (child == nullptr || !child->IsObject()) {
				// Skip the body entirely — leaves caller defaults intact.
				return;
			}
			Scope scope;
			scope.read = child;
			m_ScopeStack.push_back(scope);
			fn(*this, userData);
			m_ScopeStack.pop_back();
		}
	}

	void JsonArchive::Array(std::string_view name, std::uint32_t& count,
		void* userData,
		void (*fn)(IArchive&, std::uint32_t, void*)) {
		if (IsWriting()) {
			Json::Value* parent = CurrentWrite();
			if (parent == nullptr) return;
			Json::Value& arr = parent->AddMember(std::string(name), Json::Value::MakeArray());
			auto& elems = arr.EnsureArray();
			elems.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				elems.emplace_back(Json::Value::MakeObject());
				Scope scope;
				scope.write = &elems.back();
				m_ScopeStack.push_back(scope);
				fn(*this, i, userData);
				m_ScopeStack.pop_back();
			}
		} else {
			const Json::Value* parent = CurrentRead();
			const Json::Value* arr = (parent != nullptr) ? parent->FindMember(name) : nullptr;
			if (arr == nullptr || !arr->IsArray()) {
				count = 0;
				return;
			}
			const auto& elems = arr->GetArray();
			count = static_cast<std::uint32_t>(elems.size());
			for (std::uint32_t i = 0; i < count; ++i) {
				Scope scope;
				scope.read = &elems[i];
				m_ScopeStack.push_back(scope);
				fn(*this, i, userData);
				m_ScopeStack.pop_back();
			}
		}
	}

} // namespace Index::Serialization
