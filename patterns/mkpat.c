/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *\
 * This is GNU GO, a Go program. Contact gnugo@gnu.org, or see   *
 * http://www.gnu.org/software/gnugo/ for more information.      *
 *                                                               *
 * Copyright 1999, 2000, 2001 by the Free Software Foundation.   *
 *                                                               *
 * This program is free software; you can redistribute it and/or *
 * modify it under the terms of the GNU General Public License   *
 * as published by the Free Software Foundation - version 2.     *
 *                                                               *
 * This program is distributed in the hope that it will be       *
 * useful, but WITHOUT ANY WARRANTY; without even the implied    *
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR       *
 * PURPOSE.  See the GNU General Public License in file COPYING  *
 * for more details.                                             *
 *                                                               *
 * You should have received a copy of the GNU General Public     *
 * License along with this program; if not, write to the Free    *
 * Software Foundation, Inc., 59 Temple Place - Suite 330,       *
 * Boston, MA 02111, USA.                                        *
\* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* Compile one of the pattern databases. This takes a database file,
 * e.g. patterns.db, and produces a C code file, in this case
 * patterns.c.
 */

/* See also patterns.h, and the *.db files.
 */


/* Differences when compiling connections patterns (-c) :
 *  '*' means cutting point
 *  '!' is allowed (inhibit connection there), matches as '.'.
 *  '!' will always be written as the first elements
*/

/* As in the rest of GNU Go, co-ordinate convention (i,j) is 'i' down from
 * the top, then 'j' across from the left
 */

#define MAX_BOARD 19
#define USAGE "\
Usage : mkpat [-cvh] <prefix>\n\
 options : -v = verbose\n\
           -c = compile connections database (default is pattern database)\n\
           -b = allow both colors to be anchor (default is only O)\n\
           -X = allow only X to be anchor (default is only O)\n\
           -f = compile a fullboard pattern database\n\
           -m = try to place the anchor in the center of the pattern\n\
                (reduce dfa size)\n\
\n\
 If compiled with --enable-dfa the following options also work:\n\n\
           -D = generate a dfa and save it as a C file.\n\
           -V <level>  = dfa verbose level\n\
"


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "patterns.h"
#include "gg-getopt.h"

#if DFA_ENABLED
#include "dfa.h"
#endif

#define PATTERNS    0
#define CONNECTIONS 1

/* code assumes that ATT_O and ATT_X are 1 and 2 (in either order)
 * An attribute is a candidate for anchor if  (att & anchor) != 0
 */
#define ANCHOR_O    ATT_O
#define ANCHOR_X    ATT_X
#define ANCHOR_BOTH 3

#define MAXLINE 500
#define MAXCONSTRAINT 10000
#define MAXACTION 10000
#define MAXPATNO 5000
#define MAXLABELS 20
#define MAXPARAMS 15

#define UNUSED(x)  x=x


/* valid characters that can appear in a pattern
 * position in string is att value to store
 */
const char VALID_PATTERN_CHARS[]     = ".XOxo,a!*?Q";
const char VALID_EDGE_CHARS[]        = "+-|";
const char VALID_CONSTRAINT_LABELS[] = "abcdefghijklmnpqrstuvwyzABCDEFGHIJKLMNPQRSTUVWYZ";


/* the offsets into the list are the ATT_* defined in patterns.h
 * The following defns are for internal use only, and are not
 * written out to the compiled pattern database
 */

#define ATT_star  8
#define ATT_wild  9
#define ATT_Q    10


/* stuff used in reading/parsing pattern rows */
int maxi, maxj;                 /* (i,j) offsets of largest element */
int mini, minj;                 /* offset of top-left element
				   (0,0) unless there are edge constraints */
int where;                      /* NORTH_EDGE | WEST_EDGE, etc */
int el;                         /* next element number in current pattern */
struct patval elements[MAX_BOARD*MAX_BOARD]; /* elements of current pattern */
int num_stars;

int ci=-1,cj=-1;                /* position of origin (first piece element)
				   relative to top-left */
int patno;		        /* current pattern */
int pats_with_constraints = 0;  /* just out of interest */
int label_coords[256][2];       /* Coordinates for labeled stones in the 
				   autohelper patterns. */
int current_i;		        /* Counter for the line number of a 
				   constraint diagram. */
char constraint[MAXCONSTRAINT]; /* Store constraint lines. */
char action[MAXCONSTRAINT];     /* Store action lines. */

/* stuff to maintain info about patterns while reading */
struct pattern pattern[MAXPATNO];  /* accumulate the patterns into here */
char pattern_names[MAXPATNO][80];  /* with optional names here, */
char helper_fn_names[MAXPATNO][80]; /* helper fn names here */
char autohelper_code[MAXPATNO*300]; /* code for automatically generated */
                                    /* helper functions here */
char *code_pos;                     /* current position in code buffer */
struct autohelper_func {
  const char *name;
  int params;
  const char *code;
};

/* ================================================================ */
/*                                                                  */
/*                Autohelper function definitions                   */
/*                                                                  */
/* ================================================================ */

/* Important notice:
 * If one function has a name which is a prefix of another name, the
 * shorter name must come later in the list. E.g. "lib" must be preceded
 * by "lib2", "lib3", and "lib4".
 */
