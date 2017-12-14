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
#include <sys/syssgi.h>
#include <sys/arsess.h>
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
#include <optional_sym.h>
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

#include "mom_share.h"
#include "cpusets.h"
#include "allocnodes.h"
#include "collector.h"
#include "hammer.h"
#include "mapnodes.h"

/**
 * @file
 */
/* Global Variables */

extern int	 exiting_tasks;
extern char	 mom_host[];
extern pbs_list_head svr_alljobs;
extern int	 termin_child;
extern struct var_table vtable;

/* The "available" pool of nodes to be allocated for jobs. */
extern Bitfield nodepool;		/* Nodes available for cpusets. */

/*
 * Assign and create a cpuset for the job.  Attach the current process to
 * that process.  Return the nodes in the cpuset in the pointed-to bitfield.
 */
int	assign_cpuset(job *, Bitfield *, char *, cpuset_shared *);
extern int	getlong(resource *, unsigned long *);
int	note_nodemask(job *, char *);

char *obtain_vnames[NUM_LCL_ENV_VAR] = {
	"LANG",
	"LC_ALL",
	"LC_COLLATE",
	"LC_CTYPE",
	"LC_MONETARY",
	"LC_NUMERIC",
	"LC_TIME",
	"PATH",
	"TZ" ,
	(char *)0
};

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
/* Private variables */

/**
 * @brief
 *      set_sgi_proj - set SGI project id for job/task
 *
 *      As a side effect, the access to the project files is closed.
 *
 * @param[in] usern - he user name
 * @param[in] acct - the account attribute or NULL
 *
 * @return      int
 * @retval      0       Success
 * @retval      -1      Error
 *
 */

int
set_sgi_proj(char *usern, attribute *acct)

{
	prid_t    prid;

	if ((acct == 0) || ((acct->at_flags & ATR_VFLAG_SET) == 0)) {

		/* use default projid for user */

		prid = getdfltprojuser(usern);
	} else {

		/* use Account as project name, if valid --- convert to id */

		prid = validateproj(usern, acct->at_val.at_str);
	}

	if ((prid == -1) || (setprid(prid) == -1))
		return -1;
	return 0;
}

