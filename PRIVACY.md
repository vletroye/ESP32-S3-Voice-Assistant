# Privacy policy / Règles de confidentialité

*ESP32-S3 Voice Assistant — firmware for the ESP32-S3-BOX-3*

## English

This firmware runs on a device you own. When you link your Google account to it (Google Tasks, Gmail, Google Calendar), it uses Google's
OAuth 2.0 with the client of **your own** Google Cloud project, entered by you in the device's `CONFIG.INI`.

- **What is accessed**: your Google Tasks lists and tasks (scope `https://www.googleapis.com/auth/tasks`), your
  Gmail mails (scope `https://www.googleapis.com/auth/gmail.modify`: read, send after your confirmation, mark, archive,
  move to the bin; never deleted for good) and your Google Calendar events (scope
  `https://www.googleapis.com/auth/calendar.events`), only when you ask the assistant by voice.
- **Where it is kept**: the refresh token given by Google is kept on the device only (its internal flash), never
  sent anywhere else, never shown nor logged. No copy of your tasks, mails or events is kept on the device.
- **Who receives what**: the device talks to Google's APIs directly. To answer you, the text of your request and of
  the tasks, mails or events read is sent to the language model you chose in the device's settings (Anthropic, OpenAI, Google,
  Mistral, xAI, OpenRouter, Perplexity), under that provider's own terms. Nothing is sent to the author of this
  firmware, and no data is sold or shared for advertising.
- **Removal**: "Disconnect" on the device's Settings screen deletes the token and revokes the access at Google; you
  can also revoke it at any time in your Google account (Security → Third-party connections). A factory reset of the
  device deletes it too.

The use of information received from Google APIs adheres to the
[Google API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy), including
the Limited Use requirements.

## Français

Ce firmware fonctionne sur un appareil qui vous appartient. Quand vous y connectez votre compte Google (Google
Tasks, Gmail, Google Agenda), il utilise OAuth 2.0 de Google avec le client de **votre propre** projet Google Cloud, que vous saisissez
dans le `CONFIG.INI` de l'appareil.

- **Ce qui est consulté** : vos listes et tâches Google Tasks (autorisation
  `https://www.googleapis.com/auth/tasks`), vos mails Gmail (autorisation
  `https://www.googleapis.com/auth/gmail.modify` : lire, envoyer après votre confirmation, marquer, archiver, mettre
  à la corbeille ; jamais de suppression définitive) et les événements de votre Google Agenda (autorisation
  `https://www.googleapis.com/auth/calendar.events`), uniquement quand vous le demandez à l'assistant, à la voix.
- **Où c'est gardé** : le jeton d'actualisation donné par Google est gardé uniquement sur l'appareil (sa mémoire
  flash), jamais envoyé ailleurs, jamais affiché ni écrit dans les traces. Aucune copie de vos tâches, mails ou événements
  n'est gardée sur l'appareil.
- **Qui reçoit quoi** : l'appareil s'adresse directement aux API de Google. Pour vous répondre, le texte de votre
  demande et des tâches, mails ou événements lus est envoyé au modèle de langage choisi dans les réglages de l'appareil (Anthropic,
  OpenAI, Google, Mistral, xAI, OpenRouter, Perplexity), selon les conditions de ce fournisseur. Rien n'est envoyé
  à l'auteur de ce firmware, et aucune donnée n'est vendue ni partagée à des fins publicitaires.
- **Suppression** : « Déconnecter » sur l'écran Réglages de l'appareil efface le jeton et révoque l'accès chez
  Google ; vous pouvez aussi le révoquer à tout moment dans votre compte Google (Sécurité → Connexions à des
  applications tierces). La réinitialisation d'usine de l'appareil l'efface également.

L'utilisation des informations reçues des API Google respecte les
[règles relatives aux données utilisateur des services d'API Google](https://developers.google.com/terms/api-services-user-data-policy),
y compris les exigences d'utilisation limitée.
