# Engenharia Reversa do `Sims2EP9.exe` (v1.0.0.153) & `ijl15.dll`

Este documento consolida todas as evidências estáticas e decompilações obtidas via **REA MCP (`user-rea`) + Ghidra 12.1.2 (`/opt/ghidra`) + objdump/RTTI Extractor** nos binários instalados em `/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/`.

---

## 1. Identidade dos Binários e Evidências REA/Ghidra

| Binário | Caminho | SHA-256 | Arquitetura | Image Base | Evidências REA / Ghidra |
|---|---|---|---|---|---|
| `Sims2EP9.exe` | `/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/Sims2EP9.exe` | `48691c4a382671f033f7dec4076a7444471ecfefaaa4d4170e56face62c05c130` | `pei-i386` (32-bit x86) | `0xA0000000` | `ev_592ed9ec353b48ba8a959a4f97fd33e8c4be8737823d765c3950e8402cd9660a` (File Headers)<br>`ev_9d61bb21f545779061175ea2954fe09a56d26d67f3afe378941f5d7a29bcddcf` (Sections)<br>`ev_0627b44721093fdde1965eb38aac1077617f2b2fe6e316153c7f62d7d019c5f8` (PE Resources `1.0.0.153`) |
| `ijl15.dll` | `/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/ijl15.dll` | `41d37bfb0a39017497e0aa373dc01f71ac9c5046c75f7cd269bee52112a5f3a1` | `pei-i386` (32-bit x86) | `0x60000000` | `ev_ecbd771ba3ef5bd05ef93e47149bdc9685dd4048560b06541247184d08a35b0f` (Ghidra `binary_overview`)<br>`ev_5821b6408f7c30bad04411afaec0203ed8c4c57ada080fd87bcad4664b381b4e` (Ghidra `search_strings`)<br>`ev_6cc767112786873dd7cc46278a859a5aa6533bfaf1fb177237a098ae141e7ff8` (Ghidra `analyze_function` `ijlInit`)<br>`ev_264c1e4da6f68d8747f35bf1f4850266b7313be0b7aaa98924e63cd82dd1e84a` (Ghidra `procedure_pseudo_code` `ijlRead`)<br>Snapshot: `docs/ijl15.rea-snapshot` |

### Seções PE de `Sims2EP9.exe` (`ev_9d61bb21f545779061175ea2954fe09a56d26d67f3afe378941f5d7a29bcddcf`)
- `.text`: VMA `0xA0001000`, Tamanho `0x00F471EE` (~16.0 MB), Entry Point `0xA09BF128`
- `.rdata`: VMA `0xA0F49000`, Tamanho `0x0029C9A8` (~2.7 MB) — Contém IAT (`0xA0F49000..0xA0F49A80`) e 7.025 VTables C++
- `.data`: VMA `0xA11E6000`, Tamanho `0x000FCE00` (~1.0 MB) — Contém 5.148 `TypeDescriptors` MSVC RTTI e ponteiros globais
- `.rsrc`: VMA `0xA132C000`, Tamanho `0x00002F38`
- `.reloc`: VMA `0xA132F000`, Tamanho `0x000E6084`

### Tabela de Exports de `ijl15.dll` (`ev_6cc767112786873dd7cc46278a859a5aa6533bfaf1fb177237a098ae141e7ff8`)
| Ordinal | Nome | Endereço (`ijl15.dll`) | Assinatura (`__stdcall`) |
|---|---|---|---|
| 1 | `ijlGetLibVersion` | `0x6002B110` | `const void* __stdcall ijlGetLibVersion(void)` |
| 2 | `ijlInit` | `0x60028A60` | `int __stdcall ijlInit(void* jcprops)` |
| 3 | `ijlFree` | `0x600291F0` | `int __stdcall ijlFree(void* jcprops)` |
| 4 | `ijlRead` | `0x60029BE0` | `int __stdcall ijlRead(void* jcprops, int iotype)` |
| 5 | `ijlWrite` | `0x6002A700` | `int __stdcall ijlWrite(void* jcprops, int iotype)` |
| 6 | `ijlErrorStr` | `0x6002B120` | `const char* __stdcall ijlErrorStr(int code)` |

