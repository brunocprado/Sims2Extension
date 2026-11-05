#!/usr/bin/env python3
"""Pos-processa a saida do Ghidra:
  - export/function_index.csv : functions.csv + arquivo/linha do C decompilado
  - by_class/<Classe>.c       : todas as funcoes de uma classe juntas
  - by_class/_index.md        : lista de classes
  - README.md                 : guia dos arquivos gerados
Uso: postprocess.py <docs/ghidra>
"""
import csv, glob, hashlib, os, re, sys, collections

csv.field_size_limit(sys.maxsize)
root = sys.argv[1] if len(sys.argv) > 1 else "docs/ghidra"
dec = os.path.join(root, "decomp_named")
exp = os.path.join(root, "export")
bycls = os.path.join(root, "by_class")
os.makedirs(bycls, exist_ok=True)

# 1) indice de decompilacao
idx = {}   # addr -> (file, start, end)
rows = []
for p in sorted(glob.glob(os.path.join(dec, "_idx_part*.csv"))):
    with open(p, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.reader(f):
            addr, name, fn, a, b = r
            idx[addr] = (fn, int(a), int(b))
            rows.append((addr, name, fn, int(a), int(b)))
with open(os.path.join(exp, "decomp_index.csv"), "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f); w.writerow(["address", "name", "file", "line_start", "line_end"]); w.writerows(rows)

# 2) function_index.csv
n = 0
with open(os.path.join(exp, "functions.csv"), newline="", encoding="utf-8") as f, \
     open(os.path.join(exp, "function_index.csv"), "w", newline="", encoding="utf-8") as o:
    r = csv.DictReader(f)
    w = csv.writer(o)
    w.writerow(r.fieldnames + ["decomp_file", "line_start", "line_end"])
    for row in r:
        d = idx.get(row["address"], ("", "", ""))
        w.writerow([row[k] for k in r.fieldnames] + list(d)); n += 1

# 3) por classe
def safe(s):
    s = re.sub(r"[^A-Za-z0-9_.+-]", "_", s)
    if len(s) > 110:
        s = s[:100] + "_" + hashlib.md5(s.encode()).hexdigest()[:8]
    return s

cache = {}
def lines_of(fn):
    if fn not in cache:
        if len(cache) > 8: cache.clear()
        with open(os.path.join(dec, fn), encoding="utf-8", errors="replace") as f:
            cache[fn] = f.read().split("\n")
    return cache[fn]

groups = collections.defaultdict(list)   # classe -> [(addr, name, fn, a, b)]
for addr, name, fn, a, b in rows:
    if "::" in name and not name.startswith("Catch@"):
        groups[name.rsplit("::", 1)[0]].append((addr, name, fn, a, b))

index_md = ["# Classes (funcoes decompiladas agrupadas)\n", "| classe | funcoes | arquivo |", "|---|---|---|"]
for cls in sorted(groups):
    fname = safe(cls) + ".c"
    with open(os.path.join(bycls, fname), "w", encoding="utf-8") as o:
        o.write(f"// Classe: {cls}  ({len(groups[cls])} funcoes)\n")
        for addr, name, fn, a, b in groups[cls]:
            ls = lines_of(fn)
            o.write("\n".join(ls[a - 1:b]) + "\n")
    index_md.append(f"| `{cls}` | {len(groups[cls])} | [{fname}]({fname}) |")
with open(os.path.join(bycls, "_index.md"), "w", encoding="utf-8") as o:
    o.write("\n".join(index_md) + "\n")

# 4) README
named = sum(1 for r in rows if not r[1].startswith("FUN_"))
readme = f"""# Sims2EP9.exe - saida do Ghidra

Gerado por `tools/ghidra/run_decompile.sh`. Imagem base do exe no Ghidra: `0xa0000000`.

## Numeros
- funcoes decompiladas: {len(rows)}  (com nome diferente de `FUN_*`: {named})
- classes com funcoes agrupadas em `by_class/`: {len(groups)}

## Onde esta cada coisa
| caminho | conteudo |
|---|---|
| `decomp_named/*.c` | C decompilado, 500 funcoes por arquivo, **ja com nomes RTTI** (`Classe::vfunc_N`, `Classe::ctor_dtor_<addr>`) |
| `by_class/<Classe>.c` | todas as funcoes de uma classe juntas; `by_class/_index.md` lista tudo |
| `export/function_index.csv` | tabela mestra: endereco, nome, classe, tamanho, assinatura, n_callers/callees/strings/imports, slot de vtable, **arquivo e linhas no C** |
| `export/functions_detail.jsonl` | por funcao: callers, callees, strings usadas, imports chamados, vtables (uma linha JSON por funcao) |
| `export/calls.csv` | grafo de chamadas diretas (caller,callee) |
| `export/classes.json` | cada classe RTTI: bases diretas/todas, vftables com slots (indice:endereco:funcao), candidatos a ctor/dtor |
| `export/classes_tree.txt` | resumo legivel: `Classe [vfuncs, vftables] : Base` |
| `export/strings.csv` | strings (definidas pelo Ghidra e ASCII soltas referenciadas pelo codigo), com as funcoes que as usam |
| `export/imports.csv` | imports (DLL, nome) e funcoes que os chamam |
| `export/globals.csv` | variaveis globais nomeadas e quantas funcoes as usam |
| `export/datatypes.h` | structs/enums/typedefs do projeto em C |
| `export/memory_map.csv` | secoes do PE |
| `decomp/` | 1a decompilacao (nomes `FUN_*`), pode apagar |
| `project/` | projeto Ghidra (abre na GUI com `ghidra`); ja com os nomes aplicados |
| `backup/project.pre_names/` | projeto antes da renomeacao automatica |

## Como achar algo
- por nome de classe: `rg "cSomething" docs/ghidra/export/classes_tree.txt`, depois `docs/ghidra/by_class/cSomething.c`
- por texto/string do jogo: `rg -i "family_%s" docs/ghidra/export/strings.csv` -> pegue o endereco da funcao -> `function_index.csv` diz arquivo e linha
- por endereco: `rg "^a0144431," docs/ghidra/export/function_index.csv`

## Limites
- pseudo-C do Ghidra: tipos genericos (`undefined4`), nao compila direto.
- so metodos virtuais (via vtable) e construtores/destrutores ganharam nome automatico; o resto segue `FUN_<endereco>`.
- ASCII soltas so aparecem se referenciadas diretamente por codigo.
"""
with open(os.path.join(root, "README.md"), "w", encoding="utf-8") as o:
    o.write(readme)
print("ok:", len(rows), "funcoes;", len(groups), "classes")
