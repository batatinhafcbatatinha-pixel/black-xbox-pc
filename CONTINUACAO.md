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
