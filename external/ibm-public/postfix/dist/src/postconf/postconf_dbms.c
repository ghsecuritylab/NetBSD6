/*	$NetBSD: postconf_dbms.c,v 1.1.1.1 2013/01/02 18:59:03 tron Exp $	*/

/*++
/* NAME
/*	postconf_dbms 3
/* SUMMARY
/*	legacy support for database-defined main.cf parameter names
/* SYNOPSIS
/*	#include <postconf.h>
/*
/*	void	register_dbms_parameters(param_value, flag_parameter,
/*					local_scope)
/*	const char *param_value;
/*	const char *(flag_parameter) (const char *, int, char *);
/*	PC_MASTER_ENT *local_scope;
/* DESCRIPTION
/*	This module implements legacy support for database configuration
/*	where main.cf parameter names are generated by prepending
/*	the database name to a database-defined suffix.
/*
/*	Arguments:
/* .IP param_value
/*	A parameter value to be searched for "type:table" strings.
/*	When a database type is found that supports legacy-style
/*	configuration, the table name is combined with each of the
/*	database-defined suffixes to generate candidate parameter
/*	names for that database type.
/* .IP flag_parameter
/*	A function that takes as arguments a candidate parameter
/*	name, an unused value, and a local namespace pointer. The
/*	function will flag the parameter as "used" if it has a
/*	"name=value" entry in the local or global namespace.
/* .IP local_scope
/*	The local namespace.
/* DIAGNOSTICS
/*	No explicit diagnostics.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <string.h>

/* Utility library. */

#include <stringops.h>
#include <split_at.h>
#include <mac_expand.h>
#include <dict.h>

/* Global library. */

#include <mail_conf.h>
#include <dict_proxy.h>
#include <dict_ldap.h>
#include <dict_mysql.h>
#include <dict_pgsql.h>
#include <dict_sqlite.h>
#include <dict_memcache.h>

/* Application-specific. */

#include <postconf.h>

 /*
  * SLMs.
  */
#define STR(x)	vstring_str(x)

#ifdef LEGACY_DBMS_SUPPORT

 /*
  * The legacy database interface automagically instantiates a list of
  * parameters by prepending the table name to database-specific suffixes.
  */

/* See ldap_table(5). */

static const char *ldap_suffixes[] = {
    "bind", "bind_dn", "bind_pw", "cache", "cache_expiry", "cache_size",
    "chase_referrals", "debuglevel", "dereference", "domain",
    "expansion_limit", "leaf_result_attribute", "query_filter",
    "recursion_limit", "result_attribute", "result_format", "scope",
    "search_base", "server_host", "server_port", "size_limit",
    "special_result_attribute", "terminal_result_attribute",
    "timeout", "version", 0,
};

/* See mysql_table(5). */

static const char *mysql_suffixes[] = {
    "additional_conditions", "dbname", "domain", "expansion_limit",
    "hosts", "password", "query", "result_format", "select_field",
    "table", "user", "where_field", 0,
};

/* See pgsql_table(5). */

static const char *pgsql_suffixes[] = {
    "additional_conditions", "dbname", "domain", "expansion_limit",
    "hosts", "password", "query", "result_format", "select_field",
    "select_function", "table", "user", "where_field", 0,
};

/* See sqlite_table(5). */

static const char *sqlite_suffixes[] = {
    "additional_conditions", "dbpath", "domain", "expansion_limit",
    "query", "result_format", "select_field", "table", "where_field",
    0,
};

/* See memcache_table(5). */

static const char *memcache_suffixes[] = {
    "backup", "data_size_limit", "domain", "flags", "key_format",
    "line_size_limit", "max_try", "memcache", "retry_pause",
    "timeout", "ttl", 0,
};

 /*
  * Bundle up the database types and their suffix lists.
  */
typedef struct {
    const char *db_type;
    const char **db_suffixes;
} PC_DBMS_INFO;

static const PC_DBMS_INFO dbms_info[] = {
    DICT_TYPE_LDAP, ldap_suffixes,
    DICT_TYPE_MYSQL, mysql_suffixes,
    DICT_TYPE_PGSQL, pgsql_suffixes,
    DICT_TYPE_SQLITE, sqlite_suffixes,
    DICT_TYPE_MEMCACHE, memcache_suffixes,
    0,
};

/* register_dbms_parameters_cb - mac_expand() call-back */

static const char *register_dbms_parameters_cb(const char *mac_name,
					               int unused_mode,
					               char *context)
{
    PC_MASTER_ENT *local_scope = (PC_MASTER_ENT *) context;
    const char *mac_val;

    /*
     * Local namespace "name=value" settings are always explicit. They have
     * precedence over global namespace "name=value" settings which are
     * either explicit or defined by their default value.
     */
    if (local_scope == 0
	|| (mac_val = dict_get(local_scope->all_params, mac_name)) == 0)
	mac_val = mail_conf_lookup(mac_name);
    return (mac_val);
}

/* register_dbms_parameters - look for database_type:prefix_name */

void    register_dbms_parameters(const char *param_value,
	           const char *(flag_parameter) (const char *, int, char *),
				         PC_MASTER_ENT *local_scope)
{
    const PC_DBMS_INFO *dp;
    char   *bufp;
    char   *db_type;
    char   *prefix;
    static VSTRING *buffer = 0;
    static VSTRING *candidate = 0;
    const char **cpp;

    /*
     * Emulate Postfix parameter value expansion, prepending the appropriate
     * local (master.cf "-o name-value") namespace to the global (main.cf
     * "name=value") namespace.
     * 
     * XXX This does not examine both sides of conditional macro expansion, and
     * may expand the "wrong" conditional macros. This is the best we can do
     * for legacy database configuration support.
     */
#define NO_SCAN_FILTER	((char *) 0)

    (void) mac_expand(buffer ? buffer : (buffer = vstring_alloc(100)),
		      param_value, MAC_EXP_FLAG_RECURSE, NO_SCAN_FILTER,
		      register_dbms_parameters_cb, (char *) local_scope);

    /*
     * Naive parsing. We don't really know if the parameter specifies free
     * text or a list of databases.
     */
    bufp = STR(buffer);
    while ((db_type = mystrtok(&bufp, " ,\t\r\n")) != 0) {

	/*
	 * Skip over "proxy:" indirections.
	 */
	while ((prefix = split_at(db_type, ':')) != 0
	       && strcmp(db_type, DICT_TYPE_PROXY) == 0)
	    db_type = prefix;

	/*
	 * Look for database:prefix where the prefix is not a pathname and
	 * the database is a known type. Synthesize candidate parameter names
	 * from the user-defined prefix and from the database-defined suffix
	 * list, and see if those parameters have a "name=value" entry in the
	 * local or global namespace.
	 */
	if (prefix != 0 && *prefix != '/' && *prefix != '.') {
	    for (dp = dbms_info; dp->db_type != 0; dp++) {
		if (strcmp(db_type, dp->db_type) == 0) {
		    for (cpp = dp->db_suffixes; *cpp; cpp++) {
			vstring_sprintf(candidate ? candidate :
					(candidate = vstring_alloc(30)),
					"%s_%s", prefix, *cpp);
			flag_parameter(STR(candidate), 0, (char *) local_scope);
		    }
		    break;
		}
	    }
	}
    }
}

#endif
