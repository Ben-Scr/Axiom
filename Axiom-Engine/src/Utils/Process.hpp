#pragma once

#include "Core/Export.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace Axiom::Process {

	struct AXIOM_API Result {
		int ExitCode = -1;
		std::string Output;
		bool TimedOut = false;

		bool Succeeded() const { return ExitCode == 0 && !TimedOut; }
	};

	// Run a child process and wait for it to finish.
	// `timeout`:
	//   • duration::zero()  — wait forever (legacy behavior; matches the
	//     original API for callers that don't pass a timeout).
	//   • > zero            — kill the child if it hasn't exited by the
	//     deadline. The returned Result has TimedOut=true and ExitCode set
	//     to a platform-specific termination code.
	AXIOM_API Result Run(const std::vector<std::string>& command,
		const std::filesystem::path& workingDirectory = {},
		std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

	AXIOM_API bool LaunchDetached(const std::vector<std::string>& command,
		const std::filesystem::path& workingDirectory = {});

} // namespace Axiom::Process
