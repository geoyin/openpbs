/*
 * Copyright (C) 1994-2016 Altair Engineering, Inc.
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
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 * Commercial License Information: 
 * 
 * The PBS Pro software is licensed under the terms of the GNU Affero General 
 * Public License agreement ("AGPL"), except where a separate commercial license 
 * agreement for PBS Pro version 14 or later has been executed in writing with Altair.
 *  
 * Altair’s dual-license business model allows companies, individuals, and 
 * organizations to create proprietary derivative works of PBS Pro and distribute 
 * them - whether embedded or bundled with other software - under a commercial 
 * license agreement.
 * 
 * Use of Altair’s trademarks, including but not limited to "PBS™", 
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's 
 * trademark licensing policies.
 *
 */
/**
 * @file    reply_send.c
 *
 * @brief
 * 		This file contains the routines used to send a reply to a client following
 * 		the processing of a request.  The following routines are provided here:
 *
 *	reply_send()  - the main routine, used by all reply senders
 *	reply_ack()   - send a basic no error acknowledgement
 *	req_reject()  - send a basic error return
 *	reply_text()  - send a return with a supplied text string
 *	reply_jobid() - used by several requests where the job id must be sent
 *	reply_free()  - free the substructure that might hang from a reply
 *	set_err_msg() - set a message relating to the error "code"
 *	dis_reply_write()	- reply is sent to a remote client
 *	reply_badattr()	- Create a reject (error) reply for a request including the name of the bad attribute/resource.
 *
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include "libpbs.h"
#include "dis.h"
#include "log.h"
#include "pbs_error.h"
#include "server_limits.h"
#include "list_link.h"
#include "net_connect.h"
#include "attribute.h"
#include "credential.h"
#include "batch_request.h"
#include "work_task.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "rpp.h"


/* External Globals */


extern char *msg_daemonname;
extern char *msg_system;

#ifndef PBS_MOM
extern pbs_list_head task_list_event;
extern pbs_list_head task_list_immed;
char   *resc_in_err = NULL;
#endif	/* PBS_MOM */

extern struct pbs_err_to_txt pbs_err_to_txt[];
extern int pbs_tcp_errno;
extern int rpp_flush(int index);

void reply_free(struct batch_reply *prep);

#define ERR_MSG_SIZE 256


/**
 * @brief
 * 		set a message relating to the error "code"
 *
 * @param code[in] - the specific error
 * @param msgbuf[out] - the buffer into which the message is placed
 * @param msglen[in] - length of above buffer
 */
static void
set_err_msg(int code, char *msgbuf, size_t msglen)
{
	char *msg = (char *)0;
	char *msg_tmp;

	/* subtract 1 from buffer length to insure room for null terminator */
	--msglen;

	/* see if there is an error message associated with the code */

	*msgbuf = '\0';
	if (code == PBSE_SYSTEM) {
		strncpy(msgbuf, msg_daemonname, msglen-2);
		strcat(msgbuf, ": ");
		strncat(msgbuf, msg_system, msglen - strlen(msgbuf));
		msg_tmp = strerror(errno);

		if (msg_tmp)
			strncat(msgbuf, msg_tmp, msglen - strlen(msgbuf));
		else
			strncat(msgbuf, "Unknown error", msglen - strlen(msgbuf));


#ifndef PBS_MOM
	} else if ((
		(code == PBSE_UNKRESC)         ||
		(code == PBSE_BADATVAL)        ||
		(code == PBSE_INVALSELECTRESC) ||
		(code == PBSE_INVALJOBRESC)    ||
		(code == PBSE_INVALNODEPLACE)  ||
		(code == PBSE_DUPRESC)         ||
		(code == PBSE_INDIRECTHOP)     ||
		(code == PBSE_SAVE_ERR)        ||
		(code == PBSE_INDIRECTBT))
		&& (resc_in_err != NULL)) {
		strncpy(msgbuf, pbse_to_txt(code), msglen-2);
		/* -2 is to make sure there is room for a colon and a space */
		strcat(msgbuf, ": ");
		strncat(msgbuf, resc_in_err, msglen - strlen(msgbuf));
		free(resc_in_err);
		resc_in_err = NULL;
		msg = NULL;
#endif
	} else if (code > PBSE_) {
		msg = pbse_to_txt(code);

	} else {
		msg = strerror(code);
	}

	if (msg) {
		(void)strncpy(msgbuf, msg, msglen);
	}
	msgbuf[msglen] = '\0';
}
/**
 * @brief
 * 		reply is to be sent to a remote client
 *
 * @param[in]	sfds - connection socket
 * @param[in]	preq - batch_request which contains the reply for the request
 *
 * @return	return code
 */
