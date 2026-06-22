# Documentação da Pipeline de Scoring, Ranking e Geração de Candidatos do Plugin de Cortes

> Documento didático sobre a arquitetura atual da pipeline de curadoria semântica do plugin, com foco em geração de candidatos, scoring, ranking, reranking, coarse retrieval, embeddings, protótipos semânticos, `TranscriptIndex`, `CheapClipScorer`, `SemanticClipScorer`, `QualityGate`, MMR e critérios de início, desenvolvimento e conclusão.

---

## 1. Objetivo da pipeline

A pipeline existe para responder a uma pergunta:

> “Dado um vídeo longo transcrito, quais trechos contínuos parecem bons cortes curtos ou médios, com começo claro, desenvolvimento e conclusão, sem misturar assuntos?”

No caso específico do preset `viewer_message_response`, a regra de negócio desejada é:

> Encontrar uma resposta contínua, sem interrupção, construída a partir de uma única mensagem/pergunta de viewer. O corte deve começar no primeiro ponto autossuficiente do assunto, seguir a resposta direta do speaker, e terminar na primeira resolução local, antes de saudação, agradecimento, próxima pergunta, outro comentário do chat, meta da live ou troca de assunto.

Em termos simples:

```text
Bom corte:
  pergunta/contexto claro
  resposta direta
  desenvolvimento
  conclusão local
  fim antes de outro assunto

Corte ruim:
  começa em saudação/agradecimento
  começa antes do gancho
  começa no meio sem contexto
  mistura duas perguntas
  termina depois da conclusão
  termina em outro assunto
  é só conversa casual/meta da live
```

---

## 2. Visão geral do fluxo

A pipeline atual pode ser pensada assim:

```text
Review dialog
  ↓
Preparação da transcrição
  ↓
TranscriptIndex
  ↓
SemanticCoarseRetriever
  ↓
Geração de candidatos por regiões semânticas
  ↓
Score estrutural / Cheap score
  ↓
Refino semântico de boundaries
  ↓
SemanticClipScorer
  ↓
SemanticReranker
  ↓
CandidateQualityGate
  ↓
ClipRanker + MMR
  ↓
Marcadores aplicados na UI de review
```

Cada etapa tem uma função diferente:

| Etapa | Função principal |
|---|---|
| Transcrição | Transformar áudio/vídeo em segmentos com texto e timestamps |
| `TranscriptIndex` | Permitir consultar texto, segmentos, pausas e ranges |
| `SemanticCoarseRetriever` | Encontrar regiões grandes e promissoras do vídeo |
| Geração de candidatos | Criar possíveis ranges de corte dentro dessas regiões |
| Scoring estrutural | Dar um score barato baseado em duração, densidade, boundary e pausas |
| Refino semântico | Tentar ajustar começo e fim do corte |
| `SemanticClipScorer` | Pontuar valor, hook, resolução, meta noise e continuidade |
| `SemanticReranker` | Usar reranker local para julgar se o trecho é bom ou ruim |
| `QualityGate` | Aceitar/rejeitar candidatos com base em restrições |
| `ClipRanker` | Ordenar e selecionar os melhores, evitando duplicados |

---

## 3. Glossário dos principais termos

### 3.1 Pipeline

Pipeline é a sequência de etapas pelas quais um vídeo passa até virar uma lista de cortes sugeridos.

Exemplo:

```text
transcrição → regiões promissoras → candidatos → scores → ranking → marcadores
```

---

### 3.2 Transcript

Transcript é a transcrição do vídeo.

Normalmente é uma lista de segmentos:

```text
[3020.00 - 3024.10] texto do segmento
[3024.10 - 3028.30] outro texto
[3029.52 - 3034.00] mais texto
```

Cada segmento tem:

```cpp
startSec;
endSec;
text;
```

---

### 3.3 Segmento

Segmento é uma unidade da transcrição.

Exemplo:

```text
[3029.52 - 3034.00] Eu acho que religião não define se uma pessoa é boa.
```

O Whisper gera esses segmentos com timestamps.

---

### 3.4 Range

Range é um intervalo de tempo candidato a corte.

Exemplo:

```text
3029.52s - 3063.66s
```

No código geralmente aparece como algo equivalente a:

```cpp
ClipDuration {
    startSec;
    endSec;
}
```

---

### 3.5 Candidate / Candidato

