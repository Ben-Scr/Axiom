#include <Components/General/Transform2DComponent.hpp>
#include <Scripting/NativeScript.hpp>
#include <cmath>

class FloatingMotion : public Axiom::NativeScript {
public:
	void Start() override { m_Time = 0.0f; }

	void Update(float dt) override {
		m_Time += dt;
		auto& transform = GetComponent<Axiom::Transform2DComponent>();
		// Keep X, drive Y by a sine — sample of script-driven motion.
		// SetPosition (rather than direct field assignment) keeps physics
		// in sync via the component's dirty flag.
		transform.SetPosition({ transform.Position.x, std::sin(m_Time * 2.0f) * 1.5f });
	}

private:
	float m_Time = 0.0f;
};

REGISTER_SCRIPT(FloatingMotion)
