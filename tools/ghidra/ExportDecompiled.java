// Exporta o C decompilado de TODAS as funcoes, em blocos de FUNCS_PER_FILE funcoes.
// RETOMAVEL: blocos ja concluidos (arquivo final existe) sao pulados.
// Cada bloco e escrito em .tmp e renomeado so quando completo.
// Para cada bloco tambem grava _idx_partNNNN.csv (endereco,nome,arquivo,linha_ini,linha_fim).
// Uso (headless): -postScript ExportDecompiled.java <dir_saida> [timeout_por_funcao_s]
//@category TS2

import java.io.*;
import java.nio.file.*;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.util.task.ConsoleTaskMonitor;

public class ExportDecompiled extends GhidraScript {
    private static final int FUNCS_PER_FILE = 500;

    private PrintWriter out;
    private int line;
    private void emit(String s) {
        out.println(s);
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) == '\n') line++;
        line++;
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File outDir = new File(a.length > 0 ? a[0] : "decomp_out");
        int timeout = a.length > 1 ? Integer.parseInt(a[1]) : 60;
        outDir.mkdirs();

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();
        int total = fm.getFunctionCount();
        int idx = 0, decompiled = 0, skipped = 0, failed = 0;
        File tmp = null, fin = null, idxTmp = null, idxFin = null;
        PrintWriter ix = null;
        boolean skipChunk = false;
        PrintWriter log = new PrintWriter(new FileWriter(new File(outDir, "_progress.log"), true), true);
        long t0 = System.currentTimeMillis();
        log.printf("INICIO total=%d funcoes (blocos de %d)%n", total, FUNCS_PER_FILE);

        for (Function f : fm.getFunctions(true)) {   // ordem por endereco: deterministica
            if (monitor.isCancelled()) break;

            if (idx % FUNCS_PER_FILE == 0) {
                if (out != null) {
                    out.close(); ix.close();
                    Files.move(idxTmp.toPath(), idxFin.toPath(), StandardCopyOption.REPLACE_EXISTING);
                    Files.move(tmp.toPath(), fin.toPath(), StandardCopyOption.REPLACE_EXISTING);
                    out = null;
                }
                int chunk = idx / FUNCS_PER_FILE;
                fin = new File(outDir, String.format("%s_part%04d.c", currentProgram.getName(), chunk));
                tmp = new File(outDir, fin.getName() + ".tmp");
                idxFin = new File(outDir, String.format("_idx_part%04d.csv", chunk));
                idxTmp = new File(outDir, idxFin.getName() + ".tmp");
                skipChunk = fin.exists();
                if (!skipChunk) {
                    out = new PrintWriter(new BufferedWriter(new FileWriter(tmp)));
                    ix = new PrintWriter(new BufferedWriter(new FileWriter(idxTmp)));
                    line = 1;
                }
            }

            if (skipChunk) { skipped++; idx++; continue; }

            int start = line;
            emit("\n// ==== " + f.getName(true) + " @ " + f.getEntryPoint() + " ====");
            try {
                DecompileResults r = ifc.decompileFunction(f, timeout, new ConsoleTaskMonitor());
                if (r.decompileCompleted()) emit(r.getDecompiledFunction().getC());
                else { failed++; emit("// FALHOU: " + r.getErrorMessage()); }
            } catch (Exception e) {
                failed++; emit("// EXCECAO: " + e);
            }
            ix.printf("%s,\"%s\",%s,%d,%d%n", f.getEntryPoint(), f.getName(true).replace("\"", "\"\""), fin.getName(), start, line - 1);
            decompiled++; idx++;

            if (decompiled % 100 == 0) {
                long s = (System.currentTimeMillis() - t0) / 1000;
                log.printf("%d/%d (decompiladas nesta execucao=%d, puladas=%d, falhas=%d) %ds%n",
                           idx, total, decompiled, skipped, failed, s);
            }
        }
        if (out != null) {
            out.close(); ix.close();
            Files.move(idxTmp.toPath(), idxFin.toPath(), StandardCopyOption.REPLACE_EXISTING);
            Files.move(tmp.toPath(), fin.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
        log.printf("FIM: %d/%d (decompiladas=%d, puladas=%d, falhas=%d)%n", idx, total, decompiled, skipped, failed);
        if (!monitor.isCancelled()) new File(outDir, "_DONE").createNewFile();
        log.close();
        ifc.dispose();
    }
}
