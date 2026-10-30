# Checkpoint — 30/10/2026 tarde (Claude): partículas nativas

- `native_d3d/black_particles.c`: 0x0015B380 (simula, recorta e colore as partículas de um emissor, 1024 por vez,
  e desenha por 0x0002AC00) e o callback de quads 0x0015BF30 em C nativo, portados operação por operação do código
  traduzido (mesma ordem de operações float, minss/maxss, cvttss2si, sin/cos via double): vértices bit a bit iguais.
  Eram ~10% da thread do jogo na missão 1 (o código traduzido passa todo temporário pela pilha do guest via volatile).
- `BLACK_NATIVE_PARTICLES=0` volta ao traduzido; `BLACK_PARTICLES_CHECK=1` roda o traduzido com os draws capturados e
  depois o nativo, comparando cada lote byte a byte (435 mil atualizações de emissor, 0 diferenças);
  `RECOMP_HLE_AB=2` alterna (export novo `BlackGpuAbNewOff` da DLL).
- A/B na mesma sessão (missão 1, 1080p sem limite): 67,9 -> 72,2 fps. /O2 no código traduzido não ganhou nada
  (73,0 vs 73,2 fps): o acesso volatile à memória domina; voltou a /O1.
- Perfil depois: código do jogo plano (maior função 1,5%); a thread fica ~16% esperando no Present (GPU perto do limite).

# Checkpoint — 30/10/2026 (Claude): desempenho da missão 1

- Causa principal da lentidão local: o cache do CMake em `project/out` tinha `CMAKE_C_FLAGS_RELEASE` vazio, então o
  runtime (kernel) e a DLL do renderer eram compilados SEM otimização (o AOT tem /O1 próprio). Um configure novo já
  usa /O2; o CMakeLists agora restaura `/O2 /Ob2 /DNDEBUG` quando falta. A DLL ganhou tabelas de linha
  (`-gline-tables-only` + `/DEBUG`, sem mudar o código gerado) para o profiler.
- Renderer: o rastreador do push buffer pula blocos de dados de índice/vértice inline (0x1800/0x1808/0x1818 sem
  incremento) de uma vez — não têm estado (`BLACK_PB_NO_FASTSKIP=1` volta a ler palavra por palavra); busca das
  ligações da API por tabela hash em vez de varredura linear. `RECOMP_HLE_AB=2` alterna só essas adições.
- Medido (RTX 3050, 1080p, sem limite, missão 1, mesmo roteiro): 51,4 fps / draw 4,55 ms (build antigo) ->
  ~70 fps / draw ~2,0 ms. A/B na mesma sessão: draw 2,35 -> 2,0 ms com o pulo. Imagem conferida igual à do build antigo.
- Perfil restante: código do jogo (AOT) ~55% e espalhado; renderer ~14%.

# Checkpoint — 27/10/2026 noite (Claude): suporte a Linux, vblank nativo

- **Linux**: o mesmo código compila com clang no Linux (WSL2 Ubuntu 24.04) e roda até a missão 1 (WSLg, Vulkan em
  llvmpipe, lento mas correto). Janela/entrada/áudio por SDL2, D3D11 pela DXVK native + vkd3d-shader (como no FIFA
  Street). `build-linux.sh` chama `scripts/native_pipeline.py build` (agora portável: clang, `project/out-linux`,
  `BLACK_CMAKE_ARGS`, `BLACK_BUILD_JOBS`); `Jogar-Black.sh` abre o jogo. Shims POSIX: `hle_posix_win32.*`,
  `hle_gdi_posix.*` (do Street 1), `native_d3d/black_win32.h`; leitor de ini e mouse SDL em `black_controls.c` /
  `black_mouse.c`; no Linux o renderer é biblioteca estática com `g_esp`, `g_xbox_mem_offset` e
  `guest_cpu_suspend/resume` renomeados (eram cópias privadas da DLL no Windows).
