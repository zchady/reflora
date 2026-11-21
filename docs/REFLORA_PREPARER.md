# Preparador do REFLORA — referência

O preparador transforma o NUP do usuário num jogo pronto para rodar neste PC: `bin/wonder2.exe` + `data/`. Não usa nada
instalado no Windows além do próprio Windows e do driver da GPU. O launcher REFLORA chama o preparador e mostra o que
ele informa; nenhuma parte do jogo é distribuída.


## Como chamar

```
components\python\python.exe -X utf8 tools\prepare_switch2.py --nup <Wonder.nup> --workspace <pasta> [opções]
```

| Opção | Efeito |
|---|---|
| `--steps a,b,...` | Só estas etapas (padrão: todas, na ordem abaixo) |
| `--jobs N` | Processos paralelos da etapa `shaders` (padrão: núcleos − 2) |
| `--build-jobs N` | Compilações paralelas (padrão: o que cabe na RAM livre, até 8) |
| `--toolchain-dir <pasta>` | Instalação offline: usa os 5 arquivos fixados dali em vez de baixar |
| `--components <pasta>` | Componentes portáteis (padrão: `<reflora>\components` quando tem `components.json`) |
| `--force` | Refaz as etapas mesmo que estejam prontas |
| `--graphics-jobs N` | Threads da etapa `graphics` (padrão: núcleos lógicos − 2, até 8) |
| `--graphics-timeout S` | Tempo limite da etapa `graphics` (padrão: 600 s) |
| `--harness`, `--ryubing-archive`, `--spirv-val`, `--shader-sample`, `--graphics-salt` | Só desenvolvimento e testes controlados |

O workspace recebe a marca `.nx-workspace`. Só esse workspace e `private/` aceitam dados extraídos do jogo. O NUP é
apenas lido, nunca copiado: o jogo lê o RomFS direto dele, pelo caminho gravado em `data/nup.txt`.

## Etapas

| # | Etapa | O que faz | Saída |
|---|---|---|---|
| 1 | `verify` | Checa o processador e o disco (abaixo). Confere que o pacote contém o programa `040015100B514001` e se o build é o testado (1.2.1); com outro build, emite `WARN`. Roda sempre. | `work/package.txt` |
| 2 | `extract` | ExeFS do programa, com SHA-256 | `work/exefs` |
| 3 | `bins` | Imagens planas dos módulos | `data/analysis/*.bin` |
| 4 | `recompile` | `main`, `sdk` e `subsdk0` → C++ | `work/gen` |
| 5 | `shaders` | Varredura, extração, rebaixamento Ampere→Maxwell, tradução para SPIR-V e `spirv-val` | `data/spirv`; trabalho em `work/shaders` |
| 6 | `build` | llvm-mingw + CMake + Ninja + SDL3 + Vulkan-Headers, fixados e verificados | `bin/` (exe, `SDL3.dll`, `licenses/`) |
| 7 | `config` | Grava o caminho do NUP e cria a pasta dos saves | `data/nup.txt`, `data/user/` |
| 8 | `check` | Roda o jogo 45 s sem janela. Exige tela título, 0 shaders faltando e 0 erros | `work/check.log`, `work/check_title.ppm` |
| 9 | `graphics` | Preparação gráfica: os pipelines do catálogo do projeto compilados pelo driver deste PC (seção abaixo). Nunca falha a preparação. | `data/pipeline_catalog.bin`, `data/pipelines_<vendor>_<device>.bin`, `work/graphics.json`, `work/graphics.log` |
| — | `textures` (opcional) | Só quando pedida em `--steps`: o cache ASTC → BC7 do modo `NX_ASTC_BC7` (desligado por padrão), gerado pelo próprio runtime (seção abaixo). Nunca falha a preparação. | `data/astc_bc7_cache/`, `work/textures.json`, `work/textures.log` |

## Protocolo para o launcher (stdout, uma linha por evento)