Candidato é um possível corte.

Ele tem:

```text
range
texto limpo
texto com timestamps
scores
evidências
source
status de rejeição ou aprovação
```

Um candidato ainda não é um corte final. Ele é só uma possibilidade.

---

### 3.6 Scorer

Scorer é um componente que dá uma pontuação para algo.

Ele responde:

> “Quão bom é esse candidato de acordo com certos critérios?”

Exemplos:

```text
CheapClipScorer
SemanticClipScorer
scoreStructurally
```

Um scorer não necessariamente escolhe o melhor. Ele só calcula notas.

---

### 3.7 Ranker

Ranker é um componente que ordena candidatos.

Ele responde:

> “Entre esses candidatos já pontuados, quais devem vir primeiro?”

Exemplo:

```text
ClipRanker
```

Diferença importante:

```text
Scorer = dá nota
Ranker = ordena com base nas notas
```

---

### 3.8 Reranker

Reranker é um ranker/classificador mais caro usado depois que já existem poucos candidatos.

No projeto, o reranker local usa:

```text
/v1/rerank
qwen3-reranker-0.6b-q8_0
```

Ele não gera texto. Ele recebe uma query e documentos/candidatos, e retorna scores.

Exemplo conceitual:

```text
Query:
"Este trecho é um bom corte com começo, desenvolvimento e conclusão?"

Documento:
"[3029.52-3034.00] texto..."
```

O reranker responde com uma pontuação.

---

### 3.9 Embedding

Embedding é uma representação numérica de um texto.

Exemplo conceitual:

```text
"resposta sobre religião" → [0.12, -0.45, 0.88, ...]
```

Textos semanticamente parecidos tendem a ter embeddings próximos.

No plugin, o embedding vem de:

```text
/v1/embeddings
qwen3-embedding-0.6b-q8_0
```

---

### 3.10 Protótipo semântico

Protótipo é um texto usado como referência semântica.

Exemplo:

```text
"uma resposta direta e útil para uma pergunta de viewer"
```

Esse texto vira embedding.

Depois, a pipeline compara:

```text
embedding do candidato
vs
embedding do protótipo
```

Se forem parecidos, o candidato recebe score alto naquele critério.

Protótipo não é regra fixa por string. Ele é uma âncora semântica.

Diferença:

```text
String fixa:
  if text contains "umbanda"

Protótipo semântico:
  comparar texto com "resposta opinativa sobre uma questão específica de viewer"
```

A segunda abordagem é muito mais sustentável.

---

### 3.11 Coarse

`Coarse` significa “grosso”, “aproximado”, “primeira filtragem”.

É o contrário de `fine`.

Na pipeline:

```text
Coarse:
  encontrar regiões grandes promissoras

Fine:
  decidir começo e fim exatos do corte
```

O `SemanticCoarseRetriever` não tenta achar o corte perfeito. Ele só acha zonas do vídeo onde provavelmente existe coisa boa.

---

### 3.12 Window

Window é uma janela de tempo.

Exemplo:

```text
window = 100 segundos
```

Significa que o vídeo será analisado em blocos de 100 segundos.

Exemplo:

```text
Window 1: 0s   - 100s
Window 2: 65s  - 165s
Window 3: 130s - 230s
```

---

### 3.13 Stride

Stride é o passo entre uma window e a próxima.

Exemplo:

```text
window = 100s
stride = 65s
```

Então as janelas começam a cada 65 segundos:

```text
0s, 65s, 130s, 195s...
```

Como `stride < window`, as janelas se sobrepõem.

Isso é importante porque um assunto pode começar perto do fim de uma window e continuar na próxima.

---

### 3.14 MMR

MMR significa `Maximal Marginal Relevance`.

É uma técnica para escolher candidatos que sejam:

```text
bons
e
diferentes entre si
```

Sem MMR, o sistema poderia escolher três cortes quase iguais:

```text
3020-3060
3024-3064
3028-3068
```

Com MMR, ele penaliza candidatos muito parecidos com os já escolhidos.

Fórmula conceitual:

```text
mmrScore = relevância - penalidadePorSimilaridade
```

No log:

```text
mmr=true
mmrRelevanceWeight=0.68
```

Significa:

```text
68% peso para qualidade
32% peso para diversidade
```

---

### 3.15 Hook

Hook é o gancho do corte.

