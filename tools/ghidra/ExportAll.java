// Headless Ghidra post-script: dumps everything the analysis docs are built from.
// Usage (via tools/decompile.sh): -postScript ExportAll.java <output-dir>
// Writes: decomp.c, functions.tsv, strings.tsv, imports.tsv, data_tables.tsv
//@category Export

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportAll extends GhidraScript {

    private static String clean(String s) {
        return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : ".");
        outDir.mkdirs();

        // imports
        try (PrintWriter w = new PrintWriter(new File(outDir, "imports.tsv"), "UTF-8")) {
            w.println("library\tname\taddress\txref_functions");
            SymbolIterator it = currentProgram.getSymbolTable().getExternalSymbols();
            while (it.hasNext()) {
                Symbol s = it.next();
                Set<String> users = new TreeSet<>();
                for (Reference r : s.getReferences()) {
                    // references land on the IAT thunk; walk one more hop
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) {
                        users.add(f.getName());
                    } else {
                        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(r.getFromAddress());
                        while (ri.hasNext()) {
                            Function g = getFunctionContaining(ri.next().getFromAddress());
                            if (g != null) users.add(g.getName());
                        }
                    }
                }
                w.println(s.getParentNamespace().getName() + "\t" + s.getName() + "\t" + s.getAddress() + "\t" + String.join(",", users));
            }
        }

        // strings
        try (PrintWriter w = new PrintWriter(new File(outDir, "strings.tsv"), "UTF-8")) {
            w.println("address\ttype\tstring\txref_functions");
            DataIterator di = currentProgram.getListing().getDefinedData(true);
            while (di.hasNext()) {
                Data d = di.next();
                StringDataInstance sdi = StringDataInstance.getStringDataInstance(d);
                if (sdi == null || sdi == StringDataInstance.NULL_INSTANCE) continue;
                String v = sdi.getStringValue();
                if (v == null || v.length() < 3) continue;
                Set<String> users = new TreeSet<>();
                ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
                while (ri.hasNext()) {
                    Function f = getFunctionContaining(ri.next().getFromAddress());
                    if (f != null) users.add(f.getName());
                }
                w.println(d.getAddress() + "\t" + d.getDataType().getName() + "\t" + clean(v) + "\t" + String.join(",", users));
            }
        }

        // functions + decompilation
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        try (PrintWriter fw = new PrintWriter(new File(outDir, "functions.tsv"), "UTF-8");
             PrintWriter cw = new PrintWriter(new File(outDir, "decomp.c"), "UTF-8")) {
            fw.println("address\tname\tsize\tcallers\tcallees");
            FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
            while (fi.hasNext() && !monitor.isCancelled()) {
                Function f = fi.next();
                List<String> callers = new ArrayList<>();
                for (Function c : f.getCallingFunctions(monitor)) callers.add(c.getName());
                List<String> callees = new ArrayList<>();
                for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getName());
                fw.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + f.getBody().getNumAddresses() + "\t"
                        + String.join(",", callers) + "\t" + String.join(",", callees));
                if (f.isThunk() || f.isExternal()) continue;
                DecompileResults res = dec.decompileFunction(f, 120, monitor);
                cw.println("// ==== " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses()
                        + " callers=" + callers.size() + " ====");
                if (res != null && res.decompileCompleted()) {
                    cw.println(res.getDecompiledFunction().getC());
                } else {
                    cw.println("// DECOMPILE FAILED: " + (res == null ? "null" : res.getErrorMessage()));
                }
            }
        }
        dec.dispose();
        println("ExportAll: wrote " + outDir.getAbsolutePath());
    }
}
