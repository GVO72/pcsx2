// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Oracle.h"

#include "Config.h"
#include "DebugTools/Debug.h"
#include "Counters.h"
#include "Host.h"
#include "IopMem.h"
#include "ImGui/FullscreenUI.h"
#include "INISettingsInterface.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "VU.h"
#include "VUmicro.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace Oracle
{
	namespace
	{
		struct Spec
		{
			int version = 0;
			std::string output_dir;
			int end_frame = 0;
			std::vector<int> checkpoints;
			bool record = false;
			int canvas_width = 0;
			int canvas_height = 0;
			int max_boot_vsyncs = 0;
			std::string expect_elf;
		};
	} // namespace

	static constexpr const char* SECTION = "Oracle";

	static std::string s_spec_path;
	static std::atomic_bool s_enabled{false};
	static Spec s_spec;

	// Set (once) as soon as OutputDir is known to be usable; guarded by s_mutex.
	static std::string s_output_dir;

	static std::mutex s_mutex;
	static std::string s_fail_reason; // guarded by s_mutex
	static std::atomic_bool s_failed{false};
	static bool s_status_written = false; // guarded by s_mutex

	// Frame counting, all on the CPU thread. Frame 0 is the first vsync after the boot ELF's entry
	// point; before that, vsyncs only count against MaxBootVsyncs.
	static bool s_anchored = false;
	static int s_boot_vsyncs = 0;
	static int s_next_frame = 0;
	static std::string s_elf_log; // elf.tsv: frame, entry point, path
	static std::atomic_bool s_end_frame_reached{false};

	// Guest console capture. Written to ee-console.txt, iop-console.txt and console.tsv at shutdown.
	// Guarded by s_console_mutex: the writers run on the CPU thread, but this does not rely on it.
	namespace
	{
		struct ConsoleSource
		{
			const char* name; // console.tsv source column
			std::string text; // the whole console output
			std::string line; // current incomplete line
		};
	} // namespace

	static std::mutex s_console_mutex;
	static bool s_console_capture = false;
	static bool s_console_written = false;
	static ConsoleSource s_console_ee{"ee"};
	static ConsoleSource s_console_iop{"iop"};
	static std::string s_console_tsv;
	static bool s_console_unknown_warned = false;

	// Records the failure without requesting a shutdown. Keeps the first reason only.
	static void RecordFailure(std::string_view reason)
	{
		std::lock_guard lock(s_mutex);
		if (!s_failed.load(std::memory_order_relaxed))
		{
			s_fail_reason = std::string(reason);
			s_failed.store(true, std::memory_order_release);
		}
	}

	static bool SetFailure(Error* error, std::string message)
	{
		Console.Error("Oracle: FAIL: %s", message.c_str());
		RecordFailure(message);
		Error::SetString(error, std::move(message));
		return false;
	}

	void SetSpecPath(std::string path)
	{
		s_spec_path = std::move(path);
		s_enabled.store(true, std::memory_order_release);
	}

	bool IsEnabled()
	{
		return s_enabled.load(std::memory_order_acquire);
	}

	int GetExitCode()
	{
		return s_failed.load(std::memory_order_acquire) ? 1 : 0;
	}

	static bool ParseIntKey(INISettingsInterface& si, const char* key, bool required, int* value, std::string* err)
	{
		if (!si.ContainsValue(SECTION, key))
		{
			if (required)
				*err = fmt::format("spec: missing required key '{}'", key);
			return !required;
		}
		if (!si.GetIntValue(SECTION, key, value))
		{
			*err = fmt::format("spec: key '{}' is not an integer", key);
			return false;
		}
		return true;
	}

	// Parses "a,b,c" (may be empty) into ints.
	static bool ParseIntList(const std::string& str, std::vector<int>* out, std::string* err)
	{
		const std::string_view stripped = StringUtil::StripWhitespace(str);
		if (stripped.empty())
			return true;

		for (const std::string_view item : StringUtil::SplitString(stripped, ',', false))
		{
			const std::optional<int> v = StringUtil::FromChars<int>(StringUtil::StripWhitespace(item), 10);
			if (!v.has_value())
			{
				*err = fmt::format("spec: key 'Checkpoints' has an invalid entry '{}'", item);
				return false;
			}
			out->push_back(v.value());
		}
		return true;
	}

	static bool ParseSpec(const std::string& path, Spec* spec, std::string* err)
	{
		if (path.empty() || !FileSystem::FileExists(path.c_str()))
		{
			*err = fmt::format("spec: file '{}' does not exist", path);
			return false;
		}

		INISettingsInterface si(path);
		if (!si.Load())
		{
			*err = fmt::format("spec: failed to read '{}'", path);
			return false;
		}

		// INISettingsInterface cannot list sections, so use the INI reader it is built on.
		{
			CSimpleIniA ini(true, true);
			if (ini.LoadFile(path.c_str()) != SI_OK)
			{
				*err = fmt::format("spec: failed to read '{}'", path);
				return false;
			}
			CSimpleIniA::TNamesDepend sections;
			ini.GetAllSections(sections);
			for (const CSimpleIniA::Entry& section : sections)
			{
				if (std::string_view(section.pItem) != SECTION)
				{
					*err = fmt::format("spec: unknown section '[{}]'", section.pItem);
					return false;
				}
			}
		}

		static constexpr const char* const known_keys[] = {"Version", "OutputDir", "EndFrame", "Checkpoints", "Record",
			"CanvasWidth", "CanvasHeight", "MaxBootVsyncs", "ExpectElf"};
		std::set<std::string> seen;
		for (const auto& [key, value] : si.GetKeyValueList(SECTION))
		{
			if (key.starts_with("Input"))
			{
				*err = fmt::format("spec: key '{}': input injection not implemented", key);
				return false;
			}
			if (std::find_if(std::begin(known_keys), std::end(known_keys),
					[&key](const char* k) { return key == k; }) == std::end(known_keys))
			{
				*err = fmt::format("spec: unknown key '{}'", key);
				return false;
			}
			if (!seen.insert(key).second)
			{
				*err = fmt::format("spec: duplicate key '{}'", key);
				return false;
			}
		}

		if (!ParseIntKey(si, "Version", true, &spec->version, err))
			return false;
		if (spec->version != 1)
		{
			*err = fmt::format("spec: unsupported Version {} (expected 1)", spec->version);
			return false;
		}

		if (!si.GetStringValue(SECTION, "OutputDir", &spec->output_dir) || spec->output_dir.empty())
		{
			*err = "spec: missing required key 'OutputDir'";
			return false;
		}
		if (!Path::IsAbsolute(spec->output_dir))
		{
			*err = fmt::format("spec: OutputDir '{}' is not an absolute path", spec->output_dir);
			return false;
		}
		Error dir_error;
		if (!FileSystem::EnsureDirectoryExists(spec->output_dir.c_str(), true, &dir_error))
		{
			*err = fmt::format("spec: cannot create OutputDir '{}': {}", spec->output_dir, dir_error.GetDescription());
			return false;
		}
		{
			std::lock_guard lock(s_mutex);
			s_output_dir = spec->output_dir; // from here on status.json can be written
		}

		if (!ParseIntKey(si, "EndFrame", true, &spec->end_frame, err))
			return false;
		if (spec->end_frame < 1)
		{
			*err = fmt::format("spec: EndFrame {} must be >= 1", spec->end_frame);
			return false;
		}

		std::string checkpoints;
		if (si.ContainsValue(SECTION, "Checkpoints"))
		{
			si.GetStringValue(SECTION, "Checkpoints", &checkpoints);
			if (!ParseIntList(checkpoints, &spec->checkpoints, err))
				return false;
		}
		for (size_t i = 0; i < spec->checkpoints.size(); i++)
		{
			const int cp = spec->checkpoints[i];
			if (cp < 0 || cp > spec->end_frame)
			{
				*err = fmt::format("spec: checkpoint {} is outside 0..EndFrame ({})", cp, spec->end_frame);
				return false;
			}
			if (i > 0 && cp <= spec->checkpoints[i - 1])
			{
				*err = fmt::format("spec: Checkpoints must be strictly increasing ({} after {})", cp, spec->checkpoints[i - 1]);
				return false;
			}
		}

		if (si.ContainsValue(SECTION, "Record") && !si.GetBoolValue(SECTION, "Record", &spec->record))
		{
			*err = "spec: key 'Record' is not a boolean";
			return false;
		}

		if (!ParseIntKey(si, "CanvasWidth", true, &spec->canvas_width, err) ||
			!ParseIntKey(si, "CanvasHeight", true, &spec->canvas_height, err))
			return false;
		if (spec->canvas_width <= 0 || spec->canvas_height <= 0 || (spec->canvas_width % 8) != 0 ||
			(spec->canvas_height % 8) != 0)
		{
			*err = fmt::format("spec: canvas {}x{} must be positive multiples of 8", spec->canvas_width, spec->canvas_height);
			return false;
		}

		if (!ParseIntKey(si, "MaxBootVsyncs", true, &spec->max_boot_vsyncs, err))
			return false;
		if (spec->max_boot_vsyncs <= 0)
		{
			*err = fmt::format("spec: MaxBootVsyncs {} must be > 0", spec->max_boot_vsyncs);
			return false;
		}

		if (si.ContainsValue(SECTION, "ExpectElf"))
			si.GetStringValue(SECTION, "ExpectElf", &spec->expect_elf);

		return true;
	}

	bool CheckSettings(Error* error)
	{
		struct Check
		{
			const char* name;
			const char* expected;
			bool ok;
		};

		bool mcd_main_off = !EmuConfig.Mcd[0].Enabled && !EmuConfig.Mcd[1].Enabled;
		bool mcd_multitap_off = true;
		for (u32 slot = 2; slot < std::size(EmuConfig.Mcd); slot++)
			mcd_multitap_off = mcd_multitap_off && !EmuConfig.Mcd[slot].Enabled;

		const Check checks[] = {
			{"EmuCore/Speedhacks/vuThread (MTVU)", "false", !EmuConfig.Speedhacks.vuThread},
			{"EmuCore/GS/Renderer", "13 (software)", EmuConfig.GS.Renderer == GSRendererType::SW},
			{"EmuCore/EnableFastBoot", "true", EmuConfig.EnableFastBoot},
			{"EmuCore/ManuallySetRealTimeClock", "true", EmuConfig.ManuallySetRealTimeClock},
			{"EmuCore/EnableCheats", "false", !EmuConfig.EnableCheats},
			{"MemoryCards/Slot1_Enable and Slot2_Enable", "false", mcd_main_off},
			{"MemoryCards multitap slots 3-8", "disabled", mcd_multitap_off},
			{"EmuCore/SaveStateOnShutdown", "false", !EmuConfig.SaveStateOnShutdown},
			{"Logging/EnableEEConsole", "true", ConsoleLogging.eeConsole.IsActive()},
			{"Logging/EnableIOPConsole", "true", ConsoleLogging.iopConsole.IsActive()},
			{"SPU2/Output/Backend", "Null", EmuConfig.SPU2.Backend == AudioBackend::Null},
		};

		for (const Check& check : checks)
		{
			if (!check.ok)
			{
				Error::SetString(error, fmt::format("setting {} must be {}", check.name, check.expected));
				return false;
			}
		}
		return true;
	}

	bool OnVMInitialized(Error* error)
	{
		Console.WriteLn("Oracle: spec file '%s'", s_spec_path.c_str());

		std::string err;
		if (!ParseSpec(s_spec_path, &s_spec, &err))
			return SetFailure(error, std::move(err));

		Error settings_error;
		if (!CheckSettings(&settings_error))
			return SetFailure(error, fmt::format("settings: {}", settings_error.GetDescription()));

		const std::string state_dir = Path::Combine(s_spec.output_dir, "state");
		Error dir_error;
		if (!FileSystem::EnsureDirectoryExists(state_dir.c_str(), false, &dir_error))
			return SetFailure(error, fmt::format("cannot create '{}': {}", state_dir, dir_error.GetDescription()));

		{
			std::lock_guard lock(s_console_mutex);
			s_console_capture = true;
		}

		Console.WriteLn("Oracle: spec OK (EndFrame %d, %zu checkpoints, OutputDir '%s')", s_spec.end_frame,
			s_spec.checkpoints.size(), s_spec.output_dir.c_str());
		return true;
	}

	// Basename of an ELF path such as "cdrom0:\\HELLO.ELF;1" or "host:/dir/HELLO.ELF": the part after
	// the last separator, without an ISO9660 ";1" version suffix.
	static std::string_view ElfBasename(std::string_view path)
	{
		const size_t sep = path.find_last_of("\\/:");
		if (sep != std::string_view::npos)
			path.remove_prefix(sep + 1);
		const size_t version = path.find(';');
		if (version != std::string_view::npos)
			path = path.substr(0, version);
		return path;
	}

	static void WriteFile(const char* name, std::string_view contents)
	{
		const std::string path = Path::Combine(s_spec.output_dir, name);
		if (!FileSystem::WriteStringToFile(path.c_str(), contents))
			Fail(fmt::format("could not write '{}'", path));
	}

	// The effective configuration once the game database has applied its settings, in PCSX2.ini form.
	static void WriteEffectiveConfig()
	{
		const std::string path = Path::Combine(s_spec.output_dir, "effective-config.ini");
		INISettingsInterface si(path);
		Pcsx2Config config = EmuConfig;
		SettingsSaveWrapper wrapper(si);
		config.LoadSave(wrapper);
		if (!si.Save())
			Fail(fmt::format("could not write '{}'", path));
	}

	// ---- State dump -----------------------------------------------------------------------
	//
	// state/<frame>.bin, little endian, version 1:
	//   header (40 bytes): char magic[8] = "PS2PSTAT"; u32 version = 1; u32 section_count; u32 frame;
	//                      u32 flags = 0; u64 ee_cycle (cpuRegs.cycle); u64 iop_cycle (psxRegs.cycle)
	//   section table, section_count entries of { char tag[16] (NUL padded); u64 offset; u64 size },
	//   offsets from the start of the file;
	//   section data, each section starting at a 16-byte aligned offset, zero padded in between.
	// Section contents are the raw bytes of PCSX2's structures, in this order:
	//   EE.GPR     cpuRegs.GPR, 32 x 128 bit
	//   EE.HI      cpuRegs.HI, 128 bit        EE.LO  cpuRegs.LO, 128 bit
	//   EE.SA      cpuRegs.sa, u32            EE.PC  cpuRegs.pc, u32
	//   EE.COP0    cpuRegs.CP0, 32 x u32
	//   EE.PERF    cpuRegs.PERF (PERFregs, 16 bytes)
	//   EE.TLB     tlb[48] (tlbs: PageMask, EntryHi, EntryLo0, EntryLo1 as u32 each), as stored
	//   FPU.FPR    fpuRegs.fpr, 32 x u32      FPU.FCR  fpuRegs.fprc, 32 x u32
	//   FPU.ACC    fpuRegs.ACC (u32) then fpuRegs.ACCflag (u32)
	//   VU0.VF     VU0.VF, 32 x 128 bit       VU0.VI  low u32 of each VU0.VI entry, 32 x u32
	//   VU0.ACC    VU0.ACC, 128 bit
	//   VU0.QP     VU0.q then VU0.p, each a REG_VI as stored (16 bytes: u32 value + 12 bytes padding)
	//   VU0.MEM    VU0 data memory, 4 KB      VU0.MICRO  VU0 micro memory, 4 KB
	//   EE.RAM     first 32 MB of main memory (eeMem->Main)
	//   EE.SPR     scratchpad, 16 KB
	//   IOP.GPR    psxRegs.GPR.r, 34 x u32 (r[0..31], then r[32] = hi, r[33] = lo)
	//   IOP.COP0   psxRegs.CP0.r, 32 x u32
	//   IOP.PC     psxRegs.pc, u32
	//   IOP.RAM    first 2 MB of IOP main memory (iopMem->Main)
	static_assert(sizeof(GPR_reg) == 16 && sizeof(cpuRegs.GPR) == 32 * 16);
	static_assert(sizeof(cpuRegs.CP0) == 32 * 4 && sizeof(cpuRegs.PERF) == 16);
	static_assert(sizeof(tlbs) == 16 && sizeof(tlb) == 48 * 16);
	static_assert(sizeof(fpuRegs.fpr) == 32 * 4 && sizeof(fpuRegs.fprc) == 32 * 4);
	static_assert(sizeof(VECTOR) == 16 && sizeof(REG_VI) == 16 && sizeof(VU0.VF) == 32 * 16);
	static_assert(sizeof(psxRegs.GPR) == 34 * 4 && sizeof(psxRegs.CP0) == 32 * 4);
	static_assert(Ps2MemSize::MainRam == 32 * 1024 * 1024 && Ps2MemSize::IopRam == 2 * 1024 * 1024);
	static_assert(Ps2MemSize::Scratch == 16 * 1024 && VU0_MEMSIZE == 4096 && VU0_PROGSIZE == 4096);

	namespace
	{
		struct StateSection
		{
			char tag[16];
			const void* data;
			u64 size;
		};
	} // namespace

	static void PutU32(std::string& out, u32 v)
	{
		out.append(reinterpret_cast<const char*>(&v), sizeof(v));
	}

	static void PutU64(std::string& out, u64 v)
	{
		out.append(reinterpret_cast<const char*>(&v), sizeof(v));
	}

	static void WriteStateDump(int frame)
	{
		// Registers are in memory here: this runs inside cpuEventTest, which both recompilers only
		// reach at a block boundary, after the block has written its registers back.
		u32 vu0_vi[32];
		for (int i = 0; i < 32; i++)
			vu0_vi[i] = VU0.VI[i].UL;
		const u32 fpu_acc[2] = {fpuRegs.ACC.UL, fpuRegs.ACCflag};
		const u32 iop_pc = psxRegs.pc;

		const auto tagged = [](const char* tag, const void* data, u64 size) {
			StateSection section{};
			std::strncpy(section.tag, tag, sizeof(section.tag) - 1);
			section.data = data;
			section.size = size;
			return section;
		};
		const StateSection sections[] = {
			tagged("EE.GPR", &cpuRegs.GPR, sizeof(cpuRegs.GPR)),
			tagged("EE.HI", &cpuRegs.HI, sizeof(cpuRegs.HI)),
			tagged("EE.LO", &cpuRegs.LO, sizeof(cpuRegs.LO)),
			tagged("EE.SA", &cpuRegs.sa, sizeof(cpuRegs.sa)),
			tagged("EE.PC", &cpuRegs.pc, sizeof(cpuRegs.pc)),
			tagged("EE.COP0", &cpuRegs.CP0, sizeof(cpuRegs.CP0)),
			tagged("EE.PERF", &cpuRegs.PERF, sizeof(cpuRegs.PERF)),
			tagged("EE.TLB", tlb, sizeof(tlb)),
			tagged("FPU.FPR", fpuRegs.fpr, sizeof(fpuRegs.fpr)),
			tagged("FPU.FCR", fpuRegs.fprc, sizeof(fpuRegs.fprc)),
			tagged("FPU.ACC", fpu_acc, sizeof(fpu_acc)),
			tagged("VU0.VF", VU0.VF, sizeof(VU0.VF)),
			tagged("VU0.VI", vu0_vi, sizeof(vu0_vi)),
			tagged("VU0.ACC", &VU0.ACC, sizeof(VU0.ACC)),
			tagged("VU0.QP", &VU0.q, 2 * sizeof(REG_VI)), // q and p are adjacent members
			tagged("VU0.MEM", VU0.Mem, VU0_MEMSIZE),
			tagged("VU0.MICRO", VU0.Micro, VU0_PROGSIZE),
			tagged("EE.RAM", eeMem->Main, Ps2MemSize::MainRam),
			tagged("EE.SPR", eeMem->Scratch, Ps2MemSize::Scratch),
			tagged("IOP.GPR", &psxRegs.GPR, sizeof(psxRegs.GPR)),
			tagged("IOP.COP0", &psxRegs.CP0, sizeof(psxRegs.CP0)),
			tagged("IOP.PC", &iop_pc, sizeof(iop_pc)),
			tagged("IOP.RAM", iopMem->Main, Ps2MemSize::IopRam),
		};
		static_assert(offsetof(VURegs, p) == offsetof(VURegs, q) + sizeof(REG_VI));
		constexpr u64 section_count = std::size(sections);
		constexpr u64 header_size = 40;
		constexpr u64 entry_size = 32;

		std::string out;
		out.append("PS2PSTAT", 8);
		PutU32(out, 1);
		PutU32(out, static_cast<u32>(section_count));
		PutU32(out, static_cast<u32>(frame));
		PutU32(out, 0);
		PutU64(out, cpuRegs.cycle);
		PutU64(out, psxRegs.cycle);

		u64 offset = header_size + section_count * entry_size;
		for (const StateSection& section : sections)
		{
			offset = (offset + 15) & ~u64(15);
			out.append(section.tag, sizeof(section.tag));
			PutU64(out, offset);
			PutU64(out, section.size);
			offset += section.size;
		}
		for (const StateSection& section : sections)
		{
			out.resize((out.size() + 15) & ~size_t(15), '\0');
			out.append(static_cast<const char*>(section.data), section.size);
		}

		const std::string path = Path::Combine(Path::Combine(s_spec.output_dir, "state"), fmt::format("{}.bin", frame));
		const std::string tmp_path = path + ".tmp";
		Error error;
		if (!FileSystem::WriteStringToFile(tmp_path.c_str(), out) ||
			!FileSystem::RenamePath(tmp_path.c_str(), path.c_str(), &error))
		{
			Fail(fmt::format("could not write state dump '{}'", path));
			return;
		}
		Console.WriteLn("Oracle: wrote %s", path.c_str());
	}

	void OnEntryPoint(const std::string& elf_path, u32 entry_point)
	{
		if (s_failed.load(std::memory_order_acquire))
			return;

		s_elf_log += fmt::format("{}\t0x{:08X}\t{}\n", s_next_frame, entry_point, elf_path);
		if (s_anchored)
		{
			Console.WriteLn("Oracle: ELF %s started before frame %d", elf_path.c_str(), s_next_frame);
			return;
		}

		s_anchored = true;
		Console.WriteLn("Oracle: anchor: ELF %s, entry point 0x%08X, after %d boot vsyncs; frame 0 is the next vsync",
			elf_path.c_str(), entry_point, s_boot_vsyncs);
		if (!s_spec.expect_elf.empty() && !StringUtil::compareNoCase(ElfBasename(elf_path), s_spec.expect_elf))
			Fail(fmt::format("boot ELF is '{}', expected '{}'", elf_path, s_spec.expect_elf));
	}

	void OnVSyncCPU()
	{
		if (s_failed.load(std::memory_order_acquire) || s_end_frame_reached.load(std::memory_order_relaxed))
			return;

		if (!s_anchored)
		{
			if (++s_boot_vsyncs > s_spec.max_boot_vsyncs)
				Fail(fmt::format("the boot ELF's entry point was not reached within {} vsyncs", s_spec.max_boot_vsyncs));
			return;
		}

		const int frame = s_next_frame++;
		if (frame == 0)
		{
			// The game database has applied its settings by now (at the entry point), so check again.
			Error settings_error;
			if (!CheckSettings(&settings_error))
			{
				Fail(fmt::format("settings at frame 0: {}", settings_error.GetDescription()));
				return;
			}
			WriteEffectiveConfig();
			Console.WriteLn("Oracle: frame 0 (VM vsync count %u)", g_FrameCount);
		}

		if (std::binary_search(s_spec.checkpoints.begin(), s_spec.checkpoints.end(), frame))
		{
			Console.WriteLn("Oracle: checkpoint frame %d", frame);
			WriteStateDump(frame);
		}

		if (frame == s_spec.end_frame)
		{
			// Requesting the shutdown from the CPU thread only sets a flag: the rest of this vsync
			// still runs, so this frame reaches the GS thread before execution stops.
			s_end_frame_reached.store(true, std::memory_order_release);
			WriteFile("elf.tsv", s_elf_log);
			Console.WriteLn("Oracle: EndFrame %d reached, shutting down", frame);
			Host::RequestVMShutdown(false, false, false);
		}
	}

	// Escapes tab, CR, backslash and other control characters so that a console.tsv row stays one line.
	static std::string EscapeConsoleLine(std::string_view line)
	{
		std::string out;
		out.reserve(line.size());
		for (const char c : line)
		{
			const unsigned char u = static_cast<unsigned char>(c);
			if (c == '\t')
				out += "\\t";
			else if (c == '\r')
				out += "\\r";
			else if (c == '\\')
				out += "\\\\";
			else if (u < 0x20 || u == 0x7F)
				out += fmt::format("\\x{:02X}", u);
			else
				out += c;
		}
		return out;
	}

	// The frame column is -1 until the anchor, then the frame of the next vsync to be counted
	// (s_next_frame), the same numbering elf.tsv uses. Both are only written on the CPU thread.
	static void AppendConsoleRow(const ConsoleSource& src)
	{
		const int frame = s_anchored ? s_next_frame : -1;
		s_console_tsv += fmt::format("{}\t{}\t{}\n", frame, src.name, EscapeConsoleLine(src.line));
	}

	void OnVMConsole(std::string_view prefix, std::string_view text)
	{
		std::lock_guard lock(s_console_mutex);
		if (!s_console_capture)
			return;

		ConsoleSource* src;
		if (prefix == "EEout" || prefix == "DECI2")
			src = &s_console_ee;
		else if (prefix == "IOPout")
			src = &s_console_iop;
		else
		{
			if (!s_console_unknown_warned)
			{
				s_console_unknown_warned = true;
				Console.Warning("Oracle: ignoring console output with unknown prefix '%.*s'", static_cast<int>(prefix.size()),
					prefix.data());
			}
			return;
		}

		src->text.append(text);
		for (const char c : text)
		{
			if (c == '\n')
			{
				AppendConsoleRow(*src);
				src->line.clear();
			}
			else
				src->line.push_back(c);
		}
	}

	// Writes the console files, once; a trailing partial line gets a row too. Runs before status.json.
	static void WriteConsoleFiles()
	{
		std::string ee, iop, tsv;
		{
			std::lock_guard lock(s_console_mutex);
			if (s_console_written || !s_console_capture)
				return;
			s_console_written = true;
			s_console_capture = false;

			for (const ConsoleSource* src : {&s_console_ee, &s_console_iop})
			{
				if (!src->line.empty())
					AppendConsoleRow(*src);
			}
			ee = std::move(s_console_ee.text);
			iop = std::move(s_console_iop.text);
			tsv = std::move(s_console_tsv);
		}

		const struct
		{
			const char* name;
			const std::string& contents;
		} files[] = {{"ee-console.txt", ee}, {"iop-console.txt", iop}, {"console.tsv", tsv}};
		for (const auto& file : files)
		{
			const std::string path = Path::Combine(s_spec.output_dir, file.name);
			if (!FileSystem::WriteStringToFile(path.c_str(), file.contents))
			{
				Console.Error("Oracle: FAIL: could not write '%s'", path.c_str());
				RecordFailure(fmt::format("could not write '{}'", path));
			}
		}
	}

	void Fail(std::string_view reason)
	{
		Console.Error("Oracle: FAIL: %.*s", static_cast<int>(reason.size()), reason.data());
		RecordFailure(reason);

		// Both calls are safe from any thread: they marshal to the Qt UI thread (or, for
		// RequestVMShutdown on the CPU thread, shut the VM down directly and then queue the exit).
		// RequestVMShutdown does nothing when no VM is running, so in that case (failure during
		// boot, or while the VM is already stopping) ask the host to exit the application instead.
		if (VMManager::HasValidVM())
			Host::RequestVMShutdown(false, false, false);
		else
			Host::RequestExitApplication(false);
	}

	static std::string JsonEscape(std::string_view str)
	{
		std::string out;
		out.reserve(str.size() + 2);
		for (const char c : str)
		{
			switch (c)
			{
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
						out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
					else
						out += c;
					break;
			}
		}
		return out;
	}

	// Writes status.json once. Must be the last thing the oracle writes.
	static void WriteStatus()
	{
		if (!s_end_frame_reached.load(std::memory_order_acquire))
			RecordFailure("VM stopped before EndFrame was reached");

		WriteConsoleFiles(); // before status.json, which must be the last file written

		std::lock_guard lock(s_mutex);
		if (s_status_written)
			return;
		s_status_written = true;

		if (s_output_dir.empty())
		{
			Console.Error("Oracle: no usable OutputDir, status.json not written");
			return;
		}

		const bool failed = s_failed.load(std::memory_order_acquire);
		const std::string json = fmt::format("{{\"version\": 1, \"result\": \"{}\", \"reason\": \"{}\"}}\n",
			failed ? "failed" : "ok", JsonEscape(s_fail_reason));
		const std::string path = Path::Combine(s_output_dir, "status.json");
		if (!FileSystem::WriteStringToFile(path.c_str(), json))
		{
			// Without status.json the harness treats the run as failed; make the exit code agree.
			s_failed.store(true, std::memory_order_release);
			Console.Error("Oracle: FAIL: could not write '%s'", path.c_str());
		}
		else
			Console.WriteLn("Oracle: wrote %s (%s)", path.c_str(), failed ? "failed" : "ok");
	}

	void OnVMShutdown()
	{
		WriteStatus();
	}

	void OnProcessExit()
	{
		bool written;
		{
			std::lock_guard lock(s_mutex);
			written = s_status_written;
		}
		if (!written)
			WriteStatus();
	}
} // namespace Oracle