---

## 2. Framework Гонzo/Rizzo (`cGZFramework`, `cGZCOM`, `cTSGlobals`)

### Funções de Acesso Global
| Símbolo / Função | Endereço (VMA) | Convenção | Descrição |
|---|---|---|---|
| `GZFramework()` / `GZCOM()` | `0xA014350E` (-> `0xA0142A85`) | `__cdecl` | Retorna o singleton `cIGZCOM*` / `cIGZFrameWork*`. Método `GetClassObject(clsid, iid, void**)` está em `vtable + 0x14`. |
| `TSGetGlobals()` | `0xA020E0CB` | `__cdecl` | Retorna `cITSGlobals*` (cacheado em `.data` `0xA12E5D70`, `clsid = 0x9A353CE8`, `iid = 0xFA353CDB`). |
| `TSGetSimulator()` | `0xA020E08F` | `__cdecl` | Retorna `cITSSimulator*` (`cTSGlobals + 0x0C`). |
| `TSGetLot()` | `0xA020E0B0` | `__cdecl` | Retorna `cITSLot*` (`cTSGlobals + 0x24`). |
| `TSGetNeighborhoodMgr()` | `0xA020E0F6` | `__cdecl` | Retorna `cITSNeighborhoodManager*` (`cTSGlobals + 0x48`). |
| `TSGetObjectFolder()` | `0xA020E111` | `__cdecl` | Retorna `cITSObjectFolder*` (`cTSGlobals + 0x68`). |
| `TSGetPersistSystem()` | `0xA020E06E` | `__cdecl` | Retorna `cITSPersistSystem*` (`cTSGlobals + 0x4C`). |
| `TSGetSimSystem()` | `0xA020E23F` | `__cdecl` | Retorna o gerenciador central onde `+0xDC` é o PRNG global `cRZRandom*`. |

### GUIDs de Serviços COM (`cIGZCOM::GetClassObject`)
- **Global Property Set (`cIGZPropertySet` / `GlobalProps`)**: `clsid = 0x0AC1887E`, `iid = 0x4BB0D269`
- **`cTSGlobals`**: `clsid = 0x9A353CE8`, `iid = 0xFA353CDB`
- **`cTSInteraction`**: `clsid = 0xFC5ECC3D`, `iid = 0x5ACA6692`

---

## 3. Loop de Mensagens, Simulação em Background e Múltiplas Instâncias

### `cGZFrameworkW32::Run` (`0xA0143891`)
- O campo `BYTE [esi + 0xCC]` em `cGZFrameworkW32` indica se a janela está ativa (`m_bAppActive`).
- **`0xA014389D`**:
  ```asm
  a014389d: 38 9e cc 00 00 00    cmp BYTE PTR [esi+0xcc], bl
  a01438a3: 75 22                jne 0xa01438c7
  a01438a5: ff 15 20 96 f4 a0    call DWORD PTR ds:0xa0f49620 ; USER32.WaitMessage()
  ```
  Quando a janela perde foco (`[esi+0xCC] == 0`), o jogo chama `WaitMessage()`, congelando a thread principal até receber mensagens de janela.
- **`0xA01438DA`**:
  ```asm
  a01438da: 38 9e cc 00 00 00    cmp BYTE PTR [esi+0xcc], bl
  a01438e0: 74 07                je  0xa01438e9
  a01438e2: 8b 06                mov eax, DWORD PTR [esi]
  a01438e4: 8b ce                mov ecx, esi
  a01438e6: ff 50 40             call DWORD PTR [eax+0x40]    ; cGZFramework::OnIdle()
  ```