static struct autohelper_func autohelper_functions[] = {
  {"lib2",            1, "worm[%ci][%cj].liberties2"},
  {"lib3",            1, "worm[%ci][%cj].liberties3"},
  {"lib4",            1, "worm[%ci][%cj].liberties4"},
  {"lib",             1, "countlib2(%ci,%cj)"},
  {"alive",           1, "(dragon[%ci][%cj].matcher_status == ALIVE)"},
  {"unknown",         1, "(dragon[%ci][%cj].matcher_status == UNKNOWN)"},
  {"critical",        1, "(dragon[%ci][%cj].matcher_status == CRITICAL)"},
  {"dead",            1, "(dragon[%ci][%cj].matcher_status == DEAD)"},
  {"status",          1, "dragon[%ci][%cj].matcher_status"},
  {"ko",              1, "is_ko_point2(%ci,%cj)"},
  {"xdefend_against", 2, "defend_against(%ci,%cj,OTHER_COLOR(color),%ci,%cj)"},
  {"odefend_against", 2, "defend_against(%ci,%cj,color,%ci,%cj)"},
  {"defend_against_atari",1,"defend_against_atari_helper(ti,tj,%ci,%cj)"},
  {"does_defend",     2, "does_defend(%ci,%cj,%ci,%cj)"},
  {"does_attack",     2, "does_attack(%ci,%cj,%ci,%cj)"},
  {"attack",          1, "ATTACK_MACRO(%ci,%cj)"},
  {"defend",          1, "DEFEND_MACRO(%ci,%cj)"},
  {"weak",            1, "DRAGON_WEAK(%ci,%cj)"},
  {"safe_xmove",      1, "safe_move2(%ci,%cj,OTHER_COLOR(color))"},
  {"safe_omove",      1, "safe_move2(%ci,%cj,color)"},
  {"legal_xmove",     1, "is_legal2(%ci,%cj,other)"},
  {"legal_omove",     1, "is_legal2(%ci,%cj,color)"},
  {"x_somewhere",     -1, "somewhere(OTHER_COLOR(color), %d"},
  {"o_somewhere",     -1, "somewhere(color, %d"},
  {"xmoyo",           1, "(influence_moyo_color(%ci,%cj) == OTHER_COLOR(color))"},
  {"omoyo",           1, "(influence_moyo_color(%ci,%cj) == color)"},
  {"xarea",           1, "(influence_area_color(%ci,%cj) == OTHER_COLOR(color))"},
  {"oarea",           1, "(influence_area_color(%ci,%cj) == color)"},
  {"xterri",          1, "(influence_territory_color(%ci,%cj) == OTHER_COLOR(color))"},
  {"oterri",          1, "(influence_territory_color(%ci,%cj) == color)"},
  {"genus",           1, "dragon[%ci][%cj].genus"},
  {"xlib",            1, "accurate_approxlib(POS(%ci,%cj),OTHER_COLOR(color), MAXLIBS, NULL)"},
  {"olib",            1, "accurate_approxlib(POS(%ci,%cj),color, MAXLIBS, NULL)"},
  {"xcut",            1, "cut_possible(%ci,%cj,OTHER_COLOR(color))"},
  {"ocut",            1, "cut_possible(%ci,%cj,color)"},
  {"xplay_defend_both",   -2,
   "play_attack_defend2_n(OTHER_COLOR(color), 0, %d"},
  {"oplay_defend_both",   -2, "play_attack_defend2_n(color, 0, %d"},
  {"xplay_attack_either", -2,
   "play_attack_defend2_n(OTHER_COLOR(color), 1, %d"},
  {"oplay_attack_either", -2, "play_attack_defend2_n(color, 1, %d"},
  {"xplay_defend",   -1, "play_attack_defend_n(OTHER_COLOR(color), 0, %d"},
  {"oplay_defend",   -1, "play_attack_defend_n(color, 0, %d"},
  {"xplay_attack",   -1, "play_attack_defend_n(OTHER_COLOR(color), 1, %d"},
  {"oplay_attack",   -1, "play_attack_defend_n(color, 1, %d"},
  {"xplay_break_through", -3, "play_break_through_n(OTHER_COLOR(color), %d"},
  {"oplay_break_through", -3, "play_break_through_n(color, %d"},
  {"seki_helper",     1, "seki_helper(%ci,%cj)"},
  {"threaten_to_save",1,"threaten_to_save_helper(ti,tj,%ci,%cj)"},
  {"threaten_to_capture",1,"threaten_to_capture_helper(ti,tj,%ci,%cj)"},
  {"not_lunch",       2, "not_lunch_helper(%ci,%cj,%ci,%cj)"},
  {"area_stone",      1, "area_stone(%ci,%cj)"},
  {"area_space",      1, "area_space(%ci,%cj)"},
  {"eye",             1, "eye_space(%ci,%cj)"},
  {"proper_eye",      1, "proper_eye_space(%ci,%cj)"},
  {"marginal_eye",    1, "marginal_eye_space(%ci,%cj)"},
  {"halfeye",         1, "is_halfeye(half_eye,POS(%ci,%cj))"},
  {"max_eye_value",   1, "max_eye_value(%ci,%cj)"},
  {"owl_topological_eye", 2, "owl_topological_eye(%ci,%cj,BOARD(%ci,%cj))"},
  {"obvious_false_oeye", 1, "obvious_false_eye(%ci,%cj,color)"},
  {"obvious_false_xeye", 1, "obvious_false_eye(%ci,%cj,OTHER_COLOR(color))"},
  {"antisuji",        1, "add_antisuji_move(%ci,%cj)"},
  {"add_connect_move",2, "add_connection_move(ti,tj,%ci,%cj,%ci,%cj)"},
  {"add_cut_move",    2, "add_cut_move(ti,tj,%ci,%cj,%ci,%cj)"},
  {"add_attack_either_move",2,"add_attack_either_move(ti,tj,%ci,%cj,%ci,%cj)"},
  {"add_defend_both_move",2, "add_defend_both_move(ti,tj,%ci,%cj,%ci,%cj)"},
  {"remove_attack",   2, "remove_attack_move(%ci,%cj,%ci,%cj)"},
  {"same_dragon",     2, "same_dragon(%ci,%cj,%ci,%cj)"},
  {"same_worm",       2, "same_worm(%ci,%cj,%ci,%cj)"},
  {"dragonsize",      1, "dragon[%ci][%cj].size"},
  {"wormsize",        1, "countstones2(%ci,%cj)"},
  {"effective_size",  1, "dragon[%ci][%cj].effective_size"},
  {"vital_chain",     1, "vital_chain(%ci,%cj)"},
  {"potential_cutstone",1,"worm[%ci][%cj].cutstone2>1"},
  {"amalgamate_most_valuable_helper",3,
   "amalgamate_most_valuable_helper(%ci,%cj,%ci,%cj,%ci,%cj)"},
  {"amalgamate",      2, "join_dragons(%ci,%cj,%ci,%cj)"},
  {"make_proper_eye", 1, "make_proper_eye_space(%ci,%cj,color)"},
  {"remove_halfeye",  1, "remove_half_eye(%ci,%cj,color)"},
  {"remove_eyepoint", 1, "remove_eyepoint(%ci,%cj,color)"},
  {"owl_escape_value",1, "owl_escape_value(%ci,%cj)"},
  {"owl_goal_dragon", 1, "owl_goal_dragon(%ci,%cj)"},
  {"owl_eyespace",    2, "owl_eyespace(%ci,%cj,%ci,%cj)"},
  {"owl_big_eyespace",2, "owl_big_eyespace(%ci,%cj,%ci,%cj)"},
  {"has_aji",1,"(dragon[%ci][%cj].owl_threat_status==CAN_THREATEN_DEFENSE)"},
  {"finish_ko_helper",1, "finish_ko_helper(%ci,%cj)"},
  {"squeeze_ko_helper",1,"squeeze_ko_helper(%ci,%cj)"},
  {"backfill_helper", 3, "backfill_helper(%ci,%cj, %ci, %cj, %ci, %cj)"},
  {"owl_threatens",   2, "owl_threatens_attack(%ci,%cj,%ci,%cj)"},
  {"o_aa_attack",     2, "atari_atari_try_combination(color,POS(%ci,%cj),POS(%ci,%cj))"},
  {"x_aa_attack",     2, "atari_atari_try_combination(OTHER_COLOR(color),POS(%ci,%cj),POS(%ci,%cj))"},
  {"replace",         2, "add_replacement_move(%ci,%cj,%ci,%cj)"}
};


/* To get a valid function pointer different from NULL. */
static int
dummyhelper (struct pattern *patt, int transformation,
	     int ti, int tj, int color, int action)
{
  UNUSED(patt); UNUSED(transformation); UNUSED(ti); UNUSED(tj); UNUSED(color);
  UNUSED(action);
  return 0;
}


