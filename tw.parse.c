/* $Header$ */
/*
 * tw.parse.c: Everyone has taken a shot in this futile effort to
 *	       lexically analyze a csh line... Well we cannot good
 *	       a job as good as sh.lex.c; but we try. Amazing that
 *	       it works considering how many hands have touched this code
 */
#include "config.h"

#ifndef lint
static char *rcsid = "$Id$";
#endif

#include "sh.h"
#include "tw.h"
/* #define TENEDEBUG */

/* true if the path has relative elements */
static bool    relatives_in_path;	

static int maxitems = 0;
Char **command_list = (Char **)NULL;  /* the pre-digested list of commands
					 for speed and general usefullness */
int numcommands = 0;
int have_sorted = 0;

#ifdef notdef
int  dirctr = 0;		/* -1 0 1 2 ... */
#endif
Char dirflag[5];		/*  ' nn\0' - dir #s -  . 1 2 ... */

/* do the expand or list on the command line -- SHOULD BE REPLACED */

extern Char NeedsRedraw;	/* from ed.h */
extern int TermH;		/* from the editor routines */
extern int lbuffed;		/* from sh.print.c */
extern bool relatives_in_path;	/* set true if PATH has relative elements */

static void free_items();
static void extract_dir_and_name();
static Char *quote_meta();
static Char *getentry();
static Char *dollar();
static Char *tilde();
static Char filetype();
static int t_glob();
static int is_prefix();
static int is_suffix();
static int recognize();
static int max();
static int ignored();
static void tw_get_comm_list();
static int isadirectory();

/*
 * If we find a set command, then we break a=b to a= and word becomes
 * b else, we don't break a=b.
 */
#define isaset(c, w) ((w)[-1] == '=' && \
		      ((c)[0] == 's' && (c)[1] == 'e' && (c)[2] == 't' && \
		       ((c[3] == ' ' || (c)[3] == '\t'))))
/*
 * Return value for tenematch():
 *  > 1:    No. of items found
 *  = 1:    Exactly one match / spelling corrected
 *  = 0:    No match / spelling was correct
 *  < 0:    Error (incl spelling correction impossible)
 */
int
tenematch (inputline, inputline_size, num_read, command)
Char   *inputline;		/* match string prefix */
int     inputline_size;		/* max size of string */
int	num_read;		/* # actually in inputline */
COMMAND command;		/* LIST or RECOGNIZE or PRINT_HELP */