static int
dis_reply_write(int sfds, struct batch_request *preq)
{
	int rc;
	struct batch_reply *preply = &preq->rq_reply;

	if (preq->isrpp) {
		rc = encode_DIS_replyRPP(sfds, preq->rppcmd_msgid, preply);
	} else {
		/*
		 * clear pbs_tcp_errno - set on error in DIS_tcp_wflush when called
		 * either in encode_DIS_reply() or directly below.
		 */
		pbs_tcp_errno = 0;
		DIS_tcp_setup(sfds);		/* setup for DIS over tcp */

		rc = encode_DIS_reply(sfds, preply);
	}

	if (rc == 0) {
		DIS_wflush(sfds, preq->isrpp);
	}

	if (rc) {
		char hn[PBS_MAXHOSTNAME+1];

		if (get_connecthost(sfds, hn, PBS_MAXHOSTNAME) == -1)
			strcpy(hn, "??");
		(void)sprintf(log_buffer, "DIS reply failure, %d, to host %s, errno=%d", rc, hn, pbs_tcp_errno);
		/* if EAGAIN - then write was blocked and timed-out, note it */
		if (pbs_tcp_errno == EAGAIN)
			strcat(log_buffer, " write timed out");
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_REQUEST, LOG_WARNING,
			"dis_reply_write", log_buffer);
		close_client(sfds);
	}
	return rc;
}

/**
 * @brief
 * 		Send a reply to a batch request, reply either goes to a
 * 		remote client over the network:
 *		Encode the reply to a "presentation element",
 *		allocate the presenetation stream and attach to socket,
 *		write out reply, and free ps, pe, and isoreply structures.
 * 		Or the reply is for a request from the local server:
 *		locate the work task associated with the request and dispatch it
 *
 * @par Side-effects:
 *		The request (and reply) structures are freed.
 *
 * @param[in]	request	- batch request
 *
 * @return	error code
 * @retval	0	- success
 * @retval	!=0	- failure
 */
int
reply_send(struct batch_request *request)
{
#ifndef PBS_MOM
	struct work_task   *ptask;
#endif	/* PBS_MOM */
	int		    rc = 0;
	int		    sfds = request->rq_conn;		/* socket */

	/* if this is a child request, just move the error to the parent */

	if (request->rq_parentbr) {
		if ((request->rq_parentbr->rq_reply.brp_choice == BATCH_REPLY_CHOICE_NULL) && (request->rq_parentbr->rq_reply.brp_code == 0)) {
			request->rq_parentbr->rq_reply.brp_code = request->rq_reply.brp_code;
			request->rq_parentbr->rq_reply.brp_auxcode = request->rq_reply.brp_auxcode;
			if (request->rq_reply.brp_choice == BATCH_REPLY_CHOICE_Text) {
				request->rq_parentbr->rq_reply.brp_choice =
					request->rq_reply.brp_choice;
				request->rq_parentbr->rq_reply.brp_un.brp_txt.brp_txtlen
				= request->rq_reply.brp_un.brp_txt.brp_txtlen;
				request->rq_parentbr->rq_reply.brp_un.brp_txt.brp_str =
					strdup(request->rq_reply.brp_un.brp_txt.brp_str);
				if (request->rq_parentbr->rq_reply.brp_un.brp_txt.brp_str == NULL) {
					log_err(-1, "reply_send", "Unable to allocate Memory!\n");
					return (PBSE_SYSTEM);
				}
			}
		}
	} else if (request->rq_refct > 0) {
		/* waiting on sister (subjob) requests, will send when */
		/* last one decrecments the reference count to zero    */
		return 0;
	} else if (sfds == PBS_LOCAL_CONNECTION) {

#ifndef PBS_MOM
		/*
		 * reply stays local, find work task and move it to
		 * the immediate list for dispatching.
		 *
		 * Special Note: In this instance the function that utimately
		 * gets dispatched by the work_task entry has the RESPONSIBILITY
		 * for freeing the batch_request structure.
		 */

		ptask = (struct work_task *)GET_NEXT(task_list_event);
		while (ptask) {
			if ((ptask->wt_type == WORK_Deferred_Local) &&
				(ptask->wt_parm1 == (void *)request)) {
				delete_link(&ptask->wt_linkall);
				append_link(&task_list_immed,
					&ptask->wt_linkall, ptask);
				return (0);
			}
			ptask = (struct work_task *)GET_NEXT(ptask->wt_linkall);
		}

		/* Uh Oh, should have found a task and didn't */

		log_err(-1, __func__, "did not find work task for local request");
#endif	/* PBS_MOM */
		rc = PBSE_SYSTEM;

	} else if (sfds >= 0) {

		/*
		 * Otherwise, the reply is to be sent to a remote client
		 */
		if (rc == PBSE_NONE) {
			rc = dis_reply_write(sfds, request);
		}
	}

	free_br(request);
	return (rc);
}