#define PREAMBLE "\
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *\n\
 * This is GNU GO, a Go program. Contact gnugo@gnu.org, or see   *\n\
 * http://www.gnu.org/software/gnugo/ for more information.      *\n\
 *                                                               *\n\
 * Copyright 1999, 2000, 2001 by the Free Software Foundation.   *\n\
 *                                                               *\n\
 * This program is free software; you can redistribute it and/or *\n\
 * modify it under the terms of the GNU General Public License   *\n\
 * as published by the Free Software Foundation - version 2.     *\n\
 *                                                               *\n\
 * This program is distributed in the hope that it will be       *\n\
 * useful, but WITHOUT ANY WARRANTY; without even the implied    *\n\
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR       *\n\
 * PURPOSE.  See the GNU General Public License in file COPYING  *\n\
 * for more details.                                             *\n\
 *                                                               *\n\
 * You should have received a copy of the GNU General Public     *\n\
 * License along with this program; if not, write to the Free    *\n\
 * Software Foundation, Inc., 59 Temple Place - Suite 330,       *\n\
 * Boston, MA 02111, USA.                                        *\n\
 *                                                               *\n\
 * This file is automatically generated by mkpat. Do not edit    *\n\
 * it directly. Instead, edit the corresponding database.        *\n\
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */\n\n\
#include <stdio.h> /* for NULL */\n\
#include \"liberty.h\"\n\
#include \"patterns.h\"\n\n\
"

int fatal_errors = 0;

/* options */
int verbose = 0;  /* -v */
int pattern_type = PATTERNS;  /* -c for CONNECTIONS */
int anchor = ANCHOR_O; /* Whether both O and/or X may be anchors.
			* -b for both. -X for only X.
			*/

int choose_best_anchor = 0;  /* -m */

int fullboard = 0;   /* Whether this is a database of fullboard patterns. */
#if DFA_ENABLED
int dfa_generate = 0; /* if 1 a dfa is created. */
int dfa_c_output = 0; /* if 1 the dfa is saved as a c file */
dfa_t dfa;
#endif


/**************************
 *
 * stuff to parse the input
 *
 **************************/

/* reset state of all pattern variables */
static void
reset_pattern(void)
{
  int i;

  maxi = 0;
  maxj = 0;
  ci = -1;
  cj = -1;
  where = 0;
  el = 0;
  num_stars = 0;
  strcpy(helper_fn_names[patno], "NULL");
  for (i = 0; i < 256; i++)
    label_coords[i][0] = -1;
  current_i = 0;
  constraint[0] = 0;
  action[0] = 0;
}
  


/* this is called to compute the extents of the pattern, applying
 * edge constraints as necessary
 */

static void
find_extents(void)
{

  /* When this is called, elements go from (mini,minj) inclusive to
   * (maxi-1, maxj-1) [ie exclusive]. Make them inclusive.
   * ie maximum element lies on (maxi,maxj)
   */

  --maxi;
  --maxj;

  /* apply edge constraints to the size of the pattern */

  if (where & (NORTH_EDGE|SOUTH_EDGE|EAST_EDGE|WEST_EDGE))
    ++pats_with_constraints;

  if (verbose)
    fprintf(stderr, "Pattern %s has constraints 0x%x\n",
	    pattern_names[patno], where);

  pattern[patno].edge_constraints = where;


  /* At this point, (mini,minj) -> (maxi,maxj) contain
   * the extent of the pattern, relative to top-left
   * of pattern, rather than (ci,cj).
   *
   * But we store them in the output file relative
   * to (ci,cj), so that we can transform the corners
   * of the pattern like any other relative co-ord.
   */

  pattern[patno].mini = mini - ci;
  pattern[patno].minj = minj - cj;
  pattern[patno].maxi = maxi - ci;
  pattern[patno].maxj = maxj - cj;
}

#if DFA_ENABLED

/*
 * Here we build the dfa.
 */

static void
write_to_dfa(int index)
{
  char str[MAX_ORDER+1];
  float ratio;
  
  assert(ci != -1 && cj != -1);
  pattern[index].patn = elements; /* a little modification : keep in touch! */
  pattern[index].name = &(pattern_names[index][0]); 

  if (verbose)
    fprintf(stderr, "Add   :%s\n", pattern[index].name);

  /* First we create the string from the actual pattern */
  pattern_2_string(pattern+index, str, 0, ci, cj);
      
  /* Then We add this string to the DFA */
  ratio = (dfa_add_string(&dfa, str, index) - 1)*100;
 
  /* Complain when there is more than 10% of increase */ 
  if (dfa_size(&dfa) > 100 && ratio > 10.0) {
    fprintf(stderr, "Pattern %s => %3.1f%% increase: ",
	    pattern[index].name,ratio);
    fprintf(stderr, "another orientation may save memory.\n");
  }
  if (dfa_verbose)
    dump_dfa(stderr, &dfa);
}


#endif

/* For good performance, we want to reject patterns as quickly as
 * possible. For each pattern, this combines 16 positions around
 * the anchor stone into a 32-bit mask and value. In the matcher,
 * the same 4x4 grid is precomputed, and then we can quickly
 * test 16 board positions with one test.
 * See matchpat.c for details of how this works - basically, if
 * we AND what is on the board with the and_mask, and get the
 * value in the val_mask, we have a match. This test can be
 * applied in parallel : 2 bits per posn x 16 posns = 32 bits.
 * "Don't care" has and_mask = val_mask = 0, which is handy !
 */

static void
compute_grids(void)
{
#if GRID_OPT
  /*                       element : .  X  O  x  o  ,  a  ! */
  static const uint32 and_mask[] = { 3, 3, 3, 1, 2, 3, 3, 1 };
  static const uint32 val_mask[] = { 0, 2, 1, 0, 0, 0, 0, 0 };

  int ll;  /* iterate over rotations */
  int k;   /* iterate over elements */

  for (ll = 0; ll < 8; ++ll) {
    for (k = 0; k < el; ++k) {
      int di, dj;

      TRANSFORM(elements[k].x - ci, elements[k].y - cj, &di, &dj, ll);
      ++di;
      ++dj;
      if (di >= 0 && di < 4 && dj >= 0 && dj < 4) {
	pattern[patno].and_mask[ll]
	  |= and_mask[elements[k].att] << (30 - di * 8 - dj * 2);
	pattern[patno].val_mask[ll]
	  |= val_mask[elements[k].att] << (30 - di * 8 - dj * 2);
      }
    }
  }
#endif
}



/* We've just read a line that looks like a pattern line.
 * Now process it.
 */