/**
 * @brief
 *      Set session id and whatever else is required on this machine
 *      to create a new job.
 *      On a Cray, an ALPS reservation will be created and confirmed.
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
	Bitfield	nodes;
	int		rv;
	jid_t		sgijid;
	char 		jl_domain[PBS_MAXQUEUENAME+15] = "PBS_";
	static int	jlimiterr = 0;

	/* Set up SGI Job container */

	if (pjob->ji_extended.ji_ext.ji_jid > 0) {

		/*
		 * already have a job id - from Mother Superior
		 * join it or create one with that id
		 */

		sjr->sj_jid = pjob->ji_extended.ji_ext.ji_jid;

		if (_MIPS_SYMBOL_PRESENT(getjid) && _MIPS_SYMBOL_PRESENT(makenewjob)) {
			/* we are on a system that knows about job limits */

			if ((getjid() != pjob->ji_extended.ji_ext.ji_jid) &&
				(syssgi(SGI_JOINJOB, pjob->ji_extended.ji_ext.ji_jid)!=0)) {
				/* attempt to join job failed */
				if (errno == ENOPKG) {
					log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB,
						LOG_INFO,
						pjob->ji_qs.ji_jobid,
						"job limits ENOPKG");
				} else {
					/* have to use makenewjob() to force jid */
					sgijid = makenewjob(pjob->ji_extended.ji_ext.ji_jid,
						pjob->ji_qs.ji_un.ji_momt.ji_exuid);
					if (sgijid != pjob->ji_extended.ji_ext.ji_jid) {
						/* bad news */
						(void)sprintf(log_buffer,
							"join job limits failed: %d",
							errno);
						log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB,
							LOG_INFO,
							pjob->ji_qs.ji_jobid,
							log_buffer);
						return (-2);
					}
				}
			}	/* joined exiting job ok */
		}		/* end of have existing jid */

	} else if (_MIPS_SYMBOL_PRESENT(getjid) &&
		_MIPS_SYMBOL_PRESENT(jlimit_startjob)) {
		/* set up new job id */
		(void)strcat(jl_domain,		/* PBS_{queue_name} */
			pjob->ji_wattr[(int)JOB_ATR_in_queue].at_val.at_str);
		(void)strcat(jl_domain, ":PBS:batch");
		sgijid = jlimit_startjob(
			pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str,
			pjob->ji_qs.ji_un.ji_momt.ji_exuid,
			jl_domain);
		if (sgijid > 0) {
			/* valid job */
			sjr->sj_jid = sgijid;
			sprintf(log_buffer, "set jobid 0x%llx", sgijid);
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB,
				LOG_INFO,
				pjob->ji_qs.ji_jobid,
				log_buffer);
		} else if (errno == ENOPKG) {
			if (jlimiterr == 0) {
				/* startjob failed */
				log_err(errno, "set_job", "jlimit_startjob failed");
				jlimiterr = 1;
			}
		} else {
			/* startjob failed */
			log_err(errno, "set_job", "jlimit_startjob failed");
			return (-1);
		}
	}
	rv = -1;

	/* if there is an existing array for this job, join it */

	if ((pjob->ji_extended.ji_ext.ji_ash != 0) &&
		(getash() != pjob->ji_extended.ji_ext.ji_ash)) {
		rv = syssgi(SGI_JOINARRAYSESS, 0,
			&pjob->ji_extended.ji_ext.ji_ash);
	}
	if (rv < 0) {
		/* join failed or no session - create new array session */
		if (newarraysess() == -1) {
			(void)sprintf(log_buffer, "newarraysess failed, err=%d",
				errno);
			return (-2);
		}
	}

	sjr->sj_ash = getash();

	if ((pjob->ji_extended.ji_ext.ji_ash != 0) &&
		(sjr->sj_ash != pjob->ji_extended.ji_ext.ji_ash)) {
		/* may not have arrayd running here */
		/* try to force ash 		    */
		if (setash(pjob->ji_extended.ji_ext.ji_ash) < 0) {
			sprintf(log_buffer, "setash failed to %lld, err %d",
				pjob->ji_extended.ji_ext.ji_ash, errno);
			return (-2);
		} else {
			sjr->sj_ash = pjob->ji_extended.ji_ext.ji_ash;
		}
	}

	if (set_sgi_proj(pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str,
		&pjob->ji_wattr[(int)JOB_ATR_account]) < 0) {
		(void)sprintf(log_buffer, "Invalid project id %s/%s: %s",
			pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str,
			pjob->ji_wattr[(int)JOB_ATR_account].at_val.at_str,
			strerror(errno));
		return (-3);
	}

	sjr->sj_session = setsid();

	if (enforce_cpusets) {
		/*
		 * This is in the child process.
		 *
		 * Assign and create a cpuset for this process.  If successful,
		 * pass the assigned nodes back to the main mom in the starter
		 * job return.
		 */
		if (assign_cpuset(pjob, &nodes,
			(char *)sjr->sj_cpuset_name,
			&(sjr->sj_shared_cpuset_info))) {
			(void)sprintf(log_buffer,
				"Cannot assign cpuset to %s: %s",
				pjob->ji_qs.ji_jobid, strerror(errno));
			return (-2);
		}
		BITFIELD_CPY(&sjr->sj_nodes, &nodes);
	}

	return (sjr->sj_session);
}

/**
 * @brief
 * 	Attempt to clean up the cpuset for the job.  Once it
 * 	has been deleted, the nodes will be placed back on
 * 	the free nodepool mask.
 *
 * @retuan	Void
 *
 */

