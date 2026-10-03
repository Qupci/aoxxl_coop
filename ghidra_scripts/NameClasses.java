// Names every Kal engine class's vtable and virtual methods, plus the functions and
// globals identified so far in coop/docs/RESEARCH.md.
//
// Every engine class derives from KClass, whose vtable slot 3 is getClassGroup and
// slot 4 getClassID. Both are tiny `mov eax, imm32 / ret` stubs, so a vtable is any
// run of code pointers in .rdata whose slots 3 and 4 point at such stubs, and the two
// immediates are the class's (group, id). elbtools' classes_ax1.txt maps that pair
// to the class name.
//
// Usage (headless, NOT -readOnly so the names are saved):
//   -postScript NameClasses.java <classes_ax1.txt> <report.txt>
//@category XXL

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class NameClasses extends GhidraScript {

	private Memory mem;
	private long textStart, textEnd;

	private static final String[] KCLASS_SLOTS = {
		"dtor", "isSubclass", "reset", "getClassGroup", "getClassID",
		"sendEvent", "vf06", "vf07", "serialize", "deserialize"
	};

	private long u32(long va) throws Exception {
		return mem.getInt(toAddr(va)) & 0xFFFFFFFFL;
	}

	private boolean isText(long v) {
		return v >= textStart && v < textEnd;
	}

	// imm32 if the bytes at va are `B8 imm32 C3`, else -1.
	private long stubValue(long va) {
		try {
			if ((mem.getByte(toAddr(va)) & 0xFF) != 0xB8) return -1;
			if ((mem.getByte(toAddr(va + 5)) & 0xFF) != 0xC3) return -1;
			return mem.getInt(toAddr(va + 1)) & 0xFFFFFFFFL;
		} catch (Exception e) {
			return -1;
		}
	}

	private Function ensureFunction(long va) {
		Address a = toAddr(va);
		Function f = getFunctionAt(a);
		if (f == null) {
			try { f = createFunction(a, null); } catch (Exception e) { f = null; }
		}
		return f;
	}

	private boolean hasDefaultName(Function f) {
		String n = f.getName();
		return n.startsWith("FUN_") || n.startsWith("thunk_FUN_");
	}

	private Namespace ns(String name) throws Exception {
		SymbolTable st = currentProgram.getSymbolTable();
		return st.getOrCreateNameSpace(currentProgram.getGlobalNamespace(), name, SourceType.USER_DEFINED);
	}

	private void rename(long va, String nsName, String name, boolean force) {
		try {
			Function f = ensureFunction(va);
			if (f == null) return;
			if (!force && !hasDefaultName(f)) return;
			if (nsName != null) f.setParentNamespace(ns(nsName));
			else f.setParentNamespace(currentProgram.getGlobalNamespace());
			f.setName(name, SourceType.USER_DEFINED);
		} catch (Exception e) {
			println("rename failed at " + Long.toHexString(va) + ": " + e);
		}
	}

	private void label(long va, String name) {
		try {
			createLabel(toAddr(va), name, true, SourceType.USER_DEFINED);
		} catch (Exception e) {
			println("label failed at " + Long.toHexString(va) + ": " + e);
		}
	}

	@Override
	public void run() throws Exception {
		mem = currentProgram.getMemory();
		String[] args = getScriptArgs();
		String classFile = args[0];
		String reportFile = args.length > 1 ? args[1] : null;

		MemoryBlock tb = mem.getBlock(".text");
		MemoryBlock rb = mem.getBlock(".rdata");
		textStart = tb.getStart().getOffset();
		textEnd = tb.getEnd().getOffset() + 1;
		long rdataStart = rb.getStart().getOffset();
		long rdataEnd = rb.getEnd().getOffset() + 1;

		Map<Long, String> names = new HashMap<>();
		try (BufferedReader br = new BufferedReader(new FileReader(classFile))) {
			String line = br.readLine();  // header
			while ((line = br.readLine()) != null) {
				String[] p = line.trim().split("\\s+");
				if (p.length < 3) continue;
				try {
					names.put(((long) Integer.parseInt(p[0]) << 16) | Integer.parseInt(p[1]), p[2]);
				} catch (NumberFormatException e) { /* skip */ }
			}
		}
		// Abstract bases the registration table does not list (XXL-Editor CKGameX1.h).
		names.putIfAbsent((2L << 16) | 25, "CKHkHero");

		// --- Pass 1: find vtables -------------------------------------------------
		TreeMap<Long, String> vtables = new TreeMap<>();
		for (long a = rdataStart; a + 0x28 <= rdataEnd; a += 4) {
			boolean ok = true;
			for (int s = 0; s < 10 && ok; s++) ok = isText(u32(a + 4L * s));
			if (!ok) continue;
			long g = stubValue(u32(a + 0xC));
			long i = stubValue(u32(a + 0x10));
			if (g < 0 || g > 14 || i < 0 || i > 1000) continue;
			String cname = names.getOrDefault((g << 16) | i, "Class_" + g + "_" + i);
			vtables.put(a, cname + "|" + g + "|" + i);
		}

		// --- Pass 2: slot ownership -----------------------------------------------
		Map<Long, List<String>> refs = new LinkedHashMap<>();   // fn -> ["Class:slot", ...]
		Map<Long, Integer> vtLen = new HashMap<>();
		List<Long> starts = new ArrayList<>(vtables.keySet());
		for (int k = 0; k < starts.size(); k++) {
			long a = starts.get(k);
			long limit = (k + 1 < starts.size()) ? starts.get(k + 1) : rdataEnd;
			String cname = vtables.get(a).split("\\|")[0];
			int n = 0;
			for (long p = a; p < limit && n < 512; p += 4, n++) {
				long v = u32(p);
				if (!isText(v)) break;
				refs.computeIfAbsent(v, x -> new ArrayList<>()).add(cname + ":" + n);
			}
			vtLen.put(a, n);
			label(a, cname + "_vftable");
		}

		// --- Pass 3: name methods ------------------------------------------------
		int named = 0, shared = 0;
		for (Map.Entry<Long, List<String>> e : refs.entrySet()) {
			long fn = e.getKey();
			List<String> r = e.getValue();
			String[] first = r.get(0).split(":");
			int slot = Integer.parseInt(first[1]);
			String slotName = slot < KCLASS_SLOTS.length ? KCLASS_SLOTS[slot] : String.format("vf%02d", slot);
			if (r.size() == 1) {
				rename(fn, first[0], slotName, false);
				named++;
			} else {
				// Base-class implementation inherited by several classes: name it after
				// the first owner, flag it as shared, and list every owner in a comment.
				rename(fn, null, "Shared_" + first[0] + "_" + slotName + "_x" + r.size(), false);
				shared++;
			}
			try {
				Function f = getFunctionAt(toAddr(fn));
				if (f != null) {
					StringBuilder sb = new StringBuilder("vtable slots: ");
					for (int j = 0; j < r.size() && j < 40; j++) sb.append(j > 0 ? ", " : "").append(r.get(j));
					if (r.size() > 40) sb.append(", ... (" + r.size() + " total)");
					f.setComment(sb.toString());
				}
			} catch (Exception ex) { /* ignore */ }
		}

		// --- Pass 4: hero Life state handlers (slots 24..33 = Life states 0..9) -----
		for (Long a : starts) {
			String cname = vtables.get(a).split("\\|")[0];
			if (!cname.equals("CKHkAsterixLife") && !cname.equals("CKHkObelixLife")
					&& !cname.equals("CKHkIdefixLife")) continue;
			rename(u32(a + 17 * 4), cname, "Tick", true);
			rename(u32(a + 13 * 4), cname, "InitOnce", true);
			int len = vtLen.get(a);
			for (int st = 0; st < 10 && 24 + st < len; st++) {
				long fn = u32(a + (24L + st) * 4);
				if (refs.get(fn) != null && refs.get(fn).size() == 1)
					rename(fn, cname, "State" + st + ((st & 1) == 0 ? "_Player" : "_Ai"), true);
			}
		}

		// --- Pass 5: functions identified by hand (RESEARCH.md / session 2) ---------
		Object[][] known = {
			{0x47A240L, null, "Kal_RegisterClass"},
			{0x401000L, null, "Kal_Alloc"},
			{0x40D2C0L, null, "kfRefToPnt"},
			{0x44A1E0L, null, "Bits8_SetField"},
			{0x483CF0L, null, "Bits_SetField"},
			{0x4358B0L, null, "InputView_GetAction"},
			{0x435A50L, null, "CKInput_UpdateBinding"},
			{0x436360L, "CKInput", "Update"},
			{0x48D4F0L, null, "Hero_TickBodyMachine"},
			{0x48F720L, null, "Hero_ChooseBodyState"},
			{0x49DE10L, null, "Hero_TickAiMachine"},
			{0x49EE80L, null, "Hero_ChooseAiState"},
			{0x49C900L, null, "Hero_ChooseSituationState"},
			{0x48ABF0L, null, "Hero_ReadMoveAxes"},
			{0x48ACA0L, null, "Hero_ApplyMoveAxes"},
			{0x499660L, null, "Hero_ProcessPlayerInput"},
			{0x49A350L, null, "Hero_ReadCommandBlock"},
			{0x4A1BF0L, null, "Hero_PreLifeTick"},
			{0x4A2190L, null, "Hero_PostLifeTick"},
			{0x4A3230L, null, "CKHkAsterix_InitStateTables"},
			{0x482BB0L, null, "CKGrpTrio_OnEvent48xx"},
			{0x481880L, null, "CKGrpTrio_SwitchLeaderFollowup"},
			{0x4815C0L, null, "CKGrpTrio_BuildMoveBasis"},
			{0x413FF0L, null, "Render_View"},
			{0x414110L, null, "Render_Scene"},
			{0x414B70L, null, "Render_CalcProjCamera"},
			{0x4144A0L, null, "Render_Init"},
			{0x58FD30L, null, "RwCameraBeginUpdate"},
			{0x58FD20L, null, "RwCameraEndUpdate"},
			{0x58FFB0L, null, "RwCameraSetViewWindow"},
			{0x58FD40L, null, "RwCameraSetViewOffset"},
			{0x58FD70L, null, "RwCameraSetNearClipPlane"},
			{0x58FE40L, null, "RwCameraSetFarClipPlane"},
			{0x58FF40L, null, "RwCameraSetProjection"},
			{0x590060L, null, "RwCameraCreate"},
			{0x515CB0L, null, "CKHook_GoVanish"},
			{0x54BE20L, null, "CKGrpBonus_SpawnBonus"},
			{0x54E2C0L, null, "CKHkBasicBonus_FollowHero"},
			{0x54DF10L, null, "CKHkBasicBonus_Fall"},
			{0x5354B0L, null, "AsterixBomb_Spawn"},
			{0x459050L, null, "Node_FadeVisibility"},
			{0x479C10L, null, "Game_WndProc"},
		};
		for (Object[] k : known) rename((Long) k[0], (String) k[1], (String) k[2], true);

		Object[][] globals = {
			{0x6621F4L, "g_pYellowPages"},
			{0x665310L, "g_gameStartInfo"},
			{0x662EB4L, "g_renderFrameBufferObj"},
			{0x662EB8L, "g_mainRwCamera"},
			{0x662EBCL, "g_hudRwCamera"},
			{0x64CBD8L, "g_aiOrderNames"},
		};
		for (Object[] g : globals) label((Long) g[0], (String) g[1]);

		println(String.format("NameClasses: %d vtables, %d methods named, %d shared", vtables.size(), named, shared));
		if (reportFile != null) {
			try (PrintWriter w = new PrintWriter(reportFile)) {
				w.println("# vtable\tclass\tgroup\tid\tslots");
				for (Long a : starts) {
					String[] p = vtables.get(a).split("\\|");
					w.println(String.format("0x%08X\t%s\t%s\t%s\t%d", a, p[0], p[1], p[2], vtLen.get(a)));
				}
			}
		}
	}
}
