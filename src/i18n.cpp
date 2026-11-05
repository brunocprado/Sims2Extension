#include "ts2/i18n.hpp"
#include "ts2/win32_abi.hpp"


namespace ts2::i18n {

namespace {

struct Entry { const char* pt; const char* en; };

// Keys must match the Portuguese literal in the source EXACTLY (placeholders included).
const Entry kTable[] = {
    // ---- header / tabs / footer -------------------------------------------------------------
    {"Construir", "Build"},
    {"Jogo", "Game"},
    {"Gráficos", "Graphics"},
    {"Avançado", "Advanced"},
    {"F10 ocultar  •  F11 tela cheia  •  F5 salvar  •  F9 sincronizar  •  F8 chat",
     "F10 hide  •  F11 fullscreen  •  F5 save  •  F9 sync  •  F8 chat"},

    // ---- build catalog -----------------------------------------------------------------------
    {"F10 ocultar  •  F11 tela cheia  •  F5 salvar  •  F9 sincronizar  •  F8 chat  •  F7 catálogo",
     "F10 hide  •  F11 fullscreen  •  F5 save  •  F9 sync  •  F8 chat  •  F7 catalog"},
    {"CATÁLOGO DE OBJETOS", "OBJECT CATALOG"},
    {"%u objetos no catálogo (%u de mods)", "%u objects in the catalog (%u from mods)"},
    {"Catálogo não encontrado (rode tools/catalog/build_catalog.py)", "Catalog not found (run tools/catalog/build_catalog.py)"},
    {"Abrir catálogo (F7)", "Open catalog (F7)"},
    {"Catálogo de construção", "Build catalog"},
    {"Catálogo não encontrado.", "Catalog not found."},
    {"Gere com tools/catalog/build_catalog.py e copie ts2catalog.bin e ts2thumbs.bin",
     "Generate it with tools/catalog/build_catalog.py and copy ts2catalog.bin and ts2thumbs.bin"},
    {"para a pasta do jogo (TSBin).", "to the game folder (TSBin)."},
    {"%u objetos", "%u objects"},
    {"Buscar por nome...", "Search by name..."},
    {"Tudo", "All"},
    {"Jogo base", "Base game"},
    {"Mods (%u)", "Mods (%u)"},
    {"Nenhum objeto encontrado.", "No objects found."},
    {"Nenhum mod no catálogo. Rode build_catalog.py com a pasta Downloads (--mods).",
     "No mods in the catalog. Run build_catalog.py with your Downloads folder (--mods)."},
    {"Selecione um objeto na lista.", "Pick an object from the list."},
    {"Categoria: %s", "Category: %s"},
    {"Origem: mod (%s)", "Source: mod (%s)"},
    {"Origem: jogo base", "Source: base game"},
    {"Colocar no lote", "Place on lot"},
    {"Desfazer último (%u)", "Undo last (%u)"},
    {"Rotação: %d°", "Rotation: %d°"},
    {"Deslocamento X: %d", "Offset X: %d"},
    {"Deslocamento Y: %d", "Offset Y: %d"},
    {"ÚLTIMO OBJETO COLOCADO (%u)", "LAST PLACED OBJECT (%u)"},
    {"Remover", "Remove"},
    {"Clique no objeto e depois no lote  •  Roda do mouse rola  •  Esc fecha", "Click an object, then the lot  •  Mouse wheel scrolls  •  Esc closes"},
    {"Posicionando:", "Placing:"},
    {"Clique no lote para colocar  •  Shift: continuar colocando  •  Q / E gira  •  Botão direito ou Esc cancela", "Click the lot to place  •  Shift: keep placing  •  Q / E rotate  •  Right-click or Esc cancels"},
    {"Clique no lote para posicionar.", "Click the lot to position it."},
    {"Posicionamento cancelado.", "Placement cancelled."},
    {"Objeto colocado.", "Object placed."},
    {"Visão do lote indisponível: objeto colocado ao lado do Sim.", "Lot view unavailable: object placed next to the Sim."},
    {"Somente o anfitrião pode colocar objetos.", "Only the host can place objects."},
    {"Colocar objetos indisponível aqui.", "Placing objects is not available here."},
    {"Selecione um Sim no lote para posicionar o objeto.", "Select a Sim on the lot to position the object."},
    {"Posição fora do lote.", "Position is outside the lot."},
    {"Não foi possível criar o objeto %08X.", "Could not create object %08X."},
    {"Objeto %08X colocado ao lado do Sim selecionado.", "Object %08X placed next to the selected Sim."},
    {"Nenhum objeto colocado ainda.", "No object placed yet."},
    {"O último objeto colocado não existe mais.", "The last placed object no longer exists."},
    {"Objeto removido.", "Object removed."},
    {"Não foi possível remover o objeto.", "Could not remove the object."},
    {"Objeto movido.", "Object moved."},
    {"Não foi possível mover o objeto.", "Could not move the object."},
    {"Abra um lote para colocar objetos.", "Open a lot to place objects."},

    // ---- tab: Sim ----------------------------------------------------------------------------
    {"TEMPO", "TIME"},
    {"Pausado", "Paused"},
    {"Pausar", "Pause"},
    {"Velocidade 1x", "Speed 1x"},
    {"Velocidade 2x", "Speed 2x"},
    {"Velocidade 3x", "Speed 3x"},
    {"Manha (07h)", "Morning (7am)"},
    {"Meio-dia (12h)", "Noon (12pm)"},
    {"Tarde (18h)", "Evening (6pm)"},
    {"Noite (00h)", "Night (12am)"},
    {"DINHEIRO DA FAMILIA", "HOUSEHOLD FUNDS"},
    {"+ $1.000", "+ $1,000"},
    {"+ $50.000", "+ $50,000"},
    {"+ $250.000", "+ $250,000"},
    {"- $10.000", "- $10,000"},
    {"SIM SELECIONADO", "SELECTED SIM"},
    {"Encher necessidades", "Fill needs"},
    {"Congelar necessidades", "Freeze needs"},
    {"Congelar aspiracao", "Freeze aspiration"},
    {"Aspiracao Platina", "Platinum aspiration"},
    {"Aspiracao Ouro", "Gold aspiration"},
    {"+50.000 pts aspir.", "+50,000 aspir. pts"},
    {"Trocar Sim ativo", "Switch active Sim"},
    {"NECESSIDADES", "NEEDS"},
    {"Fome", "Hunger"},
    {"Conforto", "Comfort"},
    {"Higiene", "Hygiene"},
    {"Banheiro", "Bladder"},
    {"Energia", "Energy"},
    {"Diversão", "Fun"},
    {"Ambiente", "Environment"},

    // ---- tab: Build --------------------------------------------------------------------------
    {"COLOCAR OBJETOS", "PLACING OBJECTS"},
    {"Mover objetos livremente", "Move objects freely"},
    {"Rotacao de 45 graus", "45-degree rotation"},
    {"Posicao em 1/4 de tile", "Quarter-tile placement"},
    {"Fora da grade (sem snap)", "Off-grid (no snap)"},
    {"Terreno e piso livres", "Free terrain & floors"},
    {"Construir em dorm/apto", "Build in dorm/apartment"},
    {"LIMITES DO LOTE", "LOT LIMITS"},
    {"%d andares", "%d floors"},
    {"Telhado 15 graus", "Roof 15 degrees"},
    {"Telhado 30 graus", "Roof 30 degrees"},
    {"Telhado 45 graus", "Roof 45 degrees"},
    {"Zona residencial", "Residential zone"},
    {"Zona comunitaria", "Community zone"},
    {"Zona apartamento", "Apartment zone"},
    {"BUSCAR NO CATALOGO", "CATALOG SEARCH"},
    {"clique e digite...", "click and type..."},
    {"Limpar", "Clear"},

    // ---- tab: Game ---------------------------------------------------------------------------
    {"JANELA E DESEMPENHO", "WINDOW & PERFORMANCE"},
    {"Tela cheia sem bordas (F11)", "Borderless fullscreen (F11)"},
    {"Continuar rodando em 2o plano", "Keep running in background"},
    {"Desbloquear FPS (sem VSync)", "Unlock FPS (no VSync)"},
    {"Cheats de teste (Shift+clique)", "Testing cheats (Shift+click)"},
    {"CAMERA E VISUAL", "CAMERA & VISUALS"},
    {"Esconder diamante do Sim", "Hide Sim plumbob"},
    {"Esconder baloes de pensamento", "Hide thought bubbles"},
    {"SALVAMENTO", "SAVING"},
    {"proximo em %u s", "next in %u s"},
    {"desligado", "off"},
    {"A cada 5 min", "Every 5 min"},
    {"A cada 15 min", "Every 15 min"},
    {"Salvar agora (F5)", "Save now (F5)"},
    {"Atalhos:  F10 mostrar/ocultar painel   F11 tela cheia   F5 salvar   F9 sincronizar co-op",
     "Shortcuts:  F10 show/hide panel   F11 fullscreen   F5 save   F9 sync co-op"},

    // ---- tab: Co-op --------------------------------------------------------------------------
    {"CONVIDADO", "GUEST"},
    {"parceiro conectado", "partner connected"},
    {"nao conectado", "not connected"},
    {"aguardando parceiro...", "waiting for partner..."},
    {"CONEXAO", "CONNECTION"},
    {"Hospedar", "Host"},
    {"Conectar", "Connect"},
    {"Desconectar", "Disconnect"},
    {"Ao entrar, o convidado espelha o lote do Host (moveis e Sims). O save do convidado nao importa.",
     "On joining, the guest mirrors the Host's lot (furniture and Sims). The guest's save doesn't matter."},
    {"SINCRONIA", "SYNC"},
    {"Sincronizar (F9)", "Sync (F9)"},
    {"Moveis: ON", "Furniture: ON"},
    {"Moveis: OFF", "Furniture: OFF"},
    {"Espelhar lote", "Mirror lot"},
    {"Trocar meu Sim", "Switch my Sim"},
    {"Pausar (ambos)", "Pause (both)"},
    {"Retomar (ambos)", "Resume (both)"},
    {"Sims & Acoes: ON", "Sims & Actions: ON"},
    {"Sims & Acoes: OFF", "Sims & Actions: OFF"},
    {"IA do convidado: silenciada", "Guest AI: muted"},
    {"IA do convidado: livre", "Guest AI: free"},
    {"Moveis: %u monit. | movidos %u/%u | criados %u | vendidos %u | conflitos %u%s",
     "Furniture: %u tracked | moved %u/%u | created %u | sold %u | conflicts %u%s"},
    {" (abra lote)", " (open a lot)"},
    {"Sims: meu slot %d (NID %d) | parceiro %d (NID %d) | poses %u/%u | motivos %u",
     "Sims: my slot %d (NID %d) | partner %d (NID %d) | poses %u/%u | motives %u"},
    {"Interacoes Pie Menu sincronizadas: %u (ultima: menu=%d alvo=#%u) | BHAV: %u",
     "Pie Menu interactions synced: %u (last: menu=%d target=#%u) | BHAV: %u"},
    {"Fundos local $%d / host $%d | Tick peer %u | CRC %08X | divergencias: %u",
     "Funds local $%d / host $%d | Peer tick %u | CRC %08X | desyncs: %u"},
    {"Host: clique em Hospedar. Convidado: no MENU PRINCIPAL, clique em Conectar.",
     "Host: click Host. Guest: on the MAIN MENU, click Connect."},
    {"Sincronia completa: Relogio/SubTick, PRNG, Fundos, Sims (pose + motivos + NeighborID),",
     "Full sync: Clock/SubTick, PRNG, Funds, Sims (pose + motives + NeighborID),"},
    {"Fila de Interacoes (Pie Menu / Ir Aqui) e Compra/Venda/Movimento de Moveis!",
     "Interaction queue (Pie Menu / Go Here) and furniture Buy/Sell/Move!"},

    // ---- tab: Advanced -----------------------------------------------------------------------
    {"Premios de carreira", "Career rewards"},
    {"Forcar gemeos", "Force twins"},
    {"Envelhecimento", "Aging"},
    {"ATUAL: %u", "CURRENT: %u"},
    {"LIMITE DE SIMS NO LOTE", "SIMS PER LOT LIMIT"},
    {"%u (padrao)", "%u (default)"},
    {"LIMITE DO SIMANTICS (EVITA 'TRAVOU')", "SIMANTICS LIMIT (PREVENTS 'FROZEN')"},
    {"Executar", "Run"},
    {"Digite um comando ou cheat e clique em [Executar].", "Type a command or cheat and click [Run]."},

    // ---- tab: Graphics -----------------------------------------------------------------------
    {"Efeitos", "Effects"},
    {"Cor", "Color"},
    {"Foco (DoF)", "Focus (DoF)"},
    {"Sombras", "Shadows"},
    {"Suave", "Soft"},
    {"Equilibrado", "Balanced"},
    {"Cinematográfico", "Cinematic"},
    {"Efeitos ligados", "Effects on"},
    {"Efeitos desligados", "Effects off"},
    {"EFEITOS", "EFFECTS"},
    {"salvo automaticamente em ts2fx.cfg", "auto-saved to ts2fx.cfg"},
    {"Oclusão (AO)", "Occlusion (AO)"},
    {"Névoa", "Fog"},
    {"Raios de luz", "Light shafts"},
    {"Reflexos (SSR)", "Reflections (SSR)"},
    {"Vinheta", "Vignette"},
    {"Saturação", "Saturation"},
    {"Contraste", "Contrast"},
    {"Padrão", "Default"},
    {"Diag: %u draws 3D depois do pos-processo (%u escrevem profundidade)",
     "Diag: %u 3D draws after post-process (%u write depth)"},
    {"COR", "COLOR"},
    {"0 = neutro", "0 = neutral"},
    {"Cor por hora", "Time-of-day color"},
    {"Temperatura (frio/quente)", "Temperature (cool/warm)"},
    {"Matiz (verde/magenta)", "Tint (green/magenta)"},
    {"Pretos (lift)", "Blacks (lift)"},
    {"Brilhos (gain)", "Highlights (gain)"},
    {"Tom dividido (frio/quente)", "Split tone (cool/warm)"},
    {"Vibração", "Vibrance"},
    {"PROFUNDIDADE DE CAMPO", "DEPTH OF FIELD"},
    {"desfoca o que está fora de foco", "blurs what is out of focus"},
    {"Profundidade de campo", "Depth of field"},
    {"Foco automático (centro)", "Auto focus (center)"},
    {"Tilt-shift (miniatura)", "Tilt-shift (miniature)"},
    {"Raio máximo (px a 1080p)", "Max radius (px at 1080p)"},
    {"Zona nítida", "Sharp zone"},
    {"Foco automático: a distância mira o objeto no centro da tela. Desligado, o controle ao lado da chave é a distância fixa.",
     "Auto focus: the distance targets the object at the screen center. When off, the slider next to the switch is a fixed distance."},
    {"Tilt-shift: ignora a profundidade e deixa nítida só uma faixa horizontal (o controle é a altura da faixa).",
     "Tilt-shift: ignores depth and keeps only a horizontal band sharp (the slider is the band height)."},
    {"SOMBRAS", "SHADOWS"},
    {"calculadas na resolução da tela", "computed at screen resolution"},
    {"Sombras do sol", "Sun shadows"},
    {"Suavidade (penumbra)", "Softness (penumbra)"},
    {"Alcance", "Range"},
    {"Sombras longas com sol baixo", "Long shadows at low sun"},
    {"O jogo desenha as próprias sombras como geometria projetada (não usa mapa de sombras), então não há textura de sombra para aumentar.",
     "The game draws its own shadows as projected geometry (no shadow map), so there is no shadow texture to enlarge."},
    {"As sombras do sol acima são calculadas na resolução da tela. Suavidade controla a largura da penumbra; Alcance, o comprimento.",
     "The sun shadows above are computed at screen resolution. Softness controls the penumbra width; Range, the length."},
    {"GPU/driver sem INTZ ou RT float", "GPU/driver lacks INTZ or float RT"},
    {"falha ao criar recursos do post-fx", "failed to create post-fx resources"},
    {"ativo (%ux%u)", "active (%ux%u)"},

    // ---- chat ---------------------------------------------------------------------------------
    {"Convidado", "Guest"},
    {"Apelido definido: %s", "Nickname set: %s"},
    {"Comandos: /nick NOME   /clear", "Commands: /nick NAME   /clear"},
    {"Sem parceiro conectado: mensagem nao enviada.", "No partner connected: message not sent."},
    {"Abrir chat (F8)", "Open chat (F8)"},
    {"Chat: F8 abre a caixa de texto, Enter envia, Esc cancela.", "Chat: F8 opens the text box, Enter sends, Esc cancels."},
    {"Mensagem (Enter envia, Esc cancela)", "Message (Enter sends, Esc cancels)"},

    // ---- co-op connection messages (shown in the panel) --------------------------------------
    {"Conectado%s.", "Connected%s."},
    {" (mesmo PC)", " (same PC)"},
    {"Conectando a %s:%u...", "Connecting to %s:%u..."},
    {"Falha (WSA=%d). Tentando novamente...", "Failed (WSA=%d). Retrying..."},
    {"Recusado pelo Host. Tentando novamente...", "Refused by the Host. Retrying..."},
    {"Tempo esgotado. Tentando novamente...", "Timed out. Retrying..."},
    {"Erro ao abrir porta %u (WSA=%d)", "Error opening port %u (WSA=%d)"},
    {"Aguardando convidado na porta TCP %u...", "Waiting for guest on TCP port %u..."},
    {"IP invalido: %s", "Invalid IP: %s"},
    {"Desconectado.", "Disconnected."},

    // ---- log lines (the LOG box) ---------------------------------------------------------------
    {"[Sim] Controle alternado para o Sim #%u (0x%08X)!", "[Sim] Control switched to Sim #%u (0x%08X)!"},
    {"[TS2SE] Inicializado: ExecutePrimitive=%d ExecuteStep=%d Motives=%d (8 opcodes 0x0080..0x0087)",
     "[TS2SE] Initialized: ExecutePrimitive=%d ExecuteStep=%d Motives=%d (8 opcodes 0x0080..0x0087)"},
    {"[Motives] Motivos maximizados (+100.0) em %u Sims do lote (MotivesPtr=0x%08X)!",
     "[Motives] Motives maxed (+100.0) on %u lot Sims (MotivesPtr=0x%08X)!"},
    {"[Time] Relogio do lote ajustado para %02u:00!", "[Time] Lot clock set to %02u:00!"},
    {"[Funds] Fundos da familia ajustados em %+d -> $%d", "[Funds] Household funds adjusted by %+d -> $%d"},
    {"[Motives] Decaimento de motivos %s em %u Sims.", "[Motives] Motive decay %s on %u Sims."},
    {"[Aspiration] Decaimento de aspiracao %s em %u Sims.", "[Aspiration] Aspiration decay %s on %u Sims."},
    {"[Aspiration] Nivel de aspiracao do Sim ativo definido para %s (%d)!",
     "[Aspiration] Active Sim's aspiration level set to %s (%d)!"},
    {"[Aspiration] +%d Pontos de Aspiracao adicionados (Total: %d pts)!",
     "[Aspiration] +%d Aspiration Points added (Total: %d pts)!"},
    {"[Rewards] Todas as Recompensas de Carreira desbloqueadas no lote (%u Sims)!",
     "[Rewards] All Career Rewards unlocked on the lot (%u Sims)!"},
    {"[Sim] Cheat nativo 'forcetwins' executado no Sim selecionado!",
     "[Sim] Native 'forcetwins' cheat run on the selected Sim!"},
    {"[Cinema] PlumbBob (diamante verde) alternado no lote!", "[Cinema] PlumbBob (green diamond) toggled on the lot!"},
    {"[Cinema] Baloes de fala/pensamento (showHeadlines): %s", "[Cinema] Speech/thought bubbles (showHeadlines): %s"},
    {"CONGELADO", "FROZEN"},
    {"ATIVADO", "ENABLED"},
    {"[QoL] Simulacao em Background ATIVADA (0x%08X=EB 22, 0x%08X=90 90).",
     "[QoL] Background simulation ENABLED (0x%08X=EB 22, 0x%08X=90 90)."},
    {"[QoL] Simulacao em Background DESATIVADA (bytes originais restaurados).",
     "[QoL] Background simulation DISABLED (original bytes restored)."},
    {"[QoL] SimAntics MaxTreeIterations (0x%08X) ajustado para %u.", "[QoL] SimAntics MaxTreeIterations (0x%08X) set to %u."},
    {"[QoL] Janela Sem Bordas (Borderless Fullscreen %dx%d) ATIVADA!", "[QoL] Borderless Fullscreen (%dx%d) ENABLED!"},
    {"[QoL] Modo Janela com Bordas restaurado (%dx%d).", "[QoL] Bordered window mode restored (%dx%d)."},
    {"[Autosave] Aviso: Nenhum lote carregado no momento. Save adiado.", "[Autosave] Warning: no lot loaded right now. Save postponed."},
    {"[Autosave] Checkpoint de lote disparado (PersistSys=0x%08X, Tick=%u)!", "[Autosave] Lot checkpoint triggered (PersistSys=0x%08X, Tick=%u)!"},

    {"[Co-op] Transferencia abortada: %s", "[Co-op] Transfer aborted: %s"},
    {"[Co-op] Enviando vizinhanca %s (%u arquivos, %u KB)", "[Co-op] Sending neighborhood %s (%u files, %u KB)"},
    {"[Co-op] Vizinhanca enviada.", "[Co-op] Neighborhood sent."},
    {"[Co-op] Recebimento falhou: %s", "[Co-op] Receive failed: %s"},
    {"[Co-op] Backup do seu %s -> %s", "[Co-op] Backup of your %s -> %s"},
    {"[Co-op] Recebendo vizinhanca %s do Host...", "[Co-op] Receiving neighborhood %s from the Host..."},
    {"[Co-op] Vizinhanca %s instalada. Carregue-a pelo menu principal.", "[Co-op] Neighborhood %s installed. Load it from the main menu."},
    {"[Co-op] AVISO: tabela de objetos inesperada (vtable=%08X); sincronia de moveis desativada.",
     "[Co-op] WARNING: unexpected object table (vtable=%08X); furniture sync disabled."},
    {"[Co-op] Parceiro esta sincronizando o Sim dele: sincronia de Sims ligada neste lado.",
     "[Co-op] Partner is syncing their Sim: Sim sync turned on on this side."},
    {"[Co-op] FALHA ao criar objeto do parceiro (slot #%u, GUID=%08X, pos %d,%d).",
     "[Co-op] FAILED to create partner's object (slot #%u, GUID=%08X, pos %d,%d)."},
    {"[Co-op] Objeto comprado/criado sincronizado (slot do parceiro #%u -> local #%u, GUID=%08X).",
     "[Co-op] Bought/created object synced (partner slot #%u -> local #%u, GUID=%08X)."},
    {"[Co-op] Objeto vendido/removido sincronizado (slot #%u, GUID=%08X).", "[Co-op] Sold/removed object synced (slot #%u, GUID=%08X)."},
    {"[Co-op] Sincronizando o lote com o Host: %u moveis no snapshot, %u iguais, %u em outro slot, %u reposicionados, %u a criar, %u a remover.",
     "[Co-op] Syncing lot with the Host: %u objects in snapshot, %u identical, %u in another slot, %u repositioned, %u to create, %u to remove."},
    {"[Co-op] Lote sincronizado com o Host: %u criados, %u removidos, %u Sims posicionados.",
     "[Co-op] Lot synced with the Host: %u created, %u removed, %u Sims placed."},
    {"[Co-op] Nao consegui criar o objeto #%u (GUID=%08X) do Host.", "[Co-op] Could not create the Host's object #%u (GUID=%08X)."},
    {"[Co-op] Acao sincronizada no Sim #%u (NID %d) -> Alvo #%u guid=%08X (Menu=%d, Tree=%d, prio=%d, flags=%X, params=%d,%d,%d,%d)",
     "[Co-op] Action synced on Sim #%u (NID %d) -> Target #%u guid=%08X (Menu=%d, Tree=%d, prio=%d, flags=%X, params=%d,%d,%d,%d)"},
    {"[Co-op] AVISO: Sim do parceiro (NeighborID %d) nao existe aqui.", "[Co-op] WARNING: partner's Sim (NeighborID %d) does not exist here."},
    {"[Co-op] Sim #%d do parceiro reposicionado de (%d,%d) para (%d,%d)", "[Co-op] Partner's Sim #%d repositioned from (%d,%d) to (%d,%d)"},
    {"[Co-op] Movel #%u ignorado: posicao local (%d,%d) difere da esperada (%d,%d)",
     "[Co-op] Object #%u ignored: local position (%d,%d) differs from expected (%d,%d)"},
    {"[Co-op] Snapshot do lote enviado: %u moveis, %u Sims.", "[Co-op] Lot snapshot sent: %u objects, %u Sims."},
    {"[Co-op] Sincronia de moveis: %u objetos monitorados.", "[Co-op] Furniture sync: %u objects tracked."},
    {"[Co-op] Pedindo o lote completo ao Host.", "[Co-op] Requesting the full lot from the Host."},
    {"[Co-op] AVISO: versao do protocolo diferente (peer=%u, local=%u)", "[Co-op] WARNING: protocol version mismatch (peer=%u, local=%u)"},
    {"[Co-op] Parceiro desconectado (%s).", "[Co-op] Partner disconnected (%s)."},
    {"[Co-op] Parceiro conectado%s!", "[Co-op] Partner connected%s!"},
    {"[Co-op] IA local do convidado silenciada (NID %d): quem decide as acoes autonomas e o Host.",
     "[Co-op] Guest's local AI muted (NID %d): the Host decides autonomous actions."},
    {"[Co-op] Sincronia de moveis %s.", "[Co-op] Furniture sync %s."},
    {"[Co-op] IA do convidado: %s.", "[Co-op] Guest AI: %s."},
    {"[Co-op] Sincronia de Sims: %s.", "[Co-op] Sim sync: %s."},
    {"ligada", "on"},
    {"desligada", "off"},
    {"silenciada (segue o Host)", "muted (follows the Host)"},
    {"livre", "free"},
    {"[Co-op] Pedindo snapshot de moveis ao Host.", "[Co-op] Requesting furniture snapshot from the Host."},
    {"[Co-op v2] Pronto: TCP host-autoritativo (SimulateTick=%d, OnIdle=%d, EnqueueInter=%d)",
     "[Co-op v2] Ready: host-authoritative TCP (SimulateTick=%d, OnIdle=%d, EnqueueInter=%d)"},
    {"[Co-op] ERRO: socket() falhou (WSA=%d)", "[Co-op] ERROR: socket() failed (WSA=%d)"},
    {"[Co-op] ERRO: nao consegui escutar na porta TCP %u (WSA=%d)", "[Co-op] ERROR: could not listen on TCP port %u (WSA=%d)"},
    {"[Co-op] Host escutando na porta TCP %u", "[Co-op] Host listening on TCP port %u"},
    {"[Co-op] Convidado conectando a %s:%u", "[Co-op] Guest connecting to %s:%u"},
    {"[Co-op] Sessao encerrada.", "[Co-op] Session ended."},
    {"[Co-op] Estado enviado ao convidado.", "[Co-op] State sent to the guest."},
    {"[Co-op] Resync solicitado ao Host.", "[Co-op] Resync requested from the Host."},
};

constexpr size_t kCount = sizeof(kTable) / sizeof(kTable[0]);
constexpr uint32_t kSlots = 1024;     // power of two, > 2 * kCount

int16_t g_slot[kSlots];
bool    g_built = false;
Lang    g_lang  = Lang::PT;
bool    g_loaded = false;

uint32_t Hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= static_cast<unsigned char>(*s); h *= 16777619u; }
    return h;
}

