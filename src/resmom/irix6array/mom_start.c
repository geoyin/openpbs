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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syssgi.h>
#include <sys/arsess.h>
#include <errno.h>
#include <fcntl.h>
#include <proj.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include "portability.h"
#include "libpbs.h"
#include "list_link.h"
#include "log.h"
#include "server_limits.h"
#include "attribute.h"
#include "resource.h"
#include "job.h"
#include "mom_mach.h"
#include "mom_func.h"

/**
 * @file
 */
/* Global Variables */

extern int	 exiting_tasks;
extern char	 mom_host[];
extern pbs_list_head svr_alljobs;
extern int	 termin_child;

/* Private variables */

static PROJ sgiprojtoken;

/**
 * @brief
 *	open_sgi_proj() - open access to project files for use by set_sgi_proj()
 *
 * @return	int
 * @retval	0	Success
 * @retval	-1	Error
 *
 */

int
open_sgi_proj()
{
	sgiprojtoken = openproj(NULL, NULL);
	if (sgiprojtoken == NULL) {
		log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER, LOG_ALERT,
			"open_sgi_proj", "could not open project id files");
		return -1;
	} else {
		return 0;
	}
}

/**
 * @brief
 * 	set_sgi_proj - set SGI project id for job/task
 *
 *	As a side effect, the access to the project files is closed.
 * @param[in] usern - he user name
 * @param[in] acct - the account attribute or NULL
 *
 * @return      int
 * @retval      0       Success
 * @retval      -1      Error
 *
 */

int
set_sgi_proj(cahr *usern, attribute *acct)
{
	prid_t    prid;

	if ((acct == 0) || ((acct->at_flags & ATR_VFLAG_SET) == 0)) {

		/* use default projid for user */

		prid = fgetdfltprojuser(sgiprojtoken, usern);
	} else {

		/* use Account as project name, if valid --- convert to id */

		prid = fvalidateproj(sgiprojtoken, usern, acct->at_val.at_str);
	}

	if ((prid == -1) || (setprid(prid) == -1))
		return -1;
	closeproj(sgiprojtoken);
	return 0;
}

/**
 * set_job - set up a new job session
 * 	Set session id and whatever else is required on this machine
 *	to create a new job.
 *
 * @param[in]   pjob - pointer to job structure
 * @param[in]   sjr  - pointer to startjob_rtn structure
 *
 * @return session/job id
 * @retval -1 error from setsid(), no message in log_buffer
 * @retval -2 temporary error, retry job, message in log_buffer
 * @retval -3 permanent error, abort job, message in log_buffer
 *
 */

int
set_job(job *pjob, struct startjob_rtn *sjr)

{
	extern	char	noglobid[];
	char		cvtbuf[20];

	if (pjob->ji_globid == NULL ||
		strcmp(pjob->ji_globid, noglobid) == 0) {
		if (newarraysess() == -1) {
			sprintf(log_buffer, "newarraysess failed %d", errno);
			return (-2);
		}
		sjr->sj_ash = getash();
	}
	else {
		if (sscanf(pjob->ji_globid, "%llx", &sjr->sj_ash) != 1) {
			sprintf(log_buffer, "invalid global id %s",
				pjob->ji_globid);
			return (-2);
		}
		if (syssgi(SGI_JOINARRAYSESS, -1, &sjr->sj_ash) == -1) {
			sprintf(log_buffer, "cannot set ash %llx err %d",
				sjr->sj_ash, errno);
			return (-2);
		}
	}

	if (set_sgi_proj(pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str,
		&pjob->ji_wattr[(int)JOB_ATR_account]) < 0) {
		(void)sprintf(log_buffer, "Invalid project id");
		return (-3);
	}
	return (sjr->sj_session = setsid());
}

/**
 * @brief
 *      set_globid - set the global id for a machine type.
 *
 * @param[in] pjob - pointer to job structure
 * @param[in] sjr  - pointer to startjob_rtn structure
 *
 * @return Void
 *
 */

void
set_globid(job *pjob, struct startjob_rtn *sjr)
{
	char cvtbuf[20];

	(void)sprintf(cvtbuf, "%llx", sjr->sj_ash);
	if (pjob->ji_globid)
		free(pjob->ji_globid);
	pjob->ji_globid = strdup(cvtbuf);
	(void)decode_str(&pjob->ji_wattr[JOB_ATR_altid],
		ATTR_altid, NULL, cvtbuf);
}

/**
 * @brief
 *      set_mach_vars - setup machine dependent environment variables
 *
 * @param[in] pjob - pointer to job structure
 * @param[in] vtab - pointer to var_table structure
 *
 * @return      int
 * @retval      0       Success
 *
 */

int
set_mach_vars(job *pjob, struct var_table *vtab)
{
	return 0;
}

char *
set_shell(pjob, pwdp)
job	      *pjob;
struct passwd *pwdp;
{
	char *cp;
	int   i;
	char *shell;
	struct array_strings *vstrs;
	/*
	 * find which shell to use, one specified or the login shell
	 */

	shell = pwdp->pw_shell;
	if ((pjob->ji_wattr[(int)JOB_ATR_shell].at_flags & ATR_VFLAG_SET) &&
		(vstrs = pjob->ji_wattr[(int)JOB_ATR_shell].at_val.at_arst)) {
		for (i = 0; i < vstrs->as_usedptr; ++i) {
			cp = strchr(vstrs->as_string[i], '@');
			if (cp) {
				if (!strncmp(mom_host, cp+1, strlen(cp+1))) {
					*cp = '\0';	/* host name matches */
					shell = vstrs->as_string[i];
					break;
				}
			} else {
				shell = vstrs->as_string[i];	/* wildcard */
			}
		}
	}
	return (shell);
}

