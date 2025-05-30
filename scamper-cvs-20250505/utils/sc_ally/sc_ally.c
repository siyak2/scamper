/*
 * sc_ally : scamper driver to collect data on candidate aliases using the
 *           Ally method.
 *
 * $Id: sc_ally.c,v 1.65 2025/02/24 06:59:36 mjl Exp $
 *
 * Copyright (C) 2009-2011 The University of Waikato
 * Copyright (C) 2013-2015 The Regents of the University of California
 * Copyright (C) 2016-2024 Matthew Luckie
 * Author: Matthew Luckie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "internal.h"

#include "scamper_addr.h"
#include "scamper_list.h"
#include "ping/scamper_ping.h"
#include "dealias/scamper_dealias.h"
#include "scamper_file.h"
#include "lib/libscamperctrl/libscamperctrl.h"
#include "mjl_list.h"
#include "mjl_heap.h"
#include "mjl_splaytree.h"
#include "utils.h"

#define TEST_PING   1
#define TEST_ALLY   2

#define IPID_NONE   0
#define IPID_INCR   1
#define IPID_RAND   2
#define IPID_ECHO   3
#define IPID_CONST  4
#define IPID_UNRESP 5

#define METHOD_ICMP 1
#define METHOD_TCP  2
#define METHOD_UDP  3

typedef struct sc_ipidseq
{
  scamper_addr_t   *addr;
  uint8_t           udp;
  uint8_t           icmp;
  uint8_t           tcp;
} sc_ipidseq_t;

/*
 * sc_router_t
 *
 * collect a set of interfaces mapped to a router
 */
typedef struct sc_router
{
  slist_t          *addrs;
  dlist_node_t     *node;
} sc_router_t;

/*
 * sc_addr2router_t
 *
 * map an address to a router.
 */
typedef struct sc_addr2router
{
  scamper_addr_t   *addr;
  sc_router_t      *router;
} sc_addr2router_t;

/*
 * sc_pair_t
 *
 * associate pair of IP addresses
 */
typedef struct sc_pair
{
  scamper_addr_t   *a;
  scamper_addr_t   *b;
} sc_pair_t;

typedef struct sc_test
{
  int               type;
  void             *data;
} sc_test_t;

/*
 * sc_target
 *
 */
typedef struct sc_target
{
  scamper_addr_t   *addr;
  sc_test_t        *test;
  splaytree_node_t *node;
  slist_t          *blocked;
} sc_target_t;

/*
 * sc_allytest
 *
 * keep state about the pair-wise ally tests being conducted on a set
 * of candidate aliases, where the set contains more than two
 * addresses and we try to resolve the aliases efficiently.
 */
typedef struct sc_allytest
{
  sc_target_t      *a;
  sc_target_t      *b;
  int               attempt;
  int               method;
  slist_t          *addr_list;
  slist_node_t     *s1, *s2;
  splaytree_t      *router_tree;
  dlist_t          *router_list;
} sc_allytest_t;

/*
 * sc_pingtest
 *
 * keep state about the ping tests being used to find probe methods that
 * solicit packets with incrementing IPID values
 */
typedef struct sc_pingtest
{
  sc_target_t      *target;
  int               step;
} sc_pingtest_t;

/*
 * sc_waittest
 *
 * wait for the prescribed length of time before doing the prescribed
 * test.
 */
typedef struct sc_waittest
{
  struct timeval   tv;
  sc_test_t       *test;
} sc_waittest_t;

typedef struct sc_dump
{
  char  *descr;
  int  (*proc_ping)(const scamper_ping_t *ping);
  int  (*proc_ally)(const scamper_dealias_t *dealias);
  void (*finish)(void);
} sc_dump_t;

/* declare dump functions used for dump_funcs[] below */
static int  process_1_ally(const scamper_dealias_t *);
static void finish_1(void);
static int  process_2_ping(const scamper_ping_t *);
static void finish_2(void);
static int  process_3_ping(const scamper_ping_t *);
static int  process_3_ally(const scamper_dealias_t *);
static void finish_3(void);

static uint32_t               options       = 0;
static uint32_t               flags         = 0;
static scamper_ctrl_t        *scamper_ctrl  = NULL;
static scamper_inst_t        *scamper_inst  = NULL;
static int                    port          = 0;
static char                  *unix_name     = NULL;
static char                  *addressfile   = NULL;
static char                  *outfile_name  = NULL;
static char                  *outfile_type  = "warts";
static scamper_file_t        *outfile       = NULL;
static scamper_file_filter_t *ffilter       = NULL;
static scamper_file_t        *decode_sf     = NULL;
static scamper_file_readbuf_t *decode_rb    = NULL;
static int                    more          = 0;
static int                    probing       = 0;
static int                    waittime      = 5;
static int                    attempts      = 5;
static int                    error         = 0;
static int                    probe_wait    = 1000;
static int                    fudge         = 5000;
static struct timeval         now;
static FILE                  *text          = NULL;
static splaytree_t           *targets       = NULL;
static splaytree_t           *ipidseqs      = NULL;
static slist_t               *virgin        = NULL;
static heap_t                *waiting       = NULL;

static int                    dump_id       = 0;
static char                 **dump_files;
static int                    dump_filec    = 0;
static const sc_dump_t        dump_funcs[] = {
  {NULL, NULL, NULL, NULL},
  {"dump aliases inferred with Ally (IPID-based)",
   NULL, process_1_ally, finish_1},
  {"dump aliases inferred with Mercator (CSA-based)",
   process_2_ping, NULL, finish_2},
  {"dump aliases inferred with both Ally and Mercator",
   process_3_ping, process_3_ally, finish_3},
};
static int dump_funcc = sizeof(dump_funcs) / sizeof(sc_dump_t);

#define OPT_HELP        0x0001
#define OPT_ADDRFILE    0x0002
#define OPT_OUTFILE     0x0004
#define OPT_PORT        0x0008
#define OPT_UNIX        0x0010
#define OPT_TEXT        0x0020
#define OPT_DAEMON      0x0040
#define OPT_ATTEMPTS    0x0080
#define OPT_WAIT        0x0100
#define OPT_PROBEWAIT   0x0200
#define OPT_FUDGE       0x0400
#define OPT_OPTIONS     0x0800
#define OPT_DUMP        0x1000

#define FLAG_NOBS       0x0001
#define FLAG_TC         0x0002

static void usage(uint32_t opt_mask)
{
  int i;

  fprintf(stderr,
	  "usage: sc_ally [-D?] [-a infile] [-o outfile] [-p port] [-U unix]\n"
	  "               [-f fudge] [-i waitprobe] [-O options]\n"
	  "               [-q attempts] [-t log] [-w waittime]\n"
	  "\n"
	  "       sc_ally [-d dump-id] [-O options] <input-file>\n"
	  "\n");

  if(opt_mask == 0)
    {
      fprintf(stderr, "       sc_ally -?\n\n");
      return;
    }

  if(opt_mask & OPT_HELP)
    fprintf(stderr, "     -? give an overview of the usage of sc_ally\n");

  if(opt_mask & OPT_ADDRFILE)
    fprintf(stderr, "     -a input addressfile\n");

  if(opt_mask & OPT_OUTFILE)
    fprintf(stderr, "     -o output warts file\n");

  if(opt_mask & OPT_PORT)
    fprintf(stderr, "     -p port to find scamper on\n");

  if(opt_mask & OPT_UNIX)
    fprintf(stderr, "     -U unix domain to find scamper on\n");

  if(opt_mask & OPT_DUMP)
    {
      fprintf(stderr, "     -d dump-id\n");
      for(i=1; i<dump_funcc; i++)
	printf("        %2d : %s\n", i, dump_funcs[i].descr);
    }

  if(opt_mask & OPT_DAEMON)
    fprintf(stderr, "     -D start as daemon\n");

  if(opt_mask & OPT_PROBEWAIT)
    fprintf(stderr, "     -i inter-probe gap\n");

  if(opt_mask & OPT_FUDGE)
    fprintf(stderr, "     -f fudge\n");

  if(opt_mask & OPT_OPTIONS)
    {
      fprintf(stderr, "     -O: options\n");
      fprintf(stderr, "         nobs: do not consider byte swapped IPID values\n");
      fprintf(stderr, "         tc: dump transitive closure\n");
    }

  if(opt_mask & OPT_ATTEMPTS)
    fprintf(stderr, "     -q number of probes for ally\n");

  if(opt_mask & OPT_TEXT)
    fprintf(stderr, "     -t logfile\n");

  if(opt_mask & OPT_WAIT)
    fprintf(stderr, "     -w waittime\n");

  return;
}

