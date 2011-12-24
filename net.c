/* net.c -- CoAP network interface
 *
 * Copyright (C) 2010,2011 Olaf Bergmann <bergmann@tzi.org>
 *
 * This file is part of the CoAP library libcoap. Please see
 * README for terms of use. 
 */

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "debug.h"
#include "mem.h"
#include "str.h"
#include "async.h"
#include "resource.h"
#include "option.h"
#include "encode.h"
#include "net.h"

#ifndef WITH_CONTIKI

time_t clock_offset;
#else /* WITH_CONTIKI */
# ifndef DEBUG
#  define DEBUG DEBUG_PRINT
# endif /* DEBUG */

#include "memb.h"
#include "net/uip-debug.h"

clock_time_t clock_offset;

#define UIP_IP_BUF   ((struct uip_ip_hdr *)&uip_buf[UIP_LLH_LEN])
#define UIP_UDP_BUF  ((struct uip_udp_hdr *)&uip_buf[UIP_LLIPH_LEN])

void coap_resources_init();

unsigned char initialized = 0;
coap_context_t the_coap_context;

MEMB(node_storage, coap_queue_t, COAP_PDU_MAXCNT);
#endif /* WITH_CONTIKI */

int print_wellknown(coap_context_t *, unsigned char *, size_t *);

int
coap_insert_node(coap_queue_t **queue, coap_queue_t *node,
		 int (*order)(coap_queue_t *, coap_queue_t *node) ) {
  coap_queue_t *p, *q;
  if ( !queue || !node )
    return 0;

  /* set queue head if empty */
  if ( !*queue ) {
    *queue = node;
    return 1;
  }

  /* replace queue head if PDU's time is less than head's time */
  q = *queue;
  if ( order( node, q ) < 0) {
    node->next = q;
    *queue = node;
    return 1;
  }

  /* search for right place to insert */
  do {
    p = q;
    q = q->next;
  } while ( q && order( node, q ) >= 0 );

  /* insert new item */
  node->next = q;
  p->next = node;
  return 1;
}

int
coap_delete_node(coap_queue_t *node) {
  if ( !node )
    return 0;

  coap_free( node->pdu );
#ifndef WITH_CONTIKI
  coap_free( node );
#else /* WITH_CONTIKI */
  memb_free(&node_storage, node);
#endif /* WITH_CONTIKI */

  return 1;
}

void
coap_delete_all(coap_queue_t *queue) {
  if ( !queue )
    return;

  coap_delete_all( queue->next );
  coap_delete_node( queue );
}


coap_queue_t *
coap_new_node() {
  coap_queue_t *node;
#ifndef WITH_CONTIKI
  node = (coap_queue_t *)coap_malloc(sizeof(coap_queue_t));
#else /* WITH_CONTIKI */
  node = (coap_queue_t *)memb_alloc(&node_storage);
#endif /* WITH_CONTIKI */

  if ( ! node ) {
#ifndef NDEBUG
    coap_log(LOG_WARN, "coap_new_node: malloc");
#endif
    return NULL;
  }

  memset(node, 0, sizeof *node );
  return node;
}

coap_queue_t *
coap_peek_next( coap_context_t *context ) {
  if ( !context || !context->sendqueue )
    return NULL;

  return context->sendqueue;
}

coap_queue_t *
coap_pop_next( coap_context_t *context ) {
  coap_queue_t *next;

  if ( !context || !context->sendqueue )
    return NULL;

  next = context->sendqueue;
  context->sendqueue = context->sendqueue->next;
  next->next = NULL;
  return next;
}

#ifdef COAP_DEFAULT_WKC_HASHKEY
/** Checks if @p Key is equal to the pre-defined hash key for.well-known/core. */
#define is_wkc(Key)							\
  (memcmp((Key), COAP_DEFAULT_WKC_HASHKEY, sizeof(coap_key_t)) == 0)
#else
/* Implements a singleton to store a hash key for the .wellknown/core
 * resources. */
int
is_wkc(coap_key_t k) {
  static coap_key_t wkc;
  static unsigned char initialized = 0;
  if (!initialized) {
    initialized = coap_hash_path((unsigned char *)COAP_DEFAULT_URI_WELLKNOWN, 
				 sizeof(COAP_DEFAULT_URI_WELLKNOWN) - 1, wkc);
  }
  return memcmp(k, wkc, sizeof(coap_key_t)) == 0;
}
#endif

