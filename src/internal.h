/**/

#ifndef ADNS_INTERNAL_H_INCLUDED
#define ADNS_INTERNAL_H_INCLUDED

#define PRINTFFORMAT(a,b) __attribute__((format(printf,a,b)))
typedef unsigned char byte;

#include <stdarg.h>
#include <assert.h>
#include <unistd.h>

#include <sys/time.h>

#include "adns.h"

/* Configuration and constants */

#define MAXSERVERS 5
#define UDPMAXRETRIES /*15*/5
#define UDPRETRYMS 2000
#define TCPMS 30000
#define LOCALRESOURCEMS 20

#define DNS_PORT 53
#define DNS_MAXUDP 512
#define DNS_MAXDOMAIN 255
#define DNS_HDRSIZE 12
#define DNS_CLASS_IN 1

typedef enum {
  rcode_noerror,
  rcode_formaterror,
  rcode_servfail,
  rcode_nxdomain,
  rcode_notimp,
  rcode_refused
} dns_rcode;

/* Shared data structures */

typedef union {
  adns_status status;
  char *cp;
  adns_rrtype type;
  int i;
  struct in_addr ia;
  unsigned long ul;
} rr_align;

typedef struct {
  int used, avail;
  byte *buf;
} vbuf;

typedef union {
  void *ext;
  int dmaddr_index;
} qcontext;

typedef struct {
  adns_rrtype type;
  int rrsz;

  adns_status (*parse)(adns_state ads, adns_query qu, int serv,
		       const byte *dgram, int dglen, int cbyte, int max,
		       void *store_r);
  /* Parse one RR, in dgram of length dglen, starting at cbyte and
   * extending until at most max.
   *
   * The RR should be stored at *store_r, of length qu->typei->rrsz.
   *
   * If there is an overrun which might indicate truncation, it should set
   * *rdstart to -1; otherwise it may set it to anything else positive.
   */

  void (*makefinal)(adns_state ads, adns_query qu, void *data);
  /* Change memory management of *data.
   * Previously, used alloc_interim, now use alloc_final.
   */
} typeinfo;

typedef struct allocnode {
  struct allocnode *next;
} allocnode;

union maxalign {
  byte d[1];
  struct in_addr ia;
  long l;
  void *p;
  void (*fp)(void);
  union maxalign *up;
} data;

struct adns__query {
  enum { query_udp, query_tcpwait, query_tcpsent, query_child, query_done } state;
  adns_query back, next, parent;
  struct { adns_query head, tail; } children;
  struct { adns_query back, next; } siblings;
  struct allocnode *allocations;
  int interim_alloced, final_used;
  
  const typeinfo *typei;
  char *query_dgram;
  int query_dglen;
  
  vbuf vb;
  /* General-purpose messing-about buffer.
   * Wherever a `big' interface is crossed, this may be corrupted/changed
   * unless otherwise specified.
   */

  adns_answer *answer;
  /* This is allocated when a query is submitted, to avoid being unable
   * to relate errors to queries if we run out of memory.  During
   * query processing status, rrs is 0.  cname is set if
   * we found a cname (this corresponds to cname_dgram in the query
   * structure).  type is set from the word go.  nrrs and rrs
   * are set together, when we find how many rrs there are.
   */
  
  byte *cname_dgram;
  int cname_dglen, cname_begin;
  
  int id, flags, udpretries;
  int udpnextserver;
  unsigned long udpsent, tcpfailed; /* bitmap indexed by server */
  struct timeval timeout;
  qcontext context;

  /* Possible states:
   *
   *  state   Queue   child  id   nextudpserver  sentudp     failedtcp
   *				  
   *  udp     NONE    null   >=0  0              zero        zero
   *  udp     timew   null   >=0  any            nonzero     zero
   *  udp     NONE    null   >=0  any            nonzero     zero
   *				  
   *  tcpwait timew   null   >=0  irrelevant     zero        any
   *  tcpsent timew   null   >=0  irrelevant     zero        any
   *				  
   *  child   childw  set    >=0  irrelevant     irrelevant  irrelevant
   *  done    output  null   -1   irrelevant     irrelevant  irrelevant
   *
   *			      +------------------------+
   *             START -----> |      udp/NONE          |
   *			      +------------------------+
   *                         /                       |\  \
   *        too big for UDP /             UDP timeout  \  \ send via UDP
   *        do this ASAP!  /              more retries  \  \   do this ASAP!
   *                     |_                  desired     \  _|
   *		  +---------------+     	    	+-----------+
   *              | tcpwait/timew | ____                | udp/timew |
   *              +---------------+     \	    	+-----------+
   *                    |  ^             |                 | |
   *     TCP conn'd;    |  | TCP died    |                 | |
   *     send via TCP   |  | more        |     UDP timeout | |
   *     do this ASAP!  |  | servers     |      no more    | |
   *                    v  | to try      |      retries    | |
   *              +---------------+      |      desired    | |
   *              | tcpsent/timew | ____ |                 | |
   *    	  +---------------+     \|                 | |
   *                  \   \ TCP died     | TCP             | |
   *                   \   \ no more     | timeout         / |
   *                    \   \ servers    |                /  |
   *                     \   \ to try    |               /   |
   *                  got \   \          v             |_    / got
   *                 reply \   _| +------------------+      / reply
   *   	       	       	    \  	  | done/output FAIL |     /
   *                         \    +------------------+    /
   *                          \                          /
   *                           _|                      |_
   *                             (..... got reply ....)
   *                              /                   \
   *        need child query/ies /                     \ no child query
   *                            /                       \
   *                          |_                         _|
   *		    +--------------+		       +----------------+
   *                | child/childw | ----------------> | done/output OK |
   *                +--------------+  children done    +----------------+
   */
};

