#include <stdio.h>
#include <string.h>
#include <pocketsphinx.h>
#include <logmath.h>

#include "acmod.h"
#include "test_macros.h"

static const mfcc_t prior[13] = {
	FLOAT2MFCC(37.03),
	FLOAT2MFCC(-1.01),
	FLOAT2MFCC(0.53),
	FLOAT2MFCC(0.49),
	FLOAT2MFCC(-0.60),
	FLOAT2MFCC(0.14),
	FLOAT2MFCC(-0.05),
	FLOAT2MFCC(0.25),
	FLOAT2MFCC(0.37),
	FLOAT2MFCC(0.58),
	FLOAT2MFCC(0.13),
	FLOAT2MFCC(-0.16),
	FLOAT2MFCC(0.17)
};

int
main(int argc, char *argv[])
{
	acmod_t *acmod;
	logmath_t *lmath;
	cmd_ln_t *config;
	FILE *rawfh;
	int16 *buf;
	int16 const *bptr;
	size_t nread, nsamps;
	int nfr;
	int frame_counter;
	int bestsen1[128];

	lmath = logmath_init(1.0001, 0, 0);
	config = cmd_ln_init(NULL, pocketsphinx_args(), TRUE,
			     "-featparams", MODELDIR "/hmm/wsj1/feat.params",
			     "-mdef", MODELDIR "/hmm/wsj1/mdef",
			     "-mean", MODELDIR "/hmm/wsj1/means",
			     "-var", MODELDIR "/hmm/wsj1/variances",
			     "-tmat", MODELDIR "/hmm/wsj1/transition_matrices",
			     "-sendump", MODELDIR "/hmm/wsj1/sendump",
			     "-compallsen", "true",
			     "-tmatfloor", "0.0001",
			     "-mixwfloor", "0.001",
			     "-varfloor", "0.0001",
			     "-mmap", "no",
			     "-topn", "4",
			     "-dsratio", "1",
			     "-samprate", "16000", NULL);
	TEST_ASSERT(config);
	TEST_ASSERT(acmod = acmod_init(config, lmath, NULL, NULL));
	cmn_prior_set(acmod->fcb->cmn_struct, prior);

	nsamps = 2048;
	frame_counter = 0;
	buf = ckd_calloc(nsamps, sizeof(*buf));
	TEST_ASSERT(rawfh = fopen(DATADIR "/goforward.raw", "rb"));
	TEST_EQUAL(0, acmod_start_utt(acmod));
	while (!feof(rawfh)) {
		nread = fread(buf, sizeof(*buf), nsamps, rawfh);
		/* printf("Read %d samples\n", nread); */
		bptr = buf;
		while ((nfr = acmod_process_raw(acmod, &bptr, &nread, FALSE)) > 0) {
			ascr_t const *senscr;
			int frame_idx, best_score, best_senid;
			/* printf("Processed %d frames, %d samples remaining\n", nfr, nread); */
			while ((senscr = acmod_score(acmod, &frame_idx,
						     &best_score, &best_senid))) {
				/* printf("Frame %d best senone %d score %d\n",
				   frame_idx, best_senid, best_score); */
				TEST_EQUAL(frame_counter, frame_idx);
				if (frame_counter < 128)
					bestsen1[frame_counter] = best_senid;
				++frame_counter;
			}
		}
	}
	TEST_EQUAL(0, acmod_end_utt(acmod));
	nread = 0;
	while ((nfr = acmod_process_raw(acmod, NULL, &nread, FALSE)) > 0) {
		ascr_t const *senscr;
		int frame_idx, best_score, best_senid;
		/* printf("Processed %d frames, %d samples remaining\n", nfr, nread); */
		while ((senscr = acmod_score(acmod, &frame_idx,
					     &best_score, &best_senid))) {
			/* printf("Frame %d best senone %d score %d\n",
			   frame_idx, best_senid, best_score); */
			if (frame_counter < 128)
				bestsen1[frame_counter] = best_senid;
			TEST_EQUAL(frame_counter, frame_idx);
			++frame_counter;
		}
	}

	/* Now try to process the whole thing at once. */
	nsamps = ftell(rawfh) / sizeof(*buf);
	clearerr(rawfh);
	fseek(rawfh, 0, SEEK_SET);
	buf = ckd_realloc(buf, nsamps * sizeof(*buf));
	TEST_EQUAL(nsamps, fread(buf, sizeof(*buf), nsamps, rawfh));
	bptr = buf;
	TEST_EQUAL(0, acmod_start_utt(acmod));
	acmod_process_raw(acmod, &bptr, &nsamps, TRUE);
	TEST_EQUAL(0, acmod_end_utt(acmod));
	{
		ascr_t const *senscr;
		int frame_idx, best_score, best_senid;
		frame_counter = 0;
		while ((senscr = acmod_score(acmod, &frame_idx,
					     &best_score, &best_senid))) {
			/* printf("Frame %d best senone %d score %d\n",
			   frame_idx, best_senid, best_score); */
			if (frame_counter < 128)
				TEST_EQUAL(best_senid, bestsen1[frame_counter]);
			TEST_EQUAL(frame_counter, frame_idx);
			++frame_counter;
		}
	}
	/* Clean up, go home. */
	fclose(rawfh);
	ckd_free(buf);
	acmod_free(acmod);
	return 0;
}