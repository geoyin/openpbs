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
/**
 * @file	w32_send_hooks.c
 * @brief
 * The entry point function for pbs_daemon.
 *
 * @par Included public functions re:
 *
 *	main	initialization and main loop of pbs_daemon
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <sys/stat.h>

#include "pbs_ifl.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <io.h>
#include <windows.h>
#include "win.h"

#include "list_link.h"
#include "work_task.h"
#include "log.h"
#include "server_limits.h"
#include "attribute.h"
#include "job.h"
#include "reservation.h"
#include "credential.h"
#include "ticket.h"
#include "queue.h"
#include "server.h"
#include "libpbs.h"
#include "net_connect.h"
#include "batch_request.h"
#include "svrfunc.h"
#include "tracking.h"
#include "acct.h"
#include "sched_cmds.h"
#include "rpp.h"
#include "dis.h"
#include "dis_init.h"
#include "pbs_ifl.h"
#include "pbs_license.h"
#include "resource.h"
#include "pbs_version.h"
#include "hook.h"
#include "pbs_python.h"
#include "pbs_client_thread.h"
#include "pbs_ecl.h"
#include  "pbs_nodes.h"
#include "hook_func.h"


/* Global Data Items */
char		*path_hooks = NULL;
char		*path_hooks_tracking = NULL;
char		*path_hooks_workdir = NULL;
char		path_log[MAXPATHLEN+1];
unsigned int      pbs_server_port_dis = 0;
pbs_net_t       pbs_server_addr = 0;
char		*path_priv;
char		*path_hooks_rescdef;
char	       *acct_file = (char *)0;
char	       *acctlog_spacechar = (char *)0;
int		license_cpus;
char	       *log_file  = (char *)0;
char	       *path_acct;
char	       *path_jobs = NULL;
char	       *path_rescdef = NULL;
char	       *path_users = NULL;
char	       *path_spool = NULL;
char 	       *path_track = NULL;
char           *path_secondaryact;
char	       *pbs_o_host = "PBS_O_HOST";
pbs_net_t	pbs_mom_addr = 0;
unsigned int	pbs_mom_port = 0;
unsigned int	pbs_rm_port = 0;
pbs_net_t	pbs_scheduler_addr = 0;
unsigned int	pbs_scheduler_port = 0;
struct python_interpreter_data  svr_interp_data;

struct server	server;		/* the server structure */
struct pbs_sched	*dflt_scheduler;	/* the sched structure */
char	        server_host[PBS_MAXHOSTNAME+1];	/* host_name  */
char	        primary_host[PBS_MAXHOSTNAME+1]; /* host_name of primary */
int		shutdown_who;		/* see req_shutdown() */
char	       *mom_host = server_host;
long		new_log_event_mask = 0;
int	 	server_init_type = RECOV_WARM;
char	        server_name[PBS_MAXSERVERNAME+1]; /* host_name[:service|port] */
int		svr_delay_entry = 0;
int		svr_do_schedule = SCH_SCHEDULE_NULL;
int		svr_do_sched_high = SCH_SCHEDULE_NULL;
int		svr_total_cpus = 0;		/* total number of cpus on nodes   */
int		have_blue_gene_nodes = 0;
int		svr_ping_rate = SVR_DEFAULT_PING_RATE;	/* time between sets of node pings */
int 		ping_nodes_rate = SVR_DEFAULT_PING_RATE; /* time between ping nodes as determined from server_init_type */
/* The following are defined to resolve external reference errors in windows build */
char		*path_svrlive;		/* the svrlive file used for monitoring during failover */
char		*pbs_server_name; /* pbs server name */
char		*pbs_server_id; /* pbs server database id */
char		*path_svrdb;  /* path to server db */
char		*path_nodestate; /* path to node state file */
char		*path_nodes; /* path to nodes file */
char		*path_resvs; /* path to resvs directory */
extern void	*AVL_jctx = NULL; /* used for the jobs AVL tree */
/*
 * Used only by the TPP layer, to ping nodes only if the connection to the
 * local router to the server is up.
 * Initially set the connection to up, so that first time ping happens
 * by default.
 */
int tpp_network_up = 0;

int		svr_unsent_qrun_req = 0;
long		svr_history_enable = 0;
long		svr_history_duration = 0;