static int check_options(int argc, char *argv[])
{
  int       ch;
  long      lo;
  char     *opts = "a:d:Df:i:o:O:p:q:t:U:w:?";
  char     *opt_port = NULL, *opt_probewait = NULL, *opt_dump = NULL;
  char     *opt_text = NULL, *opt_attempts = NULL, *opt_wait = NULL;
  char     *opt_fudge = NULL;

  while((ch = getopt(argc, argv, opts)) != -1)
    {
      switch(ch)
	{
	case 'a':
	  options |= OPT_ADDRFILE;
	  addressfile = optarg;
	  break;

	case 'd':
	  options |= OPT_DUMP;
	  opt_dump = optarg;
	  break;

	case 'D':
	  options |= OPT_DAEMON;
	  break;

	case 'f':
	  options |= OPT_FUDGE;
	  opt_fudge = optarg;
	  break;

	case 'i':
	  options |= OPT_PROBEWAIT;
	  opt_probewait = optarg;
	  break;

	case 'o':
	  options |= OPT_OUTFILE;
	  outfile_name = optarg;
	  break;

	case 'O':
	  if(strcasecmp(optarg, "nobs") == 0)
	    flags |= FLAG_NOBS;
	  else if(strcasecmp(optarg, "tc") == 0)
	    flags |= FLAG_TC;
	  break;

	case 'p':
	  options |= OPT_PORT;
	  opt_port = optarg;
	  break;

	case 'q':
	  options |= OPT_ATTEMPTS;
	  opt_attempts = optarg;
	  break;

	case 't':
	  options |= OPT_TEXT;
	  opt_text = optarg;
	  break;

	case 'U':
	  options |= OPT_UNIX;
	  unix_name = optarg;
	  break;

	case 'w':
	  options |= OPT_WAIT;
	  opt_wait = optarg;
	  break;

	case '?':
	default:
	  usage(0xffffffff);
	  return -1;
	}
    }

  if((options & (OPT_ADDRFILE|OPT_OUTFILE|OPT_DUMP)) != (OPT_ADDRFILE|OPT_OUTFILE) &&
     (options & (OPT_ADDRFILE|OPT_OUTFILE|OPT_DUMP)) != OPT_DUMP)
    {
      usage(0);
      return -1;
    }

  if(options & (OPT_ADDRFILE|OPT_OUTFILE))
    {
      if((options & (OPT_PORT|OPT_UNIX)) == 0 ||
	 (options & (OPT_PORT|OPT_UNIX)) == (OPT_PORT|OPT_UNIX) ||
	 argc - optind > 0)
	{
	  usage(OPT_ADDRFILE|OPT_OUTFILE|OPT_PORT|OPT_UNIX);
	  return -1;
	}

      if(string_endswith(outfile_name, ".gz") != 0)
	{
#ifdef HAVE_ZLIB
	  outfile_type = "warts.gz";
#else
	  usage(OPT_OUTFILE);
	  fprintf(stderr, "cannot write to %s: did not link against zlib\n",
		  outfile_name);
	  return -1;
#endif
	}
      else if(string_endswith(outfile_name, ".bz2") != 0)
	{
#ifdef HAVE_LIBBZ2
	  outfile_type = "warts.bz2";
#else
	  usage(OPT_OUTFILE);
	  fprintf(stderr, "cannot write to %s: did not link against libbz2\n",
		  outfile_name);
	  return -1;
#endif
	}
      else if(string_endswith(outfile_name, ".xz") != 0)
	{
#ifdef HAVE_LIBLZMA
	  outfile_type = "warts.xz";
#else
	  usage(OPT_OUTFILE);
	  fprintf(stderr, "cannot write to %s: did not link against liblzma\n",
		  outfile_name);
	  return -1;
#endif
	}

      if(options & OPT_PORT)
	{
	  if(string_tolong(opt_port, &lo) != 0 || lo < 1 || lo > 65535)
	    {
	      usage(OPT_PORT);
	      return -1;
	    }
	  port = lo;
	}

      if(options & OPT_FUDGE)
	{
	  if(string_tolong(opt_fudge, &lo) != 0 || lo < 0 || lo > 5000)
	    {
	      usage(OPT_FUDGE);
	      return -1;
	    }
	  fudge = lo;
	}

      if(options & OPT_ATTEMPTS)
	{
	  if(string_tolong(opt_attempts, &lo) != 0 || lo < 1 || lo > 10)
	    {
	      usage(OPT_ATTEMPTS);
	      return -1;
	    }
	  attempts = lo;
	}

      if(options & OPT_WAIT)
	{
	  if(string_tolong(opt_wait, &lo) != 0 || lo < 1 || lo > 60)
	    {
	      usage(OPT_WAIT);
	      return -1;
	    }
	  waittime = lo;
	}

      if(options & OPT_PROBEWAIT)
	{
	  /* probe gap between 200 and 2000ms */
	  if(string_tolong(opt_probewait, &lo) != 0 || lo < 200 || lo > 2000)
	    {
	      usage(OPT_PROBEWAIT);
	      return -1;
	    }
	  probe_wait = lo;
	}

      if(opt_text != NULL)
	{
	  if((text = fopen(opt_text, "w")) == NULL)
	    {
	      usage(OPT_TEXT);
	      fprintf(stderr, "could not open %s\n", opt_text);
	      return -1;
	    }
	}
    }
  else
    {
      if(argc - optind < 1)
	{
	  usage(0);
	  return -1;
	}
      if(string_tolong(opt_dump, &lo) != 0 || lo < 1 || lo > dump_funcc)
	{
	  usage(OPT_DUMP);
	  return -1;
	}
      dump_id    = lo;
      dump_files = argv + optind;
      dump_filec = argc - optind;
    }

  return 0;
}

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
static void print(char *format, ...) __attribute__((format(printf, 1, 2)));
#endif

static void print(char *format, ...)
{
  va_list ap;
  char msg[512];

  va_start(ap, format);
  vsnprintf(msg, sizeof(msg), format, ap);
  va_end(ap);

  printf("%ld: %s", (long int)now.tv_sec, msg);

  if(text != NULL)
    {
      fprintf(text, "%ld: %s", (long int)now.tv_sec, msg);
      fflush(text);
    }

  return;
}

static void status(char *format, ...)
{
  va_list ap;
  char pref[32];
  char msg[512];

  snprintf(pref, sizeof(pref), "p %d, w %d, v %d",
	   probing, heap_count(waiting), slist_count(virgin));

  va_start(ap, format);
  vsnprintf(msg, sizeof(msg), format, ap);
  va_end(ap);

  print("%s : %s\n", pref, msg);
  return;
}

static sc_test_t *sc_test_alloc(int type, void *data)
{
  sc_test_t *test;

  if((test = malloc_zero(sizeof(sc_test_t))) == NULL)
    {
      fprintf(stderr, "could not malloc test\n");
      return NULL;
    }

  test->type = type;
  test->data = data;
  return test;
}

static void sc_test_free(sc_test_t *test)
{
  if(test != NULL) free(test);
  return;
}

static int sc_waittest_cmp(const sc_waittest_t *a, const sc_waittest_t *b)
{
  return timeval_cmp(&b->tv, &a->tv);
}

