/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

/*
 * @file	stat_job.c
 *
 * @brief
 * 	stat_job.c	-	Functions which support the Status Job Batch Request.
 *
 * Included funtions are:
 *	svrcached()
 *	status_attrib()
 *	status_job()
 *	status_subjob()
 *
 */
#include <sys/types.h>
#include <stdlib.h>
#include "libpbs.h"
#include <ctype.h>
#include <time.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "work_task.h"
#include "pbs_error.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "pbs_ifl.h"


/* Global Data Items: */

extern attribute_def job_attr_def[];
extern int	     resc_access_perm; /* see encode_resc() in attr_fn_resc.c */
extern struct server server;
extern char	     statechars[];

/**
 * @brief
 * 		svrcached - either link in (to phead) a cached svrattrl struct which is
 *		pointed to by the attribute, or if the cached struct isn't there or
 *		is out of date, then replace it with a new svrattrl structure.
 * @par
 *		When replacing, unlink and delete old one if the reference count goes
 *		to zero.
 *
 * @par[in,out]	pat	-	attribute structure which contains a cached svrattrl struct
 * @par[in,out]	phead	-	list of new attribute values
 * @par[in]	pdef	-	attribute for any parent object.
 *
 * @note
 *	If an attribute has the ATR_DFLAG_HIDDEN flag set, then no
 *	need to obtain and cache new svrattrl values.
 */

static void
svrcached(attribute *pat, pbs_list_head *phead, attribute_def *pdef)
{
	svrattrl *working = NULL;
	svrattrl *wcopy;
	svrattrl *encoded;

	if (pdef == NULL)
		return;

	if ((pdef->at_flags & ATR_DFLAG_HIDDEN) &&
		(server.sv_attr[(int)SRV_ATR_show_hidden_attribs].at_val.at_long == 0)) {
		return;
	}

	if (resc_access_perm & PRIV_READ)
		encoded = pat->at_priv_encoded;
	else
		encoded = pat->at_user_encoded;

	if (pat->at_flags & ATR_VFLAG_MODCACHE)
		/* free old cache value if the value has changed */
		free_svrcache(pat);

	if ((encoded == NULL) || (pat->at_flags & ATR_VFLAG_MODCACHE)) {
		if (pat->at_flags & ATR_VFLAG_SET) {
			/* encode and cache new svrattrl structure */
			(void)pdef->at_encode(pat, phead, pdef->at_name,
				(char *)0, ATR_ENCODE_CLIENT, &working);
			if (resc_access_perm & PRIV_READ)
				pat->at_priv_encoded = working;
			else
				pat->at_user_encoded = working;

			pat->at_flags &= ~ATR_VFLAG_MODCACHE;
			while (working) {
				working->al_refct++;	/* incr ref count */
				working = working->al_sister;
			}
		}
	} else {
		/* can use the existing cached svrattrl struture */

		working = encoded;
		if (working->al_refct < 2) {
			while (working) {
				CLEAR_LINK(working->al_link);
				if (phead != NULL)
					append_link(phead, &working->al_link, working);
				working->al_refct++;	/* incr ref count */
				working = working->al_sister;
			}
		} else {
			/*
			 * already linked in, must make a copy to link
			 * NOTE: the copy points to the original's data
			 * so it should be freed by itself, hence the
			 * ref count is set to 1 and the sisters are not
			 * linked in
			 */
			while (working) {
				wcopy = malloc(sizeof(struct svrattrl));
				if (wcopy) {
					*wcopy = *working;
					working = working->al_sister;
					CLEAR_LINK(wcopy->al_link);
					if (phead != NULL)
						append_link(phead, &wcopy->al_link, wcopy);
					wcopy->al_refct = 1;
					wcopy->al_sister = NULL;
				}
			}
		}
	}
}

/*
 * status_attrib - add each requested or all attributes to the status reply
 *
 * @param[in,out]	pal	-	specific attributes to status
 * @param[in]	padef	-	attribute defenition structure
 * @param[in,out]	pattr	-	attribute structure
 * @param[in]	limit	-	limit on size of def array
 * @param[in]	priv	-	user-client privilege
 * @param[in,out]	phead	-	pbs_list_head
 * @param[out]	bad	-	RETURN: index of first bad attribute
 *
 * @return	int
 * @retval	0	: success
 * @retval	-1	: on error (bad attribute)
 */