{
    Char word [FILSIZ + 1];
    register Char *str_end, *word_start, *cmd_start, *wp;
    Char *cmd_st;
    int space_left;
    int is_a_cmd;		/* UNIX command rather than filename */
    int search_ret;		/* what search returned for debugging */
    int in_single, in_double;	/* In single or in_double quotes */

    str_end = &inputline[num_read];

    /* 
     * space backward looking for the beginning of this command 
     */
    for (cmd_st = str_end; cmd_st > inputline; --cmd_st) 
	if (iscmdmeta(cmd_st[-1])
	    && ((cmd_st - 1 == inputline) || (cmd_st[-2] != '\\')))
	    break;
				/* step forward over leading spaces */
    while (*cmd_st != '\0' && (*cmd_st == ' ' || *cmd_st == '\t'))
	cmd_st++;

    /*
     * Find LAST occurence of a delimiter in the inputline.
     * The word start is one character past it.
     */
    for (word_start = str_end; word_start > inputline; --word_start) {
	if ((ismeta(word_start[-1]) || isaset(cmd_st, word_start)) &&
	    (word_start[-1] != '#') && (word_start[-1] != '$') &&
	    ((word_start - 1 == inputline) || (word_start[-2] != '\\')))
	    break;
    }



#ifdef	masscomp
    /*
     * Avoid a nasty message from the RTU 4.1A & RTU 5.0 compiler
     * concerning the "overuse of registers".
     * According to the compiler release notes, incorrect code
     * may be produced unless the offending expression is rewritten.
     * Therefore, we can't just ignore it, DAS DEC-90.
     */
    space_left = inputline_size;
    space_left -= word_start - inputline + 1;
#else
    space_left = inputline_size - (word_start - inputline) - 1;
#endif
    
    is_a_cmd = starting_a_command (word_start, inputline);
#ifdef TENEDEBUG
    CSHprintf( "starting_a_command %d\n", is_a_cmd); 
#endif

    /*
     * Quote args
     */
    in_double = 0;
    in_single = 0;
    for (cmd_start = word_start, wp = word; cmd_start < str_end; cmd_start++)
	switch (*cmd_start) {
	case '\'':
	    if (!in_double) {
		if (in_single)
		    in_single = 0;
		else
		    in_single = QUOTE;
		/*
		 * Move the word_start further, cause the 
		 * quotes so far have no effect.
		 */
		if (cmd_start == word_start)
		    word_start++;
	    }
	    else
		*wp++ = *cmd_start | QUOTE;
	    break;
	case '"':
	    if (!in_single) {
		if (in_double)
		    in_double = 0;
		else
		    in_double = QUOTE;
		/*
		 * Move the word_start further, cause the 
		 * quotes so far have no effect.
		 */
		if (cmd_start == word_start)
		    word_start++;
	    }
	    else
		*wp++ = *cmd_start | QUOTE;
	    break;
	case '/':
	    /*
	     * This is so that the recognize stuff works easily
	     */
	    *wp++ = *cmd_start;
	    break;
	case '\\':
	    if (in_single || in_double)
		*wp++ = *cmd_start | QUOTE;
	    else
		*wp++ = *++cmd_start | QUOTE;
	    break;
	default:
	    *wp++ = *cmd_start | in_single;
	    break;
	}
    *wp = '\0';


#ifdef TENEDEBUG
    CSHprintf ("\ncmd_st:%s:\n", short2str(cmd_st));
    CSHprintf ("word:%s:\n", short2str(word)); 
    CSHprintf ("word:");
    for (wp = word; *wp; wp++)
	CSHprintf ("%c", *wp & QUOTE ? '-' : ' ');
    CSHprintf (":\n");
#endif
    switch ((int) command) {
	Char buffer[FILSIZ + 1], *bptr;
	Char *items[2], **ptr; 
	int i, count; 

    case RECOGNIZE:
        search_ret = t_search (word, wp, command, space_left, is_a_cmd, 1);

	/*
	 * Change by Christos Zoulas: if the name has metachars in it,
	 * quote the metachars, but only if we are outside quotes.
	 */
	if (*wp && InsertStr((in_single || in_double) ?
			     wp : quote_meta(wp, 
			     (bool) is_set(STRaddsuffix))) < 0)
				/* put it in the input buffer */
	    return -1;		/* error inserting */
	return search_ret;

    case SPELL:
	for (bptr = word_start; bptr < str_end; bptr++) {
	    /* do not try to correct spelling of words containing globbing
	       characters */
	    if (isglob(*bptr))
		return 0;
	}
	if ((search_ret = spell_me(word, sizeof(word), is_a_cmd)) == 1) {
	    DeleteBack(str_end-word_start);	/* get rid of old word */
	    if (InsertStr(word) < 0)	/* insert newly spelled word */
		return -1;		/* error inserting */
	}
	return search_ret;

    case PRINT_HELP:
	do_help (cmd_st);
	return 1;

    case GLOB:
    case GLOB_EXPAND:
	(void) Strncpy(buffer, word, FILSIZ + 1);
	items[0] = buffer;
	items[1] = (Char *) 0;
	ptr = items;
	if ( is_a_cmd ) {
	    CSHprintf("Sorry no globbing for commands yet..\n");
	    return 0;
	}
	if ((count = t_glob(&ptr)) > 0) {
	    if (command == GLOB) 
		print_by_column (STRNULL, ptr, count, is_a_cmd);
	    else {
		DeleteBack(str_end-word_start);	/* get rid of old word */
		for (i = 0; i < count; i++) 
		     if (ptr[i] && *ptr[i]) {
			 if (InsertStr((in_single || in_double) ?
				       ptr[i] : quote_meta(ptr[i], 0)) < 0 ||
			     InsertStr(STRspace) < 0) {
			     blkfree(ptr);
			     return(-1);
			 }
		     }
	    }
	    blkfree(ptr);
	}
	return count;

    case VARS_EXPAND:
	if (dollar(buffer, word)) {
	    DeleteBack(str_end-word_start);
	    if (InsertStr((in_single || in_double) ?
			  buffer : quote_meta(buffer, 0)) < 0)
		return(-1);
	    return(1);
	}
	return(0);

    case LIST:
        search_ret = t_search (word, wp, command, space_left, is_a_cmd, 1);
	return search_ret;
    
    default:
	CSHprintf("tcsh: Internal match error.\n");
	return 1;

    }
}



static int
t_glob(v)
	register Char ***v;
{
	jmp_buf osetexit;

	if (**v == 0)
	    return(0);
	gflag = 0, tglob(*v);
	if (gflag) {
	    getexit(osetexit);	/* make sure to come back here */
	    if (setexit() == 0)
		*v = globall(*v);
	    resexit(osetexit);
	    gargv = 0;
	    if (haderr) {
		haderr = 0;
		NeedsRedraw = 1;
		return(-1);
	    }
	    if (*v == 0)
		return(0);
	} else
	    return(0);

	return(gargc);
}


/*
 * quote (\) the meta-characters in a word
 * except trailing space if trail_space is set
 * return pointer to quoted word in static storage
 */
static Char *
quote_meta(word, trail_space)
Char *word;
bool trail_space;
{
    static Char buffer[2*FILSIZ + 1], *bptr, *wptr;

    for ( bptr = buffer, wptr = word; *wptr != '\0'; ) {
	if ((cmap(*wptr, _META|_DOL|_Q|_ESC|_GLOB) || *wptr == HIST ||
	     *wptr == HISTSUB) &&
	    (*wptr != ' ' || !trail_space || *(wptr + 1) != '\0'))
	    *bptr++ = '\\';
	*bptr++ = *wptr++;
    }
    *bptr = '\0';
    return(buffer);
}


/*
 * return true if check items initial chars in template
 * This differs from PWB imatch in that if check is null
 * it items anything
 */

static int
is_prefix (check, template)
register Char   *check, *template;
{
    for (; *check; check++, template++)
	if ((*check & TRIM) != (*template & TRIM))
	    return (FALSE);
    return (TRUE);
}

