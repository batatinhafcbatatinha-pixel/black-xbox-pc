# Black (USA) — Port Nativo PC (Xbox Original Recomp)

**Estado real:** investigação estática e preparação do port. `project/` ainda não
contém runtime nem `CMakeLists.txt`; não existe build jogável validada.

Use `analyze-local.cmd` para validar o executável e auditar imports.
Use `analyze-local.cmd --disassemble` para reutilizar/gerar o banco de desmontagem,
classificar candidatos e traduzir somente a entrada. A análise usa o checkout
vizinho `xboxrecomp`, grava apenas em `analysis/` e não extrai a ISO.
O processo verifica periodicamente reserva de 20 GiB em C: e limite de 768 MiB
em `analysis/`; são limiares de interrupção, não quota atômica de disco.

## Informações do Jogo e Mídia Original

- **Origem:** `D:\ISO\Black (USA)\Black (USA).xiso.iso`
- **Tamanho da ISO:** 1.686.306.816 bytes (1.609 MiB)
- **Plataforma:** Xbox Original (x86 little-endian, GPU NV2A / Direct3D 8)
- **Desenvolvedora:** Criterion Games
- **Publicadora:** Electronic Arts (EA Games)
- **Total de Arquivos na Mídia:** 574 arquivos (1.685.287.391 bytes) — **todos extraídos em `game_files/`**

---

## Estrutura do Executável Principal (`default.xbe`)

- **Tamanho do Arquivo:** 2.813.952 bytes
- **Tamanho da Imagem Mapeada:** 5.732.928 bytes
- **SHA-256:** `df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6`
- **Title ID:** `45410083` (Black)
- **Image Base:** `0x00010000`
- **Ponto de Entrada (Entry Point):** `0x00025A3F` (Retail encoding, decodificado)
- **Endereço TLS:** `0x002836E4`
- **Kernel Thunk Address:** `0x00282FE0`
- **Região do Jogo:** `0x00000001` (NTSC / América do Norte)
- **Build do SDK do Xbox:** 5849

### Bibliotecas Estáticas Vinculadas (SDK 5849)
1. `XAPILIB` (1.0.5849)
2. `DSOUND` (1.0.5849)
3. `XBOXKRNL` (1.0.5849)
4. `XMV` (1.0.5849) — Xbox Media Video
5. `LIBCMT` (1.0.5849) — C Runtime multithreaded
6. `LIBCPMT` (1.0.5849) — C++ Standard Library
7. `D3D8LTCG` (1.0.5849) — Direct3D 8 com Link-Time Code Generation
8. `XGRAPHCL` (1.0.5849) — Xbox Graphics Helper Library

### Tabela de Seções do XBE
| Seção | Endereço Virtual | Tamanho Virtual | Tamanho Raw | Executável |
| --- | --- | --- | --- | --- |
| `.text` | `0x00011000` | 2.125.740 bytes | 2.125.740 bytes | Sim |
| `DSOUND` | `0x00217FC0` | 52.980 bytes | 52.356 bytes | Sim |
| `WMADEC` | `0x00224EC0` | 105.580 bytes | 105.580 bytes | Sim |
| `XMV` | `0x0023EB40` | 163.124 bytes | 163.108 bytes | Sim |
| `XGRPH` | `0x00266880` | 8.188 bytes | 8.188 bytes | Sim |
| `D3D` | `0x00268880` | 83.592 bytes | 69.052 bytes | Sim |
| `XPP` | `0x0027CF20` | 24.760 bytes | 24.760 bytes | Sim |
| `.rdata` | `0x00282FE0` | 79.080 bytes | 79.064 bytes | Sim |
| `.data` | `0x002964E0` | 3.040.696 bytes | 115.280 bytes | Sim |
| `DOLBY` | `0x0057CAA0` | 29.056 bytes | 29.036 bytes | Sim |
| `$$XTIMAGE` | `0x00583C20` | 10.240 bytes | 10.240 bytes | Não |
| `$$XSIMAGE` | `0x00586420` | 4.096 bytes | 4.096 bytes | Não |
| `.XTLID` | `0x00587420` | 1.544 bytes | 1.544 bytes | Não |

---

## Estrutura do Diretório do Port

- `default.xbe` — Cópia direta do executável principal do Xbox.
- `game_files/` — Árvore completa extraída do XDVDFS (574 arquivos incluindo `GlobData.bin`, `data/`, `levels/`, `sound/`, `videos/`, `language/`).
- `analysis/` — Registros de inspeção de ISO (`iso-inventory.json`) e binários extraídos.
- `config/` — Configurações do projeto e metadados do XBE.
- `project/` — Base do projeto CMake para recompilação e adaptação nativa.
- `scripts/` — Utilitários de inspeção e re-extração do ISO (`inspect_iso.py`, `extract_iso.py`).
- `build-local.cmd` — Script de compilação com Ninja / MSVC.
- `run-test.ps1` — Script de execução e teste do port.
- `Jogar-Black.cmd` — Inicializador amigável.
- `CONTINUACAO.md` — Log de progresso e próximos passos técnicos.

## Integração AOT nativa — continuação

`generate-native.cmd` traduz o conjunto de funções em `project/src/recomp/gen`; `build-local.cmd` compila **`project/out/black_aot.lib`**, não um executável jogável. Build concluída com 13.358 funções traduzidas e zero falhas do gerador; 380 destinos diretos ainda ausentes tornam-se aborts explícitos em vez de sucesso/no-op. O conjunto original tinha 69 ocorrências de instruções não suportadas; dois sites únicos STMXCSR/LDMXCSR foram reduzidos a intrínsecos SSE do host após verificar bytes/operandos do XBE (alguns aparecem em funções sobrepostas). Ver `analysis/aot/native-instructions.json`.

CMake usa `BLACK_BUILD_COMPAT_HOST=OFF` por padrão: a biblioteca CPU não liga NV2A/APU nem o umbrella de emulação. O host opcional com compatibilidade está separado e não foi compilado/executado. Ainda falta integrar a ABI de kernel e implementar GPU, áudio/vídeo e demais serviços nativos; esta biblioteca não significa port 100% funcional. Identidade, tamanho e limitações em `analysis/native-integration.json`.

Proteções: reserva de 20 GiB, limite observado de 2 GiB em `project/`, dois compiladores e nenhuma extração/cópia adicional da mídia. Nenhum jogo foi executado nesta continuação.
