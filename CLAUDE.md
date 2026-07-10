# Simulador de Pesca v2 — Documentação (PWA + ESP32-C3 + VESC 75100)

> **v2** do simulador (o **v1** fica como backup intocado em `...\Simulador pesca`, repo `simpesca`, Pages `/simpesca/`).
> O jogador recolhe a manivela contra o motor enquanto assiste ao vídeo da briga. O app manda uma **força COM SINAL**
> pro ESP32; **+ = PUXA (o peixe corre, o motor ENROLA linha)**, **− = FREIA (resistência)**, **0 = solto**. O **peso do
> peixe é aplicado NO APP** (escala a curva); o ESP só executa. A **linha recolhida vem do tacômetro do VESC** (sem AS5600).

- **Pasta:** `C:\Users\guilh\OneDrive\Documentos\Arduino\Simulador de pesca v2`
- **Repo GitHub:** `guilhermeogera-beep/Simularpesca2` · **Pages:** `https://guilhermeogera-beep.github.io/Simularpesca2/`
- **Deploy = upload MANUAL** dos arquivos do shell no repo (NÃO é repo git local). Só `curvas.json`/`config.json` sobem
  sozinhos (auto-save via API do GitHub, com token). **Totem sem token só LÊ.** Ver "Deploy / offline".

### Hardware
- **ESP32-C3** ↔ **VESC 75100** por **UART** (`Serial1` 115200 8N1): **GPIO5 TX → VESC RX**, **GPIO6 RX ← VESC TX**, GND comum.
- Lib **VescUart (SolidGeek)**. O VESC faz **puxada (por rotação) E freio (por corrente)**; a **linha vem do tacômetro do VESC**.
- **Web Bluetooth** (motor). **iPhone não tem Web Bluetooth** → usar PC com Chrome/Edge ou Android.

### Arquivos
```
esp32_vesc/esp32_vesc.ino   — firmware (VescUart; puxada=setRPM, freio=setBrakeCurrent; tacômetro; watchdog)
site/  ← FONTE do PWA (subir em /Simularpesca2/ no GitHub)
  index.html          — landing: instalar PWA + IMPORTAR os 6 vídeos locais (1 botão por peixe → IndexedDB)
  player.html         — app principal: BLE, vídeos (lazy-load), curva→motor(RPM/freio), linha ao vivo, peso, ranking
  dashboard.html      — editor de curva COM SINAL (+40/−20) + gerador + calibração (mpv/RPM/limite) + controle manual + auto-save
  ranking.html        — ranking (metros recolhidos + peso do peixe), por sim
  service-worker.js   — cache do SHELL (rede-primeiro). NÃO cacheia vídeos nem .mp4. v4.
  curvas.json         — curvas de TODOS os peixes (GitHub puxa na abertura)
  config.json         — calibração + ajustes do gerador + sincronia   ⚠ ver "config.json desatualizado"
  manifest.json       — manifesto PWA ("Simulador de Pesca v2", id "/Simularpesca2/")
_LOCAL_NAO_SUBIR/     — NÃO subir (token movido pra cá; qualquer coisa local sensível)
```
> **⚠ Vídeos:** existem `site/video_pirarara.mp4` e `site/video_tambaqui.mp4` (~80 MB) — **NÃO subir pro repo** (bloat +
> os vídeos ficam no IndexedDB, não no GitHub). São só fallback/fonte local.

---

## 1) Protocolo BLE  (device `SimuladorPesca`, UUIDs `a1b2c3d4-XXXX-4a5b-8c6d-1234567890ab`)

