/*
  Copyright (c) 2026 MariaDB

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA.
*/

#include "rpl_info_file.h"
#include <unordered_map> // Type of MASTER_VALUE_MAP
#include <string_view>   // Key type of MASTER_VALUE_MAP
#include <unordered_set> // Used by Master_info_file::load_from_file() to dedup


int change_master_id_cmp(const void *arg1, const void *arg2)
{
  const ulong &id1= *(const ulong *)arg1, &id2= *(const ulong *)arg2;
  return (id1 > id2) - (id1 < id2);
}


namespace Int_IO_CACHE
{
  /**
    @ref IO_CACHE (reading one line with the `\n`) version of std::from_chars()
    @tparam I integer type
    @return `false` if the line has parsed successfully or `true` if error
  */
  template<typename I> static bool from_chars(IO_CACHE *file, I &value)
  {
    int error;
    /**
      +2 for the terminating `\n\0`
      (They are ignored, but my_b_gets() includes them.)
    */
    char buf[BUF_SIZE<I> + 2];
    /// includes the `\n` but excludes the `\0`
    size_t length= my_b_gets(file, buf, sizeof(buf));
    if (!length) // EOF
      return true;
    char *end= &(buf[length]);
    longlong val= my_strtoll10(buf, &end, &error);
    switch (error) {
    case -1:
      if (!std::numeric_limits<I>::is_signed)
        return true;
      [[fallthrough]];
    case 0:
      /*TODO
        This upper range check is not needed when using type-
        specific variants of a safe string-to-integer converter
        (e.g., std::from_chars() when all platforms support it).
      */
      if (*end == '\n' && value <= std::numeric_limits<I>::max())
      {
        value= static_cast<I>(val);
        return false;
      }
      [[fallthrough]];
    default:
      return true;
    }
  }
  /**
    Convenience overload of from_chars(IO_CACHE *, I &) for `operator=` types
    @tparam I inner integer type
    @tparam T wrapper type
  */
  template<typename I, class T> static bool from_chars(IO_CACHE *file, T *self)
  {
    I value;
    if (from_chars(file, value))
      return true;
    (*self)= value;
    return false;
  }

  /**
    @ref IO_CACHE (writing *without* a `\n`) version of std::to_chars()
    @tparam I (inner) integer type
  */
  template<typename I> static void to_chars(IO_CACHE *file, I value)
  {
    char buf[BUF_SIZE<I>];
    /*TODO:
      * my_b_printf() needs updates and so doesn't
        support `long long`s at the moment.
      * We can avoid format parsing by expanding
        int10_to_str() if not supporting std::to_chars().
    */
    int len= std::numeric_limits<I>::is_signed ?
      snprintf(buf, BUF_SIZE<I>, "%lld", static_cast<long long>(value)) :
      snprintf(buf, BUF_SIZE<I>, "%llu", static_cast<unsigned long long>(value))
    ;
    DBUG_ASSERT(len > 0);
    my_b_write(file, reinterpret_cast<const uchar *>(buf), len);
  }
};

template<typename I>
bool Info_file::Int_value<I>::load_from(IO_CACHE *file)
{ return Int_IO_CACHE::from_chars(file, value); }
template<typename I>
void Info_file::Int_value<I>::save_to(IO_CACHE *file)
{ return Int_IO_CACHE::to_chars(file, value); }

template<auto &mariadbd_option, typename I>
bool Master_info_file::Optional_int_value<mariadbd_option, I>::load_from(IO_CACHE *file)
{ return Int_IO_CACHE::from_chars<I>(file, this); }
template<auto &mariadbd_option, typename I>
void Master_info_file::Optional_int_value<mariadbd_option, I>::save_to(IO_CACHE *file)
{ return Int_IO_CACHE::to_chars(file, operator I()); }


