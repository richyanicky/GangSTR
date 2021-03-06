/*
Copyright (C) 2017 Melissa Gymrek <mgymrek@ucsd.edu>
and Nima Mousavi (mousavi@ucsd.edu)

This file is part of GangSTR.

GangSTR is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

GangSTR is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GangSTR.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>

#include "src/genotyper.h"
#include "src/mathops.h"

using namespace std;

Genotyper::Genotyper(RefGenome& _refgenome,
		    Options& _options) {
  refgenome = &_refgenome;
  options = &_options;
  read_extractor = new ReadExtractor(_options);
  likelihood_maximizer = new LikelihoodMaximizer(_options);
}

bool Genotyper::SetFlanks(Locus* locus) {
  if (!refgenome->GetSequence(locus->chrom,
			      locus->start-options->realignment_flanklen-1,
			      locus->start-2,
			      &locus->pre_flank)) {
    return false;
  }
  if (!refgenome->GetSequence(locus->chrom,
			      locus->end,
			      locus->end+options->realignment_flanklen-1,
			      &locus->post_flank)) {
    return false;
  }
  return true;
}

bool Genotyper::ProcessLocus(BamCramMultiReader* bamreader, Locus* locus) {
  int32_t read_len = options->read_len;

  // Load preflank and postflank to locus
  if (options->verbose) {
    PrintMessageDieOnError("\tSetting flanking regions", M_PROGRESS);
  }
  if (!SetFlanks(locus)) {
    return false;
  }

  likelihood_maximizer->Reset();

  // Load all read data
  if (options->verbose) {
    PrintMessageDieOnError("\tLoading read data", M_PROGRESS);
  }
  if (!read_extractor->ExtractReads(bamreader, *locus, likelihood_maximizer->options->regionsize,
				    likelihood_maximizer->options->min_match, likelihood_maximizer)) {
    return false;
  }

  locus->enclosing_reads = likelihood_maximizer->GetEnclosingDataSize();
  locus->spanning_reads = likelihood_maximizer->GetSpanningDataSize();
  locus->frr_reads = likelihood_maximizer->GetFRRDataSize();
  locus->flanking_reads = likelihood_maximizer->GetFlankingDataSize();
  // Set flags if only spanning reads available.
  if (locus->frr_reads + locus->flanking_reads + locus->enclosing_reads < 4){
    if (options->verbose) {
      stringstream msg;
      msg<<"\tNot enough reads extracted. Enclosing: "<<locus->enclosing_reads
	 <<", Spanning: "<<locus->spanning_reads
	 <<", FRR: "<<locus->frr_reads
	 <<", Flanking: "<<locus->flanking_reads
	 <<". Skipping locus";
      PrintMessageDieOnError(msg.str(), M_PROGRESS);
    }
    return false;
  }

  // Maximize the likelihood
  if (options->verbose) {
    PrintMessageDieOnError("\tMaximizing likelihood", M_PROGRESS);
  }
  int32_t allele1, allele2;
  int32_t ref_count = (int32_t)((locus->end-locus->start+1)/locus->motif.size());
  double min_negLike, lob1, lob2, hib1, hib2;
  bool resampled = false;
  try {
    if (!likelihood_maximizer->OptimizeLikelihood(read_len, 
						(int32_t)(locus->motif.size()),
						ref_count, 
						resampled, 
						options->ploidy, 
						0,
						locus->offtarget_share,
						&allele1, 
						&allele2, 
						&min_negLike)) {
      return false;
    }
    locus->allele1 = allele1;
    locus->allele2 = allele2;
    locus->min_neg_lik = min_negLike;
    locus->enclosing_reads = likelihood_maximizer->GetEnclosingDataSize();
    locus->spanning_reads = likelihood_maximizer->GetSpanningDataSize();
    locus->frr_reads = likelihood_maximizer->GetFRRDataSize();
    locus->flanking_reads = likelihood_maximizer->GetFlankingDataSize();
    locus->depth = likelihood_maximizer->GetReadPoolSize();
    
    if (options->num_boot_samp > 0){
      if (options->verbose) {
	PrintMessageDieOnError("\tGetting confidence intervals", M_PROGRESS);
      }
      try{
	if (!likelihood_maximizer->GetConfidenceInterval(read_len, (int32_t)(locus->motif.size()),
							 ref_count, allele1, allele2, *locus,
							 &lob1, &hib1, &lob2, &hib2)) {
	  return false;
	}
	locus->lob1 = lob1;
	locus->lob2 = lob2;
	locus->hib1 = hib1;
	locus->hib2 = hib2;
	

	stringstream msg;
	msg<<"\tGenotyper Results:  "<<allele1<<", "<<allele2<<"\tlikelihood = "<<min_negLike;
	PrintMessageDieOnError(msg.str(), M_PROGRESS);
	if (options->verbose) {
	  msg.clear();
	  msg.str(std::string());
	  msg<<"\tSmall Allele Bound: ["<<lob1<<", "<<hib1<<"]";
	  PrintMessageDieOnError(msg.str(), M_PROGRESS);
	  msg.clear();
	  msg.str(std::string());
	  msg<<"\tLarge Allele Bound: ["<<lob2<<", "<<hib2<<"]";
	  PrintMessageDieOnError(msg.str(), M_PROGRESS);
	}
      }
      catch (std::exception &exc){
	if (options->verbose) {
	  stringstream msg;
	  msg<<"\tEncountered error("<< exc.what() <<") in likelihood maximization. Skipping locus";
	  PrintMessageDieOnError(msg.str(), M_PROGRESS);
	}
	return false;
      }
    }
  }
  catch (std::exception &exc){
    if (options->verbose) {
      stringstream msg;
      msg<<"\tEncountered error("<< exc.what() <<") in likelihood maximization. Skipping locus";
      PrintMessageDieOnError(msg.str(), M_PROGRESS);
    }
    return false;
  }
  return true;
}

void Genotyper::Debug(BamCramMultiReader* bamreader) {
  cerr << "testing refgenome" << endl;
  std::string seq;
  refgenome->GetSequence("3", 63898261, 63898360, &seq);
  cerr << seq << endl;
  cerr << "testing bam" << endl;
  bamreader->SetRegion("1", 0, 10000);
  BamAlignment aln;
  if (bamreader->GetNextAlignment(aln)) { // Requires SetRegion was called
    std::string testread = aln.QueryBases();
    cerr << testread << endl;
  } else {
    cerr << "testing bam failed" << endl;
  }
  cerr << "testing GSL" << endl;
  double x = TestGSL();
  cerr << "gsl_ran_gaussian_pdf(0, 1) " << x << endl;
  //  double y = TestNLOPT();
}

Genotyper::~Genotyper() {
  delete read_extractor;
  delete likelihood_maximizer;
}
