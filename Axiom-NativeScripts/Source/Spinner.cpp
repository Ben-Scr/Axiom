#include <Components/General/Transform2DComponent.hpp>
#include <Scripting/NativeScript.hpp>

class Spinner : public Axiom::NativeScript {
public:
	void Update(float dt) override {
		auto& transform = GetComponent<Axiom::Transform2DComponent>();
		transform.SetRotation(transform.Rotation + 3.14159f * dt);
	}
};

REGISTER_SCRIPT(Spinner)