static int sc_waittest(sc_test_t *test)
{
  sc_waittest_t *wt;

  if((wt = malloc_zero(sizeof(sc_waittest_t))) == NULL)
    return -1;

  timeval_add_s(&wt->tv, &now, waittime);
  wt->test = test;

  if(heap_insert(waiting, wt) == NULL)
    return -1;

  return 0;
}

static void sc_target_detach(sc_target_t *tg)
{
  sc_test_t *test;

  if(tg == NULL)
    return;

  if(tg->node != NULL)
    {
      splaytree_remove_node(targets, tg->node);
      tg->node = NULL;
    }

  if(tg->blocked != NULL)
    {
      while((test = slist_head_pop(tg->blocked)) != NULL)
	sc_waittest(test);
      slist_free(tg->blocked);
      tg->blocked = NULL;
    }

  return;
}

static void sc_target_free(sc_target_t *tg)
{
  if(tg == NULL)
    return;

  sc_target_detach(tg);

  if(tg->addr != NULL)
    scamper_addr_free(tg->addr);

  free(tg);
  return;
}

static int sc_target_cmp(const sc_target_t *a, const sc_target_t *b)
{
  return scamper_addr_cmp(a->addr, b->addr);
}

static sc_target_t *sc_target_alloc(scamper_addr_t *sa)
{
  sc_target_t *tg;
  if((tg = malloc_zero(sizeof(sc_target_t))) == NULL)
    {
      fprintf(stderr, "could not malloc target\n");
      return NULL;
    }
  tg->addr = scamper_addr_use(sa);
  return tg;
}

static int sc_target_block(sc_target_t *target, sc_test_t *block)
{
  if(target->blocked == NULL && (target->blocked = slist_alloc()) == NULL)
    {
      fprintf(stderr, "could not alloc target->blocked list\n");
      return -1;
    }

  if(slist_tail_push(target->blocked, block) == NULL)
    {
      fprintf(stderr, "could not add test to blocked list\n");
      return -1;
    }

  return 0;
}

static sc_target_t *sc_target_find(sc_target_t *target)
{
  return splaytree_find(targets, target);
}

static sc_target_t *sc_target_findaddr(scamper_addr_t *addr)
{
  sc_target_t findme;
  findme.addr = addr;
  return sc_target_find(&findme);
}

static int sc_target_add(sc_target_t *target)
{
  assert(target->node == NULL);
  assert(target->test != NULL);
  if((target->node = splaytree_insert(targets, target)) == NULL)
    {
      fprintf(stderr, "could not add target to tree\n");
      return -1;
    }
  return 0;
}

static void sc_pair_free(sc_pair_t *pair)
{
  if(pair->a != NULL) scamper_addr_free(pair->a);
  if(pair->b != NULL) scamper_addr_free(pair->b);
  free(pair);
  return;
}

static int sc_pair_cmp(const sc_pair_t *a, const sc_pair_t *b)
{
  int rc;
  if((rc = scamper_addr_cmp(a->a, b->a)) != 0)
    return rc;
  return scamper_addr_cmp(a->b, b->b);
}

static void sc_addr2router_free(sc_addr2router_t *a2r)
{
  if(a2r == NULL)
    return;
  if(a2r->addr != NULL)
    scamper_addr_free(a2r->addr);
  free(a2r);
  return;
}

static sc_addr2router_t *sc_addr2router_add(splaytree_t *tree, sc_router_t *r,
					    scamper_addr_t *a)
{
  sc_addr2router_t *a2r;
  if((a2r = malloc_zero(sizeof(sc_addr2router_t))) == NULL)
    return NULL;
  a2r->router = r;
  a2r->addr = scamper_addr_use(a);
  if(splaytree_insert(tree, a2r) == NULL)
    {
      sc_addr2router_free(a2r);
      return NULL;
    }
  if(slist_tail_push(r->addrs, a2r) == NULL)
    return NULL;
  return a2r;
}

static sc_addr2router_t *sc_addr2router_find(splaytree_t *t, scamper_addr_t *a)
{
  sc_addr2router_t fm; fm.addr = a;
  return splaytree_find(t, &fm);
}

static int sc_addr2router_human_cmp(const sc_addr2router_t *a,
				   const sc_addr2router_t *b)
{
  return scamper_addr_human_cmp(a->addr, b->addr);
}

static int sc_addr2router_cmp(const sc_addr2router_t *a,
			      const sc_addr2router_t *b)
{
  return scamper_addr_cmp(a->addr, b->addr);
}

static int sc_router_human_cmp(const sc_router_t *a, const sc_router_t *b)
{
  sc_addr2router_t *a_a2r = slist_head_item(a->addrs);
  sc_addr2router_t *b_a2r = slist_head_item(b->addrs);
  return sc_addr2router_human_cmp(a_a2r, b_a2r);
}

static void sc_router_free(sc_router_t *r)
{
  if(r == NULL)
    return;
  if(r->addrs != NULL) slist_free(r->addrs);
  free(r);
  return;
}

static sc_router_t *sc_router_alloc(dlist_t *list)
{
  sc_router_t *r;
  if((r = malloc_zero(sizeof(sc_router_t))) == NULL ||
     (r->addrs = slist_alloc()) == NULL ||
     (r->node = dlist_tail_push(list, r)) == NULL)
    {
      sc_router_free(r);
      return NULL;
    }
  return r;
}

static char *class_tostr(char *str, size_t len, uint8_t class)
{
  char *ptr;

  switch(class)
    {
    case IPID_NONE:   ptr = "none"; break;
    case IPID_INCR:   ptr = "incr"; break;
    case IPID_RAND:   ptr = "rand"; break;
    case IPID_ECHO:   ptr = "echo"; break;
    case IPID_CONST:  ptr = "const"; break;
    case IPID_UNRESP: ptr = "unresp"; break;
    default:
      snprintf(str, len, "class %d", class);
      return str;
    }

  snprintf(str, len, "%s", ptr);
  return str;
}

static void sc_ipidseq_free(sc_ipidseq_t *seq)
{
  if(seq == NULL)
    return;
  if(seq->addr != NULL)
    scamper_addr_free(seq->addr);
  free(seq);
  return;
}

static int sc_ipidseq_cmp(const sc_ipidseq_t *a, const sc_ipidseq_t *b)
{
  return scamper_addr_cmp(a->addr, b->addr);
}

static sc_ipidseq_t *sc_ipidseq_alloc(scamper_addr_t *addr)
{
  sc_ipidseq_t *seq;

  if((seq = malloc_zero(sizeof(sc_ipidseq_t))) == NULL)
    return NULL;

  seq->addr = scamper_addr_use(addr);

  if(splaytree_insert(ipidseqs, seq) == NULL)
    {
      scamper_addr_free(seq->addr);
      free(seq);
      return NULL;
    }

  return seq;
}

static sc_ipidseq_t *sc_ipidseq_find(scamper_addr_t *addr)
{
  sc_ipidseq_t fm; fm.addr = addr;
  return splaytree_find(ipidseqs, &fm);
}

/*
 * sc_ipidseq_method
 *
 * prefer icmp echo because it is benign.
 * prefer tcp to udp because it returns fewer false negatives --shared
 * counter held centrally (TCP) vs held on line card (UDP) on some routers.
 */
static int sc_ipidseq_method(sc_ipidseq_t *a, sc_ipidseq_t *b)
{
  if(a->icmp == IPID_INCR && a->icmp == b->icmp)
    return METHOD_ICMP;
  if(a->tcp == IPID_INCR && a->tcp == b->tcp)
    return METHOD_TCP;
  if(a->udp == IPID_INCR && a->udp == b->udp)
    return METHOD_UDP;
  return 0;
}

/*
 * sc_allytest_router
 *
 * add a new router to the set and return a pointer to it.
 */