- `STEP <i>/<n> <etapa> [already done (skipped)]`
- `GROUP …` — fases da etapa `shaders`, grupo por grupo.
- `PROGRESS <0-100>` — sempre o progresso dentro da etapa atual; cada etapa termina com `PROGRESS 100`.
  O total é calculado pelo launcher.
- `COUNT <feitos>/<total> <unidade>` — o que a etapa conta. A etapa `graphics` conta pipelines: `COUNT 400/773 pipelines`.
- `LOG <texto>`
- `WARN <texto>`
- `ERROR <texto>`
- `RESULT graphics <ok|partial|failed|skipped> made=<n> failed=<n> total=<n> compute=<n> seconds=<s>` — uma vez, ao
  fim da etapa `graphics`. O mesmo resultado, com mais detalhes, fica em `work/graphics.json`.
- `DONE`

Códigos de saída:

| Código | Significado |
|---|---|
| 0 | Pronto |
| 2 | Precisa do usuário: pacote errado, CPU sem AVX2, disco insuficiente, componente faltando ou alterado |
| 1 | Falha. O `ERROR` indica o log. |

**Fim da preparação:** uma preparação inteira (todas as etapas) grava `work/prepared.json`, com o NUP e o horário,
assim que o `check` passa (o jogo já é jogável) e de novo no `DONE`. Começar uma nova preparação inteira apaga esse
arquivo. O launcher só mostra "Pronto" com ele. A etapa `graphics`, que vem depois, só otimiza: pará-la ou falhar
nela não tira o "Pronto".

## Retomada

Repetir o mesmo comando continua de onde parou. Uma fase só é pulada quando duas coisas conferem:

- a impressão digital das entradas;
- o SHA-256 de cada saída.

Toda saída é escrita em `.tmp` e depois renomeada, então uma interrupção nunca deixa um resultado parcial passar por
pronto.

Pontos de retomada:

- **Shaders:** retoma grupo por grupo (243 fases).
- **Build:** o Ninja continua do último objeto compilado.
- **Downloads:** um download interrompido recomeça do zero, porque o `.part` nunca é usado. O SHA-256 é sempre
  conferido.

Isso foi provado ao derrubar a árvore inteira de processos no meio do build.

## Etapa `graphics` (preparação gráfica)

Compila no driver do PC, antes de jogar, os pipelines que o catálogo do projeto conhece: os gráficos e os de compute.
Assim a primeira execução não engasga no conteúdo já gravado.
**Quem compila é o próprio runtime**, no modo de preparação; o
preparador não tem lógica de pipelines.

**Posição:** etapa 9, depois de `check`. Ela precisa do exe que passou no `check` e de `data/spirv`.

**O que a etapa faz, em ordem:**

1. **Confere o exe.** Um exe compilado antes da preparação gráfica (sem a marca `NX_PREWARM_MODE`) não é iniciado:
   o resultado é `skipped`, e o `build` resolve.
2. **Confere o catálogo do projeto** (`games/wonder2/pipeline_catalog.bin`): formato, entradas e FNV de cada uma. Um
   catálogo ilegível ou vazio dá `skipped`.
3. **Instala o catálogo em `data/pipeline_catalog.bin`.** A cópia é atômica (`.tmp` + rename) e só acontece quando o
   SHA-256 mudou. **O jogo instalado lê esse arquivo a cada abertura** e prepara em segundo plano o que ele lista.
   Com o cache pronto, isso custa cerca de 150 ms numa thread de fundo.