- **Patches de Simulação em Background (essencial para Co-op Local e Alt-Tab)**:
  1. Em `0xA01438A3`: substituir `75 22` (`jne +0x22`) por `EB 22` (`jmp +0x22`), nunca chamando `WaitMessage()`.
  2. Em `0xA01438E0`: substituir `74 07` (`je +0x07`) por `90 90` (`nop; nop`), garantindo que `OnIdle()` continue rodando com a janela em segundo plano.

### Check de Múltiplas Instâncias (`cTSApp` @ `0xA0DC1BB9..0xA0DC1CE9` & `cRZSysMutex` @ `0xA0E06DC8`)
- Strings verificadas na inicialização (`cTSApp`, `vtable = 0xA106FB74`):
  - `0xA106FD08`: `"Global\\The Sims 2"`
  - `0xA106FCEC`: `"Global\\The Sims 2 Body Shop"`
  - `0xA106FCAC`: `"EA_LAUNCHER_SHUTDOWN_REQUEST_SIMS2EP"`
- Em `0xA0DC1CAE`:
  ```asm
  a0dc1cae: 84 db                test bl, bl
  a0dc1cb0: 74 08                je   0xa0dc1cba
  a0dc1cb2: 66 c7 47 6d 00 00    mov  WORD PTR [edi+0x6d], 0x0
  a0dc1cb8: eb 24                jmp  0xa0dc1cde
  ```
- **Desbloqueio de Múltiplas Instâncias**:
  1. Patch em `0xA0DC1CB0` (`74 08` -> `90 90`), forçando `WORD PTR [edi+0x6d] = 0` (ignora o `TryLock` dos mutexes `"Global\\The Sims 2"` e `"Global\\The Sims 2 Body Shop"`).
  2. Hook em `KERNEL32.CreateMutexA` (`IAT 0xA0F493AC`) anexando `_%PID%` ao nome do mutex e zerando `ERROR_ALREADY_EXISTS`.

---

## 4. Simulador de Lote (`cTSSimulator`) e PRNG Determinístico (`cRZRandom`)

### VTables e Alocação de `cTSSimulator`
- Construtor: `0xA04804F0` (tamanho `0x128` bytes, alocado em `0xA0480AC9`).
- VTable 0 (`+0x00`, `cIGZMessageTarget2`): `0xA0F79A0C`
- VTable 1 (`+0x04`, `cITSSimulator`, 55 métodos): `0xA0F79A24`

### Layout de Campos de `cTSSimulator` (relativo a `cITSSimulator*` = `cTSSimulator + 0x04`)
| Offset | Tipo | Nome | Descrição |
|---|---|---|---|
| `+0x10` | `uint16_t` | `m_uHour` | Hora Sim atual (`0..23`) |
| `+0x12` | `uint16_t` | `m_uDay` | Dia do mês (`1..30`) |
| `+0x1A` | `uint16_t` | `m_uMinute` | Minuto Sim atual (`0..59`) |
| `+0x1C` | `uint16_t` | `m_uSubTick` | Segundo/SubTick (`0..58`, soma +2 por tick em `0xA048305B`; 30 ticks = 1 min Sim) |
| `+0x1E` | `uint16_t` | `m_uMonth` | Mês (`1..12`) |
| `+0x20` | `uint16_t` | `m_uYear` | Ano (inicializado com `1997` / `0x07CD`) |
| `+0x30` | `int16_t` | `m_nSimulatorState` | Modo/Velocidade (`1` = Live, `2` = Buy, `3` = Build, etc.) |
| `+0x32` | `int16_t` | `m_nPauseCount` | Flag de pausa (`> 0` = Pausado) |
| `+0x36` | `int16_t` | `m_nSpeedMultiplier` | Multiplicador de velocidade (Normal/Fast/Ultra) |
| `+0x88` | `uint64_t` | `m_uTotalTicks` | Contador de ticks acumulados (64 bits) |
| `+0x90` | `uint32_t` | `m_uLotTickCount` | Contador de ticks do lote atual (incrementado em `0xA0483046`) |
| `+0xA4` | `uint32_t` | `m_uRandomSeed` | **Seed determinística do PRNG do simulador!** |
| `+0x114` | `uint32_t` | `m_uCurrentObjectID` | ObjectID sendo simulado no loop de `SimulateAllObjects` |