static sc_router_t *sc_allytest_router(sc_allytest_t *at)
{
  sc_router_t *r = NULL;
  if((at->router_list == NULL && (at->router_list=dlist_alloc()) == NULL) ||
     (r = sc_router_alloc(at->router_list)) == NULL)
    {
      print("could not add new router: %s\n", strerror(errno));
      return NULL;
    }
  return r;
}

static sc_addr2router_t *sc_allytest_find(sc_allytest_t *at, scamper_addr_t *a)
{
  if(at->router_tree == NULL)
    return NULL;
  return sc_addr2router_find(at->router_tree, a);
}

static sc_addr2router_t *sc_allytest_addr2router(sc_allytest_t *at,
						 sc_router_t *rtr,
						 scamper_addr_t *addr)
{
  sc_addr2router_t *a2r;

  if(at->router_tree == NULL &&
     (at->router_tree =
      splaytree_alloc((splaytree_cmp_t)sc_addr2router_cmp)) == NULL)
    {
      print("could not alloc at->router_tree: %s\n", strerror(errno));
      return NULL;
    }

  if((a2r = sc_addr2router_add(at->router_tree, rtr, addr)) == NULL)
    {
      print("could not add a2r to rtr->addrs\n");
      return NULL;
    }

  return a2r;
}

static int sc_allytest_pair(sc_allytest_t *at, scamper_addr_t *a,
			    scamper_addr_t *b)
{
  sc_addr2router_t *a2r_a = sc_allytest_find(at, a);
  sc_addr2router_t *a2r_b = sc_allytest_find(at, b);
  sc_router_t *r;

  if(a2r_a == NULL && a2r_b == NULL)
    {
      if((r = sc_allytest_router(at)) == NULL ||
	 sc_allytest_addr2router(at, r, a) == NULL ||
	 sc_allytest_addr2router(at, r, b) == NULL)
	return -1;
    }
  else if(a2r_a != NULL && a2r_b != NULL)
    {
      if(a2r_a->router != a2r_b->router)
	{
	  r = a2r_a->router;
	  while((a2r_a = slist_head_pop(r->addrs)) != NULL)
	    {
	      a2r_a->router = a2r_b->router;
	      if(slist_tail_push(a2r_b->router->addrs, a2r_a) == NULL)
		return -1;
	    }
	  dlist_node_pop(at->router_list, r->node);
	  sc_router_free(r);
	}
    }
  else if(a2r_a != NULL)
    {
      if(sc_allytest_addr2router(at, a2r_a->router, b) == NULL)
	return -1;
    }
  else
    {
      if(sc_allytest_addr2router(at, a2r_b->router, a) == NULL)
	return -1;
    }

  return 0;
}

/*
 * sc_allytest_next
 *
 * figure out the next pair of IP addresses that should be tested.  the
 * function works by iterating through the addresses in the list,
 * only testing the addresses that do not belong to the same router
 */
static void sc_allytest_next(sc_allytest_t *at)
{
  scamper_addr_t *a, *b;
  sc_ipidseq_t *aseq, *bseq;

  if(at->a != NULL)
    {
      sc_target_free(at->a);
      at->a = NULL;
    }
  if(at->b != NULL)
    {
      sc_target_free(at->b);
      at->b = NULL;
    }
  at->method  = 0;
  at->attempt = 0;

  for(;;)
    {
      if((at->s2 = slist_node_next(at->s2)) == NULL &&
	 ((at->s1 = slist_node_next(at->s1)) == NULL ||
	  (at->s2 = slist_node_next(at->s1)) == NULL))
	{
	  at->s1 = at->s2 = NULL;
	  break;
	}

      a = slist_node_item(at->s1);
      b = slist_node_item(at->s2);

      /*
       * if these addresses have been assigned to routers already, we
       * don't need to probe them
       */
      if(sc_allytest_find(at, a) != NULL && sc_allytest_find(at, b) != NULL)
	continue;

      /*
       * if we cannot probe them because they do not have a common
       * probe method to use, then skip them, though this could be
       * relaxed in the future to use different probe methods if we
       * assume a single shared counter.
       */
      if((aseq = sc_ipidseq_find(a)) != NULL &&
	 (bseq = sc_ipidseq_find(b)) != NULL &&
	 sc_ipidseq_method(aseq, bseq) == 0)
	continue;

      break;
    }

  return;
}

static void sc_pingtest_free(sc_pingtest_t *pt)
{
  if(pt == NULL)
    return;
  if(pt->target != NULL)
    sc_target_free(pt->target);
  free(pt);
  return;
}

static sc_test_t *sc_pingtest_new(scamper_addr_t *addr)
{
  sc_pingtest_t *pt;
  sc_target_t *tg;

  assert(addr != NULL);

  if((pt = malloc_zero(sizeof(sc_pingtest_t))) == NULL)
    {
      fprintf(stderr, "could not malloc pingtest\n");
      goto err;
    }

  if((tg = malloc_zero(sizeof(sc_target_t))) == NULL)
    {
      fprintf(stderr, "could not malloc target\n");
      goto err;
    }
  tg->addr = scamper_addr_use(addr);
  pt->target = tg;

  /* create a generic test structure which we put in a list of tests */
  if((pt->target->test = sc_test_alloc(TEST_PING, pt)) == NULL)
    goto err;

  return pt->target->test;

 err:
  if(pt != NULL) sc_pingtest_free(pt);
  return NULL;
}

static void sc_allytest_free(sc_allytest_t *at)
{
  if(at == NULL)
    return;
  if(at->addr_list != NULL)
    slist_free_cb(at->addr_list, (slist_free_t)scamper_addr_free);
  if(at->router_tree != NULL)
    splaytree_free(at->router_tree, (splaytree_free_t)sc_addr2router_free);
  if(at->router_list != NULL)
    dlist_free_cb(at->router_list, (dlist_free_t)sc_router_free);
  if(at->a != NULL) sc_target_free(at->a);
  if(at->b != NULL) sc_target_free(at->b);
  free(at);
  return;
}

static int sc_allytest_new(slist_t *list)
{
  sc_allytest_t *at = NULL;
  sc_test_t *test = NULL;

  if((at = malloc_zero(sizeof(sc_allytest_t))) == NULL ||
     (test = sc_test_alloc(TEST_ALLY, at)) == NULL)
    goto err;

  at->s1 = slist_head_node(list);
  at->s2 = slist_node_next(at->s1);
  at->addr_list = list;
  slist_tail_push(virgin, test);
  return 0;

 err:
  print("could not alloc sc_allytest_t: %s\n", strerror(errno));
  if(at != NULL) sc_allytest_free(at);
  return -1;
}

static int addressfile_line(char *buf, void *param)
{
  static int line = 0;
  splaytree_t *tree = NULL;
  slist_t *list = NULL;
  scamper_addr_t *sa;
  char *a, *b;
  int last = 0;

  line++;

  if(buf[0] == '\0' || buf[0] == '#')
    return 0;

  if((list = slist_alloc()) == NULL ||
     (tree = splaytree_alloc((splaytree_cmp_t)scamper_addr_cmp)) == NULL)
    goto err;

  a = b = buf;
  for(;;)
    {
      for(;;)
	{
	  if(*b == '\0')
	    {
	      last = 1;
	      break;
	    }
	  if(*b == ' ')
	    {
	      *b = '\0';
	      break;
	    }
	  b++;
	}

      if((sa = scamper_addr_fromstr_ipv4(a)) == NULL)
	{
	  fprintf(stderr, "could not resolve %s on line %d\n", a, line);
	  goto err;
	}
      if(splaytree_find(tree, sa) != NULL)
	{
	  fprintf(stderr, "%s occurs twice on line %d\n", a, line);
	  goto err;
	}
      if(splaytree_insert(tree, sa) == NULL ||
	 slist_tail_push(list, sa) == NULL)
	{
	  scamper_addr_free(sa);
	  goto err;
	}

      if(last != 0)
	break;
      b++; a = b;
    }

  if(slist_count(list) < 2)
    goto err;

  splaytree_free(tree, NULL); tree = NULL;
  if(sc_allytest_new(list) != 0)
    goto err;
  return 0;

 err:
  if(tree != NULL) splaytree_free(tree, NULL);
  if(list != NULL) slist_free_cb(list, (slist_free_t)scamper_addr_free);
  return -1;
}

