#include "agent_status_parse.h"
#include "app_tokens_config.h"
#include "interaction_relay_crypto.h"
#include "needs_you_send_policy.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../third_party/cjson/cJSON.h"

/* V2-kontraktet behöver bara djup 5. Extra metadata får gott om marginal,
 * men rekursionen i både cJSON och råtoken-vandringen hålls ESP-säker. */
#define TK_AGENT_JSON_MAX_DEPTH 16U
#define TK_PENDING_CANONICAL_VIEW_CAP 1024U

typedef struct {
  const char *name;
  tk_agent_state value;
} state_entry;

typedef struct {
  const char *name;
  tk_agent_activity value;
} activity_entry;

static const state_entry state_entries[] = {
    {"idle", TK_AGENT_IDLE},
    {"working", TK_AGENT_WORKING},
    {"waiting", TK_AGENT_WAITING},
    {"done", TK_AGENT_DONE},
    {"error", TK_AGENT_ERROR},
    {"unknown", TK_AGENT_UNKNOWN},
};

static const activity_entry activity_entries[] = {
    {"thinking", TK_ACTIVITY_THINKING},
    {"reading", TK_ACTIVITY_READING},
    {"editing", TK_ACTIVITY_EDITING},
    {"searching", TK_ACTIVITY_SEARCHING},
    {"running", TK_ACTIVITY_RUNNING},
    {"testing", TK_ACTIVITY_TESTING},
    {"building", TK_ACTIVITY_BUILDING},
    {"waiting_input", TK_ACTIVITY_WAITING_INPUT},
    {"waiting_approval", TK_ACTIVITY_WAITING_APPROVAL},
};

static const char *const root_required_keys[] = {
    "v", "seq", "agents", NULL,
};

static const char *const agents_required_keys[] = {
    "claude", "codex", NULL,
};

static const char *const provider_required_keys[] = {
    "active_count", "jobs", NULL,
};

static const char *const job_required_keys[] = {
    "task_id", "event_id", "state", "project", "activity", "updated_ms",
    NULL,
};

static const char *const job_allowed_keys[] = {
    "task_id", "event_id", "state", "project", "activity", "updated_ms",
    "model", "effort", NULL,
};

static const char *const pending_v2_allowed_keys[] = {
    "provider", "request_id", "view_sha256", "kind", "project",
    "expires_in_ms", "hold_ms", "options_total", "marked", "prompt",
    "title", "subtitle", "tool", "can_approve", NULL,
};