- **Vblank nativo** (`black_sync.c`): o KEVENT de vblank do device (+0x1DBC, montado pelo próprio D3D) não era
  sinalizado por ninguém; no Windows as threads do jogo (ex.: laço 0x1FBD80) giravam com dezenas de milhões de
  `KeWaitForSingleObject` falhando, no Linux o endereço do guest chegava como handle e dava segfault. Uma thread do
  host agora o sinaliza a 59,94 Hz via `xbox_KeSetGuestEvent` (novo no kernel_bridge do xboxrecomp) com
  `RECOMP_TITLE_KEVENTS=1` (padrão em main.c). Windows: ~4 mil esperas por intervalo, missão 1 igual.
- Teste Linux: `RECOMP_PAD_SCRIPT=18000:start,45000:a,55000:a,65000:a,...` (o carregamento sob llvmpipe é mais
  lento; os tempos do roteiro do Windows caem durante loads).
- **xboxrecomp em fork**: as mudanças locais do toolkit (kernel, NV2A, APU/DSP, input, recompilador) estão em
  `batatinhafcbatatinha-pixel/xboxrecomp`, branch `ports` (padrão), sobre a v0.13.1; o `main` do fork espelha o
  upstream (já na v0.16.0). Aqui é o submódulo `xboxrecomp/`; scripts (`scripts/toolkit.py`) e CMake usam o
  submódulo quando populado, senão o `../xboxrecomp` vizinho (o workspace local continua assim, submódulo em
  deinit). **Nunca enviar nada ao upstream sp00nznet**: remote `upstream` só com fetch (push desativado).

# Checkpoint — 27/10/2026 tarde (Claude): partículas/pós do push buffer, controles modernos, Vulkan, botão do PC Settings

- `hle_draw.c`: modo imediato que o próprio jogo grava no push buffer (RenderWare Im2D/Im3D em 0x16C9B0..0x176xxx:
  partículas, passes de pós) é decodificado (SET_VERTEX_DATA* 0x1880..0x1AFC entre BEGIN_END) e desenhado pelo mesmo
  caminho do Begin/End; `hle_draw_pb_sync()` roda antes de toda chamada roteada da API (ordem preservada). O caminho
  antigo por observação de Begin/SetVertexData fica desligado com o rastreador ativo.
- Controles modernos (`native_d3d/black_controls.c`, padrão): layout CoD/Battlefield no controle + teclado/mouse
  (mouse por raw input em `native_gpu/black_mouse.c`, cursor preso/escondido só em gameplay). Gameplay =
  `MEM32(0x2F36EC)==0` (página ativa de menu/pausa; achado por diff de snapshots BLACK_MEMSNAP) e alfa de
  escurecimento `0x2E2D3C` < 0.05 (prompts "PRESS A TO CONTINUE"). Layout original do jogo: L granada, R tiro,
  R3 mira (alternar), L3 agachar, A recarregar, B corpo-a-corpo, X pegar arma, Y silenciador, D-pad modo/kit/armas.
  ini `[controles]`: esquema, mira, sensibilidade_mouse, inverter_mouse.
