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
 * @file	qdel.c
 * @brief
 * 	qdel - (PBS) delete batch job
 *
 * @author  Terry Heidelberg
 * 			Livermore Computing
 *
 * @author  Bruce Kelly
 * 			National Energy Research Supercomputer Center
 *
 * @author  Lawrence Livermore National Laboratory
 * 			University of California
 */

#include "cmds.h"
#include "pbs_ifl.h"
#include <pbs_config.h>   /* the master config generated by configure */
#include <pbs_version.h>


int
main(argc, argv, envp) /* qdel */
int argc;
char **argv;
char **envp;
{
	int c;
	int errflg=0;
	int any_failed=0;
	char *pc;

	int forcedel = FALSE;
	int deletehist = FALSE;

	char job_id[PBS_MAXCLTJOBID];	/* from the command line */

	char job_id_out[PBS_MAXCLTJOBID];
	char server_out[MAXSERVERNAME];
	char rmt_server[MAXSERVERNAME];

	char *keystr, *valuestr;
	int dfltmail = 0;
	int dfltmailflg = FALSE;
	int mails;				/* number of emails we can send */
	int num_deleted = 0;
	struct attrl *attr;
	struct batch_status *ss = NULL;

#define MAX_TIME_DELAY_LEN 32
	char warg[MAX_TIME_DELAY_LEN+1];
	char warg1[MAX_TIME_DELAY_LEN+7];

#define GETOPT_ARGS "W:x"

	/*test for real deal or just version and exit*/

	execution_mode(argc, argv);

#ifdef WIN32
	winsock_init();
#endif

	warg[0]='\0';
	strcpy(warg1, NOMAIL);
	while ((c = getopt(argc, argv, GETOPT_ARGS)) != EOF) {
		switch (c) {
			case 'W':
				pc = optarg;
				if (strlen(pc) == 0) {
					fprintf(stderr, "qdel: illegal -W value\n");
					errflg++;
					break;
				}
				if (strcmp(pc, FORCEDEL) == 0) {
					forcedel = TRUE;
					break;
				}
				if (parse_equal_string(optarg, &keystr, &valuestr)) {
					if (strcmp(keystr, SUPPRESS_EMAIL) == 0) {
						dfltmail = atol(valuestr);
						dfltmailflg = TRUE;
						break;
					}
				}

				while (*pc != '\0') {
					if (! isdigit(*pc)) {
						fprintf(stderr, "qdel: illegal -W value\n");
						errflg++;
						break;
					}
					pc++;
				}
				break;
			case  'x' :
				deletehist = TRUE;
				break;
			default :
				errflg++;
		}
	}

	if (errflg || optind >= argc) {
		static char usage[] =
			"usage:\n"
		"\tqdel [-W force|suppress_email=X] [-x] job_identifier...\n"
		"\tqdel --version\n";
		fprintf(stderr, "%s", usage);
		exit(2);
	}

	if (forcedel && deletehist)
		snprintf(warg, sizeof(warg), "%s%s", FORCEDEL, DELETEHISTORY);
	else if (forcedel)
		strcpy(warg, FORCEDEL);
	else if (deletehist)
		strcpy(warg, DELETEHISTORY);

	/*perform needed security library initializations (including none)*/

	if (CS_client_init() != CS_SUCCESS) {
		fprintf(stderr, "qdel: unable to initialize security library.\n");
		exit(1);
	}

	for (; optind < argc; optind++) {
		int connect;
		int stat=0;
		int located = FALSE;

		strcpy(job_id, argv[optind]);
		if (get_server(job_id, job_id_out, server_out)) {
			fprintf(stderr, "qdel: illegally formed job identifier: %s\n", job_id);
			any_failed = 1;
			continue;
		}
cnt:
		connect = cnt2server(server_out);
		if (connect <= 0) {
			fprintf(stderr, "qdel: cannot connect to server %s (errno=%d)\n",
				pbs_server, pbs_errno);
			any_failed = pbs_errno;
			continue;
		}

		/* retrieve default: suppress_email from server: default_qdel_arguments */
		if (dfltmailflg == FALSE) {
			ss = pbs_statserver(connect, NULL, NULL);
			while (ss != NULL && dfltmailflg != TRUE) {
				attr = ss->attribs;
				while (attr != NULL) {
					if (strcmp(attr->name, ATTR_dfltqdelargs) == 0) {
						if (attr->value != NULL && dfltmailflg != TRUE) {
							if (parse_equal_string(attr->value, &keystr, &valuestr)) {
								if (strcmp(keystr, "-Wsuppress_email") == 0) {
									dfltmail = atol(valuestr);
									dfltmailflg = TRUE;
								}
								else {
									fprintf(stderr,
										"qdel: unsupported %s \'%s\'\n",
										attr->name, attr->value);
								}
							}
						}
					}
					attr = attr->next;
				}
				ss = ss->next;
			}
		}

		/* when jobs to be deleted over 1000, mail function is disabled
		 * by sending the flag below to server via its extend field:
		 *   "" -- delete a job with a mail
		 *   "nomail" -- delete a job without sending a mail
		 *   "force" -- force job to be deleted with a mail
		 *   "nomailforce" -- force job to be deleted without sending a mail
		 *   "nomaildeletehist" -- delete history of a job without sending mail
		 *   "nomailforcedeletehist" -- force delete history of a job without sending mail.
		 */
		mails = dfltmail ? dfltmail : 1000;
		if (num_deleted == mails) {

			strcat(warg1, warg);
			strcpy(warg, warg1);


		}


		stat = pbs_deljob(connect, job_id_out, warg);

		/*
		 * The counter num_deleted should not be updated  when a history job is deleted .
		 */
		if (pbs_errno != PBSE_HISTJOBDELETED)
			num_deleted++;
		if (stat && (pbs_errno != PBSE_UNKJOBID && pbs_errno != PBSE_HISTJOBDELETED)) {
			prt_job_err("qdel", connect, job_id_out);
			any_failed = pbs_errno;
		} else if (stat && (pbs_errno == PBSE_UNKJOBID) && !located) {
			located = TRUE;
			if (locate_job(job_id_out, server_out, rmt_server)) {
				pbs_disconnect(connect);
				strcpy(server_out, rmt_server);
				goto cnt;
			}
			prt_job_err("qdel", connect, job_id_out);
			any_failed = pbs_errno;
		}

		pbs_disconnect(connect);
	}

	/*cleanup security library initializations before exiting*/
	CS_close_app();

	exit(any_failed);
}
