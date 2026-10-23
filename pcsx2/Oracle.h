// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Error.h"

#include <string>
#include <string_view>

/// Oracle mode: a deterministic, headless run that is driven by a spec file (-oracle <spec.ini>).
/// Without -oracle nothing in here is active and PCSX2 behaves exactly as upstream.
namespace Oracle
{
	/// Sets the spec file path (from the command line) and enables oracle mode.
	void SetSpecPath(std::string path);

	/// Returns true if oracle mode was requested.
	bool IsEnabled();

	/// Process exit code of an oracle run: 0 if it succeeded, 1 if anything failed.
	int GetExitCode();

	/// Parses the spec, validates the effective settings and creates the output directory.
	/// Called on the CPU thread once the VM is initialised. On failure the error is set, the
	/// failure is recorded and false is returned.
	bool OnVMInitialized(Error* error);

	/// Checks the effective emulator settings against the determinism profile. Can be re-run later
	/// (e.g. once the game database has applied its settings). Sets the error and returns false on
	/// the first mismatch. Does not record the failure itself.
	bool CheckSettings(Error* error);

	/// Records a failure and requests that the VM (and the application) shut down. Thread-safe.
	/// Only the first reason is kept.
	void Fail(std::string_view reason);

	/// Called from VMManager::Shutdown, after the GS thread is idle. Writes status.json.
	void OnVMShutdown();

	/// Called by the host when the process is about to exit. Writes status.json if the VM never
	/// got as far as OnVMShutdown (e.g. a failure during boot).
	void OnProcessExit();
} // namespace Oracle
