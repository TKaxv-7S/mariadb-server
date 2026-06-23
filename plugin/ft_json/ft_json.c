/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA */

/*
  Full-text parser plugin for JSON documents.

  Each leaf key-value pair in the JSON document becomes a search token of
  the form "key=value".  Nested object keys are joined with ".":
    {"a":1,"b":"qwe","c":{"d":"ef"}}  =>  a=1  b=qwe  c.d=ef

  Array elements use the zero-based index as the key segment:
    {"tags":["foo","bar"]}  =>  tags.0=foo  tags.1=bar

  XXX XXX XXX TODO
  1. encoding/escaping to work with names and values containing '.' or '='
  2. make parser static, use it automatically for json indexes (syntax sugar)
  3. make json functions use the index were possible
  4. optimizer
  5. workaround ft_min_word_len, must not apply
  6. doesn't have to be a plugin. ft_default_parser is not.
*/

#include <my_global.h>
#include <mysql/plugin.h>
#include <json_lib.h>

#define MAX_TOKEN 512   /* maximum bytes in one emitted token */

/* -------------------------------------------------------------------------
   Emit one token to the FTS engine.
   ---------------------------------------------------------------------- */

static void emit_word(MYSQL_FTPARSER_PARAM *param, const char *word, int len)
{
  MYSQL_FTPARSER_BOOLEAN_INFO bool_info= { FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };
  param->mysql_add_word(param, word, len, &bool_info);
}

/* -------------------------------------------------------------------------
   Build the dot-separated path string from the accumulated json_path_t
   steps.  Step 0 is the root-container marker and is skipped.
   Key bytes are copied verbatim (escape sequences in keys are rare and
   acceptable as-is for FTS purposes).
   Returns the number of bytes written (not including the NUL terminator).
   ---------------------------------------------------------------------- */

static int build_path(json_path_t *p, char *buf, int buf_size)
{
  int len= 0;
  int i;

  for (i= 1; i <= p->last_step_idx; i++)
  {
    json_path_step_t *step= (json_path_step_t *) p->steps.buffer + i;
    int avail= buf_size - len - 1;   /* bytes available before the NUL slot */

    if (avail <= 0)
      break;

    if (len > 0)
    {
      buf[len++]= '.';
      avail--;
    }

    if (step->type & JSON_PATH_KEY)
    {
      int key_len= (int)(step->key_end - step->key);
      if (key_len > avail)
        key_len= avail;
      if (key_len > 0)
      {
        memcpy(buf + len, step->key, (size_t) key_len);
        len+= key_len;
      }
    }
    else if (step->type & JSON_PATH_ARRAY)
    {
      int written= snprintf(buf + len, (size_t)(avail + 1),
                            "%d", step->n_item);
      if (written > avail)
        written= avail;
      len+= written;
    }
  }

  buf[len]= '\0';
  return len;
}

/* -------------------------------------------------------------------------
   For each scalar value found by json_get_path_next, construct a
   "path=value" token and emit it.
   ---------------------------------------------------------------------- */

static void emit_token(MYSQL_FTPARSER_PARAM *param,
                       json_engine_t *je, json_path_t *p)
{
  char  word[MAX_TOKEN + 1];
  int   path_len, val_len, word_len;
  uchar val_buf[MAX_TOKEN];

  path_len= build_path(p, word, MAX_TOKEN);

  if (path_len == 0)
    return;   /* root scalar without a key — skip */

  word[path_len]= '=';
  word_len= path_len + 1;

  if (je->value_type == JSON_VALUE_STRING)
  {
    val_len= json_unescape(je->s.cs, je->value, je->value + je->value_len,
                           je->s.cs, val_buf, val_buf + MAX_TOKEN - word_len);
    if (val_len < 0)
      val_len= 0;
  }
  else
  {
    /* NUMBER, TRUE, FALSE, NULL — raw bytes are plain ASCII */
    val_len= je->value_len;
    if (val_len > MAX_TOKEN - word_len)
      val_len= MAX_TOKEN - word_len;
    memcpy(val_buf, je->value, (size_t) val_len);
  }

  if (val_len > 0)
  {
    memcpy(word + word_len, val_buf, (size_t) val_len);
    word_len+= val_len;
  }

  emit_word(param, word, word_len);
}