static bool json_whitespace(unsigned char byte) {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static bool json_digit(unsigned char byte) {
  return byte >= '0' && byte <= '9';
}

static bool json_number_valid(const char *json, size_t len, size_t *offset) {
  size_t cursor = *offset;

  if (json[cursor] == '-') cursor++;
  if (cursor >= len) return false;

  if (json[cursor] == '0') {
    cursor++;
    if (cursor < len && json_digit((unsigned char)json[cursor])) return false;
  } else if (json[cursor] >= '1' && json[cursor] <= '9') {
    do {
      cursor++;
    } while (cursor < len && json_digit((unsigned char)json[cursor]));
  } else {
    return false;
  }

  if (cursor < len && json[cursor] == '.') {
    cursor++;
    if (cursor >= len || !json_digit((unsigned char)json[cursor])) return false;
    do {
      cursor++;
    } while (cursor < len && json_digit((unsigned char)json[cursor]));
  }

  if (cursor < len && (json[cursor] == 'e' || json[cursor] == 'E')) {
    cursor++;
    if (cursor < len && (json[cursor] == '+' || json[cursor] == '-')) cursor++;
    if (cursor >= len || !json_digit((unsigned char)json[cursor])) return false;
    do {
      cursor++;
    } while (cursor < len && json_digit((unsigned char)json[cursor]));
  }

  *offset = cursor - 1;
  return true;
}

/* cJSON intentionally accepts any raw byte <= 0x20 as whitespace and keeps
 * decoded NUL bytes in C strings without exposing their decoded length. Its
 * number scanner also accepts leading zeroes. Tighten those edges before
 * reading the tree. */
static bool json_lexically_valid(const char *json, size_t len) {
  bool in_string = false;
  bool escaped = false;
  size_t depth = 0;

  for (size_t i = 0; i < len; i++) {
    unsigned char byte = (unsigned char)json[i];

    if (!in_string) {
      if (byte == '"') {
        in_string = true;
      } else if (byte < 0x20 && !json_whitespace(byte)) {
        return false;
      } else if ((byte == '-') || json_digit(byte)) {
        if (!json_number_valid(json, len, &i)) return false;
      } else if (byte == '{' || byte == '[') {
        if (depth >= TK_AGENT_JSON_MAX_DEPTH) return false;
        depth++;
      } else if (byte == '}' || byte == ']') {
        if (depth == 0) return false;
        depth--;
      }
      continue;
    }

    if (escaped) {
      escaped = false;
      if (byte == 'u' && i + 4 < len && json[i + 1] == '0' &&
          json[i + 2] == '0' && json[i + 3] == '0' && json[i + 4] == '0') {
        return false;
      }
    } else if (byte == '\\') {
      escaped = true;
    } else if (byte == '"') {
      in_string = false;
    } else if (byte < 0x20) {
      return false;
    }
  }

  return depth == 0;
}

static bool trailing_is_whitespace(const char *json, size_t len,
                                   const char *parse_end) {
  if (!parse_end || parse_end < json || parse_end > json + len) return false;
  for (const char *cursor = parse_end; cursor < json + len; cursor++) {
    if (!json_whitespace((unsigned char)*cursor)) return false;
  }
  return true;
}

typedef struct {
  const char *json;
  size_t len;
  size_t offset;
} raw_cursor;

static void raw_skip_whitespace(raw_cursor *cursor) {
  while (cursor->offset < cursor->len &&
         json_whitespace((unsigned char)cursor->json[cursor->offset])) {
    cursor->offset++;
  }
}

static bool raw_take(raw_cursor *cursor, char expected) {
  raw_skip_whitespace(cursor);
  if (cursor->offset >= cursor->len ||
      cursor->json[cursor->offset] != expected) {
    return false;
  }
  cursor->offset++;
  return true;
}

static bool raw_skip_string(raw_cursor *cursor) {
  raw_skip_whitespace(cursor);
  if (cursor->offset >= cursor->len || cursor->json[cursor->offset] != '"') {
    return false;
  }
  cursor->offset++;

  while (cursor->offset < cursor->len) {
    char byte = cursor->json[cursor->offset++];
    if (byte == '"') return true;
    if (byte == '\\') {
      if (cursor->offset >= cursor->len) return false;
      cursor->offset++;
    }
  }
  return false;
}

static bool raw_skip_literal(raw_cursor *cursor, const char *literal,
                             size_t literal_len) {
  raw_skip_whitespace(cursor);
  if (literal_len > cursor->len - cursor->offset ||
      memcmp(cursor->json + cursor->offset, literal, literal_len) != 0) {
    return false;
  }
  cursor->offset += literal_len;
  return true;
}

static bool raw_exact_uint32(raw_cursor *cursor, uint32_t *out) {
  raw_skip_whitespace(cursor);
  if (cursor->offset >= cursor->len ||
      !json_digit((unsigned char)cursor->json[cursor->offset])) {
    return false;
  }

  uint32_t value = 0;
  do {
    uint32_t digit = (uint32_t)(cursor->json[cursor->offset] - '0');
    if (value > (UINT32_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
    cursor->offset++;
  } while (cursor->offset < cursor->len &&
           json_digit((unsigned char)cursor->json[cursor->offset]));

  if (cursor->offset < cursor->len &&
      (cursor->json[cursor->offset] == '.' ||
       cursor->json[cursor->offset] == 'e' ||
       cursor->json[cursor->offset] == 'E')) {
    return false;
  }

  *out = value;
  return true;
}

/* cJSON-nodens pekaridentitet väljer rätt medlem även när samma nyckelnamn
 * finns i ett annat objekt. Samtidig gång genom råtext och träd bevarar det
 * ursprungliga nummertoken som cJSON:s double annars hade avrundat. */
static bool raw_find_item(raw_cursor *cursor, const cJSON *item,
                          const cJSON *target, uint32_t *out, bool *found) {
  raw_skip_whitespace(cursor);

  if (item == target) {
    if (!raw_exact_uint32(cursor, out)) return false;
    *found = true;
    return true;
  }

  if (cJSON_IsObject(item)) {
    if (!raw_take(cursor, '{')) return false;
    const cJSON *child = item->child;
    if (!child) return raw_take(cursor, '}');

    while (child) {
      if (!raw_skip_string(cursor) || !raw_take(cursor, ':') ||
          !raw_find_item(cursor, child, target, out, found)) {
        return false;
      }
      child = child->next;
      if (child) {
        if (!raw_take(cursor, ',')) return false;
      } else if (!raw_take(cursor, '}')) {
        return false;
      }
    }
    return true;
  }

  if (cJSON_IsArray(item)) {
    if (!raw_take(cursor, '[')) return false;
    const cJSON *child = item->child;
    if (!child) return raw_take(cursor, ']');

    while (child) {
      if (!raw_find_item(cursor, child, target, out, found)) return false;
      child = child->next;
      if (child) {
        if (!raw_take(cursor, ',')) return false;
      } else if (!raw_take(cursor, ']')) {
        return false;
      }
    }
    return true;
  }

  if (cJSON_IsString(item)) return raw_skip_string(cursor);
  if (cJSON_IsNull(item)) return raw_skip_literal(cursor, "null", 4);
  if (cJSON_IsTrue(item)) return raw_skip_literal(cursor, "true", 4);
  if (cJSON_IsFalse(item)) return raw_skip_literal(cursor, "false", 5);
  if (cJSON_IsNumber(item)) {
    if (cursor->offset >= cursor->len) return false;
    size_t number_offset = cursor->offset;
    if (!json_number_valid(cursor->json, cursor->len, &number_offset)) {
      return false;
    }
    cursor->offset = number_offset + 1;
    return true;
  }

  return false;
}

static bool raw_uint32_for_item(const char *json, size_t len,
                                const cJSON *root, const cJSON *target,
                                uint32_t *out) {
  raw_cursor cursor = {.json = json, .len = len, .offset = 0};
  if (len >= 3 && memcmp(json, "\xEF\xBB\xBF", 3) == 0) cursor.offset = 3;

  bool found = false;
  return raw_find_item(&cursor, root, target, out, &found) && found;
}

static bool uint32_member(const char *json, size_t len, const cJSON *root,
                          const cJSON *object, const char *key,
                          uint32_t *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item)) return false;
  return raw_uint32_for_item(json, len, root, item, out);
}

/* cJSON lagrar avkodade egenskapsnamn i child->string, så även t.ex.
 * "\\u0076" räknas som v. Bara kontraktsnycklar måste vara unika; okända
 * extranycklar lämnas orörda. */
static bool required_keys_once(const cJSON *object,
                               const char *const *required_keys) {
  if (!cJSON_IsObject(object)) return false;

  for (size_t i = 0; required_keys[i]; i++) {
    size_t matches = 0;
    for (const cJSON *child = object->child; child; child = child->next) {
      if (child->string && strcmp(child->string, required_keys[i]) == 0) {
        matches++;
        if (matches > 1) return false;
      }
    }
    if (matches != 1) return false;
  }

  return true;
}

static bool allowed_keys_once(const cJSON *object,
                              const char *const *allowed_keys) {
  if (!cJSON_IsObject(object)) return false;
  for (const cJSON *child = object->child; child; child = child->next) {
    if (!child->string) return false;
    bool allowed = false;
    for (size_t i = 0; allowed_keys[i]; i++) {
      if (strcmp(child->string, allowed_keys[i]) == 0) {
        allowed = true;
        break;
      }
    }
    if (!allowed) return false;
    for (const cJSON *earlier = object->child;
         earlier && earlier != child; earlier = earlier->next) {
      if (earlier->string && strcmp(earlier->string, child->string) == 0) {
        return false;
      }
    }
  }
  return true;
}

static bool nullable_string_member(const cJSON *object, const char *key,
                                   char *destination, size_t capacity) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!item) return false;
  if (cJSON_IsNull(item)) return true;
  if (!cJSON_IsString(item) || !item->valuestring) return false;

  const unsigned char *source = (const unsigned char *)item->valuestring;
  size_t length = 0;
  while (source[length] != '\0') {
    if (source[length] < 0x20 || length + 1 >= capacity) return false;
    length++;
  }

  memcpy(destination, source, length + 1);
  return true;
}