coap_context_t *
coap_new_context(const coap_address_t *listen_addr) {
#ifndef WITH_CONTIKI
  coap_context_t *c = coap_malloc( sizeof( coap_context_t ) );
  int reuse = 1;
#else /* WITH_CONTIKI */
  coap_context_t *c;

  if (initialized)
    return NULL;
#endif /* WITH_CONTIKI */

  if (!listen_addr) {
    coap_log(LOG_EMERG, "no listen address specified\n");
    return NULL;
  }

  coap_clock_init();
  prng_init((unsigned long)listen_addr ^ clock_offset);

#ifndef WITH_CONTIKI
  if ( !c ) {
#ifndef NDEBUG
    coap_log(LOG_EMERG, "coap_init: malloc:");
#endif
    return NULL;
  }
#else /* WITH_CONTIKI */
  coap_resources_init();

  c = &the_coap_context;
  initialized = 1;
#endif /* WITH_CONTIKI */

  memset(c, 0, sizeof( coap_context_t ) );

  /* register the critical options that we know */
  coap_register_option(c, COAP_OPTION_CONTENT_TYPE);
  coap_register_option(c, COAP_OPTION_PROXY_URI);
  coap_register_option(c, COAP_OPTION_URI_HOST);
  coap_register_option(c, COAP_OPTION_URI_PORT);
  coap_register_option(c, COAP_OPTION_URI_PATH);
  coap_register_option(c, COAP_OPTION_TOKEN);
  coap_register_option(c, COAP_OPTION_URI_QUERY);

#ifndef WITH_CONTIKI
  c->sockfd = socket(listen_addr->sa_family, SOCK_DGRAM, 0);
  if ( c->sockfd < 0 ) {
#ifndef NDEBUG
    coap_log(LOG_EMERG, "coap_new_context: socket");
#endif
    goto onerror;
  }

  if ( setsockopt( c->sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse) ) < 0 ) {
#ifndef NDEBUG
    coap_log(LOG_WARN, "setsockopt SO_REUSEADDR");
#endif
  }

  if ( bind (c->sockfd, listen_addr, addr_size) < 0 ) {
#ifndef NDEBUG
    coap_log(LOG_EMERG, "coap_new_context: bind");
#endif
    goto onerror;
  }

  return c;

 onerror:
  if ( c->sockfd >= 0 )
    close ( c->sockfd );
  coap_free( c );
  return NULL;

#else /* WITH_CONTIKI */
  c->conn = udp_new(NULL, 0, NULL);
  udp_bind(c->conn, listen_addr->port);

  return c;
#endif /* WITH_CONTIKI */
}

void
coap_free_context( coap_context_t *context ) {
#ifndef WITH_CONTIKI
  coap_resource_t *res, *rtmp;
#endif /* WITH_CONTIKI */
  if ( !context )
    return;

  coap_delete_all(context->recvqueue);
  coap_delete_all(context->sendqueue);

#ifndef WITH_CONTIKI
  HASH_ITER(hh, context->resources, res, rtmp) {
    free(res);
  }

  /* coap_delete_list(context->subscriptions); */
  close( context->sockfd );
  coap_free( context );
#else /* WITH_CONTIKI */
  memset(&the_coap_context, 0, sizeof(coap_context_t));
  initialized = 0;
#endif /* WITH_CONTIKI */
}

int
coap_option_check_critical(coap_context_t *ctx, 
			   coap_pdu_t *pdu,
			   coap_opt_filter_t unknown) {

  coap_opt_iterator_t opt_iter;
  int ok = 1;
  
  coap_option_iterator_init(pdu, &opt_iter, COAP_OPT_ALL);

  while (coap_option_next(&opt_iter)) {

    /* The following condition makes use of the fact that
     * coap_option_getb() returns -1 if type exceeds the bit-vector
     * filter. As the vector is supposed to be large enough to hold
     * the largest known option, we know that everything beyond is
     * bad.
     */
    if (opt_iter.type & 0x01 && 
	coap_option_getb(ctx->known_options, opt_iter.type) < 1) {
      debug("unknown critical option %d\n", opt_iter.type);
      
      ok = 0;

      /* When opt_iter.type is beyond our known option range,
       * coap_option_setb() will return -1 and we are safe to leave
       * this loop. */
      if (coap_option_setb(unknown, opt_iter.type) == -1)
	break;
    }
  }

  return ok;
}

