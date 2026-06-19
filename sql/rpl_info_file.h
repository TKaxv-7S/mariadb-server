/*
  Copyright (c) 2025 MariaDB

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

#ifndef RPL_INFO_FILE_H
#define RPL_INFO_FILE_H

#include <cstdint>    // uintN_t
#include <functional> // superclass of Info_file::Mem_fn
#include <optional>   // Storage type of Master_info_file::Optional_int_value
// Interface type of Master_info_file::master_heartbeat_period
#include "my_decimal.h"
#include <my_sys.h>   // IO_CACHE, FN_REFLEN, ...
#include "sql_const.h" // MAX_PASSWORD_LENGTH


/** Helpers for reading and writing integers to and from @ref IO_CACHE
*/
namespace Int_IO_CACHE
{
  /** Number of fully-utilized decimal digits plus
    * the partially-utilized digit (e.g., the 2's place in "2147483647")
    * The sign, if signed (:
  */
  template<typename I> static constexpr size_t BUF_SIZE=
    std::numeric_limits<I>::digits10 + 1 + std::numeric_limits<I>::is_signed;
};

/**
  A three-way comparison function for using
  sort_dynamic() and bsearch() on ID_array_value::array.
  @return -1 if first argument is less, 0 if it equal to, or 1 if it is greater
  than the second
  @deprecated Use a sorted set, such as @ref std::set,
  to save on explicitly calling those functions.
*/
int change_master_id_cmp(const void *arg1, const void *arg2);

//TODO: Remove stub from mariadb-corporation/mariadb-columnstore-engine#3951
#ifndef RPL_MASTER_INFO_FILE_H
/// enum for @ref Master_info_file::Optional_bool_value
/*TODO:
  `UNKNOWN` is the general term in ternary logic, but this name is `#define`d in
  `item_cmpfunc.h`, which is used by target RocksDB, whose *C++11* requirement
  doesn't recognize `inline` constants (whereas the server is on C++17).
*/
enum struct trilean { NO, YES, DEFAULT= -1 };
/// enum for @ref Master_info_file::master_use_gtid
enum struct enum_master_use_gtid { NO, CURRENT_POS, SLAVE_POS, DEFAULT };
/// String names for non-@ref enum_master_use_gtid::DEFAULT values
inline const char *master_use_gtid_names[]=
  {"No", "Current_Pos", "Slave_Pos", nullptr};

/**
  `mariadbd` Options for the `DEFAULT` values of @ref Master_info_file values
  @{
*/
/// Computes the `DEFAULT` value of @ref ::master_heartbeat_period
extern uint slave_net_timeout;
inline uint32_t master_connect_retry= 60;
inline std::optional<uint32_t> master_heartbeat_period= std::nullopt;
inline bool master_ssl= true;
inline const char *master_ssl_ca     = "";
inline const char *master_ssl_capath = "";
inline const char *master_ssl_cert   = "";
inline const char *master_ssl_crl    = "";
inline const char *master_ssl_crlpath= "";
inline const char *master_ssl_key    = "";
inline const char *master_ssl_cipher = "";
inline bool master_ssl_verify_server_cert= true;
/// `ulong` is the data type `my_getopt` expects.
inline ulong master_use_gtid= static_cast<ulong>(enum_master_use_gtid::DEFAULT);
inline uint64_t master_retry_count= 100000;
/// }@
#endif // RPL_MASTER_INFO_FILE_H


/**
  This common superclass of @ref Master_info_file and
  @ref Relay_log_info_file provides them common code for saving
  and loading values in their MySQL line-based sections.
  As only the @ref Master_info_file has a MariaDB `key=value`
  section with a mix of explicit and `DEFAULT`-able values,
  code for those are in @ref Master_info_file instead.

  Each value is an instance of an implementation of the
  @ref Info_file::Persistent interface. For convenience, they also have
  assignment and implicit conversion operators for their underlying types.
*/
struct Info_file
{
  IO_CACHE file;

  /// Persistence interface for an unspecified item
  struct Persistent
  {
    virtual ~Persistent()= default;
    // for save_to_file()
    virtual bool is_default() { return false; }
    /// @return `true` if the item is mandatory and couldn't provide a default
    virtual bool set_default() { return true; }
    /** set the value by reading a line from the IO and consume the `\n`
      @return `false` if the line has parsed successfully or `true` if error
      @post is_default() is `false`
    */
    virtual bool load_from(IO_CACHE *file)= 0;
    /** write the *effective* value to the IO **without** a `\n`
      (The caller will separately determine how
      to represent using the default value.)
    */
    virtual void save_to(IO_CACHE *file)= 0;
  };