/* -------------------------------------------------------------------------
   Boolean query parser: split on whitespace, honour leading +/- and
   trailing * but keep = and . as ordinary token characters so that tokens
   like "a=1" and "c.d=ef" survive intact.

   We cannot delegate to mysql_parse() / ftb_parse_query_internal() because
   that uses ft_get_word(), which treats = and . as word separators, thereby
   shredding our composite tokens.

   Pointers passed to mysql_add_word() come from param->doc (persistent), so
   MYSQL_FTFLAGS_NEED_COPY is not required here.
   ---------------------------------------------------------------------- */

static void parse_boolean_query(MYSQL_FTPARSER_PARAM *param)
{
  const char *p=   param->doc;
  const char *end= param->doc + param->length;
  const char *start;
  MYSQL_FTPARSER_BOOLEAN_INFO bool_info= { FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };

  while (p < end)
  {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
    if (p >= end)
      break;

    /* strip leading boolean operator */
    bool_info.yesno= 0;
    bool_info.trunc= 0;
    if      (*p == '+') { bool_info.yesno=  1; p++; }
    else if (*p == '-') { bool_info.yesno= -1; p++; }

    /* collect token until whitespace */
    start= p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
      p++;

    if (p > start)
    {
      const char *word_end= p;
      /* strip trailing wildcard */
      if (word_end > start && *(word_end - 1) == '*')
      {
        bool_info.trunc= 1;
        word_end--;
      }
      if (word_end > start)
        param->mysql_add_word(param, start, (int)(word_end - start), &bool_info);
    }
    bool_info.prev= ' ';
  }
}

/* -------------------------------------------------------------------------
   Main parse entry point.
   ---------------------------------------------------------------------- */

static int ft_json_parse(MYSQL_FTPARSER_PARAM *param)
{
  int              je_stack[JSON_DEPTH_DEFAULT];
  json_path_step_t p_steps[JSON_DEPTH_DEFAULT];
  json_engine_t    je;
  json_path_t      p;

  /*
    Boolean search queries: parse boolean operators ourselves instead of
    delegating to mysql_parse() (= ftb_parse_query_internal).  The built-in
    parser uses ft_get_word() which treats = and . as word separators,
    shredding tokens like "a=1" or "c.d=ef".  Our custom parser keeps those
    characters intact while still honouring leading +/- and trailing *.
  */
  if (param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO)
  {
    parse_boolean_query(param);
    return 0;
  }

  /*
    Initialise json_engine_t and json_path_t using stack-allocated buffers.
    MY_BUFFER_NO_RESIZE keeps all accesses in-bounds without any heap
    allocation.  json_lib's JSON_DEPTH_LIMIT guard ensures stack_p and
    last_step_idx stay within [0, JSON_DEPTH_DEFAULT-1].
  */
  memset(&je, 0, sizeof(je));
  memset(&p,  0, sizeof(p));

  je.stack.buffer=           (uchar *) je_stack;
  je.stack.max_element=      JSON_DEPTH_DEFAULT;
  je.stack.size_of_element=  sizeof(int);
  je.stack.malloc_flags=     MY_BUFFER_NO_RESIZE;

  p.steps.buffer=            (uchar *) p_steps;
  p.steps.max_element=       JSON_DEPTH_DEFAULT;
  p.steps.size_of_element=   sizeof(json_path_step_t);
  p.steps.malloc_flags=      MY_BUFFER_NO_RESIZE;

  /*
    Tell the FTS engine to copy each word we emit: our token buffer is a
    local stack variable that is overwritten on every iteration, so the
    engine must not hold a bare pointer into it.
  */
  param->flags|= MYSQL_FTFLAGS_NEED_COPY;

  json_get_path_start(&je, param->cs, (const uchar *) param->doc,
                      (const uchar *) param->doc + param->length, &p);

  while (json_get_path_next(&je, &p) == 0)
  {
    if (!json_value_scalar(&je))
      continue;
    emit_token(param, &je, &p);
  }

  return 0;
}

/* -------------------------------------------------------------------------
   Plugin boilerplate.
   ---------------------------------------------------------------------- */

static struct st_mysql_ftparser ft_json_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION,
  ft_json_parse,
  NULL,
  NULL 
};

maria_declare_plugin(ft_json)
{
  MYSQL_FTPARSER_PLUGIN,
  &ft_json_descriptor,
  "ft_json",
  "MariaDB plc",
  "Full-text parser for JSON documents (key=value tokens)",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
