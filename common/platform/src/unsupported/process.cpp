// SPDX-License-Identifier: GPL-3.0-or-later
// Lets a neutral build link. It is not a port (ADR-0008).
#include "sonora/platform/process.h"

namespace sonora::platform {

struct ChildProcess::Impl {};

std::string executable_dir() { return {}; }

std::unique_ptr<ChildProcess> ChildProcess::spawn(const std::vector<std::string>&, const Options&,
                                                  std::string& error) {
    error = "starting a child process is not available on this platform yet (ADR-0008 §6)";
    return nullptr;
}
ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ChildProcess::~ChildProcess() = default;
bool ChildProcess::running() { return false; }
bool ChildProcess::wait(int) { return true; }
void ChildProcess::terminate() {}
void ChildProcess::kill() {}
bool ChildProcess::exited_normally() const { return false; }
int ChildProcess::exit_code() const { return 0; }
int ChildProcess::signal_number() const { return 0; }

}  // namespace sonora::platform
