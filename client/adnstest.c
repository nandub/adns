/*
 * adnstest.c
 * - simple test program, not part of the library
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

#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "adns.h"

#include "config.h"

#ifndef OUTPUTSTREAM
# define OUTPUTSTREAM stdout
#endif

#ifndef HAVE_POLL
#undef poll
int poll(struct pollfd *ufds, int nfds, int timeout) {
  fputs("poll(2) not supported on this system\n",stderr);
  exit(5);
}
#define adns_beforepoll(a,b,c,d,e) 0
#define adns_afterpoll(a,b,c,d) 0
#endif

static void failure_status(const char *what, adns_status st) NONRETURNING;
static void failure_status(const char *what, adns_status st) {
  fprintf(stderr,"adns failure: %s: %s\n",what,adns_strerror(st));
  exit(2);
}

static void failure_errno(const char *what, int errnoval) NONRETURNING;
static void failure_errno(const char *what, int errnoval) {
  fprintf(stderr,"adns failure: %s: errno=%d\n",what,errnoval);
  exit(2);
}

static void usageerr(const char *why) NONRETURNING;
static void usageerr(const char *why) {
  fprintf(stderr,
	  "bad usage: %s\n"
	  "usage: adnstest [-<initflagsnum>[,<owninitflags>]] [/<initstring>]\n"
	  "              [ :<typenum>,... ]\n"
	  "              [ [<queryflagsnum>[,<ownqueryflags>]/]<domain> ... ]\n"
	  "initflags:   p  use poll(2) instead of select(2)\n"
	  "             s  use adns_wait with specified query, instead of 0\n"
	  "queryflags:  a  print status abbrevs instead of strings\n"
	  "exit status:  0 ok (though some queries may have failed)\n"
	  "              1 used by test harness to indicate test failed\n"
	  "              2 unable to submit or init or some such\n"
	  "              3 unexpected failure\n"
	  "              4 usage error\n"
	  "              5 operation not supported on this system\n",
	  why);
  exit(4);
}

static const adns_rrtype defaulttypes[]= {
  adns_r_a,
  adns_r_ns_raw,
  adns_r_cname,
  adns_r_soa_raw,
  adns_r_ptr_raw,
  adns_r_hinfo,
  adns_r_mx_raw,
  adns_r_txt,
  adns_r_rp_raw,
  
  adns_r_addr,
  adns_r_ns,
  adns_r_ptr,
  adns_r_mx,
  
  adns_r_soa,
  adns_r_rp,

  adns_r_none
};

static void dumptype(adns_status ri, const char *rrtn, const char *fmtn) {
  fprintf(stdout, "%s(%s)%s%s",
	  ri ? "?" : rrtn, ri ? "?" : fmtn ? fmtn : "-",
	  ri ? " " : "", ri ? adns_strerror(ri) : "");
}

static void fdom_split(const char *fdom, const char **dom_r, int *qf_r,
		       char *ownflags, int ownflags_l) {
  int qf;
  char *ep;

  qf= strtoul(fdom,&ep,0);
  if (*ep == ',' && strchr(ep,'/')) {
    ep++;
    while (*ep != '/') {
      if (--ownflags_l <= 0) { fputs("too many flags\n",stderr); exit(3); }
      *ownflags++= *ep++;
    }
  }
  if (*ep != '/') { *dom_r= fdom; *qf_r= 0; }
  else { *dom_r= ep+1; *qf_r= qf; }
  *ownflags= 0;
}

static int consistsof(const char *string, const char *accept) {
  return strspn(string,accept) == strlen(string);
}

int main(int argc, char *const *argv) {
  struct myctx {
    adns_query qu;
    int doneyet, found;
    const char *fdom;
  };
  
  adns_state ads;
  adns_query qu;
  struct myctx *mcs, *mc, *mcw;
  void *mcr;
  adns_answer *ans;
  const char *initstring, *rrtn, *fmtn;
  const char *const *fdomlist, *domain;
  char *show, *cp;
  int len, i, qc, qi, tc, ti, ch, qflags, initflagsnum, npollfds, npollfdsavail, timeout;
  struct pollfd *pollfds;
  adns_status ri;
  int r;
  const adns_rrtype *types;
  struct timeval now;
  adns_rrtype *types_a;
  char ownflags[10];
  char *ep;
  const char *initflags, *owninitflags;

  if (argv[0] && argv[1] && argv[1][0] == '-') {
    initflags= argv[1]+1;
    argv++;
  } else {
    initflags= "";
  }
  if (argv[0] && argv[1] && argv[1][0] == '/') {
    initstring= argv[1]+1;
    argv++;
  } else {
    initstring= 0;
  }

  initflagsnum= strtoul(initflags,&ep,0);
  if (*ep == ',') {
    owninitflags= ep+1;
    if (!consistsof(owninitflags,"ps")) usageerr("unknown owninitflag");
  } else if (!*ep) {
    owninitflags= "";
  } else {
    usageerr("bad <initflagsnum>[,<owninitflags>]");
  }
  
  if (argv[0] && argv[1] && argv[1][0] == ':') {
    for (cp= argv[1]+1, tc=1; (ch= *cp); cp++)
      if (ch==',') tc++;
    types_a= malloc(sizeof(*types_a)*(tc+1));
    if (!types_a) { perror("malloc types"); exit(3); }
    for (cp= argv[1]+1, ti=0; ti<tc; ti++) {
      types_a[ti]= strtoul(cp,&cp,10);
      if ((ch= *cp)) {
	if (ch != ',') usageerr("unexpected char (not comma) in or between types");
	cp++;
      }
    }
    *cp++= adns_r_none;
    types= types_a;
    argv++;
  } else {
    types= defaulttypes;
  }
  
  if (!(argv[0] && argv[1])) usageerr("no query domains supplied");
  fdomlist= (const char *const*)argv+1;

  for (qc=0; fdomlist[qc]; qc++);
  for (tc=0; types[tc] != adns_r_none; tc++);
  mcs= malloc(sizeof(*mcs)*qc*tc);
  if (!mcs) { perror("malloc mcs"); exit(3); }

  if (initstring) {
    r= adns_init_strcfg(&ads,
			(adns_if_debug|adns_if_noautosys|adns_if_checkc_freq)
			^initflagsnum,
			stdout,initstring);
  } else {
    r= adns_init(&ads,
		 (adns_if_debug|adns_if_noautosys)^initflagsnum,
		 0);
  }
  if (r) failure_errno("init",r);

  npollfdsavail= 0;
  pollfds= 0;
  
  for (qi=0; qi<qc; qi++) {
    fdom_split(fdomlist[qi],&domain,&qflags,ownflags,sizeof(ownflags));
    if (!consistsof(ownflags,"a")) usageerr("unknown ownqueryflag");
    for (ti=0; ti<tc; ti++) {
      mc= &mcs[qi*tc+ti];
      mc->doneyet= 0;
      mc->fdom= fdomlist[qi];

      fprintf(stdout,"%s flags %d type %d",domain,qflags,types[ti]);
      r= adns_submit(ads,domain,types[ti],qflags,mc,&mc->qu);
      if (r == ENOSYS) {
	fprintf(stdout," not implemented\n");
	mc->qu= 0;
	mc->doneyet= 1;
      } else if (r) {
	failure_errno("submit",r);
      } else {
	ri= adns_rr_info(types[ti], &rrtn,&fmtn,0, 0,0);
	putc(' ',stdout);
	dumptype(ri,rrtn,fmtn);
	fprintf(stdout," submitted\n");
      }
    }
  }

  for (;;) {
    for (qi=0; qi<qc; qi++) {
      for (ti=0; ti<tc; ti++) {
	mc= &mcs[qi*tc+ti];
	mc->found= 0;
      }
    }
    for (adns_forallqueries_begin(ads);
	 (qu= adns_forallqueries_next(ads,&mcr));
	 ) {
      mc= mcr;
      assert(qu == mc->qu);
      assert(!mc->doneyet);
      mc->found= 1;
    }
    mcw= 0;
    for (qi=0; qi<qc; qi++) {
      for (ti=0; ti<tc; ti++) {
	mc= &mcs[qi*tc+ti];
	if (mc->doneyet) continue;
	assert(mc->found);
	if (!mcw) mcw= mc;
      }
    }
    if (!mcw) break;

    if (strchr(owninitflags,'s')) {
      qu= mcw->qu;
      mc= mcw;
    } else {
      qu= 0;
      mc= 0;
    }

    if (strchr(owninitflags,'p')) {
      for (;;) {
	r= adns_check(ads,&qu,&ans,&mcr);
	if (r != EWOULDBLOCK) break;
	for (;;) {
	  npollfds= npollfdsavail;
	  timeout= -1;
	  r= adns_beforepoll(ads, pollfds, &npollfds, &timeout, 0);
	  if (r != ERANGE) break;
	  pollfds= realloc(pollfds,sizeof(*pollfds)*npollfds);
	  if (!pollfds) failure_errno("realloc pollfds",errno);
	  npollfdsavail= npollfds;
	}
	if (r) failure_errno("beforepoll",r);
	r= poll(pollfds,npollfds,timeout);
	if (r == -1) failure_errno("poll",errno);
	adns_afterpoll(ads,pollfds, r?npollfds:0, 0);
      }
    } else {
      r= adns_wait(ads,&qu,&ans,&mcr);
    }
    if (r) failure_errno("wait/check",r);
    
    if (mc) assert(mcr==mc);
    else mc= mcr;
    assert(qu==mc->qu);
    assert(!mc->doneyet);
    
    fdom_split(mc->fdom,&domain,&qflags,ownflags,sizeof(ownflags));

    if (gettimeofday(&now,0)) { perror("gettimeofday"); exit(3); }
      
    ri= adns_rr_info(ans->type, &rrtn,&fmtn,&len, 0,0);
    fprintf(stdout, "%s flags %d type ",domain,qflags);
    dumptype(ri,rrtn,fmtn);
    fprintf(stdout, "%s%s: %s; nrrs=%d; cname=%s; owner=%s; ttl=%ld\n",
	    ownflags[0] ? " ownflags=" : "", ownflags,
	    strchr(ownflags,'a')
	    ? adns_errabbrev(ans->status)
	    : adns_strerror(ans->status),
	    ans->nrrs,
	    ans->cname ? ans->cname : "$",
	    ans->owner ? ans->owner : "$",
	    (long)ans->expires - (long)now.tv_sec);
    if (ans->nrrs) {
      assert(!ri);
      for (i=0; i<ans->nrrs; i++) {
	ri= adns_rr_info(ans->type, 0,0,0, ans->rrs.bytes + i*len, &show);
	if (ri) failure_status("info",ri);
	fprintf(stdout," %s\n",show);
	free(show);
      }
    }
    free(ans);

    mc->doneyet= 1;
  }

  free(mcs);
  adns_finish(ads);
  
  exit(0);
}
