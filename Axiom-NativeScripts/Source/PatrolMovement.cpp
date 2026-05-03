#include <Components/General/Transform2DComponent.hpp>
#include <Scripting/NativeScript.hpp>

class PatrolMovement : public Axiom::NativeScript {
public:
	void Update(float dt) override {
		auto& transform = GetComponent<Axiom::Transform2DComponent>();
		m_Distance += 3.0f * dt * m_Dir;
		if (m_Distance > 5.0f || m_Distance < -5.0f) {
			m_Dir *= -1.0f;
		}
		transform.SetPosition({ transform.Position.x + 3.0f * dt * m_Dir, transform.Position.y });
	}

private:
	float m_Distance = 0.0f;
	float m_Dir = 1.0f;
};

REGISTER_SCRIPT(PatrolMovement)
