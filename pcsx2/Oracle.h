// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Error.h"
#include "common/Pcsx2Types.h"

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

	/// Called on the CPU thread when an ELF's entry point is about to execute. The first call is
	/// the anchor: frame 0 is the next vsync. Later calls (ELF changes) are logged with their frame.
	void OnEntryPoint(const std::string& elf_path, u32 entry_point);

	/// Called on the CPU thread at every EE vsync, before the frame is handed to the GS thread.
	/// Counts boot vsyncs until the anchor, then oracle frames, and stops the VM after EndFrame.
	void OnVSyncCPU();

	/// Called for every piece of text the guest writes to its EE or IOP console (prefix "EEout",
	/// "DECI2" or "IOPout"), after PCSX2's character set conversion. Safe to call from any thread.
	void OnVMConsole(std::string_view prefix, std::string_view text);

	/// Records a failure and requests that the VM (and the application) shut down. Thread-safe.
	/// Only the first reason is kept.
	void Fail(std::string_view reason);

	/// Called from VMManager::Shutdown, after the GS thread is idle. Writes the console files, then status.json.
	void OnVMShutdown();

	/// Called by the host when the process is about to exit. Writes status.json if the VM never
	/// got as far as OnVMShutdown (e.g. a failure during boot).
	void OnProcessExit();
} // namespace Oracle