| Canal | Props | Formato | Direção | Quando |
|---|---|---|---|---|
| **0001** | — | Service | — | filtro do `requestDevice` |
| **0002 MOTOR** | WRITE + WRITE_NR | **1 byte int8 COM SINAL** (−100..+100; na prática −20..+40) | app → ESP, ~10x/s | `loopSim` (player, 100ms), `playSim` (dash, ~10Hz) e slider manual + **keepalive** (400ms). ESP: `f>0.5`→`setRPM(MIN_RPM+(f/40)·(MAX_RPM−MIN_RPM))·RPM_SINAL`; `f<−0.5`→`setBrakeCurrent(min(1,|f|/FREIO_ESC)·MAX_FREIO_A)`; senão `setCurrent(0)`. Todo write alimenta o watchdog. |
| **0003 LINHA** | READ+NOTIFY+WRITE+WRITE_NR | notify: **int32 LE** = `tacômetro − tacoOffset`; write: qualquer payload **zera** | ESP → app / app → ESP | notify ~6x/s só quando muda; app escreve `Uint8Array([0])` no re-spool |
| **0004 CONFIG** | WRITE + WRITE_NR | **4 bytes = 2× uint16 LE** `[RPM mín, RPM máx]` | app → ESP | `enviarConfigRPM()`: ao **conectar** (player+dash), no **salvarCal** (dash) e em **aplicarConfigPlayer**. Firmware força `MAX≥MIN`. **NÃO persiste no ESP** (RAM) → reenvia sempre que conecta. |

Ao desconectar: ESP `pararTudo()` + re-anuncia; no app, `gattserverdisconnected` **pausa a sim** (player) / loga (dash).

## 2) Firmware `esp32_vesc.ino`

- **PUXADA por ROTAÇÃO** (`aplicarForca`, o coração do v2): `f>0.5` → `setRPM`. O VESC **segura a rotação com torque
  alto = ENROLA SEMPRE, IMPARÁVEL** — o peixe leva linha mesmo o pescador forçando (controle por corrente era vencível
  pela mão; foi trocado). **Pra ser imparável, o *Motor Current Max* no VESC Tool tem que estar ALTO** (é o torque).
- **FREIO** = `setBrakeCurrent(min(1, |f|/FREIO_ESC)·MAX_FREIO_A)` (resistência/inércia). `0` = coast. **`FREIO_ESC=20`
  → a base do gráfico (−20) = FREIO MÁXIMO** (simétrico com a puxada, que usa `/FORCA_MAX=40`). `ampMotor` no dash usa a mesma escala.
- **Constantes:** `MAX_FREIO_A=25` · `FREIO_ESC=20` · `FORCA_MAX=40` (valor da curva que = RPM máx) · `MIN_RPM=500`/`MAX_RPM=8000`
  (ERPM, **floats mutáveis via canal 0004**, calibráveis sem regravar) · `RPM_SINAL=1` (**−1 se enrolar pro lado errado**)
  · `RAMPA_SOBE=250`/`RAMPA_DESCE=300` força/s (com a escala 0..40 a rampa é quase um degrau; quem suaviza é o VESC)
  · `MOTOR_TIMEOUT_MS=1200`. (`MAX_PUXADA_A=30` é **legado**, não usado — puxada virou RPM.)
- **Loop (50Hz):** rampa `forcaAtual→forcaAlvo` → `aplicarForca` → **watchdog** (`|forca|>0.5` e >1,2s sem write no 0002 →
  zera tudo, cobre queda de BLE) → `getVescValues()` a cada 30ms (backoff 300ms se falhar) → notify da linha se mudou.
- **Callbacks:** MOTOR (int8→`forcaAlvo`), LINHA (write→`tacoOffset=tacoBruto`, zera+notifica), CONFIG (≥4 bytes→MIN/MAX_RPM).

## 3) Modelo de força / escala do gráfico

- **Curva de pontos `{t,pot}` por peixe, COM SINAL.** Escala **ASSIMÉTRICA: `ESC_POS=+40` (topo, PUXA) · `ESC_NEG=−20`
  (base, FREIA)**, `ESC_RANGE=60`; o **zero cai a ~⅔** pra baixo (puxada tem mais alcance que o freio). Editor, gerador,
  barra de antecipação e clamps usam essa escala. **`interpolarPontos`** faz interpolação linear.
- **`sanearCurva(pts)`** (nos dois): ao importar/carregar do GitHub, **clampa −20..+40** e **reescala ×0,4 se detectar
  escala antiga (max pot > 40)** — impede curva do v1 (0-100) de cravar o motor no RPM máx.
- **`comOffset` = passa-direto no v2** (RPM/corrente não têm zona morta; o piso da puxada é o `MIN_RPM` no ESP). O offset
  0..100 do v1 distorcia o mapa 0..40 → **neutralizado**. O campo "Offset motor" existe mas é **vestigial** (padrão 0).

## 4) PESO do peixe (escala a curva — feito NO APP)

