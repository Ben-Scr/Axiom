#pragma once

#include <cstring>
#include <typeindex>
#include <typeinfo>

namespace Index {

	class NativeScript;

	class NativeScriptRegistry {
	public:
		using Factory = NativeScript* (*)();

		struct Entry {
			const char* name;
			Factory factory;
			Entry* next;
		};

		// Secondary index: type_info* -> script name. Populated by
		// RegisterType (called from REGISTER_SCRIPT). Lets the
		// type-keyed entity dispatch (Entity::AddComponent<TScript>())
		// resolve to the existing string-keyed factory without forcing
		// every user script to expose a string-name member.
		//
		// Stored as `const std::type_info*` (pointer) rather than
		// std::type_index so the entries array can be value-initialized
		// (typeid(T) returns a reference whose lifetime spans the
		// program; std::type_index has no default ctor, so a
		// `TypeEntry entries[256]` array would be ill-formed otherwise).
		// Equality compares via `*a == *b` which is well-defined for
		// type_info.
		struct TypeEntry {
			const std::type_info* type;
			const char* name;
			TypeEntry* next;
		};

		inline static Entry* s_Head = nullptr;
		inline static TypeEntry* s_TypeHead = nullptr;

		static void Register(const char* name, Factory factory) {
			if (!name || !factory) {
				return;
			}

			for (Entry* entry = s_Head; entry; entry = entry->next) {
				if (std::strcmp(name, entry->name) == 0) {
					entry->factory = factory;
					return;
				}
			}

			static Entry entries[256];
			static int count = 0;
			if (count < 256) {
				entries[count] = { name, factory, s_Head };
				s_Head = &entries[count++];
			}
		}

		// Map `type` -> existing string name. Called once per script
		// from REGISTER_SCRIPT alongside Register(name, factory).
		// Idempotent: re-registering the same type just refreshes the
		// name pointer (matters across hot-reload of the user DLL).
		static void RegisterType(const std::type_info& type, const char* name) {
			if (!name) return;
			for (TypeEntry* e = s_TypeHead; e; e = e->next) {
				if (*e->type == type) {
					e->name = name;
					return;
				}
			}
			static TypeEntry entries[256];
			static int count = 0;
			if (count < 256) {
				entries[count] = { &type, name, s_TypeHead };
				s_TypeHead = &entries[count++];
			}
		}

		// Resolve a registered script type back to its string name, or
		// nullptr when the type was never registered via REGISTER_SCRIPT.
		// O(N) linear scan — fine for the handful of scripts a project
		// typically registers; per-call cost dwarfed by the downstream
		// CreateInstance / dynamic_cast work.
		static const char* NameOfType(const std::type_info& type) {
			for (TypeEntry* e = s_TypeHead; e; e = e->next) {
				if (*e->type == type) return e->name;
			}
			return nullptr;
		}

		static NativeScript* Create(const char* name) {
			if (!name) {
				return nullptr;
			}

			for (auto* e = s_Head; e; e = e->next) {
				if (std::strcmp(name, e->name) == 0) {
					return e->factory();
				}
			}

			return nullptr;
		}

		static bool Has(const char* name) {
			if (!name) {
				return false;
			}

			for (auto* e = s_Head; e; e = e->next) {
				if (std::strcmp(name, e->name) == 0) {
					return true;
				}
			}

			return false;
		}
	};

} // namespace Index
