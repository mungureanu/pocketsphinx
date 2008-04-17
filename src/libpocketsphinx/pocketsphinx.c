/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 2008 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * This work was supported in part by funding from the Defense Advanced 
 * Research Projects Agency and the National Science Foundation of the 
 * United States of America, and the CMU Sphinx Speech Consortium.
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

/* System headers. */
#include <stdio.h>

/* SphinxBase headers. */
#include <err.h>
#include <strfuncs.h>
#include <filename.h>
#include <pio.h>

/* Local headers. */
#include "pocketsphinx_internal.h"
#include "cmdln_macro.h"
#include "fsg_search_internal.h"
#include "ngram_search.h"
#include "ngram_search_fwdtree.h"
#include "ngram_search_fwdflat.h"
#include "ngram_search_dag.h"

static const arg_t ps_args_def[] = {
    POCKETSPHINX_OPTIONS,
    CMDLN_EMPTY_OPTION
};

/* I'm not sure what the portable way to do this is. */
static int
file_exists(const char *path)
{
    FILE *tmp;

    tmp = fopen(path, "rb");
    if (tmp) fclose(tmp);
    return (tmp != NULL);
}

static void
pocketsphinx_add_file(pocketsphinx_t *ps, const char *arg,
                      const char *hmmdir, const char *file)
{
    char *tmp = string_join(hmmdir, "/", file, NULL);

    if (cmd_ln_str_r(ps->config, arg) == NULL && file_exists(tmp)) {
        cmd_ln_set_str_r(ps->config, arg, tmp);
        ps->strings = glist_add_ptr(ps->strings, tmp);
    }
    else {
        ckd_free(tmp);
    }
}

static void
pocketsphinx_init_defaults(pocketsphinx_t *ps)
{
    char *hmmdir;

    /* Disable memory mapping on Blackfin (FIXME: should be uClinux in general). */
#ifdef __ADSPBLACKFIN__
    E_INFO("Will not use mmap() on uClinux/Blackfin.");
    cmd_ln_set_boolean_r(ps->config, "-mmap", FALSE);
#endif
    /* Get acoustic model filenames and add them to the command-line */
    if ((hmmdir = cmd_ln_str_r(ps->config, "-hmm")) != NULL) {
        pocketsphinx_add_file(ps, "-mdef", hmmdir, "mdef");
        pocketsphinx_add_file(ps, "-mean", hmmdir, "means");
        pocketsphinx_add_file(ps, "-var", hmmdir, "variances");
        pocketsphinx_add_file(ps, "-tmat", hmmdir, "transition_matrices");
        pocketsphinx_add_file(ps, "-mixw", hmmdir, "mixture_weights");
        pocketsphinx_add_file(ps, "-sendump", hmmdir, "sendump");
        pocketsphinx_add_file(ps, "-kdtree", hmmdir, "kdtrees");
        pocketsphinx_add_file(ps, "-fdict", hmmdir, "noisedict");
        pocketsphinx_add_file(ps, "-featparams", hmmdir, "feat.params");
    }
}

