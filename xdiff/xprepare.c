/*
 *  LibXDiff by Davide Libenzi ( File Differential Library )
 *  Copyright (C) 2003  Davide Libenzi
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#include "xinclude.h"



#define XDL_KPDIS_RUN 4



typedef struct s_xdlclass {
	struct s_xdlclass *next;
	unsigned long ha;
	char const *line;
	long size;
	long idx;
} xdlclass_t;

typedef struct s_xdlclassifier {
	unsigned int hbits;
	long hsize;
	xdlclass_t **rchash;
	chastore_t ncha;
	long count;
} xdlclassifier_t;




static int xdl_init_classifier(xdlclassifier_t *cf, long size);
static void xdl_free_classifier(xdlclassifier_t *cf);
static int xdl_classify_record(xdlclassifier_t *cf, xrecord_t **rhash, unsigned int hbits,
			       xrecord_t *rec);
static int xdl_prepare_ctx(mmfile_t *mf, long narec, xpparam_t const *xpp,
			   xdlclassifier_t *cf, xdfile_t *xdf);
static void xdl_free_ctx(xdfile_t *xdf);
static int xdl_clean_mmatch(char const *dis, long i, long s, long e);
static int xdl_cleanup_records(xdfile_t *xdf1, xdfile_t *xdf2);
static int xdl_trim_ends(xdfile_t *xdf1, xdfile_t *xdf2);
static int xdl_optimize_ctxs(xdfile_t *xdf1, xdfile_t *xdf2);




static int xdl_init_classifier(xdlclassifier_t *cf, long size) {
	long i;

	cf->hbits = xdl_hashbits((unsigned int) size);
	cf->hsize = 1 << cf->hbits;

	if (xdl_cha_init(&cf->ncha, sizeof(xdlclass_t), size / 4 + 1) < 0) {

		return -1;
	}
	if (!(cf->rchash = (xdlclass_t **) malloc(cf->hsize * sizeof(xdlclass_t *)))) {

		xdl_cha_free(&cf->ncha);
		return -1;
	}
	for (i = 0; i < cf->hsize; i++)
		cf->rchash[i] = NULL;

	cf->count = 0;

	return 0;
}


static void xdl_free_classifier(xdlclassifier_t *cf) {

	free(cf->rchash);
	xdl_cha_free(&cf->ncha);
}


static int xdl_classify_record(xdlclassifier_t *cf, xrecord_t **rhash, unsigned int hbits,
			       xrecord_t *rec) {
	long hi;
	char const *line;
	xdlclass_t *rcrec;

	line = rec->ptr;
	hi = (long) XDL_HASHLONG(rec->ha, cf->hbits);
	for (rcrec = cf->rchash[hi]; rcrec; rcrec = rcrec->next)
		if (rcrec->ha == rec->ha && rcrec->size == rec->size &&
		    !memcmp(line, rcrec->line, rec->size))
			break;

	if (!rcrec) {
		if (!(rcrec = xdl_cha_alloc(&cf->ncha))) {

			return -1;
		}
		rcrec->idx = cf->count++;
		rcrec->line = line;
		rcrec->size = rec->size;
		rcrec->ha = rec->ha;
		rcrec->next = cf->rchash[hi];
		cf->rchash[hi] = rcrec;
	}

	rec->ha = (unsigned long) rcrec->idx;

	hi = (long) XDL_HASHLONG(rec->ha, hbits);
	rec->next = rhash[hi];
	rhash[hi] = rec;

	return 0;
}


static int xdl_prepare_ctx(mmfile_t *mf, long narec, xpparam_t const *xpp,
			   xdlclassifier_t *cf, xdfile_t *xdf) {
	unsigned int hbits;
	long i, nrec, hsize, bsize;
	unsigned long hav;
	char const *blk, *cur, *top, *prev;
	xrecord_t *crec;
	xrecord_t **recs, **rrecs;
	xrecord_t **rhash;
	unsigned long *ha;
	char *rchg;
	long *rindex;

	if (xdl_cha_init(&xdf->rcha, sizeof(xrecord_t), narec / 4 + 1) < 0) {

		return -1;
	}
	if (!(recs = (xrecord_t **) malloc(narec * sizeof(xrecord_t *)))) {

		xdl_cha_free(&xdf->rcha);
		return -1;
	}

	hbits = xdl_hashbits((unsigned int) narec);
	hsize = 1 << hbits;
	if (!(rhash = (xrecord_t **) malloc(hsize * sizeof(xrecord_t *)))) {

		free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}
	for (i = 0; i < hsize; i++)
		rhash[i] = NULL;

	nrec = 0;
	if ((cur = blk = xdl_mmfile_first(mf, &bsize)) != NULL) {
		for (top = blk + bsize;;) {
			if (cur >= top) {
				if (!(cur = blk = xdl_mmfile_next(mf, &bsize)))
					break;
				top = blk + bsize;
			}
			prev = cur;
			hav = xdl_hash_record(&cur, top);
			if (nrec >= narec) {
				narec *= 2;
				if (!(rrecs = (xrecord_t **) realloc(recs, narec * sizeof(xrecord_t *)))) {

					free(rhash);
					free(recs);
					xdl_cha_free(&xdf->rcha);
					return -1;
				}
				recs = rrecs;
			}
			if (!(crec = xdl_cha_alloc(&xdf->rcha))) {

				free(rhash);
				free(recs);
				xdl_cha_free(&xdf->rcha);
				return -1;
			}
			crec->ptr = prev;
			crec->size = (long) (cur - prev);
			crec->ha = hav;
			recs[nrec++] = crec;

			if (xdl_classify_record(cf, rhash, hbits, crec) < 0) {

				free(rhash);
				free(recs);
				xdl_cha_free(&xdf->rcha);
				return -1;
			}
		}
	}

	if (!(rchg = (char *) malloc((nrec + 2) * sizeof(char)))) {

		free(rhash);
		free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}
	memset(rchg, 0, (nrec + 2) * sizeof(char));

	if (!(rindex = (long *) malloc((nrec + 1) * sizeof(long)))) {

		free(rchg);
		free(rhash);
		free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}
	if (!(ha = (unsigned long *) malloc((nrec + 1) * sizeof(unsigned long)))) {

		free(rindex);
		free(rchg);
		free(rhash);
		free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}

	xdf->nrec = nrec;
	xdf->recs = recs;
	xdf->hbits = hbits;
	xdf->rhash = rhash;
	xdf->rchg = rchg + 1;
	xdf->rindex = rindex;
	xdf->nreff = 0;
	xdf->ha = ha;
	xdf->dstart = 0;
	xdf->dend = nrec - 1;

	return 0;
}


static void xdl_free_ctx(xdfile_t *xdf) {

	free(xdf->rhash);
	free(xdf->rindex);
	free(xdf->rchg - 1);
	free(xdf->ha);
	free(xdf->recs);
	xdl_cha_free(&xdf->rcha);
}


int xdl_prepare_env(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
		    xdfenv_t *xe) {
	long enl1, enl2;
	xdlclassifier_t cf;

	enl1 = xdl_guess_lines(mf1) + 1;
	enl2 = xdl_guess_lines(mf2) + 1;

	if (xdl_init_classifier(&cf, enl1 + enl2 + 1) < 0) {

		return -1;
	}

	if (xdl_prepare_ctx(mf1, enl1, xpp, &cf, &xe->xdf1) < 0) {

		xdl_free_classifier(&cf);
		return -1;
	}
	if (xdl_prepare_ctx(mf2, enl2, xpp, &cf, &xe->xdf2) < 0) {

		xdl_free_ctx(&xe->xdf1);
		xdl_free_classifier(&cf);
		return -1;
	}

	xdl_free_classifier(&cf);

	if (xdl_optimize_ctxs(&xe->xdf1, &xe->xdf2) < 0) {

		xdl_free_ctx(&xe->xdf2);
		xdl_free_ctx(&xe->xdf1);
		return -1;
	}

	return 0;
}


void xdl_free_env(xdfenv_t *xe) {

	xdl_free_ctx(&xe->xdf2);
	xdl_free_ctx(&xe->xdf1);
}


static int xdl_clean_mmatch(char const *dis, long i, long s, long e) {
	long r, rdis, rpdis;

	for (r = 1, rdis = 0, rpdis = 1; (i - r) >= s; r++)
		if (!dis[i - r])
			rdis++;
		else if (dis[i - r] == 2)
			rpdis++;
		else
			break;
	for (r = 1; (i + r) <= e; r++)
		if (!dis[i + r])
			rdis++;
		else if (dis[i + r] == 2)
			rpdis++;
		else
			break;

	return rpdis * XDL_KPDIS_RUN < (rpdis + rdis);
}


/*
 * Try to reduce the problem complexity, discard records that have no
 * matches on the other file. Also, lines that have multiple matches
 * might be potentially discarded if they happear in a run of discardable.
 */
