/* ExportAnalysisJson — ps3recomp Ghidra headless post-script.
 *
 * Exports Ghidra's analysis of a PS3 PPU ELF as JSON so the recompiler
 * toolchain can use it to improve function boundaries and naming.
 *
 * Outputs (into the directory given as the first script arg):
 *   functions.json      [{addr,size,name,thunk,calling}]
 *   symbols.json        [{addr,name,type,source,namespace}]
 *   strings.json        [{func,addr,strings:[{addr,val}]}]  (string xrefs per fn)
 *   decompiled.json     [{addr,name,c}]   (only when 2nd arg == "decompile")
 *
 * Invoke via analyzeHeadless:
 *   analyzeHeadless <proj> <name> -import EBOOT.elf \
 *       -processor PowerPC:BE:64:64-32addr \
 *       -postScript ExportAnalysisJson.java <outDir> [decompile]
 *
 * @category PS3
 */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.List;

public class ExportAnalysisJson extends GhidraScript {

    private static String esc(String s) {
        if (s == null) return "";
        StringBuilder b = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
                    else b.append(c);
            }
        }
        return b.toString();
    }

    private BufferedWriter open(String dir, String name) throws Exception {
        File f = new File(dir, name);
        return new BufferedWriter(new FileWriter(f));
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = (args.length > 0) ? args[0] : ".";
        boolean doDecomp = (args.length > 1) && "decompile".equalsIgnoreCase(args[1]);
        new File(outDir).mkdirs();

        Listing listing = currentProgram.getListing();
        SymbolTable symtab = currentProgram.getSymbolTable();

        // ---- functions.json ----
        println("[export] writing functions.json");
        BufferedWriter fw = open(outDir, "functions.json");
        fw.write("[\n");
        FunctionIterator fit = listing.getFunctions(true);
        boolean first = true;
        int nfunc = 0;
        while (fit.hasNext() && !monitor.isCancelled()) {
            Function fn = fit.next();
            long addr = fn.getEntryPoint().getOffset();
            long size = fn.getBody().getNumAddresses();
            if (!first) fw.write(",\n");
            first = false;
            fw.write(String.format(
                "  {\"addr\":\"0x%08X\",\"size\":%d,\"name\":\"%s\",\"thunk\":%s,\"calling\":\"%s\"}",
                addr, size, esc(fn.getName()),
                fn.isThunk() ? "true" : "false",
                esc(fn.getCallingConventionName())));
            nfunc++;
        }
        fw.write("\n]\n");
        fw.close();
        println("[export] functions: " + nfunc);

        // ---- symbols.json ----
        println("[export] writing symbols.json");
        BufferedWriter sw = open(outDir, "symbols.json");
        sw.write("[\n");
        SymbolIterator sit = symtab.getAllSymbols(true);
        first = true;
        int nsym = 0;
        while (sit.hasNext() && !monitor.isCancelled()) {
            Symbol sym = sit.next();
            Address a = sym.getAddress();
            if (a == null || !a.isMemoryAddress()) continue;
            String ns = sym.getParentNamespace() != null
                    ? sym.getParentNamespace().getName(true) : "";
            if (!first) sw.write(",\n");
            first = false;
            sw.write(String.format(
                "  {\"addr\":\"0x%08X\",\"name\":\"%s\",\"type\":\"%s\",\"source\":\"%s\",\"namespace\":\"%s\",\"primary\":%s}",
                a.getOffset(), esc(sym.getName()), esc(sym.getSymbolType().toString()),
                esc(sym.getSource().toString()), esc(ns),
                sym.isPrimary() ? "true" : "false"));
            nsym++;
        }
        sw.write("\n]\n");
        sw.close();
        println("[export] symbols: " + nsym);

        // ---- strings.json: defined strings + the functions that reference them ----
        println("[export] writing strings.json");
        BufferedWriter stw = open(outDir, "strings.json");
        stw.write("[\n");
        first = true;
        int nstr = 0;
        java.util.Iterator<Data> dit = listing.getDefinedData(true);
        while (dit.hasNext() && !monitor.isCancelled()) {
            Data d = dit.next();
            if (d == null) continue;
            StringDataInstance sdi = StringDataInstance.getStringDataInstance(d);
            String val = (sdi != null) ? sdi.getStringValue() : null;
            if (val == null || val.length() < 3) continue;
            // Who references this string?
            List<String> refFns = new ArrayList<>();
            ReferenceIterator rit = currentProgram.getReferenceManager()
                    .getReferencesTo(d.getAddress());
            while (rit.hasNext()) {
                Reference r = rit.next();
                Function rf = listing.getFunctionContaining(r.getFromAddress());
                if (rf != null) {
                    refFns.add(String.format("0x%08X", rf.getEntryPoint().getOffset()));
                }
            }
            if (refFns.isEmpty()) continue;
            StringBuilder fnArr = new StringBuilder();
            for (int i = 0; i < refFns.size(); i++) {
                if (i > 0) fnArr.append(",");
                fnArr.append("\"").append(refFns.get(i)).append("\"");
            }
            if (!first) stw.write(",\n");
            first = false;
            stw.write(String.format(
                "  {\"addr\":\"0x%08X\",\"val\":\"%s\",\"refs\":[%s]}",
                d.getAddress().getOffset(), esc(val), fnArr.toString()));
            nstr++;
        }
        stw.write("\n]\n");
        stw.close();
        println("[export] strings (referenced): " + nstr);

        // ---- decompiled.json (optional, slow) ----
        if (doDecomp) {
            println("[export] decompiling (this is slow)...");
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            BufferedWriter dw = open(outDir, "decompiled.json");
            dw.write("[\n");
            first = true;
            int ndec = 0;
            FunctionIterator fit2 = listing.getFunctions(true);
            while (fit2.hasNext() && !monitor.isCancelled()) {
                Function fn = fit2.next();
                if (fn.isThunk()) continue;
                DecompileResults res = decomp.decompileFunction(fn, 30, monitor);
                String c = (res != null && res.decompileCompleted()
                        && res.getDecompiledFunction() != null)
                        ? res.getDecompiledFunction().getC() : "";
                if (!first) dw.write(",\n");
                first = false;
                dw.write(String.format(
                    "  {\"addr\":\"0x%08X\",\"name\":\"%s\",\"c\":\"%s\"}",
                    fn.getEntryPoint().getOffset(), esc(fn.getName()), esc(c)));
                ndec++;
                if ((ndec % 500) == 0) println("[export] decompiled " + ndec);
            }
            dw.write("\n]\n");
            dw.close();
            decomp.dispose();
            println("[export] decompiled: " + ndec);
        }

        println("[export] DONE -> " + outDir);
    }
}
