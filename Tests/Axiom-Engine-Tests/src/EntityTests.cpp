#include <doctest/doctest.h>

#include "Components/General/NameComponent.hpp"
#include "Components/Tags.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"

#include <string>

using namespace Axiom;

TEST_CASE("Entity null component access is guarded")
{
	Entity entity = Entity::Null;
	CHECK_FALSE(entity.IsValid());
	CHECK_FALSE(entity.HasComponent<NameComponent>());
	CHECK_FALSE(entity.HasAnyComponent<NameComponent>());

	NameComponent* name = nullptr;
	CHECK_FALSE(entity.TryGetComponent(name));
	CHECK(name == nullptr);
	CHECK_NOTHROW(entity.RemoveComponent<NameComponent>());

	CHECK_THROWS(entity.GetComponent<NameComponent>());
	CHECK_THROWS(entity.AddComponent<NameComponent>(std::string("Name")));

	const Entity constEntity = Entity::Null;
	CHECK_THROWS(constEntity.GetComponent<NameComponent>());
	CHECK(constEntity.GetName().find("Unnamed Entity") == 0);
}

TEST_CASE("Parent enable preserves child authored disabled state")
{
	auto scene = Scene::CreateDetachedScene("Disabled Hierarchy Test");

	Entity parent = scene->CreateEntity("Parent");
	Entity child = scene->CreateEntity("Child");
	child.SetParent(parent);
	child.SetEnabled(false);

	parent.SetEnabled(false);
	parent.SetEnabled(true);

	CHECK_FALSE(parent.HasComponent<DisabledTag>());
	CHECK(child.HasComponent<DisabledTag>());
	CHECK_FALSE(child.HasComponent<InheritedDisabledTag>());
}

TEST_CASE("Parent enable clears inherited disabled state")
{
	auto scene = Scene::CreateDetachedScene("Inherited Disabled Test");

	Entity parent = scene->CreateEntity("Parent");
	Entity child = scene->CreateEntity("Child");
	child.SetParent(parent);

	parent.SetEnabled(false);
	CHECK(child.HasComponent<DisabledTag>());
	CHECK(child.HasComponent<InheritedDisabledTag>());

	parent.SetEnabled(true);
	CHECK_FALSE(parent.HasComponent<DisabledTag>());
	CHECK_FALSE(child.HasComponent<DisabledTag>());
	CHECK_FALSE(child.HasComponent<InheritedDisabledTag>());
}