void
coap_transaction_id(const coap_address_t *peer, const coap_pdu_t *pdu, 
		    coap_tid_t *id) {
  coap_key_t h;
  coap_opt_iterator_t opt_iter;

  memset(h, 0, sizeof(coap_key_t));

  /* Compare the complete address structure in case of IPv4. For IPv6,
   * we need to look at the transport address only. */

#ifndef WITH_CONTIKI
  switch (peer->addr.sa.sa_family) {
  case AF_INET:
    coap_hash((const unsigned char *)&peer->addr.sa, peer->size, h);
    break;
  case AF_INET6:
    coap_hash((const unsigned char *)&peer->addr.sin6.sin6_port,
	      sizeof(peer->addr.sin6.sin6_port), h);
    coap_hash((const unsigned char *)&peer->addr.sin6.sin6_addr,
	      sizeof(peer->addr.sin6.sin6_addr), h);
    break;
  default:
    return;
  }
#else /* WITH_CONTIKI */
    coap_hash((const unsigned char *)&peer->port, sizeof(peer->port), h);
    coap_hash((const unsigned char *)&peer->addr, sizeof(peer->addr), h);  
#endif /* WITH_CONTIKI */

  if (coap_check_option((coap_pdu_t *)pdu, COAP_OPTION_TOKEN, &opt_iter))
    coap_hash(COAP_OPT_VALUE(opt_iter.option), 
	      COAP_OPT_LENGTH(opt_iter.option), 
	      h);

  *id = ((h[0] << 8) | h[1]) ^ ((h[2] << 8) | h[3]);
}

#ifndef WITH_CONTIKI
/* releases space allocated by PDU if free_pdu is set */
coap_tid_t
coap_send_impl(coap_context_t *context, 
	       const coap_address_t *dst,
	       coap_pdu_t *pdu, int free_pdu) {
  ssize_t bytes_written;
  coap_tid_t id = COAP_INVALID_TID;

  if ( !context || !dst || !pdu )
    return id;

  bytes_written = sendto( context->sockfd, pdu->hdr, pdu->length, 0,
			  &dst->addr.sa, dst->size);

  if (bytes_written >= 0) {
    coap_transaction_id(dst, pdu, &id);
  } else {
    coap_log(LOG_CRIT, "coap_send: sendto");
  }

  if ( free_pdu )
    coap_delete_pdu( pdu );

  return id;
}
#else  /* WITH_CONTIKI */
/* releases space allocated by PDU if free_pdu is set */
coap_tid_t
coap_send_impl(coap_context_t *context, 
	       const coap_address_t *dst,
	       coap_pdu_t *pdu, int free_pdu) {
  coap_tid_t id = COAP_INVALID_TID;

  if ( !context || !dst || !pdu )
    return id;

  /* FIXME: is there a way to check if send was successful? */
  uip_udp_packet_sendto(context->conn, pdu->hdr, pdu->length,
			&dst->addr, dst->port);

  coap_transaction_id(dst, pdu, &id);

  if (free_pdu)
    coap_delete_pdu(pdu);

  return id;
}
#endif /* WITH_CONTIKI */

coap_tid_t 
coap_send(coap_context_t *context, 
	  const coap_address_t *dst, 
	  coap_pdu_t *pdu) {
  return coap_send_impl(context, dst, pdu, 1);
}

coap_tid_t
coap_send_error(coap_context_t *context, 
		coap_pdu_t *request,
		const coap_address_t *dst,
		unsigned char code,
		coap_opt_filter_t opts) {
  coap_pdu_t *response;
  coap_tid_t result = COAP_INVALID_TID;

  assert(request);
  assert(dst);

  response = coap_new_error_response(request, code, opts);
  if (response) {
    result = coap_send(context, dst, response);
    if (result == COAP_INVALID_TID) 
      coap_delete_pdu(response);
  }
  
  return result;
}

int
_order_timestamp( coap_queue_t *lhs, coap_queue_t *rhs ) {
  return lhs && rhs && ( lhs->t < rhs->t ) ? -1 : 1;
}