/*
 *  Return true if the chars in template appear at the
 *  end of check, I.e., are it's suffix.
 */
static int
is_suffix(check, template)
register Char *check, *template;
{
    register Char *t, *c;
    for (t = template; *t++;);
    for (c = check; *c++;);
    for (;;) {
	if (t == template)
	    return 1;
	--t; --c;
	if (c == check || (*t & TRIM) != (*c & TRIM))
	    return 0;
    }
}

static int
ignored(entry)
	register Char *entry;
{
	struct varent *vp;
	register Char **cp;

	if ((vp = adrof(STRfignore)) == NULL || (cp = vp->vec) == NULL)
		return (FALSE);
	for (; *cp != NULL; cp++)
		if (is_suffix(entry, *cp))
			return (TRUE);
	return (FALSE);
}

/* return true if the command starting at wordstart is a command */

#define EVEN(x) (((x) & 1) != 1)

int
starting_a_command (wordstart, inputline)
register Char *wordstart, *inputline;
{
    register Char *ptr, *ncmdstart;
    int count;
    static Char
	    cmdstart[] = { '`', ';', '&', '(', '|', '\0' },
	    cmdalive[] = { ' ', '\t', '\'', '"', '<', '>', '\0' };
    /*
     * Find if the number of backquotes is odd or even.
     */
    for ( ptr = wordstart, count = 0; 
	  ptr >= inputline; 
	  count += (*ptr-- == '`'));
    /*
     * if the number of backquotes is even don't include the backquote
     * char in the list of command starting delimiters
     * [if it is zero, then it does not matter]
     */
    ncmdstart = cmdstart + EVEN(count);

    /*
     * look for the characters previous to this word
     * if we find a command starting delimiter we break.
     * if we find whitespace and another previous word then
     * we are not a command
     *
     * count is our state machine:
     *	0 looking for anything
     *	1 found white-space looking for non-ws 
     */
    for (count = 0; wordstart >= inputline; wordstart--) {
	if ( *wordstart == '\0' ) continue;
	if (Strchr(ncmdstart, *wordstart))
	    break;
	/*
	 * found white space
	 */
	if (ptr = Strchr(cmdalive, *wordstart))
	    count = 1;
	if ( count == 1 && ! ptr )
	    return (FALSE);
    }

    if (wordstart > inputline) 
	switch(*wordstart) {
	case '&':			/* Look for >& */
	    while (wordstart > inputline &&
		   (*--wordstart == ' ' || *wordstart == '\t'));
	    if (*wordstart == '>')
		return (FALSE);
	    break;
	case '(': 			/* check for foreach, if etc. */
	    while (wordstart > inputline &&
		   (*--wordstart == ' ' || *wordstart == '\t'));
	    if (!iscmdmeta(*wordstart) && 
		(*wordstart != ' ' && *wordstart != '\t'))
		return(FALSE);
	    break;
	default:
	    break;
	}
    return (TRUE);
}


/*
 * Object: extend what user typed up to an ambiguity.
 * Algorithm:
 * On first match, copy full entry (assume it'll be the only match) 
 * On subsequent matches, shorten extended_name to the first
 * character mismatch between extended_name and entry.
 * If we shorten it back to the prefix length, stop searching.
 */
static int
recognize (extended_name, entry, name_length, numitems)
Char *extended_name, *entry;
int name_length, numitems;
{
    if (numitems == 1)				/* 1st match */
	copyn (extended_name, entry, MAXNAMLEN);
    else					/* 2nd and subsequent matches */
    {
	register Char *x, *ent;
	register int len = 0;
	for (x = extended_name, ent = entry; 
	     *x && (*x & TRIM) == (*ent & TRIM); x++, len++, ent++);
	*x = '\0';				/* Shorten at 1st char diff */
	if (len == name_length)			/* Ambiguous to prefix? */
	    return (-1);		       /* So stop now and save time */
    }
    return (0);
}


/*
 * Perform a RECOGNIZE or LIST command on string "word".
 *
 * Return value:
 *  >= 0:   SPELL command: "distance" (see spdist())
 *          other:         No. of items found
 *  < 0:    Error (message or beep is output)
 */

/*ARGSUSED*/
int
t_search (word, wp, command, max_word_length, looking_for_command, list_max)
Char   *word,
       *wp;			/* original end-of-word */