int
pocketsphinx_reinit(pocketsphinx_t *ps, cmd_ln_t *config)
{
    char *fsgfile = NULL;
    char *lmfile, *lmctl = NULL;
    gnode_t *gn;

    if (config && config != ps->config) {
        if (ps->config) {
            for (gn = ps->strings; gn; gn = gnode_next(gn))
                ckd_free(gnode_ptr(gn));
            cmd_ln_free_r(ps->config);
        }
        ps->config = config;
    }
    /* Fill in some default arguments. */
    pocketsphinx_init_defaults(ps);

    /* Logmath computation (used in acmod and search) */
    if (ps->lmath == NULL
        || (logmath_get_base(ps->lmath) != 
            (float64)cmd_ln_float32_r(config, "-logbase"))) {
        if (ps->lmath)
            logmath_free(ps->lmath);
        ps->lmath = logmath_init
            ((float64)cmd_ln_float32_r(config, "-logbase"), 0, FALSE);
    }

    /* Acoustic model (this is basically everything that
     * uttproc.c, senscr.c, and others used to do) */
    if (ps->acmod)
        acmod_free(ps->acmod);
    if ((ps->acmod = acmod_init(config, ps->lmath, NULL, NULL)) == NULL)
        return -1;

    /* Make the acmod's feature buffer growable if we are doing two-pass search. */
    if (cmd_ln_boolean_r(config, "-fwdflat")
        && cmd_ln_boolean_r(config, "-fwdtree"))
        acmod_set_grow(ps->acmod, TRUE);

    /* Dictionary and triphone mappings (depends on acmod). */
    if (ps->dict)
        dict_free(ps->dict);
    if ((ps->dict = dict_init(config, ps->acmod->mdef)) == NULL)
        return -1;

    /* Determine whether we are starting out in FSG or N-Gram search mode. */
    if (ps->searches) {
        for (gn = ps->searches; gn; gn = gnode_next(gn))
            ps_search_free(gnode_ptr(gn));
        glist_free(ps->searches);
        ps->searches = NULL;
        ps->search = NULL;
    }
    if ((fsgfile = cmd_ln_str_r(config, "-fsg"))) {
        ps_search_t *fsgs;

        fsgs = fsg_search_init(config, ps->acmod, ps->dict);
        ps->searches = glist_add_ptr(ps->searches, fsgs);
        ps->search = fsgs;
    }
    else if ((lmfile = cmd_ln_str_r(config, "-lm"))
             || (lmctl = cmd_ln_str_r(config, "-lmctlfn"))) {
        ps_search_t *ngs;

        ngs = ngram_search_init(config, ps->acmod, ps->dict);
        ps->searches = glist_add_ptr(ps->searches, ngs);
        ps->search = ngs;
    }
    /* Otherwise, we will initialize the search whenever the user
     * decides to load an FSG or a language model. */

    /* Initialize performance timer. */
    ps->perf.name = "decode";
    ptmr_init(&ps->perf);

    return 0;
}

pocketsphinx_t *
pocketsphinx_init(cmd_ln_t *config)
{
    pocketsphinx_t *ps;

    ps = ckd_calloc(1, sizeof(*ps));
    if (pocketsphinx_reinit(ps, config) < 0) {
        pocketsphinx_free(ps);
        return NULL;
    }
    return ps;
}

arg_t const *
pocketsphinx_args(void)
{
    return ps_args_def;
}

void
pocketsphinx_free(pocketsphinx_t *ps)
{
    gnode_t *gn;

    for (gn = ps->searches; gn; gn = gnode_next(gn))
        ps_search_free(gnode_ptr(gn));
    glist_free(ps->searches);
    dict_free(ps->dict);
    acmod_free(ps->acmod);
    logmath_free(ps->lmath);
    cmd_ln_free_r(ps->config);
    for (gn = ps->strings; gn; gn = gnode_next(gn))
        ckd_free(gnode_ptr(gn));
    glist_free(ps->strings);
    ckd_free(ps->uttid);
    ckd_free(ps);
}

cmd_ln_t *
pocketsphinx_get_config(pocketsphinx_t *ps)
{
    return ps->config;
}

logmath_t *
pocketsphinx_get_logmath(pocketsphinx_t *ps)
{
    return ps->lmath;
}

ngram_model_t *
pocketsphinx_get_lmset(pocketsphinx_t *ps)
{
    if (ps->search == NULL
        || 0 != strcmp(ps_search_name(ps->search), "ngram"))
        return NULL;
    return ((ngram_search_t *)ps->search)->lmset;
}

ngram_model_t *
pocketsphinx_update_lmset(pocketsphinx_t *ps)
{
    ngram_search_t *ngs;
    gnode_t *gn;

    /* Look for N-Gram search. */
    for (gn = ps->searches; gn; gn = gnode_next(gn)) {
        if (0 == strcmp(ps_search_name(gnode_ptr(gn)), "ngram"))
            break;
    }
    if (gn == NULL) {
        /* Initialize N-Gram search. */
        ngs = (ngram_search_t *)ngram_search_init(ps->config,
                                                  ps->acmod, ps->dict);
        if (ngs == NULL)
            return NULL;
        ps->searches = glist_add_ptr(ps->searches, ngs);
    }
    else {
        /* Tell N-Gram search to update its view of the world. */
        ngs = gnode_ptr(gn);
        if (ps_search_reinit(ps_search_base(ngs)) < 0)
            return NULL;
    }
    ps->search = ps_search_base(ngs);
    return ngs->lmset;
}

fsg_set_t *
pocketsphinx_get_fsgset(pocketsphinx_t *ps)
{
    if (ps->search == NULL
        || 0 != strcmp(ps_search_name(ps->search), "fsg"))
        return NULL;
    return (fsg_set_t *)ps->search;
}