void
clear_cpuset(job *pjob)
{

	cpusetlist      *cpuset;
	char            *qname;
	cpuset_shared	share;

	cpuset = find_cpuset_byjob(inusecpusets, pjob->ji_qs.ji_jobid);
	if (cpuset == NULL) {
		char *qn;
		cpuset_CPUList_t *cpus = NULL;

		/* the job_to_qname() call will ensure that the returned */
		/* cpuset name (qn) is not in the inusecpusets list */
		/* thus preventing existing shared cpusets with valid jobs */
		/* to be deleted. */
		if ((qn=job_to_qname(pjob)) &&
			(cpus=cpusetGetCPUList(qn)) &&
			is_cpuset_pbs_owned(qn)) {
			sprintf(log_buffer,
				"destroying phantom cpuset %s for job %s",
				qn, pjob->ji_qs.ji_jobid);
			log_err(0, "clear_cpuset", log_buffer);

			destroy_cpuset(qn);
		}

		if (cpus) cpusetFreeCPUList(cpus);

		return;
	}

	qname = cpuset->name;
	/* destroy cpuset if exclusive cpuset, or
	 * this is the  only job left in shared cpuset
	 */
	if (!cpuset->sharing ||
		cpuset_shared_get_numjobs(cpuset->sharing) == 1) {
		if (cpuset_destroy_delay > 0) {
			sleep(cpuset_destroy_delay);
		}
		if (teardown_cpuset(qname, &cpuset->nodes) > 0) {
			sprintf(log_buffer, "couldn't clean up cpuset %s",
				cpuset->name);
			log_event(PBSEVENT_DEBUG,
				PBS_EVENTCLASS_JOB, LOG_DEBUG,
				pjob->ji_qs.ji_jobid, log_buffer);
		}
	}

	if (!is_small_job(pjob, &share)) {
		cpuset_shared_unset(&share);
	}
	(void)remove_from_cpusetlist(&inusecpusets, NULL, qname, &share);
#ifdef DEBUG
	print_cpusets(inusecpusets,
		"2. INUSECPUSETS----------------------------->");
#endif
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
	char obuf[512];
	char cbuf[512];

	obuf[0] = '\0';
	/* Only do this is the cpuset was just created. */
	if (enforce_cpusets &&
		pjob->ji_qs.ji_state == JOB_STATE_RUNNING &&
		pjob->ji_qs.ji_substate == JOB_SUBSTATE_PRERUN) {
		/*
		 * This is in the parent.  See also set_job() above, where
		 * the nodes are assigned.
		 *
		 * This is here because this is one of the last things that
		 * the main mom calls before returning from starting the job.
		 * Any last-minute accounting of things passed back from the
		 * starter has to be done here.
		 *
		 * Account for the nodes used by this job.  Place the cpuset
		 * on the in-use cpusetlist and remove the nodes from the
		 * global available nodepool.
		 *
		 * This all hinges on the job creation being single-threaded
		 * from the set_job() call to the set_globid() call.
		 */

		(void)add_to_cpusetlist(&inusecpusets,
			(char *)sjr->sj_cpuset_name,
			&sjr->sj_nodes,
			&(sjr->sj_shared_cpuset_info));
#ifdef DEBUG
		print_cpusets(inusecpusets,
			"INUSECPUSETS---------------------------->");
#endif

		/* And add the nodemask field to the job resources_used list. */
		(void)note_nodemask(pjob, bitfield2hex(&sjr->sj_nodes));

		if (cpuset_shared_is_set(&sjr->sj_shared_cpuset_info)) {
			sprintf(obuf, ",cpuset=%s:%dkb/%dp",
				sjr->sj_cpuset_name,
				sjr->sj_shared_cpuset_info.free_mem,
				sjr->sj_shared_cpuset_info.free_cpus);
		} else {
			sprintf(obuf, ",cpuset=%s:%dkb/%dp",
				sjr->sj_cpuset_name,
				nodemask_tot_mem(&sjr->sj_nodes),
				nodemask_num_cpus(&sjr->sj_nodes));
		}

		BITFIELD_CLRM(&nodepool, &sjr->sj_nodes);
#ifdef	DEBUG
		(void)sprintf(log_buffer, "nodepool now %s",
			bitfield2hex(&nodepool));
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_JOB, LOG_DEBUG,
			pjob->ji_qs.ji_jobid, log_buffer);
#endif	/* DEBUG */

	}
	(void)sprintf(cbuf, "jid=0x%llx,ash=0x%llx%s", sjr->sj_jid,
		sjr->sj_ash, obuf);
	(void)decode_str(&pjob->ji_wattr[JOB_ATR_altid], ATTR_altid, NULL, cbuf);
	pjob->ji_extended.ji_ext.ji_jid = sjr->sj_jid;
	pjob->ji_extended.ji_ext.ji_ash = sjr->sj_ash;
	update_ajob_status(pjob);

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

char *
set_shell(job *pjob, struct passwd *pwdp)
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
 *	scan_for_terminated - scan the list of runnings jobs for one whose
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
	task		*ptask;
	int		statloc;
	struct sig_tbl	*sptr;
	char		*sname;

#ifdef	CLD_RLIMEXCEED
	int		sig, code;
	siginfo_t	si;
