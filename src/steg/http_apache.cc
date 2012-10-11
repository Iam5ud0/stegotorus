#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <curl/curl.h>
#include <vector>
#include <sstream>

using namespace std;

#include "util.h"
#include "curl_util.h"
#include "connections.h"
#include "protocol.h"
#include "steg.h"
#include "rng.h"


#include "payload_server.h"
#include "trace_payload_server.h"
#include "apache_payload_server.h"

#include "cookies.h"
#include "swfSteg.h"
#include "pdfSteg.h"
#include "jsSteg.h"
#include "base64.h"
#include "b64cookies.h"

#include "http.h"

enum op_apache_steg_code
  {
    op_STEG_NO_OP,
    op_STEG_DICT_MAC,
    op_STEG_DICT_UP2DATE,
    op_STEG_DICT_UPDATE,
    op_STEG_DICT_WAIT_PEER,
  };


namespace  {
  struct http_apache_steg_config_t : http_steg_config_t
  {

    CURLM *_curl_multi_handle; //this is the  curl handle to manange nonblocking 
                               //connections used to communicate with http server
    int _curl_running_handle; //number of concurrent transfer

    unsigned long uri_byte_cut; /* The number of byte of the message that
                                    can be stored in url */

    op_apache_steg_code _cur_operation = op_STEG_NO_OP;

    bool uri_dict_up2date = false;
    static const char c_end_of_dict[];
    
    stringstream dict_stream; /*we receive the dictionary in form
                                of a stream */

     /* Client side communication */
     /** 
         Uses the server side database to init the URI dictionary. This is only for test purpose. return true if it succeeds

     */
     bool init_uri_dict();

    //Dictionary communications
    virtual size_t process_protocol_data();
    /** Writes the SHA256 mac of the uri_dict into
        the porotocol_buffer to send it to the peep

        return true in the case of success
    */
    bool send_dict_mac();

    /** 
        write the uri dict in a protocol_data to be send to the client
        @return the number of bytes written in the buffer
    */
    size_t send_dict_to_peer();


    STEG_CONFIG_DECLARE_METHODS(http_apache);
  };

  struct http_apache_steg_t : http_steg_t
  {

    const size_t c_min_uri_length = 1;
    const size_t c_max_uri_length = 2000; //Unofficial cap

    CURL* _curl_easy_handle;
     
    http_apache_steg_config_t* _apache_config;

    /**
        constructors and destructors taking care of curl initialization and
        clean-up
    */
    STEG_DECLARE_METHODS(http_apache);

    http_apache_steg_t(http_apache_steg_config_t *cf, conn_t *cn);

     virtual int http_client_uri_transmit (struct evbuffer *source, conn_t *conn);
     virtual int http_server_receive(conn_t *conn, struct evbuffer *dest, struct evbuffer* source);

     virtual int http_server_receive_cookie(char* p, struct evbuffer *dest);
     virtual int http_server_receive_uri(char *p, struct evbuffer *dest);
  
    /**
       We curl tries to open a socket, it calls this function which
       returns the socket which already has been opened using 
       eventbuffer_socket_connect at the time of creation of the
       connection.
     */
    static curl_socket_t get_conn_socket(void *conn,
                                curlsocktype purpose,
                                         struct curl_sockaddr *address);

    static int sockopt_callback(void *clientp, curl_socket_t curlfd,
                                curlsocktype purpose);

    static int ignore_close(void *clientp, curl_socket_t curlfd);

    static void curl_socket_event_cb(int fd, short kind,  void *userp);

  };

}

const char http_apache_steg_config_t::c_end_of_dict[] = "\r\n";

STEG_DEFINE_MODULE(http_apache);