static bool optional_nullable_string_member(const cJSON *object,
                                            const char *key,
                                            char *destination,
                                            size_t capacity,
                                            bool *has_value) {
  size_t matches = 0;
  const cJSON *item = NULL;
  for (const cJSON *child = object->child; child; child = child->next) {
    if (child->string && strcmp(child->string, key) == 0) {
      item = child;
      matches++;
      if (matches > 1) return false;
    }
  }

  *has_value = false;
  destination[0] = '\0';
  if (!item || cJSON_IsNull(item)) return true;
  if (!cJSON_IsString(item) || !item->valuestring) return false;

  const unsigned char *source = (const unsigned char *)item->valuestring;
  size_t length = 0;
  while (source[length] != '\0') {
    if (source[length] < 0x20 || length + 1 >= capacity) return false;
    length++;
  }

  memcpy(destination, source, length + 1);
  *has_value = true;
  return true;
}

static bool state_member(const cJSON *object, tk_agent_state *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "state");
  if (!cJSON_IsString(item) || !item->valuestring) return false;

  for (size_t i = 0; i < sizeof state_entries / sizeof state_entries[0]; i++) {
    if (strcmp(item->valuestring, state_entries[i].name) == 0) {
      *out = state_entries[i].value;
      return true;
    }
  }

  *out = TK_AGENT_UNKNOWN;
  return true;
}

