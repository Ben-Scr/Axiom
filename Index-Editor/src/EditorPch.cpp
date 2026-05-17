// PCH translation unit for Index-Editor. Pairs with `pchheader "pch.hpp"` +
// `pchsource "src/EditorPch.cpp"` in Index-Editor/premake5.lua. The header
// `pch.hpp` is the engine's PCH (Index-Engine/src/pch.hpp), reached via the
// engine include path. Compiling it once here lets every editor TU consume
// the precompiled blob instead of re-parsing the full STL + magic_enum +
// ImGui surface on every translation unit — without this, monolithic TUs
// like Gui/ComponentInspectors.cpp exhaust the compiler heap (C1060).

#include "pch.hpp"
