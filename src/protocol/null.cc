/* Copyright 2011 Nick Mathewson, George Kadianakis
 * Copyright 2011 SRI International
 * See LICENSE for other credits and copying information
 */

#include "util.h"
#include "cpp.h"
#include "connections.h"
#include "protocol.h"
#include "modus_operandi.h"

#include <event2/buffer.h>
#include <event2/event.h>

namespace {
  struct null_config_t : config_t
  {
    struct evutil_addrinfo *listen_addr;
    struct evutil_addrinfo *target_addr;

    CONFIG_DECLARE_METHODS(null);

   DISALLOW_COPY_AND_ASSIGN(null_config_t);
  };

  struct null_circuit_t;

  struct null_conn_t : conn_t
  {
    null_config_t *config;
    null_circuit_t *upstream;

    CONN_DECLARE_METHODS(null);

   DISALLOW_COPY_AND_ASSIGN(null_conn_t);
  };

  struct null_circuit_t : circuit_t
  {
    null_config_t *config;
    null_conn_t *downstream;

    CIRCUIT_DECLARE_METHODS(null);

   DISALLOW_COPY_AND_ASSIGN(null_circuit_t);
  };
}

PROTO_DEFINE_MODULE(null);

null_config_t::null_config_t()
{
}

null_config_t::~null_config_t()
{
  if (this->listen_addr)
    evutil_freeaddrinfo(this->listen_addr);
  if (this->target_addr)
    evutil_freeaddrinfo(this->target_addr);
}

bool
null_config_t::is_good(modus_operandi_t &mo)
{
  /* could be improved; but this is a good first sanity check */
  return mo.protocol() == "null"     &&
    !mo.mode().empty()               &&
    !mo.up_address().empty();
}

bool
null_config_t::init(int n_options, const char *const *options, modus_operandi_t &mo)
{
  const char* defport;

  const char* cmode;

  if (!mo.is_ok() && n_options < 1)
    goto usage;


  if(mo.is_ok() && n_options != 0){
    log_warn("Starting with both a configuration file *and* commandline options is *currently* not supported. Sorry.");
    return false;
  }
  
  if(!this->is_good(mo) && n_options == 0){
    log_warn("Configuration file not good enough for null (needs mode, and up_address)! Sorry.");
    return false;
  }

  cmode = mo.is_ok() ? mo.mode().c_str() : options[0];

   if (!strcmp(cmode, "client")) {
    defport = "48988"; // bf5c
    mode = LSN_SIMPLE_CLIENT;
  } else if (!strcmp(cmode, "socks")) {
    defport = "23548"; // 5bf5
    mode = LSN_SOCKS_CLIENT;
  } else if (!strcmp(cmode, "server")) {
    defport = "11253"; // 2bf5
    mode = LSN_SIMPLE_SERVER;
  } else
    goto usage;



  if(mo.is_ok()){
    vector<string> addresses;
    
    this->listen_addr  = resolve_address_port(mo.up_address().c_str(), 1, 1, defport);
    
    if (!this->listen_addr) {
      log_warn("chop: invalid up address: %s", options[1]);
      goto usage;
    }
    
    addresses = mo.down_addresses();
    
    if(this->mode != LSN_SOCKS_CLIENT) {
      if(addresses.size() > 0){
        this->target_addr = resolve_address_port(addresses[0].c_str(), 1, 0, NULL);
      }
      if (!this->target_addr)
        goto usage;
    }


  } else {

    if (n_options != (this->mode == LSN_SOCKS_CLIENT ? 2 : 3))
      goto usage;
    
    this->listen_addr = resolve_address_port(options[1], 1, 1, defport);
    if (!this->listen_addr)
      goto usage;
    
    if (this->mode != LSN_SOCKS_CLIENT) {
      this->target_addr = resolve_address_port(options[2], 1, 0, NULL);
      if (!this->target_addr)
        goto usage;
    }
    
  }
  return true;

  return true;

 usage:
  log_warn("null syntax:\n"
           "\tnull <mode> <listen_address> [<target_address>]\n"
           "\t\tmode ~ server|client|socks\n"
           "\t\tlisten_address, target_address ~ host:port\n"
           "\ttarget_address is required for server and client mode,\n"
           "\tand forbidden for socks mode.\n"
           "Examples:\n"
           "\tstegotorus null socks 127.0.0.1:5000\n"
           "\tstegotorus null client 127.0.0.1:5000 192.168.1.99:11253\n"
           "\tstegotorus null server 192.168.1.99:11253 127.0.0.1:9005");
  return false;
}

