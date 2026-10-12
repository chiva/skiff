#ifndef SKIFF_I18N_H
#define SKIFF_I18N_H

/*
 * What Skiff says to the player, in English or Spanish, chosen from the PSP's system language.
 * Every string is UTF-8 and comes from one of two tables, so a language can never miss one:
 *
 *   - SKIFF_TEXT_TABLE below: one row per UI string, with its English and Spanish text side by side
 *     and the number of placeholders both must use;
 *   - error sentences: English is error.h's message (one source), Spanish is a table in
 *     src/i18n/i18n.c naming each error.h code once; a missing or misspelt row does not compile.
 *
 * Placeholders are {1} to {9}: positional, so a translation can reorder them, and filled only with
 * strings, so a translation cannot introduce a mismatched printf conversion. Numbers are formatted
 * by the caller first (see skiff/ui.h). Each text declares how many placeholders it takes; the
 * tests check both languages use exactly {1}..{n}. Logs never use this text: they use error names.
 */

#include <stddef.h>

#include "skiff/error.h"

/* The PSP's PSP_SYSTEMPARAM_LANGUAGE_SPANISH (psputility_sysparam.h). Platform code reads the
 * system language and passes it in, so this file stays free of PSP headers. */
#define SKIFF_PSP_SYSTEM_LANGUAGE_SPANISH 3

/* The longest text after formatting that the UI asks for. */
#define SKIFF_TEXT_MAX 256

typedef enum skiff_language {
    SKIFF_LANGUAGE_ENGLISH,
    SKIFF_LANGUAGE_SPANISH,
    SKIFF_LANGUAGE_COUNT,
} skiff_language;