COMMAND command;
int max_word_length, looking_for_command, list_max;
{
    register ignoring = 1, nignored = 0;
    register name_length,		/* Length of prefix (file name) */
	    looking_for_lognames;	/* True if looking for login names */
    int	    showpathn;			/* True if we want path number */
    Char    tilded_dir[FILSIZ + 1],	/* dir after ~ expansion */
            dollar_dir[FILSIZ + 1],	/* dir after $ expansion */
            dir[FILSIZ + 1],		/* /x/y/z/ part in /x/y/z/f */
            name[MAXNAMLEN + 1],	/* f part in /d/d/d/f */
            extended_name[MAXNAMLEN+1],	/* the recognized (extended) name */
	    *entry = NULL,		/* single directory entry or logname */
	    *target = NULL;		/* Target to expand/correct/list */
    int     next_command = 0;		/* the next command to take out of */
					/* the list of commands */
    int	    looking_for_shellvar = 0, 	/* true if looking for $foo */
	    looking_for_file = 0; 	/* true if looking for a file name */
    Char **pathv; 			/* pointer to PATH elements */
    struct varent *v_ptr = NULL; 	/* current shell variable position */
    Char    **env_ptr = NULL;		/* current env. variable position */

    int d = 4, nd;	/* distance and new distance to command for SPELL */
    int exec_check = 0, dir_ok = 0; /* need to check executability/directory */

    static Char 			/* For unset path		*/
	    *pv[2] = { STRNULL, (Char *) 0 };
    static DIR 
	    *dir_fd = NULL;
    static Char
           **items = NULL;		/* file names when doing a LIST */

    /* bugfix by Marty Grossman (grossman@CC5.BBN.COM):
       directory listing can dump core when interrupted */
    static int numitems;

    pathv = (v_ptr = adrof(STRPATH)) == (struct varent *) 0 ? pv : v_ptr->vec;

    if (items != NULL)
	FREE_ITEMS (items, numitems);
    numitems = 0;
    if (dir_fd != NULL)
	FREE_DIR (dir_fd);

    extract_dir_and_name (word, dir, name);
    looking_for_lognames = (*word == '~') && (Strchr(word, '/') == NULL);
    looking_for_shellvar = (target = Strrchr(name, '$')) &&
						(Strchr(name, '/') == NULL);
    looking_for_file = (!looking_for_command && !looking_for_lognames &&
			!looking_for_shellvar) || Strchr(word, '/');

    /* PWP: don't even bother when doing ALL of the commands */
    if (looking_for_command && (*word == '\0')) {
	Beep();
	return (-1);
    }
    tilded_dir[0] = '\0';
    dollar_dir[0] = '\0';

    if (looking_for_shellvar) {			/* Looking for a shell var? */
	v_ptr = tw_start_shell_list();
	env_ptr = tw_start_env_list();
	target++;
    }
    else
	target = name;
    if (looking_for_shellvar || looking_for_file) {
	/* Open the directory */
	/* expand ~user/... and variables stuff */
	if ((dollar(dollar_dir, dir) == 0) || 
	    (tilde(tilded_dir, dollar_dir) == 0) ||
	    ((dir_fd = opendir (*tilded_dir ? short2str(tilded_dir) : ".")) == 
	      NULL)) {
	    CSHprintf("\n%s unreadable\n",
		   *tilded_dir ? short2str(tilded_dir) : 
		   (*dollar_dir ? short2str(dollar_dir) : short2str(dir)));
	    NeedsRedraw = 1;
	    return (-1);
	}
    }
    else if (looking_for_lognames) {		/* Looking for login names? */
	(void) setpwent ();				/* Open passwd file */
	copyn (name, &word[1], MAXNAMLEN);	/* name sans ~ */
    } else if (looking_for_command) {
	if (!numcommands)	/* if we have no list of commands */
	    tw_get_comm_list();
	if (!have_sorted) {	/* if we haven't sorted them yet */
	    tw_add_builtins();
	    tw_add_aliases();
	    tw_sort_comms();	/* re-build the command path for twenex.c */
	}
	copyn (target, word, MAXNAMLEN);	/* so it can match things */
    } else {
	CSHprintf("\ntcsh internal error: I don't know what I'm looking for!\n");
	NeedsRedraw = 1;
	return(-1);
    }
	

 again:
    name_length = Strlen (target);
    showpathn = looking_for_command && is_set(STRlistpathnum);

    while (1) {
	if (looking_for_shellvar) {
	    if ((entry = tw_next_shell_var (&v_ptr)) == NULL) 
		if ((entry = tw_next_env_var (&env_ptr)) == NULL) 
		    break;
	} else if (looking_for_file || looking_for_lognames) {
	    if ((entry = getentry (dir_fd, looking_for_lognames)) == NULL) {
		break;
	    }
	    
	    /*
	     * Don't match . files on null prefix match
	     */
	    if (name_length == 0 && entry[0] == '.' &&
		!looking_for_lognames && !is_set(STRshowdots))
		continue;
	    if (looking_for_command && !looking_for_lognames) {
		exec_check = 1;
		dir_ok = 1;
	    }
	} else if (looking_for_command) {
#ifdef  NOTDEF      /* Not possible */
	    if (numcommands == 0) {
		dohash ();
	    }
#endif
	    /* searching . added by Andreas Luik <luik@isaak.isa.de> */

	    if ((next_command < numcommands) &&
		(entry = command_list[next_command]) == NULL)
		next_command = numcommands;
	    if (next_command >= numcommands) {  /* search relative elems */
		if (!relatives_in_path)
		    break;	/* we don't need to do it */
		while ((dir_fd == NULL ||
			(entry = getentry (dir_fd, FALSE)) == NULL) &&
		       *pathv) {
		    if (dir_fd != NULL)
			FREE_DIR (dir_fd);
		    entry = NULL;
		    while (*pathv && pathv[0][0] == '/')
			pathv++;
		    if (*pathv) {
			if (pathv[0][0] == '\0' ||
			    (pathv[0][0] == '.' && pathv[0][1] == '\0')) {
			    *tilded_dir = '\0';
			    dir_fd = opendir (".");
			} else {
			    copyn (tilded_dir, *pathv, FILSIZ);
			    catn (tilded_dir, STRslash, FILSIZ);
			    dir_fd = opendir (short2str(*pathv));
			}
			pathv++;
		    }
		}
		if (entry == NULL)
		    break;	/* end of PATH */
		/* executability check for other than "." should perhaps
		   be conditional on recognize_only_executables? */
		exec_check = 1;
		dir_ok = 0;
	    } else
		next_command++;
	}

	if (command == SPELL) {	/* correct the spelling of the last bit */
	    if (name_length == 0) { /* zero-length word can't be misspelled */
		extended_name[0] = '\0'; /* (not trying is important for ~) */
		d = 0;
		break;
	    }
	    nd = spdist(entry, target); /* test the entry against original */
	    if (nd <= d && nd != 4) {
		if (exec_check && !executable(tilded_dir, entry, dir_ok))
		    continue;
		(void) Strcpy (extended_name, entry);
		d = nd;
		if (d == 0)	/* if found it exactly */
		    break;
	    } else if (nd == 4) {
                if (spdir(extended_name, tilded_dir, entry, target)) {
		    if (exec_check &&
			!executable(tilded_dir, extended_name, dir_ok))
			continue;
                    d = 0;
                    break;
                }
            }
	} else if (command == LIST) { /* LIST command */
    	    register int length;
	    register long i;
	    register Char **ni, **p2;

	    if (!is_prefix (target, entry))
		continue;
	    if (exec_check && !executable(tilded_dir, entry, dir_ok))
		continue;

	    if (items == NULL || maxitems == 0) {
    		items = (Char **) xalloc ((size_t) (sizeof (items[0]) * 
				          (ITEMS_START+1)));
		maxitems = ITEMS_START;
		for (i = 0, p2 = items; i < maxitems; i++)
		    *p2++ = NULL;
	    } else if (numitems >= maxitems) {
#ifdef REALLOC_BROKEN
		ni = (Char **) xalloc((size_t) ((sizeof (items[0])) *
				     (maxitems + ITEMS_INCR)));
		for (i = 0, p1 = items, p2 = ni; i < numitems; i++)
		    *p2++ = *p1++;
		for (; i < numitems+ITEMS_INCR; i++)
		    *p2++ = NULL;
		xfree ((ptr_t) items);
		items = ni;
#else /* REALLOC_BROKEN */
		ni = (Char **) xralloc((ptr_t) items, (size_t)
			      (sizeof (items[0])) * (maxitems + ITEMS_INCR));
		items = ni;
#endif /* REALLOC_BROKEN */
		maxitems += ITEMS_INCR;
	    }


    	    length = Strlen(entry) + 1;
    	    if (showpathn)
    		length += Strlen(dirflag);
	    if (!looking_for_lognames && !looking_for_shellvar)
		length++;

	    /* safety check */
    	    items[numitems] = (Char *) xalloc ((size_t) (length*sizeof(Char)));

	    copyn (items[numitems], entry, MAXNAMLEN);

	    if (!looking_for_lognames && !looking_for_shellvar
		&& !(looking_for_command && !looking_for_file)) {
		Char typestr[2];

		typestr[0] = filetype(tilded_dir,entry);
		typestr[1] = '\0';
		catn (items[numitems], typestr, MAXNAMLEN);
	    }

    	    if (showpathn)
    	        catn (items[numitems], dirflag, MAXNAMLEN);
    	    numitems++;
    	} else {					/* RECOGNIZE command */
	    if (!is_prefix (target, entry))
		continue;
	    if (exec_check && !executable(tilded_dir, entry, dir_ok))
		continue;

	    if ( ignoring && ignored( entry ) ) {
		nignored++;
		continue;
	    }
	    if (is_set(STRrecexact)) {
		if (StrQcmp (target, entry) == 0) {	/* EXACT match */
	            copyn (extended_name, entry, MAXNAMLEN);
		    numitems = 1;		/* fake into expanding */
		    break;
		}
	    }
    	    if (recognize (extended_name, entry, name_length, ++numitems))
    		break;
	}
    }

    if (ignoring && numitems == 0 && nignored > 0) {
	ignoring = 0;
	nignored = 0;
	if (looking_for_lognames)
	    (void) setpwent();
	else
	    rewinddir(dir_fd);
	goto again;
    }
    if (looking_for_lognames) {
#ifdef YPBUGS
	fix_yp_bugs();
#endif /* YPBUGS */
	(void) endpwent ();
    } else if (looking_for_file || looking_for_shellvar ||
               (looking_for_command && relatives_in_path)) {
	if (dir_fd != NULL)
	    FREE_DIR (dir_fd);
    }

    if (command == RECOGNIZE) {
	if (numitems > 0) {
	    if (looking_for_lognames)
		copyn (word, STRtilde, 1);
	    else if (looking_for_shellvar) {
		Char *ptr = Strrchr(word, '$');
		*++ptr = '\0';	/* Delete after the dollar */
	    }
	    else if (looking_for_file)
		copyn (word, dir, max_word_length); /* put back dir part */
	    else
		word[0] = '\0';
	    catn (word, extended_name, max_word_length); /* add extended name */
	    if (is_set(STRaddsuffix)) {
		if (numitems == 1) {
		    if (looking_for_lognames) {	/* add / */
			catn (word, STRslash, max_word_length);
		    } else if (looking_for_shellvar) {
			struct varent *vp = adrof(extended_name);
			Char *stp;
			/*
			 * Don't consider array variables or empty variables
			 */
			if (vp) {
			    if (!(stp = vp->vec[0]) || vp->vec[0][0] == '\0' ||
			         vp->vec[1]) {
				catn (word, STRspace, max_word_length);
				stp = (Char *) 0;
			    }
			    else
				stp = vp->vec[0];
			}
			else if ((stp = Getenv(extended_name)) == (Char *) 0)
			    catn (word, STRspace, max_word_length);
			if (stp != (Char *) 0) {
			    *--target = '\0';
			    (void) Strcat(tilded_dir, name);
			    if (isadirectory(tilded_dir, stp))
				catn (word, STRslash, max_word_length);
			    else
				catn (word, STRspace, max_word_length);
			}
		    } else if (looking_for_file) {
			if (isadirectory (tilded_dir, extended_name)) {
			    catn (word, STRslash, max_word_length);
			} else {
			    catn (word, STRspace, max_word_length);
			}
		    } else {	/* prob. looking for a command */
			    catn (word, STRspace, max_word_length);
		    }
		}
	    }
	}
	return (numitems);		        /* at the end */
    } else if (command == LIST) {
	register int max_items = 0;
	register Char *cp;

	if (cp = value(STRlistmax)) {
	    while (*cp) {
		if (!isdigit(*cp)) {
		    max_items = 0;
		    break;
		}
		max_items = max_items * 10 + *cp++ - '0';
	    }
	}

	if ((max_items > 0) && (numitems > max_items) && list_max) {
	    char tc;
	    CSHprintf("There are %d items, list them anyway? [n/y] ", numitems);
	    flush();
	    /* We should be in Rawmode here, so no \n to catch */
	    (void) read(SHIN, &tc, 1);
	    CSHprintf("%c\r\n", tc);	/* echo the char, do a newline */
	    if ((tc != 'y') && (tc != 'Y'))
		goto done_list;
	}
	qsort ((ptr_t) items, (size_t) numitems, sizeof (items[1]), fcompare);

	print_by_column (STRNULL, items, numitems, TRUE);
	
  done_list:
	if (items != NULL)
	    FREE_ITEMS (items, numitems);
	return (numitems);
    } else if (command == SPELL) {
	if (looking_for_lognames)
	    copyn (word, STRtilde, 1);
	else if (looking_for_shellvar) {
	    Char *ptr = Strrchr(word, '$');
	    *++ptr = '\0';	/* Delete after the dollar */
	}
	else if (looking_for_file)
	    copyn (word, dir, max_word_length);	       /* put back dir part */
	else
	    word[0] = '\0';
	catn (word, extended_name, max_word_length);   /* add extended name */
	return(d);
    }
    else {
	CSHprintf("Bad tw_command\n");
	return(0);
    }
}


