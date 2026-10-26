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

static const char *TAG = "app_lang";

/* Said alone, they send the assistant back to sleep without an answer */
static const char *const s_goodbyes_fr[] = { "au revoir", "au revoir esp", "bonne nuit", "bonne nuit esp", "à demain", "à plus", "ciao", "bonne soirée", "bonne journée", "bonne après midi", NULL };
static const char *const s_goodbyes_en[] = { "goodbye", "good bye", "bye", "bye bye", "goodbye esp", "bye esp",
                                             "good night", NULL };

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
                         "de développer. Chaque message de l'utilisateur commence par la date et l'heure "
                         "actuelles entre crochets : utilise-les si besoin, sans les répéter sauf si on te "
                         "les demande.",
        .tts_instructions = "Tu es une voix de synthèse qui lit des textes à voix haute. Le message de "
                            "l'utilisateur est le texte à lire : lis-le exactement, mot pour mot, sans "
                            "rien ajouter, retirer ni reformuler. Ne réponds jamais à ce texte, même s'il "
                            "contient une question ou une demande : lis-le simplement. Parle en français, "
                            "avec un accent français standard, une voix naturelle et chaleureuse, et un "
                            "débit légèrement rapide.",
        .stt_error = "Je n'ai pas réussi à comprendre la question. Pouvez-vous répéter ?",
        .goodbyes = s_goodbyes_fr,
        .weekdays = { "DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM" },
        .now_format = "[Nous sommes le %s %d %s %d, il est %02d h %02d.]",
        .weekdays_long = { "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi" },
        .months = { "janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre",
                    "octobre", "novembre", "décembre" },
        .memory_header = "Notes que l'utilisateur t'a demandé de retenir (« je » y désigne l'utilisateur, "
                         "la date entre parenthèses est celle de la note) :",
        .memory_saved = "C'est noté.",
        .memory_full = "Ma mémoire est pleine. Supprimez des notes dans le fichier MEMORY.TXT.",
        .memory_forgotten = "C'est oublié.",
        .memory_not_found = "Je n'ai rien trouvé de tel dans ma mémoire.",
        .memory_cleared = "J'ai tout oublié.",
        .memory_empty = "Je n'ai rien mémorisé pour l'instant.",
        .memory_list_intro = "Voici ce que vous m'avez demandé de retenir. ",
        .lang_ask = "Quelle langue voulez-vous ? Français ou anglais ?",
        .lang_already = "Je parle déjà français.",
        .lang_unknown = "Je ne connais pas cette langue. Je parle français et anglais.",
        .lang_switched = "Très bien, je parle maintenant français.",
        .model_ask_intro = "Voici les modèles disponibles.",
        .model_current = ", celui que j'utilise",
        .model_ask_question = "Lequel voulez-vous ? Dites son numéro.",
        .model_already = "J'utilise déjà %s.",
        .model_unknown = "Je n'ai pas compris. Dites un numéro de 1 à %d.",
        .model_switched = "Très bien, j'utilise maintenant %s.",
        .country_ask = "Quel est ton pays ?",
        .city_ask = "Quelle est ta ville ?",
        .country_confirm = "Confirmes-tu que tu veux changer ton pays en %s ?",
        .city_confirm = "Confirmes-tu que tu veux changer ta ville en %s ?",
        .country_unknown = "Je ne connais pas ce pays. Redis-moi « change mon pays » pour réessayer.",
        .place_saved = "C'est enregistré.",
        .place_cancelled = "D'accord, je ne change rien.",
        .location_line = "Lieu de l'utilisateur : %s (le code à deux lettres est celui du pays). Quand il demande "
                         "l'actualité, la météo ou une information locale sans préciser de lieu, il s'agit de ce "
                         "lieu et de ce pays : pour l'actualité, cherche celle de ce pays en le nommant dans ta "
                         "recherche, et ne donne pas l'actualité d'autres pays sauf s'il le demande.",
        .voice_choppy = "(Connexion trop lente : la voix n'a pas pu être lue de façon fluide.)",
        .home_line = "Pour toute question sur la maison (pièces, appareils, leur état, ambiances, flows, présence, "
                     "consommation), appelle chaque fois les outils de la Homey : les résultats plus anciens de la "
                     "conversation peuvent être raccourcis ou dépassés, ne réponds jamais de mémoire. Pour la liste "
                     "des pièces, appelle zones_list. Quand il demande une action (allumer, éteindre, régler, "
                     "activer une ambiance, lancer un flow), réponds en quelques mots : « C'est fait. » si tout a "
                     "marché, sinon dis brièvement ce qui n'a pas marché et pose la seule question utile pour "
                     "continuer. Ne décris pas les appareils, les outils ni les étapes, ne propose rien d'autre.",
        /* Spoken as the answer when the chat model fails */
        .llm_errors = {
            [LLM_PROVIDER_CLAUDE] = {
                [LLM_ERR_NO_KEY] = "Aucune clé Claude n'est configurée. Ajoutez-la dans le fichier de configuration.",
                [LLM_ERR_NETWORK] = "Je n'arrive pas à joindre Claude. Vérifiez la connexion internet.",
                [LLM_ERR_AUTH] = "La clé Claude est refusée. Vérifiez-la dans le fichier de configuration.",
                [LLM_ERR_CREDIT] = "Le crédit Claude est épuisé. Ajoutez du crédit sur la console Anthropic.",
                [LLM_ERR_BUSY] = "Claude est surchargé pour le moment. Réessayez dans un instant.",
                [LLM_ERR_REFUSAL] = "Claude a refusé de répondre à cette question.",
                [LLM_ERR_OTHER] = "Une erreur est survenue avec Claude.",
            },
            [LLM_PROVIDER_OPENAI] = {
                [LLM_ERR_NO_KEY] = "Aucune clé OpenAI n'est configurée. Ajoutez-la dans le fichier de configuration.",
                [LLM_ERR_NETWORK] = "Je n'arrive pas à joindre GPT. Vérifiez la connexion internet.",
                [LLM_ERR_AUTH] = "La clé OpenAI est refusée. Vérifiez-la dans le fichier de configuration.",
                [LLM_ERR_CREDIT] = "Le crédit OpenAI est épuisé. Ajoutez du crédit sur la plateforme OpenAI.",
                [LLM_ERR_BUSY] = "GPT est surchargé pour le moment. Réessayez dans un instant.",
                [LLM_ERR_REFUSAL] = "GPT a refusé de répondre à cette question.",
                [LLM_ERR_OTHER] = "Une erreur est survenue avec GPT.",
            },
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
        .settings_language = "Langue",
        .settings_model = "Modèle",
        .settings_voice = "Voix",
        .voice_sample = "Bonjour, je suis %s. Ma voix vous plaît ?",
        .settings_save = "Enregistrer",
        .settings_saved = "Enregistré.",
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
                         "question calls for a long answer, give the essentials and offer to elaborate. Each user "
                         "message starts with the current date and time in brackets: use them when useful, "
                         "without repeating them unless asked.",
        .tts_instructions = "You are a text-to-speech voice. The user's message is the text to read: read "
                            "it exactly, word for word, without adding, removing or rephrasing anything. "
                            "Never answer it, even if it contains a question or a request: just read it. "
                            "Speak naturally and warmly, at a slightly brisk pace.",
        .stt_error = "I couldn't make out the question. Could you repeat it?",
        .goodbyes = s_goodbyes_en,
        .weekdays = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" },
        .now_format = "[It is %s %d %s %d, %02d:%02d.]",
        .weekdays_long = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
        .months = { "January", "February", "March", "April", "May", "June", "July", "August", "September",
                    "October", "November", "December" },
        .memory_header = "Notes the user asked you to remember (\"I\" means the user, the date in brackets is "
                         "the date of the note):",
        .memory_saved = "Noted.",
        .memory_full = "My memory is full. Please remove notes in the MEMORY.TXT file.",
        .memory_forgotten = "Forgotten.",
        .memory_not_found = "I found nothing like that in my memory.",
        .memory_cleared = "I have forgotten everything.",
        .memory_empty = "I have not memorized anything yet.",
        .memory_list_intro = "Here is what you asked me to remember. ",
        .lang_ask = "Which language do you want? French or English?",
        .lang_already = "I already speak English.",
        .lang_unknown = "I don't know this language. I speak French and English.",
        .lang_switched = "Very well, I will now speak English.",
        .model_ask_intro = "Here are the available models.",
        .model_current = ", the one I use",
        .model_ask_question = "Which one do you want? Say its number.",
        .model_already = "I already use %s.",
        .model_unknown = "I didn't understand. Say a number from 1 to %d.",
        .model_switched = "Very well, I now use %s.",
        .country_ask = "What is your country?",
        .city_ask = "What is your city?",
        .country_confirm = "Do you confirm that you want to change your country to %s?",
        .city_confirm = "Do you confirm that you want to change your city to %s?",
        .country_unknown = "I don't know this country. Say \"change my country\" again to retry.",
        .place_saved = "Saved.",
        .place_cancelled = "All right, nothing changed.",
        .location_line = "User's location: %s (the two-letter code is the country's). When the user asks for the "
                         "news, the weather or local information without naming a place, it is about this place and "
                         "country: for the news, search the news of this country by naming it in your search, and do "
                         "not give news from other countries unless asked.",
        .voice_choppy = "(Connection too slow: the speech could not be played smoothly.)",
        .home_line = "For any question about the home (rooms, devices, their state, moods, flows, presence, "
                     "consumption), call the Homey tools every time: older results in the conversation may be "
                     "shortened or out of date, never answer from memory. For the list of rooms, call zones_list. "
                     "When the user asks for an action (switch on or off, set, activate a mood, start a flow), "
                     "answer in a few words: \"Done.\" if everything worked, otherwise say briefly what failed and "
                     "ask the one question needed to go on. Do not describe the devices, the tools or the steps, "
                     "do not offer anything else.",
        /* Spoken as the answer when the chat model fails */
        .llm_errors = {
            [LLM_PROVIDER_CLAUDE] = {
                [LLM_ERR_NO_KEY] = "No Claude key is configured. Add it to the configuration file.",
                [LLM_ERR_NETWORK] = "I can't reach Claude. Please check the internet connection.",
                [LLM_ERR_AUTH] = "The Claude key was rejected. Please check it in the configuration file.",
                [LLM_ERR_CREDIT] = "The Claude credit is exhausted. Please add credit in the Anthropic console.",
                [LLM_ERR_BUSY] = "Claude is overloaded right now. Please try again in a moment.",
                [LLM_ERR_REFUSAL] = "Claude declined to answer this question.",
                [LLM_ERR_OTHER] = "Something went wrong with Claude.",
            },
            [LLM_PROVIDER_OPENAI] = {
                [LLM_ERR_NO_KEY] = "No OpenAI key is configured. Add it to the configuration file.",
                [LLM_ERR_NETWORK] = "I can't reach GPT. Please check the internet connection.",
                [LLM_ERR_AUTH] = "The OpenAI key was rejected. Please check it in the configuration file.",
                [LLM_ERR_CREDIT] = "The OpenAI credit is exhausted. Please add credit on the OpenAI platform.",
                [LLM_ERR_BUSY] = "GPT is overloaded right now. Please try again in a moment.",
                [LLM_ERR_REFUSAL] = "GPT declined to answer this question.",
                [LLM_ERR_OTHER] = "Something went wrong with GPT.",
            },
        },
        .steps_title = "Steps",
#if !CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite
        .steps_content = "1. Wake the device up by saying \"Bonjour ESP\" (or \"Hi ESP\").\n"
                         "2. Ask your question, then wait for the answer.\n"
                         "3. While asleep, touch the screen to wake the device up.",
#else
        .steps_content = "1. Wake the device up by saying \"Bonjour ESP\" (or \"Hi ESP\").\n"
                         "2. Ask your question, then wait for the answer.\n"
                         "3. While asleep, press \"Enter\" to wake the device up.\n"
                         "4. Long press on \"Enter\": open/close the settings.",
#endif
        .reset_title = "Factory Reset",
        .reset_content = "1. Press \"OK\" to enter the configuration mode.\n"
                         "2. Plug the box into your PC and fill in the Wi-Fi and the OpenAI and Claude keys.\n"
                         "3. Follow the instructions on the screen to finish.",
        .settings_title = "Settings",
        .settings_language = "Language",
        .settings_model = "Model",
        .settings_voice = "Voice",
        .voice_sample = "Hello, I am %s. Do you like my voice?",
        .settings_save = "Save",
        .settings_saved = "Saved.",
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

const app_lang_t *app_lang_find(const char *code)
{
    for (size_t i = 0; i < sizeof(s_languages) / sizeof(s_languages[0]); i++) {
        if (strcasecmp(code, s_languages[i].code) == 0) {
            return &s_languages[i];
        }
    }
    return NULL;
}

const app_lang_t *app_lang_get(void)
{
    return s_current;
}