template<size_t size>
bool Info_file::String_value<size>::load_from(IO_CACHE *file)
{
  size_t length= my_b_gets(file, buf, size);
  if (!length) // EOF
    return true;
  /// If we stopped on a newline, kill it.
  char &last_char= buf[length-1];
  if (last_char == '\n')
  {
    last_char= '\0';
    return false;
  }
  /*
    Consume the lost line break,
    or error if the line overflows the @ref buf.
  */
  return my_b_get(file) != '\n';
}

template<size_t size>
void Info_file::String_value<size>::save_to(IO_CACHE *file)
{
  const char *buf= *this;
  my_b_write(file, reinterpret_cast<const uchar *>(buf), strlen(buf));
}

template<const char *&mariadbd_option>
Master_info_file::Optional_path_value<mariadbd_option> &
Master_info_file::Optional_path_value<mariadbd_option>::operator=(const char *other)
{
  if (other)
  {
    buf[1]= false; // not default
    String_value<>::operator=(other);
  }
  else
    set_default();
  return *this;
}


template<bool &mariadbd_option>
bool Master_info_file::Optional_bool_value<mariadbd_option>::load_from(IO_CACHE *file)
{
  /** Only three chars are required:
    * One digit
      (When base prefixes are not recognized in integer parsing,
      anything with a leading `0` stops parsing
      after converting the `0` to zero anyway.)
    * the terminating `\n\0` as in IntegerLike::from_chars(IO_CACHE *, I &)
  */
  char buf[3];
  if (my_b_gets(file, buf, 3) && buf[1] == '\n')
    switch (buf[0]) {
    case '0':
      value= trilean::NO;
      return false;
    case '1':
      value= trilean::YES;
      return false;
    }
  return true;
}

template<bool &mariadbd_option>
void Master_info_file::Optional_bool_value<mariadbd_option>::save_to(IO_CACHE *file)
{ my_b_write_byte(file, operator bool() ? '1' : '0'); }


/// @pre @ref array is initialized
bool Master_info_file::ID_array_value::load_from(IO_CACHE *file)
{
  long count;
  size_t i;
  /// +1 for the terminating delimiter
  char buf[Int_IO_CACHE::BUF_SIZE<uint32_t> + 1];
  for (i= 0; i < sizeof(buf); ++i)
  {
    int c= my_b_get(file);
    if (c == my_b_EOF)
      return true;
    buf[i]= static_cast<char>(c);
    if (c == /* End of Line */ '\n' || c == /* End of Count */ ' ')
      break;
  }
  char *end= str2int(buf, 10, 1, INT32_MAX, &count);
  // Reserve enough elements ahead of time.
  if (!end || allocate_dynamic(&array, count))
    return true;
  while (count--)
  {
    long value;
    /*
      Check that the previous number ended with a ` `,
      not `\n` or anything else.
    */
    if (*end != ' ')
      return true;
    for (i= 0; i < sizeof(buf); ++i)
    {
      /*
        Bottlenecks from repeated IO does not affect the
        performance of reading char by char thanks to the cache.
      */
      int c= my_b_get(file);
      if (c == my_b_EOF)
        return true;
      buf[i]= static_cast<char>(c);
      if (c == /* End of Count */ ' ' || c == /* End of Line */ '\n')
        break;
    }
    end= str2int(buf, 10, 1, INT32_MAX, &value);
    if (!end)
      return true;
    ulong id= value;
    bool oom= insert_dynamic(&array, (uchar *)&id);
    /*
      This should not error because enough
      memory was already allocate_dynamic()-ed.
    */
    DBUG_ASSERT(!oom);
    if (oom)
      return true;
  }
  // Check that the last number ended with a `\n`, not ` ` or anything else.
  if (*end != '\n')
    return true;
  sort_dynamic(&array, change_master_id_cmp); // to be safe
  return false;
}

/// Store the total number of elements followed by the individual elements.
void Master_info_file::ID_array_value::save_to(IO_CACHE *file)
{
  Int_IO_CACHE::to_chars(file, array.elements);
  for (size_t i= 0; i < array.elements; ++i)
  {
    ulong id;
    get_dynamic(&array, &id, i);
    my_b_write_byte(file, ' ');
    Int_IO_CACHE::to_chars(file, id);
  }
}