static void
read_pattern_line(char *p)
{
  const char *char_offset;
  int j;
  int width;

  if (where & SOUTH_EDGE)
    /* something wrong here : pattern line after a SOUTH_EDGE constraint */
    goto fatal;


  if (*p == '+' || *p == '-') {
    /* must be a north/south constraint */

    if (maxi == 0)
      where |= NORTH_EDGE;
    else
      where |= SOUTH_EDGE;

    if (*p == '+') {
      if (maxi > 0 && !(where & WEST_EDGE))
	/* if this is end of pattern, must already know about west */
	goto fatal;

      where |= WEST_EDGE;
      ++p;
    }

    /* count the number of -'s */
    for (width = 0; *p == '-' ; ++p, ++width)
      ;

    if (width == 0)
      goto fatal;

    if (*p == '+') {
      if (maxi > 0 && !(where & EAST_EDGE))
	/* if this is end of pattern, must already know about west */
	goto fatal;
      where |= EAST_EDGE;
    }

    if (maxi > 0 && width != maxj)
      goto notrectangle;

    return;
  }

  /* get here => its a real pattern entry, 
   * rather than a north/south constraint 
   */

  /* we have a pattern line - add it into the current pattern */
  if (*p == '|') {
    /* if this is not the first line, or if there is a north
     * constraint, we should already know about it
     */
    if (!(where & WEST_EDGE) && ((where & NORTH_EDGE) || maxi > 0))
      /* we should already know about this constraint */
      goto fatal;

    where |= WEST_EDGE;
    ++p;
  }
  else if (where & WEST_EDGE)
    /* need a | if we are already constrained to west */
    goto fatal;


  for (j = 0; 
       (char_offset = strchr(VALID_PATTERN_CHARS, *p)) != NULL;
       ++j, ++p) {

    /* char_offset is a pointer within the VALID_PATTERN_CHARS string.
     * so  (char-VALID_PATTERN_CHARS) is the att (0 to 10) to write to the
     * pattern element
     */

    /* one of ATT_* - see above */
    int off = char_offset - VALID_PATTERN_CHARS;

    if (off == ATT_wild)
      continue;  /* boring - pad character */

    if (off == ATT_a) /* this were used by halfeye patterns */
      goto fatal;

    if (off == ATT_star) {
      /* '*' */
      pattern[patno].movei = maxi;
      pattern[patno].movej = j;
      ++num_stars;
      off = ATT_dot;  /* add a '.' to the pattern instead */
    }

    if (off == ATT_Q) {
      /* 'Q' */
      pattern[patno].movei = maxi;
      pattern[patno].movej = j;
      ++num_stars;
    }

    assert(off <= ATT_not || off == ATT_Q);

	
    if ( (ci == -1) && (off < 3) && ((off & anchor) != 0) ) {
      /* Use this position as the pattern origin. */
      ci = maxi;
      cj = j;
      pattern[patno].anchored_at_X = (off == ATT_X) ? 3 : 0;
    }

    if (off == ATT_Q)
      off = ATT_O;  /* add an 'O' to the pattern instead */

    /* Special limitations for fullboard pattern. */
    if (fullboard) {
      if (off == ATT_dot)
	continue;
      assert(off == ATT_X || off == ATT_O);
    }
    
    /* Range checking. */
    assert(el < (int) (sizeof(elements) / sizeof(elements[0])));
    
    elements[el].x = maxi;
    elements[el].y = j;
    elements[el].att = off;  /* '*' mapped to '.' and 'Q' to 'O' above */
    ++el;
  }

  if (*p == '|') {

    /* if this is not the first line, or if there is a north
     * constraint, we should already know about it
     */
    if (!(where & EAST_EDGE) && ((where & NORTH_EDGE) || maxi > 0))
      goto fatal;  /* we should already know about this constraint */

    where |= EAST_EDGE;

  }
  else if (where & EAST_EDGE)
    goto fatal;  /* need a | if we are already constrained to east */


  if (maxi > 0 && j != maxj)
    goto notrectangle;

  if (j > maxj)
    maxj = j;

  maxi++;

  return;

fatal:
 fprintf(stderr, "Illegal pattern %s\n", pattern_names[patno]);
 fatal_errors=1;
 return;

notrectangle:
 fprintf(stderr, "Warning pattern %s not rectangular\n", pattern_names[patno]);
 return;
}


/*
 * We've just read a line that looks like a constraint pattern line.
 * Now process it.
 */

static void
read_constraint_diagram_line(char *p)
{
  int j;

  /* North or south boundary, no letter to be found. */
  if (*p == '+' || *p == '-')
    return;

  /* Skip west boundary. */
  if (*p == '|')
    p++;
  
  for (j = 0; 
       strchr(VALID_PATTERN_CHARS, *p) 
	 || strchr(VALID_CONSTRAINT_LABELS, *p);
       ++j, ++p) {
    if (strchr(VALID_CONSTRAINT_LABELS, *p) 
	&& label_coords[(int)*p][0] == -1) {

      /* New constraint letter */
      label_coords[(int)*p][0] = current_i;
      label_coords[(int)*p][1] = j;
    }
  }

  current_i++;

  return;
}



/* On reading a line starting ':', finish up and write
 * out the current pattern 
 */