static int ping_classify(scamper_ping_t *ping)
{
  const scamper_ping_probe_t *tx;
  const scamper_ping_reply_t *rx;
  int rc = -1, echo = 0, bs = 0, nobs = 0;
  int i, samples[65536];
  uint32_t u32, f, n0, n1;
  slist_t *list = NULL;
  slist_node_t *ln0, *ln1;
  uint8_t stop_reason;
  uint16_t ping_sent, reply_ipid;

  stop_reason = scamper_ping_stop_reason_get(ping);
  if(stop_reason == SCAMPER_PING_STOP_NONE ||
     stop_reason == SCAMPER_PING_STOP_ERROR)
    return IPID_UNRESP;

  if((list = slist_alloc()) == NULL)
    goto done;

  memset(samples, 0, sizeof(samples));
  ping_sent = scamper_ping_sent_get(ping);
  for(i=0; i<ping_sent; i++)
    {
      if((tx = scamper_ping_probe_get(ping, i)) != NULL &&
	 (rx = scamper_ping_probe_reply_get(tx, 0)) != NULL &&
	 scamper_ping_reply_is_from_target(ping, rx))
	{
	  /*
	   * if at least two of four samples have the same ipid as what was
	   * sent, then declare it echos.  this handles the observed case
	   * where some responses echo but others increment.
	   */
	  reply_ipid = scamper_ping_reply_ipid_get(rx);
	  if(scamper_ping_probe_ipid_get(tx) == reply_ipid && ++echo > 1)
	    {
	      rc = IPID_ECHO;
	      goto done;
	    }

	  /*
	   * if two responses have the same IPID value, declare that it
	   * replies with a constant IPID
	   */
	  if(++samples[reply_ipid] > 1)
	    {
	      rc = IPID_CONST;
	      goto done;
	    }

	  if(slist_tail_push(list, (void *)rx) == NULL)
	    goto done;
	}
    }
  if(slist_count(list) < attempts)
    {
      rc = IPID_UNRESP;
      goto done;
    }

  f = (fudge == 0) ? 5000 : fudge;

  ln0 = slist_head_node(list);
  ln1 = slist_node_next(ln0);
  while(ln1 != NULL)
    {
      rx = slist_node_item(ln0); n0 = scamper_ping_reply_ipid_get(rx);
      rx = slist_node_item(ln1); n1 = scamper_ping_reply_ipid_get(rx);

      if(n0 < n1)
	u32 = n1 - n0;
      else
	u32 = (n1 + 0x10000) - n0;
      if(u32 <= f)
	nobs++;

      if((flags & FLAG_NOBS) == 0)
	{
	  n0 = byteswap16(n0);
	  n1 = byteswap16(n1);
	  if(n0 < n1)
	    u32 = n1 - n0;
	  else
	    u32 = (n1 + 0x10000) - n0;
	  if(u32 <= f)
	    bs++;
	}

      ln0 = ln1;
      ln1 = slist_node_next(ln0);
    }

  if(nobs != attempts-1 && bs != attempts-1)
    rc = IPID_RAND;
  else
    rc = IPID_INCR;

 done:
  if(list != NULL) slist_free(list);
  return rc;
}

static int test_ping_next(sc_test_t *test)
{
  sc_pingtest_t *pt = test->data;
  char addr[64], icmp[10], tcp[10], udp[10];
  sc_ipidseq_t *seq;

  pt->step++;

  scamper_addr_tostr(pt->target->addr, addr, sizeof(addr));
  if(pt->step < 3)
    {
      if(sc_waittest(test) != 0)
	return -1;
      status("wait ping %s step %d", addr, pt->step);
      return 0;
    }

  seq = sc_ipidseq_find(pt->target->addr); assert(seq != NULL);
  status("done ping %s icmp %s udp %s tcp %s", addr,
	 class_tostr(icmp, sizeof(icmp), seq->icmp),
	 class_tostr(udp, sizeof(udp), seq->udp),
	 class_tostr(tcp, sizeof(tcp), seq->tcp));

  sc_pingtest_free(pt);
  sc_test_free(test);

  return 0;
}

static int process_ping(sc_test_t *test, scamper_ping_t *ping)
{
  scamper_addr_t *dst;
  sc_ipidseq_t *seq;
  int class;

  assert(ping != NULL);
  dst = scamper_ping_dst_get(ping);

  if((seq = sc_ipidseq_find(dst)) == NULL &&
     (seq = sc_ipidseq_alloc(dst)) == NULL)
    {
      scamper_ping_free(ping);
      return -1;
    }

  class = ping_classify(ping);

  if(scamper_ping_method_is_udp(ping))
    seq->udp = class;
  else if(scamper_ping_method_is_tcp(ping))
    seq->tcp = class;
  else if(scamper_ping_method_is_icmp(ping))
    seq->icmp = class;

  scamper_ping_free(ping); ping = NULL;

  return test_ping_next(test);
}

static int test_ally_next(sc_test_t *test, uint8_t result)
{
  sc_allytest_t *at = test->data;
  scamper_addr_t *a = at->a->addr;
  scamper_addr_t *b = at->b->addr;
  size_t off = 0;
  char msg[512];
  char buf[64];
  int rc = 0;

  string_concat3(msg, sizeof(msg), &off, "set ally ",
		 scamper_addr_tostr(a, buf, sizeof(buf)), ":");
  string_concat2(msg, sizeof(msg), &off,
		 scamper_addr_tostr(b, buf, sizeof(buf)), " ");

  at->attempt++;
  if(result == SCAMPER_DEALIAS_RESULT_NONE && at->attempt <= 4)
    {
      string_concaf(msg, sizeof(msg), &off, "wait %d", at->attempt);
      if(sc_waittest(test) != 0)
	rc = -1;
    }
  else
    {
      if(result == SCAMPER_DEALIAS_RESULT_ALIASES)
	{
	  string_concat(msg, sizeof(msg), &off, "aliases");
	  if(sc_allytest_pair(at, a, b) != 0)
	    rc = -1;
	}
      else if(result == SCAMPER_DEALIAS_RESULT_NOTALIASES)
	string_concat(msg, sizeof(msg), &off, "not aliases");
      else
	string_concat(msg, sizeof(msg), &off, "no result");

      sc_allytest_next(at);
      if(at->s1 == NULL)
	{
	  sc_allytest_free(at);
	  sc_test_free(test);
	}
      else if(sc_waittest(test) != 0)
	rc = -1;
    }

  status("%s", msg);
  return rc;
}

static int process_ally(sc_test_t *test, scamper_dealias_t *dealias)
{
  uint8_t result = scamper_dealias_result_get(dealias);
  assert(dealias != NULL);
  scamper_dealias_free(dealias);
  return test_ally_next(test, result);
}

static int sc_test_ping(sc_test_t *test, char *cmd, size_t len, sc_test_t **out)
{
  sc_pingtest_t *pt = test->data;
  scamper_addr_t *dst = pt->target->addr;
  sc_target_t *found;
  size_t off = 0;
  char buf[64];

  assert(pt->step >= 0);
  assert(pt->step < 3);

  /* first, check to see if the test is runnable. if not block */
  if((found = sc_target_find(pt->target)) != NULL && found->test != test)
    {
      if(sc_target_block(found, test) != 0)
	return -1;
      return 0;
    }
  else if(found == NULL)
    {
      /* add the test to the blocked list */
      if(sc_target_add(pt->target) != 0)
	return -1;
    }

  string_concat(cmd, len, &off, "ping -P ");
  if(pt->step == 0)
    string_concat(cmd, len, &off, "udp-dport");
  else if(pt->step == 1)
    string_concat(cmd, len, &off, "icmp-echo");
  else if(pt->step == 2)
    string_concat(cmd, len, &off, "tcp-ack-sport");
  else
    return -1;
  string_concaf(cmd, len, &off, " -i %d", probe_wait / 1000);
  if((probe_wait % 1000) != 0)
    string_concaf(cmd, len, &off, ".%03d", probe_wait % 1000);
  string_concaf(cmd, len, &off, " -c %d -o %d %s", attempts + 2, attempts,
		scamper_addr_tostr(dst, buf, sizeof(buf)));

  *out = test;
  return off;
}