/**
 * @brief
 * 		Send a normal acknowledgement reply to a request
 *
 * @param[in,out]	preq	- request structure
 *
 * @par Side-effects:
 *		Always frees the request structure.
 *
 */
void
reply_ack(struct batch_request *preq)
{
	if (preq == NULL)
		return;

	if (preq->isrpp && (preq->rpp_ack == 0)) {
		free_br(preq);
		return;
	}

	if (preq->rq_reply.brp_choice != BATCH_REPLY_CHOICE_NULL)
		/* in case another reply was being built up, clean it out */
		reply_free(&preq->rq_reply);
	preq->rq_reply.brp_code    = PBSE_NONE;
	preq->rq_reply.brp_auxcode = 0;
	preq->rq_reply.brp_choice  = BATCH_REPLY_CHOICE_NULL;
	(void)reply_send(preq);
}

/**
 * @brief
 * 		Free any sub-structures that might hang from the basic
 * 		batch_reply structure, the reply structure itself IS NOT FREED.
 *
 * @param[in]	prep	- basic batch_reply structure
 */
void
reply_free(struct batch_reply *prep)
{
	struct brp_status  *pstat;
	struct brp_status  *pstatx;
	struct brp_select  *psel;
	struct brp_select  *pselx;

	if (prep->brp_choice == BATCH_REPLY_CHOICE_Text) {
		if (prep->brp_un.brp_txt.brp_str) {
			(void)free(prep->brp_un.brp_txt.brp_str);
			prep->brp_un.brp_txt.brp_str = (char *)0;
			prep->brp_un.brp_txt.brp_txtlen = 0;
		}

	} else if (prep->brp_choice == BATCH_REPLY_CHOICE_Select) {
		psel = prep->brp_un.brp_select;
		while (psel) {
			pselx = psel->brp_next;
			(void)free(psel);
			psel = pselx;
		}

	} else if (prep->brp_choice == BATCH_REPLY_CHOICE_Status) {
		pstat = (struct brp_status *)GET_NEXT(prep->brp_un.brp_status);
		while (pstat) {
			pstatx = (struct brp_status *)GET_NEXT(pstat->brp_stlink);
			free_attrlist(&pstat->brp_attr);
			(void)free(pstat);
			pstat = pstatx;
		}
	} else if (prep->brp_choice == BATCH_REPLY_CHOICE_RescQuery) {
		(void)free(prep->brp_un.brp_rescq.brq_avail);
		(void)free(prep->brp_un.brp_rescq.brq_alloc);
		(void)free(prep->brp_un.brp_rescq.brq_resvd);
		(void)free(prep->brp_un.brp_rescq.brq_down);
	}
	prep->brp_choice = BATCH_REPLY_CHOICE_NULL;
}

/**
 * @brief
 * 		Create a reject (error) reply for a request, then send the reply.
 *
 * @param[in]	code	- PBS error code indicating the reason the rejection
 * 							is taking place.  If this is PBSE_NONE, no log message is output.
 * @param[in]	aux	- Auxiliary error code
 * @param[in,out]	preq	- Pointer to batch request
 *
 * @par Side-effects:
 *		Always frees the request structure.
 */
void
req_reject(int code, int aux, struct batch_request *preq)
{
	int   evt_type;
	char  msgbuf[ERR_MSG_SIZE];

	if (preq == NULL)
		return;

	if (code != PBSE_NONE) {
		evt_type = PBSEVENT_DEBUG;
		if (code == PBSE_BADHOST)
			evt_type |= PBSEVENT_SECURITY;
		sprintf(log_buffer,
			"Reject reply code=%d, aux=%d, type=%d, from %s@%s",
			code, aux, preq->rq_type, preq->rq_user, preq->rq_host);
		log_event(evt_type, PBS_EVENTCLASS_REQUEST, LOG_INFO,
			"req_reject", log_buffer);
	}
	set_err_msg(code, msgbuf, ERR_MSG_SIZE);
	if (preq->rq_reply.brp_choice != BATCH_REPLY_CHOICE_NULL) {
		/* in case another reply was being built up, clean it out */
		reply_free(&preq->rq_reply);
	}
	preq->rq_reply.brp_code    = code;
	preq->rq_reply.brp_auxcode = aux;
	if (*msgbuf != '\0') {
		preq->rq_reply.brp_choice  = BATCH_REPLY_CHOICE_Text;
		if ((preq->rq_reply.brp_un.brp_txt.brp_str = strdup(msgbuf)) == NULL) {
			log_err(-1, "req_reject", "Unable to allocate Memory!\n");
			return;
		}
		preq->rq_reply.brp_un.brp_txt.brp_txtlen = strlen(msgbuf);
	} else {
		preq->rq_reply.brp_choice  = BATCH_REPLY_CHOICE_NULL;
	}
	(void)reply_send(preq);
}