/**
 * @brief
 * 	scan_for_terminated - scan the list of runnings jobs for one whose
 *	session id matched that of a terminated child pid.  Mark that
 *	job as Exiting.
 *
 * @return	Void
 *
 */

void
scan_for_terminated()
{
	static	char	id[] = "scan_for_terminated";
	int		exiteval;
	pid_t		pid;
	job		*pjob;
	task		*ptask;
	int		statloc;

	/* update the latest intelligence about the running jobs;         */
	/* must be done before we reap the zombies, else we lose the info */

	termin_child = 0;

	if (mom_get_sample() == PBSE_NONE) {
		pjob = (job *)GET_NEXT(svr_alljobs);
		while (pjob) {
			if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_RUNNING)
				mom_set_use(pjob);
			pjob = (job *)GET_NEXT(pjob->ji_alljobs);
		}
	}

	/* Now figure out which task(s) have terminated (are zombies) */

	while ((pid = waitpid(-1, &statloc, WNOHANG)) > 0) {

		pjob = (job *)GET_NEXT(svr_alljobs);
		while (pjob) {
			/*
			 ** see if process was a child doing a special
			 ** function for MOM
			 */
			if (pid == pjob->ji_momsubt)
				break;
			/*
			 ** look for task
			 */
			ptask = (task *)GET_NEXT(pjob->ji_tasks);
			while (ptask) {
				if (ptask->ti_qs.ti_sid == pid)
					break;
				ptask = (task *)GET_NEXT(ptask->ti_jobtask);
			}
			if (ptask != NULL)
				break;
			pjob = (job *)GET_NEXT(pjob->ji_alljobs);
		}
		if (WIFEXITED(statloc))
			exiteval = WEXITSTATUS(statloc);
		else if (WIFSIGNALED(statloc))
			exiteval = WTERMSIG(statloc) + 10000;
		else
			exiteval = 1;

		if (pjob == NULL) {
			DBPRT(("%s: pid %d not tracked, exit %d\n",
				id, pid, exiteval))
			continue;
		}

		if (pid == pjob->ji_momsubt) {
			pjob->ji_momsubt = 0;
			if (pjob->ji_mompost) {
				pjob->ji_mompost(pjob, exiteval);
			}
			(void)job_save(pjob, SAVEJOB_QUICK);
			continue;
		}
		/*
		 ** We found task within the job which has exited.
		 */
		DBPRT(("%s: task %08.8X pid %d exit value %d\n", id,
			ptask->ti_qs.ti_task, pid, exiteval))
		kill_task(ptask, SIGKILL);
		ptask->ti_qs.ti_exitstat = exiteval;
		ptask->ti_qs.ti_status = TI_STATE_EXITED;
		(void)task_save(ptask);
		sprintf(log_buffer, "task %08.8X terminated",
			ptask->ti_qs.ti_task);
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_DEBUG,
			pjob->ji_qs.ji_jobid, log_buffer);

		exiting_tasks = 1;
	}
}


/**
 * @brief
 *      creat the master pty, this particular
 *      piece of code depends on multiplexor /dev/ptc
 *
 * @param[out] rtn_name - holds info of tty
 *
 * @return      int
 * @retval      fd      Success
 * @retval      -1      Failure
 *
 */

int
open_master(char **rtn_name)

{
	int	fds;

	*rtn_name = _getpty(&fds, O_RDWR | O_NOCTTY, 0600, 1);
	if (*rtn_name == (char *)0)
		return (-1);
	else
		return (fds);
}


/*
 * struct sig_tbl = map of signal names to numbers,
 * see req_signal() in ../requests.c
 */

struct sig_tbl sig_tbl[] = {
	{ "NULL", 0 },
	{ "HUP", SIGHUP },
	{ "INT", SIGINT },
	{ "QUIT", SIGQUIT },
	{ "ILL", SIGILL },
	{ "TRAP", SIGTRAP },
	{ "IOT", SIGIOT },
	{ "ABRT", SIGABRT },
	{ "EMT", SIGEMT },
	{ "FPE", SIGFPE },
	{ "KILL", SIGKILL },
	{ "BUS", SIGBUS },
	{ "SEGV", SIGSEGV },
	{ "SYS", SIGSYS },
	{ "PIPE", SIGPIPE },
	{ "ALRM", SIGALRM },
	{ "TERM", SIGTERM },
	{ "USR1", SIGUSR1 },
	{ "USR2", SIGUSR2 },
	{ "CHLD", SIGCHLD },
	{ "PWR", SIGPWR },
	{ "WINCH", SIGWINCH },
	{ "URG", SIGURG },
	{ "POLL", SIGPOLL },
	{ "IO", SIGIO },
	{ "STOP", SIGSTOP },
	{ "TSTP", SIGTSTP },
	{ "CONT", SIGCONT },
	{ "TTIN", SIGTTIN },
	{ "TTOU", SIGTTOU },
	{ "VTALRM", SIGVTALRM },
	{ "PROF", SIGPROF },
	{ "XCPU", SIGXCPU },
	{ "XFSZ", SIGXFSZ },
	{(char *)0, -1}
};
