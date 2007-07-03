/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

    $Id: cmd.c,v 1.69 2007-07-01 04:34:02 qqshka Exp $
*/

#include "quakedef.h"
#ifdef WITH_TCL
#include "embed_tcl.h"
#endif
#ifndef SERVERONLY
#ifdef GLQUAKE
#include "gl_model.h"
#include "gl_local.h"
#else
#include "r_model.h"
#include "r_local.h"
#endif /* !GLQUAKE */
#include "teamplay.h"
#include "rulesets.h"
#include "tp_triggers.h"
#endif /* !SERVERONLY */
#include "parser.h"

#ifndef SERVERONLY
qbool CL_CheckServerCommand (void);
#endif

static void Cmd_ExecuteStringEx (cbuf_t *context, char *text);
static int gtf = 0; // global trigger flag

cvar_t cl_warncmd = {"cl_warncmd", "0"};
cvar_t cl_oldif = {"cl_oldif", "0"};

cbuf_t cbuf_main;
#ifndef SERVERONLY
cbuf_t cbuf_svc;
cbuf_t cbuf_safe, cbuf_formatted_comms;
#endif

cbuf_t *cbuf_current = NULL;

//=============================================================================

//Causes execution of the remainder of the command buffer to be delayed until next frame.
//This allows commands like: bind g "impulse 5 ; +attack ; wait ; -attack ; impulse 2"
void Cmd_Wait_f (void)
{
#ifdef WITH_TCL
	if (in_tcl) {
			Com_Printf ("command wait cant be used with TCL\n");
			return;
	}
#endif
	if (cbuf_current)
		cbuf_current->wait = true;

	return;
}

/*
=============================================================================
						COMMAND BUFFER
=============================================================================
*/

void Cbuf_AddText (const char *text)
{
	Cbuf_AddTextEx (&cbuf_main, text);
}

void Cbuf_InsertText (const char *text)
{
	Cbuf_InsertTextEx (&cbuf_main, text);
}

void Cbuf_Execute (void)
{
	Cbuf_ExecuteEx (&cbuf_main);
#ifndef SERVERONLY
	Cbuf_ExecuteEx (&cbuf_safe);
	Cbuf_ExecuteEx (&cbuf_formatted_comms);
#endif
}

//fuh : ideally we should have 'cbuf_t *Cbuf_Register(int maxsize, int flags, qbool (*blockcmd)(void))
//fuh : so that cbuf_svc and cbuf_safe can be registered outside cmd.c in cl_* .c
//fuh : flags can be used to deal with newline termination etc for cbuf_svc, and *blockcmd can be used for blocking cmd's for cbuf_svc
//fuh : this way cmd.c would be independant of '#ifdef CLIENTONLY's'.
//fuh : I'll take care of that one day.
static void Cbuf_Register (cbuf_t *cbuf, int maxsize)
{
	assert (!host_initialized);
	cbuf->maxsize = maxsize;
	cbuf->text_buf = (char *) Hunk_Alloc(maxsize);
	cbuf->text_start = cbuf->text_end = (cbuf->maxsize >> 1);
	cbuf->wait = false;
}

void Cbuf_Init (void)
{
	Cbuf_Register (&cbuf_main, 1 << 18); // 256kb
#ifndef SERVERONLY
	Cbuf_Register (&cbuf_svc, 1 << 13); // 8kb
	Cbuf_Register (&cbuf_safe, 1 << 11); // 2kb
	Cbuf_Register (&cbuf_formatted_comms, 1 << 11); // 2kb
#endif
}

//Adds command text at the end of the buffer
void Cbuf_AddTextEx (cbuf_t *cbuf, const char *text)
{
	int new_start, new_bufsize;
	size_t len;

	len = strlen (text);

	if (cbuf->text_end + len <= cbuf->maxsize) {
		memcpy (cbuf->text_buf + cbuf->text_end, text, len);
		cbuf->text_end += len;
		return;
	}

	new_bufsize = cbuf->text_end-cbuf->text_start+len;
	if (new_bufsize > cbuf->maxsize) {
		Com_Printf ("Cbuf_AddText: overflow\n");
		return;
	}

	// Calculate optimal position of text in buffer
	new_start = ((cbuf->maxsize - new_bufsize) >> 1);

	memcpy (cbuf->text_buf + new_start, cbuf->text_buf + cbuf->text_start, cbuf->text_end-cbuf->text_start);
	memcpy (cbuf->text_buf + new_start + cbuf->text_end-cbuf->text_start, text, len);
	cbuf->text_start = new_start;
	cbuf->text_end = cbuf->text_start + new_bufsize;
}

//Adds command text at the beginning of the buffer
void Cbuf_InsertTextEx (cbuf_t *cbuf, const char *text)
{
	int new_start, new_bufsize;
	size_t len;

	len = strlen (text);

	if (len <= cbuf->text_start) {
		memcpy (cbuf->text_buf + (cbuf->text_start - len), text, len);
		cbuf->text_start -= len;
		return;
	}

	new_bufsize = cbuf->text_end - cbuf->text_start + len;
	if (new_bufsize > cbuf->maxsize) {
		Com_Printf ("Cbuf_InsertText: overflow\n");
		return;
	}

	// Calculate optimal position of text in buffer
	new_start = ((cbuf->maxsize - new_bufsize) >> 1);

	memmove (cbuf->text_buf + (new_start + len), cbuf->text_buf + cbuf->text_start, cbuf->text_end - cbuf->text_start);
	memcpy (cbuf->text_buf + new_start, text, len);
	cbuf->text_start = new_start;
	cbuf->text_end = cbuf->text_start + new_bufsize;
}

#define MAX_RUNAWAYLOOP 1000

void Cbuf_ExecuteEx (cbuf_t *cbuf)
{
	int i, j, cursize, nextsize;
	char *text, line[1024], *src, *dest;
	qbool comment, quotes;

#ifndef SERVERONLY
	if (cbuf == &cbuf_safe)
		gtf++;

	nextsize = cbuf->text_end - cbuf->text_start;
#endif

	while (cbuf->text_end > cbuf->text_start) {
		// find a \n or ; line break
		text = (char *) cbuf->text_buf + cbuf->text_start;

		cursize = cbuf->text_end - cbuf->text_start;
		comment = quotes = false;

		for (i = 0; i < cursize; i++) {
			if (text[i] == '\n')
				break;

			if (text[i] == '"') {
				quotes = !quotes;
				continue;
			}

			if (comment || quotes)
				continue;

			if (text[i] == '/' && i + 1 < cursize && text[i + 1] == '/')
				comment = true;
			else if (text[i] == ';')
				break;
		}

#ifndef SERVERONLY
		if ((cursize - i) < nextsize) // have we reached the next command?
			nextsize = cursize - i;

		// don't execute lines without ending \n; this fixes problems with
		// partially stuffed aliases not being executed properly

		if (cbuf_current == &cbuf_svc && i == cursize)
			break;
#endif

		// Copy text to line, skipping carriage return chars
		src = text;
		dest = line;
		j = min (i, sizeof (line) - 1);
		for ( ; j; j--, src++) {
			if (*src != '\r')
				*dest++ = *src;
		}
		*dest = 0;

		// delete the text from the command buffer and move remaining commands down  This is necessary
		// because commands (exec, alias) can insert data at the beginning of the text buffer
		if (i == cursize) {
			cbuf->text_start = cbuf->text_end = (cbuf->maxsize >> 1);
		} else {
			i++;
			cbuf->text_start += i;
		}

		cursize = cbuf->text_end - cbuf->text_start;

		Cmd_ExecuteStringEx (cbuf, line);	// execute the command line

		if (cbuf->text_end - cbuf->text_start > cursize)
			cbuf->runAwayLoop++;

		if (cbuf->runAwayLoop > MAX_RUNAWAYLOOP) {
			Com_Printf("\x02" "A recursive alias has caused an infinite loop.");
			Com_Printf("\x02" " Clearing execution buffer to prevent lockup.\n");
			cbuf->text_start = cbuf->text_end = (cbuf->maxsize >> 1);
			cbuf->runAwayLoop = 0;
		}

		if (cbuf->wait) {
			// skip out while text still remains in buffer, leaving it for next frame
			cbuf->wait = false;
#ifndef SERVERONLY
			cbuf->runAwayLoop += Q_rint (0.5 * cls.frametime * MAX_RUNAWAYLOOP);

			if (cbuf == &cbuf_safe)
				gtf--;
#endif
			return;
		}
	}

#ifndef SERVERONLY
	if (cbuf == &cbuf_safe)
		gtf--;
#endif

	cbuf->runAwayLoop = 0;

	return;
}