- Slider **por espécie**: no **player** (card, `peso_<id>`) e no **dashboard** (`.peso-row`, `pesoSim<i>`) — **MESMA chave
  `simpesca2_peso_<id>`** (sincronizam no mesmo dispositivo).
- **`escalaPeso = min(1, (peso/pesoMax)^(2/3))`** — lei física (arrasto ∝ área ∝ peso^⅔), IDÊNTICA nos dois. Peixe no
  máximo = 100% da curva; filhote puxa menos (mas **não** proporcional, por causa do piso do `MIN_RPM`).
- **`PESO_MAX_DEF`** (kg, FishBase/IGFA): pirarara 50 · tambaqui 40 · tucunare 13 · dourado 30 · trairao 15 · jau 80
  (`PESO_MIN=1`; fallback 20). **Duplicado à mão nos dois arquivos** — ao mudar, editar `player.html` E `dashboard.html`.
- **Sincronia entre abas:** ambos ouvem o evento **`storage`** → mudar o peso numa aba (ex.: player) reflete na outra
  (dash: slider + prévia; player: slider + rótulo) **na hora**. Dash→player já valia (o player lê o localStorage ao vivo);
  o listener resolveu o inverso (o dash lia o DOM velho).
- **NÃO entra no config.json** (é escolha de sessão no totem; sincronizar do PC sobrescreveria). Sem backup — por design.

## 5) Estimativa de linha  (`🐟 ≈ X m`)  — derivada do RPM + mpv (SEM `velmax`)

O v2 controla a puxada por **rotação**, então os metros vêm de matemática, não de um "m/s a 100%" chutado:
```
pot(t)·escalaPeso  →  rpmAlvo(pot) = MIN_RPM + (pot/40)·(MAX_RPM−MIN_RPM)   [espelha o firmware]
                   →  rpmParaMs(erpm) = |erpm|/10/4096 · mpv                [m/s]
metros = ∫ rpmParaMs dt   sobre os trechos de PUXADA (pot·esc > 0.5, = motor real)
```
- **`/10`** = o VESC conta **6 passos por rev ELÉTRICA** (contagens/s = ERPM/10). **`/4096` + `mpv`** = a MESMA calibração
  que já converte o tacômetro em metros → **auto-consistente**: a precisão vem **só do `mpv`**.
- **Calibrar `mpv` no hardware v2** pelo assistente do dashboard (**zerar → recolher X m → 🧮 Calcular**). O padrão **0.0854**
  é do AS5600 do v1 e dá números pequenos **até recalibrar** (a linha ao vivo usa o mesmo `mpv`, então batem entre si).
- O **peso entra DENTRO do mapa** (não multiplica o total). Usada no `🐟 ≈ X m` e na **validação de linha do `pedirNome`**
  (se a estimativa não couber no carretel, abre o `linhaOverlay` pedindo pra enrolar). A **trava dura** (`checarLimiteSeguranca`)
  é a rede de segurança real.

## 6) Player (`player.html`) — fluxo de uma jogada

card ▶ → **`pedirNome`** (pré-carrega o vídeo + valida a linha) → `nomeOverlay` → contagem 5s (bips WebAudio) + fullscreen
→ **`iniciarSim`** (`novoJogoContadores` zera os ganhos DO JOGO, não o carretel; se `sync>0` espera o `syncTimer`) →
**`loopSim`** (100ms): `aplicarMotor(interpolarPontos(pontos, currentTime−sync)·escalaPeso)` — **zera após o último ponto**
(curva manual terminada em >0 não puxa até o fim do vídeo) → vídeo `ended` → **`encerrarSim`**: sai do FS, libera o vídeo,
salva no ranking se o nome for válido, mostra posição/medalha.
- **`aplicarMotor`:** clampa ±100 · **trava bloqueia só a PUXADA** (`travado()&&pct>0→0`; o freio passa) · `comOffset`
  (passa-direto) · escreve int8. **`hudEstado`** usa o mesmo limiar do firmware sobre o valor arredondado que vai pro fio
  (`fio=round(pct); puxando=fio>0.5; freando=fio<−0.5`): 🐟 PEIXE PUXANDO / 🛑 RESISTINDO / 🎣 RECOLHA.
