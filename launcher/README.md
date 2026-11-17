# Launcher REFLORA (Tauri 2)

O REFLORA é o launcher do Windows que leva o usuário do NUP da própria cópia até o botão **Jogar**. Ele chama o
preparador portátil (`tools/prepare_switch2.py`, referência em `docs/REFLORA_PREPARER.md`).

**Estado: marco 2, instalação real.** O launcher escolhe e verifica o NUP, escolhe a pasta da instalação, roda o
preparador de verdade (com progresso, parada e retomada) e libera o Jogar quando a preparação termina.

- **O que funciona:**
  - escolher o NUP (seletor de arquivos) e verificá-lo na hora com a etapa `verify` do preparador: pacote certo,
    processador com AVX2, espaço em disco;
  - escolher a pasta da instalação; uma pasta que já tem outros arquivos ganha uma subpasta `REFLORA`;
  - iniciar, parar e continuar a preparação; fechar o launcher também a para, e ela continua de onde parou;
  - ver etapas, grupos de shaders, progresso total, avisos, erros e o registro, que também fica salvo em
    `<instalação>\work\reflora-prepare.log`;
  - a janela, a navegação entre Início, Instalação, Configurações e Sobre;
  - os idiomas português (Brasil), inglês e espanhol, trocados na hora;
  - o nome do jogador, que o jogo recebe como nome da conta;
  - o ícone do perfil: imagens do próprio usuário, importadas e guardadas só no PC dele, que o jogo recebe como foto
    da conta;
  - a pasta dos saves, com os arquivos listados e o botão "Abrir pasta";
  - a leitura do estado da instalação no workspace;
  - o botão principal contextual;
  - o botão Jogar, quando a instalação está pronta;
  - a **Preparação gráfica**, a nona etapa do preparador: progresso em pipelines, "concluída", "cache
    reaproveitado" ou o motivo de não ter terminado, sem nunca tirar o Jogar (decisão 15).
- **Falta:** o empacotamento (launcher, fontes e componentes juntos) e um teste num segundo PC limpo.

Onde o launcher procura o preparador:

- **REFLORA:** `REFLORA_ROOT`, senão a pasta do launcher ou uma acima dela que tenha `tools\prepare_switch2.py`
  (em desenvolvimento, a raiz do repositório).
- **Componentes:** `REFLORA_COMPONENTS`, senão `<REFLORA>\components`. Em desenvolvimento, `components/` na raiz é
  ignorado pelo Git e gerado por `tools/reflora_components.py`.

## Compilar e abrir

O Rust é instalado à parte, sem alterar o PATH do Windows: `C:\toolchains\rust` (exemplo) (rustup, toolchain fixado
em `rust-toolchain.toml`). Também são necessários as Build Tools do Visual Studio (o linker MSVC e o Windows SDK) e
o WebView2, que já vem no Windows 11.

```
$env:RUSTUP_HOME="C:\toolchains\rust\rustup"; $env:CARGO_HOME="C:\toolchains\rust\cargo"
$env:PATH="$env:CARGO_HOME\bin;$env:PATH"
cd launcher\src-tauri
cargo test                 # testes do backend (protocolo, estado da instalação, configurações)
cargo build --release      # target\release\reflora.exe (a interface vai embutida)
```

Não há Node nem bundler: `src/` é servido como está. Para ver só a interface num navegador, sirva `launcher/src`
com qualquer servidor estático. Sem o backend, o estado fica em "Verificando…".

Para testar o estado **Pronto** com um workspace já preparado, use a variável de desenvolvimento:

```
$env:REFLORA_WORKSPACE = "<workspace>"
```

## Organização