/** Retrieve the 'n'th set of listen addresses for this configuration. */
struct evutil_addrinfo *
null_config_t::get_listen_addrs(size_t n) const
{
  if (n > 0)
    return 0;
  return this->listen_addr;
}

/* Retrieve the target address for this configuration. */
struct evutil_addrinfo *
null_config_t::get_target_addrs(size_t n) const
{
  if (n > 0)
    return 0;
  return this->target_addr;
}

/* Create a circuit object. */
circuit_t *
null_config_t::circuit_create(size_t)
{
  null_circuit_t *ckt = new null_circuit_t;
  ckt->config = this;
  return ckt;
}

null_circuit_t::null_circuit_t()
  : downstream(NULL)
{
}

null_circuit_t::~null_circuit_t()
{
}

void
null_circuit_t::close()
{
  if (downstream) {
    /* break the circular reference before deallocating the
       downstream connection */
    downstream->upstream = NULL;
    conn_do_flush(downstream);
    downstream = NULL;
  }

  circuit_t::close();
}

config_t *
null_circuit_t::cfg() const
{
  return this->config;
}

/* Add a connection to this circuit. */
void
null_circuit_t::add_downstream(conn_t *cn)
{
  null_conn_t *conn = dynamic_cast<null_conn_t *>(cn);
  log_assert(conn);
  log_assert(!conn->upstream);
  log_assert(!this->downstream);

  this->downstream = conn;
  conn->upstream = this;

  log_debug(this, "added connection <%d.%d> to %s",
            this->serial, conn->serial, conn->peername);
}

/* Drop a connection from this circuit.  If this happens in this
   protocol, it is because of a network error, and the whole circuit
   should be closed.  */
void
null_circuit_t::drop_downstream(conn_t *cn)
{
  null_conn_t *conn = dynamic_cast<null_conn_t *>(cn);
  log_assert(conn);
  log_assert(this->downstream == conn);
  log_assert(conn->upstream == this);

  log_debug(this, "dropped connection <%d.%d> to %s",
            this->serial, conn->serial, conn->peername);
  this->downstream = NULL;
  conn->upstream = NULL;
  circuit_do_flush(this);
}

/* Send data from the upstream buffer. */
int
null_circuit_t::send()
{
  log_debug(this, "sending %lu bytes",
            (unsigned long)
            evbuffer_get_length(bufferevent_get_input(this->up_buffer)));

  return evbuffer_add_buffer(this->downstream->outbound(),
                             bufferevent_get_input(this->up_buffer));
}

/* Send an EOF on this circuit. */
int
null_circuit_t::send_eof()
{
  if (this->downstream)
    conn_send_eof(this->downstream);
  this->read_eof = true;
  return 0;
}

/*
  This is called everytime we get a connection for the null
  protocol.
*/

conn_t *
null_config_t::conn_create(size_t)
{
  null_conn_t *conn = new null_conn_t;
  conn->config = this;
  return conn;
}

null_conn_t::null_conn_t()
  : upstream(NULL)
{
}

null_conn_t::~null_conn_t()
{
}

void
null_conn_t::close()
{
  if (this->upstream)
    this->upstream->drop_downstream(this);

  conn_t::close();
}

/* Only used by connection callbacks */
circuit_t *
null_conn_t::circuit() const
{
  return upstream;
}

/** Null inbound-to-outbound connections are 1:1 */
int
null_conn_t::maybe_open_upstream()
{
  null_circuit_t *ckt = dynamic_cast<null_circuit_t *>
    (circuit_create(this->config, 0));
  if (!ckt)
    return -1;

  ckt->add_downstream(this);
  circuit_open_upstream(ckt);
  return 0;
}

/** Null has no handshake */
int
null_conn_t::handshake()
{
  return 0;
}

/** Receive data from connection SOURCE */
int
null_conn_t::recv()
{
  log_assert(this->upstream);
  log_debug(this, "receiving %lu bytes",
            (unsigned long)evbuffer_get_length(this->inbound()));
  return evbuffer_add_buffer(bufferevent_get_output(this->upstream->up_buffer),
                             this->inbound());
}

/** Receive EOF from connection SOURCE */
int
null_conn_t::recv_eof()
{
  if (this->upstream) {
    if (evbuffer_get_length(this->inbound()) > 0)
      if (this->recv())
        return -1;

    circuit_recv_eof(this->upstream);
  }
  return 0;
}

CONFIG_STEG_STUBS(null);
CONN_STEG_STUBS(null);
