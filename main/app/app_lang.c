/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <strings.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "app_lang.h"
#include "claude_client.h"

static const char *TAG = "app_lang";

static const app_lang_t s_languages[] = {
    {
        .code = "fr",
        .listening = "J'écoute ...",
        .thinking = "Je réfléchis ...",
        .sorry = "Désolé, je n'ai pas compris.",
        .wifi_connecting = "Connexion au Wi-Fi\n",
        .wifi_failed = "Wi-Fi non connecté\n",
        .system_prompt = "Tu es un assistant vocal qui parle français. Ta réponse est lue à voix haute "
                         "par une synthèse vocale : réponds de façon courte et orale, en une à trois "
                         "phrases simples, comme dans une conversation. N'utilise jamais de Markdown, "
                         "de listes, de titres, d'émojis ni de symboles qui ne se prononcent pas. "
                         "Si une question demande une longue réponse, donne l'essentiel et propose "
                         "de développer.",
        .tts_instructions = "Parle en français, avec un accent français standard, une voix naturelle "
                            "et chaleureuse, et un débit légèrement rapide.",
        .stt_error = "Je n'ai pas réussi à comprendre la question. Pouvez-vous répéter ?",
        /* Spoken as the answer when Claude fails */
        .claude_errors = {
            [CLAUDE_ERR_NO_KEY] = "Aucune clé Claude n'est configurée. Ajoutez-la dans le fichier de configuration.",
            [CLAUDE_ERR_NETWORK] = "Je n'arrive pas à joindre Claude. Vérifiez la connexion internet.",
            [CLAUDE_ERR_AUTH] = "La clé Claude est refusée. Vérifiez-la dans le fichier de configuration.",
            [CLAUDE_ERR_CREDIT] = "Le crédit Claude est épuisé. Ajoutez du crédit sur la console Anthropic.",
            [CLAUDE_ERR_BUSY] = "Claude est surchargé pour le moment. Réessayez dans un instant.",
            [CLAUDE_ERR_REFUSAL] = "Claude a refusé de répondre à cette question.",
            [CLAUDE_ERR_OTHER] = "Une erreur est survenue avec Claude.",
        },
        .steps_title = "Étapes",
#if !CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite
        .steps_content = "1. Réveillez l'appareil en disant « Bonjour ESP » (ou « Hi ESP »).\n"
                         "2. Posez votre question, puis attendez la réponse.\n"
                         "3. En veille, touchez l'écran pour réveiller l'appareil.",
#else
        .steps_content = "1. Réveillez l'appareil en disant « Bonjour ESP » (ou « Hi ESP »).\n"
                         "2. Posez votre question, puis attendez la réponse.\n"
                         "3. En veille, appuyez sur « Enter » pour réveiller l'appareil.\n"
                         "4. Appui long sur « Enter » : ouvrir/fermer les réglages.",
#endif
        .settings_title = "Réglages",
        .settings_region = "Région",
        .reset_title = "Réinitialisation",
        .reset_content = "1. Appuyez sur « OK » pour passer en mode configuration.\n"
                         "2. Branchez la box sur votre PC et renseignez le Wi-Fi et les clés OpenAI et Claude.\n"
                         "3. Suivez les instructions à l'écran pour terminer.",
    },
    {
        .code = "en",
        .listening = "Listening ...",
        .thinking = "Thinking ...",
        .sorry = "Sorry, I can't understand.",
        .wifi_connecting = "Connecting to Wi-Fi\n",
        .wifi_failed = "Not Connected to Wi-Fi\n",
        .system_prompt = "You are a voice assistant. Your answer is read aloud by a speech synthesizer: "
                         "answer briefly and conversationally, in one to three simple sentences. Never use "
                         "Markdown, lists, headings, emojis or symbols that cannot be pronounced. If a "
                         "question calls for a long answer, give the essentials and offer to elaborate.",
        .tts_instructions = "Speak naturally and warmly, at a slightly brisk pace.",
        .stt_error = "I couldn't make out the question. Could you repeat it?",
        /* Spoken as the answer when Claude fails */
        .claude_errors = {
            [CLAUDE_ERR_NO_KEY] = "No Claude key is configured. Add it to the configuration file.",
            [CLAUDE_ERR_NETWORK] = "I can't reach Claude. Please check the internet connection.",
            [CLAUDE_ERR_AUTH] = "The Claude key was rejected. Please check it in the configuration file.",
            [CLAUDE_ERR_CREDIT] = "The Claude credit is exhausted. Please add credit in the Anthropic console.",
            [CLAUDE_ERR_BUSY] = "Claude is overloaded right now. Please try again in a moment.",
            [CLAUDE_ERR_REFUSAL] = "Claude declined to answer this question.",
            [CLAUDE_ERR_OTHER] = "Something went wrong with Claude.",
        },
    },
};

static app_lang_t s_custom;
static char s_custom_code[LANG_CODE_SIZE];
static const app_lang_t *s_current = &s_languages[0];

void app_lang_set(const char *code)
{
    if (code == NULL || code[0] == '\0') {
        ESP_LOGW(TAG, "No language given, keeping %s", s_current->code);
        return;
    }

    for (size_t i = 0; i < sizeof(s_languages) / sizeof(s_languages[0]); i++) {
        if (strcasecmp(code, s_languages[i].code) == 0) {
            s_current = &s_languages[i];
            ESP_LOGI(TAG, "Language: %s", s_current->code);
            return;
        }
    }

    /* Unsupported UI language: English texts, but transcribe and answer in the requested language */
    s_custom = s_languages[1];
    strlcpy(s_custom_code, code, sizeof(s_custom_code));
    s_custom.code = s_custom_code;
    s_custom.system_prompt = "You are a voice assistant. Always answer in the same language as the user, "
                             "concisely (a few sentences at most) and without Markdown formatting, "
                             "because your answer will be read aloud.";
    s_current = &s_custom;
    ESP_LOGW(TAG, "No UI texts for language '%s', using English UI", s_custom_code);
}

const app_lang_t *app_lang_get(void)
{
    return s_current;
}
