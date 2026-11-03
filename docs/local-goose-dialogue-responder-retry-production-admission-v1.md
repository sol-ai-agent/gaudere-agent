# Première admission retry generation 1 — gate Fedora de production

Statut : **code-only jusqu'à CI intégralement verte, fusion et PASS Fedora réel**.

La répétition Fedora du 2026-11-03 a réussi depuis `main=178fef625769955fa3f9f41a92ee2e77f743edac`, sous #268. La copie contenait exactement un record d'audit génération 1, avec `ADMISSION_RESULT=accepted`, puis un replay `duplicate`. Toutes les quatre empreintes SQLite de production ont été conservées, le sidecar retry production est resté absent, séquence 4 pending/non ACKée, provider total 10.

Lineage déterministe attendu :
`cognition.local-goose-dialogue.responder-retry-lineage.v1:da6bb42704949f0003e670c06e7ed04078c238f08356f1853f9b3776455f2bca`.

## Autorisation indépendante

Script : `scripts/admit-first-production-responder-retry-generation1.sh`.

Lancement seulement après merge, en `sh` (mode Git intentionnel `100644`), checkout `main` propre égal à `origin/main`, token dédié `GAUDERE_RESPONDER_RETRY_ADMISSION_PRODUCTION_AUTHORIZATION=AUTHORIZED_RESPONDER_RETRY_ADMISSION_PRODUCTION`.

Ce token autorise **seulement** l'admission durable du record de lignée génération 1 dans le **nouveau** sidecar `local-goose-dialogue-responder-retry.db`. Il n'autorise aucun nouveau grant, lease, decision, intent, disposition, Task, CAS/head advance, ACK ni dispatch.

## Séquence contrôlée

1. Vérifier le checkpoint réel exact : image de production `93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071`, core pin `1316cf68db93e4c91a7bd79fbd289b8f382f8659`, `Network=none`, provider total 10, consumer cursor 3, completion humain 4, head revision 3, ancienne authority expirée et runtime dormant, sidecar retry production absent.
2. Construire le helper confiné depuis le commit exact. Il ne voit que `/rehearsal`, jamais un montage des bases de production.
3. Arrêter brièvement le service, capturer le backup stopped-state, faire quatre snapshots SQLite cohérents. Redémarrer immédiatement le service inchangé et revalider la production.
4. Prouver sur copie : `inspect` n'écrit rien ; `admit-generation-1` crée exactement le lineage canonique ; second `inspect` le retrouve ; exact replay donne `duplicate` ; les quatre bases historiques copiées restent logiquement inchangées.
5. Contrôler les SHA logiques des **quatre bases de production** contre leurs snapshots, et contrôler le sidecar retry absent. Arrêter à nouveau le service. Faire un backup stopped-state immédiatement avant commit.
6. Avec `committing=1` **avant** l'essai, copier la DB retry de la répétition vers un fichier privé temporaire en production, forcer `0600` et `fsync`, puis créer le chemin final exclusivement par lien dur atomique `os.link(staging,target)` : échec si un fichier existe déjà. Supprimer le temporaire.
7. Vérifier l'empreinte brute du nouveau sidecar contre la copie et le contenu canonique `schema=1`, exactement un lineage, ID attendu, aucun autre enregistrement. Revalider les quatre SHA logiques historiques.
8. Redémarrer le même service, revalider la disponibilité, l'image, `Network=none`, provider 10, cursor 3, head 3, séquence 4 non ACKée, runtime authority dormant, et le record retry durable.

## Échec et sûreté

- Échec **avant** tentative de commit : restart best effort, `SERVICE_RECOVERY=restart_attempted_without_production_commit` si le service était arrêté.
- Échec **depuis** le début de la tentative d'ajout atomique : stopper ou laisser arrêté le service pour inspection, `SERVICE_RECOVERY=stopped_after_possible_retry_sidecar_commit`. Ne jamais relancer aveuglément, restaurer un backup sans inspection ni effacer le sidecar.
- Les chemins de sauvegarde et de travail sont conservés en cas d'échec.
- Aucune copie du nouveau sidecar retry vers un vieux store responder. Aucun rollback destructif de preuves durables.
- Le `main` GitHub mergé **n'est pas** un déploiement et le marqueur attendu n'existe pas avant la preuve réelle.

Marqueur de preuve final : `FEDORA_GAUDERE_RESPONDER_RETRY_ADMISSION_PRODUCTION=PASS`.

## Après ce gate

Un record de retry est **une preuve d'audit, pas une autorité de dispatch**. La génération 1 n'autorise toujours pas de grant ni de lease. Une future autorité, une décision, un intent et un dispatch de la séquence 4 devront avoir un nouveau contrat durable, des tests et des gates indépendants. Ne jamais réouvrir les objets 6A/6B1/6B2 historiques, ni réinitialiser la disposition `manual_review` en place.

Provider/OpenAI reste OFF. B10 reste borné aux opérations typées existantes.