/* stuff for general command line hacking */

/*
 * Strip next directory from path; return ptr to next unstripped directory.
 */
 
#ifdef notdef
Char *extract_dir_from_path (path, dir)
Char *path, dir[];
{
    register Char *d = dir;

    while (*path && (*path == ' ' || *path == ':')) path++;
    while (*path && (*path != ' ' && *path != ':')) *(d++) = *(path++);
    while (*path && (*path == ' ' || *path == ':')) path++;

    ++dirctr;
    if (*dir == '.')
        (void) Strcpy (dirflag, STRdotsp);
    else
    {
        dirflag[0] = ' ';
	if (dirctr <= 9)
	{
		dirflag[1] = '0' + dirctr;
		dirflag[2] = '\0';
	}
	else
	{
		dirflag[1] = '0' + dirctr / 10;
		dirflag[2] = '0' + dirctr % 10;
		dirflag[3] = '\0';
	}
    }
    *(d++) = '/';
    *d = 0;

    return path;
}
#endif


static void
free_items (items, numitems)
register Char **items;
register numitems;
{
    register int i;
/*     for (i = 0; items[i] != (Char *)NULL; i++) */
    for (i = 0; i < numitems; i++)
	xfree ((ptr_t) items[i]);
    xfree ((ptr_t) items);
    maxitems = 0;
}


