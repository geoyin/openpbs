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


/*
 * tracking.h - header file for maintaining job tracking records
 *
 * These are linked into the server structure.  Entries are added or
 * updated upon the receipt of Track Job Requests and are used to
 * satisfy Locate Job requests.
 *
 * The main data is kept in the form of the track batch request so
 * that copying is easy.
 *
 * Other required header files:
 *	"server_limits.h"
 */

#define PBS_TRACK_MINSIZE   100	/* mininum size of buffer in records */
#define PBS_SAVE_TRACK_TM   300	/* time interval between saves of track data */

struct tracking {
	time_t	 tk_mtime;	/* time this entry modified */
	int      tk_hopcount;
	char     tk_jobid[PBS_MAXSVRJOBID+1];
	char     tk_location[PBS_MAXDEST+1];
	char     tk_state;
};

extern void track_save(struct work_task *);