coap_tid_t
coap_send_confirmed(coap_context_t *context, 
		    const coap_address_t *dst,
		    coap_pdu_t *pdu) {
  coap_queue_t *node;
  int r;

  node = coap_new_node();
  if (!node) {
    debug("coap_send_confirmed: insufficient memory\n");
    return COAP_INVALID_TID;
  }
  
  prng((unsigned char *)&r,sizeof(r));
  coap_ticks(&node->t);

  /* add randomized RESPONSE_TIMEOUT to determine retransmission timeout */
  node->timeout = COAP_DEFAULT_RESPONSE_TIMEOUT * COAP_TICKS_PER_SECOND +
    (COAP_DEFAULT_RESPONSE_TIMEOUT >> 1) *
    ((COAP_TICKS_PER_SECOND * (r & 0xFF)) >> 8);
  node->t += node->timeout;

  memcpy(&node->remote, dst, sizeof(coap_address_t));
  node->pdu = pdu;

  assert(&context->sendqueue);
  coap_insert_node(&context->sendqueue, node, _order_timestamp);

  node->id = coap_send_impl(context, dst, pdu, 0);
  return node->id;
}

coap_tid_t
coap_retransmit( coap_context_t *context, coap_queue_t *node ) {
  if ( !context || !node )
    return COAP_INVALID_TID;

  /* re-initialize timeout when maximum number of retransmissions are not reached yet */
  if ( node->retransmit_cnt < COAP_DEFAULT_MAX_RETRANSMIT ) {
    node->retransmit_cnt++;
    node->t += ( node->timeout << node->retransmit_cnt );
    coap_insert_node( &context->sendqueue, node, _order_timestamp );

#ifndef WITH_CONTIKI
    debug("** retransmission #%d of transaction %d\n",
	  node->retransmit_cnt, ntohs(node->pdu->hdr->id));
#else /* WITH_CONTIKI */
    debug("** retransmission #%d of transaction %d\n",
	  node->retransmit_cnt, uip_ntohs(node->pdu->hdr->id));
#endif /* WITH_CONTIKI */
    
    node->id = coap_send_impl(context, &node->remote, node->pdu, 0);
    return node->id;
  }

  /* no more retransmissions, remove node from system */

  debug("** removed transaction %d\n", node->id);

  coap_delete_node( node );
  return COAP_INVALID_TID;
}

int
_order_transaction_id( coap_queue_t *lhs, coap_queue_t *rhs ) {
  return ( lhs && rhs && lhs->pdu && rhs->pdu &&
	   ( lhs->id < lhs->id ) )
    ? -1
    : 1;
}

int
coap_read( coap_context_t *ctx ) {
#ifndef WITH_CONTIKI
  static char buf[COAP_MAX_PDU_SIZE];
  coap_hdr_t *pdu = (coap_hdr_t *)buf;
#else /* WITH_CONTIKI */
  char *buf;
  coap_hdr_t *pdu;
#endif /* WITH_CONTIKI */
  ssize_t bytes_read;
  coap_address_t src;
  coap_queue_t *node;

#ifdef WITH_CONTIKI
  buf = uip_appdata;
  pdu = (coap_hdr_t *)buf;
#endif /* WITH_CONTIKI */

  coap_address_init(&src);

#ifndef WITH_CONTIKI
  bytes_read = recvfrom(ctx->sockfd, buf, sizeof(buf), 0,
			&src.addr.sa, &src.size);
#else /* WITH_CONTIKI */
  if(uip_newdata()) {
    uip_ipaddr_copy(&src.addr, &UIP_IP_BUF->srcipaddr);
    src.port = UIP_UDP_BUF->srcport;

    bytes_read = uip_datalen();
    ((char *)uip_appdata)[uip_datalen()] = 0;
    PRINTF("Server received message from ");
    PRINT6ADDR(&src.addr);
    PRINTF(":%d\n", uip_ntohs(src.port));
  }
#endif /* WITH_CONTIKI */

  if ( bytes_read < 0 ) {
    warn("coap_read: recvfrom");
    return -1;
  }

  if ( (size_t)bytes_read < sizeof(coap_hdr_t) ) {
    debug("coap_read: discarded invalid frame\n" );
    return -1;
  }

  if ( pdu->version != COAP_DEFAULT_VERSION ) {
    debug("coap_read: unknown protocol version\n" );
    return -1;
  }

  node = coap_new_node();
  if ( !node )
    return -1;

  node->pdu = coap_pdu_init(0, 0, 0, bytes_read);
  if ( !node->pdu ) {
    coap_delete_node( node );
    return -1;
  }

  coap_ticks( &node->t );
  memcpy(&node->remote, &src, sizeof(coap_address_t));

  /* "parse" received PDU by filling pdu structure */
  memcpy( node->pdu->hdr, buf, bytes_read );
  node->pdu->length = bytes_read;

  /* finally calculate beginning of data block */
  {
    coap_opt_t *opt = options_start(node->pdu);
    unsigned char cnt;

    /* Note that we cannot use the official options iterator here as
     * it eats up the fence posts. */
    for (cnt = node->pdu->hdr->optcnt; cnt; --cnt)
      opt = options_next(opt);

    node->pdu->data = (unsigned char *)opt;
  }

  /* and add new node to receive queue */
  coap_transaction_id(&node->remote, node->pdu, &node->id);
  coap_insert_node(&ctx->recvqueue, node, _order_timestamp);

#ifndef NDEBUG
  if (LOG_DEBUG <= coap_get_log_level()) {
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 40
#endif
    unsigned char addr[INET6_ADDRSTRLEN+8];

    if (coap_print_addr(&src, addr, sizeof(addr)))
      debug("** received %d bytes from %s:\n", bytes_read, addr);

    coap_show_pdu( node->pdu );
  }
#endif

  return 0;
}