static bool activity_member(const cJSON *object, tk_agent_activity *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "activity");
  if (!item) return false;
  if (cJSON_IsNull(item)) {
    *out = TK_ACTIVITY_NONE;
    return true;
  }
  if (!cJSON_IsString(item) || !item->valuestring) return false;

  for (size_t i = 0;
       i < sizeof activity_entries / sizeof activity_entries[0]; i++) {
    if (strcmp(item->valuestring, activity_entries[i].name) == 0) {
      *out = activity_entries[i].value;
      return true;
    }
  }

  *out = TK_ACTIVITY_UNKNOWN;
  return true;
}

/* Optional display string: absent, null or unusable leaves the field empty and
 * its has_* flag false. Never fails the parse — see parse_pending. */
static void optional_pending_string(const cJSON *object, const char *key,
                                    char *destination, size_t capacity,
                                    bool *has_value) {
  *has_value = false;
  destination[0] = '\0';
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || !item->valuestring) return;

  const unsigned char *source = (const unsigned char *)item->valuestring;
  size_t length = 0;
  while (source[length] != '\0') {
    if (source[length] < 0x20 || length + 1 >= capacity) return;
    length++;
  }
  if (!length) return;
  memcpy(destination, source, length + 1);
  *has_value = true;
}

static bool pending_bool(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsTrue(item);
}

/* Security-bearing pending strings are not display fields: duplicates, null,
 * controls and truncation all invalidate the optional interaction. */
static bool pending_contract_string(const cJSON *object, const char *key,
                                    char *destination, size_t capacity,
                                    bool *present) {
  size_t matches = 0;
  const cJSON *item = NULL;
  for (const cJSON *child = object->child; child; child = child->next) {
    if (child->string && strcmp(child->string, key) == 0) {
      item = child;
      matches++;
    }
  }
  *present = false;
  destination[0] = '\0';
  if (matches == 0) return true;
  if (matches != 1 || !cJSON_IsString(item) || !item->valuestring) return false;

  const unsigned char *source = (const unsigned char *)item->valuestring;
  size_t length = 0;
  while (source[length] != '\0') {
    if (source[length] < 0x20 || length + 1 >= capacity) return false;
    length++;
  }
  if (length == 0) return false;
  memcpy(destination, source, length + 1);
  *present = true;
  return true;
}

static bool lowercase_sha256(const char *value) {
  if (!value || strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; i++) {
    char byte = value[i];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool pending_request_id_valid(const char *value) {
  if (!value) return false;
  size_t length = strlen(value);
  if (length == 0 || length >= TK_PENDING_ID_CAP) return false;
  for (size_t i = 0; i < length; i++) {
    char byte = value[i];
    if (!((byte >= 'a' && byte <= 'z') ||
          (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '_' || byte == '-')) {
      return false;
    }
  }
  return true;
}

/* cJSON returns decoded UTF-8 bytes. Validate their shortest-form structure
 * before copying them into LVGL-facing strings. Python's producer has already
 * rejected Unicode controls/format characters; the device independently
 * rejects the common control/format ranges too. */
static bool pending_utf8_valid(const char *value) {
  const unsigned char *p = (const unsigned char *)value;
  while (*p) {
    uint32_t cp = 0;
    size_t width = 0;
    if (*p < 0x80) {
      cp = *p;
      width = 1;
    } else if (*p >= 0xC2 && *p <= 0xDF &&
               (p[1] & 0xC0) == 0x80) {
      cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
      width = 2;
    } else if (*p >= 0xE0 && *p <= 0xEF &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((uint32_t)(p[0] & 0x0F) << 12) |
           ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
      width = 3;
    } else if (*p >= 0xF0 && *p <= 0xF4 &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
               (p[3] & 0xC0) == 0x80) {
      cp = ((uint32_t)(p[0] & 0x07) << 18) |
           ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
      if (cp < 0x10000 || cp > 0x10FFFF) return false;
      width = 4;
    } else {
      return false;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) ||
        (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF) {
      return false;
    }
    p += width;
  }
  return true;
}

static bool strict_optional_pending_string(const cJSON *object,
                                           const char *key,
                                           char *destination,
                                           size_t capacity,
                                           bool *has_value) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  *has_value = false;
  destination[0] = '\0';
  if (!item) return true;
  if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] ||
      !pending_utf8_valid(item->valuestring)) {
    return false;
  }
  size_t length = strlen(item->valuestring);
  if (length >= capacity) return false;
  memcpy(destination, item->valuestring, length + 1);
  *has_value = true;
  return true;
}