/*
==============================================================================
						SCRIPT COMMANDS
==============================================================================
*/

/*
Set commands are added early, so they are guaranteed to be set before
the client and server initialize for the first time.
 
Other commands are added late, after all initialization is complete.
*/
void Cbuf_AddEarlyCommands (void)
{
	int i;

	for (i = 0; i < COM_Argc () - 2; i++) {
		if (strcasecmp (COM_Argv(i), "+set"))
			continue;

		Cbuf_AddText (va ("set %s %s\n", COM_Argv (i + 1), COM_Argv (i + 2)));
		i += 2;
	}
}


/*
Adds command line parameters as script statements
Commands lead with a +, and continue until a - or another +
quake +prog jctest.qp +cmd amlev1
quake -nosound +cmd amlev1
*/
void Cmd_StuffCmds_f (void)
{
	int k, len = 0;
	char *s, *text, *token;

	// build the combined string to parse from
	for (k = 1; k < com_argc; k++)
		len += strlen (com_argv[k]) + 1;

	if (!len)
		return;

	text = (char *) Q_malloc (len + 1);
	for (k = 1; k < com_argc; k++) {
		strcat (text, com_argv[k]);
		if (k != com_argc - 1)
			strcat (text, " ");
	}

	// pull out the commands
	token = (char *) Q_malloc (len + 1);

	s = text;
	while (*s) {
		if (*s == '+')	{
			k = 0;
			for (s = s + 1; s[0] && (s[0] != ' ' || (s[1] != '-' && s[1] != '+')); s++)
				token[k++] = s[0];
			token[k++] = '\n';
			token[k] = 0;
			if (strncasecmp(token, "set ", 4))
				Cbuf_AddText (token);
		} else if (*s == '-') {
			for (s = s + 1; s[0] && s[0] != ' '; s++)
				;
		} else {
			s++;
		}
	}

	Q_free (text);
	Q_free (token);
}

void Cmd_Exec_f (void)
{
	char *f, name[MAX_OSPATH];
	int mark;

	if (Cmd_Argc () != 2) {
		Com_Printf ("exec <filename> : execute a script file\n");
		return;
	}

	strlcpy (name, Cmd_Argv(1), sizeof(name) - 4);
	mark = Hunk_LowMark();
	if (!(f = (char *) FS_LoadHunkFile (name)))	{
		char *p;
		p = COM_SkipPath (name);
		if (!strchr (p, '.')) {
			// no extension, so try the default (.cfg)
			strcat (name, ".cfg");
			f = (char *) FS_LoadHunkFile (name);
		}
		if (!f) {
			Com_Printf ("couldn't exec %s\n", Cmd_Argv(1));
			return;
		}
	}
	if (cl_warncmd.value || developer.value)
		Com_Printf ("execing %s\n", name);

#ifndef SERVERONLY
	if (cbuf_current == &cbuf_svc) {
		Cbuf_AddTextEx (&cbuf_main, f);
		Cbuf_AddTextEx (&cbuf_main, "\n");
	} else
#endif
	{
		Cbuf_InsertText ("\n");
		Cbuf_InsertText (f);
	}
	Hunk_FreeToLowMark (mark);
}

//Just prints the rest of the line to the console
/*void Cmd_Echo_f (void) {
	int i;
 
	for (i = 1; i < Cmd_Argc(); i++)
		Com_Printf ("%s ", Cmd_Argv(i));
	Com_Printf ("\n");
}*/
void Cmd_Echo_f (void)
{
#ifdef SERVERONLY
	Com_Printf ("%s\n",Cmd_Args());
#else
	int	i;
	char *str;
	char args[MAX_MACRO_STRING];
	char buf[MAX_MACRO_STRING];

	memset (args, 0, MAX_MACRO_STRING);

	snprintf (args, MAX_MACRO_STRING, "%s", Cmd_Argv(1));

	for (i = 2; i < Cmd_Argc(); i++) {
		strlcat (args, " ", MAX_MACRO_STRING);
		strlcat (args, Cmd_Argv(i), MAX_MACRO_STRING);
	}

	//	str = TP_ParseMacroString(args);

	str = TP_ParseMacroString(args);
	str = TP_ParseFunChars(str, false);

	strlcpy (buf, str, MAX_MACRO_STRING);

	CL_SearchForReTriggers (buf, RE_PRINT_ECHO); 	// BorisU
	Print_flags[Print_current] |= PR_TR_SKIP;
	Com_Printf ("%s\n", buf);
#endif
}

/*
=============================================================================
								ALIASES
=============================================================================
*/
#define ALIAS_HASHPOOL_SIZE 256
cmd_alias_t *cmd_alias_hash[ALIAS_HASHPOOL_SIZE];
cmd_alias_t	*cmd_alias;

cmd_alias_t *Cmd_FindAlias (char *name)
{
	int key;
	cmd_alias_t *alias;

	key = Com_HashKey (name) % ALIAS_HASHPOOL_SIZE;
	for (alias = cmd_alias_hash[key]; alias; alias = alias->hash_next) {
		if (!strcasecmp(name, alias->name))
			return alias;
	}
	return NULL;
}

char *Cmd_AliasString (char *name)
{
	int key;
	cmd_alias_t *alias;

	key = Com_HashKey (name) % ALIAS_HASHPOOL_SIZE;
	for (alias = cmd_alias_hash[key]; alias; alias = alias->hash_next) {
		if (!strcasecmp(name, alias->name))
#ifdef WITH_TCL
			if (!(alias->flags & ALIAS_TCL))
#endif
				return alias->value;
	}
	return NULL;
}

void Cmd_Viewalias_f (void)
{
	cmd_alias_t	*alias;
	char		*name;
	int		i,m;

	if (Cmd_Argc() < 2) {
		Com_Printf ("viewalias <cvar> [<cvar2>..] : view body of alias\n");
		return;
	}

	for (i=1; i<Cmd_Argc(); i++) {
		name = Cmd_Argv(i);


		if ( IsRegexp(name) ) {
			if (!ReSearchInit(name))
				return;
			Com_Printf ("Current alias commands:\n");

			for (alias = cmd_alias, i=m=0; alias ; alias=alias->next, i++)
				if (ReSearchMatch(alias->name)) {
#ifdef WITH_TCL
					if (alias->flags & ALIAS_TCL)
						Com_Printf ("%s : Tcl procedure\n", alias->name);
					else
#endif
						Com_Printf ("%s : %s\n", alias->name, alias->value);
					m++;
				}

			Com_Printf ("------------\n%i/%i aliases\n", m, i);
			ReSearchDone();


		} else 	{
			if ((alias = Cmd_FindAlias(name)))
#ifdef WITH_TCL
				if (alias->flags & ALIAS_TCL)
					Com_Printf ("%s : Tcl procedure\n", name);
				else
#endif
					Com_Printf ("%s : \"%s\"\n", Cmd_Argv(i), alias->value);
			else
				Com_Printf ("No such alias: %s\n", Cmd_Argv(i));
		}
	}
}


