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

#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include "libpbs.h"
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

/**
 * @brief
 *      set_job - set up a new job session
 *      Set session id and whatever else is required on this machine
 *      to create a new job.
 *
 * @param[in]   pjob - pointer to job structure
 * @param[in]   sjr  - pointer to startjob_rtn structure
 *
 * @return      session/job id or if error:
 * @retval      -1 - if setsid() fails
 * @retval      -2 - if other, message in log_buffer
 *
 * The NEC requires the parent of each job be different so we
 * fork and let the child take over while the original job proc
 * just waits for the new kid.
 */

int set_job(job *pjob, struct startjob_rtn *sjr)
{
	pid_t	kid;

	sjr->sj_parent = getpid();
	if ((kid = fork()) == -1) {
		sprintf(log_buffer, "fork errno %d", errno);
		return -2;
	}
	if (kid > 0) {		/* parent */
		/*
		 **	The only purpose for this is to wait for
		 **	the real job to exit.  After the waitpid
		 **	is done, it must exit.
		 */
		int	statloc, exiteval;
		int	i;

		for (i=0; i<30; i++)
			close(i);
		if (waitpid(kid, &statloc, 0) == -1) {
			sprintf(log_buffer, "waitpid errno %d", errno);
			log_err(errno, "set_job", log_buffer);
			exit(13);
		}
		if (WIFEXITED(statloc))
			exiteval = WEXITSTATUS(statloc);
		else if (WIFSIGNALED(statloc))
			exiteval = WTERMSIG(statloc) + 10000;
		else
			exiteval = 1;
		exit(exiteval);
	}

	/* child becomes the job */
	if ((sjr->sj_session = setsid()) == -1)
		return -1;
	if ((sjr->sj_jid = setjid()) == -1) {
		sprintf(log_buffer, "setjid errno %d", errno);
		return -2;
	}
	return (sjr->sj_session);
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
set_globid( job *pjob, struct startjob_rtn *sjr)
{
	return;
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

/**
 * @brief
 *      sets the shell to be used
 *
 * @param[in] pjob - pointer to job structure
 * @param[in] pwdp - pointer to passwd structure
 *
 * @return      string
 * @retval      shellname       Success
 *
 */

char *set_shell(job *pjob, struct passwd  *pwdp)
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
	int		exiteval;
	pid_t		pid;
	job		*pjob;
	pbs_task		*ptask;
	int		statloc;

	/* update the latest intelligence about the running jobs;         */
	/* must be done before we reap the zombies, else we lose the info */

	termin_child = 0;

	if (mom_get_sample() == PBSE_NONE) {
		pjob = (job *)GET_NEXT(svr_alljobs);
		while (pjob) {
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
			if ((pid == pjob->ji_momsubt) != 0)
				break;
			/*
			 ** look for task
			 */
			ptask = (pbs_task *)GET_NEXT(pjob->ji_tasks);
			while (ptask) {
				if (ptask->ti_qs.ti_u.ti_ext.ti_parent == pid)
					break;
				if (ptask->ti_qs.ti_sid == pid)
					break;
				ptask = (pbs_task *)GET_NEXT(ptask->ti_jobtask);
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
				__func__, pid, exiteval))
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
		DBPRT(("%s: task %08.8X pid %d exit value %d\n", __func__,
			ptask->ti_qs.ti_task, pid, exiteval))
		kill_session(ptask->ti_qs.ti_sid, SIGKILL, 0);
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
 *      This is code adapted from an example for posix_openpt in
 *      The Open Group Base Specifications Issue 6.
 *
 *      On success, this function returns an open descriptor for the
 *      master pseudotty and places a pointer to the (static) name of
 *      the slave pseudotty in *rtn_name;  on failure, -1 is returned.
 *
 * @param[in] rtn_name - holds info tty
 *
 * @return      int
 * @retval      fd      Success
 * @retval      -1      Failure
 *
 */

int
open_master(char **rtn_name)
{
	int   ptc;

	if ((ptc = open("/dev/ptmx", O_RDWR | O_NOCTTY, 0)) < 0)
		return -1;
	if (unlockpt(ptc) == -1) {
		close(ptc);
		return -1;
	}

	*rtn_name  = ptsname(ptc);	/* name of slave side */
	return (ptc);
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
	{ "XCPU", SIGXCPU },
	{ "XFSZ", SIGXFSZ },
	{(char *)0, -1}
};