static int xdl_cleanup_records(xdfile_t *xdf1, xdfile_t *xdf2) {
	long i, rhi, nreff;
	unsigned long hav;
	xrecord_t **recs;
	xrecord_t *rec;
	char *dis, *dis1, *dis2;

	if (!(dis = (char *) malloc((xdf1->nrec + xdf2->nrec + 2) * sizeof(char)))) {

		return -1;
	}
	memset(dis, 0, (xdf1->nrec + xdf2->nrec + 2) * sizeof(char));
	dis1 = dis;
	dis2 = dis1 + xdf1->nrec + 1;

	for (i = xdf1->dstart, recs = &xdf1->recs[xdf1->dstart]; i <= xdf1->dend; i++, recs++) {
		hav = (*recs)->ha;
		rhi = (long) XDL_HASHLONG(hav, xdf2->hbits);
		for (rec = xdf2->rhash[rhi]; rec; rec = rec->next)
			if (rec->ha == hav && ++dis1[i] == 2)
				break;
	}

	for (i = xdf2->dstart, recs = &xdf2->recs[xdf2->dstart]; i <= xdf2->dend; i++, recs++) {
		hav = (*recs)->ha;
		rhi = (long) XDL_HASHLONG(hav, xdf1->hbits);
		for (rec = xdf1->rhash[rhi]; rec; rec = rec->next)
			if (rec->ha == hav && ++dis2[i] == 2)
				break;
	}

	for (nreff = 0, i = xdf1->dstart, recs = &xdf1->recs[xdf1->dstart];
	     i <= xdf1->dend; i++, recs++) {
		if (dis1[i] == 1 ||
		    (dis1[i] == 2 && !xdl_clean_mmatch(dis1, i, xdf1->dstart, xdf1->dend))) {
			xdf1->rindex[nreff] = i;
			xdf1->ha[nreff] = (*recs)->ha;
			nreff++;
		} else
			xdf1->rchg[i] = 1;
	}
	xdf1->nreff = nreff;

	for (nreff = 0, i = xdf2->dstart, recs = &xdf2->recs[xdf2->dstart];
	     i <= xdf2->dend; i++, recs++) {
		if (dis2[i] == 1 ||
		    (dis2[i] == 2 && !xdl_clean_mmatch(dis2, i, xdf2->dstart, xdf2->dend))) {
			xdf2->rindex[nreff] = i;
			xdf2->ha[nreff] = (*recs)->ha;
			nreff++;
		} else
			xdf2->rchg[i] = 1;
	}
	xdf2->nreff = nreff;

	free(dis);

	return 0;
}