static int sc_test_ipidseq(sc_test_t *test, scamper_addr_t *addr,
			   char *cmd, size_t len, sc_test_t **out)
{
  sc_pingtest_t *pt;
  sc_target_t *found;
  sc_test_t *tt;

  if((found = sc_target_findaddr(addr)) != NULL)
    {
      if(sc_target_block(found, test) != 0)
	return -1;
      return 0;
    }

  if((tt = sc_pingtest_new(addr)) == NULL)
    return -1;
  pt = tt->data;
  if(sc_target_block(pt->target, test) != 0)
    return -1;
  if(sc_target_add(pt->target) != 0)
    return -1;
  return sc_test_ping(tt, cmd, len, out);
}

static int sc_test_ally(sc_test_t *test, char *cmd, size_t len, sc_test_t **out)
{
  static const char *method[] = {"icmp-echo", "tcp-ack-sport", "udp-dport"};
  sc_allytest_t *at = test->data;
  scamper_addr_t *a, *b;
  sc_ipidseq_t *aseq;
  sc_ipidseq_t *bseq;
  sc_target_t *found;
  char ab[64], bb[64];
  size_t off = 0;

  for(;;)
    {
      a = slist_node_item(at->s1);
      b = slist_node_item(at->s2);
      if(at->method != 0)
	break;
      if((aseq = sc_ipidseq_find(a)) == NULL)
	return sc_test_ipidseq(test, a, cmd, len, out);
      if((bseq = sc_ipidseq_find(b)) == NULL)
	return sc_test_ipidseq(test, b, cmd, len, out);
      if((at->method = sc_ipidseq_method(aseq, bseq)) == 0)
	{
	  sc_allytest_next(at);
	  if(at->s1 == NULL)
	    {
	      sc_allytest_free(at);
	      sc_test_free(test);
	      return 0;
	    }
	  continue;
	}
      break;
    }

  if(at->a == NULL)
    {
      if((at->a = sc_target_alloc(a)) == NULL)
	return -1;
      at->a->test = test;
    }
  if(at->b == NULL)
    {
      if((at->b = sc_target_alloc(b)) == NULL)
	return -1;
      at->b->test = test;
    }

  if(((found = sc_target_find(at->a)) != NULL && found->test != test) ||
     ((found = sc_target_find(at->b)) != NULL && found->test != test))
    {
      if(sc_target_block(found, test) != 0)
	return -1;
      return 0;
    }

  if((sc_target_find(at->a) == NULL && sc_target_add(at->a) != 0) ||
     (sc_target_find(at->b) == NULL && sc_target_add(at->b) != 0))
    return -1;

  string_concat(cmd, len, &off, "dealias -m ally");
  if(fudge == 0)
    string_concat(cmd, len, &off, " -O inseq");
  else
    string_concaf(cmd, len, &off, " -f %d", fudge);
  if(flags & FLAG_NOBS)
    string_concat(cmd, len, &off, " -O nobs");
  string_concaf(cmd, len, &off, " -W %d -q %d -p '-P %s' %s %s",
		probe_wait, attempts, method[at->method-1],
		scamper_addr_tostr(at->a->addr, ab, sizeof(ab)),
		scamper_addr_tostr(at->b->addr, bb, sizeof(bb)));

  *out = test;
  return off;
}

static int do_method(void)
{
  static int (*const func[])(sc_test_t *, char *, size_t, sc_test_t **) = {
    sc_test_ping, /* TEST_PING */
    sc_test_ally, /* TEST_ALLY */
  };

  sc_waittest_t *wt;
  sc_test_t *test, *t = NULL;
  char cmd[512];
  int off;

  if(more < 1)
    return 0;

  for(;;)
    {
      if((wt = heap_head_item(waiting)) != NULL &&
	 timeval_cmp(&now, &wt->tv) >= 0)
	{
	  test = wt->test;
	  heap_remove(waiting);
	  free(wt);
	}
      else if((test = slist_head_pop(virgin)) == NULL)
	{
	  return 0;
	}

      /* something went wrong */
      if((off = func[test->type-1](test, cmd, sizeof(cmd), &t)) == -1)
	{
	  fprintf(stderr, "something went wrong\n");
	  error = 1;
	  return -1;
	}

      /* got a command, send it */
      if(off != 0)
	{
	  assert(t != NULL);
	  if(scamper_inst_do(scamper_inst, cmd, t) == NULL)
	    {
	      fprintf(stderr, "could not send %s\n", cmd);
	      return -1;
	    }
	  probing++;
	  more--;

	  print("p %d, w %d, v %d : %s\n", probing, heap_count(waiting),
		slist_count(virgin), cmd);

	  break;
	}
    }

  return 0;
}

/*
 * do_files
 *
 * open a scamper_file_readbuf_t feed warts data into the scamper_file
 * to decode it
 *
 * also open a file to send the binary warts data file to.
 */
static int do_files(void)
{
  if((outfile = scamper_file_open(outfile_name, 'w', outfile_type)) == NULL)
    {
      fprintf(stderr, "could not open output file\n");
      return -1;
    }

  if((decode_sf = scamper_file_opennull('r', "warts")) == NULL)
    {
      fprintf(stderr, "could not alloc decode_sf\n");
      return -1;
    }
  if((decode_rb = scamper_file_readbuf_alloc()) == NULL)
    {
      fprintf(stderr, "could not alloc decode_rb\n");
      return -1;
    }
  scamper_file_setreadfunc(decode_sf, decode_rb, scamper_file_readbuf_read);
  return 0;
}

static void ctrlcb(scamper_inst_t *inst, uint8_t type, scamper_task_t *task,
		   const void *data, size_t len)
{
  sc_test_t *test;
  scamper_ping_t *ping = NULL;
  scamper_dealias_t *dealias = NULL;
  uint16_t obj_type;
  void *obj_data;

  gettimeofday_wrap(&now);

  if(type == SCAMPER_CTRL_TYPE_MORE)
    {
      more++;
      do_method();
    }
  else if(type == SCAMPER_CTRL_TYPE_DATA)
    {
      if(scamper_file_readbuf_add(decode_rb, data, len) != 0 ||
	 scamper_file_read(decode_sf, ffilter, &obj_type, &obj_data) != 0)
	{
	  goto err;
	}

      if(obj_data == NULL)
	return;

      if(scamper_file_write_obj(outfile, obj_type, obj_data) != 0)
	{
	  fprintf(stderr, "%s: could not write obj %d\n", __func__, obj_type);
	  goto err;
	}

      if(obj_type == SCAMPER_FILE_OBJ_CYCLE_START ||
	 obj_type == SCAMPER_FILE_OBJ_CYCLE_STOP)
	{
	  scamper_cycle_free(obj_data);
	  return;
	}

      probing--;
      test = scamper_task_param_get(task);

      if(test->type == TEST_PING)
	{
	  assert(obj_type == SCAMPER_FILE_OBJ_PING);
	  ping = (scamper_ping_t *)obj_data;
	  if(process_ping(test, ping) != 0)
	    goto err;
	}
      else if(test->type == TEST_ALLY)
	{
	  assert(obj_type == SCAMPER_FILE_OBJ_DEALIAS);
	  dealias = (scamper_dealias_t *)obj_data;
	  if(process_ally(test, dealias) != 0)
	    goto err;
	}
      else goto err;
    }
  else if(type == SCAMPER_CTRL_TYPE_ERR)
    {
      probing--;
      test = scamper_task_param_get(task);

      if(test->type == TEST_PING)
	{
	  if(test_ping_next(test) != 0)
	    goto err;
	}
      else if(test->type == TEST_ALLY)
	{
	  if(test_ally_next(test, SCAMPER_DEALIAS_RESULT_NONE) != 0)
	    goto err;
	}
      else goto err;
    }
  else if(type == SCAMPER_CTRL_TYPE_EOF)
    {
      scamper_inst_free(scamper_inst);
      scamper_inst = NULL;
    }
  else if(type == SCAMPER_CTRL_TYPE_FATAL)
    {
      print("fatal: %s", scamper_ctrl_strerror(scamper_ctrl));
      goto err;
    }
  return;

 err:
  error = 1;
  if(dealias != NULL) scamper_dealias_free(dealias);
  if(ping != NULL) scamper_ping_free(ping);
  return;
}