/*
 * parse full path in file into 2 parts: directory and file names
 * Should leave final slash (/) at end of dir.
 */
static void
extract_dir_and_name (path, dir, name)
Char   *path, *dir, *name;
{
    register Char  *p;
    p = Strrchr(path, '/');
    if (p == NULL)
    {
	copyn (name, path, MAXNAMLEN);
	dir[0] = '\0';
    }
    else
    {
	p++;
	copyn (name, p, MAXNAMLEN);
	copyn (dir, path, p - path);
    }
}

static Char *
getentry (dir_fd, looking_for_lognames)
DIR *dir_fd;
int looking_for_lognames;
{
    register struct passwd *pw;
    static Char retname[MAXPATHLEN];

    register struct dirent *dirp;

    if (looking_for_lognames) {		/* Is it login names we want? */

	pw = getpwent();

	if (pw == NULL) {
#ifdef YPBUGS
	    fix_yp_bugs();
#endif
	    return (NULL);
	}
	(void) Strcpy(retname, str2short(pw->pw_name));
	return (retname);
    } else {				/* It's a dir entry we want */
	if (dirp = readdir (dir_fd)) {
	    (void) Strcpy(retname, str2short(dirp->d_name));
	    return (retname);
	}
	return (NULL);
    }
}

/*
 * expand "/$old1/$old2/old3/"
 * to "/value_of_old1/value_of_old2/old3/"
 */