struct license_block licenses;
struct license_used  usedlicenses;
struct resc_sum *svr_resc_sum;
attribute      *pbs_float_lic;
pbs_list_head	svr_queues;            /* list of queues                   */
pbs_list_head	svr_allscheds;         /* list of schedulers               */
pbs_list_head	svr_alljobs;           /* list of all jobs in server       */
pbs_list_head	svr_newjobs;           /* list of incomming new jobs       */
pbs_list_head	svr_allresvs;          /* all reservations in server */
pbs_list_head	svr_newresvs;          /* temporary list for new resv jobs */
pbs_list_head	svr_allhooks;	       /* list of all hooks in server       */
pbs_list_head	svr_allhooks;
pbs_list_head	svr_queuejob_hooks;
pbs_list_head	svr_modifyjob_hooks;
pbs_list_head	svr_resvsub_hooks;
pbs_list_head	svr_movejob_hooks;
pbs_list_head	svr_runjob_hooks;
pbs_list_head	svr_provision_hooks;
pbs_list_head	svr_periodic_hooks;
pbs_list_head	svr_execjob_begin_hooks;
pbs_list_head	svr_execjob_prologue_hooks;
pbs_list_head	svr_execjob_launch_hooks;
pbs_list_head	svr_execjob_epilogue_hooks;
pbs_list_head	svr_execjob_preterm_hooks;
pbs_list_head	svr_execjob_end_hooks;
pbs_list_head	svr_exechost_periodic_hooks;
pbs_list_head	svr_exechost_startup_hooks;
pbs_list_head	svr_execjob_attach_hooks;

pbs_list_head	task_list_immed;
pbs_list_head	task_list_timed;
pbs_list_head	task_list_event;
pbs_list_head   	svr_deferred_req;
pbs_list_head   	svr_unlicensedjobs;	/* list of jobs to license */
time_t		time_now;
time_t		jan1_yr2038;
time_t          secondary_delay = 30;
struct batch_request    *saved_takeover_req;
struct python_interpreter_data  svr_interp_data;

pbs_db_conn_t *svr_db_conn;

/**
 * @brief
 *	just a dummy entry for pbs_close_stdfiles since needed by failover.obj 
 */
void
pbs_close_stdfiles(void)
{
	return;
}


/**
 * @brief
 * 	needed by failover.obj 
 */
void
make_server_auto_restart(int confirm)
{
	return;
}

/** 
 * @brief
 *	needed by svr_chk_owner.obj and user_func.obj 
 */
int
decrypt_pwd(char *crypted, size_t len, char **passwd)
{
	return (0);
}

/**
 * @brief
 * 	needed by *_recov_db.obj 
 */
void
panic_stop_db(char *txt)
{
	return;
}

/**
 * @brief
 * 	main - the initialization and main loop of pbs_daemon
 */
int
main(int argc, char *argv[])
{

	char		buf[4096];
	char		*param_name, *param_val;
	int		rc;

	execution_mode(argc, argv);

	if(set_msgdaemonname("PBS_send_hooks")) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}


	pbs_loadconf(0);

	/* If we are not run with real and effective uid of 0, forget it */
	if (!isAdminPrivilege(getlogin())) {
		fprintf(stderr, "%s: Must be run by root\n", argv[0]);
		exit(-1);
	}

	pbs_client_thread_set_single_threaded_mode();
	/* disable attribute verification */
	set_no_attribute_verification();

	/* initialize the thread context */
	if (pbs_client_thread_init_thread_context() != 0) {
		fprintf(stderr, "%s: Unable to initialize thread context\n",
			argv[0]);
		exit(-1);
	}

	winsock_init();

	connection_init();

	while (fgets(buf, sizeof(buf), stdin) != NULL) {
		buf[strlen(buf)-1] = '\0';	/* gets rid of newline */

		param_name = buf;
		param_val = strchr(buf, '=');
		if (param_val) {
			*param_val = '\0';
			param_val++;
		} else {	/* bad param_val -- skipping */
			break;
		}

		if (strcmp(param_name, "path_log") == 0) {

			path_log[0] = '\0';
			strncpy(path_log, param_val, MAXPATHLEN);
		} else if (strcmp(param_name, "path_hooks") == 0) {

			path_hooks = strdup(param_val);
			if (path_hooks == NULL)
				exit(-1);
		} else if (strcmp(param_name, "log_file") == 0) {
			log_file = strdup(param_val);
			if (log_file == NULL)
				exit(-1);
		} else if (strcmp(param_name, "path_hooks_tracking") == 0) {
			path_hooks_tracking = strdup(param_val);
			if (path_hooks_tracking == NULL)
				exit(-1);
		} else if (strcmp(param_name, "hook_action_tid") == 0) {
#ifdef WIN32
			hook_action_tid_set(_atoi64(param_val));
#else
			hook_action_tid_set(atoll(param_val));
#endif
		} else if (strcmp(param_name, "pbs_server_port_dis") == 0) {
			pbs_server_port_dis = atoi(param_val);
		} else if (strcmp(param_name, "pbs_server_addr") == 0) {
			pbs_server_addr = atol(param_val);
		} else
			break;
	}

	(void)log_open_main(log_file, path_log, 1); /* silent open */


	hook_track_recov();

	rc = sync_mom_hookfiles(NULL);


	log_close(0);	/* silent close */
	net_close(-1);
	if (log_file != NULL)
		free(log_file);
	if (path_hooks != NULL)
		free(path_hooks);
	if (path_hooks_tracking != NULL)
		free(path_hooks_tracking);
	exit(rc);


}
