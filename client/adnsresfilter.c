/*
 * adnsresfilter.c
 * - filter which does resolving, not part of the library
 */
/*
 *  This file is
 *    Copyright (C) 1999 Ian Jackson <ian@davenant.greenend.org.uk>
 *
 *  It is part of adns, which is
 *    Copyright (C) 1997-1999 Ian Jackson <ian@davenant.greenend.org.uk>
 *    Copyright (C) 1999 Tony Finch <dot@dotat.at>
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <search.h>
#include <assert.h>
#include <ctype.h>

#include <sys/fcntl.h>

#include "adns.h"
#include "config.h"
#include "dlist.h"
#include "tvarith.h"

struct outqueuenode {
  struct outqueuenode *next, *back;
  void *buffer;
  char *textp;
  int textlen;
  struct timeval printbefore;
  struct treething *addr;
};

static int bracket, forever, timeout=10;
static adns_rrtype rrt= adns_r_ptr;

static int outblocked, inputeof;
static struct { struct outqueuenode *head, *tail; } outqueue;
static int peroutqueuenode, outqueuelen;

static struct sockaddr_in sa;
static adns_state ads;

static char addrtextbuf[14];
static int cbyte, inbyte, inbuf;
static unsigned char bytes[4];
static struct timeval printbefore;

struct treething {
  unsigned char bytes[4];
  adns_query qu;
  adns_answer *ans;
};

static struct treething *newthing;
static void *treeroot;

static int nonblock(int fd, int isnonblock) {
  int r;

  r= fcntl(fd,F_GETFL); 
  if (r==-1) return -1;
  r= fcntl(fd,F_SETFL, isnonblock ? r|O_NONBLOCK : r&~O_NONBLOCK);
  if (r==-1) return -1;
  return 0;
}

static void quit(int exitstatus) NONRETURNING;
static void quit(int exitstatus) {
  nonblock(0,0);
  nonblock(1,0);
  exit(exitstatus);
}

static void sysfail(const char *what) NONRETURNING;
static void sysfail(const char *what) {
  fprintf(stderr,"adnsresfilter: system call failed: %s: %s\n",what,strerror(errno));
  quit(2);
}

static void *xmalloc(size_t sz) {
  void *r;
  r= malloc(sz);  if (r) return r;
  sysfail("malloc");
}

static void outputerr(void) NONRETURNING;
static void outputerr(void) { sysfail("write to stdout"); }

static void usage(void) {
  if (printf("usage: adnsresfilter [<options ...>]\n"
	     "       adnsresfilter  -h|--help\n"
	     "options: -b|--brackets\n"
	     "         -w|--wait\n"
	     "         -t<timeout>|--timeout <timeout>\n"
	     "         -u|--unchecked\n")
      == EOF) outputerr();
}

static void usageerr(const char *why) NONRETURNING;
static void usageerr(const char *why) {
  fprintf(stderr,"adnsresfilter: bad usage: %s\n",why);
  usage();
  quit(1);
}

static void adnsfail(const char *what, int e) NONRETURNING;
static void adnsfail(const char *what, int e) {
  fprintf(stderr,"adnsresfilter: adns call failed: %s: %s\n",what,strerror(e));
  quit(2);
}

static int comparer(const void *a, const void *b) {
  return memcmp(a,b,4);
}

static void parseargs(const char *const *argv) {
  const char *arg;
  int c;

  while ((arg= *++argv)) {
    if (arg[0] != '-') usageerr("no non-option arguments are allowed");
    if (arg[1] == '-') {
      if (!strcmp(arg,"--brackets")) {
	bracket= 1;
      } else if (!strcmp(arg,"--unchecked")) {
	rrt= adns_r_ptr_raw;
      } else if (!strcmp(arg,"--wait")) {
	forever= 1;
      } else if (!strcmp(arg,"--help")) {
	usage(); quit(0);
      } else {
	usageerr("unknown long option");
      }
    } else {
      while ((c= *++arg)) {
	switch (c) {
	case 'b':
	  bracket= 1;
	  break;
	case 'u':
	  rrt= adns_r_ptr_raw;
	  break;
	case 'w':
	  forever= 1;
	  break;
	case 'h':
	  usage(); quit(0);
	default:
	  usageerr("unknown short option");
	}
      }
    }
  }
}

static void queueoutchar(int c) {
  struct outqueuenode *entry;
  
  entry= outqueue.tail;
  if (!entry->back || entry->addr || entry->textlen >= peroutqueuenode) {
    entry= xmalloc(sizeof(*entry));
    peroutqueuenode= !entry->back || entry->addr ? 32 : 
      peroutqueuenode >= 1024 ? 2048 : peroutqueuenode<<1;
    entry->buffer= xmalloc(peroutqueuenode);
    entry->textp= entry->buffer;
    entry->textlen= 0;
    entry->addr= 0;
    LIST_LINK_TAIL(outqueue,entry);
    outqueuelen++;
  }
  *entry->textp++= c;
  entry->textlen++;
}

static void queueoutstr(const char *str, int len) {
  while (len > 0) queueoutchar(*str++);
}

static void writestdout(struct outqueuenode *entry) {
  int r;

  while (entry->textlen) {
    r= write(1, entry->textp, entry->textlen);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) { outblocked= 1; break; }
      sysfail("write stdout");
    }
    assert(r < entry->textlen);
    entry->textp += r;
    entry->textlen -= r;
  }
  if (!entry->textlen) {
    LIST_UNLINK(outqueue,entry);
    free(entry->buffer);
    free(entry);
    outqueuelen--;
  }
}

static void replacetextwithname(struct outqueuenode *entry) {
  free(entry->buffer);
  entry->buffer= 0;
  entry->textp= entry->addr->ans->rrs.str[0];
  entry->textlen= strlen(entry->textp);
}

static void checkadnsqueries(void) {
  adns_query qu;
  adns_answer *ans;
  void *context;
  struct treething *foundthing;
  int r;

  for (;;) {
    qu= 0; context= 0; ans= 0;
    r= adns_check(ads,&qu,&ans,&context);
    if (r == ESRCH || r == EAGAIN) break;
    assert(!r);
    foundthing= context;
    foundthing->ans= ans;
    foundthing->qu= 0;
  }
}

static void restartbuf(void) {
  if (inbuf>0) queueoutstr(addrtextbuf,inbuf);
  inbuf= 0;
}

static void procaddr(void) {
  struct treething *foundthing;
  void **searchfound;
  struct outqueuenode *entry;
  int r;
  
  if (!newthing) {
    newthing= xmalloc(sizeof(struct treething));
    newthing->qu= 0;
    newthing->ans= 0;
  }

  memcpy(newthing->bytes,bytes,4);
  searchfound= tsearch(newthing,&treeroot,comparer);
  if (!searchfound) sysfail("tsearch");
  foundthing= *searchfound;

  if (foundthing == newthing) {
    memcpy(&sa.sin_addr,bytes,4);
    r= adns_submit_reverse(ads, (const struct sockaddr*)&sa,
			   rrt,0,foundthing,&foundthing->qu);
    if (r) adnsfail("submit",r);
  }
  entry= xmalloc(sizeof(*entry));
  entry->buffer= xmalloc(inbuf);
  entry->textp= entry->buffer;
  memcpy(entry->textp,addrtextbuf,inbuf);
  entry->textlen= inbuf;
  entry->addr= foundthing;
  LIST_LINK_TAIL(outqueue,entry);
  outqueuelen++;
  inbuf= 0;
  cbyte= -1;
}

static void startaddr(void) {
  bytes[cbyte=0]= 0;
  inbyte= 0;
}

static void readstdin(void) {
  char readbuf[512], *p;
  int r, c, nbyte;

  while ((r= read(0,readbuf,sizeof(readbuf))) <= 0) {
    if (r == 0) { inputeof= 1; return; }
    if (r == EAGAIN) return;
    if (r != EINTR) sysfail("read stdin");
  }
  for (p=readbuf; r>0; r--,p++) {
    c= *p++;
    if (cbyte==-1 && bracket && c=='[') {
      addrtextbuf[inbuf++]= c;
      startaddr();
    } else if (cbyte==-1 && !bracket && !isalnum(c)) {
      queueoutchar(c);
      startaddr();
    } else if (cbyte>=0 && inbyte<3 && c>='0' && c<='9' &&
	       (nbyte= bytes[cbyte]*10 + (c-'0')) <= 255) {
      bytes[cbyte]= nbyte;
      addrtextbuf[inbuf++]= c;
      inbyte++;
    } else if (cbyte>=0 && cbyte<3 && inbyte>0 && c=='.') {
      bytes[++cbyte]= 0;
      addrtextbuf[inbuf++]= c;
      inbyte= 0;
    } else if (cbyte==3 && inbyte>0 && bracket && c==']') {
      addrtextbuf[inbuf++]= c;
      procaddr();
    } else if (cbyte==3 && inbyte>0 && !bracket && !isalnum(c)) {
      procaddr();
      queueoutchar(c);
      startaddr();
    } else {
      restartbuf();
      queueoutchar(c);
      cbyte= -1;
      if (!bracket && !isalnum(c)) startaddr();
    }
  }
  if (cbyte==3 && inbyte>0 && !bracket) procaddr();
}

static void startup(void) {
  int r;

  if (setvbuf(stdout,0,_IOLBF,0)) sysfail("setvbuf stdout");
  if (nonblock(0,1)) sysfail("set stdin to nonblocking mode");
  if (nonblock(1,1)) sysfail("set stdout to nonblocking mode");
  memset(&sa,0,sizeof(sa));
  sa.sin_family= AF_INET;
  r= adns_init(&ads,0,0);  if (r) adnsfail("init",r);
  cbyte= -1;
  inbyte= -1;
  inbuf= 0;
  if (!bracket) startaddr();
}

int main(int argc, const char *const *argv) {
  int r, maxfd;
  fd_set readfds, writefds, exceptfds;
  struct outqueuenode *entry;
  struct timeval *tv, tvbuf, now;

  parseargs(argv);
  startup();

  while (!inputeof || outqueue.head) {
    maxfd= 2;
    tv= 0;
    FD_ZERO(&readfds); FD_ZERO(&writefds); FD_ZERO(&exceptfds);
    if ((entry= outqueue.head) && !outblocked) {
      if (!entry->addr) {
	writestdout(entry);
	continue;
      } else if (entry->addr->ans) {
	if (entry->addr->ans->nrrs) 
	  replacetextwithname(entry);
	entry->addr= 0;
	continue;
      }
      r= gettimeofday(&now,0);  if (r) sysfail("gettimeofday");
      if (forever) {
	tv= 0;
      } else if (!timercmp(&now,&entry->printbefore,<)) {
	entry->addr= 0;
	continue;
      } else {
	tvbuf.tv_sec= printbefore.tv_sec - now.tv_sec - 1;
	tvbuf.tv_usec= printbefore.tv_usec - now.tv_usec + 1000000;
	tvbuf.tv_sec += tvbuf.tv_usec / 1000000;
	tvbuf.tv_usec %= 1000000;
	tv= &tvbuf;
      }
      adns_beforeselect(ads,&maxfd,&readfds,&writefds,&exceptfds,
			&tv,&tvbuf,&now);
    }
    if (outblocked) FD_SET(1,&writefds);
    if (!inputeof && outqueuelen<1000) FD_SET(0,&readfds);
    
    r= select(maxfd,&readfds,&writefds,&exceptfds,tv);
    if (r < 0) { if (r == EINTR) continue; else sysfail("select"); }

    r= gettimeofday(&now,0);  if (r) sysfail("gettimeofday");
    adns_afterselect(ads,maxfd,&readfds,&writefds,&exceptfds,&now);
    checkadnsqueries();

    if (FD_ISSET(0,&readfds)) {
      if (!forever) {
	printbefore= now;
	timevaladd(&printbefore,timeout);
      }
      readstdin();
    } else if (FD_ISSET(1,&writefds)) {
      outblocked= 0;
    }
  }
  if (ferror(stdin) || fclose(stdin)) sysfail("read stdin");
  if (fclose(stdout)) sysfail("close stdout");
  if (nonblock(0,0)) sysfail("un-nonblock stdin");
  if (nonblock(1,0)) sysfail("un-nonblock stdout");
  exit(0);
}