/*
 * do_scamperconnect
 *
 * allocate socket and connect to scamper process listening on the port
 * specified.
 */
static int do_scamperconnect(void)
{
  const char *type = "unknown";

  if((scamper_ctrl = scamper_ctrl_alloc(ctrlcb)) == NULL)
    {
      fprintf(stderr, "could not alloc scamper_ctrl\n");
      return -1;
    }

  if(options & OPT_PORT)
    {
      type = "port";
      scamper_inst = scamper_inst_inet(scamper_ctrl, NULL, NULL, port);
    }
#ifdef HAVE_SOCKADDR_UN
  else if(options & OPT_UNIX)
    {
      type = "unix";
      scamper_inst = scamper_inst_unix(scamper_ctrl, NULL, unix_name);
    }
#endif

  if(scamper_inst == NULL)
    {
      fprintf(stderr, "could not alloc %s inst\n", type);
      return -1;
    }

  return 0;
}

static splaytree_t *dump_t = NULL;
static dlist_t     *dump_l = NULL;

static sc_pair_t *pair_find(scamper_addr_t *a, scamper_addr_t *b)
{
  sc_pair_t fm;
  if(scamper_addr_cmp(a, b) < 0)
    {
      fm.a = a;
      fm.b = b;
    }
  else
    {
      fm.a = b;
      fm.b = a;
    }
  return splaytree_find(dump_t, &fm);
}

static int pair_add(scamper_addr_t *a, scamper_addr_t *b)
{
  sc_pair_t *pair;

  if(dump_t == NULL &&
     (dump_t = splaytree_alloc((splaytree_cmp_t)sc_pair_cmp)) == NULL)
    return -1;

  if((pair = malloc(sizeof(sc_pair_t))) == NULL)
    return -1;

  if(scamper_addr_cmp(a, b) < 0)
    {
      pair->a = scamper_addr_use(a);
      pair->b = scamper_addr_use(b);
    }
  else
    {
      pair->a = scamper_addr_use(b);
      pair->b = scamper_addr_use(a);
    }

  if(splaytree_insert(dump_t, pair) == NULL)
    {
      sc_pair_free(pair);
      return -1;
    }

  return 0;
}

static int tc_add(scamper_addr_t *a, scamper_addr_t *b)
{
  sc_addr2router_t *a2r_a, *a2r_b, *a2r_x;
  sc_router_t *r_a, *r_b, *r_x;

  if(dump_t == NULL &&
     ((dump_t = splaytree_alloc((splaytree_cmp_t)sc_addr2router_cmp))== NULL ||
      (dump_l = dlist_alloc()) == NULL))
    return -1;

  a2r_a = sc_addr2router_find(dump_t, a);
  a2r_b = sc_addr2router_find(dump_t, b);

  if(a2r_a != NULL && a2r_b != NULL)
    {
      r_a = a2r_a->router; r_b = a2r_b->router;
      if(r_a == r_b)
	return 0;
      while((a2r_x = slist_head_pop(r_b->addrs)) != NULL)
	{
	  if(slist_tail_push(r_a->addrs, a2r_x) == NULL)
	    goto err;
	  a2r_x->router = r_a;
	}
      dlist_node_pop(dump_l, r_b->node);
      sc_router_free(r_b);
    }
  else if(a2r_a != NULL)
    {
      r_a = a2r_a->router;
      if(sc_addr2router_add(dump_t, r_a, b) == NULL)
	goto err;
    }
  else if(a2r_b != NULL)
    {
      r_b = a2r_b->router;
      if(sc_addr2router_add(dump_t, r_b, a) == NULL)
	goto err;
    }
  else
    {
      if((r_x = sc_router_alloc(dump_l)) == NULL ||
	 sc_addr2router_add(dump_t, r_x, a) == NULL ||
	 sc_addr2router_add(dump_t, r_x, b) == NULL)
	goto err;
    }

  return 0;

 err:
  return -1;
}

static void tc_dump(void)
{
  sc_addr2router_t *a2r;
  slist_node_t *sn;
  dlist_node_t *dn;
  sc_router_t *r;
  char buf[64];
  int x;

  for(dn=dlist_head_node(dump_l); dn != NULL; dn = dlist_node_next(dn))
    {
      r = dlist_node_item(dn);
      slist_qsort(r->addrs, (slist_cmp_t)sc_addr2router_human_cmp);
    }
  dlist_qsort(dump_l, (dlist_cmp_t)sc_router_human_cmp);

  for(dn=dlist_head_node(dump_l); dn != NULL; dn = dlist_node_next(dn))
    {
      r = dlist_node_item(dn);
      x = 0;
      for(sn=slist_head_node(r->addrs); sn != NULL; sn = slist_node_next(sn))
	{
	  a2r = slist_node_item(sn);
	  if(x > 0) printf(" ");
	  printf("%s", scamper_addr_tostr(a2r->addr, buf, sizeof(buf)));
	  x++;
	}
      printf("\n");
    }

  return;
}

static int dump_process_ally(const scamper_dealias_t *dealias)
{
  const scamper_dealias_ally_t *ally = scamper_dealias_ally_get(dealias);
  const scamper_dealias_probedef_t *def;
  scamper_addr_t *a, *b;
  char ab[64], bb[64];

  if(ally == NULL ||
     scamper_dealias_result_get(dealias) != SCAMPER_DEALIAS_RESULT_ALIASES)
    return 0;

  def = scamper_dealias_ally_def0_get(ally);
  a = scamper_dealias_probedef_dst_get(def);
  def = scamper_dealias_ally_def1_get(ally);
  b = scamper_dealias_probedef_dst_get(def);

  if((flags & FLAG_TC) == 0)
    {
      if(pair_find(a, b) == NULL)
	{
	  if(pair_add(a, b) != 0)
	    return -1;
	  printf("%s %s\n",
		 scamper_addr_tostr(a, ab, sizeof(ab)),
		 scamper_addr_tostr(b, bb, sizeof(bb)));
	}
      return 0;
    }

  return tc_add(a, b);
}

static int dump_process_ping(const scamper_ping_t *ping)
{
  scamper_addr_t *dst, *r_addr;
  const scamper_ping_probe_t *p;
  const scamper_ping_reply_t *r;
  char a[64], b[64];
  uint16_t i, j, ping_sent;

  if(scamper_ping_method_is_udp(ping) == 0)
    return 0;

  ping_sent = scamper_ping_sent_get(ping);
  dst = scamper_ping_dst_get(ping);
  for(i=0; i<ping_sent; i++)
    {
      if((p = scamper_ping_probe_get(ping, i)) == NULL)
	continue;
      for(j=0; j<scamper_ping_probe_replyc_get(p); j++)
	{
	  r = scamper_ping_probe_reply_get(p, j);
	  r_addr = scamper_ping_reply_addr_get(r);
	  if(scamper_ping_reply_is_icmp_unreach_port(r) == 0 &&
	     scamper_addr_cmp(r_addr, dst) != 0)
	    {
	      if((flags & FLAG_TC) == 0)
		{
		  if(pair_find(r_addr, dst) == NULL)
		    {
		      if(pair_add(r_addr, dst) != 0)
			return -1;
		      printf("%s %s\n",
			     scamper_addr_tostr(r_addr, a, sizeof(a)),
			     scamper_addr_tostr(dst, b, sizeof(b)));
		    }
		}
	      else
		{
		  if(tc_add(r_addr, dst) != 0)
		    return -1;
		}
	    }
	}
    }

  return 0;
}

