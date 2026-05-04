#include "pch.hpp"
#include "Core/UUID.hpp"

#include <random>

namespace Axiom {

	thread_local std::mt19937_64 t_Engine{ std::random_device{}() };
	thread_local std::uniform_int_distribution<uint64_t> t_Distribution;

	thread_local std::mt19937 t_Engine32{ std::random_device{}() };
	thread_local std::uniform_int_distribution<uint32_t> t_Distribution32;

	UUID::UUID()
		: m_UUID(t_Distribution(t_Engine))
	{
	}

	UUID::UUID(uint64_t uuid)
		: m_UUID(uuid)
	{
	}

	UUID::UUID(const UUID& other)
		: m_UUID(other.m_UUID)
	{
	}


	UUID32::UUID32()
		: m_UUID(t_Distribution32(t_Engine32))
	{
	}

	UUID32::UUID32(uint32_t uuid)
		: m_UUID(uuid)
	{
	}

	UUID32::UUID32(const UUID32& other)
		: m_UUID(other.m_UUID)
	{
	}

}