static void
finish_pattern(char *line)
{

  /* end of pattern layout */
  char symmetry;		/* the symmetry character */
  
  mini = minj = 0; /* initially : can change with edge-constraints */

  if (num_stars > 1 || (pattern_type == PATTERNS && num_stars == 0)) {
    fprintf(stderr, "No or too many *'s in pattern %s\n",
	    pattern_names[patno]);
    fatal_errors = 1;
  }

  if (fullboard) {
    /* For fullboard patterns, the "anchor" is always at the mid point. */
    ci = (maxi-1)/2;
    cj = (maxj-1)/2;
  }
  else if (choose_best_anchor) { 

    /* try to find a better anchor if
     * the -m option is set */
      float mi,mj; /* middle */
      float d, min_d = 361.0;
      int k, min_k = -1;
      
      /* we seek the element of suitable value minimizing
       * the distance to the middle */
      mi = ((float)maxi - 1.0) / 2.0;
      mj = ((float)maxj - 1.0) / 2.0 - 0.01;
      for (k = 0; k != el; k++)
	if (elements[k].att < 3 && (elements[k].att & anchor) != 0) {
	  d = gg_abs((float)elements[k].x - mi)
	    + gg_abs((float)elements[k].y - mj);
	  if (d < min_d) {
	    min_k = k;
	    min_d = d;
	  }
	}
      assert(min_k != -1);
      ci = elements[min_k].x;
      cj = elements[min_k].y;
      pattern[patno].anchored_at_X = (elements[min_k].att == ATT_X) ? 3 : 0;

    }
    else if (ci == -1 || cj == -1) {
      fprintf(stderr, "No origin for pattern %s\n", pattern_names[patno]);
      fatal_errors = 1;
      ci = 0;
      cj = 0;
    }

  /* translate posn of * (or Q) to relative co-ords
   */

  if (num_stars == 1) {
    pattern[patno].movei -= ci;
    pattern[patno].movej -= cj;
  }
  else if (num_stars == 0) {
    pattern[patno].movei = -1;
    pattern[patno].movej = -1;
  }

  find_extents();

  compute_grids();

  pattern[patno].patlen = el;

  /* Now parse the line. Only the symmetry character and the class
   * field are mandatory. The compiler guarantees that all the fields
   * are already initialised to 0. */

  {
    int s;
    char class[80];
    char entry[80];
    char *p = line;
    int n;
    float v = 0.0;
    
    class[0] = 0;  /* in case sscanf doesn't get that far */
    s = sscanf(p, ":%c,%[^,]%n", &symmetry, class, &n);
    p += n;
    
    if (s < 2) {
      fprintf(stderr, ": line must contain symmetry character and class\n");
      fatal_errors++;
    }

    while (sscanf(p, "%*[, ]%[^,]%n", entry, &n) > 0) {
      p += n;

      if (sscanf(entry, "%*[^(](%f)", &v) > 0) {
	if (strncmp(entry, "value", 5) == 0
	    || strncmp(entry, "minvalue", 8) == 0) {
	  pattern[patno].value = v;
	  pattern[patno].class |= VALUE_MINVAL;
	}
	else if (strncmp(entry, "maxvalue", 8) == 0) {
	  pattern[patno].maxvalue = v;
	  pattern[patno].class |= VALUE_MAXVAL;
	}
	else if (strncmp(entry, "terri", 5) == 0
		 || strncmp(entry, "minterri", 8) == 0) {
	  pattern[patno].minterritory = v;
	  pattern[patno].class |= VALUE_MINTERRITORY;
	}
	else if (strncmp(entry, "maxterri", 8) == 0) {
	  pattern[patno].maxterritory = v;
	  pattern[patno].class |= VALUE_MAXTERRITORY;
	}
	else if (strncmp(entry, "shape", 5) == 0) {
	  pattern[patno].shape = v;
	  pattern[patno].class |= VALUE_SHAPE;
	}
	else if (strncmp(entry, "followup", 8) == 0) {
	  pattern[patno].followup = v;
	  pattern[patno].class |= VALUE_FOLLOWUP;
	}
	else if (strncmp(entry, "reverse_followup", 16) == 0) {
	  pattern[patno].reverse_followup = v;
	  pattern[patno].class |= VALUE_REV_FOLLOWUP;
	}
	else {
	  fprintf(stderr, "Unknown value field: %s\n", entry);
	  break;
	}
      }
      else {
	strncpy(helper_fn_names[patno], entry, 79);
	break;
      }
    }
      
    {
      if (strchr(class,'s')) pattern[patno].class |= CLASS_s;
      if (strchr(class,'O')) pattern[patno].class |= CLASS_O;
      if (strchr(class,'o')) pattern[patno].class |= CLASS_o;
      if (strchr(class,'X')) pattern[patno].class |= CLASS_X;
      if (strchr(class,'x')) pattern[patno].class |= CLASS_x;
      if (strchr(class,'D')) pattern[patno].class |= CLASS_D;
      if (strchr(class,'C')) pattern[patno].class |= CLASS_C;
      if (strchr(class,'c')) pattern[patno].class |= CLASS_c;
      if (strchr(class,'n')) pattern[patno].class |= CLASS_n;
      if (strchr(class,'B')) pattern[patno].class |= CLASS_B;
      if (strchr(class,'A')) pattern[patno].class |= CLASS_A;
      if (strchr(class,'b')) pattern[patno].class |= CLASS_b;
      if (strchr(class,'e')) pattern[patno].class |= CLASS_e;
      if (strchr(class,'E')) pattern[patno].class |= CLASS_E;
      if (strchr(class,'a')) pattern[patno].class |= CLASS_a;
      if (strchr(class,'d')) pattern[patno].class |= CLASS_d;
      if (strchr(class,'I')) pattern[patno].class |= CLASS_I;
      if (strchr(class,'J')) pattern[patno].class |= CLASS_J;
      if (strchr(class,'j')) pattern[patno].class |= CLASS_j;
      if (strchr(class,'t')) pattern[patno].class |= CLASS_t;
      if (strchr(class,'T')) pattern[patno].class |= CLASS_T;
      if (strchr(class,'U')) pattern[patno].class |= CLASS_U;
      if (strchr(class,'W')) pattern[patno].class |= CLASS_W;
    }

  }

      
  /* Now get the symmetry. There are extra checks we can make to do with
   * square-ness and edges. We do this before we work out the edge constraints,
   * since that mangles the size info.
   */
  
  switch(symmetry) {
  case '+' :
    if (where & (NORTH_EDGE|EAST_EDGE|SOUTH_EDGE|WEST_EDGE))
      fprintf(stderr,
	      "Warning : symmetry inconsistent with edge constraints (pattern %s)\n", 
	      pattern_names[patno]);
    pattern[patno].trfno = 2;
    break;

  case 'X' : 
    if (where & (NORTH_EDGE|EAST_EDGE|SOUTH_EDGE|WEST_EDGE))
      fprintf(stderr,
	      "Warning : X symmetry inconsistent with edge constraints (pattern %s)\n", 
	      pattern_names[patno]);
    if (maxi != maxj)
      fprintf(stderr,
	      "Warning : X symmetry requires a square pattern (pattern %s)\n",
	      pattern_names[patno]);
    pattern[patno].trfno = 2;
    break;

  case '-' :
    if (where & (NORTH_EDGE|SOUTH_EDGE))
      fprintf(stderr,
	      "Warning : symmetry inconsistent with edge constraints (pattern %s)\n", 
	      pattern_names[patno]);
    pattern[patno].trfno = 4;
    break;
    
  case '|' :
    if (where & (EAST_EDGE|WEST_EDGE))
      fprintf(stderr,
	      "Warning : symmetry inconsistent with edge constraints (pattern %s)\n", 
	      pattern_names[patno]);
    pattern[patno].trfno = 4;
    break;

  case '\\' :
  case '/' :
    /* FIXME: Can't be bothered putting in the edge tests.
    *         (What does this mean?)
    */
    if (maxi != maxj)
      fprintf(stderr,
	      "Warning : \\ or / symmetry requires a square pattern (pattern %s)\n", 
	      pattern_names[patno]);

    pattern[patno].trfno = 4;
    break;

  case 'O' :
    if (where & (NORTH_EDGE|EAST_EDGE|SOUTH_EDGE|WEST_EDGE))
      fprintf(stderr,
	      "Warning : symmetry inconsistent with edge constraints (pattern %s)\n", 
	      pattern_names[patno]);
    pattern[patno].trfno = 5;  /* Ugly special convention. */
    break;

  default:
    fprintf(stderr,
	    "Warning : symmetry character '%c' not implemented - using '8' (pattern %s)\n", 
	    symmetry, pattern_names[patno]);
    /* FALLTHROUGH */
  case '8' :
    pattern[patno].trfno = 8;
    break;
  }

}
      

static void
read_constraint_line(char *line)
{
  /* Avoid buffer overrun. */
  assert(strlen(constraint) + strlen(line) < MAXCONSTRAINT);

  /* Append the new line. */
  strcat(constraint, line);

  pattern[patno].autohelper_flag |= HAVE_CONSTRAINT;
}


static void
read_action_line(char *line)
{
  /* Avoid buffer overrun. */
  assert(strlen(action) + strlen(line) < MAXACTION);

  /* Append the new line. */
  strcat(action, line);

  pattern[patno].autohelper_flag |= HAVE_ACTION;
}