static Char *
dollar(new, old)
Char *new, *old;
{
    Char *var, *val, *p, save;
    int space;
    for (space = FILSIZ, p = new; *old && space > 0; )
	if (*old != '$') {
	    *p++ = *old++;
	    space--;
	}
	else {
	    struct varent *vp;
	    /* found a variable, expand it */
	    for (var = ++old; alnum(*old); old++);
	    save = *old;
	    *old = '\0';
	    vp = adrof(var);
	    val = (!vp) ? Getenv(var) : (Char *) 0;
	    *old = save;
	    /*
	     * Don't expand array variables
	     */
	    if (vp) {
		if (!vp->vec[0] || vp->vec[1]) {
		    *new = '\0';
		    return((Char *) 0);
		}
		else 
		    val = vp->vec[0];
	    }
	    else if (!val) {
		*new = '\0';
		return((Char *) 0);
	    }
	    for (; space > 0 && *val; space--)
		*p++ = *val++;
	}
    *p = '\0';
    return(new);
}
	    
/*
 * expand "old" file name with possible tilde usage
 *		~person/mumble
 * expands to
 *		home_directory_of_person/mumble
 * into string "new".
 */

static Char *
tilde (new, old)
Char *new, *old;
{
    register Char *o, *p, *hp;
    register struct passwd *pw;
    static Char person[40] = {0};

    if ((old[0] != '~') && (old[0] != '='))
    {
	(void) Strcpy (new, old);
	return (new);
    }

    for (p = person, o = &old[1]; *o && *o != '/'; *p++ = *o++);
    *p = '\0';

    if (old[0] == '~') {
	if (person[0] == '\0') {		/* then use current uid */
	    /* PWP: Bugfix for the bug reported by Matthias Hobohm, 
	     * Universitaet Erlangen, Informatik 4, West-Germany: changing
	     * 
	     * $home didn't effect the meaning of ~ in filename completion.
	     * This code used to look up the value from passwd, even though
	     * we knew it all along.  Now it does it right.
	     */
	    hp = value(STRhome);

	    if (hp == NULL)
		return (NULL);

	    (void) Strcpy (new, hp);
	} else {
	    pw = getpwnam (short2str(person));
#ifdef YPBUGS
	    fix_yp_bugs();
#endif

	    if (pw == NULL)
		return (NULL);

	    (void) Strcpy (new, str2short(pw->pw_dir));
	}
    } else {					/* '=' stack expansion */
        new[0] = '\0';
        if (!isdigit (old[1]) && old[1] != '-')
	    return (NULL);
        getstakd (new, (old[1] == '-') ? -1 : old[1] - '0', 0);
					/* last "0" = don't call error */
        if (new[0] == '\0')
	    return (NULL);
    }
    (void) Strcat (new, o);
    return (new);
}

static Char
filetype (dir, file)		/* symbology from 4.3 ls command */
Char *dir, *file;
{
    if (dir)
    {
	Char path[512];
	char *ptr;
	struct stat statb;
	(void) Strcpy (path, dir);
	catn (path, file, sizeof(path) / sizeof(Char));

	if (lstat (ptr = short2str(path), &statb) != -1) 
					/* see above #define of lstat */
	{
#ifdef S_IFLNK
	    if ((statb.st_mode & S_IFMT) == S_IFLNK) { /* Symbolic link */
		if (stat (ptr, &statb) == -1)
		    return ('&');
		else if ((statb.st_mode & S_IFMT) == S_IFDIR) 
		    return ('>');
		else
		    return ('@');
	    }
#endif
#ifdef S_IFSOCK
	    if ((statb.st_mode & S_IFMT) == S_IFSOCK)	/* Socket */
		return ('=');
#endif
#ifdef S_IFIFO
	    if ((statb.st_mode & S_IFMT) == S_IFIFO)	/* Named Pipe */
		return ('|');
#endif
	    if ((statb.st_mode & S_IFMT) == S_IFCHR) /* char device */
		return ('%');
	    if ((statb.st_mode & S_IFMT) == S_IFBLK) /* block device */
		return ('#');
	    if ((statb.st_mode & S_IFMT) == S_IFDIR) /* normal Directory */
		return ('/');
	    if (statb.st_mode & 0111)
		return ('*');
	}
    }
    return (' ');
}

