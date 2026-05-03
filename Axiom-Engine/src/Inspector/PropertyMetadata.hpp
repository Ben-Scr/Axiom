#pragma once

#include "Inspector/PropertyType.hpp"

#include <memory>
#include <string>

namespace Axiom {

	// Per-descriptor metadata. Optional fields default to "no override". The
	// drawer reads these to pick clamp ranges, drag speeds, tooltip strings,
	// section headers, etc.
	//
	// All numeric metadata is stored as double so a single struct works for
	// integer and floating-point clamps without templating.
	struct PropertyMetadata {
		// Hover tooltip text (empty = no tooltip).
		std::string Tooltip;

		// Section header drawn ABOVE the property row. Renders as a separator
		// with a bigger font when HeaderSize > 0.
		std::string HeaderContent;
		int HeaderSize = 0;

		// Vertical spacer above the property row, in pixels.
		float SpaceHeight = 0.0f;
		bool HasSpace = false;

		// Numeric clamp / drag-speed.
		bool HasClamp = false;
		double ClampMin = 0.0;
		double ClampMax = 0.0;
		float DragSpeed = 0.1f;

		// Whether the field is read-only in the inspector.
		bool ReadOnly = false;

		// For PropertyType::Enum / FlagEnum.
		std::shared_ptr<EnumDescriptor> Enum;

		// For PropertyType::ComponentRef — the displayName of the required
		// ComponentInfo (matches the engine's ComponentInfo::displayName).
		std::string ComponentTypeName;
	};

} // namespace Axiom