static void
generate_autohelper_code(int funcno, int number_of_params, int *labels)
{
  int i;
  
  if (autohelper_functions[funcno].params >= 0) {
    switch (number_of_params) {
    case 0:
      code_pos += sprintf(code_pos, autohelper_functions[funcno].code);
      break;
    case 1:
      code_pos += sprintf(code_pos, autohelper_functions[funcno].code,
			  labels[0], labels[0]);
      break;
    case 2:
      code_pos += sprintf(code_pos, autohelper_functions[funcno].code,
			  labels[0], labels[0],
			  labels[1], labels[1]);
      break;
    case 3:
      code_pos += sprintf(code_pos, autohelper_functions[funcno].code,
			  labels[0], labels[0],
			  labels[1], labels[1],
			  labels[2], labels[2]);
    }
    return;
  }
  
  /* This is a very special case where there is assumed to be a
   * variable number of parameters and these constitute a series of
   * moves to make followed by a final attack or defense test.
   */
  if (autohelper_functions[funcno].params == -1)
    code_pos += sprintf(code_pos, autohelper_functions[funcno].code,
			number_of_params-1);
  else if (autohelper_functions[funcno].params == -2)
    code_pos += sprintf(code_pos, autohelper_functions[funcno].code,
			number_of_params-2);
  else
    code_pos += sprintf(code_pos, autohelper_functions[funcno].code,
			number_of_params-3);
    
  for (i = 0; i < number_of_params; i++) {
    /* The special label '?' represents a tenuki. We replace this with
     * the coordinate pair (-1, -1).
     */
    if (labels[i] == (int) '?')
      code_pos += sprintf(code_pos, ", -1, -1");
    else
      code_pos += sprintf(code_pos, ", %ci, %cj", labels[i], labels[i]);
  }
  code_pos += sprintf(code_pos, ")");
}


/* Parse the constraint and generate the corresponding helper code.
 * We use a small state machine.
 */
static void
parse_constraint_or_action(char *line)
{
  int state = 0;
  char *p;
  int n = 0;
  int label = 0;
  int labels[MAXLABELS];
  int N = sizeof(autohelper_functions)/sizeof(struct autohelper_func);
  int number_of_params = 0;
  for (p=line; *p; p++)
  {
    switch (state) {
      case 0: /* Looking for a token, echoing other characters. */
	for (n = 0; n < N; n++) {
	  if (strncmp(p, autohelper_functions[n].name,
		      strlen(autohelper_functions[n].name)) == 0) {
	    state=1;
	    p += strlen(autohelper_functions[n].name)-1;
	    break;
	  }
	}
	if (state == 0 && *p != '\n')
	  *(code_pos++) = *p;
	break;
	
      case 1: /* Token found, now expect a '('. */
	if (*p != '(') {
	  fprintf(stderr,
		  "Syntax error in constraint or action, '(' expected (pattern %s).\n", 
		  pattern_names[patno]);
	  return;
	}
	else {
	  assert(autohelper_functions[n].params <= MAXPARAMS);
	  number_of_params = 0;
	  if (autohelper_functions[n].params != 0)
	    state = 2;
	  else
	    state = 3;
	}
	break;
	
      case 2: /* Time for a label. */
	if ((*p != '*') && (*p != '?') && !strchr(VALID_CONSTRAINT_LABELS, *p)) {
	  if (strchr("XxOo", *p))
	    fprintf(stderr,
		    "mkpat: '%c' is not allowed as a constraint label (pattern %s).\n",
		    *p, pattern_names[patno]);
	  else
	    fprintf(stderr,
		    "mkpat: Syntax error in constraint or action, label expected (pattern %s).\n", 
		    pattern_names[patno]);
	  return;
	}
	else {
	  if ((*p == '?') && (number_of_params == 0)) {
	    fprintf(stderr,
		    "mkpat: tenuki (?) cannot be the first label (pattern %s)\n", pattern_names[patno]);
	    return;
	  }
	  if (*p == '*')
	    label = (int) 't';
	  /* The special label '?' represents a tenuki. */
	  else if (*p == '?')
	    label = (int) *p;
	  else {
	    label = (int) *p;
	    if (label_coords[label][0] == -1) {
	      fprintf(stderr,
		      "mkpat: The constraint or action uses a label (%c) that wasn't specified in the diagram (pattern %s).\n", 
		      label, pattern_names[patno]);
	      return;
	    }
	  }
	  labels[number_of_params] = label;
	  number_of_params++;
	  state = 3;
	}
	break;

      case 3: /* A ',' or a ')'. */
	if (*p == ',') {
	  if ((autohelper_functions[n].params >= 0)
	      && (number_of_params == autohelper_functions[n].params)) {
	    fprintf(stderr,
		    "Syntax error in constraint or action, ')' expected (pattern %s).\n",
		    pattern_names[patno]);
	    return;
	  }
	  if (number_of_params == MAXPARAMS) {
	    fprintf(stderr,
		    "Error in constraint or action, too many parameters. (pattern %s).\n", 
		    pattern_names[patno]);
	    return;
	  }
	  state = 2;
	  break;
	}
	else if (*p != ')') {
	  fprintf(stderr, 
		  "Syntax error in constraint or action, ',' or ')' expected (pattern %s).\n", 
		  pattern_names[patno]);
	  return;
	}
	else { /* a closing parenthesis */
	  if ((autohelper_functions[n].params >= 0)
	      && (number_of_params < autohelper_functions[n].params)) {
	    fprintf(stderr,
		    "Syntax error in constraint or action, ',' expected (pattern %s).\n",
		    pattern_names[patno]);
	    return;
	  }
	  generate_autohelper_code(n, number_of_params, labels);
	  state = 0;
	}
	break;
	
      default:
	fprintf(stderr,
		"Internal error in parse_constraint_or_action(), unknown state.\n");
	return;
    }
  }
}

/* Finish up a constraint and/or action and generate the automatic
 * helper code. The constraint text is in the global variable
 * constraint. */

static void
finish_constraint_and_action(char *name)
{
  unsigned int i;
  int have_constraint = (pattern[patno].autohelper_flag & HAVE_CONSTRAINT);
  int have_action = (pattern[patno].autohelper_flag & HAVE_ACTION);

  /* Mark that this pattern has an autohelper. */
  pattern[patno].autohelper = dummyhelper;
  
  /* Generate autohelper function declaration. */
  code_pos += sprintf(code_pos, "static int\nautohelper%s%d (struct pattern *patt, int transformation, int ti, int tj, int color, int action)\n{\n  int",
		      name, patno);

  /* Generate variable declarations. */
  for (i = 0; i < sizeof(VALID_CONSTRAINT_LABELS); i++) {
    int c = (int) VALID_CONSTRAINT_LABELS[i];

    if (label_coords[c][0] != -1)
      code_pos += sprintf(code_pos, " %ci, %cj,", c, c);
  }

  /* Replace the last ',' with ';' */
  if (*(code_pos-1) == ',')
    *(code_pos-1) = ';';
  else {
    code_pos -= 3; /* no variable, erase "int" */
    code_pos += sprintf(code_pos, "UNUSED(transformation);");
  }

  /* Include UNUSED statements for two parameters */
  code_pos += sprintf(code_pos, "\n  UNUSED(patt);\n  UNUSED(color);\n");
  if (!have_constraint || !have_action)
    code_pos += sprintf(code_pos, "  UNUSED(action);\n");
  
  /* Generate coordinate transformations. */
  for (i = 0; i < sizeof(VALID_CONSTRAINT_LABELS); i++) {
    int c = (int) VALID_CONSTRAINT_LABELS[i];

    if (label_coords[c][0] != -1)
      code_pos += sprintf(code_pos,
			  "\n  offset(%d, %d, ti, tj, &%ci, &%cj, transformation);",
			  label_coords[c][0] - ci - pattern[patno].movei,
			  label_coords[c][1] - cj - pattern[patno].movej,
			  c, c);
  }

  code_pos += sprintf(code_pos, "\n\n");
  if (have_constraint && have_action)
    code_pos += sprintf(code_pos, "  if (!action)\n  ");
  if (have_constraint) {
    code_pos += sprintf(code_pos, "  return ");
    parse_constraint_or_action(constraint);
    code_pos += sprintf(code_pos, ";\n");
  }
  if (have_action) {
    code_pos += sprintf(code_pos, "  ");
    parse_constraint_or_action(action);
    code_pos += sprintf(code_pos, ";\n");
    code_pos += sprintf(code_pos, "\n  return 0;\n");
  }
  code_pos += sprintf(code_pos, "}\n\n");
  
  /* Check that we have not overrun our buffer. That would be really bad. */
  assert(code_pos <= autohelper_code + sizeof(autohelper_code));
}



