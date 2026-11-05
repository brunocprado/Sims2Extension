// Renomeia funcoes FUN_xxxx usando RTTI/vtables e SALVA o projeto.
//  1) funcoes que estao em slots de vftable  -> <Classe>::vfunc_<slot>   (usa a classe mais "base")
//  2) funcoes que gravam o ponteiro de vftable -> <Classe>::ctor_dtor_<addr>  (usa a classe mais derivada)
// Rodar SEM -readOnly (precisa salvar). Idempotente: so mexe em nomes FUN_*.
//@category TS2

import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class ApplyNames extends GhidraScript {
    static class Cand { Namespace ns; int slot; int bases; String vf;
        Cand(Namespace n, int s, int b, String v) { ns = n; slot = s; bases = b; vf = v; } }

    @Override
    public void run() throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        Map<Long, Integer> baseCount = new HashMap<>();
        List<Symbol> vft = new ArrayList<>();
        for (Symbol s : st.getAllSymbols(false)) {
            String n = s.getName();
            if (n.equals("RTTI_Base_Class_Array")) {
                Data d = getDataAt(s.getAddress());
                baseCount.put(s.getParentNamespace().getID(), d == null ? 0 : d.getNumComponents());
            } else if (n.startsWith("vftable") && !n.contains("meta_ptr")) {
                vft.add(s);
            }
        }
        println("vftables: " + vft.size());

        Map<Function, List<Cand>> slotCands = new HashMap<>();
        Map<Function, List<Cand>> ctorCands = new HashMap<>();

        for (Symbol s : vft) {
            Namespace ns = s.getParentNamespace();
            int bases = baseCount.getOrDefault(ns.getID(), 1);
            Data d = getDataAt(s.getAddress());
            if (d != null) {
                int n = d.getNumComponents() > 0 ? d.getNumComponents() : 1;
                for (int i = 0; i < n; i++) {
                    Data c = d.getNumComponents() > 0 ? d.getComponent(i) : d;
                    Object v = c == null ? null : c.getValue();
                    if (!(v instanceof Address)) continue;
                    Function f = fm.getFunctionAt((Address) v);
                    if (f == null) continue;
                    slotCands.computeIfAbsent(f, k -> new ArrayList<>()).add(new Cand(ns, i, bases, s.getName()));
                }
            }
            for (Reference r : rm.getReferencesTo(s.getAddress())) {
                Function g = fm.getFunctionContaining(r.getFromAddress());
                if (g == null) continue;
                ctorCands.computeIfAbsent(g, k -> new ArrayList<>()).add(new Cand(ns, -1, bases, s.getName()));
            }
        }

        int renVf = 0, renCtor = 0, commented = 0, fail = 0;

        for (Map.Entry<Function, List<Cand>> e : slotCands.entrySet()) {
            Function f = e.getKey();
            List<Cand> cs = e.getValue();
            StringBuilder sb = new StringBuilder("Virtual em:");
            for (Cand c : cs) sb.append(' ').append(c.ns.getName(true)).append('[').append(c.slot).append("];");
            if (f.getComment() == null) { f.setComment(sb.toString()); commented++; }
            if (!f.getName().startsWith("FUN_")) continue;
            Cand best = cs.get(0);
            for (Cand c : cs) if (c.bases < best.bases) best = c;
            if (rename(f, best.ns, "vfunc_" + best.slot, st)) renVf++; else fail++;
        }

        for (Map.Entry<Function, List<Cand>> e : ctorCands.entrySet()) {
            Function g = e.getKey();
            if (!g.getName().startsWith("FUN_")) continue;
            Cand best = e.getValue().get(0);
            for (Cand c : e.getValue()) if (c.bases > best.bases) best = c;
            String nm = "ctor_dtor_" + g.getEntryPoint();
            if (rename(g, best.ns, nm, st)) renCtor++; else fail++;
        }
        println(String.format("renomeadas vfunc=%d ctor_dtor=%d comentadas=%d falhas=%d", renVf, renCtor, commented, fail));
    }

    private boolean rename(Function f, Namespace ns, String name, SymbolTable st) {
        try {
            String nm = name;
            for (Symbol x : st.getSymbols(nm, ns)) {
                if (!x.getAddress().equals(f.getEntryPoint())) { nm = name + "_" + f.getEntryPoint(); break; }
            }
            f.setParentNamespace(ns);
            f.setName(nm, SourceType.ANALYSIS);
            return true;
        } catch (Exception ex) {
            try { f.setName(name + "_" + f.getEntryPoint(), SourceType.ANALYSIS); return true; }
            catch (Exception ex2) { return false; }
        }
    }
}