/* X(id, placeholders, English, Spanish). Append new rows anywhere: ids are not stored or shown. */
#define SKIFF_TEXT_TABLE(X)                                                                        \
    /* Separators and units for numbers the UI formats (skiff/ui.h). */                            \
    X(SKIFF_TEXT_DECIMAL_SEPARATOR, 0, ".", ",")                                                   \
    /* Screen titles. */                                                                           \
    X(SKIFF_TEXT_TITLE_LIBRARY, 0, "Library", "Biblioteca")                                        \
    X(SKIFF_TEXT_TITLE_FAVOURITES, 0, "Favourites", "Favoritos")                                   \
    X(SKIFF_TEXT_TITLE_COLLECTIONS, 0, "Collections", "Colecciones")                               \
    X(SKIFF_TEXT_TITLE_DETAILS, 0, "Details", "Detalles")                                          \
    X(SKIFF_TEXT_TITLE_QUEUE, 0, "Downloads", "Descargas")                                         \
    X(SKIFF_TEXT_TITLE_SETTINGS, 0, "Settings", "Ajustes")                                         \
    X(SKIFF_TEXT_TITLE_PAIR, 0, "Pair with RomM", "Vincular con RomM")                             \
    X(SKIFF_TEXT_TITLE_ERROR, 0, "Something went wrong", "Algo ha fallado")                        \
    X(SKIFF_TEXT_TITLE_SERVER, 0, "RomM server", "Servidor de RomM")                               \
    X(SKIFF_TEXT_TITLE_NOTICE, 0, "Notice", "Aviso")                                               \
    X(SKIFF_TEXT_TITLE_CONFIRM, 0, "Are you sure?", "¿Seguro?")                                    \
    /* Button hints. */                                                                            \
    X(SKIFF_TEXT_OK, 0, "OK", "Aceptar")                                                           \
    X(SKIFF_TEXT_CANCEL, 0, "Cancel", "Cancelar")                                                  \
    X(SKIFF_TEXT_BACK, 0, "Back", "Atrás")                                                         \
    X(SKIFF_TEXT_RETRY, 0, "Retry", "Reintentar")                                                  \
    X(SKIFF_TEXT_SELECT, 0, "Select", "Elegir")                                                    \
    X(SKIFF_TEXT_DOWNLOAD, 0, "Download", "Descargar")                                             \
    X(SKIFF_TEXT_QUIT, 0, "Quit", "Salir")                                                         \
    X(SKIFF_TEXT_SETTINGS, 0, "Settings", "Ajustes")                                               \
    /* Settings with the saves that need the player, from the library. */                          \
    X(SKIFF_TEXT_SETTINGS_WAITING, 1, "Settings ({1})", "Ajustes ({1})")                           \
    X(SKIFF_TEXT_DOWNLOADS, 0, "Downloads", "Descargas")                                           \
    X(SKIFF_TEXT_EDIT, 0, "Edit", "Editar")                                                        \
    X(SKIFF_TEXT_NEW_CODE, 0, "New code", "Código nuevo")                                          \
    X(SKIFF_TEXT_CHOOSE_NETWORK, 0, "Choose network", "Elegir red")                                \
    X(SKIFF_TEXT_CANCEL_DOWNLOAD, 0, "Cancel download", "Cancelar descarga")                       \
    X(SKIFF_TEXT_CLEAR_FINISHED, 0, "Clear finished", "Borrar terminadas")                         \
    /* Connecting. */                                                                              \
    X(SKIFF_TEXT_NET_JOINING, 1, "Connecting to Wi-Fi ({1})...",                                   \
      "Conectando a la red wifi ({1})...")                                                         \
    X(SKIFF_TEXT_NET_CONTACTING, 0, "Contacting RomM...", "Contactando con RomM...")               \
    X(SKIFF_TEXT_NET_STARTING, 0, "Starting the network...", "Iniciando la red...")                \
    X(SKIFF_TEXT_NET_WAITING_SWITCH, 0, "Turn on the Wi-Fi switch to continue",                    \
      "Activa el interruptor del wifi para continuar")                                             \
    /* Pairing. */                                                                                 \
    X(SKIFF_TEXT_PAIR_INSTRUCTIONS, 0,                                                             \
      "On your phone or computer, open:", "En el móvil o el ordenador, abre:")                     \
    X(SKIFF_TEXT_PAIR_INSTRUCTIONS_QR, 0,                                                          \
      "Scan the QR code with your phone, or open:", "Escanea el código QR con el móvil, o abre:")  \
    X(SKIFF_TEXT_PAIR_APPROVE, 0, "Sign in to RomM and approve the request.",                      \
      "Inicia sesión en RomM y aprueba la solicitud.")                                             \
    X(SKIFF_TEXT_PAIR_WAITING, 0, "Waiting for approval...", "Esperando la aprobación...")         \
    X(SKIFF_TEXT_PAIR_EXPIRED, 0, "The code has expired. Ask for a new one.",                      \
      "El código ha caducado. Pide uno nuevo.")                                                    \
    X(SKIFF_TEXT_PAIR_DENIED, 0, "The request was denied in RomM.",                                \
      "Se ha rechazado la solicitud en RomM.")                                                     \
    X(SKIFF_TEXT_PAIR_DONE, 1, "Paired with {1}", "PSP vinculada con {1}")                         \
    X(SKIFF_TEXT_PAIR_CODE, 2, "Code {1}, expires in {2}", "Código {1}, caduca en {2}")            \
    X(SKIFF_TEXT_PAIR_AGAIN, 0, "Pair again", "Volver a vincular")                                 \
    /* The server address. */                                                                      \
    X(SKIFF_TEXT_SERVER_PROMPT, 0,                                                                 \
      "Enter the address of your RomM server, starting with https://",                             \
      "Escribe la dirección de tu servidor de RomM, empezando por https://")                       \
    X(SKIFF_TEXT_SERVER_NONE, 0, "No server address yet", "Aún no hay dirección del servidor")     \
    X(SKIFF_TEXT_SERVER_CURRENT, 1, "Server: {1}", "Servidor: {1}")                                \
    X(SKIFF_TEXT_SERVER_KEYBOARD, 0, "RomM server address", "Dirección del servidor de RomM")      \
    X(SKIFF_TEXT_CONFIG_ISSUE, 3, "config.ini, line {1}: [{2}] {3}",                               \
      "config.ini, línea {1}: [{2}] {3}")                                                          \
    X(SKIFF_TEXT_CONFIG_MISSING, 2, "config.ini: [{1}] {2}", "config.ini: [{1}] {2}")              \
    /* Library and details. */                                                                     \
    X(SKIFF_TEXT_LIBRARY_LOADING, 0, "Loading...", "Cargando...")                                  \
    X(SKIFF_TEXT_LIBRARY_EMPTY, 0, "There are no PSP games in this RomM library",                  \
      "No hay juegos de PSP en esta biblioteca de RomM")                                           \
    X(SKIFF_TEXT_LIBRARY_COUNT, 1, "{1} games", "{1} juegos")                                      \
    X(SKIFF_TEXT_FAVOURITES_EMPTY, 0,                                                              \
      "No favourites yet. Mark games as favourites in RomM, then come back.",                      \
      "Aún no tienes favoritos. Márcalos en RomM y vuelve.")                                       \
    /* SELECT on the library: the list it switches to. */                                          \
    X(SKIFF_TEXT_SHOW_FAVOURITES, 0, "Favourites", "Favoritos")                                    \
    X(SKIFF_TEXT_SHOW_COLLECTIONS, 0, "Collections", "Colecciones")                                \
    X(SKIFF_TEXT_SHOW_ALL_GAMES, 0, "All games", "Todos")                                          \
    /* Collections. */                                                                             \
    X(SKIFF_TEXT_COLLECTIONS_COUNT, 1, "{1} collections", "{1} colecciones")                       \
    X(SKIFF_TEXT_COLLECTIONS_EMPTY, 0, "No collections yet. Make them in RomM, then come back.",   \
      "Aún no tienes colecciones. Créalas en RomM y vuelve.")                                      \
    X(SKIFF_TEXT_COLLECTIONS_PAIR_AGAIN, 0,                                                        \
      "Pair again to see your collections, and allow them when you approve this PSP in RomM.",     \
      "Vincula de nuevo para ver tus colecciones, y permítelas al aprobar esta PSP en RomM.")      \
    X(SKIFF_TEXT_COLLECTION_EMPTY, 0, "There are no PSP games in this collection",                 \
      "No hay juegos de PSP en esta colección")                                                    \
    X(SKIFF_TEXT_COLLECTION_UNNAMED, 0, "Untitled collection", "Colección sin nombre")             \
    /* "Download all" (the favourites, or an open collection). */                                  \
    X(SKIFF_TEXT_DOWNLOAD_ALL, 0, "Download all", "Descargar todo")                                \
    X(SKIFF_TEXT_BATCH_CHECKING, 2, "Checking favourites... {1}/{2}",                              \
      "Revisando favoritos... {1}/{2}")                                                            \
    X(SKIFF_TEXT_BATCH_CHECKING_COLLECTION, 2, "Checking the collection... {1}/{2}",               \
      "Revisando la colección... {1}/{2}")                                                         \
    X(SKIFF_TEXT_BATCH_ADDING, 0, "Adding downloads...", "Añadiendo descargas...")                 \
    X(SKIFF_TEXT_BATCH_QUESTION, 2, "Download {1} favourites ({2})?",                              \
      "¿Descargar {1} favoritos ({2})?")                                                           \
    X(SKIFF_TEXT_BATCH_QUESTION_COLLECTION, 3, "Download {1} games from {2} ({3})?",               \
      "¿Descargar {1} juegos de {2} ({3})?")                                                       \
    X(SKIFF_TEXT_BATCH_NOTHING, 0, "Nothing to download.", "No hay nada que descargar.")           \
    X(SKIFF_TEXT_BATCH_INSTALLED, 1, "{1} already installed", "{1} ya instalados")                 \
    X(SKIFF_TEXT_BATCH_QUEUED, 1, "{1} already in Downloads", "{1} ya están en Descargas")         \
    X(SKIFF_TEXT_BATCH_CHANGED, 1, "{1} changed in RomM: open each one to replace it",             \
      "{1} han cambiado en RomM: abre cada uno para sustituirlo")                                  \
    X(SKIFF_TEXT_BATCH_REFUSED, 1, "{1} can't be installed", "{1} no se pueden instalar")          \
    X(SKIFF_TEXT_BATCH_OVER_QUEUE, 2, "{1} don't fit in Downloads ({2} at most)",                  \
      "{1} no caben en Descargas ({2} como máximo)")                                               \
    X(SKIFF_TEXT_BATCH_OVER_RECORDS, 2,                                                            \
      "{1} don't fit: Skiff keeps track of {2} installed games at most",                           \
      "{1} no caben: Skiff lleva la cuenta de {2} juegos instalados como máximo")                  \
    X(SKIFF_TEXT_BATCH_OVER_SPACE, 2, "{1} don't fit on the Memory Stick ({2} free)",              \
      "{1} no caben en el Memory Stick ({2} libres)")                                              \
    X(SKIFF_TEXT_BATCH_ADDED, 1, "Added {1} downloads", "Añadidas {1} descargas")                  \
    X(SKIFF_TEXT_BATCH_ADDED_SOME, 2, "Added {1} of {2} downloads.",                               \
      "Añadidas {1} de {2} descargas.")                                                            \
    X(SKIFF_TEXT_BATCH_LEFT_FULL, 1, "{1} weren't added: Downloads is full",                       \
      "{1} no se han añadido: Descargas está lleno")                                               \
    X(SKIFF_TEXT_BATCH_LEFT_ERROR, 2, "{1} weren't added: {2}", "{1} no se han añadido: {2}")      \
    X(SKIFF_TEXT_INSTALLED, 0, "Installed", "Instalado")                                           \
    X(SKIFF_TEXT_DETAILS_SIZE, 1, "Size: {1}", "Tamaño: {1}")                                      \
    X(SKIFF_TEXT_NAME_UNSUPPORTED, 0,                                                              \
      "Skiff can't download this file: RomM names it in a way the PSP can't use",                  \
      "Skiff no puede descargar este archivo: RomM le da un nombre que la PSP no admite")          \
    X(SKIFF_TEXT_FREE_SPACE, 1, "Free space: {1}", "Espacio libre: {1}")                           \
    X(SKIFF_TEXT_INSTALL_CHANGED, 0, "Changed in RomM", "Cambiado en RomM")                        \
    X(SKIFF_TEXT_INSTALL_MULTIPLE_FILES, 0,                                                        \
      "Skiff can't download this game: it is a folder of several files",                           \
      "Skiff no puede descargar este juego: es una carpeta con varios archivos")                   \
    X(SKIFF_TEXT_INSTALL_UNKNOWN_EXTENSION, 0,                                                     \
      "Skiff can't install this file: PSP games must be .iso, .cso or .zso files",                 \
      "Skiff no puede instalar este archivo: los juegos de PSP deben ser archivos .iso, .cso o "   \
      ".zso")                                                                                      \
    X(SKIFF_TEXT_CONFIRM_REPLACE, 0,                                                               \
      "This game is already installed. Replace your installed copy?",                              \
      "Este juego ya está instalado. ¿Quieres sustituir la copia instalada?")                      \
    X(SKIFF_TEXT_CONFIRM_REPLACE_CHANGED, 0,                                                       \
      "RomM has a different version of this game. Replace your installed copy?",                   \
      "RomM tiene otra versión de este juego. ¿Quieres sustituir la copia instalada?")             \
    X(SKIFF_TEXT_INSTALLED_FULL, 0,                                                                \
      "Skiff can't keep track of more installed games. Delete some games, then open Skiff again.", \
      "Skiff no puede llevar la cuenta de más juegos instalados. Borra algunos juegos y vuelve a " \
      "abrir Skiff.")                                                                              \
    /* Downloads. */                                                                               \
    X(SKIFF_TEXT_QUEUE_EMPTY, 0, "No downloads", "No hay descargas")                               \
    X(SKIFF_TEXT_QUEUE_WAITING, 0, "Waiting", "En espera")                                         \
    X(SKIFF_TEXT_QUEUE_DOWNLOADING, 0, "Downloading", "Descargando")                               \
    X(SKIFF_TEXT_QUEUE_WAITING_WIFI, 0, "Waiting for Wi-Fi", "Esperando al wifi")                  \
    X(SKIFF_TEXT_QUEUE_CHECKING, 0, "Checking the file", "Comprobando el archivo")                 \
    X(SKIFF_TEXT_QUEUE_FAILED, 1, "Failed: {1}", "Error: {1}")                                     \
    X(SKIFF_TEXT_PROGRESS, 3, "{1} of {2} ({3}%)", "{1} de {2} ({3} %)")                           \
    X(SKIFF_TEXT_PROGRESS_RATE, 2, "{1}/s, about {2} left", "{1}/s, quedan unos {2}")              \
    X(SKIFF_TEXT_QUEUE_REJOINING, 0, "Reconnecting to Wi-Fi...", "Reconectando al wifi...")        \
    X(SKIFF_TEXT_QUEUE_RETRYING, 1, "Trying again in {1}", "Se reintentará en {1}")                \
    X(SKIFF_TEXT_QUEUE_ADDED, 1, "Added to downloads: {1}", "Añadido a las descargas: {1}")        \
    X(SKIFF_TEXT_QUEUE_FULL, 0,                                                                    \
      "The download list is full. Clear finished downloads to add more.",                          \
      "La lista de descargas está llena. Borra las descargas terminadas para añadir más.")         \
    X(SKIFF_TEXT_QUEUE_COUNT, 1, "{1} downloads", "{1} descargas")                                 \
    X(SKIFF_TEXT_CONFIRM_SERVER_CHANGE, 0,                                                         \
      "A new server cancels the current downloads, and Skiff forgets which games it installed "    \
      "(they stay on the Memory Stick). Change the server?",                                       \
      "Con otro servidor se cancelan las descargas y Skiff olvida qué juegos instaló (siguen en "  \
      "el Memory Stick). ¿Cambiar de servidor?")                                                   \
    X(SKIFF_TEXT_CONFIRM_CANCEL, 0, "Cancel this download? What it has downloaded is deleted.",    \
      "¿Cancelar esta descarga? Se borrará lo que haya descargado.")                               \
    /* Settings. */                                                                                \
    X(SKIFF_TEXT_SETTINGS_VERSION, 1, "Skiff {1}", "Skiff {1}")                                    \
    X(SKIFF_TEXT_SETTINGS_ROMM, 1, "RomM {1}", "RomM {1}")                                         \
    X(SKIFF_TEXT_STOP_FAILED, 0,                                                                   \
      "Skiff could not stop the downloads in time. Try again in a moment.",                        \
      "Skiff no ha podido detener las descargas a tiempo. Vuelve a intentarlo en un momento.")     \
    X(SKIFF_TEXT_RESTART_TO_APPLY, 0, "Saved. Quit Skiff and open it again to use it.",            \
      "Guardado. Sal de Skiff y vuelve a abrirlo para usarlo.")                                    \
    /* Notices. */                                                                                 \
    X(SKIFF_TEXT_NEWER_ROMM, 2,                                                                    \
      "RomM {1} is newer than the versions Skiff was tested with ({2}). If something doesn't "     \
      "work, "                                                                                     \
      "please report it.",                                                                         \
      "RomM {1} es más reciente que las versiones con las que se ha probado Skiff ({2}). Si algo " \
      "no funciona, avísanos.")                                                                    \
    /* Save sync. */                                                                               \
    X(SKIFF_TEXT_TITLE_SAVES, 0, "Saves", "Partidas")                                              \
    X(SKIFF_TEXT_TITLE_BACKUPS, 0, "Backups", "Copias de seguridad")                               \
    X(SKIFF_TEXT_TITLE_SAVE_CHOICE, 0, "Which save?", "¿Qué partida?")                             \
    X(SKIFF_TEXT_SYNC, 0, "Sync", "Sincronizar")                                                   \
    X(SKIFF_TEXT_SYNC_ALL, 0, "Sync all", "Sincronizar todo")                                      \
    X(SKIFF_TEXT_BACKUPS, 0, "Backups", "Copias")                                                  \
    X(SKIFF_TEXT_SYNC_SAVE, 0, "Sync save", "Sincronizar partida")                                 \
    X(SKIFF_TEXT_RESTORE, 0, "Restore", "Restaurar")                                               \
    X(SKIFF_TEXT_KEEP_THIS_PSP, 0, "Keep this PSP's", "Usar la de la PSP")                         \
    X(SKIFF_TEXT_USE_ROMM, 0, "Use RomM's", "Usar la de RomM")                                     \
    X(SKIFF_TEXT_LATER, 0, "Later", "Más tarde")                                                   \
    X(SKIFF_TEXT_DOWNLOAD_AGAIN, 0, "Download again", "Descargar de nuevo")                        \
    X(SKIFF_TEXT_LEAVE_IT, 0, "Leave it", "Dejarla así")                                           \
    X(SKIFF_TEXT_SAVES_PAIR_AGAIN, 0,                                                              \
      "Pair again to sync saves with RomM, and allow save sync when you approve this PSP in "      \
      "RomM.",                                                                                     \
      "Vincula de nuevo para sincronizar las partidas con RomM, y permite sincronizarlas al "      \
      "aprobar esta PSP en RomM.")                                                                 \
    X(SKIFF_TEXT_SAVES_PAIR_NOTE, 0, "Saves: pair again to sync them",                             \
      "Partidas: vincula de nuevo para sincronizarlas")                                            \
    X(SKIFF_TEXT_SAVES_OFFLINE, 0, "Saves sync once Skiff is connected to RomM.",                  \
      "Las partidas se sincronizan cuando Skiff está conectado a RomM.")                           \
    X(SKIFF_TEXT_SAVES_CHECKING, 0, "Checking saves...", "Comprobando partidas...")                \
    X(SKIFF_TEXT_SAVES_SYNCING, 1, "Syncing: {1}", "Sincronizando: {1}")                           \
    X(SKIFF_TEXT_SAVES_SUMMARY, 2, "Saves: {1} synced, {2} need you",                              \
      "Partidas: {1} sincronizadas, {2} te necesitan")                                             \
    /* Settings' Saves row while games need the player: one, or more. */                           \
    X(SKIFF_TEXT_SAVES_WAITING_ONE, 1, "{1} needs you", "{1} te necesita")                         \
    X(SKIFF_TEXT_SAVES_WAITING, 1, "{1} need you", "{1} te necesitan")                             \
    X(SKIFF_TEXT_SAVES_EMPTY, 0, "No saves on this PSP or in RomM yet.",                           \
      "Aún no hay partidas en esta PSP ni en RomM.")                                               \
    X(SKIFF_TEXT_SAVES_NOT_CHECKED, 0, "Choose Sync all to compare this PSP's saves with RomM's.", \
      "Elige Sincronizar todo para comparar las partidas de esta PSP con las de RomM.")            \
    X(SKIFF_TEXT_SAVE_SYNCED, 0, "Synced", "Sincronizada")                                         \
    X(SKIFF_TEXT_SAVE_SYNCED_ON, 1, "Synced {1}", "Sincronizada el {1}")                           \
    X(SKIFF_TEXT_SAVE_CHANGED_HERE, 0, "Changed on this PSP", "Cambiada en esta PSP")              \
    X(SKIFF_TEXT_SAVE_NEWER_IN_ROMM, 0, "Newer in RomM", "Más reciente en RomM")                   \
    X(SKIFF_TEXT_SAVE_CHANGED_BOTH, 0, "Changed on both", "Cambiada en ambas")                     \
    X(SKIFF_TEXT_SAVE_DIFFERENT, 0, "Differs from RomM's", "Distinta de la de RomM")               \
    X(SKIFF_TEXT_SAVE_ONLY_IN_ROMM, 0, "Only in RomM", "Solo en RomM")                             \
    X(SKIFF_TEXT_SAVE_DELETED_HERE, 0, "Deleted on this PSP", "Borrada en esta PSP")               \
    X(SKIFF_TEXT_SAVE_DELETED_IN_ROMM, 0, "Deleted in RomM", "Borrada en RomM")                    \
    X(SKIFF_TEXT_SAVE_CANT_SYNC, 0, "Can't sync", "No se sincroniza")                              \
    X(SKIFF_TEXT_SAVE_NO_ID, 0,                                                                    \
      "Can't sync: RomM has no save ID for this game. With RomM 5.4 or newer, rescan the "         \
      "platform.",                                                                                 \
      "No se puede sincronizar: RomM no tiene el identificador de partidas de este juego. Con "    \
      "RomM 5.4 o posterior, vuelve a escanear la plataforma.")                                    \
    X(SKIFF_TEXT_SAVE_NO_ID_SHORT, 0, "no save ID in RomM", "sin identificador en RomM")           \
    X(SKIFF_TEXT_SAVE_UNKNOWN_GAME, 0,                                                             \
      "Can't sync: Skiff does not know which game in RomM uses this save. Open the game in the "   \
      "library and choose Sync save.",                                                             \
      "No se puede sincronizar: Skiff no sabe qué juego de RomM usa esta partida. Abre el juego "  \
      "en la biblioteca y elige Sincronizar partida.")                                             \
    X(SKIFF_TEXT_SAVE_SEVERAL_GAMES, 0,                                                            \
      "Several games in RomM use this save. Open the one you play in the library and choose Sync " \
      "save.",                                                                                     \
      "Varios juegos de RomM usan esta partida. Abre en la biblioteca el que juegas y elige "      \
      "Sincronizar partida.")                                                                      \
    X(SKIFF_TEXT_SAVE_ERROR, 1, "Can't sync: {1}", "No se puede sincronizar: {1}")                 \
    X(SKIFF_TEXT_SAVE_LAST_SYNCED, 1, "Last synced {1}", "Última sincronización: {1}")             \
    /* Which save?: what happened, both sides, and what each button does. */                       \
    X(SKIFF_TEXT_SAVE_CONFLICT, 0, "Changed on both since the last sync.",                         \
      "Cambiada en ambas desde la última sincronización.")                                         \
    X(SKIFF_TEXT_SAVE_CONFLICT_FIRST, 0, "The two saves differ and have never synced.",            \
      "Las dos partidas son distintas y nunca se han sincronizado.")                               \
    X(SKIFF_TEXT_SAVE_DELETED_SERVER_CHOICE, 0, "Deleted in RomM since the last sync.",            \
      "Borrada en RomM desde la última sincronización.")                                           \
    X(SKIFF_TEXT_SAVE_DELETED_LOCAL_CHOICE, 0, "Deleted on this PSP since the last sync.",         \
      "Borrada en esta PSP desde la última sincronización.")                                       \
    X(SKIFF_TEXT_SIDE_THIS_PSP, 0, "This PSP", "Esta PSP")                                         \
    X(SKIFF_TEXT_SIDE_ROMM, 0, "RomM", "RomM")                                                     \
    X(SKIFF_TEXT_SAVE_GONE, 0, "Deleted", "Borrada")                                               \
    X(SKIFF_TEXT_KEEP_THIS_PSP_DETAIL, 0, "RomM's stays in its history",                           \
      "La de RomM queda en su historial")                                                          \
    X(SKIFF_TEXT_USE_ROMM_DETAIL, 0, "This PSP's goes to Backups", "La de la PSP pasa a Copias")   \
    X(SKIFF_TEXT_LATER_DETAIL, 0, "Nothing changes", "No cambia nada")                             \
    X(SKIFF_TEXT_DOWNLOAD_AGAIN_DETAIL, 0, "Puts RomM's back on this PSP",                         \
      "Vuelve a traer la de RomM")                                                                 \
    X(SKIFF_TEXT_LEAVE_IT_DETAIL, 0, "RomM keeps its copy", "RomM conserva su copia")              \
    X(SKIFF_TEXT_UPLOAD_AGAIN_DETAIL, 0, "Uploads it to RomM again", "Se vuelve a subir a RomM")   \
    X(SKIFF_TEXT_REMOVE_THIS_PSP, 0, "Remove this PSP's", "Borrar la de la PSP")                   \
    X(SKIFF_TEXT_REMOVE_THIS_PSP_DETAIL, 0, "Deleted here, after a backup",                        \
      "Se borra aquí, tras una copia")                                                             \
    X(SKIFF_TEXT_DETAILS_SAVE, 1, "Save: {1}", "Partida: {1}")                                     \
    X(SKIFF_TEXT_BACKUPS_EMPTY, 0, "No backups of this game's saves yet.",                         \
      "Aún no hay copias de las partidas de este juego.")                                          \
    X(SKIFF_TEXT_BACKUP_ROW, 1, "Backup {1}", "Copia {1}")                                         \
    X(SKIFF_TEXT_CONFIRM_RESTORE_BACKUP, 0,                                                        \
      "Restore this backup? The saves on this PSP are backed up first.",                           \
      "¿Restaurar esta copia? Antes se hace una copia de las partidas de esta PSP.")               \
    X(SKIFF_TEXT_BACKUP_RESTORED, 0, "Backup restored", "Copia restaurada")                        \
    X(SKIFF_TEXT_SAVE_SYNCED_NOTE, 1, "Synced: {1}", "Sincronizada: {1}")                          \
    X(SKIFF_TEXT_SAVE_IN_SYNC_NOTE, 1, "Already in sync: {1}", "Ya sincronizada: {1}")             \
    /* Games sent from RomM's web UI (Install on device): Settings' row and its states. */         \
    X(SKIFF_TEXT_SENDS_ROW, 1, "Games from RomM: {1}", "Juegos desde RomM: {1}")                   \
    X(SKIFF_TEXT_SENDS_ON, 0, "On", "Activado")                                                    \
    X(SKIFF_TEXT_SENDS_OFF, 0, "Off", "Desactivado")                                               \
    X(SKIFF_TEXT_SENDS_PAIR_AGAIN, 0, "Pair again to allow it",                                    \
      "Vincula de nuevo para permitirlo")                                                          \
    X(SKIFF_TEXT_SENDS_NEEDS_UPDATE, 0, "Needs RomM 5.4", "Requiere RomM 5.4")                     \
    X(SKIFF_TEXT_SENDS_TURNED_OFF, 0, "Turned off in RomM", "Desactivado en RomM")                 \
    /* The header after games sent from RomM were added, or some refused. */                       \
    X(SKIFF_TEXT_SENDS_ADDED, 1, "From RomM: {1} added to Downloads",                              \
      "Desde RomM: {1} añadidos a Descargas")                                                      \
    X(SKIFF_TEXT_SENDS_ADDED_SOME, 2, "From RomM: {1} added, {2} not added (see RomM)",            \
      "Desde RomM: {1} añadidos, {2} no añadidos (mira RomM)")                                     \
    /* Why a game sent from RomM was not added, as RomM shows it to the player. */                 \
    X(SKIFF_TEXT_SEND_CHANGED, 0,                                                                  \
      "A different copy is installed on this PSP: open the game in Skiff to replace it",           \
      "Hay otra copia instalada en esta PSP: abre el juego en Skiff para sustituirla")             \
    X(SKIFF_TEXT_SEND_FAILED_DOWNLOAD, 0, "It is in Downloads with an error: retry it there",      \
      "Está en Descargas con un error: reinténtalo allí")                                          \
    X(SKIFF_TEXT_INSTALL_EMPTY_FILE, 0, "Skiff can't install this file: it is empty",              \
      "Skiff no puede instalar este archivo: está vacío")                                          \
    /* Short month names, in order, separated by blanks. */                                        \
    X(SKIFF_TEXT_MONTHS, 0, "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec",                     \
      "ene feb mar abr may jun jul ago sep oct nov dic")                                           \
    X(SKIFF_TEXT_ERROR_CODE, 2, "{1} [{2}]", "{1} [{2}]")

