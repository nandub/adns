/*
 * query.c
 * - overall query management (allocation, completion)
 * - per-query memory management
 * - query submission and cancellation (user-visible and internal)
 */
/*
 *  This file is part of adns, which is Copyright (C) 1997-1999 Ian Jackson
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

#include "internal.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <sys/time.h>

#include "internal.h"

int adns__internal_submit(adns_state ads, adns_query *query_r,
			  const typeinfo *typei, vbuf *qumsg_vb, int id,
			  adns_queryflags flags, struct timeval now,
			  adns_status failstat, const qcontext *ctx) {
  adns_query qu;

  qu= malloc(sizeof(*qu)); if (!qu) goto x_nomemory;
  qu->answer= malloc(sizeof(*qu->answer)); if (!qu->answer) goto x_freequ_nomemory;

  qu->ads= ads;
  qu->state= query_udp;
  qu->back= qu->next= qu->parent= 0;
  LIST_INIT(qu->children);
  qu->siblings.next= qu->siblings.back= 0;
  LIST_INIT(qu->allocations);
  qu->interim_allocd= 0;
  qu->final_allocspace= 0;

  qu->typei= typei;
  adns__vbuf_init(&qu->vb);

  qu->cname_dgram= 0;
  qu->cname_dglen= qu->cname_begin= 0;
  
  qu->id= id;
  qu->flags= flags;
  qu->udpretries= 0;
  qu->udpnextserver= 0;
  qu->udpsent= qu->tcpfailed= 0;
  timerclear(&qu->timeout);
  memcpy(&qu->ctx,ctx,sizeof(qu->ctx));
  qu->expires= now.tv_sec + MAXTTLBELIEVE;

  qu->answer->status= adns_s_ok;
  qu->answer->cname= 0;
  qu->answer->type= typei->type;
  qu->answer->nrrs= 0;
  qu->answer->rrs= 0;
  qu->answer->rrsz= typei->rrsz;
  
  *query_r= qu;

  qu->query_dglen= qumsg_vb->used;
  if (qumsg_vb->used) {
    qu->query_dgram= malloc(qumsg_vb->used);
    if (!qu->query_dgram) {
      adns__query_fail(qu,adns_s_nomemory);
      return adns_s_ok;
    }
    memcpy(qu->query_dgram,qumsg_vb->buf,qumsg_vb->used);
  } else {
    qu->query_dgram= 0;
  }
  qu->vb= *qumsg_vb;
  adns__vbuf_init(qumsg_vb);
  
  if (failstat) {
    adns__query_fail(qu,failstat);
    return adns_s_ok;
  }
  adns__query_udp(qu,now);
  adns__autosys(ads,now);

  return adns_s_ok;

 x_freequ_nomemory:
  free(qu);
 x_nomemory:
  adns__vbuf_free(qumsg_vb);
  return adns_s_nomemory;
}

int adns_submit(adns_state ads,
		const char *owner,
		adns_rrtype type,
		adns_queryflags flags,
		void *context,
		adns_query *query_r) {
  qcontext ctx;
  int id, r, ol;
  vbuf vb, search_vb;
  adns_status stat;
  const typeinfo *typei;
  struct timeval now;

  typei= adns__findtype(type);
  if (!typei) return adns_s_unknownrrtype;
  
  ctx.ext= context;
  ctx.callback= 0;
  memset(&ctx.info,0,sizeof(ctx.info));
  
  r= gettimeofday(&now,0); if (r) return errno;
  id= 0;

  adns__vbuf_init(&vb);

  ol= strlen(owner);
  if (ol>DNS_MAXDOMAIN+1) { stat= adns_s_querydomaintoolong; goto xit; }
				 
  if (ol>=2 && owner[ol-1]=='.' && owner[ol-2]!='\\') { flags &= ~adns_qf_search; ol--; }

  stat= adns__mkquery(ads,&vb,&id, owner,ol, typei,flags);
			
 xit:
  return adns__internal_submit(ads,query_r, typei,&vb,id, flags,now, stat,&ctx);	
}

int adns_synchronous(adns_state ads,
		     const char *owner,
		     adns_rrtype type,
		     adns_queryflags flags,
		     adns_answer **answer_r) {
  adns_query qu;
  int r;
  
  r= adns_submit(ads,owner,type,flags,0,&qu);
  if (r) return r;

  r= adns_wait(ads,&qu,answer_r,0);
  if (r) adns_cancel(qu);

  return r;
}

static void *alloc_common(adns_query qu, size_t sz) {
  allocnode *an;

  if (!sz) return qu; /* Any old pointer will do */
  assert(!qu->final_allocspace);
  an= malloc(MEM_ROUND(MEM_ROUND(sizeof(*an)) + sz));
  if (!an) return 0;
  LIST_LINK_TAIL(qu->allocations,an);
  return (byte*)an + MEM_ROUND(sizeof(*an));
}

void *adns__alloc_interim(adns_query qu, size_t sz) {
  sz= MEM_ROUND(sz);
  qu->interim_allocd += sz;
  return alloc_common(qu,sz);
}

void *adns__alloc_mine(adns_query qu, size_t sz) {
  return alloc_common(qu,MEM_ROUND(sz));
}