/* ================================================================ */
/*           stuff to write the elements of a pattern               */
/* ================================================================ */



/* callback function used to sort the elements in a pattern. 
 * We want to sort the patterns by attribute.
 *
 *  RANK 01234567
 *  ATT  57126340
 *       ,!XOaxo.
 * 
 * so that cheaper / more specific tests are done early on
 * For connections mode, the inhibition points (7)
 * must be first.
 */

static int
compare_elements(const void *a, const void *b)
{
  static char order[] = {7,2,3,5,6,0,4,1};  /* score for each attribute */
  return  order[((const struct patval *)a)->att]
    - order[((const struct patval *)b)->att];
}



/* flush out the pattern stored in elements[]. Don't forget
 * that elements[].{x,y} and min/max{i,j} are still relative
 * to the top-left corner of the original ascii pattern, and
 * not relative to the anchor stone ci,cj
 */

static void
write_elements(char *name)
{
  int node;

  assert(ci != -1 && cj != -1);

  /* sort the elements so that least-likely elements are tested first. */
  qsort(elements, el, sizeof(struct patval), compare_elements);

  printf("static struct patval %s%d[]={\n",name,patno);

  /* This may happen for fullboard patterns. */
  if (el == 0) {
    printf("    {0,0,-1}}; /* Dummy element, not used. */\n\n");
    return;
  }
  
  for (node = 0;node < el; node++) {
    assert(elements[node].x >= mini && elements[node].y >= minj);
    assert(elements[node].x <= maxi && elements[node].y <= maxj);

    printf("   {%d,%d,%d}%s",
	   elements[node].x - ci, elements[node].y - cj, elements[node].att,
	   node < el-1 ? ",\n" : "};\n\n");
  }
}





/* ================================================================ */
/*         stuff to write out the stored pattern structures         */
/* ================================================================ */


/* sort and write out the patterns */
static void
write_patterns(char *name)
{
  int j;

  /* Write out the patterns. */
  if (fullboard)
    printf("struct fullboard_pattern %s[]={\n", name);
  else
    printf("struct pattern %s[]={\n", name);

  for (j = 0; j < patno; ++j) {
    struct pattern *p = pattern + j;

    if (fullboard) {
      printf("  {%s%d,%d,\"%s\",%2d,%2d,%f},\n", name, j, p->patlen,
	     pattern_names[j], p->movei, p->movej, p->value);
      continue;
    }
    
    /* p->min{i,j} and p->max{i,j} are the maximum extent of the elements,
     * including any rows of '?' which do not feature in the elements list.
     * ie they are the positions of the topleft and bottomright corners of
     * the pattern, relative to the pattern origin. These just transform same
     * as the elements.
     */
    
    printf("  {%s%d,%d,%d, \"%s\",%d,%d,%d,%d,%d,%d,0x%x,%d,%d",
	     name, j,
	     p->patlen,
	     p->trfno,
	     pattern_names[j],
	     p->mini, p->minj,
	     p->maxi, p->maxj,
	     p->maxi - p->mini,   /* height */
	     p->maxj - p->minj,   /* width  */
	     p->edge_constraints,
	     p->movei, p->movej);


#if GRID_OPT
    printf(",\n    {");
    {
      int ll;

      for (ll = 0; ll < 8; ++ll)
	printf(" 0x%08x%s", p->and_mask[ll], ll<7 ? "," : "");
      printf("},\n    {");
      for (ll = 0; ll < 8; ++ll)
	printf(" 0x%08x%s", p->val_mask[ll], ll<7 ? "," : "");
    }
    printf("}\n   ");
#endif

    printf(", 0x%x,%f,%f,%f,%f,%f,%f,%f,%d,%s,",
	   p->class,
	   p->value,
	   p->maxvalue,
	   p->minterritory,
	   p->maxterritory,
	   p->shape,
	   p->followup,
	   p->reverse_followup,
	   p->autohelper_flag,
	   helper_fn_names[j]);
    if (p->autohelper)
      printf("autohelper%s%d", name, j);
    else
      printf("NULL");
    printf(",%d", p->anchored_at_X);
#if PROFILE_PATTERNS
    printf(",0,0");
#if DFA_ENABLED
    printf(",0");
#endif /* DFA_ENABLED */
#endif

    printf("},\n");
  }

  if (fullboard) {
    printf("  {NULL,0,NULL,0,0,0.0}\n};\n");
    return;
  }
  
  /* Add a final empty entry. */
  printf("  {NULL, 0,0,NULL,0,0,0,0,0,0,0,0,0");
#if GRID_OPT
  printf(",{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0}");
#endif
  printf(",0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0,NULL,NULL,0");
#if PROFILE_PATTERNS
  printf(",0,0");
#if DFA_ENABLED
    printf(",0");
#endif /* DFA_ENABLED */
#endif
  printf("}\n};\n");
}

/* Write out the pattern db */
static void
write_pattern_db(char *name)
{
  /* Don't want this for fullboard patterns. */
  if (fullboard)
    return;
  
  /* Write out the pattern database. */
  printf("\n");
  printf("struct pattern_db %s_db = {\n", name);
  printf("  -1,\n");
  printf("  %s\n", name);
#if DFA_ENABLED
  if (dfa_c_output)
    printf(" ,& dfa_%s\n", name); /* pointer to the wired dfa */
  else
    printf(" , NULL\n"); /* pointer to a possible dfa */
#endif
  printf("};\n");
}