É a parte que faz o trecho começar de forma interessante, compreensível e autossuficiente.

Exemplos bons:

```text
"Você acha que uma pessoa precisa ter religião para ser boa?"
"Sobre esse assunto, eu penso que..."
"Eu acho que o problema não é a religião em si..."
```

Exemplos ruins:

```text
"Obrigado pelo presente."
"Salve, boa noite."
"Calma aí, deixa eu ver aqui."
```

Hook bom não significa clickbait. Significa começo claro.

---

### 3.16 Desenvolvimento

Desenvolvimento é o corpo da resposta.

É onde o speaker:

```text
explica
argumenta
contextualiza
dá exemplo
desenvolve uma opinião
```

Hoje a pipeline não tem um score único chamado `development`. Ela infere desenvolvimento por:

```text
semanticClipValue
topicContinuity
textDensity
duration
lowMetaNoise
```

---

### 3.17 Conclusão / Resolução

Conclusão é o primeiro fechamento local da resposta.

Exemplo:

```text
"...então, para mim, existem pessoas boas com ou sem religião."
```

A conclusão ideal termina antes de:

```text
nova pergunta
novo comentário de viewer
saudação
agradecimento
meta da live
outro assunto
```

---

### 3.18 Meta noise

Meta noise é texto que fala sobre a live, chat, presentes, sub, doação, moderação ou operação do stream, em vez de ser conteúdo do assunto.

Exemplos:

```text
obrigado pelo presente
valeu pelo sub
calma chat
manda o próximo
vou abrir aqui
ban nesse cara
```

---

### 3.19 Topic shift

Topic shift é troca de assunto.

Exemplo:

```text
Conclusão sobre religião.
[pausa]
"Cuidar de si, Fulano?"
```

Esse segundo trecho pode até ser interessante, mas pertence a outro arco.

---

### 3.20 Quality Gate

Quality Gate é a “alfândega” dos candidatos.

Ele decide:

```text
passa
ou
rejeita
```

Com base em thresholds como:

```text
hook mínimo
valor mínimo
resolução mínima
meta noise máximo
topic shift máximo
reranker bad máximo
score final mínimo
```

---

## 4. `TranscriptIndex`

### 4.1 O que ele faz

`TranscriptIndex` é uma camada de consulta sobre a transcrição.

Ele não decide se um corte é bom. Ele só permite acessar a transcrição de forma eficiente.

Ele responde perguntas como:

```text
Qual é o texto entre 3020s e 3063s?
Quais segmentos estão dentro desse range?
Qual pausa existe antes desse range?
Qual pausa existe depois?
Qual a maior pausa interna?
Monte um texto com timestamps para o reranker.
```

---

### 4.2 Por que não usar a lista de segmentos diretamente?

Porque a pipeline precisa consultar ranges muitas vezes.

Exemplo:

```text
candidato A: 3020-3063
candidato B: 3029-3055
candidato C: 4236-4258
```

Para cada um, ela precisa rapidamente buscar:

```text
texto
timestamps
pausas
segmentos
densidade
```

O `TranscriptIndex` centraliza isso.

---

### 4.3 Texto limpo vs texto com timestamps

A pipeline usa dois tipos de texto:

#### Texto limpo

Usado para embeddings:

```text
Eu acho que religião não define se uma pessoa é boa...
```

#### Texto enriquecido

Usado para reranker:

```text
[3029.52-3034.10] Eu acho que religião não define se uma pessoa é boa.
[PAUSA 2.2s]
[3036.30-3039.00] Cuidar de si, Prates?
```

O texto enriquecido ajuda o reranker a perceber pausas, viradas e caudas.

---

### 4.4 Pausas

O `TranscriptIndex` calcula:

```text
pauseBeforeSec
pauseAfterSec
maxInternalPauseSec
```

Exemplo:

```text
pauseBefore=3.9
pauseAfter=1.0
internalPause=2.2
```

Esses valores aparecem no log.

Eles ajudam a detectar:

```text
começo depois de pausa forte
fim antes de pausa forte
troca de turno dentro do candidato
```

---

## 5. `SemanticCoarseRetriever`

### 5.1 O que ele é

`SemanticCoarseRetriever` é a etapa que procura regiões grandes e promissoras do vídeo.

Ele é “coarse” porque não tenta achar o corte exato.

Ele pergunta:

> “Em quais blocos grandes da transcrição parece haver uma resposta boa?”

---

### 5.2 Ele passa pelo llama?

Sim, mas usando o endpoint de embedding:

```text
http://127.0.0.1:8080/v1/embeddings
```

Ele não usa geração de texto.

Ele faz:

```text
texto da window → embedding
protótipo → embedding
similaridade → score
```

---

### 5.3 Como funcionam as windows

No log, havia:

```text
coarseWindowSec=100.00
coarseStrideSec=65.00
```

Isso gera blocos assim:

```text
0s   - 100s
65s  - 165s
130s - 230s
195s - 295s
...
```

Cada bloco vira uma window.

---

### 5.4 Por que usar window e stride?

Porque o vídeo pode ter mais de duas horas.

Testar todos os cortes possíveis seria caro demais.

Então a pipeline primeiro folheia o vídeo em blocos.

A sobreposição evita quebrar assuntos no meio.

Sem overlap:

```text
0-100
100-200
```

Um assunto de `95-115` ficaria dividido.

Com overlap:

```text
0-100
65-165
```

O assunto aparece inteiro na segunda window.

---

### 5.5 Como a região recebe score

Cada window é comparada com protótipos:

```text
pergunta de viewer
resposta direta
valor de corte
target específico
meta noise
topic shift
```

O score sobe quando a window parece conter:

```text
pergunta ou mensagem de viewer
resposta direta
opinião/conselho/explicação
conteúdo autossuficiente
potencial de corte
```

O score cai quando parece conter:

```text
agradecimento
saudação
doação/sub/presente
gestão de live
moderação
conversa casual sem arco
```

---

### 5.6 O que sai do coarse retriever

Ele retorna regiões promissoras.

Exemplo conceitual:

```text
Região 1: 2965s - 3065s
Região 2: 4200s - 4300s
Região 3: 6000s - 6100s
```

A partir disso, a pipeline gera candidatos.

---

## 6. Geração de candidatos por regiões semânticas

### 6.1 O que significa “gerar candidatos”

Não é gerar texto.

É gerar ranges de tempo possíveis.

Exemplo:

```text
3020.00 - 3044.00
3020.00 - 3065.00
3029.52 - 3063.66
3005.00 - 3095.00
```

Cada range vira um candidato.

---

### 6.2 Por que gerar vários candidatos?

Porque o coarse só sabe que uma região é boa.

Ele não sabe exatamente:

```text
onde começa o gancho
onde termina a conclusão
se precisa de 24s, 45s, 90s ou 120s
```

Então a pipeline tenta várias possibilidades.

---

### 6.3 Durações usadas

Para o preset atual, como `viewer_message_response` não deve ficar preso a short, o teto foi elevado para:

```text
8s mínimo
180s máximo
```

A geração pode testar durações como:

```text
24s
45s
60s
90s
120s
150s
180s
```

Depois as etapas seguintes escolhem e refinam.

---

## 7. `CheapClipScorer`

### 7.1 O que ele faz

`CheapClipScorer` é um scorer barato.

Ele usa sinais rápidos e locais, sem depender necessariamente de embeddings/reranker.

Ele pontua coisas como:

```text
duração
boundary
densidade de texto
hook lexical/estrutural
viewer response
continuidade
ruído
```

---

### 7.2 Por que ele existe se já temos embedding?

Porque nem sempre o embedding/reranker estará disponível.

Além disso, score barato ajuda a filtrar antes de rodar etapas mais caras.

---

### 7.3 Limitação

Ele é mais frágil que embedding/reranker porque tende a depender mais de sinais lexicais.

O ideal é que ele fique restrito a estrutura genérica:

```text
tem pergunta?
tem pontuação?
tem pausa?
tem densidade?
tem vocativo estrutural?
tem duração aceitável?
```

E não tente decidir semanticamente temas específicos.

---

## 8. `TextAnalysis`

### 8.1 O que ele faz

`TextAnalysis` contém funções auxiliares para detectar padrões linguísticos.

Exemplos:

```text
parece pergunta?
parece nova fala de viewer?
parece cumprimento?
parece meta/social?
parece troca de assunto?
parece continuação do mesmo exchange?
```

---

### 8.2 Problema com strings fixas

Se o código faz:

```text
if text contains "Prates"
if text contains "umbanda"
if text contains "block blast"
```

isso é frágil.