- **Linha:** `metros = (contagem/4096)·mpv` (×−1 se `cal_inv`). **Conta só ganhos** (deltas +) enquanto `contando`. `metrosGanhos`
  zera por jogo; a **linha do carretel** (`linhaCarretel`, posição absoluta) **ACUMULA entre jogadores** e só zera no re-spool.
- **Trava de linha:** `|linhaCarretel| ≥ |limite|` → pausa a sim (só a puxada; o freio não gasta linha).
- **HUDs / 👁 EXIBIR:** `hudLinha` (🎣 metros), `hudEstado`, `hudTimeline` (waveform +40/−20, 3 cores, faixa laranja de
  fisgada em 0..`fisfisdur`), `hudCarretel` (off), `hudSync` (nudge ±0,2s ao vivo). Toggles em `simpesca2_hud_*`.
- **Vídeos:** lazy-load 1 por vez (IndexedDB `simpesca2-videos`/`videos`/`video_<id>`); fallback de rede `video_<id>.mp4`.
- **v2 SEM fisgada física:** `pararFisgadaFisica()` é stub; a fisgada é **desenhada na curva** (a faixa é só visual).

## 7) Dashboard (`dashboard.html`)

- **Cards:** Controle de Carretel (ganhos/perdidos/📍linha+trava, ⟲ zerar, ⇄ inverter, limite −50) · Curva de Potência
  (6 blocos recolhíveis) · **Controle Manual** · Encoder (mpv + assistente; **token GitHub**; RPM mín/máx) · Exportar/Importar · Log.
- **Editor de pontos:** timeline `DURACAO=90s`, escala +40/−20. Clique=novo · arraste=move · clique=popup (t/força −20..40/remover) ·
  **Ctrl+clique**=multi-seleção · **Ctrl+C/V**, **Delete**, **Esc**. `salvarPontos`→`agendarSync('curvas')`.
- **Gerador:** **🎲+** gera a PUXADA (preserva o freio) · **🎲−** (re)preenche o FREIO nas janelas de 0 · **🧹+/🧹−** limpam.
  Campos por peixe: assinatura (`intens`, neutro em 25) · 🎣 saque · 🏃 corrida (0..40 diretos) · fadiga · puxada(s) ·
  espaço · **🛑 freio máx (kgf)** · **🟥 freio quadrado** (parede vs característico) · 🎣 início da briga (`fisfisdur`).
  Pipeline `buildFight` → `finalizarFight` (clamp −20..+40). Espécies em `FIGHTS{}`; assinaturas distintas por peixe.
- **Controle Manual:** slider **−40..+40** com zona morta `SL_DZ=8`. **Keepalive** reenvia a força ~3x/s (senão o
  watchdog do ESP zera em 1,2s com o slider parado — atrapalhava calibrar). ⏹ PARAR zera. Gauge: **PUXA → "≈ N RPM"**
  (`rpmAlvo`) · **FREIA → "≈ N A"** (`ampMotor`, `MAX_FREIO_A=25`). `pauseSim`/`stopSim` zeram o motor.
- **Calibração:** `mpv` (padrão 0.0854, recalibrar no v2) · `offset` (vestigial, 0) · **RPM mín/máx** · `limite` (−50) · `inv`.
  Vão pro `config.json`.  **`velmax` foi REMOVIDO** (linha derivada de RPM+mpv).
- **Auto-save GitHub:** `agendarSync('curvas'|'config')` (debounce 2,5s, **gated por token**) → `fazerSync` → `ghPut`
  (GET sha → PUT `api.github.com/repos/Simularpesca2/contents/<path>`, retry 1× em 409/422). Só o **PC de edição tem token**;
  o **totem não tem → só lê**. Offline marca `simpesca2_sync_pend` e sobe no `online`.
- **`FREIO_MAX_DEF`** (kgf sustentado ≈0,84·peso^⅔): pirarara 11 · tambaqui 10 · tucunare 5 · dourado 8 · trairao 5 · jau 16.

## 8) Espécies · Prioridade da curva · Ranking · Storage

- **`ESPECIES`** (mesma ordem em player/dash/ranking/index): `pirarara`→Pirarara · `tambaqui`→**Tambacu** · `tucunare`→Tucunaré
  · `dourado`→Dourado · `trairao`→Trairão · `jau`→Jaú. (Id `tambaqui` fica; só o texto exibido é "Tambacu".)