4. **Apaga `data/pipelines_*.stamp`.** A etapa só inicia o runtime quando algo mudou (veja "Reconhecer o que já está
   pronto"), então o runtime prepara de novo sem consultar o próprio carimbo. Com o cache válido, refazer custa pouco.
5. **Inicia `bin/wonder2.exe`** sem janela e mudo, com prioridade abaixo do normal, e com:

| Variável | Valor |
|---|---|
| `NX_DATA_ROOT` | `data`, a mesma pasta do jogo instalado. O cache gerado é o que o jogo carrega. |
| `NX_LOG` | `work/graphics.log` |
| `NX_PIPELINE_PREWARM` | `data/pipeline_catalog.bin`, mais `data/pipeline_catalog_local.bin` se existir |
| `NX_PREWARM_MODE` | `exit` |
| `NX_PREWARM_THREADS` | `--graphics-jobs` (padrão: núcleos lógicos − 2, até 8) |

Todas as outras variáveis `NX_*` do ambiente são **removidas**. Assim nenhuma opção de medição (`NX_SPIRV_SALT`,
`NX_PIPELINE_RECORD`, `NX_PIPELINE_KEY_RAW`, `NX_PIPELINE_CACHE`, `NX_INPUT_SCRIPT`…) entra numa preparação.
`--graphics-salt` existe só para medir, e emite `WARN`.

6. **Acompanha o log e converte as linhas em eventos:**

| Linha do log | Vira |
|---|---|
| `pipeline catalog: N pipelines (C compute) from …` | `LOG` com o total |
| `pipeline catalog: progress d/N` | `PROGRESS` d·100/N, a cada 5 % (até 99; o 100 vem no fim da etapa) |
| `prepared (made, failed), pipeline cache …` | `LOG` final, com os números |
| cache ou catálogo danificado ou de outra GPU | `LOG` |

7. **Mede o processo:** tempo, CPU (usuário + kernel) e pico de memória (working set). Grava tudo em
   `work/graphics.json` e emite `RESULT graphics …`.

**Resultado:**

| Situação | `status` | O preparador |
|---|---|---|
| Código 0, com o arquivo de cache presente e com cabeçalho válido | `ok` | — |
| Código 4: algum pipeline recusado pelo driver | `partial` | `WARN`; refeita na próxima vez |
| Código 5: o cache não pôde ser gravado (disco cheio, permissão) | `partial` | `WARN`; refeita na próxima vez |
| Código 0, mas sem cache legível | `partial` | `WARN`; refeita na próxima vez |
| Outro código, ou uma queda | `failed` | `WARN`; refeita na próxima vez |
| Sem resposta em 120 s, ou passou de `--graphics-timeout` | `failed` | O processo é encerrado; `WARN` |
| Exe antigo, ou catálogo ilegível | `skipped` | `WARN` |

**A preparação gráfica é uma otimização.** Nenhum desses casos falha a preparação nem impede o `DONE`. O jogo continua
jogável e compila na hora o que faltar, como sempre fez. Só um exe ausente (`bin/wonder2.exe`) pede o `build`
(código 2).

**Reconhecer o que já está pronto, sem iniciar o runtime.** A etapa é pulada (`STEP 9/9 graphics already done
(skipped)`) quando o último `work/graphics.json` diz `ok` e quando nada abaixo mudou desde então:

- **Exe:** tamanho e data de `bin/wonder2.exe`.
- **Catálogos:** o SHA-256 do catálogo do projeto e o do local.
- **SPIR-V:** o SHA-256 de `data/spirv/results.tsv` e a contagem, o tamanho total e a data mais nova dos `.spv`. Uma
  nova tradução muda isso.
- **GPU e driver** como o Windows os lista (registro da classe de vídeo: id do dispositivo, nome, versão e data do
  driver). Se não der para ler, o runtime é iniciado.
- **Arquivos gerados:** o SHA-256 de `data/pipelines_*.bin` e `.stamp`. Um cache apagado, danificado ou trocado
  refaz a etapa.

A conferência custa uma fração de segundo (medida). Iniciar o runtime só para ouvir "já estava pronto"
custaria uns 4 s.

**Recuperação:**

- **Cache ausente:** a etapa refaz tudo.
- **Cache danificado:** o FNV do cabeçalho `NXVC` não confere. O runtime começa vazio, e a etapa refaz tudo.
- **Cache de outra GPU ou driver:** o cabeçalho do driver (vendor, device, `pipelineCacheUUID`) não confere. O runtime
  o descarta, e a etapa refaz tudo.
- **Interrupção** (Parar no launcher, ou o PC desligado): o cache e o carimbo são trocados por rename. Fica o arquivo
  antigo ou o novo, e o `graphics.json` não diz `ok`, então a etapa é refeita. O que já foi compilado continua no
  cache do próprio driver, e por isso a nova tentativa é mais rápida.

A etapa não toca no exe, nos saves, no `spirv` nem no catálogo do projeto. O runtime sai logo depois de criar o
dispositivo Vulkan, antes da tela título. O `check` já roda o jogo por 45 s nessa mesma pasta.

**Limites:**

- **CPU:** até 8 threads, com prioridade abaixo do normal.
- **Memória:** cada pipeline é destruído assim que é feito, porque o código compilado já está no cache. A memória
  não cresce com o catálogo.

**Catálogo parcial.** Ele cobre só o conteúdo que as rotas gravadas mostraram. O resto é compilado na hora,
como antes.

**GPUs não testadas.**

- O catálogo é o mesmo para todas. As regras canônicas são reaplicadas com as capacidades da GPU local.
- Um pipeline que o driver recusar conta como `partial`.
- Poucos hardwares foram medidos até agora.

## Etapa `textures` (opcional: cache BC7)

Para o modo `NX_ASTC_BC7=1` (desligado por padrão; `docs/nvn/SWITCHES.md`). Em GPUs sem ASTC, as texturas ASTC
são decodificadas para RGBA8 (32 bits por texel). Com o modo ligado, as que têm uma conversão aceita no cache ficam
em BC7 (8 bits por texel). A etapa gera esse cache antes de jogar, para a primeira sessão não converter durante o
jogo.

- **Quem converte é o runtime**, com `NX_ASTC_BC7_PREPARE=1`: lê todas as texturas ASTC 2D e 2D array dos BNTX do
  pacote (SARC e zstd abertos), calcula a chave como o jogo calculará, converte, julga a qualidade e grava o
  veredito em `data/astc_bc7_cache/`. Depois sai. O NUP só é lido.
- **Retomada:** os vereditos já gravados são pulados; cada arquivo é escrito com outro nome e renomeado quando
  completo. Uma interrupção nunca deixa um veredito parcial.
- **Custo:** alguns minutos, conforme o processador (todos os núcleos lógicos menos 2, prioridade abaixo do
  normal), até ~2 GB de memória; cache de cerca de 3 GB.
- **Resultado:** `RESULT textures <ok|partial|failed|skipped> converted=<n> accepted=<n> kept=<n> seconds=<s>` e
  `work/textures.json`. Uma falha só emite `WARN`: o jogo continua jogável e, com o modo ligado, converte o que
  faltar em segundo plano para a sessão seguinte.
- **Opções:** `--texture-jobs N` (padrão: núcleos lógicos − 2, até 16), `--textures-timeout S` (padrão 3600 s).
- **O cache contém dados derivados do jogo do usuário:** fica nesta máquina e nunca é publicado nem distribuído.

## Integração com o launcher

**Fluxo:** Selecionar NUP → Preparar recompilação → Preparar shaders → Preparar gráficos → Jogar.

O launcher mostra as etapas do preparador como elas são, e "Preparação gráfica" é a nona. A lista não foi
reagrupada em fases, para não redesenhar o launcher.

| Fase do fluxo | Etapas do preparador |
|---|---|
| Preparar recompilação | `verify`, `extract`, `bins`, `recompile`, `build`, `config`, `check` |
| Preparar shaders | `shaders` |
| Preparar gráficos | `graphics` |

**Eventos e arquivos que o launcher usa** (`launcher/src-tauri/src/preparer.rs`, `install.rs`):

- `STEP 9/9 graphics`, ou `… already done (skipped)`. Neste caso a tela mostra "cache reaproveitado".
- `PROGRESS` e `COUNT d/N pipelines` dentro da etapa. A tela mostra "em andamento · 400 de 773 pipelines".
- `RESULT graphics <status> made= failed= total= compute= seconds=`, exatamente uma vez quando a etapa roda. Vira o
  evento `Result` e uma linha do registro.
- `WARN` quando o status não é `ok`.
- `work/graphics.json`, lido sem iniciar nada (`install::graphics_report`), com o estado entre execuções: `none`, `ok`,
  `partial`, `failed` ou `skipped`, mais os números. Enquanto a etapa roda, o arquivo diz `failed` ("interrupted
  before it finished"). Por isso uma etapa parada no meio aparece como não concluída.

**O que a tela mostra:**

| Estado | Linha "Preparação gráfica" | Mensagem | Início |
|---|---|---|---|
| Rodando | em andamento · n de N pipelines | — | — |
| `ok` | concluída · N de N pipelines (ou "cache reaproveitado") | — | "preparado e otimizado para este PC" |
| `partial`, `failed`, `skipped` | marcador "!" (amarelo); incompleta, não concluída ou não disponível | Explica o efeito (engasgos na primeira vez) e manda usar "Verificar e atualizar" | "a preparação gráfica deste PC não foi concluída" |
| `none` numa instalação pronta (preparada antes da etapa existir) | ainda não feita | O mesmo convite | O mesmo aviso |

Em todos esses casos o botão continua sendo **Jogar**: a preparação gráfica nunca tira o "Pronto".

**Pronto continua pronto.** Uma preparação inteira grava o `work/prepared.json` assim que o `check` passa, e de novo
no `DONE`. Parar ou falhar na etapa `graphics` mantém o Jogar. Antes, o marcador só era gravado no `DONE`, e parar na
última etapa tiraria o Jogar de uma instalação jogável.

**"Verificar e atualizar"** roda a preparação inteira (o mesmo comando de Instalar). As etapas prontas são puladas ou
conferidas por SHA-256, e o `graphics` refaz a preparação só se mudaram o catálogo, o runtime (o exe), os shaders, a
GPU, o driver ou o cache. Senão, ele aparece como "cache reaproveitado".

| Etapa | Numa instalação pronta |
|---|---|
| `verify` | Roda sempre (rápida) |
| `extract`, `bins` | Puladas pelos arquivos |
| `recompile`, `shaders` | Conferem os SHA-256 e não refazem nada |
| `build` | Recompila só os arquivos do runtime que mudaram (Ninja) |
| `check` | 45 s |
| `graphics` | Pula em 0,1 s, ou prepara |

Uma instalação feita antes da etapa existir tem um exe sem a preparação gráfica. Ali o `build` recompila o runtime, e
o `graphics` prepara.

Pela linha de comando, `--steps graphics` refaz só a etapa, e `--steps graphics --force` força. O launcher não usa
essas opções.

## O que vai junto, o que é baixado

| Item | Como chega | Tamanho |
|---|---|---|
| Fontes usadas (`tools/`, `recomp/`, `runtime/`, `games/wonder2`, `CMakeLists.txt`, licenças), incluindo o catálogo de pipelines `games/wonder2/pipeline_catalog.bin` (0,57 MB, versionado no Git) | Com o launcher | 7,7 MB |
| Componentes `python` (3.12.10), `spirv_harness` (.NET 10 self-contained), `spirv-val` (v2026.4) | Com o launcher, conferidos por `components.json` | 31,6 + 81,7 + 3,8 = 117,1 MB |
| llvm-mingw 20261020, CMake 4.4.4, Ninja 1.13.2, SDL3 3.4.18, Vulkan-Headers `vulkan-sdk-1.4.363.0` | Baixados na 1ª preparação, fixados por SHA-256 | 242,4 MB |
| NUP do jogo 040015100B514001, 1.2.1, com o add-on | Fornecido pelo usuário (só lido) | 6,2 GB |

## Espaço em disco (medido)

Medição feita no workspace do teste isolado, que foi preparado com os componentes. Valores em MB alocados em NTFS,
com clusters de 4 KB:

| Pasta | Conteúdo | Alocado |
|---|---|---|
| `tools/` | Ferramentas extraídas: llvm-mingw 715, CMake 137, Vulkan-Headers 41, SDL3 19, Ninja 1 | 956 |
| `tools/downloads/` | Arquivos baixados, mantidos para reverificação. Fica vazio no modo `--toolchain-dir`. | 242 |
| `work/gen` | Código gerado (1.080 arquivos) | 1.155 |
| `work/build` | Objetos e o exe compilado | 843 |
| `work/shaders` | Blocos extraídos e intermediários (78 mil arquivos) | 733 |
| `work/exefs` | ExeFS | 39 |
| `data/` | `spirv` 495 (17.245 shaders), `analysis` 74 | 603 |
| `bin/` | `wonder2.exe` 229 + `SDL3.dll` 5 | 234 |
| **Total** | | **≈ 5.000 (4,9 GB)** |

- **Pico durante a preparação ≈ total final.** Nada é escrito em dobro: cada saída vai para `.tmp` e é renomeada, e as
  ferramentas são extraídas uma vez.
- **Uso no `%TEMP%`:** o preparador não usa; o dos compiladores e do tradutor não foi medido.
- **O que cresce depois:**
  - cache de pipelines: 0,35 MB após o `check`; 55 MB na instalação diária após muitas horas;
  - saves (menos de 30 KB) e logs.
- **Rebuild futuro:** mantém o `bin.previous` (+234 MB).
- **Mínimo para só jogar:** `bin/` + `data/` ≈ 0,84 GB. Apagar `work/` e `tools/` economiza ≈ 4,1 GB, mas perde a
  retomada e o rebuild incremental. Essa limpeza **não** está implementada.
- **Checagem do `verify`:** pede 5,5 GB para uma preparação inteira. Se houver menos livre, soma o que o workspace já
  ocupa e só recusa se faltar o restante.

## Requisitos

| | Mínimo | Recomendado |
|---|---|---|
| Sistema | Windows 10 versão 2004 ou mais novo, ou Windows 11, x64 | Windows 11 x64 |
| CPU | x86-64 com **AVX2**. O exe é compilado com `-march=haswell`: Intel Haswell (4ª geração Core) ou AMD Excavator/Zen em diante. O `verify` recusa CPU sem AVX2. | 6 núcleos/12 threads ou mais |
| GPU | Driver com **Vulkan 1.3**: o dispositivo é criado com `VkPhysicalDeviceVulkan13Features` e dynamic rendering. Sem ASTC nativo, as texturas são decodificadas na CPU. | poucas GPUs testadas |
| RAM | 16 GB (estimado, não medido num PC de 16 GB) | 32 GB |
| Disco livre no workspace | 5,5 GB | 8 GB |
| Rede | Só na 1ª preparação (242 MB), ou nenhuma com `--toolchain-dir` | — |

**Memória do jogo (medida na validação manual):**

- commit privado de 13,0 GB, pico de 14,1 GB;
- working set de pico de 4,9 GB;
- o guarda de memória encerra o jogo acima de 20 GB (`mem_limit_gb`).

Com 16 GB de RAM, o commit cabe com o arquivo de paginação do Windows gerenciado pelo sistema, porque a memória
física realmente usada fica perto de 5 GB.

**Memória da preparação:**

- O build usa jobs = mín(núcleos − 2, (RAM livre − 6 GB) / 1,2 GB, 8), sempre pelo menos 1. O maior arquivo,
  `table.cpp`, pede cerca de 1,2 GB.

**Tempo da primeira preparação** (num PC de testes; varia com o hardware):

| Fase | Tempo |
|---|---|
| `verify` + `extract` + `bins` | segundos |
| `recompile` | ~3,5 min |
| `shaders` (4 jobs) | ~7 min |
| Downloads e extração das ferramentas | ~0,6 min |
| `build` com 5 jobs | ~7 min |
| `check` | 0,75 min |
| **Total** | **≈ 19 min** |

Com 3 jobs no build, o total foi de cerca de 23 min. CPUs com menos núcleos ou menos RAM livre demoram mais; isso não
foi medido.

**Não comprovado ainda:**

- um segundo PC Windows limpo;
- GPUs NVIDIA ou Intel;
- um PC com 16 GB de RAM.