| Parte | Onde | Responsabilidade |
|---|---|---|
| Interface e componentes | `src/index.html`, `src/styles/*.css`, `src/js/ui/`, `src/js/pages/` | Moldura (barra lateral, barra de título), páginas, botão principal, barras, lista de etapas, registro |
| Textos e idiomas | `src/js/strings.js`, `src/js/i18n/{pt-BR,en,es}.js` | Um dicionário por idioma, com as mesmas chaves (`node tools/check_i18n.mjs` confere chaves e `{marcadores}`). O idioma salvo vale; sem ele, vale o do sistema; o padrão é inglês. Trocar de idioma remonta as telas, sem reiniciar. |
| Jogador | `src-tauri/src/profile.rs`, `src/js/pages/settings.js` | Nome do jogador (até 10 caracteres e 31 bytes UTF-8, sem caracteres de controle) e informações da pasta de saves |
| Ícones do perfil | `src-tauri/src/avatars.rs` | Biblioteca local em `%LOCALAPPDATA%\app.reflora.launcher\avatars`: importação pelo seletor de arquivos do Windows (PNG, JPG, WebP, BMP, GIF), recorte do quadrado central e conversão para JPEG 256×256 de até 128 KiB. O ícone escolhido vai para `data\user\profile.jpg`. |
| Estado | `src/js/state/` | `store.js` (observável), `preparation.js` (redutor dos eventos do preparador, pesos medidos das etapas), `app-state.js` (`derive`: fase, botão principal, textos) |
| Ponte com o backend | `src/js/backend.js` | Único arquivo que chama o Tauri (comandos e o evento `preparer://event`) |
| Configurações do launcher | `src-tauri/src/settings.rs` | `settings.json` em `%APPDATA%\app.reflora.launcher` (NUP, workspace), gravação atômica; `REFLORA_WORKSPACE` só para desenvolvimento |
| Estado da instalação | `src-tauri/src/install.rs` | Inspeção somente leitura do workspace: `not_installed`, `nup_selected`, `interrupted` (próxima etapa), `ready`, `running`; e o estado da preparação gráfica, lido de `work\graphics.json` (`graphics_report`) |
| Comunicação com o preparador | `src-tauri/src/preparer.rs` | `locate` (preparador e componentes); `run` (Python portátil em prioridade abaixo do normal, sem janela, num Job Object do Windows, com o PC impedido de dormir e registro em arquivo); `parse_line` (STEP/GROUP/PROGRESS/COUNT/RESULT/LOG/WARN/ERROR/DONE → `PrepEvent`); `Session` (uma preparação por vez, parar = encerrar a árvore inteira) |
| Inicialização do jogo | `src-tauri/src/game.rs` | `bin\wonder2.exe` com `NX_DATA_ROOT=<workspace>\data`, sem variáveis de teste, acompanhando o processo |
| Arte | `art/` (originais aprovados), `tools/make_assets.py` | Logo transparente, cenário e ícones gerados a partir dos originais |

## Decisões

1. **Tauri 2 com front-end sem build.** HTML, CSS e módulos ES puros, com `withGlobalTauri`. Não há npm, Vite nem
   dependências JavaScript para manter e auditar. O executável fica com 3,8 MB e a interface vai embutida nele.
2. **A interface só conhece `PrepEvent`.** O preparador escreve linhas e o Rust as converte em eventos tipados. A
   interface reduz esses eventos a estado.
   - A demonstração do marco 1 saiu; o roteiro no formato real (`src-tauri/fixtures/demo_preparation.txt`, gerado
     por `tools/make_demo_script.py`) ficou como dado de teste do analisador.
   - **Parar** encerra o Job Object, isto é, a árvore inteira (Python, CMake, Ninja, clang, tradutor). Fechar o
     launcher fecha o Job e tem o mesmo efeito. A retomada é a do próprio preparador: mesmo comando, continua de
     onde parou.
3. **O estado da instalação é inferido sem verificar SHA-256.** A presença das saídas de cada etapa só decide o que
   oferecer. A verificação de verdade continua com o preparador, que confere impressões digitais e SHA-256 a cada
   execução.
   - **Pronto** exige o `work\prepared.json`, que o preparador grava só quando uma preparação inteira chega a `DONE`
     e apaga quando outra começa. Exige também `bin\wonder2.exe`, `SDL3.dll`, `data\analysis`, `data\spirv` e o
     NUP indicado em `data\nup.txt` existindo.
   - Uma pasta montada à mão, sem `.nx-workspace` (desenvolvimento), é julgada só pelo conteúdo.
4. **Progresso geral ponderado pelos tempos medidos** : recompilação 17 %, shaders 36 %, build 39 %, demais
   etapas 8 %.
5. **Janela sem moldura do sistema**, com barra de título própria sobre o cenário (arrastar, minimizar, maximizar,
   fechar). As permissões de janela estão limitadas a essas ações em `capabilities/default.json`. A CSP só permite
   arquivos do próprio aplicativo.
6. **Identidade visual a partir das imagens aprovadas:**
   - **Cenário:** é a imagem aprovada sem alterações. O logo já vem pintado nela, e ela é deslocada para que o logo
     fique sempre centrado na área das páginas.
   - **Logo da barra lateral:** é o original de fundo claro com o fundo removido.
   - **Ícone:** é o "R" do logo.
   - **Cores e botões:** as cores saem do logo (ciano, lilás, rosa, contorno azul-marinho, estrela dourada). O botão
     principal imita as letras: gradiente, contorno escuro, brilho e profundidade.
   - **Fontes:** Fredoka para títulos e botões, Nunito para o texto, ambas SIL OFL e embutidas.
   - **O que não foi usado:** nenhuma imagem, código ou recurso do Recompia (GPL-3.0), que serviu só como referência
     de organização, e nenhum recurso do jogo.