- **Prioridade da curva** (`carregarPontos`/`carregarSim`, IDÊNTICA): **tem token (EDITOR) → localStorage vence**; **sem token
  (TOTEM) → GitHub (`curvas.json`) vence**; senão local; senão legado A/B/C (por índice); senão gera (`gerarCurvaEspecie`)/
  `GATILHOS_PADRAO`×0,4. (≠ v1, que era sempre GitHub 1º — resolveu "editei no dash e o player não espelhou".)
- **Ranking** `simpesca2_ranking` = `[{nome, metros, sim, peso, data}]`; exibe **⚖️ kg** no `ranking.html` e no overlay do player.
- **localStorage (prefixo `simpesca2_`):** `pontos_<id>` · `gen_<id>_<campo>` (corrida/saque/fadiga/puxada/espaco/intens/
  freiomax/fisfisdur; `freioquad` é local, fora do config) · `cal_mpv|offset|rpmmin|rpmmax|limite|inv` · `sync_<id>` ·
  **`peso_<id>`** · `ranking` · `hud_*` · `peixe_<id>` · `github_token` (decide a prioridade da curva) · `sync_pend` ·
  flags de UI/migração. **IndexedDB:** `simpesca2-videos`.
- **config.json** = **`versao:2`** (esquema novo). Aplica: `aplicarConfig` (dash: cal+gen+sync) · `aplicarConfigPlayer`
  (player: cal + sync + `gen.fisfisdur`). **`gen` e `offset` só são aplicados se `versao>=2`** — um config v1 antigo
  (com `teto`/`fispico`/`offset:15`/`intens` na escala velha) é ignorado nesses campos (não regride). Auto-save e export gravam `versao:2`.

## 9) Deploy / offline / cache

- **Upload manual** dos arquivos do shell no repo `Simularpesca2`. Só `curvas.json`/`config.json` sobem via API (auto-save).
- **SW `simpesca2-shell-v4`** (rede-primeiro): pré-cacheia o shell + `curvas.json`/`config.json` + ícones; `activate` limpa
  caches antigos. **NÃO cacheia `.mp4`** (vídeos ficam no IndexedDB) e **só cai no `index.html` em navegação** (não devolve
  HTML pra fetch de JSON/fonte). Ao mexer no shell, **bumpar a versão**. As páginas fazem `fetch(...,{cache:'no-store'})`.
- **Vídeos:** importados 1× no `index.html` (label→input por peixe) → IndexedDB. Trocar = re-importar (não mexe em GitHub/cache).

---

## ⚠️ Pendências / cuidados

1. **Hardware VESC:** rodar detecção **FOC** + **subir o *Motor Current Max*** (é o torque que torna a puxada imparável);
   habilitar app UART. Depois **calibrar** `MIN_RPM`/`MAX_RPM` (pelo slider manual + metros) e **`mpv`** (assistente) no v2.
   `RPM_SINAL=−1` se enrolar pro lado errado.
2. **`site/config.json` foi REGERADO no esquema `versao:2`** (cal v2 limpo — sem `velmax`, `offset:0`, `rpmmin/rpmmax`;
   gen com `corrida/saque/freiomax` nos defaults por espécie). Pode subir. Se o seu dashboard já tem tuning próprio,
   **exporte de lá (⬇ Exportar config.json)** em vez de subir esse — ele traz só os defaults.
3. **NÃO subir pro GitHub:** os `.mp4` de `site/` (~80 MB) nem qualquer token. O token já foi movido pra `_LOCAL_NAO_SUBIR/`;
   se algum PAT já foi ao repo alguma vez, **revogar e gerar novo**. O token vive só no `localStorage` do PC de edição.
4. **Conhecidas, não corrigidas** (auditoria 10/07/2026): `aplicarConfig` sobrescreve edição local offline sem a regra
   "editor prefere local" (mitigado em parte pelo guard `versao>=2`); UUID 0004 reusado do v1 (um app v1 cacheado conectando
   no ESP v2 corromperia MIN/MAX_RPM → se for problema, trocar `BLE_NOME`/`SERVICE_UUID`). **Já corrigidos:** `sync_pend`
   agora é separado (`simpesca2_sync_pend_player` no player) e o freio ficou consistente (`FREIO_ESC=20`).
