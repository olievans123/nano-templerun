// Decompile every function in an address range into one C file for local porting analysis.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;

public class ExportRange extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long lo = Long.parseLong(args[0], 16), hi = Long.parseLong(args[1], 16);
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[2]))) {
            int n = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                long a = f.getEntryPoint().getOffset();
                if (a < lo || a >= hi) continue;
                DecompileResults r = ifc.decompileFunction(f, 120, monitor);
                out.println("/* ==== " + f.getEntryPoint() + " " + f.getName() + " size " + f.getBody().getNumAddresses() + " ==== */");
                if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
                else out.println("/* decompile failed */");
                n++;
            }
            println("exported " + n + " functions");
        }
    }
}
