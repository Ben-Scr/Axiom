#pragma once

#include "Core/Export.hpp"

namespace Index {

	// IPhysicsEngine is the swappable physics simulation boundary. Application
	// owns one via std::unique_ptr<IPhysicsEngine>; the default implementation
	// is PhysicsSystem2D (Box2D-backed). Packages can substitute alternates
	// (NullPhysicsEngine for headless deterministic builds, alternate 2D/3D
	// engines) by installing a different concrete impl before Application::
	// Initialize runs.
	//
	// Current state: this is the first slice. The interface captures lifecycle
	// + per-frame ticks only — what Application itself calls. Components,
	// systems, and scripting bindings still talk to PhysicsSystem2D's static
	// world accessors and pass around raw b2Body* / b2Fixture* pointers. The
	// follow-up slice introduces PhysicsBodyHandle (opaque generation/slot ID)
	// to plug that leak so a Null / non-Box2D backend becomes implementable.
	class INDEX_API IPhysicsEngine {
	public:
		virtual ~IPhysicsEngine() = default;

		virtual void Initialize() = 0;
		virtual void Shutdown() = 0;

		// Fixed-timestep simulation step. Called by Application::FixedUpdate.
		virtual void FixedUpdate(float dt) = 0;

		// Editor-mode + play-mode sync; no simulation step. Called every frame.
		virtual void Update() = 0;
	};

}