decltype(Master_info_file::master_use_gtid)::operator enum_master_use_gtid()
{
  if (is_default())
  {
    auto default_use_gtid=
      static_cast<enum_master_use_gtid>(::master_use_gtid);
    return default_use_gtid >= enum_master_use_gtid::DEFAULT ? (
      gtid_supported ?
        enum_master_use_gtid::SLAVE_POS : enum_master_use_gtid::NO
    ) : default_use_gtid;
  }
  return mode;
}
/** @return
  `true` if the line is a @ref enum_master_use_gtid,
  `false` otherwise or on error
*/
bool decltype(Master_info_file::master_use_gtid)::load_from(IO_CACHE *file)
{
  /**
    Only 3 chars are required for the enum,
    similar to @ref Optional_bool_value::load_from()
  */
  char buf[3];
  if (!my_b_gets(file, buf, 3) ||
      buf[1] != '\n' ||
      buf[0] > /* SLAVE_POS */ '2' || buf[0] < /* NO */ '0')
    return true;
  operator=(static_cast<enum_master_use_gtid>(buf[0] - '0'));
  return false;
}
void decltype(Master_info_file::master_use_gtid)::save_to(IO_CACHE *file)
{
  my_b_write_byte(file, static_cast<uchar>(
    '0' + static_cast<unsigned char>(operator enum_master_use_gtid())));
}


Master_info_file::Heartbeat_period_value::operator uint32_t()
{
  return is_default() ? ::master_heartbeat_period.value_or(
    MY_MIN(slave_net_timeout*500ULL, std::numeric_limits<uint32_t>::max())
  ) : *(Optional_value<uint32_t>::optional);
}

/** Load from a `DECIMAL(10,3)`
  @param overprecise
    set to `true` if the decimal has more than 3 decimal digits
  @return whether the decimal is out of range
  @post Output arguments are set on success and
    not changed if the decimal is out of range.
*/
uint Master_info_file::Heartbeat_period_value::from_decimal(
  uint32_t &result, const decimal_t &decimal, bool &overprecise
)
{
  /// Wrapper to enable only-once static const construction
  struct Decimal_from_str: my_decimal
  {
    Decimal_from_str(const char *str, size_t strlen): my_decimal()
    {
      const char *end= &(str[strlen]);
      [[maybe_unused]] int unexpected_error= str2my_decimal(
        E_DEC_ERROR, str, this, const_cast<char **>(&end));
      DBUG_ASSERT(!unexpected_error && !*end);
    }
  };
  /*
    The ideal implementation would work with native `double`s as they are
    sufficiently precise in this case; but decimal2double() is currently
    implemented by printing into a string and parsing that char array,
    which is an even larger overhead than `my_decimal` multiplication.
  */
  static const auto MAX_PERIOD= Decimal_from_str(STRING_WITH_LEN(MAX)),
                    THOUSAND  = Decimal_from_str(STRING_WITH_LEN("1000"));
  [[maybe_unused]] ulonglong decimal_out;
  if (decimal.sign || decimal_cmp(&MAX_PERIOD, &decimal) < 0)
    return true; // ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE
  overprecise= decimal.frac > 3;
  // decomposed from my_decimal2int() to reduce a bit of computations
  auto product= my_decimal();
  bool unexpected_error=
    decimal_mul(&decimal, &THOUSAND, &product) ||
    (decimal2ulonglong(&product, &decimal_out, HALF_UP) > E_DEC_TRUNCATED);
  DBUG_ASSERT(!unexpected_error);
  if (unexpected_error)
    return true;
  result= static_cast<uint32_t>(decimal_out);
  return false;
}

