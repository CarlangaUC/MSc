# Backbone 2 + Métricas — texto LaTeX actualizado (julio 2026)

Incluye verify wiki\_2gb (`u+t` y `log`), consultas CUDD build/loaded con paridad, y tabla bpi post `NZDD_INIT_UNIQUE_SLOTS=8`.

## Consultas en `verify` (referencia operativa)

Además de tags y round-trip, `verify` ejecuta **las mismas intersecciones CUDD** sobre el bosque en
**build** (manager post-trim) y **loaded** (manager limpio tras `loadZddPack`), y exige paridad.

| Id | Operación | Ground truth |
|---|---|---|
| **Q1** | `\| ZDD^{Abraham} ∩ snapshot(S_i^v) \|` | `1` (primer snapshot del término en `.docs`) |
| **Q2** | `\| ZDD^{Abraham} ∩ ZDD^{beta} \|` | snapshots idénticos entre ambos términos (tags no cruzan) |

Términos: `Abraham` → termA; `beta` → termB (fallback: otro `term_id` distinto). Implementación:
[`plus_t/export/export.h`](../plus_t/export/export.h) (`runForestQueryChecks`).

Comandos y todos los modos CLI: [PIPELINE_UIHRDC_CUDD.md](PIPELINE_UIHRDC_CUDD.md).

```bash
./zdd_cudd_plus_t verify u+t resultados_test/wiki_2gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_2gb_named.voc \
  /tmp/wiki_2gb_verify.zpack 0 \
  2>&1 | tee resultados_test/wiki_2gb_verify_queries_uplus.log
```