static bool exact_bool_member(const cJSON *object, const char *key,
                              bool *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsBool(item)) return false;
  *out = cJSON_IsTrue(item);
  return true;
}

typedef struct {
  char *data;
  size_t cap;
  size_t len;
  bool ok;
} canonical_builder;

static void canonical_bytes(canonical_builder *builder, const char *data,
                            size_t len) {
  if (!builder->ok || len >= builder->cap - builder->len) {
    builder->ok = false;
    return;
  }
  memcpy(builder->data + builder->len, data, len);
  builder->len += len;
  builder->data[builder->len] = '\0';
}

static void canonical_literal(canonical_builder *builder,
                              const char *literal) {
  canonical_bytes(builder, literal, strlen(literal));
}

static void canonical_string(canonical_builder *builder, const char *value) {
  canonical_literal(builder, "\"");
  for (const char *p = value; builder->ok && *p; p++) {
    if (*p == '"' || *p == '\\') canonical_literal(builder, "\\");
    canonical_bytes(builder, p, 1);
  }
  canonical_literal(builder, "\"");
}

static void canonical_uint(canonical_builder *builder, uint32_t value) {
  char number[11];
  int written = snprintf(number, sizeof number, "%lu", (unsigned long)value);
  if (written < 0 || (size_t)written >= sizeof number) {
    builder->ok = false;
    return;
  }
  canonical_bytes(builder, number, (size_t)written);
}

static void canonical_key_string(canonical_builder *builder, bool *first,
                                 const char *key, const char *value) {
  canonical_literal(builder, *first ? "\"" : ",\"");
  canonical_literal(builder, key);
  canonical_literal(builder, "\":");
  canonical_string(builder, value);
  *first = false;
}

static void canonical_key_uint(canonical_builder *builder, bool *first,
                               const char *key, uint32_t value) {
  canonical_literal(builder, *first ? "\"" : ",\"");
  canonical_literal(builder, key);
  canonical_literal(builder, "\":");
  canonical_uint(builder, value);
  *first = false;
}

static void canonical_key_bool(canonical_builder *builder, bool *first,
                               const char *key, bool value) {
  canonical_literal(builder, *first ? "\"" : ",\"");
  canonical_literal(builder, key);
  canonical_literal(builder, value ? "\":true" : "\":false");
  *first = false;
}

static bool pending_view_digest_matches(const tk_pending_interaction *view) {
  char canonical[TK_PENDING_CANONICAL_VIEW_CAP];
  canonical_builder builder = {
      .data = canonical, .cap = sizeof canonical, .len = 0, .ok = true};
  bool first = true;
  canonical_literal(&builder, "{");
  canonical_key_bool(&builder, &first, "can_approve", view->can_approve);
  canonical_key_uint(&builder, &first, "hold_ms", view->hold_ms);
  canonical_key_string(&builder, &first, "kind",
                       view->kind == TK_PENDING_QUESTION ? "question" :
                                                         "approval");
  if (view->kind == TK_PENDING_QUESTION) {
    canonical_key_bool(&builder, &first, "marked", view->marked);
    canonical_key_uint(&builder, &first, "options_total",
                       view->options_total);
  }
  if (view->has_project) {
    canonical_key_string(&builder, &first, "project", view->project);
  }
  if (view->kind == TK_PENDING_QUESTION && view->has_prompt) {
    canonical_key_string(&builder, &first, "prompt", view->prompt);
  }
  canonical_key_string(&builder, &first, "provider",
                       view->provider == TK_AGENT_PROVIDER_CODEX ? "codex" :
                                                                  "claude");
  canonical_key_string(&builder, &first, "request_id", view->request_id);
  if (view->has_subtitle) {
    canonical_key_string(&builder, &first, "subtitle", view->subtitle);
  }
  if (view->has_title) {
    canonical_key_string(&builder, &first, "title", view->title);
  }
  if (view->kind == TK_PENDING_APPROVAL && view->has_tool) {
    canonical_key_string(&builder, &first, "tool", view->tool);
  }
  canonical_literal(&builder, "}");
  if (!builder.ok) return false;

  char calculated[TK_PENDING_VIEW_SHA256_CAP];
  tk_needs_you_sha256_hex(calculated, canonical, builder.len);
  unsigned difference = 0;
  for (size_t i = 0; i < 64; i++) {
    difference |= (unsigned char)calculated[i] ^
                  (unsigned char)view->view_sha256[i];
  }
  return difference == 0;
}

