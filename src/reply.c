/*
 * reply.c
 * - main handling and parsing routine for received datagrams
 */
/*
 *  This file is part of adns, which is Copyright (C) 1997, 1998 Ian Jackson
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA. 
 */

#include <stdlib.h>
#include <string.h>

#include "internal.h"
    
void adns__procdgram(adns_state ads, const byte *dgram, int dglen,
		     int serv, struct timeval now) {
  int cbyte, rrstart, wantedrrs, rri, foundsoa, foundns, cname_here;
  int id, f1, f2, qdcount, ancount, nscount, arcount;
  int flg_ra, flg_rd, flg_tc, flg_qr, opcode;
  int rrtype, rrclass, rdlength, rdstart;
  int anstart, nsstart, arstart;
  int ownermatched, l, nrrs;
  const typeinfo *typei;
  adns_query qu, nqu;
  dns_rcode rcode;
  adns_status st;
  vbuf tempvb;
  byte *newquery, *rrsdata;
  
  if (dglen<DNS_HDRSIZE) {
    adns__diag(ads,serv,0,"received datagram too short for message header (%d)",dglen);
    return;
  }
  cbyte= 0;
  GET_W(cbyte,id);
  GET_B(cbyte,f1);
  GET_B(cbyte,f2);
  GET_W(cbyte,qdcount);
  GET_W(cbyte,ancount);
  GET_W(cbyte,nscount);
  GET_W(cbyte,arcount);
  assert(cbyte == DNS_HDRSIZE);

  flg_qr= f1&0x80;
  opcode= (f1&0x78)>>3;
  flg_tc= f1&0x02;
  flg_rd= f1&0x01;
  flg_ra= f2&0x80;
  rcode= (f2&0x0f);

  cname_here= 0;
  
  if (!flg_qr) {
    adns__diag(ads,serv,0,"server sent us a query, not a response");
    return;
  }
  if (opcode) {
    adns__diag(ads,serv,0,"server sent us unknown opcode %d (wanted 0=QUERY)",opcode);
    return;
  }
  if (!qdcount) {
    adns__diag(ads,serv,0,"server sent reply without quoting our question");
    return;
  } else if (qdcount>1) {
    adns__diag(ads,serv,0,"server claimed to answer %d questions with one message",
	       qdcount);
    return;
  }
  for (qu= ads->timew.head; qu; qu= nqu) {
    nqu= qu->next;
    if (qu->id != id) continue;
    if (dglen < qu->query_dglen) continue;
    if (memcmp(qu->query_dgram+DNS_HDRSIZE,
	       dgram+DNS_HDRSIZE,
	       qu->query_dglen-DNS_HDRSIZE))
      continue;
    break;
  }
  if (!qu) {
    if (ads->iflags & adns_if_debug) {
      adns__vbuf_init(&tempvb);
      adns__debug(ads,serv,0,"reply not found, id %02x, query owner %s",
		  id, adns__diag_domain(ads,serv,0,&tempvb,dgram,dglen,DNS_HDRSIZE));
      adns__vbuf_free(&tempvb);
    }
    return;
  }
  anstart= qu->query_dglen;
  arstart= -1;

  LIST_UNLINK(ads->timew,qu);
  /* We're definitely going to do something with this query now */
  
  switch (rcode) {
  case rcode_noerror:
  case rcode_nxdomain:
    break;
  case rcode_formaterror:
    adns__warn(ads,serv,qu,"server cannot understand our query (Format Error)");
    adns__query_fail(qu,adns_s_serverfaulty);
    return;
  case rcode_servfail:
    adns__query_fail(qu,adns_s_servfail);
    return;
  case rcode_notimp:
    adns__warn(ads,serv,qu,"server claims not to implement our query");
    adns__query_fail(qu,adns_s_notimplemented);
    return;
  case rcode_refused:
    adns__warn(ads,serv,qu,"server refused our query");
    adns__query_fail(qu,adns_s_refused);
    return;
  default:
    adns__warn(ads,serv,qu,"server gave unknown response code %d",rcode);
    adns__query_fail(qu,adns_s_reasonunknown);
    return;
  }

  /* Now, take a look at the answer section, and see if it is complete.
   * If it has any CNAMEs we stuff them in the answer.
   */
  wantedrrs= 0;
  cbyte= anstart;
  for (rri= 0; rri<ancount; rri++) {
    rrstart= cbyte;
    st= adns__findrr(qu,serv, dgram,dglen,&cbyte,
		     &rrtype,&rrclass,&rdlength,&rdstart,
		     &ownermatched);
    if (st) { adns__query_fail(qu,st); return; }
    if (rrtype == -1) goto x_truncated;

    if (rrclass != DNS_CLASS_IN) {
      adns__diag(ads,serv,qu,"ignoring answer RR with wrong class %d (expected IN=%d)",
		 rrclass,DNS_CLASS_IN);
      continue;
    }
    if (!ownermatched) {
      if (ads->iflags & adns_if_debug) {
	adns__debug(ads,serv,qu,"ignoring RR with an unexpected owner %s",
		    adns__diag_domain(ads,serv,qu, &qu->vb, dgram,dglen,rrstart));
      }
      continue;
    }
    if (rrtype == adns_r_cname && /* fixme - implement adns_qf_nocname */
	(qu->typei->type & adns__rrt_typemask) != adns_r_cname) {
      if (!qu->cname_dgram) { /* Ignore second and subsequent CNAMEs */
	qu->cname_begin= rdstart;
	qu->cname_dglen= dglen;
	st= adns__parse_domain(ads,serv,qu, &qu->vb,
			       qu->flags & adns_qf_quoteok_cname ? pdf_quoteok : 0,
			       dgram,dglen, &rdstart,rdstart+rdlength);
	if (!qu->vb.used) goto x_truncated;
	if (st) { adns__query_fail(qu,st); return; }
	l= strlen(qu->vb.buf)+1;
	qu->answer->cname= adns__alloc_interim(qu,l);
	if (!qu->answer->cname) { adns__query_fail(qu,adns_s_nolocalmem); return; }

	qu->cname_dgram= adns__alloc_mine(qu,dglen);
	memcpy(qu->cname_dgram,dgram,dglen);

	memcpy(qu->answer->cname,qu->vb.buf,l);
	cname_here= 1;
	/* If we find the answer section truncated after this point we restart
	 * the query at the CNAME; if beforehand then we obviously have to use
	 * TCP.  If there is no truncation we can use the whole answer if
	 * it contains the relevant info.
	 */
      } else {
	adns__debug(ads,serv,qu,"ignoring duplicate CNAME (%s, as well as %s)",
		    adns__diag_domain(ads,serv,qu, &qu->vb, dgram,dglen,rdstart),
		    qu->answer->cname);
      }
    } else if (rrtype == (qu->typei->type & adns__rrt_typemask)) {
      wantedrrs++;
    } else {
      adns__debug(ads,serv,qu,"ignoring answer RR with irrelevant type %d",rrtype);
    }
  }

  /* We defer handling truncated responses here, in case there was a CNAME
   * which we could use.
   */
  if (flg_tc) goto x_truncated;
  
  nsstart= cbyte;

  if (!wantedrrs) {
    /* Oops, NODATA or NXDOMAIN or perhaps a referral (which would be a problem) */
    
    if (rcode == rcode_nxdomain) {
      adns__query_fail(qu,adns_s_nxdomain);
      return;
    }

    /* RFC2308: NODATA has _either_ a SOA _or_ _no_ NS records in authority section */
    foundsoa= 0; foundns= 0;
    for (rri= 0; rri<nscount; rri++) {
      rrstart= cbyte;
      st= adns__findrr(qu,serv, dgram,dglen,&cbyte,
		       &rrtype,&rrclass,&rdlength,&rdstart, 0);
      if (st) { adns__query_fail(qu,st); return; }
      if (rrtype==-1) goto x_truncated;
      if (rrclass != DNS_CLASS_IN) {
	adns__diag(ads,serv,qu,
		   "ignoring authority RR with wrong class %d (expected IN=%d)",
		   rrclass,DNS_CLASS_IN);
	continue;
      }
      if (rrtype == adns_r_soa_raw) { foundsoa= 1; break; }
      else if (rrtype == adns_r_ns_raw) { foundns= 1; }
    }

    if (foundsoa || !foundns) {
      /* Aha !  A NODATA response, good. */
      adns__query_fail(qu,adns_s_nodata);
      return;
    }

    /* Now what ?  No relevant answers, no SOA, and at least some NS's.
     * Looks like a referral.  Just one last chance ... if we came across
     * a CNAME in this datagram then we should probably do our own CNAME
     * lookup now in the hope that we won't get a referral again.
     */
    if (cname_here) goto x_restartquery;

    /* Bloody hell, I thought we asked for recursion ? */
    if (flg_rd) {
      adns__diag(ads,serv,qu,"server thinks we didn't ask for recursive lookup");
    }
    if (!flg_ra) {
      adns__diag(ads,serv,qu,"server is not willing to do recursive lookups for us");
      adns__query_fail(qu,adns_s_norecurse);
    } else {
      adns__diag(ads,serv,qu,"server claims to do recursion, but gave us a referral");
      adns__query_fail(qu,adns_s_serverfaulty);
    }
    return;
  }

  /* Now, we have some RRs which we wanted. */

  qu->answer->rrs.untyped= adns__alloc_interim(qu,qu->typei->rrsz*wantedrrs);
  if (!qu->answer->rrs.untyped) { adns__query_fail(qu,adns_s_nolocalmem); return; }

  typei= qu->typei;
  cbyte= anstart;
  rrsdata= qu->answer->rrs.bytes;
  
  nrrs= 0;
  for (rri=0; rri<ancount; rri++) {
    st= adns__findrr(qu,serv, dgram,dglen,&cbyte,
		     &rrtype,&rrclass,&rdlength,&rdstart,
		     &ownermatched);
    assert(!st); assert(rrtype != -1);
    if (rrclass != DNS_CLASS_IN ||
	rrtype != (qu->typei->type & adns__rrt_typemask) ||
	!ownermatched)
      continue;
    st= typei->parse(qu,serv, dgram,dglen, rdstart,rdstart+rdlength,
		     nsstart,&arstart, rrsdata+nrrs*typei->rrsz);
    if (st) { adns__query_fail(qu,st); return; }
    if (rdstart==-1) goto x_truncated;
    nrrs++;
  }
  assert(nrrs==wantedrrs);
  qu->answer->nrrs= nrrs;

  /* This may have generated some child queries ... */
  if (qu->children.head) {
    qu->state= query_child;
    LIST_LINK_TAIL(ads->childw,qu);
    return;
  }

  adns__query_done(qu);
  return;

 x_truncated:
  
  if (!flg_tc) {
    adns__diag(ads,serv,qu,"server sent datagram which points outside itself");
    adns__query_fail(qu,adns_s_serverfaulty);
    return;
  }
  qu->flags |= adns_qf_usevc;
  
 x_restartquery:
  
  if (qu->cname_dgram) {
    st= adns__mkquery_frdgram(qu->ads,&qu->vb,&qu->id,
			      qu->cname_dgram, qu->cname_dglen, qu->cname_begin,
			      qu->typei->type, qu->flags);
    if (st) { adns__query_fail(qu,st); return; }
    
    newquery= realloc(qu->query_dgram,qu->vb.used);
    if (!newquery) { adns__query_fail(qu,adns_s_nolocalmem); return; }
    
    qu->query_dgram= newquery;
    qu->query_dglen= qu->vb.used;
    memcpy(newquery,qu->vb.buf,qu->vb.used);
  }
  
  adns__reset_cnameonly(qu);
  adns__query_udp(qu,now);
}