int Cmd_AliasCompare (const void *p1, const void *p2)
{
	cmd_alias_t *a1, *a2;

	a1 = *((cmd_alias_t **) p1);
	a2 = *((cmd_alias_t **) p2);

	if (a1->name[0] == '+') {
		if (a2->name[0] == '+')
			return strcasecmp(a1->name + 1, a2->name + 1);
		else
			return -1;
	} else if (a1->name[0] == '-') {
		if (a2->name[0] == '+')
			return 1;
		else if (a2->name[0] == '-')
			return strcasecmp(a1->name + 1, a2->name + 1);
		else
			return -1;
	} else if (a2->name[0] == '+' || a2->name[0] == '-') {
		return 1;
	} else {
		return strcasecmp(a1->name, a2->name);
	}
}

void Cmd_AliasList_f (void)
{
	cmd_alias_t *a;
	int i, c, m = 0;
	static int count;
	static cmd_alias_t *sorted_aliases[2048];

#define MAX_SORTED_ALIASES (sizeof(sorted_aliases) / sizeof(sorted_aliases[0]))

	for (a = cmd_alias, count = 0; a && count < MAX_SORTED_ALIASES; a = a->next, count++)
		sorted_aliases[count] = a;
	qsort(sorted_aliases, count, sizeof (cmd_alias_t *), Cmd_AliasCompare);

	if (count == MAX_SORTED_ALIASES)
		assert(!"count == MAX_SORTED_ALIASES");

	c = Cmd_Argc();
	if (c>1)
		if (!ReSearchInit(Cmd_Argv(1)))
			return;

	Com_Printf ("List of aliases:\n");
	for (i = 0; i < count; i++) {
		a = sorted_aliases[i];
		if (c==1 || ReSearchMatch(a->name)) {
			Com_Printf ("\x02%s :", sorted_aliases[i]->name);
			Com_Printf (" %s\n\n", sorted_aliases[i]->value);
			m++;
		}
	}

	if (c>1)
		ReSearchDone();
	Com_Printf ("------------\n%i/%i aliases\n", m, count);
}





void Cmd_EditAlias_f (void)
{
#define		MAXCMDLINE	256
	extern wchar	key_lines[32][MAXCMDLINE];
	extern int		edit_line;
	cmd_alias_t	*a;
	char *s, final_string[MAXCMDLINE -1];
	int c;
	extern void Key_ClearTyping();

	c = Cmd_Argc();
	if (c == 1)	{
		Com_Printf ("%s <name> : modify an alias\n", Cmd_Argv(0));
		Com_Printf ("aliaslist : list all aliases\n");
		return;
	}

	a = Cmd_FindAlias(Cmd_Argv(1));
	if ( a == NULL ) {
		s = Q_strdup ("");
	} else {
		s = Q_strdup(a->value);
	}

	snprintf(final_string, sizeof(final_string), "/alias \"%s\" \"%s\"", Cmd_Argv(1), s);
	Key_ClearTyping();
	memcpy (key_lines[edit_line]+1, str2wcs(final_string), strlen(final_string)*sizeof(wchar));
	Q_free(s);
}



//Creates a new command that executes a command string (possibly ; separated)
void Cmd_Alias_f (void)
{
	cmd_alias_t	*a;
	char *s;
	int c, key;

	c = Cmd_Argc();
	if (c == 1)	{
		Com_Printf ("%s <name> <command> : create or modify an alias\n", Cmd_Argv(0));
		Com_Printf ("aliaslist : list all aliases\n");
		return;
	}

	s = Cmd_Argv(1);
	if (strlen(s) >= MAX_ALIAS_NAME) {
		Com_Printf ("Alias name is too long\n");
		return;
	}

	key = Com_HashKey(s) % ALIAS_HASHPOOL_SIZE;

	// if the alias already exists, reuse it
	for (a = cmd_alias_hash[key]; a; a = a->hash_next) {
		if (!strcasecmp(a->name, s)) {
			Q_free (a->value);
			break;
		}
	}

	if (!a)	{
		a = (cmd_alias_t *) Q_malloc (sizeof(cmd_alias_t));
		a->next = cmd_alias;
		cmd_alias = a;
		a->hash_next = cmd_alias_hash[key];
		cmd_alias_hash[key] = a;
	}
	strcpy (a->name, s);

	a->flags = 0;
	// QW262 -->
	s=Cmd_MakeArgs(2);
	while (*s) {
		if (*s == '%' && ( s[1]>='0' || s[1]<='9')) {
			a->flags |= ALIAS_HAS_PARAMETERS;
			break;
		}
		++s;
	}
	// <-- QW262
	if (!strcasecmp(Cmd_Argv(0), "aliasa"))
		a->flags |= ALIAS_ARCHIVE;

#ifndef SERVERONLY
	if (cbuf_current == &cbuf_svc)
		a->flags |= ALIAS_SERVER;
	if (!strcasecmp(Cmd_Argv(0), "tempalias"))
		a->flags |= ALIAS_TEMP;
#endif

	// copy the rest of the command line
	a->value = Q_strdup (Cmd_MakeArgs(2));
}

qbool Cmd_DeleteAlias (char *name)
{
	cmd_alias_t *a, *prev;
	int key;

	key = Com_HashKey (name) % ALIAS_HASHPOOL_SIZE;

	prev = NULL;
	for (a = cmd_alias_hash[key]; a; a = a->hash_next) {
		if (!strcasecmp(a->name, name)) {
			// unlink from hash
			if (prev)
				prev->hash_next = a->hash_next;
			else
				cmd_alias_hash[key] = a->hash_next;
			break;
		}
		prev = a;
	}

	if (!a)
		return false;	// not found

	prev = NULL;
	for (a = cmd_alias; a; a = a->next) {
		if (!strcasecmp(a->name, name)) {
			// unlink from alias list
			if (prev)
				prev->next = a->next;
			else
				cmd_alias = a->next;

			// free
			Q_free (a->value);
			Q_free (a);
			return true;
		}
		prev = a;
	}

	assert(!"Cmd_DeleteAlias: alias list broken");
	return false; // shut up compiler
}

void Cmd_UnAlias (qbool use_regex)
{
	int 		i;
	char		*name;
	cmd_alias_t	*a, *next;
	qbool		re_search = false;

	if (Cmd_Argc() < 2) {
		Com_Printf ("unalias <cvar> [<cvar2>..]: erase an existing alias\n");
		return;
	}

	for (i=1; i<Cmd_Argc(); i++) {
		name = Cmd_Argv(i);

		if (use_regex && (re_search = IsRegexp(name)))
			if(!ReSearchInit(name))
				continue;

		if (strlen(name) >= MAX_ALIAS_NAME) {
			Com_Printf ("Alias name is too long: \"%s\"\n", Cmd_Argv(i));
			continue;
		}

		if (use_regex && re_search) {
			for (a = cmd_alias; a; a = next) {
				next = a->next;

				if (ReSearchMatch(a->name))
					Cmd_DeleteAlias(a->name);
			}
		} else {
			if (!Cmd_DeleteAlias(Cmd_Argv(i)))
				Com_Printf ("unalias: unknown alias \"%s\"\n", Cmd_Argv(i));
		}

		if (use_regex && re_search)
			ReSearchDone();

	}
}