  /** Integer Value
    @tparam I signed or unsigned integer type
    @see Master_info_file::Optional_int_value
      version with `DEFAULT` (not a subclass)
  */
  template<typename I> struct Int_value: Persistent
  {
    I value;
    operator I() { return value; }
    auto &operator=(I value)
    {
      this->value= value;
      return *this;
    }
    virtual bool load_from(IO_CACHE *file) override;
    virtual void save_to(IO_CACHE *file) override;
  };

  /// Null-Terminated String (usually file name) Value
  template<size_t size= FN_REFLEN> struct String_value: Persistent
  {
    char buf[size];
    /**
      Reads should consider this an immutable '\0'-terminated string (especially
      with @ref Optional_path_value where a `DEFAULT` may substitute the value).
      Writes may prefers to directly address the underlying @ref buf.
    */
    virtual operator const char *() { return buf; }
    /// @param other non-`nullptr` `\0`-terminated string
    auto &operator=(const char *other)
    {
      strmake(buf, other, size-1);
      return *this;
    }
    virtual bool load_from(IO_CACHE *file) override;
    virtual void save_to(IO_CACHE *file) override;
  };


  virtual ~Info_file()= default;
  virtual bool load_from_file()= 0;
  virtual void save_to_file()= 0;

  /**
    std::Mem_fn()-like nullable replacement for
    [member pointer upcasting](https://wg21.link/P0149R3)
  */
  struct Mem_fn: std::function<Persistent &(Info_file *self)>
  {
    /// Null Constructor
    Mem_fn(std::nullptr_t null= nullptr):
      std::function<Persistent &(Info_file *)>(null) {}
    /** Non-Null Constructor
      @tparam T CRTP subclass of Info_file
      @tparam M @ref Persistent subclass of the member
      @param pm member pointer
    */
    template<class T, typename M> Mem_fn(M T::* pm):
      std::function<Persistent &(Info_file *)>(
        [pm](Info_file *self) -> Persistent &
        { return self->*static_cast<M Info_file::*>(pm); }
      ) {}
  };

protected:
  /**
    (Re)load the MySQL line-based section from the @ref file
    @param value_list
      List of wrapped member pointers to values. The first element must be a
      file name @ref String_value to be unambiguous with the line count line.
    @param default_line_count
      We cannot simply read lines until EOF as all versions
      of MySQL/MariaDB may generate more lines than needed.
      Therefore, starting with MySQL/MariaDB 4.1.x for @ref Master_info_file and
      5.6.x for @ref Relay_log_info_file, the first line of the file is number
      of one-line-per-value lines in the file, including this line count itself.
      This parameter specifies the number of effective lines before those
      versions (i.e., not counting the line count line if it was to have one),
      where the first line is a filename with extension
      (either contains a `.` or is entirely empty) rather than an integer.
    @return `false` if the file has parsed successfully or `true` if error
  */
  template<size_t size> bool load_from_file(
    const Mem_fn (&value_list)[size],
    size_t default_line_count= 0
  ) { return load_from_file(value_list, size, default_line_count); }
  /**
    Flush the MySQL line-based section to the @ref file
    @param value_list List of wrapped member pointers to values.
    @param total_line_count
      The number of lines to describe the file as on the first line of the file.
      If this is larger than `value_list.size()`, suffix the file with empty
      lines until the line count (including the line count line) is this many.
      This reservation provides compatibility with MySQL,
      who has added more old-style lines while MariaDB innovated.
  */
  template<size_t size> void save_to_file(
    const Mem_fn (&value_list)[size],
    size_t total_line_count= size + /* line count line */ 1
  ) { return save_to_file(value_list, size, total_line_count); }

private:
  bool
  load_from_file(const Mem_fn *values, size_t size, size_t default_line_count);
  void save_to_file(const Mem_fn *values, size_t size, size_t total_line_count);
};


//TODO: Remove stub from mariadb-corporation/mariadb-columnstore-engine#3951
#ifndef RPL_MASTER_INFO_FILE_H
struct Master_info_file: Info_file
{
  /** General Optional Value
    @tparam T wrapped type
 */
  template<typename T> struct Optional_value: Persistent
  {
    std::optional<T> optional;
    virtual operator T()= 0;
    /// Fowards to @ref optional perfectly
    template<typename O> auto &operator=(O&& other)
    {
      optional= std::forward<O>(other);
      return *this;
    }
    bool is_default() override { return !optional.has_value(); }
    bool set_default() override
    {
      optional.reset();
      return false;
    }
  };