#endif	/* CLD_RLIMEXCEED */

	/* Keep the last exit/signal code from the collector thread */
	static int	last_exit = -1;
	int		tries;

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

#ifndef	CLD_RLIMEXCEED
	while ((pid = waitpid(-1, &statloc, WNOHANG)) > 0) {

		if (WIFEXITED(statloc))
			exiteval = WEXITSTATUS(statloc);
		else if (WIFSIGNALED(statloc))
			exiteval = WTERMSIG(statloc) + 10000;
		else
			exiteval = 1;

#else	/* ! CLD_RLIMEXCEED */

	/*
	 * CLD_RLIMEXCEED is a NAS-local feature that indicates that the
	 * child was terminated due to attempted overuse of resource limits.
	 * We need to use waitid(2) instead of waitpid(2) to reap processes,
	 * as waitid(2) returns the siginfo(5) information for each process
	 * as it is reaped.  If the process has exceeded its memory limit,
	 * it will be sent a SIGKILL, and the code value is CLD_RLIMEXCEED.
	 */

	while (waitid(P_ALL, 0, &si, WEXITED | WNOHANG) == 0) {

		/*
		 * Work around a bug in the IRIX6.5 kernel.  waitid()
		 * returns 0 if there are any live children, but no remaining
		 * zombies to be reaped.  The manpage states that '-1' will
		 * be returned here, but things like the Bourne shell depend
		 * on the broken semantics, so we're stuck with it.
		 *
		 * Luckily, the siginfo_t is cleared, so the si_signo field
		 * is 0, not SIGCLD, in this case.  Check for this condition
		 * to break out of the loop.
		 */
		if (si.si_signo == 0)   /* No zombies remain to be reaped */
			break;

		pid  = si.si_pid;

		/*
		 * Did child exit or die from a signal?  If signaled, find out
		 * what signal and the reason code for the signal's delivery.
		 */
		sig  = 0;
		code = 0;

		if (si.si_code == CLD_EXITED) {
			/* Child exited normally. */
			exiteval = si.si_status;

		} else {
			/* Caught a signal or coredumped. */
			sig  = si.si_status;
			code = si.si_code;

			/* Return code for PBS job is signal + 10000. */
			exiteval = sig + 10000;
		}

#endif  /* !CLD_RLIMEXCEED */

		/* Was a signal caught?  If so, attempt to look it up. */
		if (exiteval > 10000) {
			sname = "???";
			for (sptr = sig_tbl; sptr && sptr->sig_name; sptr ++)
				if (sptr->sig_val == exiteval - 10000) {
					sname = sptr->sig_name;
					break;
				}
		}

		/* Is it the hammer thread?  Note it and go on. */
		if (pid == hammer_pid) {
			hammer_pid = -1;

			if (exiteval > 10000) {
				(void)sprintf(log_buffer,
					"hammer thread (pid %d) caught SIG%s.",
					pid, sname);
			} else {
				(void)sprintf(log_buffer,
					"hammer thread (pid %d) exited with %d",
					pid, exiteval);

			}
			log_err(-1, __func__, log_buffer);

			continue;
		}

		/*
		 * The collector thread isn't strictly necessary for the proper
		 * running of jobs, but should be running to allow resource
		 * usage to be collected and enforced.  Try to restart the
		 * collector if it dies, but make a reasonable effort to avoid
		 * doing so if there is something clearly wrong.
		 */
		if (pid == collector_pid) {
			collector_pid = -1;	/* To avoid confusion later. */

			/*
			 * If the exit value was COLLECTOR_BAIL_EXIT, the
			 * collector has decided that it cannot be successfully
			 * restarted.  Don't bother attempting to do so.
			 */
			if (exiteval == COLLECTOR_BAIL_EXIT) {
				(void)sprintf(log_buffer, "collector thread (pid "
					"%d) bailed out.  Not restarting!", pid);
				log_err(-1, __func__, log_buffer);

				continue;
			}

			/*
			 * Check if the collector exited due to a signal.  If
			 * so, and it was either SIGQUIT or SIGSYS, then bail.
			 * If the signal is the same as the last one, assume
			 * something is going wrong and dont' restart it.
			 * Otherwise, just sleep a bit and restart it.
			 */
			if (exiteval > 10000) {
				(void)sprintf(log_buffer,
					"collector thread (pid %d) caught SIG%s.",
					pid, sname);
				log_err(-1, __func__, log_buffer);

				/* Handle SIGQUIT and SIGSYS specially */
				if (((exiteval - 10000) == SIGQUIT) ||
					((exiteval - 10000) == SIGSYS)) {
					log_err(-1, __func__, "QUIT - NOT RESTARTING!");
					continue;
				}

				/*
				 * See if the signal was the same as the last
				 * time.  If so, assume that there is some
				 * condition that is causing it to fail that
				 * will not go away.
				 */
				if (last_exit >= 0 && exiteval == last_exit) {
					log_err(-1, __func__, "NOT RESTARTING!");
					continue;
				}

				/* Different or first signal.  Keep it around */
				last_exit = exiteval;
			}

			/* Collector died of "natural causes", try to restart */
			(void)sprintf(log_buffer, "collector thread (pid "
				"%d) exited.  Attempting to restart...", pid);
			log_err(-1, __func__, log_buffer);

			tries = 5; /* Try to restart five times, then bail. */

			while (--tries) {
				/* Fork(), sleep 5 seconds and restart. */
				collector_pid = start_collector(5);
				if (collector_pid > 0)
					break;

				sleep(2);  /* Wait for to come back. */
			}

			if (collector_pid <= 0) {
				log_err(errno, __func__, "Could not restart collector!!");
				log_err(errno, __func__,
					"Job resource tracking/enforcement disabled.");
				(void)sprintf(log_buffer,
					"correct above system error and 'kill -HUP %d' "
					"to restart collector", getpid());
				log_err(errno, __func__, log_buffer);
			} else {
				(void)sprintf(log_buffer,
					"restarted collector thread pid %d",
					collector_pid);
				log_event(PBSEVENT_SYSTEM, 0, LOG_DEBUG, __func__, log_buffer);
			}

			continue;
		}

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
		/*
		 ** We found task within the job which has exited.
		 */
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

#ifdef  CLD_RLIMEXCEED
		/* Was the child terminated due to resource overuse? */
		if (sig == SIGKILL && code == CLD_RLIMEXCEED) {
			(void)sprintf(log_buffer,
				">> Job %s exceeded resource allocation -- killed",
				pjob->ji_qs.ji_jobid);
			(void)message_job(pjob, StdErr, log_buffer);

			log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_JOB,
				LOG_INFO, __func__, log_buffer);
		}
#endif  /* CLD_RLIMEXCEED */

		clear_cpuset(pjob);

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

/**
 * @brief
 *	filter_nodepool: removes from npool any "nodes" that belong to an
 *	existing cpuset. 
 * @param[in] npool - pointer to Bitfield structure
 *
 * @return	Void
 *
 */
static void
filter_nodepool(Bitfield *npool)
{

	Bitfield        npool_current;
	Bitfield	npool_orig;
	cpusetlist      *cset = NULL;

	BITFIELD_CPY(&npool_orig, npool);
	BITFIELD_CLRALL(&npool_current);
	if (query_cpusets(&cset, &npool_current) > 0) {
		BITFIELD_CLRM(npool, &npool_current);
		if (BITFIELD_NOTEQ(npool, &npool_orig)) {
			sprintf(log_buffer,
				"ORIG NODEPOOL[node 0..%d, 1 if free]=%s",
				BITFIELD_SIZE-1, bitfield2bin(&npool_orig));
			log_err(0, "filter_nodepool", log_buffer);
			sprintf(log_buffer,
				"ACTUAL NODEPOOL[node 0..%d, 1 if free]=%s",
				BITFIELD_SIZE-1, bitfield2bin(npool));
			log_err(0, "filter_nodepool", log_buffer);
		}
		free_cpusetlist(cset);
	}
}

/**
 * @brief
 *	assign_cpuset() - Create either a new cpuset for this job, or if the job is "small" and can
 * share a cpuset, will attach itself to an existing shared cpuset.
 *
 * The cpus/nodes for the cpuset are allocated from the nodepool, the count
 * of nodes to be allocated comes from the NODE_COUNT_RESOURCE resource if
 * to be assigned an exclusive cpuset.
 *
 * If the cpuset was created successfully, attach the current process (which
 * should be the "proto-job" shell) to the cpuset, and set the environment
 * variables "PBS_CPUSET" and "PBS_NODEMASK" to the appropriate values.
 *
 * Also, the name of the assigned cpuset is passed back.

 * The nodes assigned to the cpuset are passed back in the nodes_assn argument.
 * If job was assigned a shared cpuset, information about what was taken from
 * the cpuset is passed back in the cpuset_info argument.
 * cpuset_name argument.
 *
 * @param[in] pjob - job pointer
 * @param[out] nodes_assn - pointer to Bitfield structure
 * @param[in] cpuset_name - cpuset name 
 * @param[out] cpuset_info - shared cpuset info
 *
 * @return	int
 * @retval	0	Success
 * @retval	!0	Error
 *
 */

int
assign_cpuset(job *pjob, Bitfield *nodes_assn, char *cpuset_name, cpuset_shared *cpuset_info)
{
	unsigned long	ndreq;
	resource		*pres;
	char		path[MAXPATHLEN];
	char		owner[PBS_MAXUSER + 1], *from;
	int			i;
	cpuset_shared	share_req;
	cpusetlist		*cset = NULL;
	char		qname[QNAME_STRING_LEN + 1];
	char		*jobid;
	uid_t		uid = 0;
	gid_t		gid = 0;

	jobid = pjob->ji_qs.ji_jobid;
	cpuset_shared_unset(cpuset_info);
	/* Find the job's requested node count. ssinodes resource takes */
	/* precedence. If ssinodes is found, then proceed to assign an  */
	/* exclusive cpuset. */

	ndreq = pjob->ji_hosts[pjob->ji_nodeid].hn_nrlimit.rl_ssi;

	if (ndreq == 0) {	/* didn't found ssinodes */
		if (is_small_job(pjob, &share_req)) {
			cpuset_info->free_cpus = share_req.free_cpus;
			cpuset_info->free_mem = share_req.free_mem;
			cpuset_info->time_to_live = share_req.time_to_live;
			strcpy(cpuset_info->owner, (char *)pjob->ji_qs.ji_jobid);
			/* mem request will determine # of nodes, for  ncpus is */
			/* always within one nodeboard */
			ndreq = share_req.free_mem/(maxnodemem*1024);
			if ((share_req.free_mem %(maxnodemem*1024)) > 0)
				ndreq++;
			if (cset=find_cpuset_shared(inusecpusets, &share_req)) {
				/* technically, no *new* node bits are assigned */
				/* we're reusing nodes of a pre-allocated cpuset */
				BITFIELD_CLRALL(nodes_assn);
				strcpy(qname, cset->name);
			}
		} else {

			log_err(-1, __func__,
				"cannot find \"" NODE_COUNT_RESOURCE "\" resource");
			return 1;
		}
	}

	/* create a cpuset */
	if (!cset) {
		int	node0;
		char	*qn;
		/*
		 * Attempt to allocate the requested node count from the nodepool.
		 *
		 * NOTE NOTE NOTE!  This is the child, the nodepool being operated upon
		 * is a copy of the main mom's version!  Changes must be propagated back
		 * into the main mom.
		 */
		if ( cpuset_shared_is_set(cpuset_info) && \
		(shared_nnodes(inusecpusets)+ndreq) > max_shared_nodes ) {
			(void)sprintf(log_buffer,
				"allocating %lu shared nodes for job %s will exceed max_shared_nodes=%d", ndreq, jobid, max_shared_nodes);
			log_err(-1, __func__, log_buffer);
			errno = EAGAIN;
			return 1;
		}

		filter_nodepool(&nodepool);

		/* idea here is to not allocate node 0 as much as possible.
		 * try to fit without node 0, but if it can't, then we try to
		 * use it.
		 */
		if ((node0=BITFIELD_TSTB(&nodepool, 0))) {
			BITFIELD_CLRB(&nodepool, 0);
		}

		if (alloc_nodes((int)ndreq, &nodepool, nodes_assn) != ndreq) {

			if (node0) BITFIELD_SETB(&nodepool, 0);

			if (!node0 ||
				alloc_nodes((int)ndreq, &nodepool, nodes_assn) != ndreq) {
				(void)sprintf(log_buffer,
					"cannot allocate %lu nodes for job %s",
					ndreq, jobid);
				log_err(-1, __func__, log_buffer);
				errno = EAGAIN;
				return 1;
			}

		}

		/*
		 * Create a cpuset owned by the user, named after the job, with
		 * the cpus associated with the assigned nodes.  The controlling file
		 * is just the job id with a '.cq' tacked on the end.  It is unlinked
		 * immediately after creation, so the real filename shouldn't be an
		 * issue at all.
		 */
		uid = pjob->ji_qs.ji_un.ji_momt.ji_exuid;
		gid = pjob->ji_qs.ji_un.ji_momt.ji_exgid;
		qn = job_to_qname(pjob);
		if (qn == NULL) {
			(void)sprintf(log_buffer,
				"unable to assign a cpuset name for job %s", jobid);
			log_err(-1, __func__, log_buffer);
			return 1;
		}

		(void)strncpy(qname, qn, QNAME_STRING_LEN);
		qname[QNAME_STRING_LEN] = '\0';

		cpuset_permfile(qname, path);

		if (create_cpuset(qname, nodes_assn, path, uid, gid)) {
			return 1;
		}

		from = pjob->ji_wattr[(int)JOB_ATR_job_owner].at_val.at_str;
		for (i=0; i<PBS_MAXUSER; ++i) {
			if ((*(from+i) == '@') || (*(from+i) == '\0'))
				break;
			owner[i] = *(from+i);
		}
		owner[i] = '\0';

		(void)sprintf(log_buffer,
			"cpusetCreate %s succeeded (user %s uid %d gid %d p/sid %d/%d)",
			qname, owner, uid, gid, getpid(), getsid(0));
		log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO, jobid,
			log_buffer);

	}

	/*
	 * Attach this process to the just-created cpuset.  If it fails, cleanup
	 * and bail.
	 */
	if (attach_cpuset(qname)) {
		(void)sprintf(log_buffer, "cannot attach to cpuset %s", qname);
		log_err(errno, __func__, log_buffer);

		if (destroy_cpuset(qname) && errno != ESRCH) {
			(void)sprintf(log_buffer, "cannot destroy cpuset %s", qname);
			log_err(errno, __func__, log_buffer);
		}

		return 1;
	} else if (cset) {	/* on an existing shared cpuset */
		(void)sprintf(log_buffer,
			"attached to shared cpuset %s", qname);
		log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO, jobid,
			log_buffer);
	}

	strcpy(cpuset_name, qname);
	/* Current process now lives within the cpuset. */

	/* Set environment variable for PBS_CPUSET with canonical queue name. */
	(void)bld_env_variables(&vtable, "PBS_CPUSET", current_cpuset());

	/* For compatability, set PBS_NODEMASK from bitfield. */
	(void)bld_env_variables(&vtable, "PBS_NODEMASK", bitfield2hex(nodes_assn));

	return 0;
}