void Cmd_UnAlias_f (void)
{
	Cmd_UnAlias(false);
}

void Cmd_UnAlias_re_f (void)
{
	Cmd_UnAlias(true);
}

// remove all aliases
void Cmd_UnAliasAll_f (void)
{
	cmd_alias_t	*a, *next;

	for (a = cmd_alias; a ; a = next) {
		next = a->next;
		Q_free (a->value);
		Q_free (a);
	}
	cmd_alias = NULL;

	// clear hash
	memset (cmd_alias_hash, 0, sizeof(cmd_alias_t*) * ALIAS_HASHPOOL_SIZE);
}




void DeleteServerAliases(void)
{
	cmd_alias_t *a, *next;

	for (a = cmd_alias; a; a = next) {
		next = a->next;

		if (a->flags & ALIAS_SERVER)
			Cmd_DeleteAlias (a->name);
	}
}



void Cmd_WriteAliases (FILE *f)
{
	cmd_alias_t	*a;

	for (a = cmd_alias ; a ; a=a->next)
		if (a->flags & ALIAS_ARCHIVE)
			fprintf (f, "aliasa %s \"%s\"\n", a->name, a->value);
}

/*
=============================================================================
					LEGACY COMMANDS
=============================================================================
*/

typedef struct legacycmd_s
{
	char *oldname, *newname;
	struct legacycmd_s *next;
} legacycmd_t;

static legacycmd_t *legacycmds = NULL;

void Cmd_AddLegacyCommand (char *oldname, char *newname)
{
	legacycmd_t *cmd;
	cmd = (legacycmd_t *) Q_malloc (sizeof(legacycmd_t));
	cmd->next = legacycmds;
	legacycmds = cmd;

	cmd->oldname = oldname;
	cmd->newname = newname;
}

qbool Cmd_IsLegacyCommand (char *oldname)
{
	legacycmd_t *cmd;

	for (cmd = legacycmds; cmd; cmd = cmd->next) {
		if (!strcasecmp(cmd->oldname, oldname))
			return true;
	}
	return false;
}

static qbool Cmd_LegacyCommand (void)
{
	qbool recursive = false;
	legacycmd_t *cmd;
	char text[1024];

	for (cmd = legacycmds; cmd; cmd = cmd->next) {
		if (!strcasecmp(cmd->oldname, Cmd_Argv(0)))
			break;
	}
	if (!cmd)
		return false;

	if (!cmd->newname[0])
		return true;		// just ignore this command

	// build new command string
	strlcpy (text, cmd->newname, sizeof(text));
	strlcat (text, " ", sizeof(text));
	strlcat (text, Cmd_Args(), sizeof(text));

	assert (!recursive);
	recursive = true;
	Cmd_ExecuteString (text);
	recursive = false;

	return true;
}

/*
=============================================================================
					COMMAND EXECUTION
=============================================================================
*/

#define	MAX_ARGS		80

static	int		cmd_argc;
static	char	*cmd_argv[MAX_ARGS];
static	char	*cmd_null_string = "";
static	char	*cmd_args = NULL;

#define CMD_HASHPOOL_SIZE 512
cmd_function_t	*cmd_hash_array[CMD_HASHPOOL_SIZE];
/*static*/ cmd_function_t	*cmd_functions;		// possible commands to execute

int Cmd_Argc (void)
{
	return cmd_argc;
}

char *Cmd_Argv (int arg)
{
	if (arg >= cmd_argc)
		return cmd_null_string;
	return cmd_argv[arg];
}

//Returns a single string containing argv(1) to argv(argc() - 1)
char *Cmd_Args (void)
{
	if (!cmd_args)
		return "";
	return cmd_args;
}

//Returns a single string containing argv(start) to argv(argc() - 1)
//Unlike Cmd_Args, shrinks spaces between argvs
char *Cmd_MakeArgs (int start)
{
	int i, c;

	static char	text[1024];

	text[0] = 0;
	c = Cmd_Argc();
	for (i = start; i < c; i++) {
		if (i > start)
			strncat (text, " ", sizeof(text) - strlen(text) - 1);
		strncat (text, Cmd_Argv(i), sizeof(text) - strlen(text) - 1);
	}

	return text;
}

//Parses the given string into command line tokens.
void Cmd_TokenizeString (char *text)
{
	int idx;
	static char argv_buf[1024];

	idx = 0;

	cmd_argc = 0;
	cmd_args = NULL;

	while (1) {
		// skip whitespace
		while (*text == ' ' || *text == '\t' || *text == '\r')
			text++;

		if (*text == '\n') {	// a newline separates commands in the buffer
			text++;
			break;
		}

		if (!*text)
			return;

		if (cmd_argc == 1)
			cmd_args = text;

		text = COM_Parse (text);
		if (!text)
			return;

		if (cmd_argc < MAX_ARGS) {
			cmd_argv[cmd_argc] = argv_buf + idx;
			strcpy (cmd_argv[cmd_argc], com_token);
			idx += strlen(com_token) + 1;
			cmd_argc++;
		}
	}
}

void Cmd_AddCommand (char *cmd_name, xcommand_t function)
{
	cmd_function_t *cmd;
	int	key;

	/* commented out when vid_restart was added
	if (host_initialized)	// because hunk allocation would get stomped
		assert (!"Cmd_AddCommand after host_initialized");
	*/

/*	// fail if the command is a variable name
	if (Cvar_FindVar(cmd_name)) {
		Com_Printf ("Cmd_AddCommand: %s already defined as a var\n", cmd_name);
		return;
	} */

	key = Com_HashKey (cmd_name) % CMD_HASHPOOL_SIZE;

	// fail if the command already exists
	for (cmd = cmd_hash_array[key]; cmd; cmd=cmd->hash_next) {
		if (!strcasecmp (cmd_name, cmd->name)) {
			Com_Printf ("Cmd_AddCommand: %s already defined\n", cmd_name);
			return;
		}
	}

	cmd = (cmd_function_t *) Hunk_Alloc (sizeof(cmd_function_t));
	cmd->name = cmd_name;
	cmd->function = function;
	cmd->next = cmd_functions;
	cmd_functions = cmd;
	cmd->hash_next = cmd_hash_array[key];
	cmd_hash_array[key] = cmd;
}

qbool Cmd_Exists (char *cmd_name)
{
	int	key;
	cmd_function_t	*cmd;

	key = Com_HashKey (cmd_name) % CMD_HASHPOOL_SIZE;
	for (cmd=cmd_hash_array[key]; cmd; cmd = cmd->hash_next) {
		if (!strcasecmp (cmd_name, cmd->name))
			return true;
	}
	return false;
}

cmd_function_t *Cmd_FindCommand (const char *cmd_name)
{
	int	key;
	cmd_function_t *cmd;

	key = Com_HashKey (cmd_name) % CMD_HASHPOOL_SIZE;
	for (cmd = cmd_hash_array[key]; cmd; cmd = cmd->hash_next) {
		if (!strcasecmp (cmd_name, cmd->name))
			return cmd;
	}
	return NULL;
}

