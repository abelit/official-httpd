/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2002 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

/*
 * http_auth: authentication
 * 
 * Rob McCool & Brian Behlendorf.
 * 
 * Adapted to Apache by rst.
 *
 * dirkx - Added Authoritative control to allow passing on to lower  
 *         modules if and only if the userid is not known to this
 *         module. A known user with a faulty or absent password still
 *         causes an AuthRequired. The default is 'Authoritative', i.e.
 *         no control is passed along.
 */

#define APR_WANT_STRFUNC
#include "apr_want.h"
#include "apr_strings.h"
#include "apr_dbm.h"
#include "apr_md5.h"

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"   /* for ap_hook_(check_user_id | auth_checker)*/

typedef struct {
    char *grpfile;
    char *dbmtype;
    int authoritative;
} authz_dbm_config_rec;

/* This should go into APR; perhaps with some nice
 * caching/locking/flocking of the open dbm file.
 *
 * Duplicated in mod_auth_dbm.c
 */
static apr_status_t get_dbm_entry_as_str(request_rec *r, char *user,
                                         char *auth_dbmfile, char *dbtype,
                                         char ** str)
{
    apr_dbm_t *f;
    apr_datum_t d, q;
    apr_status_t retval;
    q.dptr = user;

#ifndef NETSCAPE_DBM_COMPAT
    q.dsize = strlen(q.dptr);
#else
    q.dsize = strlen(q.dptr) + 1;
#endif

    retval = apr_dbm_open_ex(&f, dbtype, auth_dbmfile, APR_DBM_READONLY, 
                             APR_OS_DEFAULT, r->pool);

    if (retval != APR_SUCCESS) {
        return retval;
    }

    *str = NULL;

    if (apr_dbm_fetch(f, q, &d) == APR_SUCCESS && d.dptr) {
        *str = apr_pstrmemdup(r->pool, d.dptr, d.dsize);
    }

    apr_dbm_close(f);

    return retval;
}

static void *create_authz_dbm_dir_config(apr_pool_t *p, char *d)
{
    authz_dbm_config_rec *conf = apr_palloc(p, sizeof(*conf));

    conf->grpfile = NULL;
    conf->dbmtype = "default";
    conf->authoritative = 1;  /* fortress is secure by default */

    return conf;
}

static const command_rec authz_dbm_cmds[] =
{
    AP_INIT_TAKE1("AuthDBMGroupFile", ap_set_file_slot,
     (void *)APR_OFFSETOF(authz_dbm_config_rec, grpfile),
     OR_AUTHCFG, "database file containing group names and member user IDs"),
    AP_INIT_TAKE1("AuthzDBMType", ap_set_string_slot,
     (void *)APR_OFFSETOF(authz_dbm_config_rec, dbmtype),
     OR_AUTHCFG, "what type of DBM file the group file is"),
    AP_INIT_FLAG("AuthzDBMAuthoritative", ap_set_flag_slot,
     (void *)APR_OFFSETOF(authz_dbm_config_rec, authoritative),
     OR_AUTHCFG, "Set to 'Off' to allow access control to be passed along to "
     "lower modules, if the group required is not found or empty, or the user "
     " is not in the required groups. (default is On.)"),
    {NULL}
};

module AP_MODULE_DECLARE_DATA authz_dbm_module;

/* We do something strange with the group file.  If the group file
 * contains any : we assume the format is
 *      key=username value=":"groupname [":"anything here is ignored]
 * otherwise we now (0.8.14+) assume that the format is
 *      key=username value=groupname
 * The first allows the password and group files to be the same 
 * physical DBM file;   key=username value=password":"groupname[":"anything]
 *
 * mark@telescope.org, 22Sep95
 */

static apr_status_t get_dbm_grp(request_rec *r, char *user, char *dbmgrpfile, 
                                char *dbtype, const char ** out)
{
    char *grp_data;
    char *grp_colon;
    char *grp_colon2;

    apr_status_t status = get_dbm_entry_as_str(r, user, dbmgrpfile,
                                               dbtype, &grp_data);

    if (status != APR_SUCCESS) {
        return status;
    }

    *out = NULL;

    if (grp_data == NULL) {
        return APR_SUCCESS;
    }

    if ((grp_colon = strchr(grp_data, ':')) != NULL) {
        grp_colon2 = strchr(++grp_colon, ':');
        if (grp_colon2) {
            *grp_colon2 = '\0';
        }
        *out = grp_colon;
        return APR_SUCCESS;
    }

    return APR_SUCCESS;
}

/* Checking ID */
static int dbm_check_auth(request_rec *r)
{
    authz_dbm_config_rec *conf = ap_get_module_config(r->per_dir_config,
                                                      &authz_dbm_module);
    char *user = r->user;
    int m = r->method_number;
    const apr_array_header_t *reqs_arr = ap_requires(r);
    require_line *reqs = reqs_arr ? (require_line *) reqs_arr->elts : NULL;
    register int x;
    const char *t;
    char *w;
    apr_status_t status;

    if (!conf->grpfile) {
        return DECLINED;
    }

    if (!reqs_arr) {
        return DECLINED;
    }

    for (x = 0; x < reqs_arr->nelts; x++) {

        if (!(reqs[x].method_mask & (AP_METHOD_BIT << m))) {
            continue;
        }

        t = reqs[x].requirement;
        w = ap_getword_white(r->pool, &t);
 
        if (!strcmp(w, "group")) {
            const char *orig_groups, *groups;
            char *v;

            status = get_dbm_grp(r, user, conf->grpfile, conf->dbmtype,
                                 &groups);

            if (status != APR_SUCCESS) {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r,
                      "could not open dbm (type %s) group access file: %s", 
                        conf->dbmtype, conf->grpfile);
                return HTTP_INTERNAL_SERVER_ERROR;
           }

           if (groups == NULL) {
                if (!conf->authoritative) {
                    return DECLINED;
                }

                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                            "user %s not in DBM group file %s: %s",
                            user, conf->grpfile, r->filename);

                ap_note_basic_auth_failure(r);
                return HTTP_UNAUTHORIZED;
            }

            orig_groups = groups;
            while (t[0]) {
                w = ap_getword_white(r->pool, &t);
                groups = orig_groups;
                while (groups[0]) {
                    v = ap_getword(r->pool, &groups, ',');
                    if (!strcmp(v, w)) {
                        return OK;
                    }
                }
            }
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "user %s not in right group: %s",
                          user, r->filename);
            ap_note_basic_auth_failure(r);
            return HTTP_UNAUTHORIZED;
        }
    }

    return DECLINED;
}

static void register_hooks(apr_pool_t *p)
{
    ap_hook_auth_checker(dbm_check_auth, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA authz_dbm_module =
{
    STANDARD20_MODULE_STUFF,
    create_authz_dbm_dir_config, /* dir config creater */
    NULL,                        /* dir merger --- default is to override */
    NULL,                        /* server config */
    NULL,                        /* merge server config */
    authz_dbm_cmds,              /* command apr_table_t */
    register_hooks               /* register hooks */
};
