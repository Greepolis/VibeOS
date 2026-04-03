# Architectural Review For Developer (2026-04-03)

## 1. Valutazione generale dell'architettura
- Coerenza con le specifiche: **Parziale (tendente ad Alta sulla foundation)**
- Stabilità architetturale: buona sulle primitive core; ancora fragile su bring-up runtime reale
- Scalabilità prevista: buona da design, non ancora validata su SMP/hardware reali
- Manutenibilità: alta su struttura modulo/documentazione; media sulle integrazioni ancora stub

Sintesi: architettura coerente con il modello ibrido previsto, ma validata principalmente in host simulation.

## 2. Conformità alle specifiche

| Componente / Modulo | Specifica prevista | Implementazione attuale | Conformità | Note |
| --- | --- | --- | --- | --- |
| Kernel model ibrido/modulare | Core minimo in kernel, servizi in user-space | Core + servizi user-space presenti | Conforme | Allineato SAS |
| Boot system | Bootloader UEFI reale + handoff | Boot stub e builder boot info | Parzialmente conforme | manca boot reale |
| Process management | lifecycle, isolamenti, object model completo | lifecycle + PID handle isolation + duplication policy | Parzialmente conforme | manca integrazione completa thread/process object |
| Scheduler | preemptive per-CPU e multicore | runqueue per-CPU + preemption base | Parzialmente conforme | SMP reale mancante |
| Memory management | PMM + paging/COW/fault path | PMM bump + VM map/protect/unmap | Parzialmente conforme | backend paging mancante |
| IPC | channel/event/waitset + transfer sicuro | implementato con ownership/lifecycle/timed waits | Conforme | forte per fase |
| Syscall interface | ABI handle-based versionata | dispatcher + policy rights + PID enforcement | Parzialmente conforme | ABI/versioning da formalizzare |
| Security model | capability + MAC-like | capability/token/policy base | Parzialmente conforme | MAC/propagation incomplete |
| Driver architecture | user-space-first + supervision | framework/devmgr/host stub | Parzialmente conforme | runtime supervision da maturare |
| Filesystem | VFS + native FS integrity | VFS runtime base | Parzialmente conforme | native FS non implementato |
| Networking | stack TCP/IP progressivo | socket/runtime base | Parzialmente conforme | stack completo mancante |
| Compatibility layers | Linux/Windows/macOS demo | solo strutture e piano | Non conforme | nessuna demo eseguibile |
| Observability | logging/tracing/debug robusti | contatori + diagnostica test base | Parzialmente conforme | tracing framework assente |
| Testing strategy | CI ripetibile + smoke reali | test host-side verdi | Parzialmente conforme | CI/smoke emulatore incompleti |

## 3. Deviazioni architetturali rilevate

| Deviazione | Motivazione possibile | Impatto tecnico | Gravità |
| --- | --- | --- | --- |
| Validazione soprattutto host-side | velocità di iterazione iniziale | mismatch possibile con runtime reale | Alta |
| Limite `VIBEOS_MAX_PROCESSES=32` | workaround stack/test | scala limitata e artificiale | Media |
| Evoluzione argomenti syscall senza ABI congelata | sviluppo incrementale | rischio regressioni interfaccia | Media |
| Compat layer non implementati | priorità foundation kernel | milestone M8/M9 non supportate | Alta |
| Bootloader reale assente | focus su primitive core | blocco validazione M2/M3 | Alta |

## 4. Criticità PM valutate come architettura/tecnica

| Problema | Tipo | Impatto sull'architettura | Azione consigliata |
| --- | --- | --- | --- |
| CI/CMake non stabile end-to-end | Tecnico/organizzativo | valida poco la qualità architetturale | standardizzare toolchain e pipeline unica |
| Gap host simulation vs bare-metal | Tecnico/architetturale | rischio decisioni non trasferibili | introdurre smoke QEMU/UEFI obbligatori |
| Milestone avanzate non sostenute da artefatti runtime | Strategico | disallineamento roadmap/architettura | ribaseline milestone con DoD tecniche |
| KPI temporali mancanti | Organizzativo | forecasting debole | introdurre metriche delivery/quality |

## 5. Qualità tecnica del sistema
- Modularità: buona
- Separazione responsabilità: buona in design, parziale in runtime
- Accoppiamento: medio-basso
- Riusabilità: buona sulle primitive
- Testabilità: buona host-side, debole bare-metal
- Gestione errori: basica ma coerente
- Sicurezza: direzione corretta, enforcement ancora incompleto
- Performance: strutturalmente buona, non misurata su target reale

Debiti tecnici principali: ABI syscall non congelata, boot reale assente, paging backend mancante, compat layer non operativi.

## 6. Rischi tecnici e architetturali

| Rischio | Probabilità | Impatto | Priorità |
| --- | --- | --- | --- |
| Divergenza host test vs runtime reale | Alta | Alta | Alta |
| Rework su VM/fault model | Media-Alta | Alta | Alta |
| Scope creep compatibilità multi-OS | Alta | Alta | Alta |
| Sicurezza incompleta (MAC/token propagation) | Media | Alta | Alta |
| ABI syscall instabile | Media | Media-Alta | Media-Alta |
| Crescita complessità senza CI gate | Alta | Media | Media-Alta |

## 7. Modifiche architetturali raccomandate

| Modifica | Motivazione | Impatto | Priorità |
| --- | --- | --- | --- |
| Congelare `syscall ABI v0` | prevenire regressioni interfaccia | stabilità alta | Alta |
| Object manager unificato process/thread/handle | chiudere integrazione lifecycle | coerenza sicurezza/lifecycle | Alta |
| Waitset su waitqueue/time source reale | ridurre simulazione | correttezza scheduling/latency | Alta |
| Policy ADR obbligatoria per eccezioni kernel-space | evitare erosione confini | governance architetturale | Media |
| Compatibility adapter con demo minima | progressi misurabili M8/M9 | riduce rischio roadmap | Media |

## 8. Debito tecnico identificato

| Debito tecnico | Impatto futuro | Complessità di risoluzione | Priorità |
| --- | --- | --- | --- |
| Bootloader UEFI reale mancante | blocca milestones base | Alta | Alta |
| VM avanzata assente (paging/COW/fault) | limita sicurezza/performance | Alta | Alta |
| CI smoke emulatore assente | regressioni tardive | Media | Alta |
| MAC e token propagation incompleti | security-first non raggiunto | Media-Alta | Alta |
| Compatibilità runtime non implementata | rischio strategico su obiettivi | Alta | Media-Alta |
| Limite processi workaround | riduce validità stress test | Bassa-Media | Media |

## 9. Raccomandazioni operative per sviluppo

### Azioni urgenti
1. Definire e documentare `syscall ABI v0` (arg semantics per syscall group).
2. Implementare smoke test QEMU/UEFI in pipeline CI.
3. Chiudere M2/M3: boot reale, timer/interrupt reali, paging base reale.

### Miglioramenti consigliati
1. Chiudere `handle-based process/thread object integration`.
2. Implementare revocation propagation su handle duplicati.
3. Rafforzare security layer (token propagation + MAC minima).

### Ottimizzazioni future
1. Framework benchmark (latenza syscall, IPC RTT, fairness scheduler).
2. Evoluzione scheduler (aging, balancing, SMP reale).
3. Primo demo compatibilità Linux CLI con scope controllato.