Funciona para um vídeo, mas quebra em outros.

---

### 8.3 Abordagem sustentável

A camada local deve detectar estrutura genérica:

```text
interrogação
vocativo estrutural
pontuação
pausa
baixa densidade
marcadores genéricos de saudação/agradecimento
```

A parte semântica deve ficar com:

```text
embedding
protótipos
reranker
feedback histórico
```

---

## 9. `SemanticPrototypes`

### 9.1 O que são

São textos de referência usados para comparação semântica.

Exemplo:

```text
"uma pergunta específica de um viewer pedindo opinião ou conselho"
"uma resposta direta e completa para essa pergunta"
"um trecho dominado por agradecimento, saudação ou meta da live"
"uma conclusão local antes de mudar de assunto"
```

Esses textos viram embeddings.

---

### 9.2 Diferença entre protótipo e keyword

Keyword:

```text
contains("obrigado")
```

Protótipo:

```text
similaridade com "trecho de agradecimento casual antes do conteúdo principal"
```

O protótipo é mais flexível.

Ele pode reconhecer variações:

```text
valeu
obrigadão
thanks
agradeço
muito obrigado pelo presente
```

Sem listar tudo manualmente.

---

### 9.3 Protótipos por idioma

A pipeline foi ajustada para usar protótipos conforme o idioma:

```text
transcriptionLanguage=pt
semanticLanguage=pt
```

Assim, os protótipos ficam em português quando a transcrição é em português.

Isso melhora:

```text
hook
resolução
meta noise
topic shift
pergunta de viewer
resposta direta
```

---

## 10. `SemanticClipScorer`

### 10.1 O que ele faz

`SemanticClipScorer` pontua semanticamente cada candidato.

Ele usa embeddings para comparar o candidato com protótipos.

Ele calcula scores como:

```text
semanticClipValue
semanticHook
semanticOpeningHook
semanticResolution
semanticEndingResolution
semanticMetaNoise
semanticOpeningMetaNoise
semanticEndingMetaNoise
semanticEndingTopicShift
topicContinuity
viewerResponse
```

---

### 10.2 Hook

Há dois conceitos:

```text
semanticHook
semanticOpeningHook
```

`semanticHook` mede se o trecho como um todo tem potencial de gancho.

`semanticOpeningHook` mede se o começo do corte já é um bom gancho.

Exemplo:

```text
"Obrigado pelo presente... sobre religião, eu acho..."
```

Pode ter:

```text
semanticHook médio/alto
semanticOpeningHook baixo
semanticOpeningMetaNoise alto
```

Porque o tema é bom, mas o começo é ruim.

---

### 10.3 Desenvolvimento

Hoje a pipeline não possui um score único chamado `development`.

O desenvolvimento é inferido por:

```text
semanticClipValue
topicContinuity
textDensity
duration
lowMetaNoise
```

Isso significa:

```text
tem conteúdo útil?
continua no mesmo assunto?
não é frase solta?
não é só meta/social?
tem corpo suficiente?
```

---

### 10.4 Conclusão

A conclusão é avaliada por:

```text
semanticResolution
semanticEndingResolution
semanticEndingMetaNoise
semanticEndingTopicShift
```

`semanticResolution` mede se o trecho contém resolução em algum lugar.

`semanticEndingResolution` mede se o final do corte parece uma resolução.

Isso é importante porque um corte pode ter resolução e ainda terminar tarde demais.

Exemplo:

```text
"...existem pessoas boas com ou sem religião."
[pausa]
"cuidar de si, Fulano?"
```

O primeiro trecho é conclusão. O segundo já deveria estar fora.

---

## 11. Refino semântico de boundaries

### 11.1 Por que precisa de refino?

A geração de candidatos cria ranges aproximados.

Exemplo:

```text
3020.00 - 3063.66
```

Mas o corte correto talvez seja:

```text
3029.52 - 3055.00
```

O refinador tenta aparar começo e final.

---

### 11.2 Como funciona

Ele divide o candidato em chunks.

Exemplo:

```text
Chunk 1: agradecimento / prólogo social
Chunk 2: pergunta ou início real do assunto
Chunk 3: desenvolvimento da resposta
Chunk 4: conclusão
Chunk 5: pausa + novo assunto
```

Depois testa spans possíveis:

```text
Chunk 1-4
Chunk 2-4
Chunk 2-5
Chunk 3-4
```