char *Cmd_CompleteCommand (char *partial)
{
	cmd_function_t *cmd;
	int len;
	cmd_alias_t *alias;

	len = strlen(partial);

	if (!len)
		return NULL;

	// check for exact match
	for (cmd = cmd_functions; cmd; cmd = cmd->next)
		if (!strcasecmp (partial, cmd->name))
			return cmd->name;
	for (alias = cmd_alias; alias; alias = alias->next)
		if (!strcasecmp (partial, alias->name))
			return alias->name;

	// check for partial match
	for (cmd = cmd_functions; cmd; cmd = cmd->next)
		if (!strncasecmp (partial, cmd->name, len))
			return cmd->name;
	for (alias = cmd_alias; alias; alias = alias->next)
		if (!strncasecmp (partial, alias->name, len))
			return alias->name;

	return NULL;
}

int Cmd_CompleteCountPossible (char *partial)
{
	cmd_function_t *cmd;
	int len, c = 0;

	len = strlen(partial);
	if (!len)
		return 0;

	for (cmd = cmd_functions; cmd; cmd = cmd->next)
		if (!strncasecmp (partial, cmd->name, len))
			c++;

	return c;
}

int Cmd_AliasCompleteCountPossible (char *partial)
{
	cmd_alias_t *alias;
	int len, c = 0;

	len = strlen(partial);
	if (!len)
		return 0;

	for (alias = cmd_alias; alias; alias = alias->next)
		if (!strncasecmp (partial, alias->name, len))
			c++;

	return c;
}

int Cmd_CommandCompare (const void *p1, const void *p2)
{
	return strcmp((*((cmd_function_t **) p1))->name, (*((cmd_function_t **) p2))->name);
}

void Cmd_CmdList (qbool use_regex)
{
	static cmd_function_t *sorted_cmds[512];
	int i, c, m = 0, count;
	cmd_function_t *cmd;
	char *pattern;
	qbool forwarded_only = !strcmp(">", Cmd_Argv(1)); // "cmdlist >" mean show only commands forwarded to server

#define MAX_SORTED_CMDS (sizeof (sorted_cmds) / sizeof (sorted_cmds[0]))

	for (cmd = cmd_functions, count = 0; cmd && count < MAX_SORTED_CMDS; cmd = cmd->next)
	{
		if (forwarded_only && cmd->function)
			continue; // if function is NULL then this is forwarded to server command

		sorted_cmds[count] = cmd;
		count++;
	}
	qsort (sorted_cmds, count, sizeof (cmd_function_t *), Cmd_CommandCompare);

	if (count == MAX_SORTED_CMDS)
		assert(!"count == MAX_SORTED_CMDS");

	pattern = (!forwarded_only && Cmd_Argc() > 1) ? Cmd_Argv(1) : NULL;

	if (use_regex && ((c = Cmd_Argc()) > 1))
		if (!ReSearchInit(Cmd_Argv(1)))
			return;

	Com_Printf ("List of commands:\n");
	for (i = 0; i < count; i++) {
		cmd = sorted_cmds[i];
		if (use_regex) {
			if (!(c == 1 || ReSearchMatch (cmd->name)))
				continue;
		} else {
			if (pattern && !Q_glob_match (pattern, cmd->name))
				continue;
		}

		Com_Printf ("%s\n", cmd->name);
		m++;
	}

	if (use_regex && (c > 1))
		ReSearchDone ();

	Com_Printf ("------------\n%i/%i commands\n", m,count);
}

void Cmd_CmdList_f (void)
{
	Cmd_CmdList (false);
}

void Cmd_CmdList_re_f (void)
{
	Cmd_CmdList (true);
}

#define MAX_MACROS 64

static macro_command_t macro_commands[MAX_MACROS];
static int macro_count = 0;

void Cmd_ReInitAllMacro (void)
{
	int i;
	int teamplay;

	teamplay = (int) Rulesets_RestrictTriggers ();

	for (i = 0; i < macro_count; i++)
		if (macro_commands[i].teamplay != MACRO_NORULES)
			macro_commands[i].teamplay = teamplay;
}

void Cmd_AddMacroEx (const char *s, char *(*f) (void), int teamplay)
{
	if (macro_count == MAX_MACROS)
		Sys_Error ("Cmd_AddMacro: macro_count == MAX_MACROS");

	snprintf (macro_commands[macro_count].name, sizeof (macro_commands[macro_count].name), "%s", s);
	macro_commands[macro_count].func = f;
	macro_commands[macro_count].teamplay = teamplay;

#ifdef WITH_TCL
// disconnect: it seems macro are safe with TCL NOW
//	if (!teamplay)	// don't allow teamplay protected macros since there's no protection for this in TCL yet
		TCL_RegisterMacro (macro_commands + macro_count);
#endif

	macro_count++;
}

void Cmd_AddMacro (const char *s, char *(*f) (void))
{
	Cmd_AddMacroEx (s, f, MACRO_NORULES);
}

char *Cmd_MacroString (const char *s, int *macro_length)
{
	int i;
	macro_command_t	*macro;

	for (i = 0; i < macro_count; i++) {
		macro = &macro_commands[i];
		if (!strncasecmp (s, macro->name, strlen (macro->name))) {
#ifndef SERVERONLY
			if (cbuf_current == &cbuf_main && (macro->teamplay == MACRO_DISALLOWED))
				cbuf_current = &cbuf_formatted_comms;
#endif
			*macro_length = strlen (macro->name);
			return macro->func();
		}
		macro++;
	}

	*macro_length = 0;

	return NULL;
}

static int Cmd_MacroCompare (const void *p1, const void *p2)
{
	return strcmp ((*((macro_command_t **) p1))->name, (*((macro_command_t **) p2))->name);
}

void Cmd_MacroList_f (void)
{
	int i, c, m = 0;
	static macro_command_t *sorted_macros[MAX_MACROS];

	for (i = 0; i < macro_count; i++)
		sorted_macros[i] = &macro_commands[i];
	qsort (sorted_macros, macro_count, sizeof (macro_command_t *), Cmd_MacroCompare);

	if (macro_count == MAX_MACROS)
		assert(!"count == MAX_MACROS");

	c = Cmd_Argc();
	if (c > 1)
		if (!ReSearchInit (Cmd_Argv (1)))
			return;

	Com_Printf ("List of macros:\n");
	for (i = 0; i < macro_count; i++) {
		if (c==1 || ReSearchMatch (sorted_macros[i]->name)) {
			Com_Printf ("$%s\n", sorted_macros[i]->name);
			m++;
		}
	}

	if (c > 1)
		ReSearchDone();

	Com_Printf ("------------\n%i/%i macros\n", m, macro_count);
}



//Expands all $cvar expressions to cvar values
//If not SERVERONLY, also expands $macro expressions
//Note: dest must point to a 1024 byte buffer
void Cmd_ExpandString (const char *data, char *dest)
{
	unsigned int c;
	char buf[255], *str;
	int i, len = 0, quotes = 0, name_length = 0;
	cvar_t *var, *bestvar;
#ifndef SERVERONLY
	int macro_length;
#endif

	while ((c = *data)) {
		if (c == '"')
			quotes++;

		if (c == '$' && !(quotes & 1)) {
			data++;

			// Copy the text after '$' to a temp buffer
			i = 0;
			buf[0] = 0;
			bestvar = NULL;
			while ((c = *data) > 32) {
				if (c == '$')
					break;

				data++;
				buf[i++] = c;
				buf[i] = 0;

				if ((var = Cvar_FindVar (buf)))
					bestvar = var;

				if (i >= (int) sizeof (buf) - 1)
					break; // there no more space in buf
			}

#ifndef SERVERONLY
			if (!dedicated) {
				str = Cmd_MacroString (buf, &macro_length);
				name_length = macro_length;

				if (bestvar && (!str || (strlen (bestvar->name) > macro_length))) {
					str = bestvar->string;
					name_length = strlen(bestvar->name);
                    if (bestvar->teamplay)
                        cbuf_current = &cbuf_formatted_comms;
				}
			} else
#endif
			{
				if (bestvar) {
					str = bestvar->string;
					name_length = strlen (bestvar->name);
                    if (bestvar->teamplay)
                        cbuf_current = &cbuf_formatted_comms;
                } else {
					str = NULL;
				}
			}

			if (str) {
				// check buffer size
				if (len + strlen (str) >= 1024 - 1)
					break;

				strcpy (&dest[len], str);
				len += strlen (str);
				i = name_length;
				while (buf[i])
					dest[len++] = buf[i++];
			} else {
				// no matching cvar or macro
				dest[len++] = '$';
				if (len + strlen (buf) >= 1024 - 1)
					break;

				strcpy (&dest[len], buf);
				len += strlen (buf);
			}
		} else {
			dest[len] = c;
			data++;
			len++;
			if (len >= 1024 - 1)
				break;
		}
	}

	dest[len] = 0;
}

