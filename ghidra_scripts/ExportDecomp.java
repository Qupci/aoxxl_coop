// Decompiles every function in the program to greppable pseudo-C.
//
// Output (in <outDir>):
//   0xNNNN0000.c  - every function whose entry lies in that 64 KB region, in address
//                   order, each with a header giving name, size, callers and callees
//   index.tsv     - one row per function: address, name, size, callers, callees
//
// This is an analysis artefact for reading and grepping, not something to recompile.
//
// Usage (headless, -readOnly is fine):
//   -postScript ExportDecomp.java <outDir> [threads]
//@category XXL

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.util.task.TaskMonitor;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicInteger;

public class ExportDecomp extends GhidraScript {

	private static String names(Set<Function> fs, int max) {
		StringBuilder sb = new StringBuilder();
		int n = 0;
		for (Function f : fs) {
			if (n == max) { sb.append(", ...(").append(fs.size()).append(")"); break; }
			if (n++ > 0) sb.append(", ");
			sb.append(f.getName(true));
		}
		return sb.toString();
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args[0]);
		int threads = args.length > 1 ? Integer.parseInt(args[1]) : 4;
		outDir.mkdirs();

		List<Function> funcs = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) funcs.add(f);
		println("ExportDecomp: " + funcs.size() + " functions, " + threads + " threads");

		ConcurrentHashMap<Long, String> code = new ConcurrentHashMap<>();
		ConcurrentLinkedQueue<Function> queue = new ConcurrentLinkedQueue<>(funcs);
		AtomicInteger done = new AtomicInteger();
		ExecutorService pool = Executors.newFixedThreadPool(threads);
		List<Future<?>> futures = new ArrayList<>();
		for (int t = 0; t < threads; t++) {
			futures.add(pool.submit(() -> {
				DecompInterface di = new DecompInterface();
				di.setOptions(new DecompileOptions());
				di.toggleCCode(true);
				di.toggleSyntaxTree(false);
				di.setSimplificationStyle("decompile");
				di.openProgram(currentProgram);
				Function f;
				while ((f = queue.poll()) != null) {
					String c;
					try {
						DecompileResults r = di.decompileFunction(f, 90, TaskMonitor.DUMMY);
						if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null)
							c = r.getDecompiledFunction().getC();
						else
							c = "/* decompile failed: " + (r == null ? "null" : r.getErrorMessage()) + " */\n";
					} catch (Exception e) {
						c = "/* exception: " + e + " */\n";
					}
					code.put(f.getEntryPoint().getOffset(), c);
					int d = done.incrementAndGet();
					if (d % 1000 == 0) println("ExportDecomp: " + d + " done");
				}
				di.dispose();
				return null;
			}));
		}
		for (Future<?> fu : futures) fu.get();
		pool.shutdown();

		TreeMap<Long, List<Function>> regions = new TreeMap<>();
		for (Function f : funcs) {
			long a = f.getEntryPoint().getOffset();
			regions.computeIfAbsent(a & ~0xFFFFL, k -> new ArrayList<>()).add(f);
		}

		try (PrintWriter idx = new PrintWriter(new File(outDir, "index.tsv"), "UTF-8")) {
			idx.println("address\tname\tsize\tcallers\tcallees");
			for (var e : regions.entrySet()) {
				File out = new File(outDir, String.format("0x%08X.c", e.getKey()));
				try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
					for (Function f : e.getValue()) {
						long a = f.getEntryPoint().getOffset();
						Set<Function> callers = f.getCallingFunctions(TaskMonitor.DUMMY);
						Set<Function> callees = f.getCalledFunctions(TaskMonitor.DUMMY);
						long size = f.getBody().getNumAddresses();
						String cmt = f.getComment();
						w.println(String.format("// ==== 0x%08X  %s  (size 0x%X)", a, f.getName(true), size));
						if (cmt != null) w.println("// " + cmt.replace("\n", " "));
						w.println("// callers: " + names(callers, 25));
						w.println("// callees: " + names(callees, 40));
						w.println(code.getOrDefault(a, "/* missing */"));
						idx.println(String.format("0x%08X\t%s\t%d\t%s\t%s", a, f.getName(true), size,
							names(callers, 60), names(callees, 60)));
					}
				}
			}
		}
		println("ExportDecomp: wrote " + regions.size() + " region files to " + outDir);
	}
}