```latex
\paragraph{Backbone 2: bosque \texorpdfstring{$\mathrm{ZDD}^{t}$}{ZDDt}
autoidentificable con codificación \texorpdfstring{$\{u+t\}$}{u+t} o
\texorpdfstring{$\varphi(t)$}{phi(t)}}

El bosque del Backbone~1 es anónimo: la raíz de $\mathrm{ZDD}^{t}$ solo se conoce por su
posición en \texttt{pointerList}$[t]$, y un diagrama aislado no revela a qué término
pertenece. El Backbone~2 hace cada familia \emph{autoidentificable} añadiendo a $F_t$ un
subconjunto de codificación reservado fuera del dominio de documentos. Sea
\[
F_t = \bigcup_{s \in \mathcal{S}_t} \mathrm{ZDD}(s),
\]
donde $\mathcal{S}_t$ agrupa los snapshots distintos de la posting list del término $t$.
El mismo motor admite dos codificaciones físicas del tag, seleccionadas en ejecución:
\[
\begin{aligned}
\texttt{u+t}:&\qquad
  \mathrm{ZDD}^{t}=F_t\cup\bigl\{\{u+t\}\bigr\},\\
\texttt{log}:&\qquad
  \mathrm{ZDD}^{t}=F_t\cup\bigl\{\varphi(t)\bigr\},\qquad
  \varphi(t)=\{\tau_b\mid \operatorname{bit}_b(t)=1\}.
\end{aligned}
\]
Aquí $u = 2^{40}$ es el límite superior exclusivo del dominio de \texttt{master\_id}
(los 40 bits de \texttt{version\_packing.h}) y $t \in \{1,\ldots,V\}$ es 1-indexed.
Los masters viven en $[0,u)$ y las identidades lógicas $\{u+1\},\ldots,\{u+V\}$ en
$[u+1,u+V]$, sin colisión entre ambos ejes. En el modo \texttt{log}, $\varphi(t)$ no es
un documento: es el subconjunto de variables auxiliares $\tau_b$ que codifica en binario
el mismo término lógico $\{u+t\}$.

La implementación utiliza un único ejecutable \texttt{zdd\_cudd\_plus\_t}, compilado
desde \texttt{plus\_t/main.cpp}. El motor compartido reside en
\texttt{plus\_t/engine/engine.h}; los comandos CLI, la serialización y el modo
\texttt{demo} están en \texttt{plus\_t/cmd/}, \texttt{plus\_t/export/} y
\texttt{plus\_t/demo/}. Se conserva el modelo del Backbone~1: versión fuera del
diagrama, snapshots deduplicados al agrupar por versión, \emph{postings} de 64 bits y
pool CUDD compartido. La diferencia se concentra en el layout de variables y en la
función \texttt{addEncodingToFt}:
\[
\begin{array}{lll}
\texttt{u+t}:&
  \operatorname{var}(u+t)=t,&
  \texttt{docOffset}=V+1,\\[2mm]
\texttt{log}:&
  \operatorname{var}(\tau_b)=1+b,&
  \texttt{tagWidth}=\lceil\log_2(V+1)\rceil,\quad
  \texttt{docOffset}=1+\texttt{tagWidth},\\[2mm]
\text{ambos}:&
  \operatorname{var}(m)=m+\texttt{docOffset},&
  \texttt{numZddVars}=\texttt{maxMaster}+\texttt{docOffset}+1.
\end{array}
\]
En \texttt{wiki\_1gb} ($V = 147\,992$), el modo \texttt{u+t} desplaza los masters al
nivel $147\,993$; en \texttt{wiki\_2gb} ($V = 250\,550$), el modo \texttt{log} reserva
solo $\texttt{tagWidth}=18$ variables tag y usa $\texttt{docOffset}=19$.

Para cada término, \texttt{buildFtPointersForRange} construye localmente $F_t$ como
unión balanceada de snapshots, luego \texttt{addEncodingToFt} crea el subconjunto de
codificación y ejecuta \texttt{Cudd\_zddUnion}; la raíz resultante queda en
\texttt{pointerList}$[t]$. Así, $F_t$ existe transitoriamente durante la iteración, pero
no hay una segunda pasada global que etiquete un bosque ya terminado. Tras la unión se
libera la referencia temporal a $F_t$. La compartición global por \emph{hash-consing}
del \texttt{DdManager} único se mantiene intacta.

El sobrecosto es marginal y verificable. En modo \texttt{u+t}, el \texttt{.zpack} de
\texttt{wiki\_1gb} pasa de $4\,976\,605$ a $5\,122\,668$ nodos ($+2.9\%$). Cada familia
gana exactamente un subconjunto adicional: el término \texttt{Abraham}
($t = 23\,828$) pasa de $|F_t| = 30$ a $|\mathrm{ZDD}^{t}| = 31$, con $|M_t| = 20$
invariante, y su raíz responde $\texttt{tag} = \{u + 23\,829\}$ sin consultar el
vocabulario. En \texttt{wiki\_2gb}, el modo \texttt{u+t} serializa $16\,806\,290$ nodos
frente a $16\,555\,740$ sin tags ($+1.5\%$); en modo \texttt{log}, $16\,931\,564$ nodos
($+2.3\%$), con $\texttt{numZddVars}=4\,346$ frente a un diseño canónico que reservaría
$O(V)$ variables solo para tags.

El modo \texttt{verify} reconstruye el bosque desde el \texttt{.docs} y comprueba
término a término que (i) el tag leído coincide con $t$, (ii)
$|\mathrm{ZDD}^{t}| = |\mathcal{S}_t| + 1$, donde el término adicional es el
subconjunto de codificación, y (iii) el \emph{round-trip} \texttt{.zpack} preserva
\texttt{DagSize}, cardinalidad, identidad lógica y $|M_t|$. Además ejecuta consultas
CUDD empíricas en build y en loaded: (iv)~$|\mathrm{ZDD}^{t_A}\cap S^v|=1$ sobre el
primer snapshot de \texttt{Abraham}, y (v)~$|\mathrm{ZDD}^{t_A}\cap\mathrm{ZDD}^{t_B}|$
igual al número de snapshots idénticos entre $t_A$ y $t_B$; las respuestas deben
coincidir en ambas fases (paridad build/loaded), demostrando que la EDD serializada
conserva la semántica consultable pese a un pool más liviano en RAM
(\texttt{bytes\_cudd\_loaded} $\approx 57\%$ de build en \texttt{wiki\_2gb}).
Sobre \texttt{wiki\_1gb} en modo \texttt{u+t} arroja $0$ discrepancias de tag,
$0$ de \emph{round-trip} y $0$ de consulta en los $147\,992$ términos
(\texttt{overall: PASS}), con recarga del bosque completo en $\sim\!4.4$~s.
Sobre \texttt{wiki\_2gb}, ambos modos terminan en \texttt{PASS} con
$0/250\,550$ discrepancias de tag, \emph{round-trip} y consulta
($\sim\!19$~s en \texttt{u+t}, $\sim\!18$~s en \texttt{log}).
En \texttt{wiki\_2gb}, \texttt{Abraham} ($t = 40\,422$) cumple $|\mathrm{ZDD}^{t}| = 50$,
$|M_t| = 43$ y decodifica $\{u + 40\,423\}$ en ambos modos.
El archivo \texttt{.zpack} no guarda explícitamente la codificación empleada, por lo que
\texttt{load} y \texttt{verify} deben recibir el mismo argumento \texttt{u+t} o
\texttt{log} usado al construir el bosque. Un modo \texttt{demo} auxiliar ($u=4$, $V=2$)
construye un bosque de juguete con el mismo motor y exporta los diagramas a Graphviz; es
el único modo que genera \texttt{.dot}/\texttt{.png}.

\begin{verbatim}
g++ -O2 -std=c++17 -fopenmp -o zdd_cudd_plus_t plus_t/main.cpp \
  -I plus_t -I . -I ./cudd/cudd -I ./cudd -L ./cudd/cudd/.libs \
  -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd \
  -I ./TdZdd/include \
  -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils

./zdd_cudd_plus_t verify u+t \
  resultados_test/wiki_1gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc \
  resultados_test/wiki_1gb_plus_t_tmp.zpack 0

./zdd_cudd_plus_t verify u+t \
  resultados_test/wiki_2gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_2gb_named.voc \
  resultados_test/wiki_2gb_plus_t_tmp.zpack 0

./zdd_cudd_plus_t verify log \
  resultados_test/wiki_2gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_2gb_named.voc \
  resultados_test/wiki_2gb_plus_t_log_tmp.zpack 0
\end{verbatim}

En síntesis, el Backbone~2 convierte el índice posicional
\texttt{pointerList}$[t]\!\to\!\mathrm{ZDD}^{t}$ en un bosque de familias
autoidentificables: el modo \texttt{u+t} usa el singleton $\{u+t\}$ y el modo
\texttt{log} el subconjunto binario $\varphi(t)$, ambos recuperables desde la raíz
sin consultar el vocabulario, con sobrecosto serializado modesto y sin alterar la
semántica documental ni la compartición global del Backbone~1.

\section{Métricas: bits por entero indexado (bpi)}

Sea $V=|\mathcal{V}|$ el número de términos del índice ($t\in\{1,\ldots,V\}$),
$u=2^{40}$ el límite exclusivo de \texttt{master\_id}, y
$N=\texttt{Total\_Ints}=\sum_{t=1}^{V}|\text{posting list}(t)|$ el número de
enteros \texttt{packed64} del \texttt{.docs}. La baseline cruda es $64$~bpi por
entero. Como referencia, índices invertidos compactos sobre \emph{docIDs}
(denominador $=$ total de postings) reportan PEF $4.10$~bpi (Gov2) y
$5.85$~bpi (ClueWeb09~B), OptPFD $4.72$ y $6.42$, Varint-G8IU $10.60$ y
$10.99$~\cite{Ottaviano2014PartitionedEliasFano}; la unidad es el
\emph{docID}, distinta de nuestro par \texttt{(master,rel)}, así que la
comparación es orientativa. PEF no explota redundancia inter-versión; para
colecciones versionadas el baseline adecuado es la línea de índices sobre
colecciones altamente
repetitivas~\cite{Claude2016UniversalIndexesRepetitive,Pibiri2019InvertedIndexCompression}.

Medimos cuatro bpi. \textbf{bpi\_build}$=\texttt{Bytes\_CUDD}\cdot 8/N$ al final
del build: incluye basura del manager y \emph{no} representa la EDD consultable
($\sim\!56$~bpi en \texttt{wiki\_1gb}; $\sim\!80$--$113$~bpi en \texttt{wiki\_2gb}).
\textbf{bpi\_edd}$=\texttt{edd\_nodes}\cdot|\texttt{DdNode}|\cdot 8/N$
($|\texttt{DdNode}|=32$~B) es la \textbf{métrica principal}: tras
\texttt{load} (o \texttt{optimize}) cuenta solo nodos del bosque, excluyendo
\texttt{univ}; es la forma en que el índice responde consultas hoy.
\textbf{bpi\_mem}$=\texttt{Cudd\_ReadMemoryInUse}\cdot 8/N$ mide el proceso
CUDD completo (caché, subtablas, nodos muertos): útil para dimensionar RAM, no
como tamaño de la EDD (el sobrecosto cae de $>\!95\%$ en 100~MB a
$\sim\!22\%$ en 2~GB). \textbf{bpi\_file}$=|\texttt{.zpack}|\cdot 8/N$ mide
persistencia en disco; el \texttt{.zpack} \emph{no} es consultable por mapeo
en memoria (\texttt{loadZddPack} reconstruye el DAG en CUDD), por lo que no se
reporta como tamaño operativo.

El $32$ de la fórmula son \textbf{bytes por nodo}, no bits del entero indexado:
de ellos, $20$~B codifican el DAG (\texttt{index}+dos hijos) y $12$~B son
\texttt{ref}+\texttt{next} del manager; el \texttt{.zpack} persiste exactamente
esos $20$~B. La brecha frente a las cotas siguientes es un artefacto de
representación CUDD, no de falta de compartición estructural.

Sobre el mismo DAG ($n=\texttt{edd\_nodes}$, $c=\texttt{numZddVars}$)
derivamos tres cotas con acceso aleatorio:
$\texttt{bits\_zdd\_std}=2n\lceil\log_2(n{+}1)\rceil+n\lceil\log_2(c{+}1)\rceil$
(línea base \emph{standard ZDD} de DenseZDD/Top~ZDD~\cite{Denzumi2014DenseZDD,Matsuda2021TopZDD},
\emph{no} cota inferior);
$\texttt{bits\_level\_grouped}=2n\lceil\log_2(n{+}1)\rceil+c\lceil\log_2(n{+}1)\rceil$
(etiquetas amortizadas por nivel);
$\texttt{bits\_dag\_counting}=2\log_2((n{+}1)!)+c\lceil\log_2(n{+}1)\rceil$
(piso information-theoretic). Cada una se reporta como
$\texttt{bpi\_}\!\cdot=\texttt{bits\_}\!\cdot/N$. En 2~GB \texttt{log} tras
\texttt{nodes\_desc+sift}: \texttt{bpi\_edd}$=29.36$,
\texttt{zdd\_std}$=7.00$, \texttt{level\_grouped}$=5.51$,
\texttt{dag\_counting}$=5.12$ ($\sim\!5.7\times$ entre CUDD y la cota más
ajustada). Bajo agrupación por nivel, $\varphi(t)$ y $\{u+t\}$ coinciden
(\texttt{level\_grouped}$=6.93$ en baseline 2~GB): la ventaja de
\texttt{log} era solo el ancho de etiqueta por nodo en CUDD. Un ZDD no
comparte subestructuras a alturas distintas~\cite{Matsuda2021TopZDD}; en
índices versionados eso acota el reordenamiento y motiva compresión
\emph{top-DAG}. Cotas sin acceso aleatorio (Hansen et
al.~\cite{Hansen2008CompressingBDD}) miden archivado, no una estructura
consultable.

\textbf{Backbone~1} (\texttt{nzdd\_cudd\_serialize}): $\mathrm{ZDD}^{t}=F_t$,
familias anónimas (el término solo se conoce por \texttt{pointerList}$[t]$),
\texttt{docOffset}$=1$.

\textbf{Backbone~2} (\texttt{zdd\_cudd\_plus\_t}): familias autoidentificables.
CUDD asigna a cada elemento del ZDD una variable entera; \texttt{docOffset}
separa la zona de tag (vars $<$\texttt{docOffset}) de la de masters, con
$\operatorname{var}(m)=m+\texttt{docOffset}$. En modo \texttt{u+t},
$\mathrm{ZDD}^{t}=F_t\cup\bigl\{\{u+t\}\bigr\}$: cada término usa una var
propia ($\operatorname{var}(u+t)=t$), luego \texttt{docOffset}$=V+1$. En modo
\texttt{log}, $\mathrm{ZDD}^{t}=F_t\cup\bigl\{\varphi(t)\bigr\}$, donde
$\varphi(t)=\{\tau_b \mid \mathrm{bit}_b(t)=1\}$ codifica $t$ en binario sobre
$w=\lceil\log_2(V+1)\rceil$ vars auxiliares $\tau_b$ (LSB en $b=0$), con
\texttt{docOffset}$=1+w$; el $+1$ en el logaritmo cubre el caso $V$ potencia
de dos (p.\,ej.\ $V=8$ requiere 4 bits). Aquí $\varphi(t)$ no ocupa el rango
lógico $[u+1,u+V]$: son vars CUDD reservadas; la identidad $u+t$ se recupera
al decodificar.

Frente a Backbone~1, \texttt{bpi\_file} es casi igual en las tres variantes;
\texttt{bpi\_mem} refleja el pool de nodos más la reserva interna de CUDD por
\texttt{numZddVars}. El \texttt{load} usa subtablas iniciales mínimas
(\texttt{NZDD\_INIT\_UNIQUE\_SLOTS}$=8$) para no reservar hash vacío en los
$O(V)$ niveles de tag del modo \texttt{u+t}.

Datos Backbone~2 regenerables desde \texttt{resultados\_test/plus\_t\_bpi\_ladder.csv}
(celda~4 de \texttt{scripts/analisis\_CUDD.ipynb}). Filas Backbone~1: comparativa histórica
(\texttt{edd\_bpi\_ladder.csv}).

\begin{table}[htbp]
\centering
\caption{bpi tras \texttt{load} en manager limpio (julio 2026).
\textbf{bpi\_edd}: tamaño del DAG; \textbf{bpi\_mem}: proceso CUDD completo.}
\label{tab:bpi-ladder}
\small
\begin{tabular}{l l r r r r}
\hline
Dataset & Variante & $N$ & bpi\_file & bpi\_edd & bpi\_mem \\
\hline
100 MB & Backbone~1 (sin tags)   & 5.4M   & 0.15  & ---    & 15.57 \\
100 MB & Backbone~2 \texttt{log}   & 5.4M   & 0.58  & 0.74   & 16.58 \\
100 MB & Backbone~2 \texttt{u+t}   & 5.4M   & 0.43  & 0.51   & 18.01 \\
1 GB   & Backbone~1 (sin tags)     & 61.4M  & 13.11 & ---    & 28.88 \\
1 GB   & Backbone~2 \texttt{log}   & 61.4M  & 13.69 & 21.65  & 30.25 \\
1 GB   & Backbone~2 \texttt{u+t}   & 61.4M  & 13.49 & 21.34  & 36.49 \\
2 GB   & Backbone~1 (sin tags)     & 122.2M & 21.81 & ---    & 44.30 \\
2 GB   & Backbone~2 \texttt{log}   & 122.2M & 22.30 & 35.47  & 45.42 \\
2 GB   & Backbone~2 \texttt{u+t}   & 122.2M & 22.14 & 35.21  & 47.06 \\
\hline
\end{tabular}
\end{table}

\textbf{bpi\_file} permanece en $13$--$22$~bpi (1--2~GB) con o sin tag: el
\texttt{.zpack} serializa solo el DAG alcanzable y el tag añade pocos nodos.
Para el \emph{tamaño de la EDD} debe usarse \textbf{bpi\_edd}; \textbf{bpi\_mem}
sirve para estimar RAM del proceso. Backbone~2 \texttt{log} y \texttt{u+t}
tienen \textbf{bpi\_edd} del mismo orden en 1--2~GB ($\sim\!21$--$35$~bpi),
mientras \texttt{bpi\_mem} penaliza más a \texttt{u+t} por
\texttt{numZddVars}\,$\approx V$ (p.\,ej.\ 36.5 vs 30.3~bpi en 1~GB).
El modo \texttt{log} mantiene la huella de proceso cercana a Backbone~1;
\texttt{u+t} es la codificación canónica directa con penalización RAM acotada
tras la optimización de subtablas.

\subsection{Optimización por reordenamiento CUDD}

El build de \texttt{plus\_t} inserta términos en un único \texttt{DdManager}
compartido \emph{sin} reordenar niveles (\texttt{Cudd\_AutodynDisableZdd}).
La compactación del DAG es un paso \textbf{post-build}: el modo
\texttt{optimize} carga un \texttt{.zpack}, aplica una heurística de
permutación de variables CUDD sobre el manager global (no sobre
\texttt{pointerList}$[t]$ aislado) y mide el tamaño del EDD compartido.
Semántica: solo se reordenan niveles; la función booleana de cada
$\mathrm{ZDD}^{t}$ se preserva y se verifica con
\texttt{Cudd\_zddCountDouble} sobre una muestra de raíces. Un
\texttt{TIMEOUT} no modifica el \texttt{.zpack} de entrada.

La métrica de evaluación es \textbf{bpi\_edd} (no \textbf{bpi\_mem}).
Heurísticas admitidas en ZDD: estáticas (\texttt{nodes\_desc/asc},
\texttt{df\_desc/asc} vía \texttt{Cudd\_zddShuffleHeap}); dinámicas
(\texttt{sift}, \texttt{sift\_conv}, \texttt{symm\_sift\*},
\texttt{random\*}); compuestas (p.\,ej.\ \texttt{nodes\_desc+sift}).
Se rechazan \texttt{linear\*} (alteran $\operatorname{var}\!\to\!\texttt{master}$)
y heurísticas no implementadas en \texttt{Cudd\_zddReduceHeap} para ZDD.

Barrido homogéneo (ago.~2026): 14 heurísticas, $\texttt{max\_sift\_vars}=200$,
packs Backbone~2. Umbral \textbf{timeout\_s} por heurística (fork+SIGKILL):

\begin{center}
\small
\begin{tabular}{l l r l}
\hline
Escala & Pack & \texttt{timeout\_s} & Equiv. \\
\hline
100~MB & \texttt{wiki\_100mb\_plus\_t} & $600$ & $10$~min \\
1~GB   & \texttt{wiki\_1gb\_plus\_t\_bin} (\texttt{log}) & $7200$ & $2$~h \\
2~GB   & \texttt{wiki\_2gb\_plus\_t\_bin} (\texttt{log}) & $10800$ & $3$~h \\
\hline
\end{tabular}
\end{center}

\begin{table}[htbp]
\centering
\caption{Reordenamiento post-build en \texttt{wiki\_100mb}
($N=5\,424\,820$; \texttt{timeout\_s}$=600$). Ganadora: \texttt{sift\_conv}.}
\label{tab:optimize-100mb}
\small
\begin{tabular}{l r r r l}
\hline
Heurística & bpi\_edd & $\Delta\%$ & $t$~(s) & Status \\
\hline
baseline & 0.509 & $0.00$ & $\approx 0$ & ok \\
\texttt{df\_desc} & 0.513 & $+0.82$ & 0.03 & ok \\
\texttt{nodes\_asc} & 0.508 & $-0.21$ & 0.01 & ok \\
\texttt{nodes\_desc} & 0.506 & $-0.57$ & 0.00 & ok \\
\texttt{df\_asc} & 0.503 & $-1.15$ & 0.02 & ok \\
\texttt{random} & 0.500 & $-1.85$ & 10.0 & ok \\
\texttt{df\_desc+sift\_conv} & 0.500 & $-1.78$ & 1.82 & ok \\
\texttt{sift} / \texttt{symm\_sift} & 0.499 & $-1.92$ & 0.35--0.39 & ok \\
\texttt{random\_pivot} & 0.499 & $-1.92$ & 178 & ok \\
\texttt{nodes\_desc+sift} & 0.499 & $-1.96$ & 0.38 & ok \\
\texttt{symm\_sift\_conv} & 0.499 & $-1.98$ & 1.63 & ok \\
\texttt{nodes\_desc+sift\_conv} & 0.499 & $-1.99$ & 1.62 & ok \\
\textbf{\texttt{sift\_conv}} & \textbf{0.499} & $\mathbf{-2.03}$ & 1.64 & ok \\
\hline
\end{tabular}
\end{table}

\begin{table}[htbp]
\centering
\caption{Reordenamiento post-build en \texttt{wiki\_1gb} modo \texttt{log}
($N=61\,447\,986$; \texttt{timeout\_s}$=7200$).
Ganadora: \texttt{df\_desc+sift\_conv}.}
\label{tab:optimize-1gb}
\small
\begin{tabular}{l r r r l}
\hline
Heurística & bpi\_edd & $\Delta\%$ & $t$~(s) & Status \\
\hline
baseline & 21.65 & $0.00$ & $\approx 0$ & ok \\
\texttt{df\_asc} & 21.22 & $-1.98$ & 181 & ok \\
\texttt{df\_desc} & 20.69 & $-4.44$ & 158 & ok \\
\texttt{nodes\_asc} & 20.39 & $-5.81$ & 214 & ok \\
\texttt{nodes\_desc} & 20.25 & $-6.48$ & 148 & ok \\
\texttt{random\_pivot} & 19.90 & $-8.08$ & 3790 & ok \\
\texttt{random} & 19.26 & $-11.02$ & 642 & ok \\
\texttt{sift} / \texttt{symm\_sift} & 17.67 & $-18.39$ & 281 & ok \\
\texttt{nodes\_desc+sift} & 17.44 & $-19.47$ & 293 & ok \\
\texttt{sift\_conv} & 16.49 & $-23.81$ & 3230 & ok \\
\texttt{symm\_sift\_conv} & 16.49 & $-23.82$ & 3750 & ok \\
\textbf{\texttt{df\_desc+sift\_conv}} & \textbf{16.48} & $\mathbf{-23.88}$ & 3950 & ok \\
\texttt{nodes\_desc+sift\_conv} & --- & --- & $\ge 7200$ & TIMEOUT \\
\hline
\end{tabular}
\end{table}

\begin{table}[htbp]
\centering
\caption{Reordenamiento post-build en \texttt{wiki\_2gb} modo \texttt{log}
($N=122\,197\,799$; \texttt{timeout\_s}$=10800$). Barrido completo
(14 heurísticas; 9 ok + 5 TIMEOUT). Mejor:
\texttt{nodes\_desc+sift}.}
\label{tab:optimize-2gb}
\small
\begin{tabular}{l r r r l}
\hline
Heurística & bpi\_edd & $\Delta\%$ & $t$~(s) & Status \\
\hline
baseline & 35.47 & $0.00$ & $\approx 0$ & ok \\
\texttt{df\_asc} & 34.83 & $-1.80$ & 1420 & ok \\
\texttt{df\_desc} & 34.16 & $-3.69$ & 906 & ok \\
\texttt{nodes\_asc} & 33.27 & $-6.21$ & 1490 & ok \\
\texttt{nodes\_desc} & 33.17 & $-6.49$ & 984 & ok \\
\texttt{random} & 31.55 & $-11.06$ & 5880 & ok \\
\texttt{sift} / \texttt{symm\_sift} & 30.02 & $-15.37$ & 2380 / 1480 & ok \\
\textbf{\texttt{nodes\_desc+sift}} & \textbf{29.36} & $\mathbf{-17.22}$ & 2010 & ok \\
\texttt{sift\_conv} / \texttt{symm\_sift\_conv} & --- & --- & $\ge 10800$ & TIMEOUT \\
\texttt{nodes\_desc+sift\_conv} / \texttt{df\_desc+sift\_conv} & --- & --- & $\ge 10800$ & TIMEOUT \\
\texttt{random\_pivot} & --- & --- & $\ge 10800$ & TIMEOUT \\
\hline
\end{tabular}
\end{table}

\textbf{Lectura.} En 100~MB el techo es $\sim\!2\%$ (\texttt{sift\_conv},
segundos). En 1~GB las dinámicas convergentes y el compuesto
\texttt{df\_desc+sift\_conv} alcanzan $\sim\!24\%$ de reducción de
\textbf{bpi\_edd} (21.65$\to$16.48), a costa de $50$--$66$~min;
\texttt{nodes\_desc+sift} ofrece un buen compromiso ($-19.5\%$ en $\sim\!5$~min).
En 2~GB, con umbral de $3$~h, el mejor ok es \texttt{nodes\_desc+sift}
($-17.2\%$, $\sim\!33$~min); \texttt{sift}/\texttt{symm\_sift} quedan en
$-15.4\%$ y \texttt{random} en $-11.1\%$ ($\sim\!98$~min). Todas las
variantes convergentes (\texttt{*\_conv}) y \texttt{random\_pivot}
agotaron el timeout: a esta escala el sifting convergente no termina
en $3$~h. Logs: \texttt{optimize\_sweep\_*.csv}; notebook
\texttt{scripts/analisis\_CUDD.ipynb} (celdas~5--7).
```