struct adns__state {
  adns_initflags iflags;
  FILE *diagfile;
  struct { adns_query head, tail; } timew, childw, output;
  int nextid, udpsocket, tcpsocket;
  vbuf tcpsend, tcprecv;
  int nservers, tcpserver;
  enum adns__tcpstate { server_disconnected, server_connecting, server_ok } tcpstate;
  struct timeval tcptimeout;
  struct server {
    struct in_addr addr;
  } servers[MAXSERVERS];
};

/* From setup.c: */

void adns__vdiag(adns_state ads, const char *pfx, adns_initflags prevent,
		 int serv, const char *fmt, va_list al);

void adns__debug(adns_state ads, int serv, adns_query qu,
		 const char *fmt, ...) PRINTFFORMAT(3,4);
void adns__warn(adns_state ads, int serv, adns_query qu,
		const char *fmt, ...) PRINTFFORMAT(3,4);
void adns__diag(adns_state ads, int serv, adns_query qu,
		const char *fmt, ...) PRINTFFORMAT(3,4);

int adns__vbuf_ensure(vbuf *vb, int want);
int adns__vbuf_appendstr(vbuf *vb, const char *data);
int adns__vbuf_append(vbuf *vb, const byte *data, int len);
/* 1=>success, 0=>realloc failed */
void adns__vbuf_appendq(vbuf *vb, const byte *data, int len);
void adns__vbuf_init(vbuf *vb);
void adns__vbuf_free(vbuf *vb);

int adns__setnonblock(adns_state ads, int fd); /* => errno value */

/* From submit.c: */

int adns__internal_submit(adns_state ads, adns_query *query_r,
			  adns_rrtype type, char *query_dgram, int query_len,
			  adns_queryflags flags, struct timeval now,
			  adns_status failstat, const qcontext *ctx);
/* Submits a query (for internal use, called during external submits).
 *
 * The new query is returned in *query_r, or we return adns_s_nomemory.
 *
 * The query datagram should already have been assembled; memory for it
 * is taken over by this routine whether it succeeds or fails.
 *
 * If failstat is nonzero then if we are successful in creating the query
 * it is immediately failed with code failstat (but _submit still succeds).
 *
 * ctx is copied byte-for-byte into the query.
 */

void *adns__alloc_interim(adns_query qu, size_t sz);
/* Allocates some memory, and records which query it came from
 * and how much there was.
 *
 * If an error occurs in the query, all its memory is simply freed.
 *
 * If the query succeeds, one large buffer will be made which is
 * big enough for all these allocations, and then adns__alloc_final
 * will get memory from this buffer.
 *
 * _alloc_interim can fail, in which case it will fail the query too,
 * so nothing more need be done with it.
 */

void *adns__alloc_final(adns_query qu, size_t sz);
/* Cannot fail.
 */

/* From query.c: */

void adns__query_udp(adns_state ads, adns_query qu, struct timeval now);
void adns__query_tcp(adns_state ads, adns_query qu, struct timeval now);
adns_status adns__mkquery(adns_state ads, const char *owner, int ol, int id,
			  const typeinfo *typei, adns_queryflags flags);

void adns__query_ok(adns_state ads, adns_query qu);
void adns__query_fail(adns_state ads, adns_query qu, adns_status stat);

/* From reply.c: */

void adns__procdgram(adns_state ads, const byte *dgram, int len,
		     int serv, struct timeval now);

/* From types.c: */

const typeinfo *adns__findtype(adns_rrtype type);

/* From parse.c: */

typedef struct {
  adns_state ads, int serv;
  const byte *dgram;
  int dglen, max, cbyte, namelen;
  int *dmend_rlater, *namelen_rlater;
} findlabel_state;

void adns__findlabel_start(findlabel_state *fls,
			   adns_state ads, int serv,
			   const byte *dgram, int dglen, int max,
			   int dmbegin, int *dmend_rlater);
/* Finds labels in a domain in a datagram.
 *
 * Call this routine first.
 * endpoint_rlater may be null.
 */

adns_status adns__findlabel_next(findlabel_state *fls,
				 int *lablen_r, int *labstart_r);