http_apache_steg_config_t::http_apache_steg_config_t(config_t *cfg)
  : http_steg_config_t(cfg, false)
{
  string payload_filename;
  if (is_clientside)
    payload_filename = "apache_payload/client_list.txt";
  else
    payload_filename = "apache_payload/server_list.txt";
  
  payload_server = new ApachePayloadServer(is_clientside ? client_side : server_side, payload_filename);

  if (!is_clientside) {//on server side the dictionary is ready to be used
    size_t no_of_uris = ((ApachePayloadServer*)payload_server)->uri_dict.size();
    for(uri_byte_cut = 0; (no_of_uris /=256) > 0; uri_byte_cut++);
  }

  if (!(_curl_multi_handle = curl_multi_init()))
    log_abort("failed to initiate curl multi object.");

  if (!(protocol_data_in || protocol_data_out))
    log_abort("failed to allocate evbuffer for protocol data");

}

http_apache_steg_config_t::~http_apache_steg_config_t()
{
  //delete payload_server; maybe we don't need it
  /* always cleanup */ 
  curl_multi_cleanup(_curl_multi_handle);

}

steg_t *
http_apache_steg_config_t::steg_create(conn_t *conn)
{
  return new http_apache_steg_t(this, conn);
}

http_apache_steg_t::http_apache_steg_t(http_apache_steg_config_t *cf, conn_t *cn)
  : http_steg_t((http_steg_config_t*)cf, cn), _apache_config(cf)
{

  if (!_apache_config->payload_server)
    log_abort("payload server is not initialized.");

  //we need to use a fresh curl easy object because we might have
  //multiple connection at a time. FIX ME:It might be a way to re-
  //cycle these objects
  _curl_easy_handle = curl_easy_init();
  if (!_curl_easy_handle)
    log_abort("failed to initiate curl");

  curl_easy_setopt(_curl_easy_handle, CURLOPT_HEADER, 1L);
  curl_easy_setopt(_curl_easy_handle, CURLOPT_HTTP_CONTENT_DECODING, 0L);
  curl_easy_setopt(_curl_easy_handle, CURLOPT_HTTP_TRANSFER_DECODING, 0L);
  curl_easy_setopt(_curl_easy_handle, CURLOPT_VERBOSE, 1L);
  //curl_easy_setopt(_curl_easy_handle, CURLOPT_WRITEFUNCTION, read_data_cb);
  //Libevent should be able to take care of this we might need to
  //discard data if it starts writing on stdout
  curl_easy_setopt(_curl_easy_handle, CURLOPT_OPENSOCKETFUNCTION, get_conn_socket);
  curl_easy_setopt(_curl_easy_handle, CURLOPT_OPENSOCKETDATA, conn);
  //tells curl the socket is already connected
  curl_easy_setopt(_curl_easy_handle, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
  curl_easy_setopt(_curl_easy_handle, CURLOPT_CLOSESOCKETFUNCTION, ignore_close);

  /** setup the buffer we communicate with chop */
  //Every connection checks if the dict is valid
  if (_apache_config->is_clientside && !_apache_config->uri_dict_up2date
      && _apache_config->_cur_operation == op_STEG_NO_OP) { //Request for uri dict validation
    _apache_config->_cur_operation = op_STEG_DICT_WAIT_PEER;
    char status_to_send = op_STEG_DICT_MAC;
    evbuffer_add(_apache_config->protocol_data_out, &status_to_send, 1);
    evbuffer_add(_apache_config->protocol_data_out, ((ApachePayloadServer*)_apache_config->payload_server)->uri_dict_mac(), SHA256_DIGEST_LENGTH);
  }

}

int
http_apache_steg_t::http_client_uri_transmit (struct evbuffer *source, conn_t *conn)
{

  size_t sbuflen = evbuffer_get_length(source);

  char* data;
  char* data2 = (char*) xmalloc (sbuflen*4);
  size_t len;

  // '+' -> '-', '/' -> '_', '=' -> '.' per
  // RFC4648 "Base 64 encoding with RL and filename safe alphabet"
  // (which does not replace '=', but dot is an obvious choice; for
  // this use case, the fact that some file systems don't allow more
  // than one dot in a filename is irrelevant).

  //First make the evbuffer data like a normal buffor
  data = (char*) evbuffer_pullup(source, sbuflen);
  if (!data) {
    log_debug("evbuffer_pullup failed");
    return -1;
  }

  //Extra log info in case the decryption fails
 /*string hex_data; buf2hex((uint8_t*)data, sbuflen, hex_data);
    log_debug(conn, "Enc data to send: %s", hex_data.c_str());*/

  /*First we need to cut the first few bytes into the url */
  string chosen_url;
  //If the uri dict has no element we always can request / uri 
  //also before we make sure that our uri dict is in sync with 
  //server, we shouldn't use it because it will corrupt the 
  //coding
  if (!(((ApachePayloadServer*)_apache_config->payload_server)->uri_dict.size() && _apache_config->uri_dict_up2date)) {
    log_debug("Synced uri dict is not available yet");
    chosen_url = "";
  }
  else {
    unsigned long url_index = 0;
    //memcpy((void*)&url_index, data, uri_byte_cut); this machine dependent
    for(unsigned int i = 0; i < _apache_config->uri_byte_cut && i < sbuflen; i++)
      {
        url_index *=256;
        url_index += (uint8_t) data[i];
        log_debug("uri index so far %lu", url_index);
      }
    
    chosen_url= ((ApachePayloadServer*)_apache_config->payload_server)->uri_dict[url_index].URL;
  }

  type = ((ApachePayloadServer*)_apache_config->payload_server)->find_url_type(chosen_url.c_str());

  string uri_to_send("http://");
  string test_uri("http://127.0.0.1");
  if (sbuflen > _apache_config->uri_byte_cut)
    {
      sbuflen -= _apache_config->uri_byte_cut;
      data += _apache_config->uri_byte_cut;

      //Now we encode the rest in a paramter in the uri
      base64::encoder E(false, '-', '_', '.');

      memset(data2, 0, sbuflen*4);
      len  = E.encode(data, sbuflen, data2);
      len += E.encode_end(data2+len);

      uri_to_send += conn->peername;
      uri_to_send += "/"+ chosen_url + "?q=" + data2;
      test_uri += "/" + chosen_url + "?q=" + data2;

      if (uri_to_send.size() > c_max_uri_length)
        {
          log_debug("%lu too big to be send in uri", uri_to_send.size());
            return -1;
        }
   
    }
  else
    {
      //the buffer is too short we need to indicate the number
      //bytes. But this probably never happens
      sprintf(data2,"%lu", sbuflen);
      uri_to_send = chosen_url + "?p=";
      uri_to_send += sbuflen;

    }
  
  //now we are using curl to send the request
  curl_easy_setopt(_curl_easy_handle, CURLOPT_URL, uri_to_send.c_str());
  curl_easy_setopt(_curl_easy_handle, CURLOPT_WRITEFUNCTION,   discard_data);
  curl_easy_setopt(_curl_easy_handle, CURLOPT_WRITEDATA,       conn);

  CURLMcode res = curl_multi_add_handle(_apache_config->_curl_multi_handle, _curl_easy_handle);

  if (res != CURLM_OK) {
    log_debug(conn,"error in adding curl handle. CURL Error %s", curl_multi_strerror(res));
  }

  bufferevent_disable(conn->buffer, EV_WRITE);
  event* write_url_event = event_new(bufferevent_get_base(conn->buffer), conn->socket(), EV_WRITE, curl_socket_event_cb, this);
  event_add(write_url_event, NULL);

  log_debug(conn, "curl is fetching %s", uri_to_send.c_str());
  log_debug(conn, "%u handles are still running",_apache_config->_curl_running_handle);

   have_transmitted = 1;

  //FIX ME I need to clean-up the easy handle but I don't know
  //where should I do it. If I keep track of all easy handle
  //and recycle them it also will help with clean-up.
  // while((res = curl_multi_perform(_apache_config->_curl_multi_handle, &_apache_config->_curl_running_handle)) == CURLM_CALL_MULTI_PERFORM);

   return 0;

}

int
http_apache_steg_t::http_server_receive(conn_t *conn, struct evbuffer *dest, struct evbuffer* source) {

  char* data;
  int type;

  do {
    struct evbuffer_ptr s2 = evbuffer_search(source, "\r\n\r\n", sizeof ("\r\n\r\n") -1 , NULL);
    char *p;

    //int cookie_mode = 0;

     if (s2.pos == -1) {
      log_debug(conn, "Did not find end of request %d",
                (int) evbuffer_get_length(source));
      return RECV_INCOMPLETE;
    }

     log_debug(conn, "SERVER received request header of length %d", (int)s2.pos);

    data = (char*) evbuffer_pullup(source, s2.pos+4);

    if (data == NULL) {
      log_debug(conn, "SERVER evbuffer_pullup fails");
      return RECV_BAD;
    }

    data[s2.pos+3] = 0;

    type = _apache_config->payload_server->find_uri_type((char *)data, s2.pos+4);
    if (type == -1) { //If we can't recognize the type we assign a random type
      //type = rng_int(NO_CONTENT_TYPES) + 1; //For now, till we decide about the type
      log_debug("Could not recognize request type. Assume html");
      type = HTTP_CONTENT_HTML; //Fail safe to html
    }

    if (strstr((char*) data, "Cookie") != NULL) {
      p = strstr((char*) data, "Cookie:") + sizeof "Cookie: "-1;
      //cookie_mode = 1;
      if (http_server_receive_cookie(p, dest) == RECV_BAD)
        return RECV_BAD;
    }
    else
      {
        p = data + sizeof "GET /" -1;
        http_server_receive_uri(p, dest);
      }

    evbuffer_drain(source, s2.pos + sizeof("\r\n\r\n") - 1);
  } while (evbuffer_get_length(source));

  have_received = 1;
  this->type = type;

  // FIXME: should decide whether or not to do this based on the
  // Connection: header.  (Needs additional changes elsewhere, esp.
  // in transmit_room.)
  conn->expect_close();

  conn->transmit_soon(100);
  return RECV_GOOD;
}

int
http_apache_steg_t::http_server_receive_cookie(char* p, evbuffer* dest)
{
    char outbuf[MAX_COOKIE_SIZE * 3/2];
    char outbuf2[MAX_COOKIE_SIZE];
    char *pend;

    size_t sofar;

    log_debug("Cookie: %s", p);
    pend = strstr(p, "\r\n");
    log_assert(pend);
    if (pend - p > MAX_COOKIE_SIZE * 3/2)
      log_abort(conn, "cookie too big: %lu (max %lu)",
                (unsigned long)(pend - p), (unsigned long)MAX_COOKIE_SIZE);

    memset(outbuf, 0, sizeof(outbuf));
    size_t cookielen = unwrap_b64_cookies(outbuf, p, pend - p);

    base64::decoder D('-', '_', '.');
    memset(outbuf2, 0, sizeof(outbuf2));
    sofar = D.decode(outbuf, cookielen+1, outbuf2);

    if (sofar <= 0)
      log_warn(conn, "base64 decode failed\n");

    if (sofar >= MAX_COOKIE_SIZE)
      log_abort(conn, "cookie decode buffer overflow\n");

    if (evbuffer_add(dest, outbuf2, sofar)) {
      log_debug(conn, "Failed to transfer buffer");
      return RECV_BAD;
    }

    return RECV_GOOD;

}

int
http_apache_steg_t::http_server_receive_uri(char *p, evbuffer* dest)
{
    char outbuf[MAX_COOKIE_SIZE * 3/2];
    char outbuf2[MAX_COOKIE_SIZE];
    char *uri_end;

    size_t sofar = 0;

    log_debug(conn, "uri: %s", p);
    uri_end = strchr(p, ' ');
    log_assert(uri_end);
    if ((size_t)(uri_end - p) > c_max_uri_length * 3/2)
      log_abort(conn, "uri too big: %lu (max %lu)",
                (unsigned long)(uri_end - p), (unsigned long)c_max_uri_length);

    memset(outbuf, 0, sizeof(outbuf));
    char* url_end = strstr(p, "?");
    string extracted_url = string(p, url_end - p);
    unsigned long url_code = 0;
    size_t url_meaning_length = 0;
    bool param_valid_load = true;
    if (extracted_url != "") { 
       //Otherwise the uri_dict sync hasn't been verified so 
      //we can't use it
      url_code = ((ApachePayloadServer*)_apache_config->payload_server)->uri_decode_book[extracted_url];
      log_debug(conn, "url code %lu", url_code);

      if (*(url_end + sizeof("?") - 1) == 'p') { //all info are coded in url
        
        url_meaning_length = atoi(url_end + sizeof("?p") -1);
        param_valid_load = false;
      }
      else
        url_meaning_length = _apache_config->uri_byte_cut;
    }
        
    for(size_t i = 0; i < url_meaning_length; i++)
    {
      log_debug(conn, "url byte %u", (uint8_t)(url_code % 256));
      outbuf2[i] = (uint8_t)(url_code % 256);
      url_code /= 256;
    }

    if (param_valid_load)
      {
        char* param_val_begin = url_end+sizeof("?q=")-1;

        memset(outbuf, 0, sizeof(outbuf));
        size_t cookielen = unwrap_b64_cookies(outbuf, param_val_begin, uri_end - param_val_begin);

         base64::decoder D('-', '_', '.');
        memset(outbuf2+url_meaning_length, 0, sizeof(outbuf2) - url_meaning_length);
        sofar = D.decode(outbuf, cookielen+1, outbuf2+url_meaning_length);

        if (sofar <= 0)
          log_warn(conn, "base64 decode failed\n");

        if (sofar >= c_max_uri_length)
          log_abort(conn, "uri decode buffer overflow\n");
      }

    //Extra logging in the case decryption failure
    /*string hex_data; buf2hex((uint8_t*)outbuf2, sofar+url_meaning_length, hex_data);
      log_debug(conn, "Enc data received: %s", hex_data.c_str());*/

    if (evbuffer_add(dest, outbuf2, sofar+url_meaning_length)) {
      log_debug(conn, "failed to transfer buffer");
      return RECV_BAD;
    }

    return RECV_GOOD;
}

http_apache_steg_t::~http_apache_steg_t()
{
  curl_easy_cleanup(_curl_easy_handle);

}

bool http_apache_steg_config_t::init_uri_dict()
{
  /* We need to call this explicitly because it is only called by the payload
     server on the server side, but we want to use it on the client side */
  if (!((ApachePayloadServer*)payload_server)->init_uri_dict())
    return false;

  size_t no_of_uris = ((ApachePayloadServer*)payload_server)->uri_dict.size();
  for(uri_byte_cut = 0; (no_of_uris /=256) > 0; uri_byte_cut++);

  return true;
}

steg_config_t *
http_apache_steg_t::cfg()
{
  return config;
}

size_t
http_apache_steg_t::transmit_room(size_t pref, size_t lo, size_t hi)
{
  log_debug("computing available room of type %u", type);
  if (have_transmitted)
    /* can't send any more on this connection */
    return 0;

  if (config->is_clientside) {
    // MIN_COOKIE_SIZE and MAX_COOKIE_SIZE are *after* base64'ing
    if (lo < c_min_uri_length * 3/4)
      lo = c_min_uri_length *3/4;

    if (hi > c_max_uri_length*1/2)
      hi = c_max_uri_length*1/2;
    
    if (hi < lo)
      log_abort("hi<lo: client=%d type=%d hi=%ld lo=%ld",
                config->is_clientside, type,
                (unsigned long)hi, (unsigned long)lo);

    return clamp(pref + rng_range_geom(hi - lo, 8), lo, hi);

  }

  //Server side
  size_t cur_type_room  = http_steg_t::transmit_room(pref,  lo, hi);
  return cur_type_room;
  //bool type_checked[NO_CONTENT_TYPES];
  //if one type doesn't have enough room we check other type
  //FIX: TODO This I need to make client detect the type based 
  //on received content, for now we need to stick to clients
  //request
  /*for(uint8_t no_tries = 0; no_tries < NO_CONTENT_TYPES; no_tries++)
    {
      if ((cur_type_room = http_steg_t::transmit_room(pref,  lo, hi)))
          return cur_type_room;
      else {
        do {
          log_debug("type %ui does not have room. changing type...", type);
          type = rng_int(NO_CONTENT_TYPES) + 1;
        } while(type_checked[type]);
        log_debug("now using type %ui", type);
      }
      }
  //if reach here, is because all types were disappointing
  return 0;*/
}

int
http_apache_steg_t::transmit(struct evbuffer *source)
{
  if (config->is_clientside) {
      return http_client_uri_transmit(source, conn);
      //return http_client_cookie_transmit(source, conn);
  }
  else
    return http_steg_t::transmit(source);
}

int
http_apache_steg_t::receive(struct evbuffer *dest)
{
  return http_steg_t::receive(dest);
}

size_t
http_apache_steg_config_t::process_protocol_data()
{
  char status_to_send;
  size_t avail = evbuffer_get_length(protocol_data_in);
  evbuffer_ptr fin_location;
  log_assert(avail); //do not call process protocol if there's no data

  //because data comes in batches we need to keep track
  //the operation till we gets all data that we expected
  if ((_cur_operation == op_STEG_NO_OP) || (_cur_operation == op_STEG_DICT_WAIT_PEER))
    evbuffer_remove(protocol_data_in, &_cur_operation, 1);

  switch (_cur_operation) {
  case op_STEG_DICT_MAC:
    //server side
    avail = evbuffer_get_length(protocol_data_in);
    if (avail >= SHA256_DIGEST_LENGTH) {
      _cur_operation = op_STEG_NO_OP;
      char peer_dict_mac[SHA256_DIGEST_LENGTH];
      evbuffer_remove(protocol_data_in, &peer_dict_mac, SHA256_DIGEST_LENGTH);
      
      if (!memcmp(peer_dict_mac, ((ApachePayloadServer*)payload_server)->uri_dict_mac(), SHA256_DIGEST_LENGTH)) { //Macs matches just acknowledge that.
        status_to_send = op_STEG_DICT_UP2DATE;
        evbuffer_add(protocol_data_out, &status_to_send, 1);
        _cur_operation = op_STEG_NO_OP;
        log_debug("Peer's uri dict is synced with ours");
        return 1;
      }
      else //send the entire dict to the client.
        return send_dict_to_peer();
    }
    return 0; //not enough bytes
  case op_STEG_DICT_UP2DATE:
    {
      //client side
      uri_dict_up2date = true;

      size_t no_of_uris = ((ApachePayloadServer*)payload_server)->uri_dict.size();
      for(uri_byte_cut = 0; (no_of_uris /=256) > 0; uri_byte_cut++);

      _cur_operation = op_STEG_NO_OP;
      log_debug("peer's uri dict is synced with ours");
      return 0;
    }
        
  case op_STEG_DICT_UPDATE:
    //client side
    {
      size_t fin_len = strlen(http_apache_steg_config_t::c_end_of_dict);
      fin_location = evbuffer_search(protocol_data_in, http_apache_steg_config_t::c_end_of_dict, fin_len, NULL);

      if (fin_location.pos != -1)
        { size_t dict_buf_size = evbuffer_get_length(protocol_data_in);
          log_debug("uri dict of size %lu completely received", dict_buf_size+sizeof(_cur_operation)); 
          char* dict_buf = new char[dict_buf_size];
          evbuffer_remove(protocol_data_in, dict_buf, dict_buf_size);
          
          stringstream dict_str_stream;
          dict_str_stream.write(dict_buf, dict_buf_size-fin_len);
          
          ((ApachePayloadServer*)payload_server)->init_uri_dict((iostream&)dict_str_stream);
          ((ApachePayloadServer*)payload_server)->store_dict(dict_buf, dict_buf_size-fin_len);
          
          //We need a way to inform server that we got updated.
          uri_dict_up2date = true;

          size_t no_of_uris = ((ApachePayloadServer*)payload_server)->uri_dict.size();
          for(uri_byte_cut = 0; (no_of_uris /=256) > 0; uri_byte_cut++);

          log_debug("uri dict updated"); 
          _cur_operation = op_STEG_NO_OP;
          
        }
    }
    return 0;
        
  default:
    log_debug("unrecognizable op_STEG code");

  }

  return 0;
}

size_t
http_apache_steg_config_t::send_dict_to_peer()
{
  //we send the dictionary as a multiline buffer 
  char status_to_send = op_STEG_DICT_UPDATE;
  evbuffer_add(protocol_data_out, &status_to_send, 1);

  stringstream dict_stream;
 
  ((ApachePayloadServer*)payload_server)->export_dict(dict_stream);
  string dict_string = dict_stream.str();
  evbuffer_add(protocol_data_out, dict_string.c_str(), dict_string.size());

  //We mark the end of the data by \r\n
  evbuffer_add(protocol_data_out, &http_apache_steg_config_t::c_end_of_dict, strlen(http_apache_steg_config_t::c_end_of_dict));
  _cur_operation = op_STEG_NO_OP;
  size_t dict_buf_size =  evbuffer_get_length(protocol_data_out);

  log_debug("updating peer's uri dict. need to transmit %lu bytes", dict_buf_size);

  return dict_buf_size;

}

curl_socket_t
http_apache_steg_t::get_conn_socket(void *conn,
                                     curlsocktype purpose,
                                         struct curl_sockaddr *address)
{
  (void)purpose;
  //We just igonre the address because the connection has been established
  //before hand.
  (void)address;
  curl_socket_t conn_sock = (curl_socket_t)((conn_t*)conn)->socket();//In case Zack doesn't like the idea of adding function to conn_t: (curl_socket_t)(bufferevent_getfd(((conn_t*)conn)->buffer));
  log_debug((conn_t*)conn, "socket no: %u", conn_sock);
  return conn_sock;
}

int
http_apache_steg_t::sockopt_callback(void *clientp, curl_socket_t curlfd,
                            curlsocktype purpose)
{
  (void)clientp;
  (void)curlfd;
  (void)purpose;
  /* This return code was added in libcurl 7.21.5 */ 
  return CURL_SOCKOPT_ALREADY_CONNECTED;
}

int
http_apache_steg_t::ignore_close(void *clientp, curl_socket_t curlfd)
{
  (void) clientp;
  (void) curlfd;
  return 0;
}

/* Called by libevent when we get action on a multi socket */ 
void
http_apache_steg_t::curl_socket_event_cb(int fd, short kind, void *userp)
{
  http_apache_steg_t*  steg_mod = (http_apache_steg_t*) userp;
  CURLMcode rc;
  
  log_debug(steg_mod->conn, "socket is ready for %s", kind & EV_READ ? "read" : (kind & EV_WRITE ? "write" : "unknow op"));
  if (kind & EV_READ)
    {
      bufferevent_flush(steg_mod->conn->buffer, EV_READ, BEV_NORMAL);
    }

  int action =
    (kind & EV_READ ? CURL_CSELECT_IN : 0) |
    (kind & EV_WRITE ? CURL_CSELECT_OUT : 0);
 
  if (action == CURL_CSELECT_OUT) {
    rc = curl_multi_socket_action(steg_mod->_apache_config->_curl_multi_handle, fd, action, &steg_mod->_apache_config->_curl_running_handle);

  if (rc == CURLM_OK)
    {
      curl_multi_remove_handle(steg_mod->_apache_config->_curl_multi_handle, steg_mod->_curl_easy_handle);

      bufferevent_enable(steg_mod->conn->buffer, EV_READ|EV_WRITE);

      steg_mod->conn->cease_transmission();
    }
  else
    {
      log_abort(steg_mod->conn, "error in requesting the uri. CURL Error %s", curl_multi_strerror(rc));
    }

  }
  else
    log_abort(steg_mod->conn, "We are not supposed to be here, only write action is acceptable for libcur");

}