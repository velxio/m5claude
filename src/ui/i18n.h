// UI strings. Add a language by adding a Strings entry and a Lang value.
// Keep translations short: the screen is 320 px wide and most labels share a row.
#pragma once
#include <string.h>

namespace ui {

enum Lang : unsigned char { LANG_EN, LANG_ES, LANG_COUNT };

struct Strings {
  const char* lang_name;  // in its own language
  // Header and states
  const char *claude, *usage;
  const char *waiting_title, *waiting_1, *waiting_2, *hint_settings;
  const char *no_accounts, *no_accounts_hint, *error, *unavailable;
  // Footer
  const char *details, *tokens, *breakdown, *home, *change;
  // Limits
  const char *week, *weekly, *session_5h, *scoped_fmt, *resets, *resets_fmt, *in_fmt;
  const char *resets_in_fmt, *Resets_in_fmt, *reset_waiting, *Reset_waiting, *not_on_plan;
  const char *limits_fmt, *limits_retry_fmt;
  // Values and tokens
  const char *today_up, *seven_days, *tokens_today, *tokens_7d, *tokens_7days, *top_model;
  const char *spend_7d, *cost_7d, *api_value, *api_value_7d, *api_value_today, *at_list_prices;
  const char *today_cost_fmt, *n_tokens_fmt, *value_fmt, *today_tokens_fmt, *today_tokens_value_fmt;
  const char *tokens_per_day, *avg_fmt, *models, *no_data;
  const char *input, *output, *cache_read, *cache_write;
  const char *today_at_fmt;
  const char* wday[7];
  const char* wday_letter[7];
  // Pairing and toasts
  const char *new_computer, *allow_question, *approve_hint, *deny, *allow;
  const char *paired, *pc_not_connected, *pcs_cleared, *refreshing, *data_updated;
  // Alerts
  const char *alert_fmt, *reset_fmt, *kind_session, *kind_weekly;
  // Settings
  const char *settings, *set_language, *set_brightness, *set_sounds, *set_alert_at;
  const char *set_screen_off, *set_night, *set_paired, *set_about, *set_close;
  const char* sounds[3];
  const char* screen_off[3];
  const char *off, *on, *paired_fmt;
  const char *forget_title, *forget_subject_fmt, *forget_question;
  // Bridge errors, translated by tr_error()
  const char *err_login_expired, *err_rate_limited, *err_not_logged_in, *err_offline;
};

static const Strings kStrings[LANG_COUNT] = {
    {
        "English",
        "Claude", "Usage",
        "Waiting for your PC", "Run m5claude-bridge on your computer.",
        "It connects over USB or Bluetooth automatically.", "Hold C for settings",
        "No accounts configured", "Log in with Claude Code, or add an API key in the bridge",
        "Error", "unavailable",
        "Details", "Tokens", "Breakdown", "Home", "Change",
        "week", "WEEKLY", "SESSION · 5H", "%s · 7D", "Resets", "Resets %s", "in %s",
        "resets in %s", "Resets in %s", "reset, waiting for update", "Reset, waiting for update",
        "Not on this plan",
        "Limits: %s", "Limits: %s, retrying",
        "TODAY", "7 DAYS", "TOKENS TODAY", "TOKENS · 7D", "TOKENS · 7 DAYS", "TOP MODEL",
        "SPEND · 7 DAYS", "COST · 7D", "API VALUE", "API VALUE · 7D", "API VALUE · TODAY",
        "at API list prices",
        "Today  $%.2f", "%s tokens", "$%.2f value", "Today %s tokens", "Today %s · $%.2f value",
        "Tokens per day", "avg %s", "MODELS", "No data",
        "Input", "Output", "Cache read", "Cache write",
        "today %02d:%02d",
        {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
        {"S", "M", "T", "W", "T", "F", "S"},
        "New computer", "Allow it to send usage data?", "Approve only computers you own.",
        "Deny", "Allow",
        "Computer paired", "PC not connected", "Paired computers cleared", "Refreshing…",
        "Data updated",
        "%s: %s at %d%%", "%s: %s reset", "session", "weekly limit",
        "Settings", "Language", "Brightness", "Sounds", "Alert at",
        "Screen off on battery", "Dim at night (22–07)", "Paired computers", "About", "Close",
        {"Off", "Alerts", "Alerts + clicks"},
        {"Never", "After 1 min", "After 5 min"},
        "Off", "On", "%d · Forget",
        "Forget computers", "%d approved", "They will need approval again.",
        "Login expired", "Rate limited", "Not logged in", "Offline",
    },
    {
        "Español",
        "Claude", "Uso",
        "Esperando a tu PC", "Abre m5claude-bridge en tu computadora.",
        "Se conecta solo, por USB o Bluetooth.", "Mantén C para ajustes",
        "Sin cuentas configuradas", "Inicia sesión en Claude Code o agrega una API key",
        "Error", "no disponibles",
        "Detalle", "Tokens", "Desglose", "Inicio", "Cambiar",
        "semana", "SEMANAL", "SESIÓN · 5H", "%s · 7D", "Reinicia", "Reinicia %s", "en %s",
        "reinicia en %s", "Reinicia en %s", "reiniciado, esperando datos",
        "Reiniciado, esperando datos", "No incluido en el plan",
        "Límites: %s", "Límites: %s, reintentando",
        "HOY", "7 DÍAS", "TOKENS HOY", "TOKENS · 7D", "TOKENS · 7 DÍAS", "MODELO PRINCIPAL",
        "GASTO · 7 DÍAS", "COSTO · 7D", "VALOR API", "VALOR API · 7D", "VALOR API · HOY",
        "a precios de la API",
        "Hoy  $%.2f", "%s tokens", "valor $%.2f", "Hoy %s tokens", "Hoy %s · valor $%.2f",
        "Tokens por día", "prom. %s", "MODELOS", "Sin datos",
        "Entrada", "Salida", "Caché lectura", "Caché escritura",
        "hoy %02d:%02d",
        {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"},
        {"D", "L", "M", "X", "J", "V", "S"},
        "Nueva computadora", "¿Permitir que envíe datos de uso?", "Aprueba solo computadoras tuyas.",
        "Rechazar", "Permitir",
        "Computadora emparejada", "PC no conectada", "Computadoras olvidadas", "Actualizando…",
        "Datos actualizados",
        "%s: %s al %d%%", "%s: %s reiniciado", "sesión", "límite semanal",
        "Ajustes", "Idioma", "Brillo", "Sonidos", "Alertar al",
        "Apagar pantalla (batería)", "Atenuar de noche (22–07)", "Computadoras", "Acerca de",
        "Cerrar",
        {"No", "Alertas", "Alertas + clics"},
        {"Nunca", "Tras 1 min", "Tras 5 min"},
        "No", "Sí", "%d · Olvidar",
        "Olvidar computadoras", "%d aprobadas", "Tendrán que aprobarse otra vez.",
        "Sesión expirada", "Demasiadas consultas", "Sin sesión", "Sin conexión",
    },
};

// Errors arrive from the bridge in English; show the known ones in the UI language.
inline const char* tr_error(const Strings& t, const char* e) {
  if (!e || !e[0]) return t.error;
  if (!strcmp(e, "Login expired")) return t.err_login_expired;
  if (!strcmp(e, "Rate limited")) return t.err_rate_limited;
  if (!strcmp(e, "Not logged in")) return t.err_not_logged_in;
  if (!strcmp(e, "Offline")) return t.err_offline;
  return e;
}

}  // namespace ui