O melhor span deveria ser:

```text
Chunk 2-4
```

---

### 11.3 O que é um boundary bom

Começo bom:

```text
começa no assunto
é autossuficiente
não começa em saudação/agradecimento
não começa com meta da live
não começa no meio de frase sem contexto
```

Final bom:

```text
termina na primeira resolução local
não inclui próxima pergunta
não inclui outro comentário
não inclui pós-conclusão
não inclui meta/social depois
```

---

### 11.4 Pausas

Pausas ajudam a detectar trocas.

Exemplo:

```text
resolução
[PAUSA 2.2s]
novo vocativo/pergunta
```

Isso sugere:

```text
fim antes da pausa
```

---

## 12. `SemanticReranker`

### 12.1 O que ele faz

O reranker reavalia candidatos com um modelo mais adequado para ranking.

Ele usa:

```text
/v1/rerank
qwen3-reranker-0.6b-q8_0
```

Ele não gera texto.

Ele recebe:

```text
query
documento/candidato
```

E retorna uma pontuação.

---

### 12.2 Query positiva

A query positiva pergunta:

```text
Este trecho é um bom corte completo?
```

Critérios:

```text
uma única mensagem de viewer
resposta direta
início claro
desenvolvimento
primeira resolução local
sem misturar assunto
sem saudação/meta no começo
sem cauda no final
```

Score resultante:

```text
rerankerRaw
```

---

### 12.3 Query negativa

A query negativa pergunta:

```text
Este trecho tem defeitos graves?
```

Defeitos:

```text
começa em saudação
começa em agradecimento
é dominado por meta/social
mistura assuntos
tem cauda após a conclusão
tem nova pergunta no final
não tem arco completo
```

Score resultante:

```text
rerankerBadClip
```

---

### 12.4 Margin

A margem é:

```text
margin = rerankerRaw - rerankerBadClip
```

Interpretação:

```text
raw alto, bad baixo -> candidato forte
raw alto, bad alto -> candidato ambíguo
raw baixo, bad alto -> candidato ruim
raw baixo, bad baixo -> candidato irrelevante ou pouco claro
```

---

## 13. `CandidateQualityGate`

### 13.1 O que ele faz

Ele decide se um candidato pode ir para o ranking final.

Ele aplica restrições mínimas.

---

### 13.2 Restrições positivas

O candidato precisa ter sinais como:

```text
valor semântico
hook
opening hook
resolução
ending resolution
viewer response
continuidade
score final suficiente
```

Exemplo conceitual:

```text
semanticClipValue >= mínimo
semanticOpeningHook >= mínimo
semanticEndingResolution >= mínimo
topicContinuity >= mínimo
```

---

### 13.3 Restrições negativas

O candidato não pode ser dominado por:

```text
opening meta noise
ending meta noise
topic shift
semantic noise
reranker bad clip
pausa longa não aparada
stream management
texto insuficiente
```

Exemplo conceitual:

```text
semanticOpeningMetaNoise <= máximo
semanticEndingTopicShift <= máximo
rerankerBadClip <= máximo
```

---

### 13.4 Problema de calibração

Se o gate for severo demais:

```text
zero marcadores
```

Se for permissivo demais:

```text
passa corte com saudação
passa corte com final tarde
passa lixo sem arco
```

Por isso o gate precisa ser usado como validação, não como substituto de boundary detection.

---

## 14. `ClipRanker`

### 14.1 O que ele faz

Ordena os candidatos aprovados.

Ele considera:

```text
final score
reranker score
boundary score
tempo de início
diversidade
overlap
```

---

### 14.2 MMR no ranking

MMR evita selecionar candidatos parecidos.

Exemplo sem MMR:

```text
3020-3063
3022-3065
3025-3068
```

Exemplo com MMR:

```text
3020-3063
4236-4258
6042-6061
```

O objetivo é cobrir assuntos/regiões diferentes.

---

## 15. Diferença entre os principais componentes

### 15.1 Coarse vs Fine

| Conceito | Função |
|---|---|
| Coarse | Achar regiões grandes promissoras |
| Fine | Decidir começo/fim e qualidade exata |

Coarse responde:

```text
Onde vale procurar?
```

Fine responde:

```text
Qual é o corte exato?
```

---

### 15.2 Scorer vs Ranker