/* Then, call this one repeatedly.
 *
 * It will return adns_s_ok if all is well, and tell you the length
 * and start of successive labels.  labstart_r may be null, but
 * lablen_r must not be.
 *
 * After the last label, it will return with *lablen_r zero.
 * Do not then call it again; instead, just throw away the findlabel_state.
 *
 * *dmend_rlater will have been set to point to the next part of
 * the datagram after the label (or after the uncompressed part,
 * if compression was used).  *namelen_rlater will have been set
 * to the length of the domain name (total length of labels plus
 * 1 for each intervening dot).
 *
 * If the datagram appears to be truncated, *lablen_r will be -1.
 * *dmend_rlater, *labstart_r and *namelen_r may contain garbage.
 * Do not call _next again.
 *
 * There may also be errors, in which case *dmend_rlater,
 * *namelen_rlater, *lablen_r and *labstart_r may contain garbage.
 * Do not then call findlabel_next again.
 */

adns_status adns__parse_domain(adns_state ads, int serv, vbuf *vb,
			       const byte *dgram, int dglen,
			       int *cbyte_io, int max);
/* vb must already have been initialised; it will be reset if necessary.
 * If there is truncation, vb->used will be set to 0; otherwise
 * (if there is no error) vb will be null-terminated.
 * If there is an error vb and *cbyte_io may be left indeterminate.
 */

adns_status adns__findrr(adns_state ads, int serv,
			 const byte *dgram, int dglen, int *cbyte_io,
			 int *type_r, int *class_r, int *rdlen_r, int *rdstart_r,
			 const byte *eo_dgram, int eo_dglen, int eo_cbyte,
			 int *eo_matched_r);
  /* Finds the extent and some of the contents of an RR in a datagram
   * and does some checks.  The datagram is *dgram, length dglen, and
   * the RR starts at *cbyte_io (which is updated afterwards to point
   * to the end of the RR).
   *
   * The type, class and RRdata length and start are returned iff
   * the corresponding pointer variables are not null.  type_r and
   * class_r may not be null.
   *
   * If the caller thinks they know what the owner of the RR ought to
   * be they can pass in details in eo_*: this is another (or perhaps
   * the same datagram), and a pointer to where the putative owner
   * starts in that datagram.  In this case *eo_matched_r will be set
   * to 1 if the datagram matched or 0 if it did not.  Either
   * both eo_dgram and eo_matched_r must both be non-null, or they
   * must both be null (in which case eo_dglen and eo_cbyte will be ignored).
   * The eo datagram and contained owner domain MUST be valid and
   * untruncated.
   *
   * If there is truncation then *type_r will be set to -1 and
   * *cbyte_io, *class_r, *rdlen_r, *rdstart_r and *eo_matched_r will be
   * undefined.
   *
   * If an error is returned then *type_r will be undefined too.
   */

int vbuf__append_quoted1035(vbuf *vb, const byte *buf, int len);

/* From event.c: */

void adns__tcp_broken(adns_state ads, const char *what, const char *why);
void adns__tcp_tryconnect(adns_state ads, struct timeval now);
void adns__autosys(adns_state ads, struct timeval now);

/* Useful static inline functions: */

static inline void timevaladd(struct timeval *tv_io, long ms) {
  struct timeval tmp;
  assert(ms>=0);
  tmp= *tv_io;
  tmp.tv_usec += (ms%1000)*1000000;
  tmp.tv_sec += ms/1000;
  if (tmp.tv_usec >= 1000000) { tmp.tv_sec++; tmp.tv_usec -= 1000; }
  *tv_io= tmp;
}

static inline int ctype_whitespace(int c) { return c==' ' || c=='\n' || c=='\t'; }
static inline int ctype_digit(int c) { return c>='0' && c<='9'; }
static inline int ctype_alpha(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' || c <= 'Z');
}

/* Useful macros */

#define MEM_ROUND(sz) \
  (( ((sz)+sizeof(union maxalign)-1) / sizeof(union maxalign) ) \
   * sizeof(union maxalign) )

#define LIST_INIT(list) ((list).head= (list).tail= 0)

#define LIST_UNLINK_PART(list,node,part) \
  do { \
    if ((node)->back) (node)->back->part next= (node)->part next; \
      else                        (list).head= (node)->part next; \
    if ((node)->next) (node)->next->part back= (node)->part back; \
      else                        (list).tail= (node)->part back; \
  } while(0)

#define LIST_LINK_TAIL_PART(list,node,part) \
  do { \
    (node)->part back= 0; \
    (node)->part next= (list).tail; \
    if ((list).tail) (list).tail->part back= (node); else (list).part head= (node); \
    (list).tail= (node); \
  } while(0)

#define LIST_UNLINK(list,node) LIST_UNLINK_PART(list,node,)
#define LIST_LINK_TAIL(list,node) LIST_LINK_TAIL_PART(list,node,)

#define GETIL_B(cb) (dgram[(cb)++])
#define GET_B(cb,tv) ((tv)= GETIL_B((cb)))
#define GET_W(cb,tv) ((tv)=0, (tv)|=(GETIL_B((cb))<<8), (tv)|=GETIL_B(cb), (tv))

#endif
