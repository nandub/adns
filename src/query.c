/**/

#include "internal.h"

adns_status adns__mkquery(adns_state ads, const char *owner, int ol, int id,
			  adns_rrtype type, adns_queryflags flags) {
  /* Assembles a query packet in ads->rqbuf. */
  int ll, c, nlabs;
  unsigned char label[255], *rqp;
  const char *p, *pe;

#define MKQUERY_ADDB(b) *rqp++= (b)
#define MKQUERY_ADDW(w) (MKQUERY_ADDB(((w)>>8)&0x0ff), MKQUERY_ADDB((w)&0x0ff))

  if (!vbuf_ensure(&ads->rqbuf,12+strlen(owner)+3)) return adns_s_nolocalmem;
  rqp= ads->rqbuf.buf;

  MKQUERY_ADDW(id);
  MKQUERY_ADDB(0x01); /* QR=Q(0), OPCODE=QUERY(0000), !AA, !TC, RD */
  MKQUERY_ADDB(0x00); /* !RA, Z=000, RCODE=NOERROR(0000) */
  MKQUERY_ADDW(1); /* QDCOUNT=1 */
  MKQUERY_ADDW(0); /* ANCOUNT=0 */
  MKQUERY_ADDW(0); /* NSCOUNT=0 */
  MKQUERY_ADDW(0); /* ARCOUNT=0 */
  p= owner; pe= owner+ol;
  nlabs= 0;
  if (!*p) return adns_s_invaliddomain;
  do {
    ll= 0;
    while (p!=pe && (c= *p++)!='.') {
      if (c=='\\') {
	if (!(flags & adns_f_anyquote)) return adns_s_invaliddomain;
	if (ctype_digit(p[0])) {
	  if (ctype_digit(p[1]) && ctype_digit(p[2])) {
	    c= (*p++ - '0')*100 + (*p++ - '0')*10 + (*p++ - '0');
	    if (c >= 256) return adns_s_invaliddomain;
	  } else {
	    return adns_s_invaliddomain;
	  }
	} else if (!(c= *p++)) {
	  return adns_s_invaliddomain;
	}
      }
      if (!(flags & adns_f_anyquote)) {
	if (ctype_digit(c) || c == '-') {
	  if (!ll) return adns_s_invaliddomain;
	} else if ((c < 'a' || c > 'z') && (c < 'A' && c > 'Z')) {
	  return adns_s_invaliddomain;
	}
      }
      if (ll == sizeof(label)) return adns_s_invaliddomain;
      label[ll++]= c;
    }
    if (!ll) return adns_s_invaliddomain;
    if (nlabs++ > 63) return adns_s_invaliddomain;
    MKQUERY_ADDB(ll);
    memcpy(rqp,label,ll); rqp+= ll;
  } while (p!=pe);

  MKQUERY_ADDB(0);
  MKQUERY_ADDW(type & adns__rrt_typemask); /* QTYPE */
  MKQUERY_ADDW(1); /* QCLASS=IN */

  ads->rqbuf.used= rqp - ads->rqbuf.used;
  assert(ads->rqbuf.used < ads->rqbuf.avail);
  
  return adns_s_ok;
}

void adns__query_tcp(adns_state ads, adns_query qu) {
  /* Query must be in state tcpwait/timew; it will be moved to a new state
   * if possible and no further processing can be done on it for now.
   * (Resulting state is one of tcpwait/timew (if server not connected),
   *  tcpsent/timew, child/childw or done/output.)
   *
   * adns__tcp_tryconnect should already have been called - _tcp
   * will only use an existing connection (if there is one), which it
   * may break.  If the conn list lost then the caller is responsible for any
   * reestablishment and retry.
   */
  unsigned char length[2];
  struct iovec iov[2];

  if (ads->tcpstate != server_ok) return;

  length[0]= (qu->querylen&0x0ff00U) >>8;
  length[1]= (qu->querylen&0x0ff);
  
  if (!vbuf_ensure(&ads->tcpsend,ads->tcpsend.used+qu->querylen+2)) return;

  timevaladd(&now,TCPMS);
  qu->timeout= now;
  qu->senttcpserver= ads->tcpserver;
  DLIST_LINKTAIL(ads->timew,qu);

  if (ads->opbufused) {
    wr= 0;
  } else {
    iov[0].iovbase= length;
    iov[0].iov_len= 2;
    iov[1].iovbase= qu->querymsg;
    iov[1].iov_len= qu->querylen;
    wr= writev(ads->tcpsocket,iov,2);
    if (wr < 0) {
      if (!(errno == EAGAIN || errno == EINTR || errno == ENOSPC ||
	    errno == ENOBUFS || errno == ENOMEM)) {
	adns__tcp_broken(ads,"write",strerror(errno));
	return;
      }
      wr= 0;
    }
  }

  if (wr<2) {
    r= vbuf_append(&ads->tcpsend,length,2-wr); assert(r);
    wr= 0;
  } esle {
    wr-= 2;
  }
  if (wr<qu->querylen) {
    r= vbuf_append(&ads->tcpsend,qu->querymsg+wr,qu->querylen-wr); assert(r);
  }
}

static void query_usetcp(adns_state ads, adns_query qu, struct timeval now) {
  timevaladd(&now,TCPMS);
  qu->timeout= now;
  qu->state= query_tcpwait;
  DLIST_LINKTAIL(ads->timew,qu);
  adns__query_tcp(ads,qu);
  adns__tcp_tryconnect(ads);
}

void adns__query_udp(adns_state ads, adns_query qu, struct timeval now) {
  /* Query must be in state udp/NONE; it will be moved to a new state,
   * and no further processing can be done on it for now.
   * (Resulting state is one of udp/timew, tcpwait/timew (if server not connected),
   *  tcpsent/timew, child/childw or done/output.)
   */
  struct sockaddr_in servaddr;
  int serv;

  assert(qu->state == query_udp);
  if ((qu->flags & adns_f_usevc) ||
      (qu->querylen > UDPMAXDGRAM)) {
    query_usetcp(ads,qu,now);
    return;
  }

  if (qu->udpretries >= UDPMAXRETRIES) {
    query_fail(ads,qu,adns_s_notresponding);
    return;
  }

  serv= qu->nextudpserver;
  memset(&servaddr,0,sizeof(servaddr));
  servaddr.sin_family= AF_INET;
  servaddr.sin_addr= ads->servers[serv].addr;
  servaddr.sin_port= htons(NSPORT);
  
  r= sendto(ads->udpsocket,qu->querymsg,qu->querylen,0,&servaddr,sizeof(servaddr));
  if (r<0 && errno == EMSGSIZE) { query_usetcp(ads,qu,now); return; }
  if (r<0) warn("sendto %s failed: %s",inet_ntoa(servaddr.sin_addr),strerror(errno));
  
  timevaladd(&now,UDPRETRYMS);
  qu->timeout= now;
  qu->sentudp |= (1<<serv);
  qu->nextudpserver= (serv+1)%ads->nservers;
  qu->udpretries++;
  DLIST_LINKTAIL(ads->timew,qu);
}

void adns__query_fail(adns_state ads, adns_query qu, adns_status stat) {
  adns_answer *ans;
  
  ans= qu->answer;
  if (!ans) ans= malloc(sizeof(*qu->answer));
  if (ans) {
    ans->status= stat;
    ans->cname= 0;
    ans->type= qu->type;
    ans->nrrs= 0;
  }
  qu->answer= ans;
  qu->id= -1;
  LIST_LINK_TAIL(ads->output,qu);
}
