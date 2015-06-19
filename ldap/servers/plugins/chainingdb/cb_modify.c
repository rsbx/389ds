/** BEGIN COPYRIGHT BLOCK
 * Copyright (C) 2001 Sun Microsystems, Inc. Used by permission.
 * Copyright (C) 2005 Red Hat, Inc.
 * All rights reserved.
 *
 * License: GPL (version 3 or any later version).
 * See LICENSE for details. 
 * END COPYRIGHT BLOCK **/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "cb.h"

static void cb_remove_illegal_mods(cb_backend_instance * inst, LDAPMod **mods);

/*
 * Perform a modify operation
 *
 * Returns:
 *   0  - success
 *   <0 - fail
 *
 */

int
chaining_back_modify ( Slapi_PBlock *pb )
{
	cb_outgoing_conn    *cnx;
	Slapi_Backend       *be;
	cb_backend_instance *cb;
	LDAPControl         **ctrls, **serverctrls;
	LDAPMod             ** mods;
	LDAPMessage         *res;
	LDAP                *ld = NULL;
	Slapi_DN            *sdn = NULL;
	const char          *dn = NULL;
	char                **referrals=NULL;
	char                *matched_msg, *error_msg;
	char                *cnxerrbuf=NULL;
	time_t              endtime = 0;
	int                 rc, parse_rc, msgid, i;

	if ( LDAP_SUCCESS != (rc=cb_forward_operation(pb) )) {
		cb_send_ldap_result( pb, rc, NULL, "Chaining forbidden", 0, NULL );
		return -1;
	}

	slapi_pblock_get( pb, SLAPI_BACKEND, &be );
	cb = cb_get_instance(be);
	cb_update_monitor_info(pb,cb,SLAPI_OPERATION_MODIFY);

	/* Check wether the chaining BE is available or not */
	if ( cb_check_availability( cb, pb ) == FARMSERVER_UNAVAILABLE ){
		return -1;
	}

	slapi_pblock_get( pb, SLAPI_MODIFY_TARGET_SDN, &sdn );
	if (NULL == sdn) {
		cb_send_ldap_result(pb, LDAP_INVALID_DN_SYNTAX, NULL, "Null target DN", 0, NULL );
		return -1;
	}
	dn = slapi_sdn_get_dn(sdn);

	if (cb_debug_on()) {
		slapi_log_error( SLAPI_LOG_PLUGIN, CB_PLUGIN_SUBSYSTEM,"modify: target:<%s>\n",dn);
	}

	ctrls = serverctrls = NULL;
	slapi_pblock_get( pb, SLAPI_MODIFY_MODS, &mods );
	slapi_pblock_get( pb, SLAPI_REQCONTROLS, &ctrls );

	/* Check acls				*/
	if ( cb->local_acl && !cb->associated_be_is_disabled ) {
		char * errbuf=NULL;
		Slapi_Entry *te = slapi_entry_alloc();
		slapi_entry_set_sdn(te, sdn); /* sdn: copied */
		rc = slapi_acl_check_mods( pb, te, mods, &errbuf);
		slapi_entry_free(te);

		if ( rc != LDAP_SUCCESS ) {
			cb_send_ldap_result( pb, rc, NULL, errbuf, 0, NULL );
			slapi_ch_free_string(&errbuf);
			return -1;
		}
	}


	/* Grab a connection handle */
	rc = cb_get_connection(cb->pool, &ld, &cnx, NULL, &cnxerrbuf);
	if (LDAP_SUCCESS != rc) {
		static int warned_get_conn = 0;
		if (!warned_get_conn) {
			slapi_log_error(SLAPI_LOG_FATAL, CB_PLUGIN_SUBSYSTEM,
			                "cb_get_connection failed (%d) %s\n",
			                rc, ldap_err2string(rc));
			warned_get_conn = 1;
		}
		cb_send_ldap_result(pb, LDAP_OPERATIONS_ERROR, NULL, cnxerrbuf, 0, NULL);
		slapi_ch_free_string(&cnxerrbuf);
		/* ping the farm.
		 * If the farm is unreachable, we increment the counter */
		cb_ping_farm(cb, NULL, 0);
		return -1;
	}

	/* Control management */
	if ( (rc = cb_update_controls( pb,ld,&ctrls,CB_UPDATE_CONTROLS_ADDAUTH )) != LDAP_SUCCESS ) {
		cb_send_ldap_result( pb, rc, NULL,NULL, 0, NULL);
		cb_release_op_connection(cb->pool,ld,CB_LDAP_CONN_ERROR(rc));
		/* Don't free mods here: are freed at the do_modify level */
		return -1;
	}

	if ( slapi_op_abandoned( pb )) {
		cb_release_op_connection(cb->pool,ld,0);
		/* Don't free mods here: are freed at the do_modify level */
		ldap_controls_free(ctrls);
		return -1;
	}

	/* Remove illegal attributes from the mods */
	cb_remove_illegal_mods(cb,mods);

	/* heart-beat management */
	if (cb->max_idle_time>0)
		endtime=current_time() + cb->max_idle_time;

	/*
	 * Call the backend preoperation plugins
	 */
	if((rc = slapi_plugin_call_preop_be_plugins(pb, SLAPI_PLUGIN_MOD_OP))){
		slapi_log_error( SLAPI_LOG_FATAL, CB_PLUGIN_SUBSYSTEM, "modify (%s): pre betxn failed, error (%d)\n",dn,rc);
		cb_release_op_connection(cb->pool,ld,0);
		ldap_controls_free(ctrls);
		return -1;
	}

	/* Send LDAP operation to the remote host */
	rc = ldap_modify_ext( ld, dn, mods, ctrls, NULL, &msgid );
	ldap_controls_free(ctrls);

	if ( rc != LDAP_SUCCESS ) {
		cb_send_ldap_result( pb, LDAP_OPERATIONS_ERROR, NULL, ldap_err2string(rc), 0, NULL);
		cb_release_op_connection(cb->pool,ld,CB_LDAP_CONN_ERROR(rc));
		return -1;
	}

	while ( 1 ) {
		if (cb_check_forward_abandon(cb,pb,ld,msgid)) {
			/* connection handle released */
			return -1;
		}

		rc = ldap_result( ld, msgid, 0, &cb->abandon_timeout, &res );
		switch ( rc ) {
		case -1:
			cb_send_ldap_result( pb, LDAP_OPERATIONS_ERROR, NULL,
				ldap_err2string(rc), 0, NULL);
			cb_release_op_connection(cb->pool,ld,CB_LDAP_CONN_ERROR(rc));
			ldap_msgfree(res);
			return -1;
		case 0:
			if ((rc=cb_ping_farm(cb,cnx,endtime)) != LDAP_SUCCESS) {
				/* does not respond. give up and return a error to the client. */

				/*cb_send_ldap_result(pb,LDAP_OPERATIONS_ERROR, NULL,
					ldap_err2string(rc), 0, NULL);*/
				cb_send_ldap_result(pb,LDAP_OPERATIONS_ERROR, NULL, "FARM SERVER TEMPORARY UNAVAILABLE", 0, NULL);
				cb_release_op_connection(cb->pool,ld,CB_LDAP_CONN_ERROR(rc));
				ldap_msgfree(res);
				return -1;
			}
#ifdef CB_YIELD
			DS_Sleep(PR_INTERVAL_NO_WAIT);
#endif
			break;

		default:
			matched_msg=error_msg=NULL;
			serverctrls=NULL;
			parse_rc = ldap_parse_result( ld, res, &rc, &matched_msg, 
									  &error_msg, &referrals, &serverctrls, 1 );
			if ( parse_rc != LDAP_SUCCESS ) {
				static int warned_parse_rc = 0;
				if (!warned_parse_rc) {
					slapi_log_error( SLAPI_LOG_FATAL, CB_PLUGIN_SUBSYSTEM,
						"%s%s%s\n",
						matched_msg?matched_msg:"",
						(matched_msg&&(*matched_msg!='\0'))?": ":"",
						ldap_err2string(parse_rc));
					warned_parse_rc = 1;
				}
				cb_send_ldap_result( pb, LDAP_OPERATIONS_ERROR, NULL,
									 ENDUSERMSG, 0, NULL );
				cb_release_op_connection(cb->pool,ld,CB_LDAP_CONN_ERROR(parse_rc));
				slapi_ch_free_string(&matched_msg);
				slapi_ch_free_string(&error_msg);
				ldap_controls_free(serverctrls);
				charray_free(referrals);
				return -1;
			}

			if ( rc != LDAP_SUCCESS ) {
				struct berval ** refs =  referrals2berval(referrals); 
				static int warned_rc = 0;
				if (!warned_rc && error_msg) {
					slapi_log_error( SLAPI_LOG_FATAL, CB_PLUGIN_SUBSYSTEM,
						"%s%s%s\n",
						matched_msg?matched_msg:"",
						(matched_msg&&(*matched_msg!='\0'))?": ":"",
						error_msg );
					warned_rc = 1;
				}
				cb_send_ldap_result( pb, rc, matched_msg, ENDUSERMSG, 0, refs);
				cb_release_op_connection(cb->pool,ld,CB_LDAP_CONN_ERROR(rc));
				slapi_ch_free_string(&matched_msg);
				slapi_ch_free_string(&error_msg);
				if (refs) 
					ber_bvecfree(refs);
				charray_free(referrals);
				ldap_controls_free(serverctrls);
				return -1;
			}

			cb_release_op_connection(cb->pool,ld,0);

			/* Call the backend postoperation plugins */
			if((rc = slapi_plugin_call_postop_be_plugins(pb, SLAPI_PLUGIN_MOD_OP))){
				slapi_log_error( SLAPI_LOG_FATAL, CB_PLUGIN_SUBSYSTEM, "modify (%s): post betxn failed, error (%d)\n",dn,rc);
			}

			/* Add control response sent by the farm server */
			for (i=0; serverctrls && serverctrls[i];i++)
				slapi_pblock_set( pb, SLAPI_ADD_RESCONTROL, serverctrls[i]);
			/* SLAPI_ADD_RESCONTROL dups controls */
			ldap_controls_free(serverctrls);
			slapi_ch_free_string(&matched_msg);
			slapi_ch_free_string(&error_msg);
			charray_free(referrals);
			cb_send_ldap_result( pb, LDAP_SUCCESS, NULL, NULL, 0, NULL );

			return 0;
		}
	}
	/* Never reached */
	/* return 0; */
}

/* Function removes mods which are not allowed over-the-wire */
static void
cb_remove_illegal_mods(cb_backend_instance *inst, LDAPMod **mods)
{
	int             i, j;
	LDAPMod         *tmp;

	if ( inst->illegal_attributes != NULL ) {	/* Unlikely to happen */
		slapi_rwlock_wrlock(inst->rwl_config_lock);

		for (j=0; inst->illegal_attributes[j]; j++) {
			for ( i = 0; mods && mods[i] != NULL; i++ ) {
				if (slapi_attr_types_equivalent(inst->illegal_attributes[j],mods[i]->mod_type)) {
					tmp = mods[i];
					for ( j = i; mods[j] != NULL; j++ ) {
						mods[j] = mods[j + 1];
					}
					slapi_ch_free( (void**)&(tmp->mod_type) );
					if ( tmp->mod_bvalues != NULL ) {
						ber_bvecfree( tmp->mod_bvalues );
					}
					slapi_ch_free( (void**)&tmp );
					i--;
				}
			}
		}
		slapi_rwlock_unlock(inst->rwl_config_lock);
	}
}