static void dump_finish(void)
{
  if((flags & FLAG_TC) == 0)
    {
      splaytree_free(dump_t, (splaytree_free_t)sc_pair_free);
    }
  else
    {
      tc_dump();
      splaytree_free(dump_t, (splaytree_free_t)sc_addr2router_free);
      dlist_free_cb(dump_l, (dlist_free_t)sc_router_free);
    }
  return;
}

static int process_1_ally(const scamper_dealias_t *dealias)
{
  return dump_process_ally(dealias);
}

static void finish_1(void)
{
  dump_finish();
  return;
}

static int process_2_ping(const scamper_ping_t *ping)
{
  return dump_process_ping(ping);
}

static void finish_2(void)
{
  dump_finish();
  return;
}

static int process_3_ally(const scamper_dealias_t *dealias)
{
  return dump_process_ally(dealias);
}

static int process_3_ping(const scamper_ping_t *ping)
{
  return dump_process_ping(ping);
}

static void finish_3(void)
{
  dump_finish();
  return;
}

static void cleanup(void)
{
  if(virgin != NULL)
    {
      slist_free(virgin);
      virgin = NULL;
    }

  if(waiting != NULL)
    {
      heap_free(waiting, NULL);
      waiting = NULL;
    }

  if(targets != NULL)
    {
      splaytree_free(targets, NULL);
      targets = NULL;
    }

  if(ipidseqs != NULL)
    {
      splaytree_free(ipidseqs, (splaytree_free_t)sc_ipidseq_free);
      ipidseqs = NULL;
    }

  if(ffilter != NULL)
    {
      scamper_file_filter_free(ffilter);
      ffilter = NULL;
    }

  if(decode_rb != NULL)
    {
      scamper_file_readbuf_free(decode_rb);
      decode_rb = NULL;
    }

  if(decode_sf != NULL)
    {
      scamper_file_close(decode_sf);
      decode_sf = NULL;
    }

  if(scamper_inst != NULL)
    {
      scamper_inst_free(scamper_inst);
      scamper_inst = NULL;
    }

  if(scamper_ctrl != NULL)
    {
      scamper_ctrl_free(scamper_ctrl);
      scamper_ctrl = NULL;
    }

  if(text != NULL)
    {
      fclose(text);
      text = NULL;
    }

  if(outfile != NULL)
    {
      scamper_file_close(outfile);
      outfile = NULL;
    }

  return;
}

static int ally_init(void)
{
  uint16_t types[4];
  int typec = 0;

  types[typec++] = SCAMPER_FILE_OBJ_PING;
  types[typec++] = SCAMPER_FILE_OBJ_DEALIAS;
  if(options & OPT_OUTFILE)
    {
      types[typec++] = SCAMPER_FILE_OBJ_CYCLE_START;
      types[typec++] = SCAMPER_FILE_OBJ_CYCLE_STOP;
    }
  if((ffilter = scamper_file_filter_alloc(types, typec)) == NULL)
    return -1;

  return 0;
}

static int ally_read(void)
{
  scamper_file_t *in;
  char *filename;
  uint16_t type;
  void *data;
  int i, stdin_used=0;

  for(i=0; i<dump_filec; i++)
    {
      filename = dump_files[i];

      if(string_isdash(filename) != 0)
	{
	  if(stdin_used == 1)
	    {
	      fprintf(stderr, "stdin already used\n");
	      return -1;
	    }
	  stdin_used++;
	  in = scamper_file_openfd(STDIN_FILENO, "-", 'r', "warts");
	}
      else
	{
	  in = scamper_file_open(filename, 'r', NULL);
	}

      if(in == NULL)
	{
	  fprintf(stderr,"could not open %s: %s\n", filename, strerror(errno));
	  return -1;
	}

      while(scamper_file_read(in, ffilter, &type, &data) == 0)
	{
	  /* EOF */
	  if(data == NULL)
	    break;

	  if(type == SCAMPER_FILE_OBJ_PING)
	    {
	      if(dump_funcs[dump_id].proc_ping != NULL)
		dump_funcs[dump_id].proc_ping(data);
	      scamper_ping_free(data);
	    }
	  else if(type == SCAMPER_FILE_OBJ_DEALIAS)
	    {
	      if(dump_funcs[dump_id].proc_ally != NULL)
		dump_funcs[dump_id].proc_ally(data);
	      scamper_dealias_free(data);
	    }
	}

      scamper_file_close(in);
    }

  if(dump_funcs[dump_id].finish != NULL)
    dump_funcs[dump_id].finish();

  return 0;
}

static int ally_data(void)
{
  struct timeval tv, *tv_ptr;
  sc_waittest_t *wait;
  int done = 0;

#ifdef HAVE_DAEMON
  /* start a daemon if asked to */
  if((options & OPT_DAEMON) != 0 && daemon(1, 0) != 0)
    {
      fprintf(stderr, "could not daemon");
      return -1;
    }
#endif

  random_seed();

  if((targets = splaytree_alloc((splaytree_cmp_t)sc_target_cmp)) == NULL)
    return -1;
  if((ipidseqs = splaytree_alloc((splaytree_cmp_t)sc_ipidseq_cmp)) == NULL)
    return -1;
  if((virgin = slist_alloc()) == NULL)
    return -1;
  if((waiting = heap_alloc((heap_cmp_t)sc_waittest_cmp)) == NULL)
    return -1;
  if(file_lines(addressfile, addressfile_line, NULL) != 0)
    {
      fprintf(stderr, "could not read %s\n", addressfile);
      return -1;
    }

  /*
   * connect to the scamper process
   */
  if(do_scamperconnect() != 0)
    return -1;

  /*
   * sort out the files that we'll be working with.
   */
  if(do_files() != 0)
    return -1;

  while(scamper_ctrl_isdone(scamper_ctrl) == 0)
    {
      /*
       * need to set a timeout on select if scamper's processing window is
       * not full and there is something in the waiting queue.
       */
      tv_ptr = NULL;
      gettimeofday_wrap(&now);
      if(more > 0)
	{
	  /*
	   * if there is something ready to probe now, then try and
	   * do it.
	   */
	  wait = heap_head_item(waiting);
	  if(slist_count(virgin) > 0 ||
	     (wait != NULL && timeval_cmp(&wait->tv, &now) <= 0))
	    {
	      if(do_method() != 0)
		return -1;
	    }

	  /*
	   * if we could not send a new command just yet, but scamper
	   * wants one, then wait for an appropriate length of time.
	   */
	  wait = heap_head_item(waiting);
	  if(more > 0 && wait != NULL)
	    {
	      tv_ptr = &tv;
	      if(timeval_cmp(&wait->tv, &now) > 0)
		timeval_diff_tv(&tv, &now, &wait->tv);
	      else
		memset(&tv, 0, sizeof(tv));
	    }
	}

      if(splaytree_count(targets) == 0 && slist_count(virgin) == 0 &&
	 heap_count(waiting) == 0 && done == 0)
	{
	  scamper_inst_done(scamper_inst);
	  done = 1;
	}

      scamper_ctrl_wait(scamper_ctrl, tv_ptr);

      if(error != 0)
	return -1;
    }

  return 0;
}

int main(int argc, char *argv[])
{
#if defined(DMALLOC)
  free(malloc(1));
#endif

  atexit(cleanup);

  if(check_options(argc, argv) != 0)
    return -1;

  if(ally_init() != 0)
    return -1;

  if(options & OPT_DUMP)
    return ally_read();

  return ally_data();
}