int
status_attrib(svrattrl *pal, attribute_def *padef, attribute *pattr, int limit, int priv, pbs_list_head *phead, int *bad)
{
	int   index;
	int   nth = 0;

	priv &= (ATR_DFLAG_RDACC | ATR_DFLAG_SvWR);  /* user-client privilege */
	resc_access_perm = priv;  /* pass privilege to encode_resc()	*/

	/* for each attribute asked for or for all attributes, add to reply */

	if (pal) {		/* client specified certain attributes */
		while (pal) {
			++nth;
			index = find_attr(padef, pal->al_name, limit);
			if (index < 0) {
				*bad = nth;
				return (-1);
			}
			if ((padef+index)->at_flags & priv) {
				svrcached(pattr+index, phead, padef+index);
			}
			pal = (svrattrl *)GET_NEXT(pal->al_link);
		}

	} else {	/* non specified, return all readable attributes */

		for (index = 0; index < limit; index++) {
			if ((padef+index)->at_flags & priv) {
				svrcached(pattr+index, phead, padef+index);
			}
		}
	}
	return (0);
}

/**
 * @brief
 * 		status_job - Build the status reply for a single job, regular or Array,
 *		but not a subjob of an Array Job.
 *
 * @param[in,out]	pjob	-	ptr to job to status
 * @param[in]	preq	-	request structure
 * @param[in]	pal	-	specific attributes to status
 * @param[in,out]	pstathd	-	RETURN: head of list to append status to
 * @param[out]	bad	-	RETURN: index of first bad attribute
 *
 * @return	int
 * @retval	0	: success
 * @retval	PBSE_PERM	: client is not authorized to status the job
 * @retval	PBSE_SYSTEM	: memory allocation error
 * @retval	PBSE_NOATTR	: attribute error
 */

int
status_job(job *pjob, struct batch_request *preq, svrattrl *pal, pbs_list_head *pstathd, int *bad)
{
	struct brp_status *pstat;
	time_t tm;
	long oldtime = 0;
	int old_elig_flags = 0;
	int old_atyp_flags = 0;

	/* see if the client is authorized to status this job */

	if (! server.sv_attr[(int)SRV_ATR_query_others].at_val.at_long)
		if (svr_authorize_jobreq(preq, pjob))
			return (PBSE_PERM);

	if (pjob->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) {
		/* for Array Job, if array_indices_remaining is modified */
		/* then need to recalculate the string value	     */
		char *pnewstr;
		attribute *premain;

		premain = &pjob->ji_wattr[(int)JOB_ATR_array_indices_remaining];
		if (premain->at_flags & ATR_VFLAG_MODCACHE) {
			pnewstr = cvt_range(pjob->ji_ajtrk, JOB_STATE_QUEUED);
			if (pnewstr == NULL)
				pnewstr = "-";
			job_attr_def[JOB_ATR_array_indices_remaining].at_free(premain);
			job_attr_def[JOB_ATR_array_indices_remaining].at_decode(premain, 0, 0, pnewstr);
			/* also update value of attribute "array_state_count" */
			update_subjob_state_ct(pjob);
		}
	}

	/* calc eligible time on the fly and return, don't save. */
	if (server.sv_attr[(int)SRV_ATR_EligibleTimeEnable].at_val.at_long != 0) {
		if (pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_val.at_long == JOB_ELIGIBLE) {
			time(&tm);
			oldtime = pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_val.at_long;
			pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_val.at_long += ((long)tm - pjob->ji_wattr[(int)JOB_ATR_sample_starttime].at_val.at_long);
			pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags |= ATR_VFLAG_MODCACHE;

			/* Note: ATR_VFLAG_MODCACHE must be set because of svr_cached() does */
			/* 	 not correctly check ATR_VFLAG_SET */
		}
	} else {
		/* eligible_time_enable is off so,				       */
		/* clear set flag so that eligible_time and accrue type dont show */
		old_elig_flags = pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags;
		pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags &= ~ATR_VFLAG_SET;
		pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags |= (ATR_VFLAG_MODCACHE);

		old_atyp_flags = pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags;
		pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags &= ~ATR_VFLAG_SET;
		pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags |= (ATR_VFLAG_MODCACHE);

		/* Note: ATR_VFLAG_MODCACHE must be set because of svr_cached() does */
		/*		not correctly check ATR_VFLAG_SET */
	}

	/* allocate reply structure and fill in header portion */

	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == (struct brp_status *)0)
		return (PBSE_SYSTEM);
	CLEAR_LINK(pstat->brp_stlink);
	pstat->brp_objtype = MGR_OBJ_JOB;
	(void)strcpy(pstat->brp_objname, pjob->ji_qs.ji_jobid);
	CLEAR_HEAD(pstat->brp_attr);
	append_link(pstathd, &pstat->brp_stlink, pstat);

	/* add attributes to the status reply */

	*bad = 0;
	if (status_attrib(pal, job_attr_def, pjob->ji_wattr, JOB_ATR_LAST,
		preq->rq_perm, &pstat->brp_attr, bad))
		return (PBSE_NOATTR);

	/* reset eligible time, it was calctd on the fly, real calctn only when accrue_type changes */

	if (server.sv_attr[(int)SRV_ATR_EligibleTimeEnable].at_val.at_long != 0) {
		if (pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_val.at_long == JOB_ELIGIBLE) {
			pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_val.at_long = oldtime;
			pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags |= ATR_VFLAG_MODCACHE;

			/* Note: ATR_VFLAG_MODCACHE must be set because of svr_cached() does */
			/*	 not correctly check ATR_VFLAG_SET */
		}
	} else {
		/* reset the set flags */
		pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags = old_elig_flags;
		pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags = old_atyp_flags;
	}

	return (0);
}

