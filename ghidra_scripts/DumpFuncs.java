// DumpFuncs.java -- headless helper for the XXL co-op mod.
//
// Usage: -postScript DumpFuncs.java <outFile> <cmd> [args...]
//
//   decomp  <addr> [addr...]        decompile each function containing addr
//   xrefs   <addr> [addr...]        list callers/references to each addr
//   scanimm <value> [value...]      find instructions using an immediate value
//   strings <substr>                find defined strings containing substr, with xrefs
//   imports                         list imported symbols by DLL
//   callees <addr>                  list functions called by the function at addr
//@category XXL

import java.io.PrintWriter;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class DumpFuncs extends GhidraScript {

	private PrintWriter out;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			println("DumpFuncs: need <outFile> <cmd> [args...]");
			return;
		}
		out = new PrintWriter(args[0]);
		try {
			// Several commands can be chained in one run, separated by ";" --
			// starting Ghidra headless costs ~30s, so batching matters.
			List<String> cur = new ArrayList<>();
			for (int i = 1; i <= args.length; i++) {
				// "SEP" separates commands; PowerShell eats "--" and cmd.exe
				// eats ";" before either can reach us.
				if (i == args.length || args[i].equals("SEP")) {
					if (!cur.isEmpty()) runOne(cur);
					cur.clear();
				} else {
					cur.add(args[i]);
				}
			}
		} finally {
			out.flush();
			out.close();
		}
	}

	private void runOne(List<String> argv) throws Exception {
		String cmd = argv.get(0);
		String[] rest = argv.subList(1, argv.size()).toArray(new String[0]);
		out.println("##### " + String.join(" ", argv) + " #####");
		switch (cmd) {
			case "decomp":  cmdDecomp(rest); break;
			case "xrefs":   cmdXrefs(rest); break;
			case "scanimm": cmdScanImm(rest); break;
			case "strings": cmdStrings(rest); break;
			case "imports": cmdImports(); break;
			case "callees": cmdCallees(rest); break;
			case "sym":     cmdSym(rest); break;
			case "vtbl":    cmdVtbl(rest); break;
			case "strtab":  cmdStrtab(rest); break;
			case "dis":     cmdDis(rest); break;
			case "vcall":   cmdVcall(rest); break;
			default: out.println("unknown command: " + cmd);
		}
		out.println();
	}

	// Dump `count` dwords starting at `addr` as a vtable: each slot with its
	// index, target address and the size of the function living there.
	private void cmdVtbl(String[] args) throws Exception {
		Address base = addr(args[0]);
		int count = args.length > 1 ? Integer.decode(args[1]) : 32;
		for (int i = 0; i < count; i++) {
			Address slot = base.add(i * 4L);
			long v;
			try {
				v = currentProgram.getMemory().getInt(slot) & 0xFFFFFFFFL;
			} catch (Exception e) {
				out.println("  [" + i + "] <unreadable>");
				break;
			}
			String desc = "";
			if (v >= 0x401000L && v < 0x621000L) {
				Address t = addr("0x" + Long.toHexString(v));
				Function f = getFunctionContaining(t);
				if (f == null) f = createFunction(t, null);
				desc = (f == null) ? "(no func)"
						: f.getName() + " size=" + f.getBody().getNumAddresses();
			}
			out.printf("  [%2d] +0x%02X  %08X  %s%n", i, i * 4, v, desc);
		}
	}

	// Find every indirect CALL through a vtable at a given displacement, e.g.
	// "vcall 0x34" locates all `CALL dword ptr [reg + 0x34]` -- the way to work
	// out who actually invokes a particular virtual slot.
	private void cmdVcall(String[] args) {
		long want = Long.decode(args[0]);
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		int hits = 0;
		while (it.hasNext() && hits < 400) {
			Instruction ins = it.next();
			if (!ins.getMnemonicString().equals("CALL")) continue;
			String s = ins.toString();
			// Only indirect calls through a register + displacement.
			if (!s.contains("[") || !s.contains("+")) continue;
			boolean match = false;
			for (int op = 0; op < ins.getNumOperands(); op++)
				for (Object o : ins.getOpObjects(op))
					if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() == want)
						match = true;
			if (!match) continue;
			Function f = getFunctionContaining(ins.getAddress());
			out.println(ins.getAddress() + "  " + s + "   in "
					+ (f == null ? "<none>" : f.getName() + "@" + f.getEntryPoint()));
			hits++;
		}
		out.println("(" + hits + " hits)");
	}

	// Raw disassembly over a range. Ghidra's function detection gives up in
	// parts of this binary, so sometimes reading the instructions is the only
	// way to find where a function actually starts.
	private void cmdDis(String[] args) {
		Address a = addr(args[0]);
		int len = args.length > 1 ? Integer.decode(args[1]) : 0x100;
		Address end = a.add(len);
		Listing lst = currentProgram.getListing();
		while (a.compareTo(end) < 0) {
			Instruction ins = lst.getInstructionAt(a);
			if (ins == null) {
				out.println("  " + a + "  <no instruction>");
				a = a.add(1);
				continue;
			}
			Function f = getFunctionContaining(a);
			String mark = (f != null && f.getEntryPoint().equals(a)) ? " <<< " + f.getName() : "";
			out.println("  " + a + "  " + ins + mark);
			a = a.add(ins.getLength());
		}
	}

	// Read `count` char* entries starting at `addr` and print what they point at.
	private void cmdStrtab(String[] args) {
		Address base = addr(args[0]);
		int count = args.length > 1 ? Integer.decode(args[1]) : 16;
		for (int i = 0; i < count; i++) {
			try {
				long p = currentProgram.getMemory().getInt(base.add(i * 4L)) & 0xFFFFFFFFL;
				String s = "";
				if (p >= 0x400000L && p < 0x700000L) {
					Address sa = addr("0x" + Long.toHexString(p));
					StringBuilder sb = new StringBuilder();
					for (int k = 0; k < 128; k++) {
						int c = currentProgram.getMemory().getByte(sa.add(k)) & 0xFF;
						if (c == 0) break;
						sb.append((char) c);
					}
					s = "\"" + sb + "\"";
				}
				out.printf("  [%2d] %08X  %s%n", i, p, s);
			} catch (Exception e) {
				out.println("  [" + i + "] <unreadable>");
				break;
			}
		}
	}

	// Locate symbols (imports, thunks, labels) whose name contains a substring,
	// and list every reference to each -- the way to find e.g. all the code that
	// touches DirectInput8Create.
	private void cmdSym(String[] args) {
		String needle = args.length > 0 ? args[0].toLowerCase() : "";
		SymbolTable st = currentProgram.getSymbolTable();
		ReferenceManager rm = currentProgram.getReferenceManager();
		SymbolIterator it = st.getAllSymbols(true);
		int hits = 0;
		while (it.hasNext() && hits < 200) {
			Symbol s = it.next();
			if (!s.getName().toLowerCase().contains(needle)) continue;
			hits++;
			out.println(s.getAddress() + "  " + s.getParentNamespace().getName()
					+ "!" + s.getName() + "  [" + s.getSymbolType() + "]");
			int n = 0;
			for (Reference r : rm.getReferencesTo(s.getAddress())) {
				if (++n > 40) { out.println("      ..."); break; }
				Function f = getFunctionContaining(r.getFromAddress());
				out.println("      <- " + r.getFromAddress() + " " + r.getReferenceType()
						+ " in " + (f == null ? "<none>" : f.getName() + "@" + f.getEntryPoint()));
			}
		}
		out.println("(" + hits + " symbols)");
	}

	private Address addr(String s) {
		return currentProgram.getAddressFactory().getDefaultAddressSpace()
				.getAddress(Long.decode(s));
	}

	// ---------------------------------------------------------------- decomp

	private void cmdDecomp(String[] args) throws Exception {
		DecompInterface di = new DecompInterface();
		DecompileOptions opts = new DecompileOptions();
		di.setOptions(opts);
		di.openProgram(currentProgram);
		for (String a : args) {
			Address ad = addr(a);
			Function f = getFunctionContaining(ad);
			if (f == null) {
				// Ghidra may not have made a function here; try to create one.
				f = createFunction(ad, null);
			}
			if (f == null) {
				out.println("=== " + a + ": NO FUNCTION ===");
				disassembleRange(ad, 0x60);
				continue;
			}
			out.println("=== " + f.getName() + " @ " + f.getEntryPoint()
					+ "  (" + f.getCallingConventionName() + ", body "
					+ f.getBody().getNumAddresses() + " bytes) ===");
			DecompileResults res = di.decompileFunction(f, 90, monitor);
			if (res != null && res.decompileCompleted()) {
				out.println(res.getDecompiledFunction().getC());
			} else {
				out.println("// decompilation failed: "
						+ (res == null ? "null" : res.getErrorMessage()));
				disassembleRange(f.getEntryPoint(), 0x80);
			}
			out.println();
		}
		di.dispose();
	}

	private void disassembleRange(Address start, int len) {
		Listing lst = currentProgram.getListing();
		Address a = start;
		Address end = start.add(len);
		while (a.compareTo(end) < 0) {
			Instruction ins = lst.getInstructionAt(a);
			if (ins == null) break;
			out.println("  " + a + "  " + ins);
			a = a.add(ins.getLength());
		}
	}

	// ----------------------------------------------------------------- xrefs

	private void cmdXrefs(String[] args) {
		ReferenceManager rm = currentProgram.getReferenceManager();
		for (String a : args) {
			Address ad = addr(a);
			out.println("=== refs to " + a + " ===");
			for (Reference r : rm.getReferencesTo(ad)) {
				Address from = r.getFromAddress();
				Function ff = getFunctionContaining(from);
				out.println("  " + from + "  " + r.getReferenceType()
						+ "  in " + (ff == null ? "<none>" : ff.getName() + "@" + ff.getEntryPoint())
						+ "   " + insnAt(from));
			}
			out.println();
		}
	}

	private String insnAt(Address a) {
		Instruction i = currentProgram.getListing().getInstructionAt(a);
		return i == null ? "" : i.toString();
	}

	// --------------------------------------------------------------- scanimm

	private void cmdScanImm(String[] args) {
		Set<Long> wanted = new HashSet<>();
		for (String a : args) wanted.add(Long.decode(a));
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		int hits = 0;
		while (it.hasNext() && hits < 4000) {
			Instruction ins = it.next();
			for (int op = 0; op < ins.getNumOperands(); op++) {
				for (Object o : ins.getOpObjects(op)) {
					if (o instanceof Scalar) {
						long v = ((Scalar) o).getUnsignedValue();
						if (wanted.contains(v)) {
							Function f = getFunctionContaining(ins.getAddress());
							out.println(ins.getAddress() + "  " + ins + "   in "
									+ (f == null ? "<none>" : f.getName() + "@" + f.getEntryPoint()));
							hits++;
						}
					}
				}
			}
		}
		out.println("(" + hits + " hits)");
	}

	// --------------------------------------------------------------- strings

	private void cmdStrings(String[] args) {
		String needle = args.length > 0 ? args[0].toLowerCase() : "";
		ReferenceManager rm = currentProgram.getReferenceManager();
		DataIterator di = currentProgram.getListing().getDefinedData(true);
		int hits = 0;
		while (di.hasNext() && hits < 800) {
			Data d = di.next();
			if (!(d.getValue() instanceof String)) continue;
			String s = (String) d.getValue();
			if (!s.toLowerCase().contains(needle)) continue;
			hits++;
			out.println(d.getAddress() + "  \"" + s.replace("\n", "\\n") + "\"");
			for (Reference r : rm.getReferencesTo(d.getAddress())) {
				Function f = getFunctionContaining(r.getFromAddress());
				out.println("      <- " + r.getFromAddress() + " in "
						+ (f == null ? "<none>" : f.getName() + "@" + f.getEntryPoint()));
			}
		}
		out.println("(" + hits + " strings)");
	}

	// --------------------------------------------------------------- imports

	private void cmdImports() {
		SymbolTable st = currentProgram.getSymbolTable();
		for (Symbol s : st.getExternalSymbols()) {
			out.println(s.getParentNamespace().getName() + "!" + s.getName());
		}
		out.println("--- memory blocks ---");
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			out.println(b.getName() + "  " + b.getStart() + "-" + b.getEnd()
					+ "  " + (b.isExecute() ? "X" : "-") + (b.isWrite() ? "W" : "-"));
		}
	}

	// --------------------------------------------------------------- callees

	private void cmdCallees(String[] args) {
		for (String a : args) {
			Function f = getFunctionContaining(addr(a));
			if (f == null) { out.println(a + ": no function"); continue; }
			out.println("=== callees of " + f.getName() + "@" + f.getEntryPoint() + " ===");
			for (Function c : f.getCalledFunctions(monitor)) {
				out.println("  " + c.getEntryPoint() + "  " + c.getName());
			}
			out.println("=== callers ===");
			for (Function c : f.getCallingFunctions(monitor)) {
				out.println("  " + c.getEntryPoint() + "  " + c.getName());
			}
			out.println();
		}
	}
}