int Commands_Compare_Func (const void * arg1, const void * arg2)
{
	return strcasecmp (*(char**) arg1, *(char**) arg2);
}
char *msgtrigger_commands[] = {
                                  "play", "playvol", "stopsound", "set", "echo", "say", "say_team",
                                  "alias", "unalias", "msg_trigger", "inc", "bind", "unbind", "record",
                                  "easyrecord", "stop", "if", "if_exists", "wait", "log", "match_forcestart",
                                  "dns", "addserver", "connect", "join", "observe",
                                  "tcl_proc", "tcl_exec", "tcl_eval", "exec",
                                  "set_ex", "set_alias_str", "set_bind_str","unset", "unset_re" ,
                                  "toggle", "toggle_re", "set_calc", "rcon", "user", "users",
                                  "unalias", "unalias_re",
                                  "re_trigger", "re_trigger_options", "re_trigger_delete",
                                  "re_trigger_enable","re_trigger_disable", "re_trigger_match",
                                  "hud262_add","hud262_remove","hud262_position","hud262_bg",
                                  "hud262_move","hud262_width","hud262_alpha","hud262_blink",
                                  "hud262_disable","hud262_enable","hud262_list","hud262_bringtofront",
                                  "hud_262font","hud262_hover","hud262_button"
                                  //               ,NULL
                              };

char *formatted_comms_commands[] = {
                                       "if", "wait", "echo", "say", "say_team", "set_tp",
                                       "tp_point", "tp_pickup", "tp_took",
                                       NULL
                                   };

float	impulse_time = -9999;
int		impulse_counter;

#ifndef SERVERONLY
qbool AllowedImpulse(int imp)
{

	static int Allowed_TF_Impulses[] = {
	                                   135, 99, 101, 102, 103, 104, 105, 106, 107, 108, 109, 23, 144, 145,
	                                   159, 160, 161, 162, 163, 164, 165, 166, 167
	                               };

	int i;

	if (!cl.teamfortress) return false;
	for (i=0; i<sizeof(Allowed_TF_Impulses)/sizeof(Allowed_TF_Impulses[0]); i++) {
		if (Allowed_TF_Impulses[i] == imp) {
			if(++impulse_counter >= 30) {
				if (cls.realtime < impulse_time + 5 && !cls.demoplayback) {
					return false;
				}
				impulse_time = cls.realtime;
				impulse_counter = 0;
			}
			return true;
		}
	}
	return false;
}

static qbool Cmd_IsCommandAllowedInMessageTrigger( const char *command )
{
	if( !strcasecmp( command, "impulse") )
		return AllowedImpulse(Q_atoi(Cmd_Argv(1)));

	return 	  bsearch( &(command), msgtrigger_commands,
	                   sizeof(msgtrigger_commands)/sizeof(msgtrigger_commands[0]),
	                   sizeof(msgtrigger_commands[0]),Commands_Compare_Func) != NULL;
}
static qbool Cmd_IsCommandAllowedInTeamPlayMacros( const char *command )
{
	char **s;
	for (s = formatted_comms_commands; *s; s++) {
		if (!strcasecmp(command, *s))
			break;
	}
	return *s != NULL;
}
#endif /* SERVERONLY */

//A complete command line has been parsed, so try to execute it
static void Cmd_ExecuteStringEx (cbuf_t *context, char *text)
{
	cvar_t *v;
	cmd_function_t *cmd;
	cmd_alias_t *a;
	static char buf[1024];
	cbuf_t *inserttarget, *oldcontext;
	char *p, *n, *s;
	char text_exp[1024];

	oldcontext = cbuf_current;
	cbuf_current = context;

#ifndef SERVERONLY
	Cmd_ExpandString (text, text_exp);
	Cmd_TokenizeString (text_exp);
#else
	Cmd_TokenizeString (text);
#endif

	if (!Cmd_Argc())
		goto done; // no tokens

#ifndef SERVERONLY
	if (cbuf_current == &cbuf_svc) {
		if (CL_CheckServerCommand())
			goto done;
	}
#endif

	// check functions
	if ((cmd = Cmd_FindCommand(cmd_argv[0]))) {
#ifndef SERVERONLY
		if (gtf || cbuf_current == &cbuf_safe) {
			if (!Cmd_IsCommandAllowedInMessageTrigger(cmd_argv[0])) {
				Com_Printf ("\"%s\" cannot be used in message triggers\n", cmd_argv[0]);
				goto done;
			}
		} else if ((cbuf_current == &cbuf_formatted_comms)) {
			if (!Cmd_IsCommandAllowedInTeamPlayMacros(cmd_argv[0])) {
				Com_Printf ("\"%s\" cannot be used in combination with teamplay $macros\n", cmd_argv[0]);
				goto done;
			}
		}
#endif

		if (cmd->function)
			cmd->function();
		else
			Cmd_ForwardToServer ();
		goto done;
	}

	// some bright guy decided to use "skill" as a mod command in Custom TF, sigh
	if (!strcmp(Cmd_Argv(0), "skill") && cmd_argc == 1 && Cmd_FindAlias("skill"))
		goto checkaliases;

	// check cvars
	if ((v = Cvar_FindVar (Cmd_Argv(0)))) {
#ifndef SERVERONLY
		if ((cbuf_current == &cbuf_formatted_comms)) {
			Com_Printf ("\"%s\" cannot be used in combination with teamplay $macros\n", cmd_argv[0]);
			goto done;
		}
#endif
		if (Cvar_Command())
			goto done;
	}

	// check aliases
checkaliases:
	if ((a = Cmd_FindAlias(cmd_argv[0]))) {

		// QW262 -->
#ifdef WITH_TCL
		if (a->flags & ALIAS_TCL)
		{
			TCL_ExecuteAlias (a);
			goto done;
		}
#endif

		if (a->value[0]=='\0') goto done; // alias is empty.

		if(a->flags & ALIAS_HAS_PARAMETERS) { // %parameters are given in alias definition
			s=a->value;
			buf[0] = '\0';
			do {
				n = strchr(s, '%');
				if(n) {
					if(*++n >= '1' && *n <= '9') {
						n[-1] = 0;
						strlcat(buf, s, sizeof(buf));
						n[-1] = '%';
						// insert numbered parameter
						strlcat(buf,Cmd_Argv(*n-'0'), sizeof(buf));
					} else if (*n == '0') {
						n[-1] = 0;
						strlcat(buf, s, sizeof(buf));
						n[-1] = '%';
						// insert all parameters
						strlcat(buf, Cmd_Args(), sizeof(buf));
					} else if (*n == '%') {
						n[0] = 0;
						strlcat(buf, s, sizeof(buf));
						n[0] = '%';
					} else {
						if (*n) {
							char tmp = n[1];
							n[1] = 0;
							strlcat(buf, s, sizeof(buf));
							n[1] = tmp;
						} else
							strlcat(buf, s, sizeof(buf));
					}
					s=n+1;
				}
			} while(n);
			strlcat(buf, s, sizeof(buf));
			p = buf;

		} else  // alias has no parameters
			p = a->value;
		// <-- QW262

#ifndef SERVERONLY
		if (cbuf_current == &cbuf_svc)
		{
			Cbuf_AddText (p);
			Cbuf_AddText ("\n");
		} else
#endif
		{

#ifdef SERVERONLY
			inserttarget = &cbuf_main;
#else
			inserttarget = cbuf_current ? cbuf_current : &cbuf_main;
#endif

			Cbuf_InsertTextEx (inserttarget, "\n");

			// if the alias value is a command or cvar and
			// the alias is called with parameters, add them
			if (Cmd_Argc() > 1 && !strchr(p, ' ') && !strchr(p, '\t') &&
			        (Cvar_FindVar(p) || (Cmd_FindCommand(p) && p[0] != '+' && p[0] != '-'))
			   ) {
				Cbuf_InsertTextEx (inserttarget, Cmd_Args());
				Cbuf_InsertTextEx (inserttarget, " ");
			}
			Cbuf_InsertTextEx (inserttarget, p);
		}
		goto done;
	}

#ifndef SERVERONLY
	if (Cmd_LegacyCommand())
		goto done;
#endif

	if (!host_initialized && Cmd_Argc() > 1) {
		if (Cvar_CreateTempVar())
			goto done;
	}

#ifndef SERVERONLY
	if (cbuf_current != &cbuf_svc)
#endif
	{
		if (cl_warncmd.value || developer.value)
			Com_Printf ("Unknown command \"%s\"\n", Cmd_Argv(0));
	}

done:
	cbuf_current = oldcontext;
}