/**
 * @brief
 * 		status_subjob - status a single subjob (of an Array Job)
 *		Works by statusing the parrent unless subjob is actually running.
 *
 * @param[in,out]	pjob	-	ptr to parent Array
 * @param[in]	preq	-	request structure
 * @param[in]	pal	-	specific attributes to status
 * @param[in]	subj	-	if not = -1 then include subjob [n]
 * @param[in,out]	pstathd	-	RETURN: head of list to append status to
 * @param[out]	bad	-	RETURN: index of first bad attribute
 *
 * @return	int
 * @retval	0	: success
 * @retval	PBSE_PERM	: client is not authorized to status the job
 * @retval	PBSE_SYSTEM	: memory allocation error
 * @retval	PBSE_IVALREQ	: something wrong with the flags
 */
int
status_subjob(job *pjob, struct batch_request *preq, svrattrl *pal, int subj, pbs_list_head *pstathd, int *bad)
{
	int		   limit = (int)JOB_ATR_LAST;
	struct brp_status *pstat;
	job		  *psubjob;	/* ptr to job to status */
	char		   realstate;
	int		   rc = 0;
	int		   oldeligflags = 0;
	int		   oldatypflags = 0;
	int 		   subjob_state = -1;
	char 		   *old_subjob_comment = NULL;

	/* see if the client is authorized to status this job */

	if (! server.sv_attr[(int)SRV_ATR_query_others].at_val.at_long)
		if (svr_authorize_jobreq(preq, pjob))
			return (PBSE_PERM);

	if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) == 0)
		return PBSE_IVALREQ;

	/* if subjob is running, use real job structure */

	if (get_subjob_state(pjob, subj) == JOB_STATE_RUNNING) {
		psubjob = find_job(mk_subjob_id(pjob, subj));
		if (psubjob)
			status_job(psubjob, preq, pal, pstathd, bad);
		return 0;
	}

	/* otherwise we fake it with info from the parent      */
	/* allocate reply structure and fill in header portion */


	/* for the general case, we don't want to include the parent's */
	/* array related attrbutes as they belong only to the Array    */
	if (pal == NULL)
		limit = JOB_ATR_array;
	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == (struct brp_status *)0)
		return (PBSE_SYSTEM);
	CLEAR_LINK(pstat->brp_stlink);
	pstat->brp_objtype = MGR_OBJ_JOB;
	(void)strcpy(pstat->brp_objname, mk_subjob_id(pjob, subj));
	CLEAR_HEAD(pstat->brp_attr);
	append_link(pstathd, &pstat->brp_stlink, pstat);

	/* add attributes to the status reply */

	*bad = 0;

	/*
	 * fake the job state and comment by setting the parent job's state
	 * and comment to that of the subjob
	 */
	subjob_state = get_subjob_state(pjob, subj);
	realstate = pjob->ji_wattr[(int)JOB_ATR_state].at_val.at_char;
	pjob->ji_wattr[(int)JOB_ATR_state].at_val.at_char = statechars[subjob_state];
	pjob->ji_wattr[(int)JOB_ATR_state].at_flags |= ATR_VFLAG_MODCACHE;

	if (subjob_state == JOB_STATE_EXPIRED || subjob_state == JOB_STATE_FINISHED) {
		if (pjob->ji_ajtrk->tkm_tbl[subj].trk_substate == JOB_SUBSTATE_FINISHED) {
			if (pjob->ji_wattr[(int)JOB_ATR_Comment].at_flags & ATR_VFLAG_SET) {
				old_subjob_comment = strdup(pjob->ji_wattr[(int)JOB_ATR_Comment].at_val.at_str);
				if (old_subjob_comment == (char *)0)
					return (PBSE_SYSTEM);
			}
			if (job_attr_def[(int)JOB_ATR_Comment].at_decode(&pjob->ji_wattr[(int)JOB_ATR_Comment],
				(char *)0, (char *)0, "Subjob finished") == PBSE_SYSTEM) {
				free(old_subjob_comment);
				return (PBSE_SYSTEM);
			}
		} else if (pjob->ji_ajtrk->tkm_tbl[subj].trk_substate == JOB_SUBSTATE_FAILED) {
			if (pjob->ji_wattr[(int)JOB_ATR_Comment].at_flags & ATR_VFLAG_SET) {
				old_subjob_comment = strdup(pjob->ji_wattr[(int)JOB_ATR_Comment].at_val.at_str);
				if (old_subjob_comment == (char *)0)
					return (PBSE_SYSTEM);
			}
			if (job_attr_def[(int)JOB_ATR_Comment].at_decode(&pjob->ji_wattr[(int)JOB_ATR_Comment],
				(char *)0, (char *)0, "Subjob failed") == PBSE_SYSTEM) {
				free(old_subjob_comment);
				return (PBSE_SYSTEM);
			}
		} else if (pjob->ji_ajtrk->tkm_tbl[subj].trk_substate == JOB_SUBSTATE_TERMINATED) {
			if (pjob->ji_wattr[(int)JOB_ATR_Comment].at_flags & ATR_VFLAG_SET) {
				old_subjob_comment = strdup(pjob->ji_wattr[(int)JOB_ATR_Comment].at_val.at_str);
				if (old_subjob_comment == (char *)0)
					return (PBSE_SYSTEM);
			}
			if (job_attr_def[(int)JOB_ATR_Comment].at_decode(&pjob->ji_wattr[(int)JOB_ATR_Comment],
				(char *)0, (char *)0, "Subjob terminated") == PBSE_SYSTEM) {
				free(old_subjob_comment);
				return (PBSE_SYSTEM);
			}
		}
	}

	/* when eligible_time_enable is off,				      */
	/* clear the set flag so that eligible_time and accrue_type dont show */
	if (server.sv_attr[(int)SRV_ATR_EligibleTimeEnable].at_val.at_long == 0) {
		oldeligflags = pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags;
		pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags &= ~ATR_VFLAG_SET;
		pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags |= ATR_VFLAG_MODCACHE;

		oldatypflags = pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags;
		pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags &= ~ATR_VFLAG_SET;
		pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags |= ATR_VFLAG_MODCACHE;

		/* Note: ATR_VFLAG_MODCACHE must be set because of svr_cached() does */
		/* 	 not correctly check ATR_VFLAG_SET */
	}

	if (status_attrib(pal, job_attr_def, pjob->ji_wattr, limit,
		preq->rq_perm, &pstat->brp_attr, bad))
		rc =  PBSE_NOATTR;

	/* Set the parent state back to what it really is */

	pjob->ji_wattr[(int)JOB_ATR_state].at_val.at_char = realstate;
	pjob->ji_wattr[(int)JOB_ATR_state].at_flags |= ATR_VFLAG_MODCACHE;

	/* Set the parent comment back to what it really is */
	if (old_subjob_comment != NULL) {
		if (job_attr_def[(int)JOB_ATR_Comment].at_decode(&pjob->ji_wattr[(int)JOB_ATR_Comment],
			(char *)0, (char *)0, old_subjob_comment) == PBSE_SYSTEM) {
			free(old_subjob_comment);
			return (PBSE_SYSTEM);
		}

		free(old_subjob_comment);
	}

	/* reset the flags */
	if (server.sv_attr[(int)SRV_ATR_EligibleTimeEnable].at_val.at_long == 0) {
		pjob->ji_wattr[(int)JOB_ATR_eligible_time].at_flags = oldeligflags;
		pjob->ji_wattr[(int)JOB_ATR_accrue_type].at_flags = oldatypflags;
	}

	return (rc);
}
