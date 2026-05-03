#pragma once

#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/Entity.hpp"

#include <span>
#include <string>

namespace Axiom::PropertyDrawer {

	// Render one property row for a multi-selection of entities. Reads the
	// current value from each entity through `descriptor.Get`, decides if
	// it's uniform or mixed, dispatches to the right ImGui widget, and on
	// edit pushes the new value back through `descriptor.Set` for every
	// entity in the span.
	//
	// `fieldKey` is used to disambiguate reference-picker callbacks across
	// frames. For native components a stable key like
	// "<componentDisplayName>.<fieldName>" works.
	//
	// Returns true if the user changed the value this frame.
	bool Draw(std::span<const Entity> entities, const PropertyDescriptor& descriptor,
		const std::string& fieldKey);

	// Convenience overload: builds a key from a caller-supplied prefix +
	// descriptor.Name.
	bool DrawWithPrefix(std::span<const Entity> entities,
		const PropertyDescriptor& descriptor, const std::string& fieldKeyPrefix);

	// Render every PropertyDescriptor on a component for a multi-selection.
	// The default drawInspector that ComponentInfo gains via the editor's
	// auto-attach calls this with `descriptors = info.properties` and
	// `fieldKeyPrefix = info.displayName`.
	void DrawAll(std::span<const Entity> entities,
		std::span<const PropertyDescriptor> descriptors,
		const std::string& fieldKeyPrefix);

} // namespace Axiom::PropertyDrawer
