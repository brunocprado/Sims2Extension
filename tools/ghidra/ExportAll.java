// Exporta metadados completos do programa para CSV/JSON/H (somente leitura).
// Uso: -postScript ExportAll.java <dir_saida>
//@category TS2

import java.io.*;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;
public class ExportAll extends GhidraScript {
    static class FInfo {
        LinkedHashSet<Address> callees = new LinkedHashSet<>();
        LinkedHashSet<Address> strings = new LinkedHashSet<>();
        LinkedHashSet<String> imports = new LinkedHashSet<>();
        List<String> vmember = new ArrayList<>();
    }
    static class VF { String sym; Address addr; List<String> slots = new ArrayList<>(); }
    static class Cls { String name; List<VF> vfs = new ArrayList<>(); List<String> basesAll = new ArrayList<>();
        List<String> basesDirect = new ArrayList<>(); List<String> ctors = new ArrayList<>(); }

    Listing lst; ReferenceManager rm; SymbolTable st; FunctionManager fm; File dir;
    Map<Address, String> strCache = new HashMap<>();          // todas as strings referenciadas por codigo
    Map<Address, Set<Address>> strRefs = new HashMap<>();     // string -> funcoes que a usam

    /** string em 'to': definida pelo Ghidra OU ASCII "solta" em bloco de dados (nao executavel). */
    String strAt(Address to) {
        String c = strCache.get(to);
        if (c != null) return c.isEmpty() ? null : c;
        String v = null;
        Data d = lst.getDataAt(to);
        if (d != null) { if (d.hasStringValue()) v = String.valueOf(d.getValue()); }
        else {
            MemoryBlock b = currentProgram.getMemory().getBlock(to);
            if (b != null && !b.isExecute() && b.isInitialized() && lst.getInstructionContaining(to) == null) {
                try {
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < 300; i++) {
                        int ch = currentProgram.getMemory().getByte(to.add(i)) & 0xff;
                        if (ch == 0) break;
                        if (ch >= 0x20 && ch < 0x7f || ch == 9 || ch == 10 || ch == 13) sb.append((char) ch); else { sb = null; break; }
                    }
                    if (sb != null && sb.length() >= 4) v = sb.toString();
                } catch (Exception e) { /* fora da memoria */ }
            }
        }
        strCache.put(to, v == null ? "" : v);
        return v;
    }

    static String q(String s) { return "\"" + (s == null ? "" : s.replace("\"", "\"\"").replace("\r", "\\r").replace("\n", "\\n")) + "\""; }
    static String j(String s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            switch (c) { case '"': b.append("\\\""); break; case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break; case '\r': b.append("\\r"); break; case '\t': b.append("\\t"); break;
                default: if (c < 0x20) b.append(String.format("\\u%04x", (int) c)); else b.append(c); }
        }
        return b.append('"').toString();
    }
    static String jl(Collection<String> c) { StringBuilder b = new StringBuilder("["); boolean f = true;
        for (String s : c) { if (!f) b.append(','); f = false; b.append(j(s)); } return b.append(']').toString(); }
    PrintWriter w(String name) throws IOException { return new PrintWriter(new BufferedWriter(new FileWriter(new File(dir, name)), 1 << 16)); }

    @Override
    public void run() throws Exception {
        dir = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : "export_out");
        dir.mkdirs();
        lst = currentProgram.getListing(); rm = currentProgram.getReferenceManager();
        st = currentProgram.getSymbolTable(); fm = currentProgram.getFunctionManager();
        PrintWriter sum = w("summary.txt");

        step("memory_map", () -> memoryMap());
        Map<Address, List<String>> vmembers = new HashMap<>();
        List<Cls> classes = new ArrayList<>();
        step("classes", () -> classes(classes, vmembers));
        Map<Address, FInfo> infos = new LinkedHashMap<>();
        Map<Address, List<Address>> callers = new HashMap<>();
        step("analyze_functions", () -> analyze(infos, callers, vmembers));
        step("functions", () -> functions(infos, callers));
        step("strings", () -> strings());
        step("imports", () -> imports());
        step("globals", () -> globals());
        step("datatypes", () -> datatypes());

        sum.printf("programa: %s%nfuncoes: %d%nclasses (RTTI): %d%nstrings/funcs: ver csv%n",
            currentProgram.getName(), fm.getFunctionCount(), classes.size());
        sum.close();
        new File(dir, "_DONE").createNewFile();
    }

    interface Job { void run() throws Exception; }
    void step(String n, Job j) {
        long t = System.currentTimeMillis();
        try { j.run(); println("[ok] " + n + " " + (System.currentTimeMillis() - t) / 1000 + "s"); }
        catch (Throwable e) { println("[ERRO] " + n + ": " + e); e.printStackTrace(); }
    }

    void memoryMap() throws Exception {
        try (PrintWriter o = w("memory_map.csv")) {
            o.println("name,start,end,size,read,write,exec,initialized");
            for (MemoryBlock b : currentProgram.getMemory().getBlocks())
                o.printf("%s,%s,%s,%d,%b,%b,%b,%b%n", q(b.getName()), b.getStart(), b.getEnd(), b.getSize(),
                    b.isRead(), b.isWrite(), b.isExecute(), b.isInitialized());
        }
    }

    // ---------- classes / vtables / hierarquia ----------
    String className(Address typeDesc) {
        Symbol s = st.getPrimarySymbol(typeDesc);
        return s == null ? null : s.getParentNamespace().getName(true);
    }

    void classes(List<Cls> out, Map<Address, List<String>> vmembers) throws Exception {
        Map<Long, Cls> byNs = new LinkedHashMap<>();
        List<Symbol> arrays = new ArrayList<>();
        List<Symbol> vfs = new ArrayList<>();
        for (Symbol s : st.getAllSymbols(false)) {
            String n = s.getName();
            if (n.equals("RTTI_Base_Class_Array")) arrays.add(s);
            else if (n.startsWith("vftable") && !n.contains("meta_ptr")) vfs.add(s);
        }
        for (Symbol s : vfs) {
            Namespace ns = s.getParentNamespace();
            Cls c = byNs.computeIfAbsent(ns.getID(), k -> { Cls x = new Cls(); x.name = ns.getName(true); return x; });
            VF v = new VF(); v.sym = s.getName(); v.addr = s.getAddress();
            Data d = getDataAt(s.getAddress());
            if (d != null) {
                int n = d.getNumComponents() > 0 ? d.getNumComponents() : 1;
                for (int i = 0; i < n; i++) {
                    Data cd = d.getNumComponents() > 0 ? d.getComponent(i) : d;
                    Object val = cd == null ? null : cd.getValue();
                    if (val instanceof Address) {
                        Address fa = (Address) val; Function f = fm.getFunctionAt(fa);
                        v.slots.add(i + ":" + fa + ":" + (f == null ? "?" : f.getName(true)));
                        vmembers.computeIfAbsent(fa, k -> new ArrayList<>()).add(c.name + "[" + i + "]");
                    }
                }
            }
            c.vfs.add(v);
            for (Reference r : rm.getReferencesTo(s.getAddress())) {
                Function g = fm.getFunctionContaining(r.getFromAddress());
                if (g != null) { String e = g.getEntryPoint().toString(); if (!c.ctors.contains(e)) c.ctors.add(e); }
            }
        }
        for (Symbol s : arrays) {
            Cls c = byNs.get(s.getParentNamespace().getID());
            if (c == null) { c = new Cls(); c.name = s.getParentNamespace().getName(true); byNs.put(s.getParentNamespace().getID(), c); }
            try {
                Data arr = getDataAt(s.getAddress());
                int n = arr.getNumComponents();
                String[] nm = new String[n]; long[] cont = new long[n];
                for (int i = 0; i < n; i++) {
                    Address da = (Address) arr.getComponent(i).getValue();
                    Data bd = getDataAt(da);
                    Address td = (Address) bd.getComponent(0).getValue();
                    nm[i] = className(td);
                    Object cv = bd.getComponent(1).getValue();
                    cont[i] = cv instanceof Scalar ? ((Scalar) cv).getValue() : 0;
                    if (i > 0 && nm[i] != null) c.basesAll.add(nm[i]);
                }
                for (int i = 1; i < n; i += 1 + (int) cont[i]) if (nm[i] != null) c.basesDirect.add(nm[i]);
            } catch (Exception ex) { /* RTTI incompleto para essa classe */ }
        }
        out.addAll(byNs.values());
        try (PrintWriter o = w("classes.json")) {
            o.println("[");
            boolean first = true;
            for (Cls c : out) {
                if (!first) o.println(","); first = false;
                o.print("  {\"name\":" + j(c.name) + ",\"bases_direct\":" + jl(c.basesDirect) + ",\"bases_all\":" + jl(c.basesAll)
                    + ",\"ctor_dtor_candidates\":" + jl(c.ctors) + ",\"vftables\":[");
                boolean f2 = true;
                for (VF v : c.vfs) {
                    if (!f2) o.print(","); f2 = false;
                    o.print("{\"symbol\":" + j(v.sym) + ",\"address\":" + j(v.addr.toString()) + ",\"slots\":" + jl(v.slots) + "}");
                }
                o.print("]}");
            }
            o.println("\n]");
        }
        try (PrintWriter o = w("classes_tree.txt")) {
            List<Cls> sorted = new ArrayList<>(out);
            sorted.sort(Comparator.comparing(x -> x.name));
            for (Cls c : sorted) {
                int slots = 0; for (VF v : c.vfs) slots = Math.max(slots, v.slots.size());
                o.printf("%s  [vfuncs=%d, vftables=%d]%s%n", c.name, slots, c.vfs.size(),
                    c.basesDirect.isEmpty() ? "" : "  : " + String.join(", ", c.basesDirect));
            }
        }
    }

    // ---------- analise por funcao ----------
    void analyze(Map<Address, FInfo> infos, Map<Address, List<Address>> callers, Map<Address, List<String>> vmembers) {
        for (Function f : fm.getFunctions(true)) {
            if (monitor.isCancelled()) break;
            FInfo fi = new FInfo();
            List<String> vm = vmembers.get(f.getEntryPoint());
            if (vm != null) fi.vmember.addAll(vm);
            AddressIterator ai = rm.getReferenceSourceIterator(f.getBody(), true);
            while (ai.hasNext()) {
                Address a = ai.next();
                for (Reference r : rm.getReferencesFrom(a)) {
                    Address to = r.getToAddress();
                    if (r.getReferenceType().isCall()) {
                        String imp = importName(to);
                        if (imp != null) { fi.imports.add(imp); continue; }
                        Function cf = fm.getFunctionAt(to);
                        if (cf == null) cf = fm.getFunctionContaining(to);
                        if (cf != null) {
                            if (cf.isThunk()) { Function t = cf.getThunkedFunction(true);
                                if (t != null && t.isExternal()) { fi.imports.add(t.getName(true)); continue; } }
                            fi.callees.add(cf.getEntryPoint());
                        } else {
                            Data d = lst.getDataAt(to);   // call [IAT]
                            if (d != null) for (Reference r2 : d.getReferencesFrom()) {
                                String im = importName(r2.getToAddress()); if (im != null) fi.imports.add(im);
                            }
                        }
                    } else if (r.getReferenceType().isData()) {
                        if (strAt(to) != null) {
                            fi.strings.add(to);
                            strRefs.computeIfAbsent(to, k -> new HashSet<>()).add(f.getEntryPoint());
                        }
                    }
                }
            }
            for (Address c : fi.callees) callers.computeIfAbsent(c, k -> new ArrayList<>()).add(f.getEntryPoint());
            infos.put(f.getEntryPoint(), fi);
        }
    }

    String importName(Address to) {
        if (to.isExternalAddress()) {
            Symbol s = st.getPrimarySymbol(to);
            return s == null ? "EXTERNAL" : s.getName(true);
        }
        return null;
    }

    void functions(Map<Address, FInfo> infos, Map<Address, List<Address>> callers) throws Exception {
        try (PrintWriter o = w("functions.csv"); PrintWriter e = w("calls.csv"); PrintWriter dj = w("functions_detail.jsonl")) {
            o.println("address,name,namespace,size_bytes,signature,calling_convention,is_thunk,n_callers,n_callees,n_strings,n_imports,vtable_membership");
            e.println("caller,callee");
            for (Function f : fm.getFunctions(true)) {
                FInfo fi = infos.get(f.getEntryPoint());
                if (fi == null) continue;
                List<Address> cl = callers.getOrDefault(f.getEntryPoint(), Collections.emptyList());
                String sig; try { sig = f.getPrototypeString(false, false); } catch (Exception x) { sig = ""; }
                o.printf("%s,%s,%s,%d,%s,%s,%b,%d,%d,%d,%d,%s%n", f.getEntryPoint(), q(f.getName()), q(f.getParentNamespace().getName(true)),
                    f.getBody().getNumAddresses(), q(sig), f.getCallingConventionName(), f.isThunk(),
                    cl.size(), fi.callees.size(), fi.strings.size(), fi.imports.size(), q(String.join(";", fi.vmember)));
                for (Address c : fi.callees) e.println(f.getEntryPoint() + "," + c);

                List<String> cle = new ArrayList<>(), cae = new ArrayList<>(), str = new ArrayList<>();
                for (Address c : fi.callees) { Function cf = fm.getFunctionAt(c); cle.add(c + (cf == null ? "" : " " + cf.getName(true))); }
                for (Address c : cl) { Function cf = fm.getFunctionAt(c); cae.add(c + (cf == null ? "" : " " + cf.getName(true))); }
                int k = 0;
                for (Address sa : fi.strings) { if (k++ >= 40) break;
                    String v = strCache.get(sa); if (v.length() > 160) v = v.substring(0, 160) + "..."; str.add(sa + " " + v); }
                dj.println("{\"address\":" + j(f.getEntryPoint().toString()) + ",\"name\":" + j(f.getName(true))
                    + ",\"signature\":" + j(sig) + ",\"size\":" + f.getBody().getNumAddresses()
                    + ",\"vtable\":" + jl(fi.vmember) + ",\"imports\":" + jl(fi.imports)
                    + ",\"callees\":" + jl(cle) + ",\"callers\":" + jl(cae) + ",\"strings\":" + jl(str) + "}");
            }
        }
    }

    // ---------- strings ----------
    void strings() throws Exception {
        try (PrintWriter o = w("strings.csv")) {
            o.println("address,length,n_refs,referencing_functions,defined_by_ghidra,string");
            Set<Address> seen = new HashSet<>();
            for (Data d : lst.getDefinedData(true)) {
                if (monitor.isCancelled()) break;
                if (!d.hasStringValue()) continue;
                String v = String.valueOf(d.getValue());
                if (v.length() < 3) continue;
                seen.add(d.getAddress());
                LinkedHashSet<String> fs = new LinkedHashSet<>(); int n = 0;
                for (Reference r : rm.getReferencesTo(d.getAddress())) { n++;
                    Function g = fm.getFunctionContaining(r.getFromAddress());
                    if (g != null && fs.size() < 20) fs.add(g.getEntryPoint().toString()); }
                o.printf("%s,%d,%d,%s,true,%s%n", d.getAddress(), v.length(), n, q(String.join(";", fs)), q(v));
            }
            List<Address> extra = new ArrayList<>(strRefs.keySet());
            Collections.sort(extra);
            for (Address a : extra) {
                if (seen.contains(a)) continue;
                String v = strCache.get(a);
                Set<Address> fs = strRefs.get(a);
                o.printf("%s,%d,%d,%s,false,%s%n", a, v.length(), fs.size(),
                    q(String.join(";", fs.stream().limit(20).map(Address::toString).toList())), q(v));
            }
        }
    }

    // ---------- imports ----------
    void imports() throws Exception {
        try (PrintWriter o = w("imports.csv")) {
            o.println("library,name,address,n_callers,callers");
            for (Symbol s : st.getExternalSymbols()) {
                ExternalLocation el = s.getSymbolType() == SymbolType.LABEL || s.getSymbolType() == SymbolType.FUNCTION
                    ? currentProgram.getExternalManager().getExternalLocation(s) : null;
                String lib = el == null ? s.getParentNamespace().getName() : el.getLibraryName();
                LinkedHashSet<String> cs = new LinkedHashSet<>();
                for (Reference r : s.getReferences()) collectUsers(r.getFromAddress(), cs, 0);
                o.printf("%s,%s,%s,%d,%s%n", q(lib), q(s.getName()), el == null || el.getAddress() == null ? "" : el.getAddress().toString(),
                    cs.size(), q(String.join(";", cs.stream().limit(40).toList())));
            }
        }
    }
    void collectUsers(Address from, Set<String> out, int depth) {
        Function g = fm.getFunctionContaining(from);
        if (g != null) { out.add(g.getEntryPoint().toString()); return; }
        if (depth > 1) return;
        for (Reference r : rm.getReferencesTo(from)) collectUsers(r.getFromAddress(), out, depth + 1);
    }

    // ---------- globals ----------
    void globals() throws Exception {
        try (PrintWriter o = w("globals.csv")) {
            o.println("address,name,namespace,datatype,size,n_refs,n_ref_functions");
            for (Data d : lst.getDefinedData(true)) {
                if (monitor.isCancelled()) break;
                Symbol s = st.getPrimarySymbol(d.getAddress());
                if (s == null || s.getSource() == SourceType.DEFAULT) continue;
                String n = s.getName();
                if (n.startsWith("RTTI_") || n.startsWith("vftable") || n.startsWith("s_") || d.hasStringValue()) continue;
                int refs = 0; Set<Address> fs = new HashSet<>();
                for (Reference r : rm.getReferencesTo(d.getAddress())) { refs++;
                    Function g = fm.getFunctionContaining(r.getFromAddress()); if (g != null) fs.add(g.getEntryPoint()); }
                if (refs == 0) continue;
                o.printf("%s,%s,%s,%s,%d,%d,%d%n", d.getAddress(), q(n), q(s.getParentNamespace().getName(true)),
                    q(d.getDataType().getName()), d.getLength(), refs, fs.size());
            }
        }
    }

    // ---------- tipos (header C) ----------
    void datatypes() throws Exception {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        List<DataType> l = new ArrayList<>();
        Iterator<DataType> it = dtm.getAllDataTypes();
        while (it.hasNext()) {
            DataType dt = it.next();
            if (dt instanceof Composite || dt instanceof ghidra.program.model.data.Enum || dt instanceof TypeDef) l.add(dt);
        }
        try (PrintWriter o = w("datatypes.h")) {
            DataTypeWriter dw = new DataTypeWriter(dtm, o);
            dw.write(l, monitor);
        }
        println("tipos exportados: " + l.size());
    }
}