int
coap_remove_from_queue(coap_queue_t **queue, coap_tid_t id, coap_queue_t **node) {
  coap_queue_t *p, *q;

  if ( !queue || !*queue)
    return 0;

  /* replace queue head if PDU's time is less than head's time */

  if ( id == (*queue)->id ) { /* found transaction */
    *node = *queue;
    *queue = (*queue)->next;
    (*node)->next = NULL;
    /* coap_delete_node( q ); */
    debug("*** removed transaction %u\n", id);
    return 1;
  }

  /* search transaction to remove (only first occurence will be removed) */
  q = *queue;
  do {
    p = q;
    q = q->next;
  } while ( q && id != q->id );

  if ( q ) {			/* found transaction */
    p->next = q->next;
    q->next = NULL;
    *node = q;
    /* coap_delete_node( q ); */
    debug("*** removed transaction %u\n", id);
    return 1;
  }

  return 0;

}

coap_queue_t *
coap_find_transaction(coap_queue_t *queue, coap_tid_t id) {
  while (queue && queue->id != id)
    queue = queue->next;

  return queue;
}

coap_pdu_t *
coap_new_error_response(coap_pdu_t *request, unsigned char code, 
			coap_opt_filter_t opts) {
  coap_opt_iterator_t opt_iter;
  coap_pdu_t *response;
  size_t size = sizeof(coap_hdr_t) + 4; /* some bytes for fence-post options */
  unsigned char buf[2];
  int type; 

#if COAP_ERROR_PHRASE_LENGTH > 0
  char *phrase = coap_response_phrase(code);

  /* Need some more space for the error phrase and the Content-Type option */
  if (phrase)
    size += strlen(phrase) + 2;
#endif

  assert(request);

  /* cannot send ACK if original request was not confirmable */
  type = request->hdr->type == COAP_MESSAGE_CON 
    ? COAP_MESSAGE_ACK
    : COAP_MESSAGE_NON;

  /* Estimate how much space we need for options to copy from
   * request. We always need the Token, for 4.02 the unknown critical
   * options must be included as well. */
  coap_option_clrb(opts, COAP_OPTION_CONTENT_TYPE); /* we do not want this */
  coap_option_setb(opts, COAP_OPTION_TOKEN);

  coap_option_iterator_init(request, &opt_iter, opts);

  while(coap_option_next(&opt_iter))
    size += COAP_OPT_SIZE(opt_iter.option);

  /* Now create the response and fill with options and payload data. */
  response = coap_pdu_init(type, code, request->hdr->id, size);
  if (response) {
#if COAP_ERROR_PHRASE_LENGTH > 0
    if (phrase)
      coap_add_option(response, COAP_OPTION_CONTENT_TYPE, 
		      coap_encode_var_bytes(buf, COAP_MEDIATYPE_TEXT_PLAIN), buf);
#endif

    /* copy all options */
    coap_option_iterator_init(request, &opt_iter, opts);
    while(coap_option_next(&opt_iter))
      coap_add_option(response, opt_iter.type, 
		      COAP_OPT_LENGTH(opt_iter.option),
		      COAP_OPT_VALUE(opt_iter.option));
    
#if COAP_ERROR_PHRASE_LENGTH > 0
    if (phrase)
      coap_add_data(response, strlen(phrase), (unsigned char *)phrase);
#endif
  }

  return response;
}

