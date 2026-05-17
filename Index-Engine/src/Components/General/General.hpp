#pragma once

// Group umbrella for "General" components — transform, name, identity, hierarchy,
// prefab linkage, and meta. These travel together in practice: any TU that
// touches one of them almost always touches several. Pulling this header has
// no transitive dependency on Graphics / Physics / Audio / Scripting.

#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/NewNativeComponent.hpp"