void Build() {
    for (uint32_t i = 0; i < kSlots; ++i) g_slot[i] = -1;
    for (size_t i = 0; i < kCount; ++i) {
        uint32_t k = Hash(kTable[i].pt) & (kSlots - 1);
        while (g_slot[k] >= 0) k = (k + 1) & (kSlots - 1);
        g_slot[k] = static_cast<int16_t>(i);
    }
    g_built = true;
}

const char* kLangFile = "ts2lang.cfg";

void LoadChoice() {
    g_loaded = true;
    char buf[16] = {};
    HANDLE h = CreateFileA(kLangFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(h);
        buf[got] = 0;
        if (buf[0] == 'e' || buf[0] == 'E') { g_lang = Lang::EN; return; }
        if (buf[0] == 'p' || buf[0] == 'P') { g_lang = Lang::PT; return; }
    }
    // no saved choice: follow the system UI language (Portuguese -> PT, everything else -> EN)
    const unsigned primary = static_cast<unsigned>(GetUserDefaultUILanguage()) & 0x3FFu;
    g_lang = (primary == 0x16u /* LANG_PORTUGUESE */) ? Lang::PT : Lang::EN;
}

} // namespace

Lang Get() {
    if (!g_loaded) LoadChoice();
    return g_lang;
}

void Set(Lang l) {
    g_loaded = true;
    g_lang = l;
    HANDLE h = CreateFileA(kLangFile, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const char* v = (l == Lang::EN) ? "en\r\n" : "pt\r\n";
    DWORD wr = 0;
    WriteFile(h, v, 4, &wr, nullptr);
    CloseHandle(h);
}

const char* Tr(const char* pt) {
    if (!pt || !*pt) return pt;
    if (!g_loaded) LoadChoice();
    if (g_lang == Lang::PT) return pt;
    if (!g_built) Build();
    uint32_t k = Hash(pt) & (kSlots - 1);
    while (g_slot[k] >= 0) {
        const Entry& e = kTable[g_slot[k]];
        if (strcmp(e.pt, pt) == 0) return e.en;
        k = (k + 1) & (kSlots - 1);
    }
    return pt;
}

} // namespace ts2::i18n