coap_pdu_t *
wellknown_response(coap_context_t *context, coap_pdu_t *request) {
  coap_pdu_t *resp;
  coap_opt_iterator_t opt_iter;
  coap_opt_t *token;
  size_t len;
  unsigned char buf[2];

  resp = coap_pdu_init(COAP_MESSAGE_ACK, COAP_RESPONSE_CODE(205),
		       request->hdr->id, COAP_MAX_PDU_SIZE);
  if (!resp)
    return NULL;

  /* add Content-Type */
  coap_add_option(resp, COAP_OPTION_CONTENT_TYPE,
     coap_encode_var_bytes(buf, COAP_MEDIATYPE_APPLICATION_LINK_FORMAT), buf);
  
  token = coap_check_option(request, COAP_OPTION_TOKEN, &opt_iter);
  if (token)
    coap_add_option(resp, COAP_OPTION_TOKEN, 
		    COAP_OPT_LENGTH(token), COAP_OPT_VALUE(token));
  
  /* set payload of response */
  len = resp->max_size - resp->length;
  
  if (!print_wellknown(context, resp->data, &len)) {
    debug("print_wellknown failed\n");
    coap_delete_pdu(resp);
    return NULL;
  } 
  
  resp->length += len;
  return resp;
}

#define WANT_WKC(Pdu,Key)					\
  (((Pdu)->hdr->code == COAP_REQUEST_GET) && is_wkc(Key))

void
handle_request(coap_context_t *context, coap_queue_t *node) {      
  coap_method_handler_t h = NULL;
  coap_pdu_t *response = NULL;
  coap_opt_filter_t opt_filter;
  coap_resource_t *resource;
  coap_key_t key;

  coap_option_filter_clear(opt_filter);
  coap_option_setb(opt_filter, COAP_OPTION_TOKEN); /* we always need the token */
  
  /* try to find the resource from the request URI */
  coap_hash_request_uri(node->pdu, key);
  resource = coap_get_resource_from_key(context, key);
  
  if (!resource) {
    /* The resource was not found. Check if the request URI happens to
     * be the well-known URI. In that case, we generate a default
     * response, otherwise, we return 4.04 */

    switch(node->pdu->hdr->code) {

    case COAP_REQUEST_GET: 
      if (is_wkc(key)) {	/* GET request for .well-known/core */
	info("create default response for %s\n", COAP_DEFAULT_URI_WELLKNOWN);
	response = wellknown_response(context, node->pdu);

      } else { /* GET request for any another resource, return 4.04 */

	debug("GET for unknown resource 0x%02x%02x%02x%02x, return 4.04\n", 
	      key[0], key[1], key[2], key[3]);
	response = 
	  coap_new_error_response(node->pdu, COAP_RESPONSE_CODE(404), 
				  opt_filter);
      }
      break;

    default: 			/* any other request type */

      debug("unhandled request for unknown resource 0x%02x%02x%02x%02x,"
	    "return 4.05\n", key[0], key[1], key[2], key[3]);
	response = coap_new_error_response(node->pdu, COAP_RESPONSE_CODE(405), 
					   opt_filter);
    }
      
    if (!response || (coap_send(context, &node->remote, response)
		      == COAP_INVALID_TID)) {
      warn("cannot send response for transaction %u\n", node->id);
      coap_delete_pdu(response);
    }

    return;
  }
  
  
  /* the resource was found, check if there is a registered handler */
  if (node->pdu->hdr->code < sizeof(resource->handler))
    h = resource->handler[node->pdu->hdr->code - 1];
  
  if (h) {
    debug("call custom handler for resource 0x%02x%02x%02x%02x\n", 
	  key[0], key[1], key[2], key[3]);
    h(context, resource, &node->remote, node->pdu, node->id);
  } else {
    if (WANT_WKC(node->pdu, key)) {
      debug("create default response for %s\n", COAP_DEFAULT_URI_WELLKNOWN);
      response = wellknown_response(context, node->pdu);
    } else
      response = coap_new_error_response(node->pdu, COAP_RESPONSE_CODE(405), 
					 opt_filter);
    
    if (!response || (coap_send(context, &node->remote, response)
		      == COAP_INVALID_TID)) {
      debug("cannot send response for transaction %u\n", node->id);
      coap_delete_pdu(response);
    }
  }  
}