| Conceito | Função |
|---|---|
| Scorer | Dá pontuação |
| Ranker | Ordena candidatos |

Scorer:

```text
este candidato vale 0.72
```

Ranker:

```text
este candidato é melhor que aquele
```

---

### 15.3 Ranker vs Reranker

| Conceito | Função |
|---|---|
| Ranker | Ordena com base nos scores já calculados |
| Reranker | Usa um modelo específico para reavaliar candidatos |

Reranker geralmente é mais caro e usado depois de filtrar candidatos.

---

### 15.4 Protótipo vs Prompt

Protótipo:

```text
Texto curto usado como referência semântica para embedding.
```

Prompt/query do reranker:

```text
Instrução usada para classificar/rankear um documento.
```

Protótipo responde:

```text
este texto é parecido semanticamente com este conceito?
```

Reranker responde:

```text
este documento satisfaz esta tarefa?
```

---

### 15.5 Embedding vs Reranker

| Item | Embedding | Reranker |
|---|---|---|
| Entrada | Texto | Query + documento |
| Saída | Vetor | Score |
| Uso | Similaridade | Julgamento de relevância |
| Custo | Menor | Maior |
| Bom para | Buscar regiões/candidatos | Decidir qualidade final |

---

### 15.6 Score vs Evidence

Score é número.

Exemplo:

```text
semanticClipValue=0.80
```

Evidence é rastro de diagnóstico.

Exemplo:

```text
semantic_clip_value
semantic_boundary_end_refined
reranker_strong_match
```

Evidence serve para entender por que o candidato passou ou falhou.

---

## 16. Como a pipeline decide o que é útil

Um candidato é útil quando combina:

```text
tema interessante
resposta direta
arco completo
início claro
desenvolvimento
conclusão local
baixa meta/social
boa continuidade
bom reranker
```

Sinais positivos:

```text
semanticClipValue alto
semanticHook alto
semanticOpeningHook alto
semanticResolution alto
semanticEndingResolution alto
topicContinuity alto
viewerResponse alto
rerankerRaw alto
rerankerBadClip baixo
```

Sinais negativos:

```text
semanticOpeningMetaNoise alto
semanticEndingMetaNoise alto
semanticEndingTopicShift alto
rerankerBadClip alto
noise alto
pausa interna não aparada
topicContinuity baixo
```

---

## 17. Como a pipeline deveria classificar início, desenvolvimento e conclusão

### 17.1 Início

Deveria responder:

```text
O corte começa no ponto em que o assunto fica claro?
O primeiro trecho é autossuficiente?
O início é conteúdo ou é social/meta?
```

Scores atuais relacionados:

```text
semanticOpeningHook
semanticOpeningMetaNoise
viewerResponse
boundary
pauseBeforeSec
```

---

### 17.2 Desenvolvimento

Deveria responder:

```text
Existe corpo da resposta?
O speaker desenvolve o assunto?
O trecho não é só uma frase solta?
O assunto se mantém coerente?
```

Scores atuais relacionados:

```text
semanticClipValue
topicContinuity
textDensity
duration
semanticResolution
```

Ponto fraco atual:

```text
Não existe ainda um score explícito chamado semanticDevelopment.
```

---

### 17.3 Conclusão

Deveria responder:

```text
O final é a primeira resolução local?
O corte para antes de novo assunto?
O final não está carregando cauda social/meta?
```

Scores atuais relacionados:

```text
semanticEndingResolution
semanticEndingMetaNoise
semanticEndingTopicShift
pauseAfterSec
maxInternalPauseSec
rerankerBadClip
```

---

## 18. Pontos fortes da arquitetura atual

A arquitetura atual tem bons elementos:

```text
usa transcript cache
usa embeddings locais
usa reranker local
usa protótipos por idioma
tem coarse retrieval para vídeos longos
usa MMR para diversidade
tem evidence nos logs
tem tentativa de pause-aware boundary
tem teto global de 180s para arco completo
```

Isso é bem mais avançado que uma simples busca por palavras-chave.

---

## 19. Pontos fracos atuais

### 19.1 Boundary detection ainda está espalhado

Hoje começo/fim são influenciados por:

```text
Semantic topic span
SemanticClipScorer
SemanticReranker
QualityGate
CheapClipScorer
TextAnalysis
```

Isso dificulta calibrar.

O ideal seria um componente dedicado:

```text
BoundaryRefiner
ou
ExchangeArcDetector
```

---

### 19.2 Desenvolvimento não tem score próprio

Hoje desenvolvimento é inferido.

Seria melhor ter:

```cpp
semanticDevelopment
answerBodyScore
arcCompleteness
```

---

### 19.3 Quality Gate faz papel demais

O gate deveria só validar.

Hoje ele também acaba compensando falhas de boundary e scoring.

Isso cria oscilações:

```text
zero marcadores
ou
marcadores ruins
```

---

### 19.4 Heurísticas lexicais ainda existem

Mesmo após refatoração, ainda há sinais lexicais genéricos.

Eles devem ser usados apenas como fallback fraco.

A decisão semântica principal deve vir de:

```text
embedding
reranker
feedback do usuário
```

---

## 20. Arquitetura ideal futura

Uma arquitetura mais estável seria:

```text
1. TranscriptIndex
   Fornece texto, timestamps e pausas.

2. SemanticCoarseRetriever
   Encontra regiões promissoras.

3. CandidateGenerator
   Cria candidatos largos.

4. ExchangeArcDetector / BoundaryRefiner
   Classifica chunks:
     social_prelude
     viewer_question
     answer_opening
     answer_body
     local_resolution
     tail_new_turn
     tail_meta

5. SemanticClipScorer
   Pontua o corte já aparado.

6. SemanticReranker
   Reavalia qualidade global.

7. QualityGate
   Valida restrições.

8. ClipRanker + MMR
   Seleciona os melhores.
```

O componente novo mais importante seria:

```cpp
struct ArcBoundaryDecision {
    int startSegmentIndex;
    int endSegmentIndex;

    double openingConfidence;
    double developmentConfidence;
    double resolutionConfidence;
    double cleanBoundaryConfidence;

    bool startsWithSocialPrelude;
    bool hasAnswerBody;
    bool endsAtLocalResolution;
    bool hasTailNewTurn;
};
```

Assim, a pipeline deixaria de depender de score agregado para decidir boundary.

---

## 21. Exemplo completo com o caso do corte problemático

Candidato gerado:

```text
3020.00 - 3063.66
```

Conteúdo aproximado:

```text
[3020] agradecimento / prólogo
[3029] início real do assunto
[3040] desenvolvimento
[3055] conclusão local
[pausa]
[3063] novo assunto / vocativo / nova pergunta
```

O comportamento ideal seria:

```text
remover 3020-3029
manter 3029-3055
remover 3055-3063
```

Scores esperados:

```text
Chunk 3020-3029:
  openingMetaNoise alto
  openingHook baixo

Chunk 3029-3040:
  openingHook alto
  viewerResponse alto

Chunk 3040-3055:
  clipValue alto
  topicContinuity alto
  resolution alto

Chunk 3055-3063:
  topicShift alto
  endingMetaNoise alto ou newTurn alto
```

Range final esperado:

```text
3029.52 - 3055.xx
```

---

## 22. Resumo final

A pipeline atual é uma combinação de:

```text
busca semântica coarse
geração de ranges
scoring estrutural
scoring semântico
reranking
quality gate
ranking final com diversidade
```

A diferença entre os termos principais é:

```text
Coarse:
  achar regiões grandes boas.

Window:
  bloco de tempo analisado.

Stride:
  passo entre blocos.

Protótipo:
  texto usado como referência semântica para embedding.

Scorer:
  calcula notas.

Ranker:
  ordena candidatos.

Reranker:
  reavalia candidatos com modelo de ranking.

Quality Gate:
  aceita ou rejeita.

MMR:
  escolhe bons candidatos sem duplicar assunto/região.

TranscriptIndex:
  consulta transcrição, ranges, timestamps e pausas.
```

A regra de negócio central continua sendo:

```text
um único assunto
uma única resposta
começo claro
desenvolvimento
primeira conclusão local
sem saudação/meta no começo
sem cauda/outro assunto no final
até 180s
```

O próximo grande salto de qualidade não deve ser mais adicionar strings ou thresholds soltos. Deve ser extrair uma etapa explícita de detecção de arco:

```text
ExchangeArcDetector / BoundaryRefiner
```

Essa etapa classificaria cada chunk como parte do arco ou ruído antes/depois do arco. A partir daí, o resto da pipeline ficaria mais simples e mais previsível.

---
