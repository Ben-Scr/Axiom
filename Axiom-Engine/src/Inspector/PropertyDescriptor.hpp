#pragma once

#include "Inspector/PropertyMetadata.hpp"
#include "Inspector/PropertyType.hpp"
#include "Inspector/PropertyValue.hpp"
#include "Scene/Entity.hpp"

#include <functional>
#include <string>
#include <utility>

namespace Axiom {

	// Describes ONE inspectable field on a component (or script).
	//
	// `Get(entity)` reads the field's current value into a PropertyValue.
	// `Set(entity, value)` writes a new value back. Both run per entity in a
	// multi-selection. Component-aware code lives entirely inside the
	// captured lambdas, so the drawer never has to know which native type
	// owns the field.
	struct PropertyDescriptor {
		std::string Name;          // Stable serialised name (matches the C# field name).
		std::string DisplayName;   // Pretty label shown in the inspector. Defaults to Name.
		PropertyType Type = PropertyType::None;
		PropertyMetadata Metadata;

		std::function<PropertyValue(const Entity&)> Get;
		std::function<void(Entity&, const PropertyValue&)> Set;
	};

} // namespace Axiom
