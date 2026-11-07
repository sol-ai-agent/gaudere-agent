# Fedora retry generation-1 — inspection sur instantané figé (gate isolé)

Statut : **code/CI seulement**, jusqu'à fusion puis autorisation humaine distincte. Ce document ne donne aucune permission d'exécuter le runner ou d'activer une autorité.

## Pré-requis observés

- Admission production du lineage génération 1 rapportée avec
  `FEDORA_GAUDERE_RESPONDER_RETRY_ADMISSION_PRODUCTION=PASS`, 2026-11-06,
  dans les preuves terminal opérateur de la PR #269.
- #271: `inspect_generation_1()` ne fait que classifier les preuves.
- #272: ouverture strictement `existing_read_only` du sidecar retry,
  sur copie SQLite stable et sûre.
- #273: exécutable confiné `gaudere-responder-retry-frozen-snapshot`
  sans surface d'écriture ni production.

Lignée attendue:
`cognition.local-goose-dialogue.responder-retry-lineage.v1:da6bb42704949f0003e670c06e7ed04078c238f08356f1853f9b3776455f2bca`.

## Exécution exclusivement sous gate explicite

Runner versionné:
`scripts/prove-responder-retry-frozen-snapshot-fedora.sh`.

Mode Git volontaire `100644`: invocation seulement par `sh`, après fusion sur `main` et sur autorisation humaine spécifique.

`GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_AUTHORIZATION=AUTHORIZED_RESPONDER_RETRY_FROZEN_SNAPSHOT`

Cette autorisation ne couvre que l'arrêt bref, le backup stopped-state, les cinq copies SQLite cohérentes, la relance de l'image inchangée et l'inspection du répertoire privé. Elle n'autorise aucun grant, lease, decision, intent, disposition, dispatch, Task, ACK ou mutation de la lignée.

### Préflight fail-closed

Le runner impose un checkout propre `main == origin/main` et le core pin exact `1316cf68db93e4c91a7bd79fbd289b8f382f8659`. Il vérifie l'image de production exacte
`93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071`,
Podman rootless, `Network=none`, provider total durable 10,
schema autorité 3, responder cursor 3, prochaine sequence 4 humaine,
head rev3/Task exacte, disposition historique `manual_review`,
observer désarmé, runtime authority `disabled`.

Le sidecar retry doit **déjà exister**, être fichier ordinaire `0600`,
schema 1, avoir exactement une lignée ID canonique, sans compagnon
SQLite `-wal/-shm/-journal`. Il n'est jamais créé par cette opération.

### Preuve sur copie

1. Construire l'image helper rootless confinée, en verrouillant agent ref et core ref et l'unique commande `inspect-generation-1`.
2. Stopper le service seulement pour backup et snapshots `sqlite3.Connection.backup` des cinq fichiers: `state.db`, thread, completion, responder/authority et retry.
3. Redémarrer immédiatement l'image de production inchangée, revalider le pré-état et les cinq empreintes logiques; conserver aussi un SHA brut du sidecar retry original.
4. Exécuter le helper avec `--network none`, `--read-only`, capacités retirées, uniquement `/rehearsal` monté. Les stores historiques peuvent ouvrir leur **copie** en mode ordinaire; le sidecar retry doit être ouvert par `existing_read_only` et non muté.
5. Exiger `ELIGIBILITY=evidence_consistent_no_authority` et le marqueur helper `GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_INSPECT=PASS`.
6. Revalider tous les hashes logiques copiés, l'absence de sidecars SQLite générés par le reader retry, tous les hashes logiques de production et le SHA brut retry; revérifier service, image, `Network=none`, provider 10, cursor 3, sequence 4 non ACKée et head3.

Toute erreur avant/pendant la copie tente seulement de redémarrer le service inchangé si celui-ci est encore arrêté; aucun restore automatique et aucune mutation d'autorité. Le backup et le workspace sont conservés pour inspection.

Marqueur final attendu seulement après preuve réelle:

`FEDORA_GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_ISOLATED=PASS`

## Frontières

Cette tranche GitHub n'exécute pas le runner Fedora. La preuve doit être **distinctement autorisée** par l'opérateur, puis inscrite comme preuve terminal réelle; CI et merge ne valent ni déploiement ni PASS Fedora.

Une éligibilité positive est une cohérence d'audit, **jamais une permission d'intervenir**. Le futur contrat grant/lease/decision/intent/disposition propre au retry doit rester séparé et ne jamais contourner `UNIQUE(consumer_id,event_id)` ni la disposition historique v1. Aucun ACK sequence4, nouveau tour, provider, cycle ou stimulus. Production `Network=none`, provider/OpenAI OFF.
