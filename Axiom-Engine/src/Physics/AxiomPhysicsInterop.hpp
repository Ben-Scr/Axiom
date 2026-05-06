#pragma once

// The standalone Axiom-Physics package still exposes its API under the old
// BoltPhys namespace. Keep the engine-side name stable after the Axiom rename.
namespace BoltPhys {}
namespace AxiomPhys = BoltPhys;
