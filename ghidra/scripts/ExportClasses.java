// Decompile every function in an address range into one C file per class, for local porting analysis.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class ExportClasses extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long lo = Long.parseLong(args[0], 16), hi = Long.parseLong(args[1], 16);
        File dir = new File(args[2]);
        dir.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        Map<String, PrintWriter> files = new HashMap<>();
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            long a = f.getEntryPoint().getOffset();
            if (a < lo || a >= hi) continue;
            String ns = f.getParentNamespace().getName().replaceAll("[^A-Za-z0-9_]", "_");
            if (ns.equals("Global")) ns = "_global";
            PrintWriter out = files.get(ns);
            if (out == null) { out = new PrintWriter(new FileWriter(new File(dir, ns + ".c"))); files.put(ns, out); }
            DecompileResults r = ifc.decompileFunction(f, 180, monitor);
            out.println("/* ==== " + f.getEntryPoint() + " " + f.getName(true) + " size " + f.getBody().getNumAddresses() + " ==== */");
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            else out.println("/* decompile failed */");
            n++;
        }
        for (PrintWriter w : files.values()) w.close();
        println("exported " + n + " functions into " + files.size() + " files");
    }
}