  /** Integer Value with `DEFAULT`
    @tparam mariadbd_option
      server options variable that determines the value of `DEFAULT`
    @tparam I integer type (auto-deduced from `mariadbd_option`)
    @see Int_value version without `DEFAULT` (not a superclass)
  */
  template<auto &mariadbd_option,
           typename I= std::remove_reference_t<decltype(mariadbd_option)>>
  struct Optional_int_value: Optional_value<I>
  {
    using Optional_value<I>::operator=;
    operator I() override
    { return Optional_value<I>::optional.value_or(mariadbd_option); }
    virtual bool load_from(IO_CACHE *file) override;
    virtual void save_to(IO_CACHE *file) override;
  };

  /**
    Optional Path Value (for SSL): @ref FN_REFLEN-sized '\0'-
    terminated string with a `mariadbd` option for the `DEFAULT`.
    @note This reuses the @ref String_value::buf to track the `DEFAULT`ed state,
      which is a bit more efficient and convenient than
      `std::optional<std::array<char, FN_REFLEN>>`.
      Specifically, when the strlen() is 0, the value is an empty string if
      the index 1 char is also '\0', or is set_default() if it is '\1'.
  */
  template<const char *&mariadbd_option>
  struct Optional_path_value: String_value<>
  {
    operator const char *() override
    {
      if (is_default())
        return mariadbd_option;
      return String_value<>::operator const char *();
    }
    /// @param other `\0`-terminated string, or `nullptr` to call set_default()
    Optional_path_value<mariadbd_option> &operator=(const char *other);
    bool is_default() override { return /* strlen() == 0 */ !buf[0] && buf[1]; }
    bool set_default() override
    {
      buf[0]= false;
      buf[1]= true;
      return false;
    }
    bool load_from(IO_CACHE *file) override
    {
      buf[1]= false; // not default
      return String_value<>::load_from(file);
    }
  };

  /** Boolean Value with `DEFAULT`.
    @note
    * This uses the @ref trilean enum,
      which is more efficient than `std::optional<bool>`.
    * load_from() and save_to() are also engineered
      to make use of the range of only two cases.
  */
  template<bool &mariadbd_option> struct Optional_bool_value: Persistent
  {
    trilean value;
    operator bool()
    { return is_default() ? mariadbd_option : (value != trilean::NO); }
    bool is_default() override { return value <= trilean::DEFAULT; }
    bool set_default() override
    {
      value= trilean::DEFAULT;
      return false;
    }
    auto &operator=(trilean other)
    {
      this->value= other;
      return *this;
    }
    auto &operator=(bool value)
    { return operator=(value ? trilean::YES : trilean::NO); }
    /// @return `true` if the line is `0` or `1`, `false` otherwise or on error
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };

  /** @ref uint32_t Array value
    @deprecated
      Only one of `DO_DOMAIN_IDS` and `IGNORE_DOMAIN_IDS` can be active
      at a time, so giving them separate arrays, let alone value instances,
      is wasteful. Until we refactor this pair, this will only reference
      to existing arrays to reduce changes that will be obsolete by then.
      As references, the struct does not manage (construct/destruct) the array.
  */
  struct ID_array_value: Persistent
  {
    /// Array of `long`s (FIXME: Domain and Server IDs should be `uint32_t`s.)
    DYNAMIC_ARRAY &array;
    ID_array_value(DYNAMIC_ARRAY &array): array(array) {}
    operator DYNAMIC_ARRAY &() { return array; }
    /// @pre @ref array is initialized
    bool load_from(IO_CACHE *file) override;
    /// Store the total number of elements followed by the individual elements.
    void save_to(IO_CACHE *file) override;
  };


  /**
    `@@master_info_file` values, in SHOW SLAVE STATUS order where applicable
    @{
  */

