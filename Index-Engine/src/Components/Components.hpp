#pragma once

// DEPRECATED: use the narrower headers below instead.
//
// This umbrella used to pull in every built-in component header, which forced
// every TU that included it to also compile the full Graphics, Physics, Audio,
// and Scripting headers. To break that coupling, this header now only re-exports:
//
//   - "Components/Forward.hpp"      forward declarations of every built-in
//   - "Components/General/General.hpp" concrete General/* components
//   - "Components/UI/UI.hpp"        concrete UI/* components
//   - "Components/Tags.hpp"         empty tag types
//
// Translation units that need a Graphics, Physics, Audio, or Scripting
// component must include the specific header(s) they touch. Registration sites
// that legitimately need every built-in (e.g. BuiltInComponentRegistration.cpp)
// should include the per-subsystem headers explicitly.
//
// This header will become a hard error in a future release; please migrate.

#if defined(_MSC_VER)
#pragma message("warning: <Components/Components.hpp> is deprecated. Include Forward.hpp or specific component headers; see header for details.")
#elif defined(__GNUC__) || defined(__clang__)
#warning "<Components/Components.hpp> is deprecated. Include Forward.hpp or specific component headers; see header for details."
#endif

#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