/* The pending interaction is OPTIONAL and parsed softly on purpose.
 *
 * Every other field here is all-or-nothing, because half a quota number is a
 * lie. This one is different: it arrives on the same payload as the agent
 * list, and rejecting the whole body over an unexpected pending field would
 * blank the screen's existing, shipped contract — the exact failure the
 * service is careful to avoid on its side. So anything wrong here means "no
 * interaction", never "no agent status", and a future service may add fields
 * inside `pending` without bricking a panel running this build.
 *
 * The conservative direction matters too: on any doubt the panel shows
 * nothing to tap, and the decision stays in the terminal where it is safe. */
static void parse_pending(const char *json, size_t len, const cJSON *root,
                          tk_pending_interaction *out) {
  memset(out, 0, sizeof *out);

  const cJSON *pending = cJSON_GetObjectItemCaseSensitive(root, "pending");
  if (!cJSON_IsObject(pending)) return;

  char provider[8];
  bool has_provider = false;
  if (!pending_contract_string(pending, "provider", provider, sizeof provider,
                               &has_provider)) {
    return;
  }
  if (has_provider) {
    if (strcmp(provider, "claude") == 0) {
      out->provider = TK_AGENT_PROVIDER_CLAUDE;
#if TK_CODEX_ENABLED
    } else if (strcmp(provider, "codex") == 0) {
      out->provider = TK_AGENT_PROVIDER_CODEX;
#endif
    } else {
      /* A Claude-only build treats "codex" exactly like any unsupported
       * provider: the item is dropped here, so no Codex question ever
       * reaches the takeover, the queue or the answer path. */
      return;
    }
  } else {
    out->provider = TK_AGENT_PROVIDER_CLAUDE;
  }

  bool has_view_sha256 = false;
  if (!pending_contract_string(pending, "view_sha256", out->view_sha256,
                               sizeof out->view_sha256,
                               &has_view_sha256)) {
    memset(out, 0, sizeof *out);
    return;
  }
  if (has_view_sha256 && !lowercase_sha256(out->view_sha256)) {
    memset(out, 0, sizeof *out);
    return;
  }
  /* Provider and digest are the v2 marker and must travel as a pair for both
   * providers. Only a payload missing BOTH is genuine Claude v1. */
  if (has_provider != has_view_sha256) {
    memset(out, 0, sizeof *out);
    return;
  }
  bool uses_v2 = has_provider;
  if (uses_v2 && !allowed_keys_once(pending, pending_v2_allowed_keys)) {
    memset(out, 0, sizeof *out);
    return;
  }
  out->has_view_sha256 = has_view_sha256;

  const cJSON *kind = cJSON_GetObjectItemCaseSensitive(pending, "kind");
  if (!cJSON_IsString(kind) || !kind->valuestring) return;
  if (strcmp(kind->valuestring, "question") == 0) {
    out->kind = TK_PENDING_QUESTION;
  } else if (strcmp(kind->valuestring, "approval") == 0) {
    out->kind = TK_PENDING_APPROVAL;
  } else {
    return; /* a kind this build cannot render is not one it may answer */
  }

  bool has_request_id = false;
  if (!pending_contract_string(pending, "request_id", out->request_id,
                               sizeof out->request_id, &has_request_id) ||
      !has_request_id || !pending_request_id_valid(out->request_id)) {
    memset(out, 0, sizeof *out);
    return; /* nothing to answer with */
  }

  uint32_t expires_in_ms = 0;
  if (!uint32_member(json, len, root, pending, "expires_in_ms",
                     &expires_in_ms)) {
    memset(out, 0, sizeof *out);
    return;
  }
  out->expires_in_ms = expires_in_ms;

  uint32_t hold_ms = 0;
  bool has_hold_ms = uint32_member(json, len, root, pending, "hold_ms",
                                   &hold_ms);
  if (uses_v2 && (!has_hold_ms || hold_ms == 0)) {
    memset(out, 0, sizeof *out);
    return;
  }
  /* Optional only on legacy: an older service did not send the original hold
   * and the countdown ring simply reads full. */
  if (has_hold_ms) {
    out->hold_ms = hold_ms;
  }

  uint32_t options_total = 0;
  if (uses_v2) {
    const cJSON *marked_item = cJSON_GetObjectItemCaseSensitive(
        pending, "marked");
    const cJSON *options_item = cJSON_GetObjectItemCaseSensitive(
        pending, "options_total");
    const cJSON *prompt_item = cJSON_GetObjectItemCaseSensitive(
        pending, "prompt");
    const cJSON *tool_item = cJSON_GetObjectItemCaseSensitive(pending, "tool");
    if (out->kind == TK_PENDING_QUESTION) {
      if (!uint32_member(json, len, root, pending, "options_total",
                         &options_total) || options_total == 0 ||
          options_total > UINT8_MAX ||
          !exact_bool_member(pending, "marked", &out->marked) || tool_item) {
        memset(out, 0, sizeof *out);
        return;
      }
      out->options_total = (uint8_t)options_total;
    } else if (marked_item || options_item || prompt_item) {
      memset(out, 0, sizeof *out);
      return;
    }

    if (!strict_optional_pending_string(
            pending, "project", out->project, sizeof out->project,
            &out->has_project) ||
        !strict_optional_pending_string(
            pending, "prompt", out->prompt, sizeof out->prompt,
            &out->has_prompt) ||
        !strict_optional_pending_string(
            pending, "title", out->title, sizeof out->title,
            &out->has_title) ||
        !strict_optional_pending_string(
            pending, "subtitle", out->subtitle, sizeof out->subtitle,
            &out->has_subtitle) ||
        !strict_optional_pending_string(
            pending, "tool", out->tool, sizeof out->tool, &out->has_tool) ||
        !exact_bool_member(pending, "can_approve", &out->can_approve) ||
        (out->can_approve &&
         (!out->has_title ||
          (out->kind == TK_PENDING_QUESTION && !out->marked)))) {
      memset(out, 0, sizeof *out);
      return;
    }
    if (!pending_view_digest_matches(out)) {
      memset(out, 0, sizeof *out);
      return;
    }
  } else {
    if (uint32_member(json, len, root, pending, "options_total",
                      &options_total) && options_total <= 0xFF) {
      out->options_total = (uint8_t)options_total;
    }
    optional_pending_string(pending, "project", out->project,
                            sizeof out->project, &out->has_project);
    optional_pending_string(pending, "prompt", out->prompt,
                            sizeof out->prompt, &out->has_prompt);
    optional_pending_string(pending, "title", out->title,
                            sizeof out->title, &out->has_title);
    optional_pending_string(pending, "subtitle", out->subtitle,
                            sizeof out->subtitle, &out->has_subtitle);
    optional_pending_string(pending, "tool", out->tool,
                            sizeof out->tool, &out->has_tool);
    out->marked = pending_bool(pending, "marked");
    out->can_approve = pending_bool(pending, "can_approve") && out->has_title;
  }

  /* APPROVE exists only when the service says so AND there is something
   * readable to approve. Two independent reasons to withhold it, because
   * approving text you cannot see is the failure this whole feature must not
   * ship with. */
  out->present = true;
}

