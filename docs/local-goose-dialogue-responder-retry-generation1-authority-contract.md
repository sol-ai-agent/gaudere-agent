# Autorité de retry génération 1 — contrat d’architecture (préparatoire)

**Statut : conception uniquement. Aucune autorité créée ou activée.**

## Base prouvée

La tentative historique de la séquence humaine 4 a été récupérée en production : grant activé dans une fenêtre désormais expirée, lease expirée sans tour, decision et intent expirés, disposition `manual_review`, cursor 3, thread main revision 3, séquence 4 non ACKée, aucun successor. Cette chaîne est **immuable**.

La preuve Fedora isolée de #268 a admis sur **copie** le lineage generation 1 ID :

`cognition.local-goose-dialogue.responder-retry-lineage.v1:da6bb42704949f0003e670c06e7ed04078c238f08356f1853f9b3776455f2bca`

L’admission durable en production est une **étape séparée**, préparée dans #269, qui doit être prouvée avant toute activation du retry. Un lineage ne confère aucun droit.

## Obstacles structurels actuels (code réel)

- AuthorityStore schema3 : `local_goose_dialogue_responder_decision` impose `UNIQUE(consumer_id,event_id)` ; une ancienne décision terminale ne peut pas être remplacée pour le même événement.
- ResponderStore : `local_goose_dialogue_responder_disposition` impose `UNIQUE(consumer_id,completion_sequence)` et `UNIQUE(consumer_id,event_id)` ; la disposition historique `manual_review` doit rester intacte.
- Dispatcher historique exige la disposition liée au même intent et ne peut pas dispatch de nouvelle tentative sur cet événement sans évolution de contrat.

**Interdit** : `DELETE` d’un objet historique, réouverture d’une lease, modification de la décision ou de la disposition originale, suppression des contraintes uniques sans migration explicite, replay du helper #260, restauration d’un ancien backup pour contourner une expiration.

## Choix architectural pour la prochaine preuve isolée

Préférer un espace de noms **distinct pour la génération 1** dans un sidecar retry versionné, plutôt qu’un réemploi ambigu des objets schema3 historiques :

- `retry_lineage` canonique et immuable (déjà schema1) ;
- `retry_grant` préparé puis activé explicitement, avec source d’autorité attestée côté hôte ;
- `retry_lease` finie, quota exact (premier essai : un tour), non renouvelable ;
- `retry_decision` signée logiquement par le nouveau grant + lease et **l’événement/head exacts** ;
- `retry_intent` distinct de l’intent historique, avec nouvelle clé d’idempotence ;
- `retry_disposition` generation1 distincte, n’écrasant jamais le `manual_review` historique.

Ces tables/records n’existent **pas encore** : ce texte est un contrat de développement, pas l’affirmation d’une capacité exécutable.

L’extension schema1->schema2 du sidecar retry devra être **opt-in**, d’abord sur copie ; un open de lecture ordinaire ne doit jamais migrer ou activer. Les IDs/requêtes doivent être déterministes et immuables, avec unicité `(consumer,event,generation)` pour la tentative et unicité de request-id. Aucun grant, lease, decision, intent ou disposition ne doit être synthétisé à partir du lineage seul.

## Contrat de décision et dispatch

1. **Observation** : une séquence 4 pending et un lineage valide ne créent aucune décision. L’observer historique peut encore classer l’événement `intervention_owned` du fait du précédent `manual_review`; une tentative retry doit vérifier l’evidence génération1 explicitement plutôt que réinterpréter cette classification comme autorité.
2. **Grant préparé** : record durable sans lease. Restart ne l’active pas.
3. **Activation distincte** : une lease finite, une par thread, quota strict, expiration absolue. Pas de renouvellement, pas d’activation implicite.
4. **Decision préparée** : event-id/result SHA/sequence 4, `human/bertrand`, thread alias `main`, revision 3 et Task exacts, grant/lease generation1 et bytes exacts du futur message. Pas d’intent, Task ou ACK.
5. **Bind distinct** : crée un intent génération1 et une retry disposition associée, mais **aucun Task et aucun ACK**.
6. **Dispatch distinct** : autorité explicite encore valide, event pending, cursor3/head3, CAS sur le preferred-head V3, nouvelle Task/idempotency key, accounting quota, réconciliation après crash, puis seulement ACK exact de sequence4. Idempotent à la même Task, jamais deux successors.
7. **Stale/expiry/failure** : échouer fermé, terminaliser les objets génération1 de manière cohérente ; audit historique inchangé ; aucune consommation de quota ni ACK par défaut.

Le code du dispatcher historique ne doit pas être réutilisé par un simple contournement des recherches de disposition. Un dispatcher retry spécifique ou une extension génération-aware revue et testée est requis. Le modèle local doit rester tool-free; provider/OpenAI OFF, `Network=none`, cycle/stimuli/wake/shell hors autorité.

## Ordre des tranches

- **R0** : #268 preuve de lineage sur copie — Fedora PASS, CLOSED.
- **R1** : #269 ajouter uniquement le lineage génération1 en production — code/CI/merge puis gate Fedora distinct.
- **R2** : schema2 retry + store/identités génération1 + tests adversariaux — **aucun wiring production**.
- **R3** : service typed retry + dispatcher generation-aware, bornés et idle par défaut ; tests crash/restart/duplicate/stale/expiry/quota.
- **R4** : preuve Fedora isolée sur copie de production, aucun Task/ACK de production.
- **R5** : déploiement code-only dormant et migration schema2 explicitement autorisée, sans grant.
- **R6** : préparer puis activer un nouveau grant/lease génération1 par **gates séparés**.
- **R7** : préparer une nouvelle decision puis la binder par **gates séparés**.
- **R8** : dispatch/ACK de sequence4 **gate humain séparé**, jamais implicitement lancé après R7.

Tous les scripts de R6-R8 doivent être écrits, revus, compilés et mergés **avant l’activation de la lease**, pour éviter qu’une lease de deux heures expire pendant la CI comme lors de la tentative historique. Aucun helper ne doit provisionner automatiquement une autorité pour compenser un délai.

R2-R8 restent à implémenter et prouver. Tant que R1 n’a pas un PASS réel, ne pas exécuter ni déclarer ces étapes opérationnelles.