int
main(int argc, char *argv[])
{
  char line[MAXLINE];  /* current line from file */
  int state = 0;
  int i;
  
  {
    /* parse command-line args */
#if DFA_ENABLED
    while ( (i=gg_getopt(argc, argv, "V:vcbXfmtD")) != EOF) {
#else
    while ( (i=gg_getopt(argc, argv, "vcbXfm")) != EOF) {
#endif
      switch(i) {
      case 'v': verbose = 1; break;
      case 'c': pattern_type = CONNECTIONS; break;
      case 'b': anchor = ANCHOR_BOTH; break;
      case 'X': anchor = ANCHOR_X; break;
      case 'f': fullboard = 1; break;
      case 'm': choose_best_anchor = 1; break;
#if DFA_ENABLED
      case 'D':
	dfa_generate = 1; dfa_c_output = 1; 
	break;
      case 'V': dfa_verbose = strtol(gg_optarg, NULL, 10); break;

#endif
      default:
	fprintf(stderr, "Invalid argument ignored\n");
      }
    }
  }


#if DFA_ENABLED
  if (dfa_generate) {
    dfa_init();
    new_dfa(&dfa, "mkpat's dfa");
  }
#endif 

  if (gg_optind >= argc) {
    fputs(USAGE, stderr);
    exit(EXIT_FAILURE);
  }

  printf(PREAMBLE);

  /* Parse the input file, output pattern elements as as we find them,
   * and store pattern entries for later output.
   */

  /* Initialize pattern number and buffer for automatically generated
   * helper code.
   */
  patno = -1;
  autohelper_code[0] = 0;
  code_pos = autohelper_code;
  
  /* We do this parsing too with the help of a state machine.
   * state
   *   0     Waiting for a Pattern line.
   *   1     Pattern line found, waiting for a position diagram.
   *   2     Reading position diagram.
   *   3     Waiting for entry line (":" line).
   *   4     Waiting for optional constraint diagram.
   *   5     Reading constraint diagram.
   *   6     Waiting for constraint line (";" line).
   *   7     Reading a constraint
   *   7     Reading an action
   *
   * FIXME: This state machine should be possible to simplify.
   *
   */
  
  while (fgets(line, MAXLINE, stdin)) {

    if (line[strlen(line)-1] != '\n') {
      fprintf(stderr, "mkpat: line truncated: %s, length %d\n", line,
	      (int) strlen(line));

      fatal_errors++;
    }

    /* remove trailing white space from line */

    i = strlen(line)-2;	/* start removing backwards just before newline */
    while (i >= 0 
	   && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
      line[i]   = '\n';
      line[i+1] = '\0';
      i--;
    }

    /* FIXME: We risk overruning a buffer here. */
    if (sscanf(line, "Pattern %s", pattern_names[patno+1]) == 1) {
      char *p = strpbrk(pattern_names[patno+1], " \t");

      if (p)
	*p = 0;
      if (patno >= 0) {
	switch (state) {
	case 1:
	  fprintf(stderr, "Warning, empty pattern %s\n",
		  pattern_names[patno]);
	  break;
	case 2:
	case 3:
	  fprintf(stderr, "Warning, no entry line for pattern %s\n",
		  pattern_names[patno]);
	  break;
	case 5:
	case 6:
	  fprintf(stderr,
		  "Warning, constraint diagram but no constraint line for pattern %s\n",
		  pattern_names[patno]);
	  break;
	case 7:
	case 8:
	  finish_constraint_and_action(argv[gg_optind]); /* fall through */
	case 0:
	case 4:
	  patno++;
	  reset_pattern();
	}
      }
      else {
	patno++;
	reset_pattern();
      }
      state = 1;
    }
    else if (line[0] == '\n' || line[0] == '#') { 
      /* nothing */
      if (state == 2 || state == 5)
	state++;
    }
    else if ( strchr(VALID_PATTERN_CHARS, line[0]) ||
		strchr(VALID_EDGE_CHARS, line[0]) ||
		strchr(VALID_CONSTRAINT_LABELS, line[0])) { 
      /* diagram line */
      switch (state) {
      case 0:
      case 3:
      case 6:
      case 7:
      case 8:
	fprintf(stderr, "Huh, another diagram here? (pattern %s)\n",
		pattern_names[patno]);
	break;
      case 1:
	state++; /* fall through */
      case 2:
	read_pattern_line(line);
	break;
      case 4:
	state++; /* fall through */
      case 5:
	read_constraint_diagram_line(line);
	break;
      }	
    }
    else if (line[0] == ':') {
      if (state == 2 || state == 3) {
	finish_pattern(line);
	write_elements(argv[gg_optind]);
#if DFA_ENABLED
	if (dfa_generate)
	  write_to_dfa(patno);
#endif
	state = 4;
      }
      else {
	fprintf(stderr, "Warning, unexpected entry line in pattern %s\n",
		pattern_names[patno]);
      }
    } 
    else if (line[0] == ';') {
      if (state == 5 || state == 6 || state == 7) {
	read_constraint_line(line+1);
	state = 7;
      }
      else {
	fprintf(stderr, "Warning, unexpected constraint line in pattern %s\n",
		pattern_names[patno]);
      }
    } 
    else if (line[0] == '>') {
      if (state == 4 || state == 5 || state == 6 || state == 7 || state == 8) {
	read_action_line(line+1);
	state = 8;
      }
      else {
	fprintf(stderr, "Warning, unexpected action line in pattern %s\n",
		pattern_names[patno]);
      }
    } 
    else
      fprintf(stderr, "Warning, malformed line \"%s\" in pattern %s\n",
	      line, pattern_names[patno]);
  }

  if (patno >= 0) {
    switch (state) {
    case 1:
      fprintf(stderr, "Warning, empty pattern %s\n",
	      pattern_names[patno]);
      break;
    case 2:
    case 3:
      fprintf(stderr, "Warning, no entry line for pattern %s\n",
	      pattern_names[patno]);
      break;
    case 5:
    case 6:
      fprintf(stderr,
	      "Warning, constraint diagram but no constraint line for pattern %s\n",
	      pattern_names[patno]);
      break;
    case 7:
    case 8:
      finish_constraint_and_action(argv[gg_optind]); /* fall through */
    case 0:
    case 4:
      patno++;
      reset_pattern();
    }
  }


  if (verbose)
    fprintf(stderr, "%d / %d patterns have edge-constraints\n",
	    pats_with_constraints, patno);

  /* Write the autohelper code. */
  printf("%s", autohelper_code);

  write_patterns(argv[gg_optind]);

#if DFA_ENABLED
  if (dfa_generate) {
    fprintf(stderr, "---------------------------\n");
    fprintf(stderr, "dfa for %s\n",argv[gg_optind]);
    fprintf(stderr, "size: %dKb for ", dfa_size(&dfa));
    fprintf(stderr, "%d patterns\n", patno);

    strcpy(dfa.name,argv[gg_optind]);
    print_c_dfa(argv[gg_optind], &dfa);
    fprintf(stderr, "---------------------------\n");

    if (DFA_MAX_MATCHED/8 < patno)
      fprintf(stderr,"Warning: Increase DFA_MAX_MATCHED in 'dfa.h'.\n");

    kill_dfa(&dfa);
    dfa_end();
  }
#endif


  write_pattern_db(argv[gg_optind]);

  return fatal_errors ? 1 : 0;
}


/*
 * Local Variables:
 * tab-width: 8
 * c-basic-offset: 2
 * End:
 */