bool tk_agent_status_parse_relay_view(
    const uint8_t *view, size_t view_len, uint32_t expires_in_ms,
    const char view_sha256[TK_PENDING_VIEW_SHA256_CAP],
    tk_pending_interaction *out) {
  if (out != NULL) memset(out, 0, sizeof *out);
  if (view == NULL || out == NULL || view_sha256 == NULL ||
      view_len < 2 || view_len > TK_IR_MAX_VIEW_BYTES ||
      expires_in_ms == 0 ||
      view[0] != '{' || view[view_len - 1] != '}' ||
      memchr(view, '\0', view_len) != NULL ||
      !lowercase_sha256(view_sha256)) {
    return false;
  }

  char calculated[TK_PENDING_VIEW_SHA256_CAP];
  tk_needs_you_sha256_hex(calculated, (const char *)view, view_len);
  unsigned difference = 0;
  for (size_t i = 0; i < 64; ++i) {
    difference |= (unsigned char)calculated[i] ^
                  (unsigned char)view_sha256[i];
  }
  if (difference != 0) return false;

  /* Add only transport fields that stable view_bytes() intentionally omits.
   * parse_pending then enforces the same strict v2 fields, UTF-8 limits and
   * canonical digest as the direct LAN path. */
  char wrapped[TK_PENDING_CANONICAL_VIEW_CAP];
  static const char prefix[] = "{\"pending\":";
  int suffix_len = snprintf(
      wrapped + sizeof prefix - 1 + view_len - 1,
      sizeof wrapped - (sizeof prefix - 1 + view_len - 1),
      ",\"expires_in_ms\":%lu,\"view_sha256\":\"%s\"}}",
      (unsigned long)expires_in_ms, view_sha256);
  if (suffix_len <= 0) return false;
  size_t used = sizeof prefix - 1 + view_len - 1 + (size_t)suffix_len;
  if (used >= sizeof wrapped) return false;
  memcpy(wrapped, prefix, sizeof prefix - 1);
  memcpy(wrapped + sizeof prefix - 1, view, view_len - 1);
  wrapped[used] = '\0';

  if (!json_lexically_valid(wrapped, used)) return false;
  const char *parse_end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts(wrapped, used, &parse_end, false);
  if (root == NULL) return false;
  bool ok = trailing_is_whitespace(wrapped, used, parse_end) &&
            cJSON_IsObject(root);
  if (ok) {
    parse_pending(wrapped, used, root, out);
    ok = out->present;
  }
  cJSON_Delete(root);
  if (!ok) memset(out, 0, sizeof *out);
  memset(wrapped, 0, sizeof wrapped);
  memset(calculated, 0, sizeof calculated);
  return ok;
}