### Métodos Chave de `cITSSimulator` (`0xA0F79A24`)
- `vtable[10]` (`+0x28`): `0xA04829B0` — `cTSSimulator::OnIdle()`
- `vtable[11]` (`+0x2C`): `0xA0483390` — `cTSSimulator::SimulateAllObjects()`
- `vtable[12]` (`+0x30`): `0xA0482F90` — `cTSSimulator::SimulateTick()`
- `vtable[18]` (`+0x48`): `0xA024C050` — `uint32_t GetTickCount()` (`return [ecx+0x90]`)
- `vtable[20]` (`+0x50`): `0xA0482730` — `void SetSimulatorState(int state, int flags)`
- `vtable[21]` (`+0x54`): `0xA0480F20` — `int16_t GetSimulatorState()`
- `vtable[24]` (`+0x60`): `0xA0481490` — `void Pause(bool)`
- `vtable[25]` (`+0x64`): `0xA0482180` — `void Unpause(bool)`
- `vtable[26]` (`+0x68`): `0xA0481220` — `bool IsPaused()`
- `vtable[27]` (`+0x6C`): `0xA0480D90` — `int16_t GetSpeedMultiplier()`
- `vtable[28]` (`+0x70`): `0xA04826C0` — `void SetSpeedMultiplier(int speed)`

### PRNG (`cRZRandom`)
- `0xA01B919B`: `uint32_t __thiscall cRZRandom::RandomUint32()` (polinômio LCG `seed * 0x0019660D + 0x3C6EF35F`).
- `0xA01B91B4`: `int32_t __thiscall cRZRandom::RandomRange(int32_t max)`.
- No início de `cTSSimulator::SimulateTick` (`0xA0482F90`), o jogo copia `cTSSimulator::m_uRandomSeed` (`[esi+0xA4]`) para o `cRZRandom` global, executa `SimulateAllObjects` (`0xA0483390`), e ao final copia a seed resultante de volta para `[esi+0xA4]`.

---

## 5. Máquina Virtual SimAntics (`cTSTreeSim`, `cTSObject`, `cTSTreeStackElem`)

### Despacho de BHAV e Primitivas (`0xA0576500`, `0xA053E780`, `0xA0534880`)
1. **`cTSTreeSim::ExecuteStep` (`0xA0576500`, `vtable[13]`)**:
   - Assinatura: `int __thiscall ExecuteStep(cTSTreeSim* this, cTSTreeStackElem* pStackElem)`
   - Limite global de iterações por tick: `DWORD 0xA1211914` (padrão `10000`, `0x2710`).
   - Obtém o opcode do nó BHAV atual em `0xA0576778`: `uint16_t opcode = pNode->GetOpcode()` (`vtable + 0x0C`).
   - Se `opcode < 0x0100` (`jl 0xA0576AFD` em `0xA0576783`), trata-se de uma **Primitiva SimAntics** e chama `this->ExecutePrimitive(pStackElem, pNode)` (`vtable[6]`, que em `cTSObject`/`cTSPerson` é o thunk `0xA0521A8D -> 0xA053E780`).
2. **`cTSObject::ExecutePrimitive` (`0xA053E780`)**:
   - Assinatura: `int __thiscall ExecutePrimitive(cTSObject* this, cTSTreeStackElem* pStackElem, cTSBehaviorTreeNode* pNode)`
   - Retornos: `0` = True branch, `1` = False branch, `2` = Yield/Wait, `3` = Error/Reset.
   - Chama `cEdithObject::ExecutePrimitive` (`0xA0579440`, `vtable+0x98`) e, se não tratado, chama `this->ExecuteObjectPrimitive(pStackElem, pNode, &result)` (`vtable+0x530`: `0xA0534880` em `cTSObject`, `0xA051E700` em `cTSPerson`).
