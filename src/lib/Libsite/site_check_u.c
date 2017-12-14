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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <netdb.h>
#include "portability.h"
#include "list_link.h"
#include "attribute.h"
#include "server_limits.h"
#include "job.h"
#include "pbs_nodes.h"
#include "reservation.h"
#include "queue.h"
#include "log.h"
#include "pbs_ifl.h"
#include "svrfunc.h"            /* to resolve cvrt_fqn_to_name */

/**
 * @file	site_check_u.c
 */
/* Global Data Items */

extern char *pbs_o_host;
extern char  server_host[];
extern char *msg_orighost;	/* error message: no PBS_O_HOST */

/**
 * @brief
 * 	site_check_user_map - site_check_user_map()
 *	This routine determines if the specified "luser" is authorized
 *	on this host to serve as a kind of "proxy" for the object's owner.
 *	Uses the object's "User_List" attribute.
 *
 *	As provided, this routine uses ruserok(3N).  If this is a problem,
 *	It's replacement is "left as an exercise for the reader."
 *
 * @param[in] pjob - job info
 * @param[in] objtype - type of object
 * @param[in] luser - username
 *
 * @return	int
 * @retval	0	success
 * @retval	>0	error
 *
 */

int
site_check_user_map(void *pobj, int objtype, char *luser)
{
	char    *orighost;
	char	 owner[PBS_MAXUSER+1];
	char	*p1;
	char	*objid;
	int	event_type, event_class;
	int	 rc;


	/* set pointer variables etc based on object's type */
	if (objtype == JOB_OBJECT) {
		p1 = ((job *)pobj)->ji_wattr[JOB_ATR_job_owner].at_val.at_str;
		objid = ((job *)pobj)->ji_qs.ji_jobid;
		event_type = PBSEVENT_JOB;
		event_class = PBS_EVENTCLASS_JOB;
	} else {
		p1 = ((resc_resv *)pobj)->ri_wattr[RESV_ATR_resv_owner].at_val.at_str;
		objid = ((resc_resv *)pobj)->ri_qs.ri_resvID;
		event_type = PBSEVENT_JOB;
		event_class = PBS_EVENTCLASS_JOB;
	}

	/* the owner name, without the "@host" */
	cvrt_fqn_to_name(p1, owner);

	orighost = strchr(p1, '@');
	if ((orighost == (char *)0) || (*++orighost == '\0')) {
		log_event(event_type, event_class, LOG_INFO, objid, msg_orighost);
		return (-1);
	}
	if (!strcasecmp(orighost, server_host) && !strcmp(owner, luser))
		return (0);

#ifdef WIN32
	rc =   ruserok(orighost, isAdminPrivilege(luser), owner, luser);
	if (rc == -2) {
		sprintf(log_buffer, "User %s does not exist!", luser);
		log_err(0, "site_check_user_map", log_buffer);
		rc = -1;
	} else if (rc == -3) {
		sprintf(log_buffer,
			"User %s's [HOMEDIR]/.rhosts is unreadable! Needs SYSTEM or Everyone access", luser);
		log_err(0, "site_check_user_map", log_buffer);
		rc = -1;
	}	
#else
	rc =   ruserok(orighost, 0, owner, luser);
#endif

#ifdef sun
	/* broken Sun ruserok() sets process so it appears to be owned	*/
	/* by the luser, change it back for cosmetic reasons		*/
	if (setuid(0) == -1) {
		log_err(errno, "site_check_user_map", "cannot go back to root");
		exit(1);
	}
#endif	/* sun */
	return (rc);
}

/**
 * @brief
 *	site_check_u - site_acl_check()
 *    	This routine is a place holder for sites that wish to implement
 *    	access controls that differ from the standard PBS user, group, host
 *    	access controls.  It does NOT replace their functionality.
 *
 * @param[in] pjob - job pointer 
 * @param[in] pqueue - pointer to queue defn
 *
 * @return	int
 * @retval	0	ok
 * @retval	-1	access denied
 * 
 */

int
site_acl_check(job *pjob, pbs_queue *pque)
{
	return (0);
}