static bool job_member(const char *json, size_t len, const cJSON *root,
                       const cJSON *job, tk_agent_status *out) {
  if (!required_keys_once(job, job_required_keys) ||
      !allowed_keys_once(job, job_allowed_keys)) {
    return false;
  }

  return nullable_string_member(job, "task_id", out->task_id,
                                sizeof out->task_id) &&
         nullable_string_member(job, "event_id", out->event_id,
                                sizeof out->event_id) &&
         state_member(job, &out->state) &&
         nullable_string_member(job, "project", out->project,
                                sizeof out->project) &&
         activity_member(job, &out->activity) &&
         optional_nullable_string_member(job, "model", out->model,
                                         sizeof out->model,
                                         &out->has_model) &&
         optional_nullable_string_member(job, "effort", out->effort,
                                         sizeof out->effort,
                                         &out->has_effort) &&
         uint32_member(json, len, root, job, "updated_ms",
                       &out->updated_ms);
}

static bool provider_member(const char *json, size_t len, const cJSON *root,
                            const cJSON *agents, const char *key,
                            tk_agent_provider_status *out) {
  const cJSON *provider = cJSON_GetObjectItemCaseSensitive(agents, key);
  if (!required_keys_once(provider, provider_required_keys) ||
      !allowed_keys_once(provider, provider_required_keys)) {
    return false;
  }

  uint32_t active_count = 0;
  if (!uint32_member(json, len, root, provider, "active_count",
                     &active_count) || active_count > UINT8_MAX) {
    return false;
  }
  const cJSON *jobs = cJSON_GetObjectItemCaseSensitive(provider, "jobs");
  if (!cJSON_IsArray(jobs)) return false;
  int count = cJSON_GetArraySize(jobs);
  if (count < 0 || count > TK_AGENT_JOBS_MAX) return false;

  out->active_count = (uint8_t)active_count;
  out->job_count = (uint8_t)count;
  for (int i = 0; i < count; i++) {
    const cJSON *job = cJSON_GetArrayItem(jobs, i);
    if (!job_member(json, len, root, job, &out->jobs[i])) return false;
  }
  return true;
}

static bool agent_status_parse(const char *json, size_t len,
                               tk_agent_snapshot *out,
                               bool allow_pending) {
  if (!json || !out || !json_lexically_valid(json, len)) return false;

  const char *parse_end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts(json, len, &parse_end, false);
  if (!root) return false;

  bool ok = false;
  tk_agent_snapshot next = {0};
  uint32_t version = 0;

  if (!trailing_is_whitespace(json, len, parse_end)) goto done;
  if (!cJSON_IsObject(root)) goto done;
  if (cJSON_GetObjectItemCaseSensitive(root, "error")) goto done;
  if (!allow_pending &&
      cJSON_GetObjectItemCaseSensitive(root, "pending") != NULL) goto done;
  if (!required_keys_once(root, root_required_keys)) goto done;
  if (!uint32_member(json, len, root, root, "v", &version) || version != 2) {
    goto done;
  }
  if (!uint32_member(json, len, root, root, "seq", &next.seq)) goto done;

  const cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
  if (!required_keys_once(agents, agents_required_keys) ||
      !allowed_keys_once(agents, agents_required_keys)) goto done;
  if (!provider_member(json, len, root, agents, "claude", &next.claude)) {
    goto done;
  }
  if (!provider_member(json, len, root, agents, "codex", &next.codex)) {
    goto done;
  }

  parse_pending(json, len, root, &next.pending);

  *out = next;
  ok = true;

done:
  cJSON_Delete(root);
  return ok;
}

bool tk_agent_status_parse(const char *json, size_t len,
                           tk_agent_snapshot *out) {
  return agent_status_parse(json, len, out, true);
}

bool tk_agent_status_parse_relay(const char *json, size_t len,
                                 tk_agent_snapshot *out) {
  return agent_status_parse(json, len, out, false);
}
