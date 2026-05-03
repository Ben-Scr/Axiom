#include <Components/General/Transform2DComponent.hpp>
#include <Scripting/NativeScript.hpp>

class NewNativeScript : public Axiom::NativeScript {
public:
	void Start() override
	{
		AIM_NATIVE_LOG_INFO("NewNativeScript started!");
	}

	void Update(float dt) override
	{
		auto& transform = GetComponent<Axiom::Transform2DComponent>();
		transform.SetPosition({ transform.Position.x + 5.0f * dt, transform.Position.y });
	}
};
REGISTER_SCRIPT(NewNativeScript)