fsg_set_t *
pocketsphinx_update_fsgset(pocketsphinx_t *ps)
{
    gnode_t *gn;
    fsg_search_t *fsgs;

    /* Look for FSG search. */
    for (gn = ps->searches; gn; gn = gnode_next(gn)) {
        if (0 == strcmp(ps_search_name(gnode_ptr(gn)), "fsg"))
            break;
    }
    if (gn == NULL) {
        /* Initialize FSG search. */
        fsgs = (fsg_search_t *)fsg_search_init(ps->config,
                                               ps->acmod, ps->dict);
        ps->searches = glist_add_ptr(ps->searches, fsgs);
    }
    else {
        /* Tell FSG search to update its view of the world. */
        fsgs = gnode_ptr(gn);
        if (ps_search_reinit(ps_search_base(fsgs)) < 0)
            return NULL;
    }
    ps->search = ps_search_base(fsgs);
    return (fsg_set_t *)fsgs;
}

int
pocketsphinx_add_word(pocketsphinx_t *ps,
                      char const *word,
                      char const *phones,
                      int update)
{
    int32 wid, lmwid;
    ngram_model_t *lmset;
    char *pron;
    int rv;

    pron = ckd_salloc(phones);
    if ((wid = dict_add_word(ps->dict, word, pron)) == -1) {
        ckd_free(pron);
        return -1;
    }
    ckd_free(pron);

    if ((lmset = pocketsphinx_get_lmset(ps)) != NULL) {
        /* FIXME: There is a way more efficient way to do this, since all
         * we did was replace a placeholder string with the new word
         * string - therefore what we ought to do is add it directly to
         * the current LM, then update the mapping without reallocating
         * everything. */
        /* Add it to the LM set (meaning, the current LM).  In a perfect
         * world, this would result in the same WID, but because of the
         * weird way that word IDs are handled, it doesn't. */
        if ((lmwid = ngram_model_add_word(lmset, word, 1.0))
            == NGRAM_INVALID_WID)
            return -1;
    }
 
    /* Rebuild the widmap and search tree if requested. */
    if (update) {
        if ((rv = ps_search_reinit(ps->search) < 0))
            return rv;
    }
    return wid;
}

int
pocketsphinx_decode_raw(pocketsphinx_t *ps, FILE *rawfh,
                        char const *uttid, long maxsamps)
{
    long total, pos;

    pocketsphinx_start_utt(ps, uttid);
    /* If this file is seekable or maxsamps is specified, then decode
     * the whole thing at once. */
    if (maxsamps != -1 || (pos = ftell(rawfh)) >= 0) {
        int16 *data;

        if (maxsamps == -1) {
            long endpos;
            fseek(rawfh, 0, SEEK_END);
            endpos = ftell(rawfh);
            fseek(rawfh, pos, SEEK_SET);
            maxsamps = endpos - pos;
        }
        data = ckd_calloc(maxsamps, sizeof(*data));
        total = fread(data, sizeof(*data), maxsamps, rawfh);
        pocketsphinx_process_raw(ps, data, total, FALSE, TRUE);
        ckd_free(data);
    }
    else {
        /* Otherwise decode it in a stream. */
        total = 0;
        while (!feof(rawfh)) {
            int16 data[256];
            size_t nread;

            nread = fread(data, sizeof(*data), sizeof(data)/sizeof(*data), rawfh);
            pocketsphinx_process_raw(ps, data, nread, FALSE, FALSE);
            total += nread;
        }
    }
    pocketsphinx_end_utt(ps);
    return total;
}

int
pocketsphinx_start_utt(pocketsphinx_t *ps, char const *uttid)
{
    int rv;

    ptmr_reset(&ps->perf);
    ptmr_start(&ps->perf);

    if (uttid) {
        ckd_free(ps->uttid);
        ps->uttid = ckd_salloc(uttid);
    }
    else {
        char nuttid[16];
        ckd_free(ps->uttid);
        sprintf(nuttid, "%09u", ps->uttno);
        ps->uttid = ckd_salloc(nuttid);
        ++ps->uttno;
    }

    if ((rv = acmod_start_utt(ps->acmod)) < 0)
        return rv;

    return ps_search_start(ps->search);
}