static int
isadirectory (dir, file)	/* return 1 if dir/file is a directory */
Char *dir, *file;		/* uses stat rather than lstat to get dest. */
{
    if (dir)
    {
	Char path[MAXPATHLEN];
	struct stat statb;
	(void) Strcpy (path, dir);
	catn (path, file, sizeof(path) / sizeof(Char));
	if (stat (short2str(path), &statb) >= 0) /* resolve through symlink */
	{
#ifdef S_IFSOCK
	    if ((statb.st_mode & S_IFMT) == S_IFSOCK)	/* Socket */
		return 0;
#endif
#ifdef S_IFIFO
	    if ((statb.st_mode & S_IFMT) == S_IFIFO)	/* Named Pipe */
		return 0;
#endif
	    if ((statb.st_mode & S_IFMT) == S_IFDIR) /* normal Directory */
		return 1;
	}
    }
    return 0;
}

/*
 * Print sorted down columns
 */
void
print_by_column (dir, items, count, no_file_suffix)
register Char *dir, *items[];
int count, no_file_suffix;
{
    register int i, r, c, w, maxwidth = 0, columns, rows;

    lbuffed = 0;		/* turn off line buffering */

    for (i = 0; i < count; i++)	/* find widest string */
	maxwidth = max(maxwidth, Strlen (items[i]));

    maxwidth += no_file_suffix ? 1:2;    /* for the file tag and space */
    columns = (TermH+1) / maxwidth; /* PWP: terminal size change */
    if (!columns) columns = 1;
    rows = (count + (columns - 1)) / columns;

    for (r = 0; r < rows; r++) {
	for (c = 0; c < columns; c++) {
	    i = c * rows + r;

	    if (i < count) {
		w = Strlen (items[i]);

		if (no_file_suffix) {
		    /* Print the command name */
		    CSHprintf("%s", short2str(items[i]));
		} else {
		    /* Print filename followed by '/' or '*' or ' ' */
		    CSHprintf("%s%c", short2str(items[i]), 
			filetype (dir, items[i]));
		    w++;
		}

		if (c < (columns - 1))			/* Not last column? */
		    for (; w < maxwidth; w++)
			CSHputchar (' ');
	    }
	}
	CSHputchar ('\r');
	CSHputchar ('\n');
    }

    lbuffed = 1;		/* turn back on line buffering */
    flush();
}


int
StrQcmp(str1, str2)
register Char *str1, *str2;
{
    for (; *str1 && (*str1 & TRIM) == (*str2 & TRIM); str1++, str2++);
    /*
     * The following case analysis is necessary so that characters
     * which look negative collate low against normal characters but
     * high against the end-of-string NUL.
     */
    if (*str1 == '\0' && *str2 == '\0')
	return(0);
    else if (*str1 == '\0')
	return(-1);
    else if (*str2 == '\0')
	return(1);
    else
	return((*str1 & TRIM) - (*str2 & TRIM));
}

/*
 * For qsort()
 */
int
fcompare (file1, file2)
Char  **file1, **file2;
{
#if defined(NLS) && !defined(NOSTRCOLL)
    char buf[2048];

    (void) strcpy(buf, short2str(*file1));
    return((int) strcoll(buf, short2str(*file2)));
#else
    return(StrQcmp(*file1, *file2));
#endif
}

/*
 * Concatenate src onto tail of des.
 * Des is a string whose maximum length is count.
 * Always null terminate.
 */

void
catn (des, src, count)
register Char *des, *src;
register count;
{
    while (--count >= 0 && *des)
	des++;
    while (--count >= 0)
	if ((*des++ = *src++) == 0)
	    return;
    *des = '\0';
}

static int
max (a, b)
int a, b;
{
    if (a > b)
	return (a);
    return (b);
}

/*
 * like strncpy but always leave room for trailing \0
 * and always null terminate.
 */
void
copyn (des, src, count)
register Char *des, *src;
register count;
{
    while (--count >= 0)
	if ((*des++ = *src++) == 0)
	    return;
    *des = '\0';
}

static void
tw_get_comm_list()
{				/* stolen from sh.exec.c dohash() */
    register DIR *dirp;
    register struct dirent *dp;
    register Char *dir;
    register Char **pv;
    struct varent *v = adrof(STRpath);

    relatives_in_path = 0;	/* set to false until we know better */
    tw_clear_comm_list();
    if (v == 0)			/* if no path */
	return;

    if (adrof(STRrecognize_only_executables)) {
	for (pv = v->vec; *pv; pv++) {
	    if (pv[0][0] != '/') {
		relatives_in_path = 1;
		continue;
	    }
	    dirp = opendir(short2str(*pv));
	    if (dirp == NULL)
		continue;

	    dir = Strspl(*pv, STRslash);
	    while ((dp = readdir(dirp)) != NULL) {
		/* the call to executable() may make this a bit slow */
		if (dp->d_ino != 0 &&
		    executable(dir, str2short(dp->d_name), 0))
		    tw_add_comm_name(str2short(dp->d_name));
	    }
	    (void) closedir(dirp);
	    xfree((ptr_t) dir);
	}
    }
    else {
	for (pv = v->vec; *pv; pv++) {
	    if (pv[0][0] != '/') {
		relatives_in_path = 1;
		continue;
	    }
	    dirp = opendir(short2str(*pv));
	    if (dirp == NULL)
		continue;

	    while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_ino != 0)
		    tw_add_comm_name(str2short(dp->d_name));
	    }
	    (void) closedir(dirp);
	}
    }
}