/**
 * @brief
 * 		Create a reject (error) reply for a request including
 * 		the name of the bad attribute/resource.
 *
 * @param[in]	code	- PBS error code indicating the reason the rejection
 * 							is taking place.  If this is PBSE_NONE, no log message is output.
 * @param[in]	aux	- Auxiliary error code
 * @param[in,out]	pal	- external form of attributes.
 * @param[in]	preq	- Pointer to batch request
 */
void
reply_badattr(int code, int aux, svrattrl *pal,
	struct batch_request *preq)
{
	int    i = 1;
	size_t len;
	char   msgbuf[ERR_MSG_SIZE];

	if (preq == NULL)
		return;

#ifdef NAS /* localmod 005 */
	set_err_msg(code, msgbuf, sizeof(msgbuf));
#else
	set_err_msg(code, msgbuf, ERR_MSG_SIZE);
#endif /* localmod 005 */

	while (pal) {
		if (i == aux) {
			/* append attribute info only if it fits in msgbuf */
			/* add one for space between msg and attribute name */
			len = strlen(msgbuf) + 1 + strlen(pal->al_name);
			if (pal->al_resc)
				/* add one for dot between attribute and resource */
				len += 1 + strlen(pal->al_resc);

#ifdef NAS /* localmod 005 */
			if (len < sizeof(msgbuf)) {
#else
			if (len < ERR_MSG_SIZE) {
#endif /* localmod 005 */
				(void)strcat(msgbuf, " ");
				(void)strcat(msgbuf, pal->al_name);
				if (pal->al_resc) {
					(void)strcat(msgbuf, ".");
					(void)strcat(msgbuf, pal->al_resc);
				}
			}
			break;
		}
		pal = (svrattrl *)GET_NEXT(pal->al_link);
		++i;
	}

 	(void)reply_text(preq, code, msgbuf);
}

/**
 * @brief
 * 		Return a reply with a supplied text string.
 *
 * @param[out]	preq	- Pointer to batch request
 * @param[in]	code	- PBS error code indicating the reason the rejection
 * 							is taking place.  If this is PBSE_NONE, no log message is output.
 * @param[in]	text	- text string for the reply.
 *
 * @return	error code
 */
int
reply_text(struct batch_request *preq, int code, char *text)
{
	if (preq == NULL)
		return 0;

	if (preq->rq_reply.brp_choice != BATCH_REPLY_CHOICE_NULL)
		/* in case another reply was being built up, clean it out */
		reply_free(&preq->rq_reply);

	preq->rq_reply.brp_code    = code;
	preq->rq_reply.brp_auxcode = 0;
	if (text && *text) {
		preq->rq_reply.brp_choice  = BATCH_REPLY_CHOICE_Text;
		if ((preq->rq_reply.brp_un.brp_txt.brp_str = strdup(text)) == NULL)
			return 0;
		preq->rq_reply.brp_un.brp_txt.brp_txtlen = strlen(text);
	} else {
		preq->rq_reply.brp_choice  = BATCH_REPLY_CHOICE_NULL;
	}
	return  reply_send(preq);
}

/**
 * @brief
 * 		Return a reply with the job id.
 *
 * @see req_queuejob()
 * @see req_rdytocommit()
 * @see req_commit()
 *
 * @par Side-effects:
 *		Always frees the request structure.
 *
 * @param[out]	preq	- Pointer to batch request
 * @param[in]	jobid	- job id.
 * @param[in]	which	- the union discriminator
 */
int
reply_jobid(struct batch_request *preq, char *jobid, int which)
{
	if (preq->rq_reply.brp_choice != BATCH_REPLY_CHOICE_NULL)
		/* in case another reply was being built up, clean it out */
		reply_free(&preq->rq_reply);

	preq->rq_reply.brp_code    = 0;
	preq->rq_reply.brp_auxcode = 0;
	preq->rq_reply.brp_choice  = which;
	(void)strncpy(preq->rq_reply.brp_un.brp_jid, jobid, PBS_MAXSVRJOBID);
	return (reply_send(preq));
}