/*
 * dtest.c
 * - simple test program, not part of the library
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

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>

#include "adns.h"

static void failure(const char *what, adns_status st) {
  fprintf(stderr,"adns failure: %s: %s\n",what,adns_strerror(st));
  exit(2);
}

static const char *defaultargv[]= { "ns.chiark.greenend.org.uk", 0 };

static const adns_rrtype defaulttypes[]= {
  adns_r_a,
  adns_r_ns_raw,
  adns_r_cname,
  adns_r_ptr_raw,
  adns_r_mx_raw,
  adns_r_txt,
  adns_r_rp_raw,
  adns_r_addr,
  adns_r_ns,
  adns_r_mx,
  adns_r_none
};

static void dumptype(adns_status ri, const char *rrtn, const char *fmtn) {
  fprintf(stdout, "%s(%s)%s%s",
	  ri ? "?" : rrtn, ri ? "?" : fmtn ? fmtn : "-",
	  ri ? " " : "", ri ? adns_strerror(ri) : "");
}

int main(int argc, char *const *argv) {
  adns_state ads;
  adns_query *qus, qu;
  adns_answer *ans;
  const char *rrtn, *fmtn, *const *domlist;
  char *show, *cp;
  int len, i, qc, qi, tc, ti, ch;
  adns_status r, ri;
  const adns_rrtype *types;
  adns_rrtype *types_a;

  if (argv[0] && argv[1] && argv[1][0] == ':') {
    for (cp= argv[1]+1, tc=1; (ch= *cp); cp++)
      if (ch==',') tc++;
    types_a= malloc(sizeof(*types_a)*tc);
    if (!types_a) { perror("malloc types"); exit(3); }
    for (cp= argv[1]+1, ti=0; ti<tc; ti++) {
      types_a[ti]= strtoul(cp,&cp,10);
      if ((ch= *cp)) {
	if (ch != ':') {
	  fputs("usage: dtest [:<typenum>,...] [<domain> ...]",stderr);
	  exit(4);
	}
	cp++;
      }
    }
    types= types_a;
    argv++;
  } else {
    types= defaulttypes;
  }
  
  if (argv[0] && argv[1]) domlist= (const char *const*)argv+1;
  else domlist= defaultargv;

  for (qc=0; qc[domlist]; qc++);
  for (tc=0; types[tc] != adns_r_none; tc++);
  qus= malloc(sizeof(qus)*qc*tc);
  if (!qus) { perror("malloc qus"); exit(3); }

  r= adns_init(&ads,adns_if_debug|adns_if_noautosys,0);
  if (r) failure("init",r);

  for (qi=0; qi<qc; qi++) {
    for (ti=0; ti<tc; ti++) {
      fprintf(stdout,"%s type %d",domlist[qi],types[ti]);
      r= adns_submit(ads,domlist[qi],types[ti],0,0,&qus[qi*tc+ti]);
      if (r == adns_s_notimplemented) {
	fprintf(stdout," not implemented\n");
	qus[qi*tc+ti]= 0;
      } else if (r) {
	failure("submit",r);
      } else {
	ri= adns_rr_info(types[ti], &rrtn,&fmtn,0, 0,0);
	putchar(' ');
	dumptype(ri,rrtn,fmtn);
	fprintf(stdout," submitted\n");
      }
    }
  }

  for (qi=0; qi<qc; qi++) {
    for (ti=0; ti<tc; ti++) {
      qu= qus[qi*tc+ti];
      if (!qu) continue;
      r= adns_wait(ads,&qu,&ans,0);
      if (r) failure("wait",r);

      ri= adns_rr_info(ans->type, &rrtn,&fmtn,&len, 0,0);
      fprintf(stdout, "%s type ", domlist[qi]);
      dumptype(ri,rrtn,fmtn);
      fprintf(stdout, ": %s; nrrs=%d; cname=%s\n",
	      adns_strerror(ans->status),
	      ans->nrrs, ans->cname ? ans->cname : "$");
      if (ans->nrrs) {
	assert(!ri);
	for (i=0; i<ans->nrrs; i++) {
	  r= adns_rr_info(ans->type, 0,0,0, ans->rrs.bytes+i*len,&show);
	  if (r) failure("info",r);
	  printf(" %s\n",show);
	  free(show);
	}
      }
      free(ans);
    }
  }

  free(qus);
  exit(0);
}