/** Load from a '\0'-terminated string
  @param expected_end This function also checks that the exclusive end
    of the decimal *(which may be `str_end` itself)* is this delimiter.
  @return from_decimal(), or `true` on unexpected contents
  @post Output arguments are set on success and not changed on error.
*/
uint Master_info_file::Heartbeat_period_value::from_chars(
  std::optional<uint32_t> &self, const char *str,
  const char *str_end, bool &overprecise, char expected_end
)
{
  uint32_t result;
  auto decimal= my_decimal();
  if (str2my_decimal(
      E_DEC_ERROR, str, &decimal, const_cast<char **>(&str_end)
    ) || *str_end != expected_end ||
    from_decimal(result, decimal, overprecise))
    return true;
  self.emplace(result);
  return false;
}
bool Master_info_file::Heartbeat_period_value::load_from(IO_CACHE *file)
{
  /**
    Number of chars Optional_int_value::load_from() uses plus
    1 for the decimal point; truncate the excess precision,
    which there should not be unless the file is edited externally.
  */
  char buf[Int_IO_CACHE::BUF_SIZE<uint32_t> + 3];
  bool overprecise;
  size_t length= my_b_gets(file, buf, sizeof(buf));
  return !length ||
    from_chars(optional, buf, &(buf[length]), overprecise) || overprecise;
}

/**
  This method is engineered (that is, hard-coded) to take
  full advantage of the non-negative `DECIMAL(10,3)` format.
*/
void Master_info_file::Heartbeat_period_value::save_to(IO_CACHE *file) {
  auto[integer_part, decimal_part]= div(operator uint32_t(), 1000);
  my_b_printf(file, "%u.%03u", integer_part, decimal_part);
}


/**
  Guard agaist extra left-overs at the end of file in case a later update
  causes the effective content to shrink compared to earlier contents
*/
static constexpr const char END_MARKER[]= "END_MARKER";

static const Info_file::Mem_fn MASTER_VALUE_LIST[] {
  &Master_info_file::master_log_file,
  &Master_info_file::master_log_pos,
  &Master_info_file::master_host,
  &Master_info_file::master_user,
  &Master_info_file::master_password,
  &Master_info_file::master_port,
  &Master_info_file::master_connect_retry,
  &Master_info_file::master_ssl,
  &Master_info_file::master_ssl_ca,
  &Master_info_file::master_ssl_capath,
  &Master_info_file::master_ssl_cert,
  &Master_info_file::master_ssl_cipher,
  &Master_info_file::master_ssl_key,
  &Master_info_file::master_ssl_verify_server_cert,
  &Master_info_file::master_heartbeat_period,
  nullptr, // &Master_info_file::master_bind, // MDEV-19248
  &Master_info_file::ignore_server_ids,
  nullptr, // MySQL `master_uuid`, which MariaDB ignores.
  &Master_info_file::master_retry_count,
  &Master_info_file::master_ssl_crl,
  &Master_info_file::master_ssl_crlpath
};

/** A keyed iterable for the `key=value` section of `@@master_info_file`.
  For bidirectional compatibility with MySQL
  (codenames only at this writing) and earlier versions of MariaDB,
  keys should match the corresponding old property name in @ref Master_info.
*/
// C++ default allocator to match that `mysql_execute_command()` uses `new`
static
const std::unordered_map<std::string_view, const Info_file::Mem_fn> MASTER_VALUE_MAP= {
  /*
    These are here to annotate whether they are `DEFAULT`.
    They are repeated from @ref VALUE_LIST to enable bidirectional
    compatibility with MySQL and earlier versions of MariaDB
    (where unrecognized keys, such as those from the future, are ignored).
  */
  {"connect_retry"    , &Master_info_file::master_connect_retry         },
  {"ssl"              , &Master_info_file::master_ssl                   },
  {"ssl_ca"           , &Master_info_file::master_ssl_ca                },
  {"ssl_capath"       , &Master_info_file::master_ssl_capath            },
  {"ssl_cert"         , &Master_info_file::master_ssl_cert              },
  {"ssl_cipher"       , &Master_info_file::master_ssl_cipher            },
  {"ssl_key"          , &Master_info_file::master_ssl_key               },
  {"ssl_crl"          , &Master_info_file::master_ssl_crl               },
  {"ssl_crlpath"      , &Master_info_file::master_ssl_crlpath           },
  {"ssl_verify_server_cert",
                        &Master_info_file::master_ssl_verify_server_cert},
  {"heartbeat_period" , &Master_info_file::master_heartbeat_period      },
  {"retry_count"      , &Master_info_file::master_retry_count           },
  // These are the ones new in MariaDB.
  {"using_gtid",        &Master_info_file::master_use_gtid  },
  {"do_domain_ids",     &Master_info_file::do_domain_ids    },
  {"ignore_domain_ids", &Master_info_file::ignore_domain_ids},
  {END_MARKER, nullptr}
};

