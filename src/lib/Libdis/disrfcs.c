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
 * @file	disrfcs.c
 *
 * @par Synopsis:
 *	int disrfcs(int stream, size_t *nchars, size_t achars, char *value)
 *
 *	Gets a Data-is-Strings character string from <stream> and converts it
 *	into a counted string, puts the string into <value>, a pre-allocated
 *	string, <achars> long, and puts the count into *<nchars>.  The character
 *	string in <stream> consists of an unsigned integer, followed by a number
 *	of characters determined by the unsigned integer.
 *
 *	Disrfcs returns DIS_SUCCESS if everything works well.  It returns an
 *	error code otherwise.  In case of an error, the <stream> character
 *	pointer is reset, making it possible to retry with some other conversion
 *	strategy, and <nchars> is set to 0.
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

#include "dis.h"
#include "dis_.h"

/**
 * @brief
 *	-Gets a Data-is-Strings character string from <stream> and converts it
 *      into a counted string, puts the string into <value>, a pre-allocated
 *      string, <achars> long, and puts the count into *<nchars>.  The character
 *      string in <stream> consists of an unsigned integer, followed by a number
 *      of characters determined by the unsigned integer.
 *
 * @param[in] stream - socket descriptor
 * @param[out] nchars - chars count
 * @param[out] achars - long value
 * @param[out] value - string value
 *
 * @return	int
 * @retval	DIS_success/error status
 *
 */
int
disrfcs(int stream, size_t *nchars, size_t achars, char *value)
{
	int		locret;
	int		negate;
	unsigned	count = 0;

	assert(nchars != NULL);
	assert(value != NULL);
	assert(dis_gets != NULL);
	assert(disr_commit != NULL);

	locret = disrsi_(stream, &negate, &count, 1, 0);
	if (locret == DIS_SUCCESS) {
		if (negate)
			locret = DIS_BADSIGN;
		else if ((*nchars = count) > achars)
			locret = DIS_OVERFLOW;
		else if ((*dis_gets)(stream, value, *nchars) != *nchars)
			locret = DIS_PROTO;
	}
	locret = (*disr_commit)(stream, locret == DIS_SUCCESS) ?
		DIS_NOCOMMIT : locret;
	if (locret != DIS_SUCCESS)
		*nchars = 0;
	return (locret);
}