int
pocketsphinx_process_raw(pocketsphinx_t *ps,
			 int16 const *data,
			 size_t n_samples,
			 int no_search,
			 int full_utt)
{
    int n_searchfr = 0;

    if (no_search)
        acmod_set_grow(ps->acmod, TRUE);

    while (n_samples) {
        int nfr;

        /* Process some data into features. */
        if ((nfr = acmod_process_raw(ps->acmod, &data,
                                    &n_samples, full_utt)) < 0)
            return nfr;

        /* Score and search as much data as possible */
        if (!no_search) {
            while ((nfr = ps_search_step(ps->search)) > 0) {
                n_searchfr += nfr;
            }
            if (nfr < 0)
                return nfr;
        }
    }

    ps->n_frame += n_searchfr;
    return n_searchfr;
}

int
pocketsphinx_process_cep(pocketsphinx_t *ps,
			 mfcc_t **data,
			 int32 n_frames,
			 int no_search,
			 int full_utt)
{
    int n_searchfr = 0;

    if (no_search)
        acmod_set_grow(ps->acmod, TRUE);

    while (n_frames) {
        int nfr;

        /* Process some data into features. */
        if ((nfr = acmod_process_cep(ps->acmod, &data,
                                     &n_frames, full_utt)) < 0)
            return nfr;

        /* Score and search as much data as possible */
        if (!no_search) {
            while ((nfr = ps_search_step(ps->search)) > 0) {
                n_searchfr += nfr;
            }
            if (nfr < 0)
                return nfr;
        }
    }

    ps->n_frame += n_searchfr;
    return n_searchfr;
}

int
pocketsphinx_end_utt(pocketsphinx_t *ps)
{
    int rv;

    acmod_end_utt(ps->acmod);
    while ((rv = ps_search_step(ps->search)) > 0) {
    }
    if (rv < 0) {
        ptmr_stop(&ps->perf);
        return rv;
    }
    rv = ps_search_finish(ps->search);
    ptmr_stop(&ps->perf);
    return rv;
}

char const *
pocketsphinx_get_hyp(pocketsphinx_t *ps, int32 *out_best_score, char const **uttid)
{
    char const *hyp;

    ptmr_start(&ps->perf);
    hyp = ps_search_hyp(ps->search, out_best_score);
    if (uttid)
        *uttid = ps->uttid;
    ptmr_stop(&ps->perf);
    return hyp;
}

ps_seg_t *
pocketsphinx_seg_iter(pocketsphinx_t *ps, int32 *out_best_score)
{
    ps_seg_t *itor;

    ptmr_start(&ps->perf);
    itor = ps_search_seg_iter(ps->search, out_best_score);
    ptmr_stop(&ps->perf);
    return itor;
}

ps_seg_t *
pocketsphinx_seg_next(ps_seg_t *seg)
{
    return ps_search_seg_next(seg);
}

char const *
pocketsphinx_seg_word(ps_seg_t *seg)
{
    return seg->word;
}

void
pocketsphinx_seg_frames(ps_seg_t *seg, int *out_sf, int *out_ef)
{
    *out_sf = seg->sf;
    *out_ef = seg->ef;
}

void
pocketsphinx_seg_prob(ps_seg_t *seg, int32 *out_pprob)
{
    *out_pprob = seg->prob;
}

void
pocketsphinx_seg_free(ps_seg_t *seg)
{
    ps_search_seg_free(seg);
}

void
pocketsphinx_get_utt_time(pocketsphinx_t *ps, double *out_nspeech,
                          double *out_ncpu, double *out_nwall)
{
    int32 frate;

    frate = cmd_ln_int32_r(ps->config, "-frate");
    *out_nspeech = (double)ps->acmod->output_frame / frate;
    *out_ncpu = ps->perf.t_cpu;
    *out_nwall = ps->perf.t_elapsed;
}

void
pocketsphinx_get_all_time(pocketsphinx_t *ps, double *out_nspeech,
                          double *out_ncpu, double *out_nwall)
{
    int32 frate;

    frate = cmd_ln_int32_r(ps->config, "-frate");
    *out_nspeech = (double)ps->n_frame / frate;
    *out_ncpu = ps->perf.t_tot_cpu;
    *out_nwall = ps->perf.t_tot_elapsed;
}

void
ps_search_init(ps_search_t *search, ps_searchfuncs_t *vt,
               cmd_ln_t *config, acmod_t *acmod, dict_t *dict)
{
    search->vt = vt;
    search->config = config;
    search->acmod = acmod;
    search->dict = dict;
}

void
ps_search_deinit(ps_search_t *search)
{
    /* FIXME: We will have refcounting on acmod, config, etc, at which
     * point we will free them here too. */
    ckd_free(search->hyp_str);
}