void Cmd_ExecuteString (char *text)
{
	Cmd_ExecuteStringEx (NULL, text);
}

#ifndef SERVERONLY
static qbool is_numeric (char *c)
{
	return ( isdigit((int)(unsigned char)*c) ||
	         ((*c == '-' || *c == '+') && (c[1] == '.' || isdigit((int)(unsigned char)c[1]))) ||
	         (*c == '.' && isdigit((int)(unsigned char)c[1])) );
}

void Re_Trigger_Copy_Subpatterns (const char *s, int* offsets, int num, cvar_t *re_sub); // QW262
extern cvar_t re_sub[10]; // QW262

void Cmd_CatchTriggerSubpatterns(const char *s, int* offsets, int num)
{
	Re_Trigger_Copy_Subpatterns(s, offsets, min(num, 10), re_sub);
}

// this is a test replacement of the "if" command
void Cmd_If_New(void)
{
	// syntax of this command has two possibilities:
	// if (<expr>) then <cmd1> [else <cmd2>]
	// if o1 o2 o3 [then] <cmd1> [else <cmd2>]
	// the second one is for backward compatibility
	parser_extra pars_ex;
	int c;
	int then_pos = 0;
	qbool then_found = false, else_found = false;
	int i, clen;
	size_t expr_len = 0;
	char* expr, * curarg;
	int result, error;
	char buf[1024];
	qbool addquot_1 = false, addquot_3 = false;

	pars_ex.subpatt_fnc = Cmd_CatchTriggerSubpatterns;
	pars_ex.var2val_fnc = NULL;

	c = Cmd_Argc();

	// 0  1 2    3
	// if e then c
	if (c < 4) {
		Com_Printf("Usage: if <expr> then <cmds> [else <cmds>]\n");
		return;
	}

	for (i = 2; i < c; i++) {
		if (!strcmp(Cmd_Argv(i), "then")) {
			then_pos = i; then_found = true; break;
		}
	}

	if (!then_pos) then_pos = 4;

	if (then_pos == 4 && Cmd_Argv(1)[0] != '(')
	{	// backward compatibility patch: most configs contain "<a> isin <b>", where
		// one of the strings can get wrongly interpretted as some non-string token in the parser
		// so if we have 3 arguments long expression and it is not enclosed in parentheses,
		// we force the operands be enclosed in quotes (') to make sure they get recognized as strings
		if (Cmd_Argv(1)[0] != '\'') addquot_1 = true;
		if (Cmd_Argv(3)[0] != '\'') addquot_3 = true;
	}

	for (i = 1; i < then_pos; i++) {
		clen = strlen(Cmd_Argv(i));
		expr_len += clen ? clen + 1 : 3; // we will take '' as a representation of an empty string
		if (i == 1 && addquot_1) expr_len += 2;
		if (i == 3 && addquot_3) expr_len += 2;
	}

	expr = (char *) Q_malloc(expr_len+1);
	expr[0] = '\0';

	for (i = 1; i < then_pos; i++) {
		if (i > 1) strcat(expr, " ");
		curarg = Cmd_Argv(i);
		if (*curarg)
		{
			if ((i == 1 && addquot_1) || (i == 3 && addquot_3)) strlcat(expr, "'", expr_len);
			strlcat(expr, curarg, expr_len);
			if ((i == 1 && addquot_1) || (i == 3 && addquot_3)) strlcat(expr, "'", expr_len);
		}
		else strlcat(expr, "''", expr_len);
	}

	error = Expr_Eval_Bool(expr, &pars_ex, &result);
	if (error != EXPR_EVAL_SUCCESS) {
		Com_Printf("Error in condition: %s (\"%s\")\n", Parser_Error_Description(error), expr);
		free(expr);
		return;
	}
	free(expr);

	if (then_found) then_pos++;	// skin "then"

	buf[0] = '\0';
	if (result)	// true case
	{		
		for (i = then_pos; i < c; i++) {
			if (!else_found && !strcmp(Cmd_Argv(i), "else")) break;
			if (buf[0])
				strlcat (buf, " ", sizeof(buf));
			strlcat (buf, Cmd_Argv(i), sizeof(buf));
		}
	}
	else // result = false
	{
		for (i = then_pos; i < c; i++) {
			if (else_found) {
				if (buf[0])
					strlcat (buf, " ", sizeof(buf));
				strlcat (buf, Cmd_Argv(i), sizeof(buf));
			}
			if (!else_found && !strcmp(Cmd_Argv(i), "else")) else_found = true;
		}
	}

	strlcat (buf, "\n", sizeof(buf));
	Cbuf_InsertTextEx (cbuf_current ? cbuf_current : &cbuf_main, buf);
}