static const Info_file::Mem_fn RELAY_LOG_VALUE_LIST[] {
  &Relay_log_info_file::relay_log_file,
  &Relay_log_info_file::relay_log_pos,
  &Relay_log_info_file::read_master_log_file,
  &Relay_log_info_file::read_master_log_pos,
  &Relay_log_info_file::sql_delay
};


Master_info_file::Master_info_file(
  DYNAMIC_ARRAY &ignore_server_ids,
  DYNAMIC_ARRAY &do_domain_ids, DYNAMIC_ARRAY &ignore_domain_ids
):
  ignore_server_ids(ignore_server_ids),
  do_domain_ids(do_domain_ids), ignore_domain_ids(ignore_domain_ids)
{
  for (std::unordered_map<std::string_view,
          const Mem_fn>::const_iterator it= MASTER_VALUE_MAP.begin();
        it != MASTER_VALUE_MAP.end(); ++it)
    if (it->second)
      it->second(this).set_default();
}


bool
Info_file::load_from_file(const Mem_fn *values, size_t size, size_t default_line_count)
{
  long val;
  /**
    The first row is temporarily stored in the first value. If it is a line
    count and not a log name (new format), the second row will overwrite it.
  */
  auto &line1= dynamic_cast<String_value<> &>(values[0](this));
  if (line1.load_from(&file))
    return true;
  char *end= str2int(line1.buf, 10, 0, INT32_MAX, &val);
  /**
    If this first line was not a number - the line count,
    then it was the first value for real,
    so the for loop should then skip over it, the index 0 of the list.
  */
  size_t i= !end || *end != '\0';
  /*
    Set the default after parsing: While std::from_chars() does not replace
    the output if it failed, it does replace if the line is not fully spent.
  */
  size_t line_count= i ? default_line_count: static_cast<size_t>(val);
  for (; i < line_count; ++i)
  {
    int c;
    if (i < size) // line known in the `value_list`
    {
      const Mem_fn &pm= values[i];
      if (pm)
      {
        if (pm(this).load_from(&file))
          return true;
        continue;
      }
    }
    /*
      Count and discard unrecognized lines.
      This is especially to prepare for @ref Master_info_file for MariaDB 10.0+,
      which reserves a bunch of lines before its unique `key=value` section
      to accomodate any future line-based (old-style) additions in MySQL.
      (This will make moving from MariaDB to MySQL easier by not
      requiring MySQL to recognize MariaDB `key=value` lines.)
    */
    while ((c= my_b_get(&file)) != '\n')
      if (c == my_b_EOF)
        return true; // EOF already?
  }
  return false;
}