- Vulkan: `native_gpu/hle_gpu_api.c` portado do Street 1 (DXVK em `vulkan\`, d3d11/dxgi com /DELAYLOAD); DLLs em
  `third_party/dxvk/` (não versionadas) copiadas no build. Testado: device "(Vulkan)", missão 1 renderiza igual.
- PC Settings: botão "(BACK) PC SETTINGS" nas páginas do jogo (screen_has_button = página ativa); F1, Back+Start,
  Backspace ou clique abrem.
- Pendências: Linux (o renderer já tem SDL2/DXVK-native; falta o lado POSIX do host do Black), fundo preto atrás dos
  prompts de tutorial (deveria ser a cena escurecida), linhas largas da UI.

# Checkpoint — 27/10/2026 (Claude): missão 1 renderiza com geometria e texturas corretas

Causas encontradas e corrigidas (teste: `BLACK_SKIP_MOVIES=1 RECOMP_PAD_SCRIPT=18000:start,26000:a,34000:a,40000:a`,
missão 1 em ~50 s, 60 fps):
- `D3DDevice_DrawIndexedVertices` (0x269F10) é stdcall com 3 argumentos na pilha (prim, count, pIndexData; confirmado
  no corpo e nos chamadores). O XbSymbolDatabase dizia count em EAX: binding {8,0,9} -> {8,9,10} em
  black_gpu_bindings.inc e analysis/native-gpu-bindings.json, e `scripts/wrap_native_gpu.py` regenerado — o wrapper só
  capturava 2 palavras de pilha, então pIndexData chegava 0 e o draw virava "vértices 0..n-1" lendo além do VB (índices
  e nomes de material do Unit_01.bin viravam vértices: os "espinhos" pretos). ~80 -> ~600 draws/frame.
- `hle_draw.c`: BaseVertexIndex (device +0x1C, somado por 0x270A40) aplicado aos índices.
- `hle_draw.c` pb_track(): antes de cada draw lê os métodos NV097 escritos no push buffer desde o draw anterior (PUT em
  0x279B50, segue JUMPs) e mantém o shadow de registradores, constantes de VS (0x1EA4/0x0B80) e programa (0x1E9C/0x0B00)
  — o LTCG grava setters inline no push buffer. BLACK_PB_TRACK=0 desliga. Draws da API agora na fase 1 (após a chamada,
  com o estado preguiçoso 0x2709A0 já aplicado); BLACK_DRAW_PHASE=0 volta ao antigo.
- Cutscenes: `BLACK_SKIP_MOVIES=1` encerra cada filme no primeiro passo (black_skip.c), para testes.
- Diagnósticos novos: BLACK_VSH_DUMP=<dir> (HLSL+microcódigo de cada VS), trace com verificação de vértices em CPU,
  BLACK_WATCH=<va>[:bytes] (guard page; loga escritas com RIP para black_native.map), [BLACK-VSH] compara o programa no
  slot com o do handle.
- Codex (antes de bater o limite de uso): menu PC Settings (`native_gpu/hle_options.c`, F1/Back+Start, black-pc.ini) com
  padrões borderless na resolução do monitor, Hz do monitor, vsync, sem limite de FPS, sem escala, aniso 16x; e
  `black_timing.c` (relógio de animação 0x82210 acumula a fração de ticks; sem perda em FPS alto).
Estado: sala inicial da missão 1 com texturas, janelas, luz e HUD corretos; arma/luvas escuras mas texturizadas.

# Checkpoint — Codex: janela visível e diagnóstico de texturas

- Corrigido `black_gpu_host.c`: `hle_options_initial_window` removia WS_VISIBLE, deixando a janela do renderer invisível. Agora preserva visibilidade e abre em posição (80,80). Build incremental passou; processo com título `Black 2` e MainWindowHandle não zero confirmado. Jogo deixado aberto a pedido do usuário.
- `BLACK_GPU_TRACE_EVERY=N` habilita diagnóstico de um frame a cada N swaps. Acrescentados estados FF, escala UV e layout do stream customizado ao trace.
- Teste anterior limitado a 40 s chegou ao timeout sem crash espontâneo; imagens e log em runs/smoke/texture-before*. Texturas estão vinculadas e possuem SRV (control 0x101); a hipótese de ausência de binding não se confirmou.
- Pista para próxima etapa: ff0=00000004/00000002 (argumentos todos zero), custom stride 24 com attrs 0=22@0, 1=22@8, 2=40@16. Conferir layout LTCG dos estados/atributos antes de alterar semântica. Imagem inspecionada ainda sem texto legível; jogabilidade não validada.
# Checkpoint — 26/10/2026 (Claude, continuação): roda sem hardware até "PRESS START"; texturas faltando

- Input nativo (`native_d3d/black_input.c`, XInput sem pilha USB; teclado via callback do renderer em
  native_hardware_boundary.c), framebuffer window = no-op, troca de modo de vídeo no Swap (0x277130) como cópia
  nativa sem PRAMDAC. Recuperação: sementes de ponteiros também em tabelas dentro de seções de código e imediatos
  dos corpos recuperados.
- `DrawVerticesUP` (0x269C90) ligado ao renderer (ID novo em hle_d3d8_table.h, binding, caso em hle_native_observe).
- Resultado: roda 90 s sem crash, ~30-45 fps, draws nativos; mostra a moldura da tela "PRESS START", mas os glifos e o
  fundo saem sem textura (blocos sólidos). Próximo: texturas/estágios (LTCG escreve D3D__TextureState inline);
  `BLACK_GPU_STATS=<swaps>` e `BLACK_PB_CENSUS=<kicks>` ajudam; dumps: RECOMP_FB_DUMP=<prefixo> RECOMP_HLE_DUMP_EVERY=<n>.
- Ainda há produtores no jogo que escrevem no pushbuffer (BeginPush: 0x2BAC0, 0x2BC45, 0x37E90, 0x3E1B0, 0x3E488;
  MakeRequestedSpace: 0x16C9B0, 0x16E280, 0x171A10, 0x171C60, 0x1728F0, 0x175F80) — trocar por draws nativos.
- `Jogar-Black.cmd` agora abre project/out/black_native.exe.

# Checkpoint — 26/10/2026 madrugada (Claude): CreateDevice e DirectSound nativos; boot parado em alvo indireto 0x27D14E

Feito e testado (`python scripts/smoke.py --seconds 30`; crash: `python scripts/crash_where.py 60`):
- Infra: `project/src/native_d3d/*.c` substitui funções da biblioteca; `scripts/apply_native_overrides.py` renomeia a
  traduzida para `*_gen` (aceita comentário entre `(void)` e `{`); `scripts/make_native_copies.py` gera cópias
  nativas com acessos de hardware removidos (hoje: CDevice::Init 0x2718F0). Ambos no `native_pipeline.py`.
- `black_device.c`: bring-up da miniport sem NV2A (0x273095/0x2730C0/0x273103/0x273184/0x2732A4/0x27336D/0x275A71,
  SetTile 0x27357F). Device 0x279B50, miniport +0x1C28. `black_sync.c`: KickOff 0x26CF10 no caminho GPU-less da
  própria biblioteca (flag 0x2000). Resultado: CreateDevice completo, renderer D3D11 nativo como primário.
- `black_dsound.c`: DirectSound nativo (buffers, streams com vtable 0x2838A0, mixer do Street 1, XAudio2);
  `xbox_ResolveHandle` exportado em xboxrecomp kernel_bridge.c. XAudio2 inicializa.
- Recuperação: inclui APIs da tabela de bindings e ponteiros de código em .rdata/.data (1267 funções, 9 aborts);
  `black_recovered_funcs_lookup` registrado em integration.c para chamadas indiretas.
- `scripts/map_street1_d3d.py` → `analysis/street1-d3d-map.json` (endereços Street1→Black por alinhamento).

Próximo: `Black: untranslated indirect target 0x0027D14E` (seção XPP/XInput 0x27CF20+) — ponteiro em código
(imediato), não em dados: estender as sementes da recuperação para constantes do código que apontem para seções
executáveis (excluindo endereços de retorno), ou adicionar em config/runtime-targets.json. Depois seguir crash a crash;
falta: vblank/flip nativo, API D3D nativa ligada ao D3D11 (porte de hle_native_api.c do Street 1), 10 funções do jogo
que escrevem no pushbuffer, input nativo (XInput/XPP), 3D/I3DL2 de áudio.

# Plano "100% nativo" do Black (decisão do usuário 26/10/2026: tudo nativo, sem NV2A/MMIO)

Base: a camada nativa do FIFA Street 1 (modo NO_HW validado, mesmo XDK 5849): `street1/project/src/hle_native_create.c`
(CreateDevice/Reset em C), `hle_native_api.c`, `hle_native_resources.c`, `hle_native_states.c`, `hle_native_misc.c`,
`hle_native_logic.c`, `hle_sync.c` (KickOff/fences/vblank sem hardware), `hle_device.c`, `hle_d3d8.c` (interceptação por
`sub_<api>` → `_gen`), ~4.100 linhas. O renderer D3D11 (`hle_d3d11.c`, `hle_draw.c`, VSH) já está copiado em
`project/src/native_gpu/`, mas hoje só observa enquanto o D3D original roda e toca a NV2A.

Diferenças do Black que mudam o porte:
- D3D8 **LTCG**: convenções internas diferentes (argumentos em registradores; ver `analysis/native-gpu-bindings.json`)
  e código de D3D embutido no jogo: **40 funções do jogo** acessam `D3D_g_pDevice` (0x27BFF8) direto, além de 65 da
  biblioteca. Esse código escreve métodos NV2A no pushbuffer → precisa de um decodificador nativo de métodos
  (estado + draws → D3D11, sem rasterizador), no KickOff nativo.
- Endereços: 120 dos 171 símbolos D3D do Street 1 foram achados no Black pelo XbSymbolDatabase
  (`analysis/xbsdb-symbols.txt`); os outros 51 (KickOff, InitHardware, CreateCtxDmaObject, LockRect...) estão
  embutidos ou precisam ser localizados à mão (vários já estão em `native-gpu-bindings.json`). Cada global/tabela
  usado pelo Street 1 (DEV, MP, 0x2639EC tamanho do pushbuffer, 0x25F4E8 tabela de modos, ...) precisa do
  equivalente no Black.

Ordem: (1) tabela de correspondência de endereços Street1→Black; (2) CreateDevice nativo (sem InitHardware) e
sync/KickOff nativo descartando o pushbuffer → boot passa sem tocar hardware; (3) API nativa (recursos/estados/
draws) ligada ao D3D11; (4) decodificador de métodos para o código embutido pelo LTCG; (5) vblank/present/input.

# Checkpoint — 26/10/2026 noite (Claude): destinos ausentes resolvidos; parada na fronteira de hardware do D3D

- `scripts/recover_missing_functions.py` (no pipeline, generate e build): recupera os destinos diretos ausentes como
  funções inteiras (com chamadas), seguindo recursivamente os callees novos. A evidência é a própria chamada emitida pelo
  gerador (a desmontagem `--text-only` não tinha xrefs para DSOUND/D3D/XPP). Resultado: **986 funções recuperadas**,
  349 → **9 aborts** (todos endereços fora de seções executáveis = lixo de desmontagem). Saída
  `project/src/recomp/gen/recomp_recovered_funcs.c`, relatório `analysis/aot/recovered-functions.json`.
  Conta como definidas as funções renomeadas para `*_gpu_original` pelo `wrap_native_gpu.py`.
- `lower_native_instructions.py` agora também baixa `wbinvd` (bytes conferidos no XBE) para no-op do host; roda depois
  da recuperação no estágio build. `int3` após checagem falha é pulado como no resto do AOT.
- `project/CMakeLists.txt`: `/MAP` no black_native → `project/out/black_native.map` para traduzir endereços de crash
  em `sub_XXXXXXXX` (cdb: `kn`, depois resolver offsets `black_native+0x...` pelo map, base 0x140000000).
- Boot agora passa do ponto anterior (partições, TDATA/UDATA, save meta) e morre com access violation lendo
  `0xFD001804` (MMIO da NV2A). Pilha: `Direct3D_CreateDevice` (wrapper observador) → corpo original → `sub_002718F0`
  (init do device, contém wbinvd) → `D3D_CMiniport_InitHardware` `0x272F35` → `sub_00272F35`/`sub_00273095`.
  **Decisão pendente:** o bridge da GPU nativa só observa e ainda roda o corpo original do D3D, que fala com o hardware.
  Precisa trocar as fronteiras de hardware do D3D (InitHardware, KickOff/PFIFO put, fences/BlockOnTime,
  MakeRequestedSpace) por implementações nativas, ou mudar para substituição completa do D3D8 (como o Street 1).

# Checkpoint e Continuação — Black (USA) Xbox Original Port

**Último checkpoint:** black_aot.lib compilada: 13.358 funções traduzidas, 13.737 símbolos sub_* conferidos contra fontes (inclui destinos ausentes). Sem bibliotecas NV2A/APU ligadas por padrão; 380 destinos e 63 ocorrências de instruções ainda falham explicitamente. Ver analysis/native-integration.json.

## Estado Atual (25/10/2026)

- [x] Extração e inventário completo da mídia XDVDFS `D:\ISO\Black (USA)\Black (USA).xiso.iso`: 574 arquivos (1.68 GB) extraídos em `game_files/`.
- [x] Extração e validação do binário principal `default.xbe`:
  - SHA-256: `df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6`
  - Title ID: `45410083` (Black)
  - Image Base: `0x00010000`, Entry Point: `0x00025A3F`, Thunks: `0x00282FE0`
  - Seções e bibliotecas mapeadas (SDK 5849, D3D8LTCG, DSOUND, XMV, XBOXKRNL).
- [x] Estrutura de pastas, scripts de build (`build-local.cmd`), testes (`run-test.ps1`), lançador (`Jogar-Black.cmd`) e configuração (`config/xbe-config.json`) criados.

---

## Próximos Passos Técnicos

1. **Desmontagem e Análise de Símbolos:**
   - Realizar mapeamento de kernel thunks (`0x00282FE0`) para resolução dos ordinais de `XBOXKRNL.EXE` utilizados pelo motor do jogo.
   - Analisar chamadas para D3D8, XMV e DirectSound.
2. **Definição de HLE / Recompilação x86:**
   - Mapear funções de inicialização da GPU NV2A e comandos de vertex/pixel shader.
   - Avaliar reaproveitamento do toolkit de renderização D3D11 nativo de `street2` para emulação/tradução das chamadas de draw e texturas.
3. **Cadeia de Áudio e Vídeo:**
   - Implementar wrappers de reprodução para os arquivos de vídeo XMV (`videos/*.xmv`) e bancos de áudio.

## Continuação — análise local e proteção de espaço (25/10/2026)

- `analyze-local.cmd` / `scripts/analyze_local.py` agora validam o SHA-256 esperado,
  comparam dois parsers de cabeçalho e verificam as cópias em `game_files/` e
  `analysis/executables/`. Resultado: seis verificações passaram em
  `analysis/consistency.json`.
- `analysis/xbe-analysis.json` resolve 115 imports de kernel. O auditor de fontes
  do XboxRecomp encontra rota de bridge/thunk para os 115 e nenhuma remapagem de
  ordinal necessária. Isso verifica disponibilidade de rotas no código local,
  não semântica correta ou execução das APIs. Detalhes em `kernel-coverage.log`.
- `analysis/disasm/` preserva desmontagem e referências. O resumo detecta
  697.830 instruções e 13.360 candidatos a funções, com 121.413 referências.
  A opção usada foi `--text-only`; o detector também registra candidatos em
  seções auxiliares. Contagens e limites são heurísticos, não progresso jogável.
- `analysis/kernel-call-sites.json` liga 248 callsites estáticos aos thunks de
  75 dos 115 imports. Nenhum callsite classificado como kernel ficou sem thunk.
  Os outros 40 imports podem ser usados por caminhos indiretos ou fora dessa
  cobertura; ausência de callsite não significa import morto.
- A entrada `0x00025A3F` foi delimitada em 114 bytes pelo detector, com chamadas
  diretas candidatas para `0x000256ED`, `0x00025987` e `0x00026053`.
- Tradução estática dessa entrada concluída com código de saída 0; prévia em
  `analysis/entry-preview.c` e diagnóstico em `analysis/entry-translation.log`.
  Esse C depende do runtime e dos destinos chamados; não é executável isolado
  e não foi compilado. A preparação do tradutor carregou 13.362 funções após
  seus ajustes internos, outra contagem diferente dos candidatos originais.
- `analysis/func-id/` registra classificação heurística. Ela acrescenta
  candidatos de vtables e não equivale ao conjunto de funções do disassembler;
  nomes/categorias não foram confirmados manualmente.
- `build-local.cmd` agora encerra com código 2 e explicação quando não há
  `project/CMakeLists.txt`, antes de tentar Visual Studio/CMake. O estado
  anterior era apenas um script de build apontando para uma pasta vazia.
- Limites do pipeline: monitorar `analysis/` em 768 MiB e interromper abaixo de
  20 GiB livres na unidade do projeto; lock impede duas instâncias desse
  pipeline. Não há extração nova, nova cópia da ISO ou build grande nesta etapa.
- Consumo total de `analysis/` após essa rodada: aproximadamente 84,3 MiB.
  Validações executadas: pipeline estático com `--disassemble` (código 0),
  revalidação rápida (código 0), sintaxe Python e precondição de build (código 2
  esperado, sem chamar compilador).

Próxima integração: revisar os três destinos da entrada e o tratamento de TLS/
SEH; criar um runtime específico do Black usando componentes comuns do toolkit,
sem copiar endereços/hooks de Street 2. GPU NV2A, áudio/vídeo, controles e saves
continuam sem validação. `project/` continua sem CMake/runtime; não há boot do
jogo demonstrado.

## Integração AOT nativa — continuação

`generate-native.cmd` traduz o conjunto de funções em `project/src/recomp/gen`; `build-local.cmd` compila **`project/out/black_aot.lib`**, não um executável jogável. Build concluída com 13.358 funções traduzidas e zero falhas do gerador; 380 destinos diretos ainda ausentes tornam-se aborts explícitos em vez de sucesso/no-op. O conjunto original tinha 69 ocorrências de instruções não suportadas; dois sites únicos STMXCSR/LDMXCSR foram reduzidos a intrínsecos SSE do host após verificar bytes/operandos do XBE (alguns aparecem em funções sobrepostas). Ver `analysis/aot/native-instructions.json`.

CMake usa `BLACK_BUILD_COMPAT_HOST=OFF` por padrão: a biblioteca CPU não liga NV2A/APU nem o umbrella de emulação. O host opcional com compatibilidade está separado e não foi compilado/executado. Ainda falta integrar a ABI de kernel e implementar GPU, áudio/vídeo e demais serviços nativos; esta biblioteca não significa port 100% funcional. Identidade, tamanho e limitações em `analysis/native-integration.json`.

Proteções: reserva de 20 GiB, limite observado de 2 GiB em `project/`, dois compiladores e nenhuma extração/cópia adicional da mídia. Nenhum jogo foi executado nesta continuação.

## Reservas de espaço removidas — 26/10/2026

Por solicitação do usuário, os pipelines não exigem mais reservas mínimas de 20 GiB nem de 1 GiB para fontes/logs. As referências anteriores a essas reservas são históricas. Permanecem os limites de tamanho de artefatos e logs, as verificações de integridade e, nas extrações, a verificação de espaço para os próprios arquivos. A estimativa de build do Ben10 é apenas informativa.


## Continuação Black — host nativo e recuperação

31 folhas de destinos diretos recuperadas do XBE; 349 destinos diretos permanecem aborts. Teste de atualização da política e duas execuções preservou fontes e funções. `black_native.exe` liga AOT, ponte Win32 de kernel e black_native_gpu.dll, sem fontes nv2a_* nem umbrella XboxRecomp. Caminhos de hardware/framebuffer/player indisponíveis abortam; OHCI não conectado não sintetiza interrupções. Input ainda não integrado.

Teste chegou à entrada AOT e mais de 200 chamadas de kernel, encerrando no alvo indireto ausente 0x0021B7D2. Este inicializador DSOUND de 11 bytes (MOV de constante para memória, RET) foi recuperado pelo tradutor e registrado via recovered_indirect.h. Evidência preservada em analysis/aot/first-indirect-target.log; configuração em config/runtime-targets.json. Teste: python scripts/smoke.py --seconds 20. Nenhuma cena jogável validada.