  String_value<HOSTNAME_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1> master_host;
  String_value<USERNAME_LENGTH + 1> master_user;
  // Not in SHOW SLAVE STATUS
  String_value<MAX_PASSWORD_LENGTH*SYSTEM_CHARSET_MBMAXLEN + 1> master_password;
  Int_value<uint32_t> master_port;
  /// Connect_Retry
  Optional_int_value<::master_connect_retry> master_connect_retry;
  String_value<> master_log_file;
  /// Read_Master_Log_Pos
  Int_value<my_off_t> master_log_pos;
  /// Master_SSL_Allowed
  Optional_bool_value<::master_ssl> master_ssl;
  /// Master_SSL_CA_File
  Optional_path_value<::master_ssl_ca> master_ssl_ca;
  /// Master_SSL_CA_Path
  Optional_path_value<::master_ssl_capath> master_ssl_capath;
  Optional_path_value<::master_ssl_cert> master_ssl_cert;
  Optional_path_value<::master_ssl_cipher> master_ssl_cipher;
  Optional_path_value<::master_ssl_key> master_ssl_key;
  Optional_bool_value<::master_ssl_verify_server_cert>
    master_ssl_verify_server_cert;
  /// Replicate_Ignore_Server_Ids
  ID_array_value ignore_server_ids;
  Optional_path_value<::master_ssl_crl> master_ssl_crl;
  Optional_path_value<::master_ssl_crlpath> master_ssl_crlpath;

  /** Singleton class of @ref Master_info_file::master_use_gtid:
    It is a @ref enum_master_use_gtid value
    with a `DEFAULT` value of @ref ::master_use_gtid,
    which in turn has a `DEFAULT` value based on @ref gtid_supported.
  */
  struct: Persistent
  {
    enum_master_use_gtid mode;
    /**
      The default `master_use_gtid` is normally `SLAVE_POS`; however, if the
      master does not supports GTIDs, we fall back to `NO`. This value caches
      the check so future RESET SLAVE commands don't revert to `SLAVE_POS`.
      load_from() and save_to() are engineered (that is, hard-coded)
      on the single-digit range of @ref enum_master_use_gtid,
      similar to Optional_bool_value.
    */
    bool gtid_supported= true;
    operator enum_master_use_gtid();
    operator bool()
    { return operator enum_master_use_gtid() != enum_master_use_gtid::NO; }
    auto &operator=(enum_master_use_gtid mode)
    {
      this->mode= mode;
      return *this;
    }
    bool is_default() override
    { return mode >= enum_master_use_gtid::DEFAULT; }
    bool set_default() override
    {
      mode= enum_master_use_gtid::DEFAULT;
      return false;
    }
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  }
  /// Using_Gtid
  master_use_gtid;

  /// Replicate_Do_Domain_Ids
  ID_array_value do_domain_ids;
  /// Replicate_Ignore_Domain_Ids
  ID_array_value ignore_domain_ids;
  Optional_int_value<::master_retry_count> master_retry_count;

  /** Singleton class of Master_info_file::master_heartbeat_period:
    It is a non-negative `DECIMAL(10,3)` seconds value internally
    calculated as an unsigned integer milliseconds value.
    It has a `DEFAULT` value of @ref ::master_heartbeat_period,
    which in turn has a `DEFAULT` value of `@@slave_net_timeout / 2` seconds.
  */
  struct Heartbeat_period_value: Optional_value<uint32_t>
  {
    /**
      @return std::numeric_limits<uint32_t>::max() / 1000.0
        as a constant '\0'-terminated string
    */
    static constexpr char MAX[]= "4294967.295";
    using Optional_value::operator=;
    operator uint32_t() override;
    static uint from_decimal(
      uint32_t &result, const decimal_t &decimal, bool &overprecise
    );
    static uint from_chars(
      std::optional<uint32_t> &self, const char *str,
      const char *str_end, bool &overprecise, char expected_end= '\n'
    );
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  }
  /// `Slave_heartbeat_period` of SHOW ALL SLAVES STATUS
  master_heartbeat_period;

  /// }@

  Master_info_file(
    DYNAMIC_ARRAY &ignore_server_ids,
    DYNAMIC_ARRAY &do_domain_ids, DYNAMIC_ARRAY &ignore_domain_ids
  );
  bool load_from_file() override;
  void save_to_file() override;
};
#endif // RPL_MASTER_INFO_FILE_H


struct Relay_log_info_file: Info_file
{
  /**
    `@@relay_log_info_file` values in SHOW SLAVE STATUS order
    @{
  */
  String_value<> relay_log_file;
  Int_value<my_off_t> relay_log_pos;
  /// Relay_Master_Log_File (of the event *group*)
  String_value<> read_master_log_file;
  /// Exec_Master_Log_Pos (of the event *group*)
  Int_value<my_off_t> read_master_log_pos;
  /// SQL_Delay
  Int_value<uint32_t> sql_delay;
  /// }@

  bool load_from_file() override;
  void save_to_file() override;
};


#endif // RPL_INFO_FILE_H
