/*
============================================================================
DELLY: Structural variant discovery by integrated PE mapping and SR analysis
============================================================================
Copyright (C) 2012 Tobias Rausch

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Tobias Rausch (rausch@embl.de)
============================================================================
*/

#ifndef SPANNING_H
#define SPANNING_H

#include <boost/container/flat_set.hpp>
#include <boost/unordered_map.hpp>
#include <htslib/sam.h>
#include "tags.h"

namespace torali {


  template<typename TDefaultOrientation>
    inline bool
    _mateIsUpstream(TDefaultOrientation defOrient, bool firstRead, bool reverse) {
    if (firstRead) {
      if (reverse) {
	if (defOrient % 2 == 0) return false;
	else return true;
      } else {
	if (defOrient % 2 == 0) return true;
	else return false;
      }
    } else {
      if (reverse) {
	if ((defOrient==1) || (defOrient==2)) return false;
	else return true;
      } else {
	if ((defOrient==1) || (defOrient==2)) return true;
	else return false;
      }
    }
  }   

  template<typename TFiles, typename TSampleLibrary, typename TSVs, typename TCountMap, typename TSVType>
    inline void
    annotateSpanningCoverage(TFiles const& files, uint8_t const minMapQual, TSampleLibrary& sampleLib, TSVs& svs, TCountMap& spanCountMap, TSVType svType)
  {
    typedef typename TCountMap::key_type TSampleSVPair;
    typedef typename TCountMap::mapped_type TCountPair;
    typedef typename TSampleLibrary::value_type TLibraryMap;

    // Open file handles
    typedef std::vector<samFile*> TSamFile;
    typedef std::vector<hts_idx_t*> TIndex;
    TSamFile samfile;
    TIndex idx;
    samfile.resize(files.size());
    idx.resize(files.size());
    for(unsigned int file_c = 0; file_c < files.size(); ++file_c) {
      samfile[file_c] = sam_open(files[file_c].string().c_str(), "r");
      idx[file_c] = sam_index_load(samfile[file_c], files[file_c].string().c_str());
    }

    // Get maximum insert size
    int maxInsertSize = 0;
    for(unsigned int file_c = 0; file_c < files.size(); ++file_c)
      for(typename TLibraryMap::iterator libIt = sampleLib[file_c].begin(); libIt != sampleLib[file_c].end(); ++libIt)
	if (libIt->second.median > maxInsertSize) maxInsertSize=libIt->second.median;
    
    // Sort Structural Variants
    std::sort(svs.begin(), svs.end(), SortSVs<StructuralVariantRecord>());

    // Initialize count map
    for(unsigned int file_c = 0; file_c < files.size(); ++file_c) {
      for(typename TSVs::const_iterator itSV = svs.begin(); itSV!=svs.end(); ++itSV) {
	// Left breakpoint
	spanCountMap.insert(std::make_pair(std::make_pair(file_c, -itSV->id), TCountPair()));
	// Right breakpoint
	spanCountMap.insert(std::make_pair(std::make_pair(file_c, itSV->id), TCountPair()));
      }
    }

    // Iterate all samples
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Breakpoint spanning coverage annotation" << std::endl;
    boost::progress_display show_progress( (svs.end() - svs.begin()) );
#pragma omp parallel for default(shared)
    for(unsigned int file_c = 0; file_c < files.size(); ++file_c) {
      // Read alignments
      typename TSVs::const_iterator itSV = svs.begin();
      typename TSVs::const_iterator itSVEnd = svs.end();
      for(;itSV!=itSVEnd;++itSV) {
	if (file_c==(files.size()-1)) ++show_progress;
	if (itSV->peSupport == 0) continue;

	// Set up the mapping quality iterators
	TSampleSVPair svSample = std::make_pair(file_c, -itSV->id);
	typename TCountMap::iterator leftIt = spanCountMap.find(svSample);
	svSample = std::make_pair(file_c, itSV->id);
	typename TCountMap::iterator rightIt = spanCountMap.find(svSample);

	// Qualities
	typedef boost::unordered_map<std::size_t, uint8_t> TQualities;
	TQualities qualities;

	// Pre-compute regions
	unsigned int maxBp = 2;
	int32_t regionChr1 = itSV->chr;
	int regionStart1 = std::max(0, (int) itSV->svStart - (int) maxInsertSize);
	int regionEnd1 = itSV->svStart + maxInsertSize;
	int32_t regionChr2 = itSV->chr2;
	int regionStart2 = std::max(0, (int) itSV->svEnd - (int) maxInsertSize);
	int regionEnd2 = itSV->svEnd + maxInsertSize;
	if ((regionChr1 == regionChr2) && (regionEnd1 + maxInsertSize >= regionStart2)) {
	  // Small SV, scan area only once
	  maxBp = 1;
	  regionEnd1 = regionEnd2;
	}

	// Scan left and right breakpoint
	for (unsigned int bpPoint = 0; bpPoint < maxBp; ++bpPoint) {
	  int32_t regionChr;
	  int regionStart;
	  int regionEnd;
	  if (bpPoint==(unsigned int)(itSV->chr==itSV->chr2)) {
	    regionChr = regionChr2;
	    regionStart = regionStart2;
	    regionEnd = regionEnd2;
	  } else {
	    regionChr = regionChr1;
	    regionStart = regionStart1;
	    regionEnd = regionEnd1;
	  }
	  hts_itr_t* iter = sam_itr_queryi(idx[file_c], regionChr, regionStart, regionEnd);
	  bam1_t* rec = bam_init1();
	  while (sam_itr_next(samfile[file_c], iter, rec) >= 0) {
	    if (rec->core.flag & (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP | BAM_FSUPPLEMENTARY | BAM_FUNMAP | BAM_FMUNMAP)) continue;
	    if (!(rec->core.flag & BAM_FPAIRED) || (rec->core.qual < minMapQual)) continue;

	    // Mapping positions valid?
	    if (_mappingPosGeno(rec->core.tid, rec->core.mtid, rec->core.pos, rec->core.mpos, svType)) continue;

	    // Get the library information
	    std::string rG = "DefaultLib";
	    uint8_t *rgptr = bam_aux_get(rec, "RG");
	    if (rgptr) {
	      char* rg = (char*) (rgptr + 1);
	      rG = std::string(rg);
	    }
	    typename TLibraryMap::iterator libIt = sampleLib[file_c].find(rG);
	    if (libIt->second.median == 0) continue; // Single-end library
	    int outerISize = std::abs(rec->core.pos - rec->core.mpos) + rec->core.l_qseq;

	    // Abnormal paired-end
	    if ((getStrandIndependentOrientation(rec->core) != libIt->second.defaultOrient) || (outerISize < libIt->second.minNormalISize) || (outerISize > libIt->second.maxNormalISize) || (rec->core.tid!=rec->core.mtid)) {
	      if (_acceptedInsertSize(libIt->second, abs(rec->core.isize), svType)) continue;  // Normal paired-end (for deletions, insertions only - this uses minISizeCutoff and maxISizeCutoff)
	      if (_acceptedOrientation(libIt->second.defaultOrient, getStrandIndependentOrientation(rec->core), svType)) continue;  // Orientation disagrees with SV type
	      if (!(((itSV->chr == rec->core.tid) && (itSV->chr2 == rec->core.mtid)) || ((itSV->chr == rec->core.mtid) && (itSV->chr2 == rec->core.tid)))) continue;

	      // Does the pair confirm the SV
	      int32_t const minPos = _minCoord(rec->core.pos, rec->core.mpos, svType);
	      int32_t const maxPos = _maxCoord(rec->core.pos, rec->core.mpos, svType);

	      if (rec->core.tid==itSV->chr) {
		if (minPos < itSV->svStart) {
		  if (_pairsDisagree(minPos, maxPos, rec->core.l_qseq, libIt->second.maxNormalISize, itSV->svStart, itSV->svEnd, rec->core.l_qseq, libIt->second.maxNormalISize, _getSpanOrientation(rec->core, libIt->second.defaultOrient, svType), itSV->ct, svType)) continue;
		} else {
		  if (_pairsDisagree(itSV->svStart, itSV->svEnd, rec->core.l_qseq, libIt->second.maxNormalISize, minPos, maxPos, rec->core.l_qseq, libIt->second.maxNormalISize, itSV->ct, _getSpanOrientation(rec->core, libIt->second.defaultOrient, svType), svType)) continue;
		}
	      }
	    }
		    
	    // Get or store the mapping quality for the partner
	    if (_firstPairObs(rec->core.tid, rec->core.mtid, rec->core.pos, rec->core.mpos, svType)) {
	      uint8_t r2Qual = rec->core.qual;
	      uint8_t* ptr = bam_aux_get(rec, "AS");
	      if (ptr) {
		int score = std::abs((int) bam_aux2i(ptr));
		r2Qual = std::min(r2Qual, (uint8_t) ( (score<255) ? score : 255 ));
	      }
	      qualities[hash_pair(rec)] = r2Qual;
	    } else {
	      // Get the two mapping qualities
	      uint8_t r2Qual = rec->core.qual;
	      uint8_t* ptr = bam_aux_get(rec, "AS");
	      if (ptr) {
		int score = std::abs((int) bam_aux2i(ptr));
		r2Qual = std::min(r2Qual, (uint8_t) ( (score<255) ? score : 255 ));
	      }
	      uint8_t pairQuality = std::min(qualities[hash_pair_mate(rec)], r2Qual);

	      // Pair quality
	      if (pairQuality < minMapQual) continue;
	      
	      // Insert the interval
	      if ((getStrandIndependentOrientation(rec->core) == libIt->second.defaultOrient) && (outerISize >= libIt->second.minNormalISize) && (outerISize <= libIt->second.maxNormalISize) && (rec->core.tid==rec->core.mtid)) {
		// Normal spanning coverage, take inner insert-size
		//int32_t sPosStart = std::min(rec->core.pos, rec->core.mpos);
		//int32_t ePosStart = std::min(rec->core.pos, rec->core.mpos) + rec->core.l_qseq;
		//int32_t sPosEnd = std::max(rec->core.pos, rec->core.mpos);
		//int32_t ePosEnd = std::max(rec->core.pos, rec->core.mpos) + rec->core.l_qseq;
		//if ((itSV->chr==rec->core.tid) && (itSV->svStart>=sPosStart) && (itSV->svStart<=ePosStart)) leftIt->second.first.push_back(pairQuality);
		//if ((itSV->chr2==rec->core.tid) && (itSV->svEnd>=sPosEnd) && (itSV->svEnd<=ePosEnd)) rightIt->second.first.push_back(pairQuality);
		int32_t sPos = std::min(rec->core.pos, rec->core.mpos);
		int32_t ePos = std::max(rec->core.pos, rec->core.mpos) + rec->core.l_qseq;
		int32_t midPoint = sPos+(ePos-sPos)/2;
		sPos=std::max(sPos, midPoint - rec->core.l_qseq);
		ePos=std::min(ePos, midPoint + rec->core.l_qseq);
		int32_t innerSPos = std::min(rec->core.pos, rec->core.mpos) + rec->core.l_qseq;
		int32_t innerEPos = std::max(rec->core.pos, rec->core.mpos);
		if ((innerSPos<innerEPos) && ((innerEPos - innerSPos) > (ePos-sPos))) {
		  sPos = innerSPos;
		  ePos = innerEPos;
		}
		if (std::abs(midPoint - itSV->svStart) < std::abs(itSV->svEnd - midPoint)) {
		  if ((itSV->chr==rec->core.tid) && (itSV->svStart>=sPos) && (itSV->svStart<=ePos)) leftIt->second.first.push_back(pairQuality);
		} else {
		  if ((itSV->chr2==rec->core.tid) && (itSV->svEnd>=sPos) && (itSV->svEnd<=ePos)) rightIt->second.first.push_back(pairQuality);
		}
	      } else if ((getStrandIndependentOrientation(rec->core) != libIt->second.defaultOrient) || (outerISize < libIt->second.minNormalISize) || (outerISize > libIt->second.maxNormalISize) || (rec->core.tid!=rec->core.mtid)) {
		// Missing spanning coverage
		if (_mateIsUpstream(libIt->second.defaultOrient, (rec->core.flag & BAM_FREAD1), (rec->core.flag & BAM_FREVERSE))) {
		  if ((itSV->chr==rec->core.tid) && (itSV->svStart>=rec->core.pos) && (itSV->svStart<=(rec->core.pos + libIt->second.maxNormalISize))) leftIt->second.second.push_back(pairQuality);
		  if ((itSV->chr2==rec->core.tid) && (itSV->svEnd>=rec->core.pos) && (itSV->svEnd<=(rec->core.pos + libIt->second.maxNormalISize))) rightIt->second.second.push_back(pairQuality);
		} else {
		  if ((itSV->chr==rec->core.tid) && (itSV->svStart>=std::max(0, rec->core.pos + rec->core.l_qseq - libIt->second.maxNormalISize)) && (itSV->svStart<=(rec->core.pos + rec->core.l_qseq))) leftIt->second.second.push_back(pairQuality);
		  if ((itSV->chr2==rec->core.tid) && (itSV->svEnd>=std::max(0, rec->core.pos + rec->core.l_qseq - libIt->second.maxNormalISize)) && (itSV->svEnd<=(rec->core.pos + rec->core.l_qseq))) rightIt->second.second.push_back(pairQuality);
		}
		if (_mateIsUpstream(libIt->second.defaultOrient, !(rec->core.flag & BAM_FREAD1), (rec->core.flag & BAM_FMREVERSE))) {
		  if ((itSV->chr==rec->core.mtid) && (itSV->svStart>=rec->core.mpos) && (itSV->svStart<=(rec->core.mpos + libIt->second.maxNormalISize))) leftIt->second.second.push_back(pairQuality);
		  if ((itSV->chr2==rec->core.mtid) && (itSV->svEnd>=rec->core.mpos) && (itSV->svEnd<=(rec->core.mpos + libIt->second.maxNormalISize))) rightIt->second.second.push_back(pairQuality);
		} else {
		  if ((itSV->chr==rec->core.mtid) && (itSV->svStart>=std::max(0, rec->core.mpos + rec->core.l_qseq - libIt->second.maxNormalISize)) && (itSV->svStart<=(rec->core.mpos + rec->core.l_qseq))) leftIt->second.second.push_back(pairQuality);
		  if ((itSV->chr2==rec->core.mtid) && (itSV->svEnd>=std::max(0,rec->core.mpos + rec->core.l_qseq - libIt->second.maxNormalISize)) && (itSV->svEnd<=(rec->core.mpos + rec->core.l_qseq))) rightIt->second.second.push_back(pairQuality);
		}
	      }
	    }
	  }
	  bam_destroy1(rec);
	  hts_itr_destroy(iter);
	}
      }
    }
    // Clean-up
    for(unsigned int file_c = 0; file_c < files.size(); ++file_c) {
      hts_idx_destroy(idx[file_c]);
      sam_close(samfile[file_c]);
    }
  }

}

#endif