void Cmd_If_Old (void)
{
	int	i, c;
	char *op, buf[1024] = {0};
	qbool result;

	if ((c = Cmd_Argc()) < 5) {
		Com_Printf ("Usage: if <expr1> <op> <expr2> <command> [else <command>]\n");
		return;
	}

	op = Cmd_Argv(2);
	if (!strcmp(op, "==") || !strcmp(op, "=") || !strcmp(op, "!=") || !strcmp(op, "<>")) {
		if (is_numeric(Cmd_Argv(1)) && is_numeric(Cmd_Argv(3)))
			result = Q_atof(Cmd_Argv(1)) == Q_atof(Cmd_Argv(3));
		else
			result = !strcmp(Cmd_Argv(1), Cmd_Argv(3));

		if (op[0] != '=')
			result = !result;
	} else if (!strcmp(op, ">")) {
		result = Q_atof(Cmd_Argv(1)) > Q_atof(Cmd_Argv(3));
	} else if (!strcmp(op, "<")) {
		result = Q_atof(Cmd_Argv(1)) < Q_atof(Cmd_Argv(3));
	} else if (!strcmp(op, ">=")) {
		result = Q_atof(Cmd_Argv(1)) >= Q_atof(Cmd_Argv(3));
	} else if (!strcmp(op, "<=")) {
		result = Q_atof(Cmd_Argv(1)) <= Q_atof(Cmd_Argv(3));

	} else if (!strcmp(op, "isin")) {
		result = (strstr(Cmd_Argv(3), Cmd_Argv(1)) ? 1 : 0);
	} else if (!strcmp(op, "!isin")) {
		result = (strstr(Cmd_Argv(3), Cmd_Argv(1)) ? 0 : 1);

	} else if (!strcmp(op, "=~") || !strcmp(op, "!~")) {
		pcre*		regexp;
		const char	*error;
		int		error_offset;
		int		rc;
		int		offsets[99];

		regexp = pcre_compile (Cmd_Argv(3), 0, &error, &error_offset, NULL);
		if (!regexp) {
			Com_Printf ("Error in regexp: %s\n", error);
			return;
		}
		rc = pcre_exec (regexp, NULL, Cmd_Argv(1), strlen(Cmd_Argv(1)),
		                0, 0, offsets, 99);
		if (rc >= 0) {
			Re_Trigger_Copy_Subpatterns (Cmd_Argv(1), offsets, min(rc, 10), re_sub);
			result = true;
		} else
			result = false;

		if (op[0] != '=')
			result = !result;

		pcre_free (regexp);
	} else {
		Com_Printf ("unknown operator: %s\n", op);
		Com_Printf ("valid operators are ==, =, !=, <>, >, <, >=, <=, isin, !isin, =~, !~\n");
		return;
	}

	if (result)	{
		for (i = 4; i < c; i++) {
			if ((i == 4) && !strcasecmp(Cmd_Argv(i), "then"))
				continue;
			if (!strcasecmp(Cmd_Argv(i), "else"))
				break;
			if (buf[0])
				strncat (buf, " ", sizeof(buf) - strlen(buf) - 2);
			strncat (buf, Cmd_Argv(i), sizeof(buf) - strlen(buf) - 2);
		}
	} else {
		for (i = 4; i < c ; i++) {
			if (!strcasecmp(Cmd_Argv(i), "else"))
				break;
		}
		if (i == c)
			return;
		for (i++; i < c; i++) {
			if (buf[0])
				strncat (buf, " ", sizeof(buf) - strlen(buf) - 2);
			strncat (buf, Cmd_Argv(i), sizeof(buf) - strlen(buf) - 2);
		}
	}

	strncat (buf, "\n", sizeof(buf) - strlen(buf) - 1);
	Cbuf_InsertTextEx (cbuf_current ? cbuf_current : &cbuf_main, buf);
}

void Cmd_If_f(void) {
	if (cl_oldif.value) Cmd_If_Old();
	else				Cmd_If_New();
}

void Cmd_If_Exists_f(void)
{
	int	argc;
	char	*type;
	char	*name;
	qbool	exists;
	qbool	iscvar, isalias, istrigger, ishud;

	argc = Cmd_Argc();
	if ( argc < 4 || argc > 5) {
		Com_Printf ("if_exists <type> <name> <cmd1> [<cmd2>] - conditional execution\n");
		return;
	}

	type = Cmd_Argv(1);
	name = Cmd_Argv(2);
	if ( ( (iscvar = !strcmp(type, "cvar")) && Cvar_FindVar (name) )			||
	        ( (isalias = !strcmp(type, "alias")) && Cmd_FindAlias (name) )			||
#ifndef SERVERONLY
	        ( (istrigger = !strcmp(type, "trigger")) && CL_FindReTrigger (name) )	||
	        ( (ishud = !strcmp(type, "hud")) && Hud_FindElement (name) ) )
#endif
		exists = true;
	else {
		exists = false;
		if (!(iscvar || isalias || istrigger || ishud)) {
			Com_Printf("if_exists: <type> can be cvar, alias, trigger, hud\n");
			return;
		}
	}

	if (exists) {
		Cbuf_InsertTextEx (cbuf_current ? cbuf_current : &cbuf_main,"\n");
		Cbuf_InsertTextEx (cbuf_current ? cbuf_current : &cbuf_main,Cmd_Argv(3));
	} else if (argc == 5) {
		Cbuf_InsertTextEx (cbuf_current ? cbuf_current : &cbuf_main,"\n");
		Cbuf_InsertTextEx (cbuf_current ? cbuf_current : &cbuf_main,Cmd_Argv(4));
	} else
		return;
}

void Cmd_Eval_f(void)
{
	int errn;
	expr_val value;

	if (Cmd_Argc() != 2) {
		Com_Printf("Usage: eval <expression>\n"
			"Prints the value of given expression after evaluation in the internal parser\n");
		return;
	}

	value = Expr_Eval(Cmd_Argv(1), NULL, &errn);

	if (errn != EXPR_EVAL_SUCCESS)
	{
		Com_Printf("Error occured: %s\n", Parser_Error_Description(errn));
		return;
	}
	else
	{
		switch (value.type) {
		case ET_INT:  Com_Printf("Result: %i (integer)\n", value.i_val); break;
		case ET_DBL:  Com_Printf("Result: %f (double)\n",  value.d_val); break;
		case ET_BOOL: Com_Printf("Result: %s (bool)\n", value.b_val ? "true" : "false"); break;
		case ET_STR:  Com_Printf("Result: (string)\n\"%s\"\n", value.s_val); free(value.s_val); break;
		default:      Com_Printf("Error: Unknown value type\n"); break;
		}
	}
}

#endif /* SERVERONLY */

void Cmd_Init (void)
{
	// register our commands
	Cmd_AddCommand ("exec", Cmd_Exec_f);
	Cmd_AddCommand ("echo", Cmd_Echo_f);
	Cmd_AddCommand ("aliaslist", Cmd_AliasList_f);
	Cmd_AddCommand ("aliasedit", Cmd_EditAlias_f);
	//Cmd_AddCommand ("aliasa", Cmd_Alias_f);
	Cmd_AddCommand ("alias", Cmd_Alias_f);
	Cmd_AddCommand ("tempalias", Cmd_Alias_f);
	Cmd_AddCommand ("viewalias", Cmd_Viewalias_f);
	Cmd_AddCommand ("unaliasall", Cmd_UnAliasAll_f);
	Cmd_AddCommand ("unalias", Cmd_UnAlias_f);
	Cmd_AddCommand ("unalias_re", Cmd_UnAlias_re_f);
	Cmd_AddCommand ("wait", Cmd_Wait_f);
	Cmd_AddCommand ("cmdlist", Cmd_CmdList_f);
	Cmd_AddCommand ("cmdlist_re", Cmd_CmdList_re_f);
#ifndef SERVERONLY
	Cmd_AddCommand ("if", Cmd_If_f);
	Cmd_AddCommand ("if_exists", Cmd_If_Exists_f);
	Cmd_AddCommand ("eval", Cmd_Eval_f);
#endif

	Cvar_SetCurrentGroup(CVAR_GROUP_CONSOLE);
	Cvar_Register(&cl_oldif);
	Cvar_ResetCurrentGroup();

	Cmd_AddCommand ("macrolist", Cmd_MacroList_f);
	qsort(msgtrigger_commands,
	      sizeof(msgtrigger_commands)/sizeof(msgtrigger_commands[0]),
	      sizeof(msgtrigger_commands[0]),Commands_Compare_Func);
}