7. **Nada decorativo.** Todo botão faz o que diz. Quando uma ação é recusada, o motivo aparece traduzido: preparação
   em andamento, jogo aberto, NUP faltando, preparador ou componentes não encontrados.
8. **Nome do jogador.** O runtime (`runtime/src/svc_account.cpp`) entrega ao jogo, como `ProfileBase.nickname`, o
   primeiro nome que encontrar nesta ordem:
   - `NX_NICKNAME`, que o launcher define ao abrir o jogo;
   - `nickname=` em `data/user/profile.ini`, que o launcher grava ao salvar o nome e antes de abrir o jogo, para o
     jogo aberto fora do launcher usar o mesmo nome;
   - o padrão "Zagreus".

   O nome fica em `settings.json` do launcher, então pode ser escolhido antes de existir uma instalação. O
   `settings.ini` e o overlay do jogo não foram tocados. O jogo lê o nome ao abrir.
9. **Saves** ficam em `<instalação>\data\user\save`, a pasta de dados de save do runtime. A página Configurações
   mostra o caminho e os arquivos com tamanho e data, e abre a pasta no Explorer.
10. **Ícones do perfil.**
    - Ficam só no PC do usuário e o launcher não traz nenhum. Personagens e artes de terceiros podem ser usados pelo próprio usuário no próprio PC, mas não podem ir para o repositório nem para o
      pacote.
    - O runtime responde às funções de imagem de perfil (`GetImageSize` e `LoadImage`, usadas por
      `nn::account::LoadProfileImage`) com `data/user/profile.jpg`. O arquivo precisa ser um JPEG de até 128 KiB.
      Sem o arquivo, o jogo fica sem foto, como antes.
    - O launcher grava esse arquivo ao escolher o ícone e antes de abrir o jogo, e o apaga quando o usuário escolhe
      "Sem ícone".
12. **Ícones dos botões.** São a fonte do sistema do console, `nintendo_ext_003.bfttf`, e o REFLORA não a distribui.
    - O usuário importa a fonte do próprio console em Configurações (`src-tauri/src/button_font.rs`), validada como o
      runtime a lê.
    - Ela fica em `%LOCALAPPDATA%\app.reflora.launcher\fonts` e é copiada para `<instalação>\data\fonts` ao importar
      e antes de abrir o jogo.
13. **Atualizar uma instalação pronta:** "Verificar e atualizar" roda o preparador de novo, que reaproveita tudo e
    recompila só o que mudou.
14. **Uma instância** (`tauri-plugin-single-instance`): dois launchers abertos sobrescreviam as configurações um do
    outro.
11. **Requisitos não são mostrados.** Os valores medidos não cobrem outros PCs e GPUs, então foram tirados
    da interface. O preparador continua conferindo AVX2 e espaço em disco antes de começar.

15. **Preparação gráfica** (etapa `graphics`, `docs/REFLORA_PREPARER.md`). O launcher não tem lógica de pipelines:
    ele mostra os eventos (`COUNT`, `RESULT`) e o `work\graphics.json`.
    - **Pronto não depende dela.** O preparador grava o `prepared.json` logo depois do `check`. Com a preparação
      gráfica incompleta, o botão continua sendo Jogar, mas o Início e a Instalação **não** dizem "otimizado": eles
      explicam o efeito (engasgos na primeira vez) e indicam "Verificar e atualizar", que refaz a etapa quando o
      catálogo, o runtime, os shaders, a GPU, o driver ou o cache mudaram.
    - **O jogo aberto pelo launcher** não recebe nenhuma variável de pipelines (`NX_SPIRV_SALT`,
      `NX_PIPELINE_*`, `NX_PREWARM_*`). Ele usa o catálogo instalado em `data\` e o cache preparado.
    - `tools/check_graphics_state.mjs` confere o redutor e o que cada estado mostra.
      `src-tauri/tests/graphics_flows.rs` (manual) passa pelos fluxos reais com um pacote e uma instalação de teste.

12. **Protocolo do preparador** (corrigido no marco 2): `PROGRESS` é sempre o progresso dentro da etapa atual, e cada
    etapa termina com `PROGRESS 100`. Antes, o preparador também mandava o total ao fim de cada etapa na mesma linha.
    O total é calculado pelo launcher (decisão 4).