3. **`cTSObject::ExecuteObjectPrimitive` (`0xA0534880`)**:
   - Assinatura: `bool __thiscall ExecuteObjectPrimitive(cTSObject* this, cTSTreeStackElem* pStackElem, cTSBehaviorTreeNode* pNode, int* pOutResult)`
   - Em `0xA05348C0`: `cmp eax, 0x7E; ja 0xA0534BFF`.
   - As primitivas nativas do jogo vão de **`0x0000` a `0x007E`** (tabela de índices em `0xA0534CDC`, jump table em `0xA0534C34`).
   - **Qualquer opcode `0x007F..0x00FF` (incluindo `0x0080+`) é livre para extensão pelo `TS2SE`!**

### Layout de `cTSTreeStackElem` (`vtable = 0xA0F867AC`)
| Offset | Tipo | Nome | Descrição |
|---|---|---|---|
| `+0x08` | `uint16_t` | `m_uStackObjectID` | ID do Stack Object atual (`& 0x7FFF`) |
| `+0x0A` | `uint16_t` | `m_uTargetObjectID` | ID do objeto alvo |
| `+0x0C` | `uint16_t` | `m_uCurrentNodeID` | Índice do nó BHAV atual (`0xFFFD` = True return, `0xFFFE` = False return) |
| `+0x10` | `cITSBehaviorTree*` | `m_pBehaviorTree` | Ponteiro para a árvore BHAV em execução |
| `+0x14` | `uint8_t` | `m_uNumLocals` | Quantidade de variáveis locais da frame |
| `+0x15` | `uint8_t` | `m_uNumParams` | Quantidade de parâmetros da frame |
| `+0x24` | `int16_t*` | `m_pParams` | Array de parâmetros (`int16_t[m_uNumParams]`) |
| `+0x30` | `int16_t*` | `m_pLocals` | Array de variáveis locais (`int16_t[m_uNumLocals]`) |

---

## 6. Fila de Interações (`cTSInteractionContainer`), Motivos (`cTSMotives`) e Cheats (`cTSCheatSystem`)

### `cTSInteractionContainer` (`vtable = 0xA0F7751C`)
- `+0x08`: `cITSInteraction** m_pBegin`
- `+0x0C`: `cITSInteraction** m_pEnd`
- `0xA044F9B0` (`vtable[3]`): `bool __thiscall PushInteraction(cTSInteractionContainer* this, cITSInteraction* pInteraction)`
- `0xA0451780` (`vtable[4]`): `cITSInteraction* __thiscall GetInteractionAt(cTSInteractionContainer* this, int index)`
- `0xA0153DA0` (`vtable[5]`): `int __thiscall GetCount(cTSInteractionContainer* this)`
- `0xA044FA30`: `bool __thiscall EnqueueNewInteraction(cTSInteractionContainer* this, cITSObject* pTarget, cITSPerson* pActor, int interactionIndex, int priorityAndFlags)`

### `cTSMotives` (`vtable = 0xA0F73B3C`)
- `+0x48`: `float m_fMotives[16]` (escala `-100.0f` a `+100.0f`)
- `0xA0436B70` (`vtable[10]`): `float __thiscall GetMotive(cTSMotives* this, int index)`
- `0xA0437000` (`vtable[11]`): `void __thiscall SetMotive(cTSMotives* this, int index, float value)`

### `cTSCheatSystem` (`vtable = 0xA108F264`)
- `0xA0CA1C20`: `bool __thiscall RegisterCheatCommand(...)`
- `0xA0E589B0` / `0xA0C9F770`: `bool __thiscall ExecuteCheatString(void* pCheatParser, const char* szCommand)`

### Direct3D 9 (`d3d9.dll`) & Window Procedure
- IAT `Direct3DCreate9`: `0xA0F4974C` (thunk `JMP` em `0xA09F5758`, chamado em `cGZD3D9Driver` `0xA0864CE6`).
- IAT `CreateWindowExA`: `0xA0F496B0`.
