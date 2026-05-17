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

#include "remote_event_stream.h"
#include <my_global.h> // required for `sql_common.h`; don't know which part
#include <m_string.h> // required for `sql_common.h` to fix "C++ in C linkage"
#include <sql_common.h> // `mysql.h`, mysql_reconnect()
#include <violite.h> // vio_close(), vio_shutdown()


void Remote_event_stream::Connection_options::operator()(MYSQL *connector) const
{
  //TODO: Connector/C deprecated mysql_options() in favor of `mysql_optionsv()`.
  /*
  mysql_optionsv(connector, MARIADB_OPT_HOST, host);
  mysql_optionsv(connector, MARIADB_OPT_USER, user);
  mysql_optionsv(connector, MARIADB_OPT_PASSWORD, password);
  mysql_optionsv(connector, MARIADB_OPT_PORT, &port);
  */
  connector->host= host; connector->user= user; connector->passwd= password;
  connector->port= port;
  //TODO: port Zero-Configuration TLS to Connector/C
  if (ssl_options && ssl_options->ssl_key[0] && ssl_options->ssl_cert[0])
  {
    mysql_ssl_set(connector,
      ssl_options->ssl_key, ssl_options->ssl_cert,
      ssl_options->ssl_ca, ssl_options->ssl_capath, ssl_options->ssl_cipher);
    mysql_options(connector, MYSQL_OPT_SSL_CRL, ssl_options->ssl_crl);
    mysql_options(connector, MYSQL_OPT_SSL_CRLPATH, ssl_options->ssl_crlpath);
    mysql_options(connector, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                  &ssl_options->ssl_verify_server_cert);
  }
  else
  /*
  {
    constexpr unsigned char NO= false;
    mysql_optionsv(connector, MYSQL_OPT_SSL_ENFORCE, &NO);
  }
  */
    connector->options.use_ssl= false;
  mysql_options(connector, MYSQL_SET_CHARSET_NAME, charset_name);
  // In case the master asks for an external authentication plugin
  mysql_options(connector, MYSQL_PLUGIN_DIR, plugin_dir);
}


Rpl_slave_connection::Rpl_slave_connection(
  const Connection_options &options, uint32_t timeout)
{
  connector= mysql_init(nullptr);
  if (!connector)
    return;
  mysql_options(connector, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(connector, MYSQL_OPT_READ_TIMEOUT, &timeout);
  options(connector);
}

Remote_event_stream::Remote_event_stream(
  const Connection_options &options, uint32_t timeout,
  unsigned long max_allowed_packet): Rpl_slave_connection(options, timeout)
{
  if (!connector)
    return;
  constexpr unsigned char YES= true;
  //@deprecated not applicable with Connector/C
  mysql_options(connector, MYSQL_OPT_USE_THREAD_SPECIFIC_MEMORY, &YES);
  //mysql_options(connector, MYSQL_OPT_MAX_ALLOWED_PACKET, &max_allowed_packet);
  connector->options.max_allowed_packet= max_allowed_packet;
  mysql_options(connector, MYSQL_OPT_RECONNECT, &YES);
}

Semi_sync_graceful_killer::Semi_sync_graceful_killer(
  const Connection_options &options, uint32_t timeout):
  Rpl_slave_connection(options, timeout)
{
  if (connector)
    mysql_options(connector, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
}

Rpl_slave_connection::~Rpl_slave_connection()
{
  if (connector)
  {
    vio_close(connector->net.vio); //@deprecated already covered by Connector/C
    mysql_close(connector);
  }
}


unsigned int Remote_event_stream::errnum() { return mysql_errno(connector); }
const char *Remote_event_stream::errmsg() { return mysql_error(connector); }

/** Call mysql_real_connect() with matching properties in the struct
  TODO: Refactor Connector/C to avoid re-passing these parameters
*/
bool Rpl_slave_connection::connect(unsigned long flags)
{
  return !mysql_real_connect(connector,
    connector->host, connector->user, connector->passwd, connector->db,
    connector->port, connector->unix_socket, flags);
}

bool Remote_event_stream::connect(bool compress)
{
  if (do_reconnect)
    return mysql_reconnect(connector);
  do_reconnect= true;
  return Rpl_slave_connection::connect(
    CLIENT_REMEMBER_OPTIONS | (compress ? CLIENT_COMPRESS : 0));
}


bool Rpl_slave_connection::real_query(const char *query, size_t strlen)
{ return mysql_real_query(connector, query, strlen); }

unsigned long Remote_event_stream::master_version()
{ return mysql_get_server_version(connector); }

MYSQL_RES_P Remote_event_stream::store_result()
{ return mysql_store_result(connector); }
char **Remote_event_stream::fetch_row(MYSQL_RES_P query_result)
{ return mysql_fetch_row(query_result); }
void Remote_event_stream::free_result(MYSQL_RES_P query_result)
{ return mysql_free_result(query_result); }

bool Remote_event_stream::send_command(
  int command, const unsigned char *args, size_t strlen, bool skip_check)
{
  return simple_command(connector, static_cast<enum_server_command>(command),
    args, static_cast<unsigned long>(strlen), skip_check);
}

unsigned long Remote_event_stream::thread_id()
{ return connector->thread_id; }


///TODO: Split this part from Connector/C's mariadb_rpl_fetch()
std::basic_string_view<unsigned char> Remote_event_stream::next()
{
  auto strlen= static_cast<size_t>(cli_safe_read(connector));
  return {connector->net.read_pos, strlen};
}

bool Remote_event_stream::semisync_ack(
  const std::string_view log_name, uint64_t next_pos)
{
  constexpr size_t HEAD_SIZE= /* Semi-Sync Header */1 + sizeof(next_pos);
  char payload[HEAD_SIZE + (FN_REFLEN+1)]= {'\xEF'};
  int8store(&(payload[1]), next_pos);
  size_t strlen=
    log_name.copy(&(payload[HEAD_SIZE]), sizeof(payload)-HEAD_SIZE);
  NET *net= &connector->net;
  //@deprecated: not required in Connector/C
  net->pkt_nr_can_be_reset= true;
  // Connector/C might not require resetting; better be safe until confirmed.
  net_clear(net, false);
  return my_net_write(net, reinterpret_cast<const unsigned char *>(payload),
    HEAD_SIZE + (strlen+1)) || net_flush(net);
}

void Remote_event_stream::abort()
{ vio_shutdown(connector->net.vio, SHUT_RDWR); }
