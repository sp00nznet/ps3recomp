// ExportCompareUnits.java -- Ghidra headless post-script.
//
// Emits one units.json matching tools/compare/schema.py (the unified Unit
// record) for whatever program is currently loaded. Works for BOTH sides of
// the harness because Ghidra disassembles Xenon (Xbox 360) and Cell PPU (PS3)
// with the same processor module: PowerPC:BE:64:64-32addr.
//
// Usage (headless), e.g. for the PS3 side:
//   analyzeHeadless <proj_dir> <proj_name> \
//     -import EBOOT.ELF \
//     -processor PowerPC:BE:64:64-32addr \
//     -postScript ExportCompareUnits.java ps3 ppc64-cell out/ps3.units.json
//
// For the 360 side, import the XEX (Ghidra's XEX loader handles it) and pass:
//   -postScript ExportCompareUnits.java x360 ppc64-xenon out/x360.units.json
//
// Args: <platform> <arch> <outputPath>
//
// @category Recompilation

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;

import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class ExportCompareUnits extends GhidraScript {

    private static final long CONST_MIN = 0x1000; // mirror matcher's CONST_ANCHOR_MIN

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String platform = args.length > 0 ? args[0] : "unknown";
        String arch = args.length > 1 ? args[1] : "unknown";
        String outPath = args.length > 2 ? args[2]
                : (currentProgram.getName() + ".units.json");

        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        sb.append("  \"schema_version\": 1,\n");
        sb.append("  \"platform\": ").append(jstr(platform)).append(",\n");
        sb.append("  \"binary\": ").append(jstr(currentProgram.getName())).append(",\n");
        sb.append("  \"arch\": ").append(jstr(arch)).append(",\n");
        sb.append("  \"source\": \"ghidra\",\n");
        sb.append("  \"units\": [\n");

        boolean first = true;
        int n = 0;
        for (Function fn : fm.getFunctions(true)) {
            if (monitor.isCancelled()) break;
            if (fn.isExternal() || fn.isThunk()) continue;

            AddressSetView body = fn.getBody();
            long sizeBytes = body.getNumAddresses();

            Map<String, Integer> mnem = new TreeMap<>();
            List<String> strRefs = new ArrayList<>();
            List<Long> constRefs = new ArrayList<>();
            List<Long> calls = new ArrayList<>();
            List<String> imports = new ArrayList<>();
            int insnCount = 0;
            boolean isLeaf = true;

            for (Instruction insn : listing.getInstructions(body, true)) {
                if (monitor.isCancelled()) break;
                insnCount++;
                String m = insn.getMnemonicString().toLowerCase();
                mnem.merge(m, 1, Integer::sum);

                // scalar immediates -> const refs
                for (int op = 0; op < insn.getNumOperands(); op++) {
                    Object[] objs = insn.getOpObjects(op);
                    for (Object o : objs) {
                        if (o instanceof Scalar) {
                            long v = ((Scalar) o).getUnsignedValue();
                            if (v >= CONST_MIN) constRefs.add(v);
                        }
                    }
                }

                // references out of this instruction
                for (Reference ref : insn.getReferencesFrom()) {
                    RefType rt = ref.getReferenceType();
                    Address to = ref.getToAddress();
                    if (rt.isCall()) {
                        isLeaf = false;
                        Function callee = fm.getFunctionAt(to);
                        if (callee != null && (callee.isExternal() || callee.isThunk())) {
                            imports.add(callee.getName());
                        } else if (to != null) {
                            calls.add(to.getOffset());
                        }
                    } else if (rt.isData() && to != null) {
                        Data d = listing.getDataAt(to);
                        if (d != null && d.hasStringValue()) {
                            Object val = d.getValue();
                            if (val != null) strRefs.add(val.toString());
                        } else {
                            constRefs.add(to.getOffset());
                        }
                    }
                }
            }

            int frame = 0;
            try {
                frame = Math.abs(fn.getStackFrame().getFrameSize());
            } catch (Exception ignore) { }

            if (!first) sb.append(",\n");
            first = false;
            sb.append("    {");
            sb.append("\"addr\": ").append(jstr(hex(fn.getEntryPoint().getOffset())));
            sb.append(", \"size\": ").append(sizeBytes);
            sb.append(", \"name\": ").append(jstr(fn.getName()));
            sb.append(", \"insn_count\": ").append(insnCount);
            sb.append(", \"is_leaf\": ").append(isLeaf);
            sb.append(", \"stack_size\": ").append(frame);
            sb.append(", \"calls\": ").append(hexArr(calls));
            sb.append(", \"imports\": ").append(strArr(imports));
            sb.append(", \"string_refs\": ").append(strArr(strRefs));
            sb.append(", \"const_refs\": ").append(hexArr(constRefs));
            sb.append(", \"mnemonic_hist\": ").append(histObj(mnem));
            sb.append("}");
            n++;
        }

        sb.append("\n  ]\n}\n");

        try (FileWriter fw = new FileWriter(outPath)) {
            fw.write(sb.toString());
        }
        println("ExportCompareUnits: wrote " + n + " units -> " + outPath);
    }

    private static String hex(long v) {
        return String.format("0x%08x", v);
    }

    private static String jstr(String s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder("\"");
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
        return b.append("\"").toString();
    }

    private static String strArr(List<String> xs) {
        StringBuilder b = new StringBuilder("[");
        for (int i = 0; i < xs.size(); i++) {
            if (i > 0) b.append(", ");
            b.append(jstr(xs.get(i)));
        }
        return b.append("]").toString();
    }

    private static String hexArr(List<Long> xs) {
        StringBuilder b = new StringBuilder("[");
        for (int i = 0; i < xs.size(); i++) {
            if (i > 0) b.append(", ");
            b.append(jstr(hex(xs.get(i))));
        }
        return b.append("]").toString();
    }

    private static String histObj(Map<String, Integer> m) {
        StringBuilder b = new StringBuilder("{");
        boolean first = true;
        for (Map.Entry<String, Integer> e : m.entrySet()) {
            if (!first) b.append(", ");
            first = false;
            b.append(jstr(e.getKey())).append(": ").append(e.getValue());
        }
        return b.append("}").toString();
    }
}
