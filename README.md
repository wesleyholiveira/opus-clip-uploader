# OBS Clip Cropper Plugin

Plugin para **OBS Studio 30.1+** que facilita o envio de gravações/lives para o Opus Clip, com suporte a edição/revisão de trechos, transcrição local com Whisper.cpp/GGML, geração de contexto com OpenAI/GPT e upload com progresso em background.

## Índice

- [Versão suportada do OBS](#versão-suportada-do-obs)
- [Features](#features)
  - [Upload para Opus Clip](#upload-para-opus-clip)
  - [Review e marcação de trechos](#review-e-marcação-de-trechos)
  - [Edição e revisão de vídeos](#edição-e-revisão-de-vídeos)
  - [Curadoria automática pelo Opus](#curadoria-automática-pelo-opus)
  - [Transcrição local com Whisper.cpp/GGML](#transcrição-local-com-whispercppggml)
  - [Integração com OpenAI/GPT](#integração-com-openaigpt)
  - [Settings](#settings)
  - [Internacionalização](#internacionalização)
  - [FFmpeg embutido](#ffmpeg-embutido)
- [Modelos Whisper/GGML](#modelos-whisperggml)
  - [Nomes exatos dos modelos](#nomes-exatos-dos-modelos)
  - [Onde colocar os modelos](#onde-colocar-os-modelos)
- [Estrutura de pastas do projeto](#estrutura-de-pastas-do-projeto)
- [Como rodar/buildar no Windows](#como-rodarbuildar-no-windows)
- [Como instalar manualmente no OBS no Windows](#como-instalar-manualmente-no-obs-no-windows)
- [Como rodar/buildar no Linux](#como-rodarbuildar-no-linux)
- [Como validar se CUDA está sendo usado](#como-validar-se-cuda-está-sendo-usado)
- [Fluxo principal de uso](#fluxo-principal-de-uso)
- [Troubleshooting](#troubleshooting)
- [Comandos rápidos Windows](#comandos-rápidos-windows)

## Versão suportada do OBS

Este plugin foi desenvolvido e testado para:

```txt
OBS Studio 30.1+
```

Versões anteriores do OBS podem não possuir APIs, layout de diretórios ou dependências compatíveis com o plugin.

## Features

### Upload para Opus Clip

O plugin permite enviar gravações ou trechos selecionados para o Opus Clip.

Principais recursos:

- envio de vídeos para o Opus Clip;
- upload com barra de progresso;
- upload em janela separada, não-modal e minimizável;
- cancelamento pelo botão **Cancelar**, tecla **Esc** ou botão **X** da janela;
- suporte a envio de múltiplos vídeos;
- suporte a ranges selecionados no editor/review antes do upload;
- suporte a curadoria automática ou envio de trecho fixo.

### Review e marcação de trechos

Antes do upload, o plugin permite revisar o vídeo e selecionar trechos que serão enviados ao Opus.

Recursos principais:

- dialog de revisão antes do envio;
- editor de vídeo com timeline;
- seleção de ranges com início e fim;
- clique em marcador para reposicionar o vídeo;
- timeline com seek imediato;
- suporte a fullscreen;
- validação dos ranges antes do upload;
- integração com transcrição local e geração de prompt/contexto com GPT.

Se nenhum trecho for selecionado, o plugin pode usar um range padrão inicial para evitar envio vazio.

### Edição e revisão de vídeos

O plugin possui uma tela de edição/revisão para selecionar os trechos do vídeo que serão enviados ao Opus Clip.

Essa etapa define se o Opus receberá:

```txt
um trecho fixo
```

ou:

```txt
uma janela maior de curadoria
```

Quando `Skip Curate` está desmarcado, o range selecionado funciona como uma área de busca para o Opus procurar bons cortes automaticamente.

Quando `Skip Curate` está marcado, o range selecionado é tratado como um clipe fixo.

#### Features do editor de vídeo

A tela de edição/revisão possui:

- player de vídeo integrado;
- timeline com seek imediato;
- controle de play/pause;
- seleção de início e fim do range;
- visualização dos ranges marcados;
- clique em marcador para reposicionar o vídeo;
- suporte a ranges longos para curadoria automática;
- suporte a fullscreen;
- loop/reprodução controlada durante a revisão;
- integração com transcrição e prompt gerado por GPT antes do envio;
- validação dos ranges antes do upload;
- continuação do fluxo de upload após aceitar o review.

#### Como selecionar um trecho

Fluxo recomendado:

```txt
1. Abra o vídeo no editor/review.
2. Use a timeline para navegar até o ponto inicial.
3. Marque o início do range.
4. Navegue até o ponto final.
5. Marque o fim do range.
6. Revise o trecho selecionado.
7. Confirme o envio para o Opus.
```

Exemplo:

```txt
Início: 10min
Fim: 2h
```

Com `Skip Curate` desmarcado, isso significa:

```txt
“Opus, procure cortes bons dentro do intervalo de 10min até 2h.”
```

Com `Skip Curate` marcado, isso significa:

```txt
“Use exatamente o trecho de 10min até 2h como clipe fixo.”
```

#### Atalhos e ações úteis no editor

Os atalhos podem depender do foco atual dentro da janela, mas o editor foi pensado para suportar estas ações durante a revisão:

| Ação | Comportamento |
|---|---|
| Play/Pause | Inicia ou pausa a reprodução do vídeo |
| Clique na timeline | Move o vídeo para o ponto clicado |
| Arrastar timeline | Faz seek para o ponto desejado |
| Clique em marcador | Reposiciona o vídeo no marcador selecionado |
| Botão de fullscreen | Alterna o vídeo para visualização em tela cheia |
| Confirmar review | Continua para transcrição/GPT/upload |
| Cancelar review | Fecha a revisão sem enviar para o Opus |

#### Recomendações para bons cortes

Para melhorar a qualidade dos clipes gerados pelo Opus:

- selecione ranges com conteúdo suficiente para curadoria;
- evite ranges pequenos demais quando quiser múltiplos clipes;
- use um range maior quando quiser que o Opus encontre os melhores momentos;
- mantenha `Skip Curate` desmarcado para curadoria automática;
- use `Skip Curate` apenas quando quiser enviar um corte fixo;
- evite selecionar trechos com muito silêncio, tela parada ou conversa fora do assunto principal.

#### Relação com transcrição e GPT

Antes do envio ao Opus, o plugin pode executar este fluxo:

```txt
vídeo selecionado
↓
FFmpeg extrai áudio
↓
Whisper/GGML transcreve localmente
↓
transcrição é salva em cache
↓
GPT gera prompt/contexto de curadoria
↓
review/upload continua
```

O prompt gerado ajuda o Opus a entender quais cortes devem ser priorizados.

O prompt padrão orienta que cada corte tenha:

```txt
começo, meio e fim
```

e que trate de:

```txt
exatamente um assunto principal
```

Também orienta a remover partes fora do assunto, como comentários repetitivos sobre chat atrasado, PIX, estrelinha, doações ou leitura de mensagens, exceto quando esse for o assunto principal do corte.

### Curadoria automática pelo Opus

O plugin diferencia dois modos principais: curadoria ativa e `Skip Curate`.

#### Curadoria ativa

Quando `Skip Curate` está desmarcado:

```txt
range selecionado = janela de busca
```

O plugin envia o intervalo para o Opus como área de curadoria, e o Opus decide os melhores cortes dentro daquele trecho.

Nesse modo, o plugin não força:

```txt
clip_start
clip_duration
clipDurations
```

#### Skip Curate

Quando `Skip Curate` está marcado:

```txt
range selecionado = clipe fixo
```

O plugin envia início e duração exatos para o Opus.

### Transcrição local com Whisper.cpp/GGML

O plugin pode transcrever o áudio do vídeo localmente antes de enviar o conteúdo para o GPT/Opus.

Recursos:

- extração de áudio do vídeo usando FFmpeg;
- transcrição local usando Whisper.cpp;
- suporte a modelos GGML;
- suporte a backend CUDA/GPU quando habilitado no build;
- cache da transcrição por vídeo;
- logs detalhados para validar se CUDA foi solicitado e se o Whisper carregou corretamente;
- cancelamento durante transcrição;
- progresso em janela não-modal/minimizável.

O fluxo atual é:

```txt
vídeo
↓
FFmpeg extrai áudio
↓
Whisper.cpp/GGML transcreve
↓
TranscriptStore salva cache
```

### Integração com OpenAI/GPT

O plugin pode gerar automaticamente um contexto/prompt antes do review/upload.

Recursos:

- geração automática de prompt antes do review/upload;
- cache do prompt gerado por vídeo;
- modelo OpenAI configurável;
- modelo OpenAI automaticamente desativado quando o modelo Whisper/GGML não é encontrado;
- template do input enviado ao GPT configurável no menu de Settings;
- template padrão salvo em `PluginConfig`;
- template editável pelo usuário.

O prompt padrão orienta o GPT a:

- sugerir cortes com começo, meio e fim;
- manter exatamente um assunto principal por corte;
- evitar misturar assuntos diferentes no mesmo clipe;
- remover trechos fora do assunto principal;
- ignorar trechos repetitivos sobre chat atrasado, PIX, estrelinha, doações, super chat ou leitura de mensagens, exceto quando esse for o assunto principal do corte.

### Settings

O plugin possui tela de configurações com:

- API key do Opus;
- configurações de OpenAI;
- seleção de modelo OpenAI;
- seleção/configuração do modelo Whisper/GGML;
- template do input enviado ao GPT;
- preferências de curadoria;
- campos localizados em `pt-BR` e `en-US`.

Quando o modelo Whisper/GGML não é encontrado, o plugin pode gravar automaticamente:

```txt
openai_model = disabled
```

Isso evita que o usuário pense que a geração GPT/transcrição está habilitada quando o modelo local está ausente.

### Internacionalização

O plugin possui arquivos de locale para:

```txt
pt-BR
en-US
```

Arquivos:

```txt
data/locale/pt-BR.ini
data/locale/en-US.ini
```

### FFmpeg embutido

O plugin usa FFmpeg para extrair áudio dos vídeos antes da transcrição.

O build pode preparar automaticamente o runtime do FFmpeg usando:

```bat
node .github\scripts\prepare-ffmpeg-runtime.mjs
```

No pacote instalado, o FFmpeg fica em:

```txt
data/obs-plugins/clip-cropper/ffmpeg/
```

No Windows:

```txt
data/obs-plugins/clip-cropper/ffmpeg/ffmpeg.exe
```

No Linux:

```txt
share/obs/obs-plugins/clip-cropper/ffmpeg/ffmpeg
```

## Modelos Whisper/GGML

Os modelos **não são empacotados automaticamente** no build final, porque são arquivos grandes e opcionais.

A pasta `models` é criada no pacote apenas com `.gitkeep`.

### Nomes exatos dos modelos

O nome do arquivo precisa bater com o modelo selecionado no plugin. Caso contrário, o plugin vai considerar que o modelo não foi encontrado e pode desativar automaticamente o modelo OpenAI com:

```txt
openai_model = disabled
```

Use exatamente um destes nomes:

```txt
ggml-tiny.bin
ggml-base.bin
ggml-small.bin
ggml-medium.bin
ggml-large-v3.bin
```

Exemplo válido:

```txt
ggml-base.bin
```

Exemplos inválidos:

```txt
base.bin
whisper-base.bin
ggml-base.en.bin
ggml_base.bin
ggml-large.bin
large-v3.bin
```

O plugin procura o arquivo pelo nome exato.

Então, se nas configurações estiver selecionado `tiny`, o arquivo esperado será:

```txt
ggml-tiny.bin
```

Se estiver selecionado `base`, o arquivo esperado será:

```txt
ggml-base.bin
```

Se estiver selecionado `small`, o arquivo esperado será:

```txt
ggml-small.bin
```

Se estiver selecionado `medium`, o arquivo esperado será:

```txt
ggml-medium.bin
```

Se estiver selecionado `large-v3`, o arquivo esperado será:

```txt
ggml-large-v3.bin
```

### Onde colocar os modelos

#### Caminho recomendado no pacote Windows

Depois de rodar o install local:

```txt
package\data\obs-plugins\clip-cropper\models
```

Exemplo:

```txt
package\data\obs-plugins\clip-cropper\models\ggml-base.bin
```

#### Caminho recomendado em uma instalação real do OBS no Windows

Se você copiar o conteúdo do `package` para a pasta do OBS, o caminho esperado é:

```txt
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models
```

Exemplo:

```txt
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models\ggml-base.bin
```

#### Caminho Linux

Em pacotes Linux/DEB, o caminho esperado é:

```txt
/usr/share/obs/obs-plugins/clip-cropper/models
```

Exemplo:

```txt
/usr/share/obs/obs-plugins/clip-cropper/models/ggml-base.bin
```

#### Fallbacks legados

O plugin também pode procurar em caminhos legados, como:

```txt
%APPDATA%\obs-studio\plugins\clip-cropper\models
%APPDATA%\obs-studio\plugins\clip-cropper\data\models
```

Mas o caminho recomendado é o diretório de dados do plugin:

```txt
data/obs-plugins/clip-cropper/models
```

## Estrutura de pastas do projeto

Estrutura geral:

```txt
.
├── CMakeLists.txt
├── CMakePresets.json
├── data/
├── deps/
├── cmake/
├── src/
├── .github/
└── README.md
```

### `src/`

Código-fonte principal do plugin.

```txt
src/
├── auth/
├── gpt/
├── models/
├── opus/
├── transcription/
├── ui/
├── utils/
└── worker/
```

#### `src/auth/`

Código relacionado à autenticação e OAuth.

Contém, por exemplo:

```txt
google-oauth.cpp
google-oauth.hpp
oauth-callback-server.cpp
oauth-callback-server.hpp
```

Responsável por fluxos de autenticação, callback local e integração com serviços externos quando necessário.

#### `src/gpt/`

Código da integração com OpenAI/GPT.

Contém:

```txt
gpt-prompt-client.cpp
gpt-prompt-client.hpp
gpt-prompt-store.cpp
gpt-prompt-store.hpp
```

Responsabilidades:

- montar o input enviado ao GPT;
- aplicar o template configurável do prompt;
- enviar requisição para OpenAI;
- salvar prompt gerado em cache;
- recuperar prompt salvo por vídeo.

#### `src/models/`

Objetos e estruturas de domínio simples.

Contém, por exemplo:

```txt
curation-settings.hpp
transcript.hpp
```

Responsável por representar:

- configurações de curadoria;
- transcript;
- segmentos transcritos;
- ranges selecionados.

#### `src/opus/`

Cliente de integração com Opus Clip.

Contém:

```txt
opus-clip-client.cpp
opus-clip-client.hpp
```

Responsabilidades:

- criar upload/projeto no Opus;
- enviar metadados de curadoria;
- diferenciar range como janela de curadoria ou clipe fixo;
- montar payload para API do Opus.

#### `src/transcription/`

Código de transcrição local.

Contém:

```txt
realtime-transcription-service.cpp
realtime-transcription-service.hpp
transcript-store.cpp
transcript-store.hpp
```

Apesar do nome `realtime-transcription-service`, o fluxo atual não usa callback de áudio bruto do OBS.

O serviço hoje faz:

```txt
vídeo
↓
FFmpeg extrai áudio
↓
Whisper.cpp transcreve
↓
transcript cache
```

Responsabilidades:

- resolver modelo Whisper/GGML;
- carregar Whisper.cpp;
- processar áudio extraído;
- gerar segmentos de transcript;
- salvar e recuperar transcript em cache.

#### `src/ui/`

Código de interface Qt.

Contém:

```txt
ui.cpp
ui-common.cpp
ui-common.hpp
settings-dialog.cpp
upload-confirm-dialog.cpp
upload-flow.cpp
upload-review-dialog.cpp
video-editor-actions.cpp
video-marker-editor.cpp
gpt-review-prompt.cpp
advanced-settings-tree.cpp
```

Responsabilidades:

- tela de configurações;
- dialogs de confirmação;
- dialogs de progresso;
- review antes do upload;
- editor de vídeo e markers;
- integração visual com o OBS;
- janelas minimizáveis de transcrição/GPT/upload.

#### `src/utils/`

Funções utilitárias.

Contém:

```txt
config.cpp
config.hpp
file.cpp
file.hpp
request.cpp
request.hpp
```

Responsabilidades:

- leitura/escrita de configuração via `PluginConfig`;
- helpers de arquivo;
- helpers de request HTTP.

#### `src/worker/`

Workers assíncronos.

Contém:

```txt
upload-worker.cpp
upload-worker.hpp
```

Responsável por executar upload sem bloquear a UI principal do OBS.

### `data/`

Arquivos de dados empacotados com o plugin.

```txt
data/
├── locale/
└── models/
```

#### `data/locale/`

Arquivos de localização:

```txt
en-US.ini
pt-BR.ini
```

#### `data/models/`

Pasta esperada para modelos Whisper/GGML.

No repositório e no pacote, deve conter apenas:

```txt
.gitkeep
```

Os arquivos `.bin` dos modelos não devem ser versionados nem empacotados automaticamente.

### `deps/`

Dependências vendorizadas ou externas do projeto.

```txt
deps/
└── whisper.cpp/
```

A principal dependência usada aqui é o `whisper.cpp`, incluindo `ggml`.

### `cmake/`

Scripts auxiliares de build.

```txt
cmake/
├── common/
├── linux/
├── macos/
└── windows/
```

Responsável por:

- bootstrap de dependências;
- defaults por plataforma;
- helpers de instalação;
- empacotamento;
- integração com OBS Plugin Template.

### `.github/`

Configuração de CI/CD.

```txt
.github/
├── actions/
├── scripts/
└── workflows/
```

#### `.github/actions/`

Actions compostas usadas pelos workflows.

Inclui ações como:

```txt
build-plugin
run-clang-format
check-changes
```

#### `.github/scripts/`

Scripts auxiliares.

Exemplo:

```txt
prepare-ffmpeg-runtime.mjs
Build-Windows.ps1
```

Responsável por preparar runtime do FFmpeg e auxiliar builds locais/CI.

#### `.github/workflows/`

Workflows do GitHub Actions.

Responsável por:

- build Windows;
- build Linux;
- build macOS;
- clang-format;
- empacotamento;
- release artifacts.

## Como rodar/buildar no Windows

### Requisitos

- Windows 10/11
- OBS Studio 30.1+
- CMake
- Ninja
- Visual Studio Build Tools/MSVC
- Node.js
- CUDA Toolkit, se for usar Whisper com GPU
- Qt vindo dos deps do OBS/plugin

### Preparar FFmpeg runtime

```bat
node .github\scripts\prepare-ffmpeg-runtime.mjs
```

### Definir variável do FFmpeg

```bat
set "CLIP_CROPPER_FFMPEG_RUNTIME_DIR=%CD%\.ffmpeg-runtime"
```

### Configurar com preset Ninja CUDA

```bat
cmake --preset windows-ninja-cuda-x64 -DCLIP_CROPPER_FFMPEG_RUNTIME_DIR="%CLIP_CROPPER_FFMPEG_RUNTIME_DIR%"
```

### Buildar

```bat
cmake --build --preset windows-ninja-cuda-x64 --parallel
```

### Instalar no diretório local `package`

```bat
cmake --install build_cuda --prefix "%CD%\package"
```

### Validar arquivos principais

```bat
dir "%CD%\package\obs-plugins\64bit\clip-cropper.dll"
```

```bat
dir "%CD%\package\data\obs-plugins\clip-cropper\ffmpeg\ffmpeg.exe"
```

```bat
dir "%CD%\package\data\obs-plugins\clip-cropper\models"
```

A pasta `models` deve conter apenas:

```txt
.gitkeep
```

até você copiar manualmente o modelo GGML.

## Como instalar manualmente no OBS no Windows

Após gerar o pacote local:

```txt
package\
```

copie o conteúdo para a raiz da instalação do OBS.

Exemplo de destino:

```txt
C:\Program Files\obs-studio\
```

A estrutura final deve ficar parecida com:

```txt
C:\Program Files\obs-studio\obs-plugins\64bit\clip-cropper.dll
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\locale\pt-BR.ini
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\locale\en-US.ini
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\ffmpeg\ffmpeg.exe
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models\ggml-base.bin
```

## Como rodar/buildar no Linux

### Build

Fluxo geral:

```bash
cmake -S . -B build_x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_x86_64 --parallel
```

### Package

```bash
cmake --build build_x86_64 --target package
```

### Observação sobre CUDA no Linux

Quando o Whisper CUDA está habilitado, o plugin pode depender de bibliotecas do driver NVIDIA, como:

```txt
libcuda.so.1
```

Essa biblioteca vem do driver NVIDIA da máquina final, não necessariamente do CUDA Toolkit instalado no runner de CI.

Por isso, no empacotamento DEB com CUDA, o projeto desativa o `CPACK_DEBIAN_PACKAGE_SHLIBDEPS`, evitando que o `dpkg-shlibdeps` quebre tentando resolver `libcuda.so.1` no GitHub Actions.

## Como validar se CUDA está sendo usado

Durante uma transcrição, rode:

```bat
nvidia-smi -l 1
```

Procure o processo:

```txt
obs64.exe
```

E observe se o uso de VRAM/GPU sobe durante a transcrição.

O plugin também registra logs como:

```txt
[clip-cropper] Whisper CUDA compile flag CLIP_CROPPER_WHISPER_CUDA_REQUESTED=true
[clip-cropper] Whisper context params prepared. use_gpu=true
[clip-cropper] Whisper model loaded successfully. use_gpu=true cudaRequested=true
```

Se o modelo Whisper não for encontrado, o plugin desativa o modelo OpenAI com:

```txt
openai_model = disabled
```

## Fluxo principal de uso

1. Grave ou selecione um vídeo no OBS/plugin.
2. Abra o editor/review.
3. Marque o range desejado.
4. O plugin extrai o áudio via FFmpeg.
5. O Whisper transcreve localmente.
6. O transcript é salvo em cache.
7. O GPT gera um prompt/contexto de curadoria.
8. O review é aberto para validação.
9. O vídeo/range é enviado para o Opus Clip.
10. O Opus gera os cortes conforme a curadoria.

## Troubleshooting

### O plugin não encontra o modelo Whisper

Verifique se o `.bin` está usando um dos nomes exatos aceitos:

```txt
ggml-tiny.bin
ggml-base.bin
ggml-small.bin
ggml-medium.bin
ggml-large-v3.bin
```

E se está dentro da pasta:

```txt
data/obs-plugins/clip-cropper/models
```

No pacote local:

```txt
package\data\obs-plugins\clip-cropper\models
```

No OBS instalado:

```txt
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models
```

### O OpenAI aparece como disabled

Isso acontece quando o modelo Whisper/GGML selecionado não foi encontrado.

Copie o modelo para a pasta `models` e reabra as configurações.

### O FFmpeg não foi encontrado

Verifique se existe:

```txt
data/obs-plugins/clip-cropper/ffmpeg/ffmpeg.exe
```

No pacote local:

```txt
package\data\obs-plugins\clip-cropper\ffmpeg\ffmpeg.exe
```

Também é possível configurar manualmente o caminho por variável de ambiente:

```bat
set "CLIP_CROPPER_FFMPEG_PATH=C:\caminho\para\ffmpeg.exe"
```

### O build Linux falha com erro de `-fPIC`

Bibliotecas estáticas como `libwhisper.a` e `libggml.a` precisam ser compiladas com código position-independent para serem linkadas dentro do plugin `.so`.

O projeto força isso no Linux com:

```cmake
CMAKE_POSITION_INDEPENDENT_CODE ON
```

e flags como:

```txt
-fPIC
-Xcompiler=-fPIC
```

### O pacote DEB falha procurando `libcuda.so.1`

`libcuda.so.1` vem do driver NVIDIA da máquina final. Em runners de CI, ela pode não existir.

Para builds CUDA, o projeto desativa a geração automática de dependências via `dpkg-shlibdeps`.

## Comandos rápidos Windows

```bat
node .github\scripts\prepare-ffmpeg-runtime.mjs
```

```bat
set "CLIP_CROPPER_FFMPEG_RUNTIME_DIR=%CD%\.ffmpeg-runtime"
```

```bat
cmake --preset windows-ninja-cuda-x64 -DCLIP_CROPPER_FFMPEG_RUNTIME_DIR="%CLIP_CROPPER_FFMPEG_RUNTIME_DIR%"
```

```bat
cmake --build --preset windows-ninja-cuda-x64 --parallel
```

```bat
cmake --install build_cuda --prefix "%CD%\package"
```