static inline void
handle_response(coap_context_t *context, 
		coap_queue_t *sent, coap_queue_t *rcvd) {
  
  /* send ACK if rcvd is confirmable (i.e. a separate response) */
  if (rcvd->pdu->hdr->type == COAP_MESSAGE_CON)
    coap_send_ack(context, &rcvd->remote, rcvd->pdu);

  /* only call response handler when id is valid */
  if (context->response_handler) {
    context->response_handler(context, 
			      &rcvd->remote, sent ? sent->pdu : NULL, 
			      rcvd->pdu, rcvd->id);
  }
}

static inline int
#ifdef __GNUC__
handle_locally(coap_context_t *context __attribute__ ((unused)), 
	       coap_queue_t *node __attribute__ ((unused))) {
#else /* not a GCC */
handle_locally(coap_context_t *context, coap_queue_t *node) {
#endif /* GCC */
  /* this function can be used to check if node->pdu is really for us */
  return 1;
}

void
coap_dispatch( coap_context_t *context ) {
  coap_queue_t *rcvd = NULL, *sent = NULL;
  coap_pdu_t *response;
  coap_opt_filter_t opt_filter;

  if (!context)
    return;

  memset(opt_filter, 0, sizeof(coap_opt_filter_t));

  while ( context->recvqueue ) {
    rcvd = context->recvqueue;

    /* remove node from recvqueue */
    context->recvqueue = context->recvqueue->next;
    rcvd->next = NULL;

    if ( rcvd->pdu->hdr->version != COAP_DEFAULT_VERSION ) {
      debug("dropped packet with unknown version %u\n", rcvd->pdu->hdr->version);
      goto cleanup;
    }
    
    switch ( rcvd->pdu->hdr->type ) {
    case COAP_MESSAGE_ACK:
      /* find transaction in sendqueue to stop retransmission */
      coap_remove_from_queue(&context->sendqueue, rcvd->id, &sent);
      if (rcvd->pdu->hdr->code == 0)
	goto cleanup;
      break;

    case COAP_MESSAGE_RST :
      /* We have sent something the receiver disliked, so we remove
       * not only the transaction but also the subscriptions we might
       * have. */

#ifndef WITH_CONTIKI
      coap_log(LOG_ALERT, "got RST for message %u\n", ntohs(rcvd->pdu->hdr->id));
#else /* WITH_CONTIKI */
      coap_log(LOG_ALERT, "got RST for message %u\n", uip_ntohs(rcvd->pdu->hdr->id));
#endif /* WITH_CONTIKI */

      /* find transaction in sendqueue to stop retransmission */
      coap_remove_from_queue(&context->sendqueue, rcvd->id, &sent);
      break;

    case COAP_MESSAGE_NON :	/* check for unknown critical options */
      if (coap_option_check_critical(context, rcvd->pdu, opt_filter) == 0)
	goto cleanup;
      break;

    case COAP_MESSAGE_CON :	/* check for unknown critical options */
      if (coap_option_check_critical(context, rcvd->pdu, opt_filter) == 0) {

	response = 
	  coap_new_error_response(rcvd->pdu, COAP_RESPONSE_CODE(402), opt_filter);

	if (!response)
	  warn("coap_dispatch: cannot create error reponse\n");
	else {
	  if (coap_send(context, &rcvd->remote, response) 
	      == COAP_INVALID_TID) {
	    warn("coap_dispatch: error sending reponse\n");
	    coap_delete_pdu(response);
	  }
	}	 
	
	goto cleanup;
      }
      break;
    }
   
    /* Pass message to upper layer if a specific handler was
     * registered for a request that should be handled locally. */
    if (handle_locally(context, rcvd)) {
      if (COAP_MESSAGE_IS_REQUEST(rcvd->pdu->hdr))
	handle_request(context, rcvd);
      else if (COAP_MESSAGE_IS_RESPONSE(rcvd->pdu->hdr))
	handle_response(context, sent, rcvd);
      else
	debug("dropped message with invalid code\n");
    }
    
  cleanup:
    coap_delete_node(sent);
    coap_delete_node(rcvd);
  }
}

int
coap_can_exit( coap_context_t *context ) {
  return !context || (context->recvqueue == NULL && context->sendqueue == NULL);
}