/**
 * @brief
 *	note_nodemask() - Set the resources_used.nodemask field to maskstr on pjob.
 * 
 * @param[in] pjob - job pointer
 * @param[out] maskstr - string to hold nodemask for job
 *
 * @return	int
 * @retval	0	Success
 * 2retval	-1	Error
 *
 */
 
int
note_nodemask(job *pjob, char *maskstr)
{
	attribute		*at;
	resource		*pres;
	resource_def	*rd;

	at = &pjob->ji_wattr[(int)JOB_ATR_resc_used];
	at->at_flags |= ATR_VFLAG_MODIFY;

	rd = find_resc_def(svr_resc_def, "nodemask", svr_resc_size);
	if (rd == NULL) {
#ifdef  DEBUG
		log_err(-1, __func__, "cannot find nodemask resource definition");
#endif
		return -1;
	}
	pres = add_resource_entry(at, rd);
	if (pres == NULL) {
		log_err(-1, __func__, "cannot add resource for nodemask");
		return -1;
	}
	pres->rs_value.at_flags |= ATR_VFLAG_MODIFY | ATR_VFLAG_SET;
	pres->rs_value.at_type   = ATR_TYPE_STR;
	pres->rs_value.at_val.at_str = strdup(maskstr);

	/*
	 * add_resource_entry() sets the ATR_VFLAG_SET flag, but this code is
	 * called before the mom_set_use() call.  We have to manually unset this
	 * flag in order for the test in mom_set_use() to fail, causing it to
	 * initialize all the other variables.
	 *
	 * This is icky, but necessary.
	 */
	at->at_flags &= ~ATR_VFLAG_SET;

	return 0;
}