bool Master_info_file::load_from_file()
{
  /// Repurpose the trailing `\0` spot to prepare for the `=` or `\n`
  static constexpr size_t LONGEST_KEY_SIZE= sizeof("ssl_verify_server_cert");
  if (Info_file::load_from_file(MASTER_VALUE_LIST, /* MASTER_CONNECT_RETRY */ 7))
    return true;
  /*
    Info_file::load_from_file() is only for fixed-position entries.
    Proceed with `key=value` lines for MariaDB 10.0 and above:
    The "value" can then be read individually after consuming the`key=`.
  */
  /**
    MariaDB 10.0 does not have the `END_MARKER` before any left-overs at
    the end of the file, so ignore any non-first occurrences of a key.
    @note
      This set only "contains" the static strings of @ref MASTER_VALUE_MAP's keys,
      which means it can simply compare pointers by face values rather than
      their pointed content, in contrast with how `HASH` of `include/hash.h`
      is designed for string contents in a specified charset.
  */
  auto seen= std::unordered_set<const char *>();
  while (true)
  {
    /**
      A `key=value` line might not actually have the `=value` part;
      in this case, it means this value was set_default().
    */
    bool found_equal= false;
    char key[LONGEST_KEY_SIZE];
    for (size_t i= 0; i < LONGEST_KEY_SIZE; ++i)
    {
      switch (int c= my_b_get(&file)) {
      case my_b_EOF:
        return i; // OK if no chars were read, or error if the line hits EOF.
      case '=':
        found_equal= true;
      [[fallthrough]];
      case '\n':
      {
        decltype(MASTER_VALUE_MAP)::const_iterator kv=
          MASTER_VALUE_MAP.find(std::string_view(
            key,
            i // size = exclusive end index of the string
          ));
        // The "unknown" lines would be ignored to facilitate downgrades.
        if (kv != MASTER_VALUE_MAP.cend()) // found
        {
          const char *key= kv->first.data();
          if (key == END_MARKER)
            return false;
          /**
            The `second` member of std::unordered_set::insert()'s return
            is `true` for a new insertion or `false` for a duplicate.
          */
          else if (seen.insert(key).second)
          {
            Persistent &value= kv->second(this);
            if (found_equal ? value.load_from(&file) : value.set_default())
              return true;
          }
        }
        goto break_for;
      }
      default:
        key[i]= static_cast<char>(c);
      }
    }
break_for:;
  }
}

bool Relay_log_info_file::load_from_file()
{
  return Info_file::load_from_file(RELAY_LOG_VALUE_LIST, /* Exec_Master_Log_Pos */ 4);
}


void Info_file::save_to_file(const Mem_fn *values, size_t size, size_t total_line_count)
{
  DBUG_ASSERT(total_line_count > size);
  my_b_seek(&file, 0);
  /*
    If the new contents take less space than the previous file contents,
    then this code would write the file with unerased trailing garbage lines.
    But these garbage don't matter thanks to the number
    of effective lines in the first line of the file.
  */
  Int_IO_CACHE::to_chars(&file, total_line_count);
  my_b_write_byte(&file, '\n');
  for (size_t i= 0; i < size; ++i)
  {
    const Mem_fn &pm= values[i];
    if (pm)
      pm(this).save_to(&file);
    my_b_write_byte(&file, '\n');
  }
  /*
    Pad additional reserved lines:
    (1 for the line count line + line count) inclusive -> max line inclusive
      = line count exclusive <- max line inclusive
  */
  for (; total_line_count > size; --total_line_count)
    my_b_write_byte(&file, '\n');
}

void Master_info_file::save_to_file()
{
  // Write the line-based section with some reservations for MySQL additions
  Info_file::save_to_file(MASTER_VALUE_LIST, 33);
  /* Write MariaDB `key=value` lines:
    The "value" can then be written individually after generating the`key=`.
  */
  for (std::unordered_map<std::string_view,
          const Mem_fn>::const_iterator it= MASTER_VALUE_MAP.begin();
        it != MASTER_VALUE_MAP.end(); ++it)
  {
    if (it->second)
    {
      Persistent &value= it->second(this);
      my_b_write(&file,
                  reinterpret_cast<const uchar *>(it->first.data()), it->first.size());
      if (!value.is_default())
      {
        my_b_write_byte(&file, '=');
        value.save_to(&file);
      }
      my_b_write_byte(&file, '\n');
    }
  }
  my_b_write(&file, reinterpret_cast<const uchar *>(END_MARKER),
              sizeof(END_MARKER) - /* the '\0' */ 1);
  my_b_write_byte(&file, '\n');
}

void Relay_log_info_file::save_to_file()
{
  return Info_file::save_to_file(RELAY_LOG_VALUE_LIST);
}
