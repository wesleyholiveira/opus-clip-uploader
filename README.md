# Clip Cropper / Opus Clip Uploader

Plugin OBS para revisar gravações, sugerir cortes localmente, coletar feedback estruturado e enviar ranges aprovados para o Opus Clip / Clip Anything. Este README é o manual técnico principal do projeto inteiro. A pasta `docs/` foi removida para evitar divergência entre documentação e código.

A documentação abaixo não é apenas um glossário de scoring. Ela cobre o ciclo completo do plugin: inicialização no OBS, menus, configurações, armazenamento, transcrição, WhisperX, review, timeline, geração de candidatos, scoring, reranking, feedback, upload Opus, CMake, CI, empacotamento, ferramentas offline, troubleshooting e mapa de arquivos.

---

## Índice

1. [Resumo executivo](#1-resumo-executivo)
2. [Fluxo ponta a ponta](#2-fluxo-ponta-a-ponta)
3. [Arquitetura em camadas](#3-arquitetura-em-camadas)
4. [Entrada do plugin no OBS](#4-entrada-do-plugin-no-obs)
5. [Menus, ações e estado de gravação pendente](#5-menus-ações-e-estado-de-gravação-pendente)
6. [Configurações do plugin](#6-configurações-do-plugin)
7. [Armazenamento, cache e chaves persistidas](#7-armazenamento-cache-e-chaves-persistidas)
8. [Tela de review](#8-tela-de-review)
9. [Timeline, marcadores, zoom e seeking](#9-timeline-marcadores-zoom-e-seeking)
10. [Tabelas, diagnósticos e feedback na UI](#10-tabelas-diagnósticos-e-feedback-na-ui)
11. [Modelo de dados principal](#11-modelo-de-dados-principal)
12. [Transcrição local com whisper.cpp](#12-transcrição-local-com-whispercpp)
13. [WhisperX: transcrição primária e alinhamento](#13-whisperx-transcrição-primária-e-alinhamento)
14. [Preparação de transcript e scoring no review](#14-preparação-de-transcript-e-scoring-no-review)
15. [Presets, perfis e intenção de corte](#15-presets-perfis-e-intenção-de-corte)
16. [Vocabulário técnico da curadoria](#16-vocabulário-técnico-da-curadoria)
17. [Pipeline de sugestão de cortes](#17-pipeline-de-sugestão-de-cortes)
18. [Como candidatos são criados](#18-como-candidatos-são-criados)
19. [Como scoring é calculado](#19-como-scoring-é-calculado)
20. [Embeddings, protótipos e busca coarse](#20-embeddings-protótipos-e-busca-coarse)
21. [Reranker semântico](#21-reranker-semântico)
22. [Boundary refinement](#22-boundary-refinement)
23. [Quality gate e diagnósticos](#23-quality-gate-e-diagnósticos)
24. [Feedback: gravação, memória e influência futura](#24-feedback-gravação-memória-e-influência-futura)
25. [Quando feedback vira dataset](#25-quando-feedback-vira-dataset)
26. [Upload para Opus Clip](#26-upload-para-opus-clip)
27. [Resample/corte local via FFmpeg antes do upload](#27-resamplecorte-local-via-ffmpeg-antes-do-upload)
28. [Payload de curadoria enviado ao Opus](#28-payload-de-curadoria-enviado-ao-opus)
29. [llama.cpp, llama-server e batch seguro](#29-llamacpp-llama-server-e-batch-seguro)
30. [Cancelamento, RAII e segurança de callbacks](#30-cancelamento-raii-e-segurança-de-callbacks)
31. [C++/Qt moderno usado no projeto](#31-cqt-moderno-usado-no-projeto)
32. [Build, opções CMake e empacotamento](#32-build-opções-cmake-e-empacotamento)
33. [CI, scripts auxiliares e ferramentas offline](#33-ci-scripts-auxiliares-e-ferramentas-offline)
34. [Internacionalização](#34-internacionalização)
35. [Troubleshooting por sintoma/log](#35-troubleshooting-por-sintomalog)
36. [Mapa de arquivos por responsabilidade](#36-mapa-de-arquivos-por-responsabilidade)
37. [Regras de manutenção](#37-regras-de-manutenção)

---

## 1. Resumo executivo

O plugin existe para resolver um problema prático: depois de uma gravação longa no OBS, o usuário precisa revisar, escolher trechos bons e enviá-los ao Opus Clip. Fazer isso manualmente em vídeos longos é caro. O plugin cria uma camada local de revisão e curadoria antes do upload.

Ele faz quatro coisas principais:

1. **Review local do vídeo**: abre uma tela com player, timeline, marcadores e tabela de ranges.
2. **Sugestão local de cortes**: usa transcrição, sinais textuais, pausas, embeddings, reranker e feedback para propor trechos.
3. **Coleta de feedback**: registra o que o usuário aceitou, rejeitou, ajustou ou adicionou manualmente.
4. **Upload Opus**: envia vídeo/ranges para o Opus Clip, opcionalmente gerando vídeos otimizados só com os ranges selecionados.

O plugin não tenta substituir revisão humana. Ele tenta reduzir o espaço de busca. Em vez de procurar manualmente bons trechos em horas de live, o usuário recebe candidatos prováveis e diagnósticos dos candidatos rejeitados.

---

## 2. Fluxo ponta a ponta

Fluxo normal após uma gravação:

```text
OBS recording started
  -> plugin guarda horário inicial e valida API key

OBS recording stopped
  -> plugin resolve arquivo(s) gravado(s)
  -> salva paths pendentes em memória
  -> abre fluxo de review/upload no thread de UI

Review dialog
  -> carrega vídeo no QMediaPlayer
  -> carrega marcadores salvos ou cria range default
  -> carrega opções salvas desse vídeo/range
  -> usuário pode ajustar markers manualmente
  -> usuário pode pedir "Sugerir melhores cortes"

Sugestão semântica
  -> prepara transcrição local/cache
  -> roda pipeline de scoring
  -> aplica marcadores sugeridos
  -> salva snapshot para feedback
  -> mostra candidatos aprováveis e diagnósticos

Feedback
  -> usuário aceita/rejeita/ajusta/adiciona ranges
  -> feedback JSONL é salvo
  -> memória futura usa positivos/negativos para calibrar sugestões

Upload
  -> valida API key e arquivos
  -> opcionalmente cria vídeo otimizado via FFmpeg
  -> cria upload-link no Opus
  -> faz upload resumable
  -> cria projeto(s) de clip
  -> opcionalmente aguarda project terminal em uploads independentes
```

Pontos importantes:

- A tela intermediária anterior ao review foi removida do fluxo normal. `upload-confirm-dialog.cpp` ainda existe como ponto de compatibilidade, mas direciona para review.
- O review é o centro do fluxo. Tudo que vai para upload nasce dos ranges aprovados/selecionados nele.
- A sugestão local não altera o vídeo. Ela só altera marcadores/ranges e metadados locais.
- O Opus recebe o vídeo ou um vídeo recortado/otimizado, além de preferências de curadoria.

---

## 3. Arquitetura em camadas

A arquitetura é organizada por responsabilidade, não por tipo genérico de arquivo:

```text
OBS/plugin entry
  src/plugin-main.cpp

UI / review / editor
  src/ui/*
  src/ui/review/*

Transcription
  src/transcription/*

Curation / scoring / feedback
  src/curation/*
  src/curation/scoring/*
  src/curation/feedback/*

Opus API
  src/opus/*

Upload worker
  src/worker/*

Infra leve / utilitários
  src/utils/*
  src/models/*
```

Dependências desejadas:

```text
UI -> transcription, curation, opus/upload, utils
worker -> opus, transcription store, utils
curation -> models, feedback, scoring providers
opus -> models/curation settings
transcription -> models, utils, whisper.cpp/WhisperX worker
utils -> OBS config/file helpers
```

Regras arquiteturais:

- UI orquestra, mas não deve concentrar regra de scoring.
- Scoring não deve depender de widgets Qt.
- Feedback deve ser persistência/memória, não lógica de player.
- Opus client não deve conhecer timeline ou tabela de review.
- Transcription store não deve conhecer Opus nem scoring.
- Helpers de performance devem ser header-only apenas quando forem pequenos, genéricos e sem estado global.

---

## 4. Entrada do plugin no OBS

Arquivo principal: `src/plugin-main.cpp`.

Responsabilidades:

- declarar o módulo OBS com `OBS_DECLARE_MODULE()`;
- carregar locale padrão com `OBS_MODULE_USE_DEFAULT_LOCALE("clip-cropper", "en-US")`;
- adicionar menu/ações ao OBS;
- acompanhar eventos de gravação;
- resolver arquivo gravado;
- abrir UI no thread correto usando `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`;
- evitar duplicidade com eventos de vertical recording.

### 4.1 Gravação iniciada

Quando o OBS emite `OBS_FRONTEND_EVENT_RECORDING_STARTED`:

```text
recordingStartedAt = currentDateTime - 10s
recordingDirectory = resolve_recording_directory()
ensure_opus_api_key_on_ui_thread()
```

O buffer de 10 segundos antes/depois existe para lidar com diferenças entre horário de evento e horário de modificação do arquivo no filesystem.

### 4.2 Gravação finalizada

Quando o OBS emite `OBS_FRONTEND_EVENT_RECORDING_STOPPED`:

```text
recordingStoppedAt = currentDateTime + 10s
paths = resolve_recording_files_for_upload()
set_pending_recording_paths(paths)
open_confirm_dialog_on_ui_thread()
```

A resolução tenta:

1. diretório atual de gravação do OBS;
2. arquivos `.mp4`, `.mkv`, `.mov`, `.flv` modificados entre início/fim;
3. fallback para `obs_frontend_get_last_recording()`.

---

## 5. Menus, ações e estado de gravação pendente

Arquivos:

```text
src/ui/ui.cpp
src/ui/ui.hpp
src/ui/ui-actions.hpp
src/ui/video-editor-actions.cpp
src/ui/settings-dialog.cpp
src/ui/upload-confirm-dialog.cpp
```

`ui.cpp` mantém `pendingRecordingPaths`, que é uma lista em memória dos vídeos que acabaram de ser gravados e ainda serão revisados/enviados. Ela é consumida pelo upload flow.

Ações principais:

| Ação | Função | Efeito |
|---|---|---|
| Configurações | `open_settings` | Abre dialog de API key, modelos e backends locais. |
| Review/upload | `open_confirm_dialog` | Abre review para gravações pendentes. |
| Edição de vídeo | `open_video_editor` | Permite escolher manualmente um vídeo para revisar. |
| Validar API key | `ensure_opus_api_key` | Mostra settings se a chave não existir. |

`upload-confirm-dialog.cpp` é hoje um ponto de compatibilidade. O comportamento antigo de perguntar antes do review foi removido. O fluxo correto é abrir o review diretamente.

---

## 6. Configurações do plugin

Arquivo principal: `src/ui/settings-dialog.cpp`.

Persistência: `PluginConfig`, em `src/utils/config.*`.

### 6.1 Configurações básicas

| Campo | Chave | Uso |
|---|---|---|
| Opus API key | `opus_api_key` | Autorização Bearer na API Opus. |
| Whisper model | `whisper_model_file` | Nome do `.bin` usado pelo whisper.cpp. |
| Brand template ID | `opus_brand_template_id` | Enviado para criação de projeto Opus. |

### 6.2 Upload/resample

| Campo | Chave | Uso |
|---|---|---|
| Upload resample threshold | `upload_resample_threshold_percent` | Define quando criar vídeo otimizado só com ranges selecionados. |

Cálculo:

```text
selectedDuration = soma(endSec - startSec) dos ranges válidos
differencePercent = ((originalDuration - selectedDuration) / originalDuration) * 100
resample = differencePercent >= upload_resample_threshold_percent
```

Exemplo: vídeo de 60 minutos, ranges somam 8 minutos. Diferença = 86,6%. Com threshold 60%, o plugin gera um vídeo otimizado antes do upload.

### 6.3 WhisperX

| Campo | Chave | Valores |
|---|---|---|
| Backend | `whisperx_backend` | `disabled`, `alignment`, `primary` |
| Python path | `whisperx_python_path` | Executável Python do ambiente WhisperX. |
| Worker path | `whisperx_worker_path` | Script `tools/whisperx_align_worker.py`. |
| Device | `whisperx_device` | `cuda` ou `cpu`. |
| FFmpeg path | `whisperx_ffmpeg_path` | Executável ou diretório do FFmpeg. |
| Model | `whisperx_model` | Ex.: `large-v3`. |
| Compute type | `whisperx_compute_type` | `float16`, `int8_float16`, `int8`, `float32`. |
| Batch size | `whisperx_batch_size` | 1 a 128. |

Modos:

- `disabled`: usa whisper.cpp e não tenta alinhamento por palavra.
- `alignment`: usa whisper.cpp para transcrever e WhisperX apenas para alinhar palavras.
- `primary`: usa WhisperX como transcritor principal; se falhar sem cancelamento, pode cair para whisper.cpp.

### 6.4 Embeddings e reranker locais

| Campo | Chave | Uso |
|---|---|---|
| Local embedding backend | `local_embedding_backend` | `llama_cpp` ou `llama_server`. |
| Local embedding endpoint | `local_embedding_endpoint` | Endpoint HTTP `/v1/embeddings`. |
| Local embedding model | `local_embedding_model_id` | Nome/id ou caminho GGUF. |
| Local reranker backend | `local_reranker_backend` | `llama_cpp` ou `llama_server`. |
| Local reranker endpoint | `local_reranker_endpoint` | Endpoint HTTP `/v1/rerank`. |
| Local reranker model | `local_reranker_model_id` | Nome/id ou caminho GGUF. |

O backend `llama_cpp` roda dentro do processo do OBS, por isso o wrapper é conservador. O backend `llama_server` isola o modelo fora do processo do OBS e tende a ser mais seguro para testes pesados.

### 6.5 Limpeza de caches

O botão de limpeza remove:

```text
video_markers.*
review.settings.*
video_transcript.*
video_transcript_v3.*
video_transcript_range.*
video_transcript_range_v3.*
transcription-cache/*
```

Ele mantém API key e configurações gerais.

---

## 7. Armazenamento, cache e chaves persistidas

### 7.1 PluginConfig

`PluginConfig` encapsula leitura/escrita em configuração do OBS. Use ele para valores pequenos e settings. Não use para arquivos grandes.

### 7.2 Marcadores por vídeo

Chave:

```text
video_markers.<safeFileNameBase64>
```

O valor é uma lista `;` de segundos com três casas decimais. Dois marcadores consecutivos formam um range.

### 7.3 Settings por review/range

Chave:

```text
review.settings.<safeVideoKey>.<rangeKey>
```

Guarda:

- arquivo;
- duração original;
- ranges selecionados;
- keywords;
- gênero;
- preset;
- modelo;
- clip length;
- idiomas.

### 7.4 Transcript cache

Arquivos/chaves:

```text
video_transcript_v3.<fileHash>.<language>              # segmentos sem word timings em config
video_transcript_v4.<fileHash>.<language>.jsonl        # alinhado/word timings no disco
video_transcript_range_v3.<fileHash>.<language>.<hash> # cache de ranges selecionados
```

`safeFileKey(videoPath)` usa caminho resolvido, tamanho do arquivo e lastModified para evitar reaproveitar transcript de arquivo alterado.

### 7.5 Feedback

Diretório aproximado:

```text
<AppDataLocation>/feedback/
```

Arquivos principais:

```text
curation-feedback.jsonl
boundary-calibration.json
```

O JSONL é append-only. Isso preserva histórico e permite reconstruir memória/dataset offline.

---

## 8. Tela de review

Arquivo principal: `src/ui/upload-review-dialog.*`.

O `UploadReviewDialog` é o orquestrador. Ele deve coordenar widgets e serviços, não concentrar toda regra de scoring. Responsabilidades atuais:

- montar layout de review;
- carregar/salvar opções de curadoria;
- conectar player/timeline/tabela;
- chamar preparação de transcrição;
- chamar pipeline de scoring;
- aplicar ranges sugeridos;
- coletar feedback estruturado;
- iniciar upload.

Componentes extraídos:

```text
src/ui/review/timeline-widget.*
src/ui/review/playback-icon-button.*
src/ui/review/review-clip-table-widget.*
src/ui/review/diagnostics-dialogs.*
```

Estados importantes:

| Estado | Significado |
|---|---|
| `semanticSuggestionInProgress` | Evita disparar sugestão concorrente. |
| `diagnosticReviewMode` | Tabela está mostrando candidatos rejeitados/diagnósticos. |
| `boundaryFeedbackSaved` | Evita salvar feedback duplicado no mesmo ciclo. |
| `feedbackTranscriptLoaded` | Indica que transcript necessário para feedback foi carregado. |
| `restoredReviewedPositiveMarkersFromMemory` | Indica recuperação de ranges aprovados previamente. |

---

## 9. Timeline, marcadores, zoom e seeking

Arquivos:

```text
src/ui/video-marker-editor.*
src/ui/review/timeline-widget.*
src/ui/review/playback-icon-button.*
```

### 9.1 Modelo de marcadores

Internamente o editor armazena pontos em segundos:

```text
clipMarkersSec = [start1, end1, start2, end2, ...]
```

A lista de ranges é derivada de pares consecutivos:

```text
range[i] = { startSec = markers[2*i], endSec = markers[2*i + 1] }
```

Se não houver marcador explícito, o editor pode criar um range default de até 90 segundos.

### 9.2 Interação

Atalhos principais:

| Ação | Comportamento |
|---|---|
| `M` | Adiciona marcador na posição atual. |
| `Delete` | Remove marcador/range selecionado. |
| `Space` | Play/pause. |
| Clique na timeline | Seek. |
| Drag marcador | Move boundary. |
| Selecionar range | Atualiza preview/tabela. |

### 9.3 Ctrl + Scroll

`CTRL + Scroll` aplica zoom na região sob o cursor.

Conceito:

```text
mouseRatio = x / timelineWidth
anchorTime = visibleStart + mouseRatio * visibleDuration
newDuration = visibleDuration * zoomFactor
newStart = anchorTime - mouseRatio * newDuration
newEnd = newStart + newDuration
```

O range visível é clampado entre `0` e `durationSec`. Marcadores e regiões são redesenhados dentro do novo range visível, então a manipulação fina fica mais precisa.

### 9.4 Seeking

O seek usa `QMediaPlayer::setPosition`. Como alguns backends demoram para decodificar frame após seek, o UI mostra indicação `Seeking...` e faz warm-up do frame. Callbacks/timers usam `QPointer` quando o widget pode ser destruído antes do retorno.

---

## 10. Tabelas, diagnósticos e feedback na UI

Arquivos:

```text
src/ui/review/review-clip-table-widget.*
src/ui/review/diagnostics-dialogs.*
src/ui/upload-review-dialog.*
```

A tabela pode operar em dois modos:

1. **Review markers**: ranges atuais que podem ser enviados ao Opus.
2. **Diagnostic candidates**: candidatos rejeitados pelo gate, exibidos para revisão/feedback.

Feedback possível:

| Decisão | Significado |
|---|---|
| Aceitar | O range é bom ou suficientemente bom. |
| Rejeitar | O range não deve voltar a ser sugerido igual. |
| Ajustar | O usuário moveu start/end; gera negativo do original e positivo do corrigido. |
| Ignorar para treino | Diagnóstico útil para UI, mas não deve contaminar dataset. |

Diagnóstico não é só mensagem visual. Ele vira sinal potencial para memória futura quando for estruturado e elegível.

---

## 11. Modelo de dados principal

### 11.1 ClipDuration

Arquivo: `src/models/curation-settings.hpp`.

```cpp
struct ClipDuration {
    double startSec;
    double endSec;
};
```

Representa um intervalo no vídeo original ou no vídeo resampled, dependendo do estágio. Invariante esperado:

```text
0 <= startSec < endSec
```

### 11.2 CurationSettings

Guarda tudo que influencia upload e sugestão:

| Campo | Uso |
|---|---|
| `rangeStartSec/rangeEndSec` | Range simples legado. |
| `clipDurations` | Lista de ranges selecionados. |
| `topicKeywords` | Keywords manuais para Opus/scoring. |
| `genre` | Gênero enviado ao Opus. |
| `curationPreset` | Perfil local de curadoria. |
| `skipCurate` | Se true, Opus recebe range fixo. |
| `model` | Modelo Opus, normalmente `ClipAnything`. |
| `clipLengthPreset` | Short/Medium/Long/Auto. |
| `sourceLanguage` | Idioma de origem enviado ao Opus. |
| `transcriptionLanguage` | Idioma usado na transcrição local. |
| `aiPrompt` | Prompt manual enviado ao Opus. |
| `uploadClipRangesIndependently` | Se true, cada range vira upload/projeto separado. |

### 11.3 TranscriptSegment e WordTiming

Arquivo: `src/models/transcript.hpp`.

`TranscriptSegment` representa um trecho textual com start/end. `WordTiming` representa palavra com start/end/score. Word timings são opcionais; quando existem, `WordBoundarySnapper` consegue ajustar boundaries com mais precisão.

### 11.4 ClipCandidate

Arquivo: `src/curation/scoring/clip-candidate.hpp`.

Um candidato contém, conceitualmente:

- range start/end;
- texto agregado;
- score total;
- sinais/evidências;
- source/stage de origem;
- diagnósticos;
- flags de feedback/gate/reranker.

Ele é o objeto que atravessa a pipeline de curadoria.

---

## 12. Transcrição local com whisper.cpp

Arquivos:

```text
src/transcription/realtime-transcription-service.*
src/transcription/transcript-store.*
```

Fluxo:

```text
1. Normaliza idioma: auto, pt-br -> pt, en-us -> en.
2. Consulta cache v4 alinhado.
3. Consulta cache v3 sem word timings.
4. Se cache existe, retorna ou tenta alinhar com WhisperX.
5. Se cache não existe, resolve FFmpeg.
6. Detecta quantidade de streams de áudio.
7. Para cada stream: extrai PCM f32le mono 16k.
8. Verifica se PCM tem sinal audível.
9. Carrega whisper.cpp e transcreve em chunks.
10. Divide/ajusta segmentos usando pausas de áudio.
11. Salva cache.
12. Opcionalmente chama WhisperX para alinhamento.
```

### 12.1 Extração de áudio

FFmpeg gera PCM:

```text
-ac 1
-ar 16000
-acodec pcm_f32le
-f f32le
```

O sample rate alvo é `16000`, que é o esperado pelo Whisper.

### 12.2 Detecção de silêncio

O serviço calcula RMS em frames de 25ms:

```text
rms = sqrt(sum(sample²) / frameSamples)
```

Depois calcula percentis aproximados (`p10`, `p25`, `p50`) dos RMS e usa threshold adaptativo:

```text
adaptiveThreshold = clamp(max(0.0032, p10 * 3.25, p25 * 1.85), 0.0032, 0.034)
speechAwareThreshold = min(adaptiveThreshold, max(0.006, p50 * 0.58))
```

Um frame é silencioso quando:

```text
rms <= speechAwareThreshold
ou
rms <= max(0.0042, p10 * 2.80)
```

Silêncios internos de pelo menos ~0,70s podem ser usados para dividir segmentos longos. Isso reduz segmentos gigantes do Whisper que misturam pergunta, resposta e próximo tópico.

### 12.3 Chunks e threads

A transcrição é conservadora porque roda dentro do OBS. O número de threads é limitado:

```text
threads = min(hardware_concurrency - 1, 4)
```

Objetivo: não congelar o OBS/player durante review.

### 12.4 Cache

Cache v3 fica em `PluginConfig`. Cache v4 alinhado fica em JSONL no diretório de transcrição. O v4 tem prioridade porque contém word timings.

---

## 13. WhisperX: transcrição primária e alinhamento

Arquivos:

```text
src/transcription/whisperx-settings.*
src/transcription/whisperx-alignment-service.*
tools/whisperx_align_worker.py
```

O plugin não importa WhisperX diretamente no processo C++. Ele chama um worker Python via `QProcess`. Isso isola dependências Python e evita misturar runtime Python pesado dentro do OBS.

### 13.1 Modos

| Modo | Como funciona |
|---|---|
| `disabled` | Não chama WhisperX. |
| `alignment` | whisper.cpp gera segmentos; WhisperX alinha palavras. |
| `primary` | WhisperX transcreve e alinha; whisper.cpp vira fallback se não houver cancelamento. |

### 13.2 Comunicação com worker

O worker emite linhas JSON com tipos como:

```json
{"type":"progress","progress":42,"message":"..."}
{"type":"metric","name":"duration","value":123}
```

O C++ drena stdout/stderr, mantém tail limitado e converte progresso para a UI.

### 13.3 Saída

Quando alinhado, o transcript salvo possui:

```text
segment.startSec
segment.endSec
segment.text
segment.words[] = { word, startSec, endSec, score }
```

Isso melhora boundary snapping e reduz cortes começando ou terminando no meio de palavra.

---

## 14. Preparação de transcript e scoring no review

Arquivos:

```text
src/ui/curation-transcript-preparation.*
src/ui/review-scoring-preparation.*
src/curation/scoring/clip-scoring-pipeline.*
```

O botão "Sugerir melhores cortes" não roda tudo diretamente dentro do slot do botão. Ele passa por preparação com callbacks de progresso e cancelamento.

Etapas visíveis na UI:

```text
Verificando configurações semânticas locais...
Preparando transcrição para sugerir cortes...
Transcrição pronta. Preparando análise semântica...
Preparando pipeline de pontuação...
Conectando aos modelos locais de embedding e reranker...
Montando candidatos de corte...
Executando embedding, reranker, continuidade de assunto e análise de gancho...
Ranqueando candidatos de corte...
Aplicando marcadores sugeridos...
```

A preparação deve sempre respeitar:

- não bloquear main thread;
- permitir cancelamento;
- usar cache quando possível;
- reportar progresso em `ReviewScoringProgressUpdate`;
- retornar ranges e diagnósticos de forma que a UI consiga exibir e coletar feedback.

---

## 15. Presets, perfis e intenção de corte

Arquivos:

```text
src/curation/curation-preset.*
src/curation/curation-preset-profile.*
src/curation/curation-signals.*
```

### 15.1 Preset

Um **preset** é o perfil de intenção do corte. Ele muda limites, pesos e regras esperadas.

Exemplos de intenção:

| Preset/perfil | O que busca |
|---|---|
| `auto` | Corte bom geral, sem exigir formato específico. |
| `viewer_message_response` | Comentário/pergunta de viewer + resposta completa do streamer. |
| advice | Trecho de conselho/recomendação. |
| explanation | Explicação técnica/conceitual. |
| story | Narrativa com começo/meio/fim. |
| opinion | Opinião/tomada de posição. |
| tutorial | Passos/instrução prática. |
| emotional reaction | Reação emocional forte. |

### 15.2 CurationPresetProfile

Um perfil rico contém:

| Campo | Uso |
|---|---|
| `id` | Identificador persistido. |
| `label` | Nome exibível. |
| `archetype` | Forma dominante do conteúdo. |
| `contentKind` | Tipo semântico esperado. |
| `minDurationSec/maxDurationSec` | Bounds preferidos. |
| `allowMultipleClips` | Permite múltiplos resultados. |
| `ExchangeArcPolicy` | Regras de abertura/desenvolvimento/conclusão. |

### 15.3 Main target

`mainTarget` é o assunto principal que o corte deve cobrir. Quando `reliableMainTarget = true`, ele ganha força sobre sinais emocionais genéricos. Isso é crucial para `viewer_message_response`: se o usuário quer a resposta sobre um comentário específico, um trecho emocional bom mas fora do assunto não deve vencer.

---

## 16. Vocabulário técnico da curadoria

### 16.1 Embedding

Embedding é um vetor de floats que representa o sentido de um texto. O projeto usa `QVector<float>`. A comparação padrão é similaridade de cosseno:

```text
cosine(a, b) = dot(a, b) / (norm(a) * norm(b))
```

Uso no plugin:

- comparar janela da transcrição com protótipos;
- comparar candidato com target/preset;
- medir continuidade começo/fim;
- alimentar reranker/coarse retriever.

### 16.2 Protótipo semântico

Protótipo semântico é um texto representativo de uma categoria desejada ou indesejada. Exemplo conceitual:

```text
"a streamer clearly reads a viewer question and gives a complete answer"
```

O texto vira embedding. O candidato também vira embedding. A similaridade indica se o candidato parece pertencer àquela categoria.

### 16.3 Densidade de fala

Densidade de fala mede quanto texto existe por segundo no candidato. No código, uma forma usada é:

```text
charsPerSecond = candidate.text.trimmed().size() / durationSec
textDensityScore = clamp(charsPerSecond / 8.0, 0, 1)
```

Baixa densidade pode indicar silêncio, espera, gameplay sem fala ou trecho pouco informativo. Densidade alta demais pode indicar transcrição aglutinada ou corte sem pausas. O score não decide sozinho; ele compõe com outros sinais.

### 16.4 Pausa

Pausa pode vir de:

- gap entre segmentos de transcrição;
- silêncio detectado no PCM;
- word timings do WhisperX;
- distância temporal antes/depois do range.

Pausas ajudam a escolher começo/fim naturais.

### 16.5 Boundary

Boundary é o limite do corte: `startSec` e `endSec`. Refinar boundary significa mover início/fim para capturar o gancho e encerrar na resolução sem invadir próximo assunto.

### 16.6 Arc

Arc é a estrutura narrativa/conversacional do candidato:

```text
abertura -> desenvolvimento -> resolução
```

No preset `viewer_message_response`, o arc esperado é:

```text
viewer cue/pergunta -> resposta do streamer -> conclusão local
```

### 16.7 Beam

Beam é o conjunto dos melhores candidatos mantidos entre estágios. Em vez de expandir todos os ranges possíveis, o pipeline mantém um orçamento limitado.

### 16.8 Beam expander

`CandidateBeamExpander` cria variações ao redor de candidatos fortes:

```text
[start - lookback, end]
[start, end + lookahead]
[start - small, end + small]
variações alinhadas a segmentos/palavras
```

Ele existe porque um candidato pode estar semanticamente certo, mas começar 5 segundos tarde ou terminar antes da conclusão.

### 16.9 Gate

Gate é uma etapa que rejeita candidato mesmo que ele tenha score razoável. Exemplo: trecho com score alto mas sem conclusão, com troca de tópico ou com início ruim.

### 16.10 Diagnostic candidate

Diagnostic candidate é candidato rejeitado que ainda é útil para review. Ele aparece para explicar por que o pipeline não usou aquele range e permite o usuário corrigir/ensinar.

---

## 17. Pipeline de sugestão de cortes

Orquestrador: `src/curation/scoring/clip-scoring-pipeline.*`.

Fluxo conceitual:

```text
RecordingTranscript
  -> TranscriptIndex
  -> feedback memory
  -> preset profile/options
  -> semantic providers
  -> candidate sources
  -> candidate generation
  -> cheap scoring
  -> semantic scoring
  -> boundary refinement
  -> beam expansion
  -> quality gates
  -> reranker
  -> interval selection
  -> feedback-aware selection
  -> diagnostics
  -> ReviewScoringPreparationResult
```

A pipeline é dividida em arquivos auxiliares:

| Arquivo | Responsabilidade |
|---|---|
| `clip-scoring-pipeline.cpp` | Orquestra fluxo principal. |
| `clip-scoring-pipeline-detail.hpp` | Tipos/helpers internos compartilhados. |
| `clip-scoring-pipeline-diagnostics.cpp` | Monta rejeitados/diagnósticos. |
| `clip-scoring-pipeline-feedback-replay.cpp` | Evita reapresentar ranges já avaliados. |
| `clip-scoring-pipeline-novelty-exploration.cpp` | Busca novidades quando candidatos bons acabaram. |
| `clip-scoring-pipeline-option-mappers.cpp` | Converte options entre stages. |
| `clip-scoring-pipeline-summary.cpp` | Gera resumo/log dos resultados. |

---

## 18. Como candidatos são criados

### 18.1 TranscriptIndex

Arquivo: `transcript-index.*`.

Cria consultas eficientes sobre segmentos:

- segmentos dentro de range;
- texto agregado de range;
- gaps/pausas antes/depois;
- janelas temporais;
- índices de segmentos próximos a cues.

Ele evita repetir scans manuais por toda a transcrição em cada stage.

### 18.2 SemanticCoarseRetriever

Arquivo: `semantic-coarse-retriever.*`.

Divide a transcrição em janelas maiores e usa embeddings para encontrar regiões promissoras. É "coarse" porque não decide start/end final; só aponta regiões onde vale gerar candidatos.

Fluxo:

```text
transcript -> windows -> embedding(window) -> similarity(prototypes/target) -> top windows
```

### 18.3 CandidateSourceBuilder

Arquivo: `candidate-source-builder.*`.

Junta fontes de candidatos:

- janelas coarse semânticas;
- cues de viewer/pergunta;
- ranges vindos de feedback positivo;
- ranges exploratórios;
- janelas locais ao redor de segmentos fortes.

Ele deduplica ranges com `QSet`, evitando `QStringList::contains` em loops grandes.

### 18.4 CandidateArcPlanner

Arquivo: `candidate-arc-planner.*`.

Planeja ranges que tenham chance de conter arco completo. Em vez de pegar uma janela fixa qualquer, ele tenta respeitar:

```text
começo inteligível
parte de desenvolvimento
resolução local
não invadir próxima pergunta/tópico
```

No preset de viewer, ele marca se o range começa perto de cue de viewer.

### 18.5 CandidateGenerator

Arquivo: `candidate-generator.*`.

Gera candidatos brutos por estratégias diferentes. Ele não é o scorer final. A responsabilidade dele é produzir alternativas suficientes para os estágios seguintes escolherem.

### 18.6 CandidateBuilder

Arquivo: `candidate-builder.*`.

Materializa um `ClipCandidate` para um range:

```text
range -> segmentos sobrepostos -> texto agregado -> duração -> pausas -> densidade -> evidências iniciais
```

Ele calcula sinais estruturais iniciais porque esses sinais dependem diretamente do range.

### 18.7 CandidateBeamExpander

Arquivo: `candidate-beam-expander.*`.

Recebe candidatos fortes e cria variações de start/end. Isso corrige o caso comum em que o assunto é certo, mas o range começa tarde ou termina cedo.

### 18.8 CandidateStageLimiter

Arquivo: `candidate-stage-limiter.*`.

Impõe orçamento. Sem limite, combinações de janelas, cues, expansão e feedback podem gerar explosão combinatória.

---

## 19. Como scoring é calculado

O score final é composto. Não existe um único critério. O pipeline combina sinais baratos, semânticos, estruturais, feedback e reranker.

### 19.1 Score estrutural inicial

Exemplos de sinais:

| Sinal | Intuição |
|---|---|
| duração dentro do preset | Evita corte curto/longo demais. |
| densidade de fala | Evita silêncio ou trecho vazio. |
| pausa antes/depois | Favorece boundary natural. |
| pergunta/viewer cue | Favorece começo com contexto. |
| resolução/conclusão | Favorece fim completo. |
| penalidade de meta/stream management | Evita trecho operacional sem valor. |

Exemplo de fórmula documentada no código:

```text
textDensityScore = clamp(charsPerSecond / 8.0, 0, 1)
```

Para boundary com pausa:

```text
pauseBoundary = clamp(min(pauseBeforeSec, 4) * 0.12 + min(pauseAfterSec, 4) * 0.18)
```

### 19.2 CheapClipScorer

Arquivo: `cheap-clip-scorer.*`.

É barato porque usa texto, duração, pausas e heurísticas locais. Ele roda antes de etapas caras de embedding/reranking.

Avalia:

- presença de hook;
- linguagem de conclusão;
- sinais emocionais;
- sinais de advice/explanation/story/opinion/tutorial;
- ruído/transição/meta;
- duração;
- completude aproximada.

### 19.3 CurationSignals

Arquivo: `curation-signals.*`.

Funções como:

```text
emotionalScoreForText
adviceScoreForText
explanationScoreForText
storyScoreForText
opinionScoreForText
tutorialScoreForText
```

Elas são sinais lexicais, não modelos neurais. Servem como fallback e complemento aos embeddings.

### 19.4 SemanticClipScorer

Arquivo: `semantic-clip-scorer.*`.

Usa embeddings para comparar candidato com protótipos e target. Ele ajuda quando palavras exatas variam, mas a intenção semântica é parecida.

### 19.5 FeedbackSimilarityScorer

Arquivo: `feedback-similarity-scorer.*`.

Compara candidato com memória de ranges positivos/negativos. Pode penalizar ranges muito parecidos com rejeitados e favorecer ranges próximos a positivos, dependendo da origem e conflito.

### 19.6 ClipRanker

Arquivo: `clip-ranker.*`.

Ordena candidatos e aplica diversidade. A ideia é evitar resultado final com vários cortes quase iguais. O ranker considera score e separação/overlap.

### 19.7 IntervalDpSelector

Arquivo: `interval-dp-selector.*`.

Seleciona conjunto de intervalos ponderados respeitando orçamento e evitando overlap problemático. Usa programação dinâmica porque o problema é escolher subconjunto de intervalos com pesos.

---

## 20. Embeddings, protótipos e busca coarse

Arquivos:

```text
semantic-model.*
semantic-prototypes.*
semantic-embedding-settings.*
embedding-cache.*
semantic-coarse-retriever.*
semantic-clip-scorer.*
```

### 20.1 Providers

| Provider | Arquivo | Observação |
|---|---|---|
| llama-server embeddings | `llama-server-embedding-provider.*` | Chama endpoint HTTP. |
| llama-server reranker | `llama-server-reranker-provider.*` | Chama `/v1/rerank`. |
| llama.cpp embeddings | `llama-cpp-embedding-provider.*` | In-process opcional. |
| llama.cpp reranker | `llama-cpp-reranker-provider.*` | In-process opcional. |

### 20.2 EmbeddingCache

Evita recalcular embedding do mesmo texto/modelo. Cache é em memória durante execução da pipeline, não deve virar armazenamento permanente sem uma política clara de invalidação.

### 20.3 Protótipos

`semantic-prototypes.*` fornece frases protótipo por tipo de conteúdo. Essas frases são intencionalmente descritivas, porque o embedding captura significado aproximado.

---

## 21. Reranker semântico

Arquivo: `semantic-reranker.*`.

O reranker recebe candidatos finalistas e pergunta ao modelo qual deles combina melhor com a intenção. Diferente de embedding simples, um reranker pode comparar query/candidato em conjunto.

Uso esperado:

```text
query = preset + target + instruções de qualidade
candidate = texto/range/evidências
score = relevância do candidato para a query
```

O reranker deve ser aplicado tarde, depois de reduzir o número de candidatos, porque ele é mais caro.

---

## 22. Boundary refinement

### 22.1 BoundaryRefinementStage

Arquivo: `boundary-refinement-stage.*`.

Orquestra refinadores. Usa processamento em chunks para não duplicar lógica de `std::async` em vários lugares.

### 22.2 ExchangeArcBoundaryRefiner

Arquivo: `exchange-arc-boundary-refiner.*` e `internal/*.inc`.

Especializado em `viewer_message_response`. Procura:

```text
viewer cue/pergunta
começo real da resposta
continuação do mesmo assunto
resolução local
ponto antes de próxima pergunta/troca/meta
```

Os `.inc` internos separam responsabilidades:

| `.inc` | Função |
|---|---|
| `viewer-cues` | Detecta cues de viewer/pergunta. |
| `role-scoring` | Pontua papéis de segmentos. |
| `span-rules` | Regras temporais de range. |
| `recovery` | Fallbacks conservadores. |
| `evidence` | Evidências para logs/diagnóstico. |

### 22.3 ArcDpBoundaryRefiner

Arquivo: `arc-dp-boundary-refiner.*`.

Usa uma abordagem de programação dinâmica sobre estados de arco. A ideia é selecionar sequência de segmentos que maximize abertura/desenvolvimento/resolução e minimize defeitos.

### 22.4 WordBoundarySnapper

Arquivo: `word-boundary-snapper.*`.

Quando existem `WordTiming`, ajusta start/end para limites de palavra. Isso evita cortes começando no meio de fala.

---

## 23. Quality gate e diagnósticos

Arquivos:

```text
candidate-quality-gate.*
candidate-quality-rules.*
candidate-quality-gate-shape.cpp
candidate-quality-gate-support.cpp
candidate-quality-gate-recovery.cpp
viewer-arc-gate.*
```

### 23.1 CandidateQualityGate

O gate decide se um candidato pode ir para resultado final. Ele rejeita por motivos como:

- duração inválida;
- início sem contexto;
- final sem conclusão;
- troca de tópico;
- cauda longa;
- stream meta/agradecimento/doação fora do objetivo;
- ausência de viewer arc quando o preset exige;
- conflito com feedback negativo.

### 23.2 ViewerArcGate

Verifica se existe arco de viewer:

```text
viewer/comment/question -> answer/development -> local resolution
```

Não basta ter ponto de interrogação. O candidato precisa parecer uma troca completa.

### 23.3 Diagnósticos

Candidatos rejeitados podem virar diagnóstico. Isso permite o usuário entender e corrigir. Diagnóstico bom deve dizer:

- qual range foi rejeitado;
- por qual motivo;
- quais evidências sustentam a rejeição;
- se é recuperável por ajuste de boundary;
- se deve ou não virar sinal de treino.

---

## 24. Feedback: gravação, memória e influência futura

Arquivos:

```text
curation-feedback-store.*
curation-feedback-detail.hpp
curation-feedback-records.cpp
curation-feedback-memory-rules.cpp
curation-feedback-memory-conflicts.cpp
boundary-calibration.*
```

### 24.1 O que é salvo

Cada evento vira uma linha JSONL com dados como:

- vídeo;
- content id;
- preset;
- ranges sugeridos;
- ranges do usuário;
- decisão;
- diagnóstico;
- source;
- timestamps;
- sinais estruturados.

### 24.2 Content id

O content id tenta identificar o conteúdo de modo estável. Usa arquivo/transcript/suggestion conforme disponível. Ele é necessário porque nome de arquivo sozinho não é confiável.

### 24.3 Sinais positivos e negativos

Tipos comuns:

| Sinal | Origem |
|---|---|
| positivo aceito | usuário aceitou range. |
| positivo ajustado | range corrigido pelo usuário. |
| negativo rejeitado | usuário rejeitou range. |
| negativo original ajustado | range original antes da correção. |
| weak negative | rejeição fraca/exploratória. |
| ignoreForTraining | não entra como dado de treino. |

### 24.4 Influência futura

Feedback influencia por:

- suprimir ranges já rejeitados;
- recuperar ranges aceitos previamente;
- gerar candidatos guiados por positivos;
- penalizar candidatos parecidos com negativos;
- calibrar boundary;
- fornecer dataset offline para ranker.

### 24.5 Conflitos

Se um range aparece como positivo e negativo, `curation-feedback-memory-conflicts.cpp` tenta resolver considerando overlap, source, decisão explícita e pares original/corrigido.

---

## 25. Quando feedback vira dataset

Feedback está maduro quando existe diversidade suficiente:

- vídeos diferentes;
- assuntos diferentes;
- presets diferentes;
- positivos e negativos reais;
- ajustes de boundary feitos pelo usuário;
- diagnósticos não contaminados;
- rejeições que não são apenas bug de uma versão antiga.

Sinais de que ainda não está pronto:

- poucos vídeos;
- só negativos repetidos;
- todos os dados vêm do mesmo tipo de live;
- feedback contaminado por bug que apagou bons clipes;
- ranges default/no-marker salvos como se fossem decisão real;
- rejeições exploratórias tratadas como negativos fortes.

Ferramentas relacionadas:

```text
tools/analyze_boundary_feedback.py
tools/build_feedback_ranker_dataset.py
tools/calibrate_boundary_thresholds.py
tools/train_feedback_ranker.py
```

---

## 26. Upload para Opus Clip

Arquivos:

```text
src/ui/upload-flow.*
src/worker/upload-worker.*
src/opus/opus-clip-client.*
src/opus/opus-curation-payload-builder.*
```

### 26.1 Batch de gravações

`start_upload` pega `pendingRecordingPaths` e processa até 3 uploads em paralelo:

```text
MAX_PARALLEL_UPLOADS = 3
```

Cada arquivo ganha um `UploadWorker` em `QThread`. O progresso global é média simples dos workers.

### 26.2 UploadWorker

Responsabilidades:

- validar ranges;
- decidir se precisa resample;
- chamar FFmpeg;
- salvar markers/settings ajustados para vídeo resampled;
- salvar transcript ajustado para vídeo resampled;
- iniciar `OpusClipClient`;
- suportar cancelamento.

### 26.3 OpusClipClient

Fluxo HTTP:

```text
POST /api/upload-links
  -> recebe upload URL + uploadId

POST uploadUrl com X-Goog-Resumable:start
  -> recebe Location resumable

PUT Location com bytes do vídeo
  -> upload completo

POST /api/clip-projects
  -> cria projeto com curationPref

GET /api/clip-projects/<built-in function id>
  -> polling opcional até estado terminal
```

A API base fica em:

```text
https://api.opus.pro
```

---

## 27. Resample/corte local via FFmpeg antes do upload

O plugin pode criar um vídeo temporário contendo apenas ranges selecionados. Isso reduz créditos/tempo de upload quando a seleção é muito menor que o vídeo original.

### 27.1 Modo concatenado

Para ranges múltiplos no mesmo projeto:

```text
1. Para cada range, gera segment-N.mp4 com libx264/aac.
2. Escreve concat-list.txt.
3. Roda ffmpeg concat com -c copy.
4. Ajusta CurationSettings para novo vídeo.
5. Salva markers e transcript remapeados.
```

### 27.2 Upload independente por range

Quando `uploadClipRangesIndependently = true`, cada range pode virar payload MP4 em memória e projeto separado. O pool interno é:

```text
OpusIndependentProjectPoolSize = 4
```

Nesse modo, cada range vira vídeo de duração própria começando em `0.0`, e `skipCurate` é forçado para range fixo.

### 27.3 Resolução do FFmpeg

Candidatos de caminho:

- variável `CLIP_CROPPER_FFMPEG_PATH`;
- `data/obs-plugins/clip-cropper/ffmpeg/ffmpeg(.exe)`;
- diretório do OBS/app;
- `ffmpeg`/`ffmpeg.exe` no PATH.

---

## 28. Payload de curadoria enviado ao Opus

Arquivo: `opus-curation-payload-builder.*`.

Payload conceitual:

```json
{
  "range": { "startSec": 10.0, "endSec": 70.0 },
  "model": "ClipAnything",
  "topicKeywords": ["..."],
  "genre": "Auto",
  "skipCurate": false,
  "clipDurations": [[30, 90]],
  "prompt": "...",
  "userPrompt": "..."
}
```

Quando `skipCurate = true`, o plugin força range fixo:

```json
{
  "clipDurations": [[startSec, endSec]],
  "clip_start": [startSec],
  "clip_duration": [durationSec]
}
```

Quando `clipLengthPreset` define bounds, o builder limita min/max pela duração real do range.

---

## 29. llama.cpp, llama-server e batch seguro

### 29.1 llama-server

Vantagem: modelo roda fora do OBS. Se travar, tende a não derrubar o OBS.

Usos:

```text
/v1/embeddings
/v1/rerank
```

### 29.2 llama.cpp in-process

Ativado por CMake:

```text
-DCLIP_CROPPER_USE_LLAMA_CPP=ON
-DCLIP_CROPPER_LLAMA_CPP_DIR=deps/llama.cpp
```

Risco: roda dentro do OBS. Por isso precisa:

- micro-batch pequeno;
- cancelamento;
- RAII;
- evitar paralelismo agressivo;
- liberar buffers grandes cedo;
- não bloquear main thread.

### 29.3 LlamaCppBatchWrapper

Arquivo: `llama-cpp-batch-wrapper.hpp`.

Responsabilidades:

- dividir inputs em micro-batches;
- preservar ordem;
- retornar falhas por item/lote;
- permitir cancelamento;
- evitar duplicação de loops de batch nos providers.

---

## 30. Cancelamento, RAII e segurança de callbacks

Padrões usados:

| Situação | Padrão |
|---|---|
| memória de OBS retornada por `bfree` | `std::unique_ptr<char, decltype(&bfree)>` |
| QObject que pode ser destruído antes do callback | `QPointer` |
| worker em thread | `moveToThread`, `deleteLater`, `thread->quit` |
| upload HTTP cancelável | `QNetworkReply::abort` |
| processo FFmpeg/WhisperX cancelável | `QProcess::kill` + `waitForFinished` |
| payload grande em memória | `std::move` + swap com `QByteArray` vazio para liberar storage |

Regra: lambda que captura widget/QObject de vida incerta deve usar parent correto ou `QPointer`. Não capturar ponteiro cru se o callback pode rodar depois do widget morrer.

---

## 31. C++/Qt moderno usado no projeto

O projeto está em C++17. Coroutines C++20 não foram ativadas para não alterar toolchain/ABI do plugin OBS.

Usos aplicados:

- `QPointer` para callbacks Qt seguros;
- `std::unique_ptr` com deleter customizado para ponteiros OBS;
- `std::shared_ptr` apenas para estado compartilhado de callbacks;
- `std::move` em payloads/ranges grandes;
- `QSet` para dedupe O(n) médio;
- `QStringView` em consultas de evidência;
- templates header-only em `parallel-chunk-map.hpp`;
- chunks/async conservador em estágios pesados;
- streaming com `QProcess`/`QNetworkReply` sem carregar tudo quando possível.

Não usar recurso moderno só por estética. Usar quando reduz cópia, risco, duplicação ou bloqueio da UI.

---

## 32. Build, opções CMake e empacotamento

### 32.1 Dependências

- OBS/libobs dev package ou build local de OBS;
- obs-frontend-api quando `ENABLE_FRONTEND_API=ON`;
- Qt 6: Core, Gui, Widgets, Network, Multimedia, MultimediaWidgets;
- CURL;
- whisper.cpp/ggml;
- opcionalmente CUDA/GGML CUDA;
- opcionalmente llama.cpp;
- FFmpeg runtime opcional para pacote.

### 32.2 Opções CMake

| Opção | Uso |
|---|---|
| `ENABLE_FRONTEND_API` | Configuração disponível em `CMakeLists.txt`. |
| `ENABLE_QT` | Configuração disponível em `CMakeLists.txt`. |
| `USE_FLATPAK_PATH` | Configuração disponível em `CMakeLists.txt`. |
| `ENABLE_WHISPER_CUDA` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_USE_LLAMA_CPP` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_LLAMA_CPP_DIR` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_STATIC_DEPS` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_STATIC_QT` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_DEPLOY_QT_RUNTIME` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_STATIC_MSVC_RUNTIME` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_STATIC_CUDA_RUNTIME` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_GGML_NATIVE` | Configuração disponível em `CMakeLists.txt`. |
| `CLIP_CROPPER_FFMPEG_RUNTIME_DIR` | Configuração disponível em `CMakeLists.txt`. |
| `CLANGD_LIGHT_MODE` | Configuração disponível em `CMakeLists.txt`. |


### 32.3 CLANGD_LIGHT_MODE

`CLANGD_LIGHT_MODE=ON` evita importar dependências pesadas como whisper.cpp, ajudando indexação local. Use para IDE, não para pacote final.

### 32.4 Install/package

`tools/` não entra no install. `docs/` não existe mais. Modelos grandes não são empacotados; apenas `.gitkeep` garante pasta esperada.

No Windows, o plugin instala em:

```text
obs-plugins/64bit
```

e dados em:

```text
data/obs-plugins/clip-cropper
```

---

## 33. CI, scripts auxiliares e ferramentas offline

### 33.1 CI/scripts

A pasta `.github/scripts` contém scripts por plataforma. `build-aux` contém helpers de formatação/log.

### 33.2 Ferramentas offline

`tools/` é mantido no repositório, mas não instalado no pacote.

| Ferramenta | Uso |
|---|---|
| `analyze_boundary_feedback.py` | Analisa feedback de boundary e padrões de erro. |
| `build_feedback_ranker_dataset.py` | Constrói dataset a partir do JSONL de feedback. |
| `calibrate_boundary_thresholds.py` | Gera/calibra thresholds para `boundary-calibration.json`. |
| `train_feedback_ranker.py` | Treina ranker leve usando feedback exportado. |
| `whisperx_align_worker.py` | Worker chamado pelo plugin para WhisperX. |

---

## 34. Internacionalização

Arquivos:

```text
data/locale/en-US.ini
data/locale/pt-BR.ini
```

O código usa `obs_module_text(key)` encapsulado por `obsText`. Toda string de UI nova deve entrar nos dois arquivos. Logs técnicos podem continuar hardcoded em inglês quando forem diagnóstico para desenvolvedor.

Regra: se aparece para usuário final, entra no locale.

---

## 35. Troubleshooting por sintoma/log

### 35.1 `Could not find LibObsConfig.cmake`

O ambiente de build não tem OBS/libobs dev package ou `CMAKE_PREFIX_PATH` não aponta para o build do OBS.

Verifique:

```text
.deps/obs-studio/build_x64/libobs/libobsConfig.cmake
libobs_DIR
CMAKE_PREFIX_PATH
```

### 35.2 Modelo Whisper não encontrado

Verifique `whisper_model_file` e a pasta de modelos do plugin. O arquivo esperado é algo como:

```text
ggml-base.bin
ggml-small.bin
ggml-large-v3.bin
```

### 35.3 FFmpeg não encontrado

Configure `CLIP_CROPPER_FFMPEG_PATH` ou empacote runtime com `CLIP_CROPPER_FFMPEG_RUNTIME_DIR`.

### 35.4 Sugestão não gera candidatos bons

Verifique:

- transcript existe e tem segmentos;
- idioma de transcrição está correto;
- preset combina com o vídeo;
- embeddings/reranker estão acessíveis;
- feedback antigo não está suprimindo tudo;
- diagnósticos mostram motivo de rejeição.

### 35.5 Só aparecem diagnósticos

Isso significa que candidatos foram gerados, mas o gate rejeitou como final. Olhe motivos de diagnóstico. Se o usuário corrigir ranges, o feedback deve registrar positivo corrigido e negativo original.

### 35.6 Reaparece candidato já rejeitado

Verifique se o feedback foi salvo com content id correto e se não foi marcado como `ignoreForTraining`/weak negative.

### 35.7 OBS trava durante modelo local

Prefira `llama-server` externo ou reduza batch/modelo. Backend in-process deve ser conservador porque compartilha processo com OBS.

---

## 36. Mapa de arquivos por responsabilidade

### 36.1 Raiz, CMake, CI, locale e tools

{inventory_table(['.clang-format','.clangd','.gersemirc','.github/','build-aux/','cmake/','CMakeLists.txt','CMakePresets.json','LICENSE','README.md','buildspec.json','data/','models/','tools/'])}

### 36.2 `src/models`, `src/utils` e entrada OBS

{inventory_table(['src/models/','src/utils/','src/plugin-main.cpp','src/plugin-support.c.in','src/plugin-support.h'])}

### 36.3 UI

{inventory_table(['src/ui/'])}

### 36.4 Transcription

{inventory_table(['src/transcription/'])}

### 36.5 Opus e worker

{inventory_table(['src/opus/','src/worker/'])}

### 36.6 Curation, scoring e feedback

{inventory_table(['src/curation/'])}

---

## 37. Regras de manutenção

1. Antes de criar arquivo novo, defina a camada: UI, transcription, curation, opus, worker, utils ou model.
2. Antes de adicionar lógica ao `UploadReviewDialog`, verifique se ela pertence a widget extraído, preparation service, scoring ou feedback.
3. Antes de adicionar regra de scoring, escolha o estágio correto: geração, cheap scoring, semantic scoring, boundary, gate, reranker ou selection.
4. Regra booleana reutilizável deve ir para `*-rules.*` ou helper temático.
5. Callback Qt com vida incerta deve usar parent correto ou `QPointer`.
6. Não capture `this` cru em lambda assíncrona sem garantir vida do objeto.
7. Não use `QStringList::contains` em dedupe grande; prefira `QSet`.
8. Não copie `QVector<ClipCandidate>` grande por worker; processe por intervalo ou mova resultados.
9. Não salve modelos grandes no pacote; use placeholders e documentação.
10. Não instale `tools/`; ferramentas offline ficam no repositório.
11. Se remover uso de classe/struct, remova definição, include e entrada CMake quando aplicável.
12. Se extrair arquivo, limpe includes herdados do arquivo original.
13. Se criar setting de UI, adicione em `pt-BR.ini` e `en-US.ini`.
14. Se alterar formato de feedback/cache, documente schema e migração.
15. Se alterar scoring, documente fórmula, arquivo responsável e impacto esperado.
16. Se alterar upload/resample, documente como ranges são remapeados.
17. Se alterar transcrição, documente cache key e fallback.
18. Se mexer em CMake, rode checagem de fontes listadas e arquivo existente.
19. README deve explicar comportamento operacional, não apenas nome de classes.
20. A fonte da verdade da documentação é este README.