typedef enum skiff_text_id {
#define SKIFF_TEXT_ENUM(id, placeholders, english, spanish) id,
    SKIFF_TEXT_TABLE(SKIFF_TEXT_ENUM)
#undef SKIFF_TEXT_ENUM
        SKIFF_TEXT_COUNT
} skiff_text_id;

/* SKIFF_LANGUAGE_SPANISH for SKIFF_PSP_SYSTEM_LANGUAGE_SPANISH, English for every other value. */
skiff_language skiff_language_from_psp(int system_language);

/* The text in language; English for a language outside the table; "" for an id outside it. */
const char *skiff_text(skiff_language language, skiff_text_id id);

/* Stable name such as "SKIFF_TEXT_TITLE_LIBRARY", for tests and logs; NULL outside the table. */
const char *skiff_text_name(skiff_text_id id);

/* How many placeholders the text takes; -1 outside the table. */
int skiff_text_placeholders(skiff_text_id id);

/* The sentence for err in language: error.h's message in English; English for codes outside the
 * table or a language outside it. */
const char *skiff_error_text(skiff_language language, skiff_err err);

/*
 * Fills template's {1}..{9} with args[0..count-1] into out. A brace not followed by a digit 1-9 and
 * '}' is copied as is; {n} past count expands to nothing. Returns SKIFF_ERR_INVALID_ARG for a NULL
 * template or out, a zero out_size, or NULL args with a non-zero count (out is empty when out_size
 * allows); SKIFF_ERR_BUFFER_TOO_SMALL when the result does not fit, with out holding as much as
 * fits, cut between UTF-8 characters.
 */
skiff_err skiff_text_format(const char *template_text, const char *const *args, size_t count,
                            char *out, size_t out_size);

/* "<sentence> [<code>]", what the player sees for an error and quotes in a bug report. */
skiff_err skiff_error_line(skiff_language language, skiff_err err, char *out, size_t out_size);

#endif