/*
 * Early trim initial and terminal matching records.
 */
static int xdl_trim_ends(xdfile_t *xdf1, xdfile_t *xdf2) {
	long i, lim;
	xrecord_t **recs1, **recs2;

	recs1 = xdf1->recs;
	recs2 = xdf2->recs;
	for (i = 0, lim = XDL_MIN(xdf1->nrec, xdf2->nrec); i < lim;
	     i++, recs1++, recs2++)
		if ((*recs1)->ha != (*recs2)->ha)
			break;

	xdf1->dstart = xdf2->dstart = i;

	recs1 = xdf1->recs + xdf1->nrec - 1;
	recs2 = xdf2->recs + xdf2->nrec - 1;
	for (lim -= i, i = 0; i < lim; i++, recs1--, recs2--)
		if ((*recs1)->ha != (*recs2)->ha)
			break;

	xdf1->dend = xdf1->nrec - i - 1;
	xdf2->dend = xdf2->nrec - i - 1;

	return 0;
}


static int xdl_optimize_ctxs(xdfile_t *xdf1, xdfile_t *xdf2) {

	if (xdl_trim_ends(xdf1, xdf2) < 0 ||
	    xdl_cleanup_records(xdf1, xdf2) < 0) {

		return -1;
	}

	return 0;
}

