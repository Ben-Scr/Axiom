#pragma once

#include "Collections/Vec2.hpp"
#include "Components/UI/UIAlignment.hpp"

#include <cstdint>

namespace Axiom {

	// Which corner of the parent rect holds the FIRST child cell. The
	// remaining cells flow from there along StartAxis.
	enum class GridLayoutStartCorner : std::uint8_t {
		UpperLeft  = 0,
		UpperRight = 1,
		LowerLeft  = 2,
		LowerRight = 3,
	};

	// Which axis we fill first before wrapping. Horizontal = fill the
	// current row left-to-right, then drop to the next row; Vertical =
	// fill the current column top-to-bottom, then move to the next column.
	enum class GridLayoutStartAxis : std::uint8_t {
		Horizontal = 0,
		Vertical   = 1,
	};

	// How the row × column count is decided.
	enum class GridLayoutConstraint : std::uint8_t {
		// Cell count derived from the parent rect's size and CellSize.
		Flexible         = 0,
		// Exactly ConstraintCount columns; rows expand as needed.
		FixedColumnCount = 1,
		// Exactly ConstraintCount rows; columns expand as needed.
		FixedRowCount    = 2,
	};

	// Lays out an entity's children in a 2D grid where every cell has the
	// same size (CellSize) and is separated from its neighbours by Spacing
	// pixels. Mirrors Unity's GridLayoutGroup.
	struct GridLayoutGroupComponent {
		// Padding inset (Left, Right, Top, Bottom) inside the parent rect.
		float PaddingLeft = 0.0f;
		float PaddingRight = 0.0f;
		float PaddingTop = 0.0f;
		float PaddingBottom = 0.0f;

		// Each cell's size, in pixels. Applied to every child's SizeDelta.
		Vec2 CellSize{ 100.0f, 100.0f };
		// Pixels between adjacent cells (X = horizontal gap, Y = vertical gap).
		Vec2 Spacing{ 0.0f, 0.0f };

		GridLayoutStartCorner StartCorner = GridLayoutStartCorner::UpperLeft;
		GridLayoutStartAxis   StartAxis   = GridLayoutStartAxis::Horizontal;

		// Aligns the grid block as a whole inside the parent rect when the
		// children don't fill it.
		UIAlignment ChildAlignment = UIAlignment::UpperLeft;

		GridLayoutConstraint Constraint = GridLayoutConstraint::Flexible;
		int ConstraintCount = 1;

		// When true, children are placed in reverse hierarchy order: the
		// LAST child takes the first cell, the first child takes the last
		// cell. Useful for stacking-from-the-end layouts (recent-message
		// lists, undo histories, "newest at top") without rebuilding the
		// hierarchy. Cell positions still flow from StartCorner along
		// StartAxis exactly as before — only the children → cells mapping
		// is reversed.
		bool Reverse = false;
	};

}