void adns__transfer_interim(adns_query from, adns_query to, void *block, size_t sz) {
  allocnode *an;

  if (!block) return;
  an= (void*)((byte*)block - MEM_ROUND(sizeof(*an)));

  assert(!to->final_allocspace);
  assert(!from->final_allocspace);
  
  LIST_UNLINK(from->allocations,an);
  LIST_LINK_TAIL(to->allocations,an);

  from->interim_allocd -= sz;
  to->interim_allocd += sz;

  if (to->expires > from->expires) to->expires= from->expires;
}

void *adns__alloc_final(adns_query qu, size_t sz) {
  /* When we're in the _final stage, we _subtract_ from interim_alloc'd
   * each allocation, and use final_allocspace to point to the next free
   * bit.
   */
  void *rp;

  sz= MEM_ROUND(sz);
  rp= qu->final_allocspace;
  assert(rp);
  qu->interim_allocd -= sz;
  assert(qu->interim_allocd>=0);
  qu->final_allocspace= (byte*)rp + sz;
  return rp;
}

static void cancel_children(adns_query qu) {
  adns_query cqu, ncqu;

  for (cqu= qu->children.head; cqu; cqu= ncqu) {
    ncqu= cqu->siblings.next;
    adns_cancel(cqu);
  }
  LIST_INIT(qu->children);
}

void adns__reset_cnameonly(adns_query qu) {
  assert(!qu->final_allocspace);
  cancel_children(qu);
  qu->answer->nrrs= 0;
  qu->answer->rrs= 0;
  qu->interim_allocd= qu->answer->cname ? MEM_ROUND(strlen(qu->answer->cname)+1) : 0;
}

static void free_query_allocs(adns_query qu) {
  allocnode *an, *ann;

  cancel_children(qu);
  for (an= qu->allocations.head; an; an= ann) { ann= an->next; free(an); }
  adns__vbuf_free(&qu->vb);
}

void adns_cancel(adns_query qu) {
  switch (qu->state) {
  case query_udp: case query_tcpwait: case query_tcpsent:
    LIST_UNLINK(qu->ads->timew,qu);
    break;
  case query_child:
    LIST_UNLINK(qu->ads->childw,qu);
    break;
  case query_done:
    LIST_UNLINK(qu->ads->output,qu);
    break;
  default:
    abort();
  }
  free_query_allocs(qu);
  free(qu->answer);
  free(qu);
}

void adns__update_expires(adns_query qu, unsigned long ttl, struct timeval now) {
  time_t max;

  assert(ttl <= MAXTTLBELIEVE);
  max= now.tv_sec + ttl;
  if (qu->expires < max) return;
  qu->expires= max;
}

static void makefinal_query(adns_query qu) {
  adns_answer *ans;
  int rrn;

  ans= qu->answer;

  if (qu->interim_allocd) {
    ans= realloc(qu->answer, MEM_ROUND(MEM_ROUND(sizeof(*ans)) + qu->interim_allocd));
    if (!ans) goto x_nomem;
    qu->answer= ans;
  }

  qu->final_allocspace= (byte*)ans + MEM_ROUND(sizeof(*ans));
  adns__makefinal_str(qu,&ans->cname);
  
  if (ans->nrrs) {
    adns__makefinal_block(qu, &ans->rrs.untyped, ans->nrrs*ans->rrsz);

    for (rrn=0; rrn<ans->nrrs; rrn++)
      qu->typei->makefinal(qu, ans->rrs.bytes + rrn*ans->rrsz);
  }
  
  free_query_allocs(qu);
  return;
  
 x_nomem:
  qu->answer->status= adns_s_nomemory;
  qu->answer->cname= 0;
  adns__reset_cnameonly(qu);
  free_query_allocs(qu);
}

void adns__query_done(adns_query qu) {
  adns_answer *ans;
  adns_query parent;

  qu->id= -1;
  ans= qu->answer;

  if (ans->nrrs && qu->typei->diff_needswap) {
    if (!adns__vbuf_ensure(&qu->vb,qu->typei->rrsz)) {
      adns__query_fail(qu,adns_s_nomemory);
      return;
    }
    adns__isort(ans->rrs.bytes, ans->nrrs, ans->rrsz,
		qu->vb.buf,
		(int(*)(void*, const void*, const void*))qu->typei->diff_needswap,
		qu->ads);
  }

  ans->expires= qu->expires;
  parent= qu->parent;
  if (parent) {
    LIST_UNLINK_PART(parent->children,qu,siblings.);
    qu->ctx.callback(parent,qu);
    free_query_allocs(qu);
    free(qu);
  } else {
    makefinal_query(qu);
    LIST_LINK_TAIL(qu->ads->output,qu);
  }
}

void adns__query_fail(adns_query qu, adns_status stat) {
  adns__reset_cnameonly(qu);
  qu->answer->status= stat;
  adns__query_done(qu);
}

void adns__makefinal_str(adns_query qu, char **strp) {
  int l;
  char *before, *after;

  before= *strp;
  if (!before) return;
  l= strlen(before)+1;
  after= adns__alloc_final(qu,l);
  memcpy(after,before,l);
  *strp= after;  
}

void adns__makefinal_block(adns_query qu, void **blpp, size_t sz) {
  void *before, *after;

  before= *blpp;
  if (!before) return;
  after= adns__alloc_final(qu,sz);
  memcpy(after,before,sz);
  *blpp= after;
}
