//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#include <set>
#include <iostream>
#include <cassert>
#include <algorithm>
#include <thread>
#include <ctime>
#include <queue>

#include "overlap.h"
#include "../common/config.h"
#include "../common/utils.h"
#include "../common/parallel.h"
#include "../common/disjoint_set.h"


//Check if it is a proper overlap
bool OverlapDetector::overlapTest(const OverlapRange& ovlp,
								  bool& outSuggestChimeric) const
{
	static const float OVLP_DIVERGENCE = Config::get("overlap_divergence_rate");
	if (ovlp.curRange() < _minOverlap || 
		ovlp.extRange() < _minOverlap) 
	{
		return false;
	}

	float lengthDiff = abs(ovlp.curRange() - ovlp.extRange());
	float meanLength = (ovlp.curRange() + ovlp.extRange()) / 2.0f;
	if (lengthDiff > meanLength * OVLP_DIVERGENCE)
	{
		return false;
	}

	if (ovlp.curId == ovlp.extId.rc()) outSuggestChimeric = true;
	if (_checkOverhang)
	{
		if (std::min(ovlp.curBegin, ovlp.extBegin) > 
			_maxOverhang) 
		{
			return false;
		}
		if (std::min(ovlp.curLen - ovlp.curEnd, ovlp.extLen - ovlp.extEnd) > 
			_maxOverhang)
		{
			return false;
		}
	}

	return true;
}


namespace
{
	struct KmerMatch
	{
		KmerMatch(int32_t cur = 0, int32_t ext = 0,
				  FastaRecord::Id extId = FastaRecord::ID_NONE): 
			curPos(cur), extPos(ext), extId(extId) {}
		int32_t curPos;
		int32_t extPos;
		FastaRecord::Id extId;
	};

	struct MatchVecWrapper
	{
		MatchVecWrapper(){}
		MatchVecWrapper(const KmerMatch& match, size_t capacity):
			v(new std::vector<KmerMatch>)
		{
			v->reserve(capacity);
			v->push_back(match);
		}
		std::shared_ptr<std::vector<KmerMatch>> v;
		std::vector<KmerMatch>* operator->() {return v.get();}
		std::vector<KmerMatch>& operator*() {return *v;}
	};
}


//This implementation was inspired by Hen Li's minimap2 paper
std::vector<OverlapRange> 
OverlapDetector::getSeqOverlaps(const FastaRecord& fastaRec, 
								bool uniqueExtensions,
								bool& outSuggestChimeric) const
{
	const float MIN_KMER_SURV_RATE = 0.01;	//TODO: put into config
	const int MAX_SECONDARY_OVLPS = 5;
	const int MAX_LOOK_BACK = 50;
	const int kmerSize = Parameters::get().kmerSize;

	//static float totalDpTime = 0;
	//static float totalKmerTime = 0;
	//static float totalHashTime = 0;
	//clock_t begin = clock();

	outSuggestChimeric = false;
	int32_t curLen = fastaRec.sequence.length();

	std::vector<unsigned char> seqHitCount(_seqContainer.getMaxSeqId(), 0);

	std::vector<KmerMatch> vecMatches;
	vecMatches.reserve(10000000);

	for (auto curKmerPos : IterKmers(fastaRec.sequence))
	{
		if (!_vertexIndex.isSolid(curKmerPos.kmer)) continue;

		FastaRecord::Id prevSeqId = FastaRecord::ID_NONE;
		for (const auto& extReadPos : _vertexIndex.iterKmerPos(curKmerPos.kmer))
		{
			//no trivial matches
			if ((extReadPos.readId == fastaRec.id &&
				extReadPos.position == curKmerPos.position)) continue;

			//count one seq match for one unique k-mer
			//since k-mers in vector are stored relative to fwd strand,
			//check both read orientations
			if (prevSeqId != extReadPos.readId &&
				prevSeqId != extReadPos.readId.rc())
			{
				if (seqHitCount[extReadPos.readId.rawId()] <
					std::numeric_limits<unsigned char>::max())
				{
					++seqHitCount[extReadPos.readId.rawId()];
				}
			}
			prevSeqId = extReadPos.readId;

			vecMatches.emplace_back(curKmerPos.position, 
									extReadPos.position,
									extReadPos.readId);
		}
	}
	//auto hashTime = clock();
	//totalHashTime += double(hashTime - begin) / CLOCKS_PER_SEC;

	cuckoohash_map<FastaRecord::Id, MatchVecWrapper> seqMatches;
	seqMatches.reserve(500);
	for (auto& match : vecMatches)
	{
		if (seqHitCount[match.extId.rawId()] < 
			MIN_KMER_SURV_RATE * _minOverlap) continue;

		seqMatches.upsert(match.extId, 
				  [&seqHitCount, &match](MatchVecWrapper& v)
				  {
					  v->push_back(match);
				  }, 
				  match, seqHitCount[match.extId.rawId()]);	//if key is not found
		//MatchVecWrapper w = seqMatches[match.extId];
		//assert(w.v != 0);
	}
	
  	//clock_t end = clock();
  	//double elapsed_secs = double(end - hashTime) / CLOCKS_PER_SEC;
	//totalKmerTime += elapsed_secs;

	std::vector<OverlapRange> detectedOverlaps;
	//int uniqueCandidates = 0;
	for (auto& seqVec : seqMatches.lock_table())
	{
		const std::vector<KmerMatch>& matchesList = *seqVec.second;
		int32_t extLen = _seqContainer.seqLen(seqVec.first);

		//pre-filtering
		int32_t minCur = matchesList.front().curPos;
		int32_t maxCur = matchesList.back().curPos;
		int32_t minExt = std::numeric_limits<int32_t>::max();
		int32_t maxExt = std::numeric_limits<int32_t>::min();
		int32_t uniquePos = 0;
		int32_t prevPos = -1;
		for (auto& match : matchesList)
		{
			minExt = std::min(minExt, match.extPos);
			maxExt = std::max(maxExt, match.extPos);
			if (match.curPos != prevPos)
			{
				prevPos = match.curPos;
				++uniquePos;
			}
		}
		if (maxCur - minCur < _minOverlap || 
			maxExt - minExt < _minOverlap) continue;
		if (_checkOverhang)
		{
			if (std::min(minCur, minExt) > _maxOverhang) continue;
			if (std::min(curLen - maxCur, 
						 extLen - maxExt) > _maxOverhang) continue;
		}
		//++uniqueCandidates;
		//if (uniqueCandidates > MAX_EXT_SEQS) break;
		
		//chain matiching positions with DP
		std::vector<int32_t> scoreTable(matchesList.size(), 0);
		std::vector<int32_t> backtrackTable(matchesList.size(), -1);
		int32_t skipCurPos = 0;
		int32_t skipCurId = 0;
		for (int32_t i = 1; i < (int32_t)scoreTable.size(); ++i)
		{
			int32_t maxScore = 0;
			int32_t maxId = 0;
			int32_t curNext = matchesList[i].curPos;
			int32_t extNext = matchesList[i].extPos;
			int32_t noImprovement = 0;

			if (curNext != skipCurPos)
			{
				skipCurPos = curNext;
				skipCurId = i - 1;
			}

			for (int32_t j = skipCurId; j >= 0; --j)
			{
				int32_t nextScore = 0;
				int32_t curPrev = matchesList[j].curPos;
				int32_t extPrev = matchesList[j].extPos;
				if (0 < curNext - curPrev && curNext - curPrev < _maxJump &&
					0 < extNext - extPrev && extNext - extPrev < _maxJump)
				{
					int32_t matchScore = 
						std::min(std::min(curNext - curPrev, extNext - extPrev),
										  kmerSize);
					int32_t jumpDiv = abs((curNext - curPrev) - 
										  (extNext - extPrev));
					int32_t gapCost = jumpDiv ? 
							0.01f * kmerSize * jumpDiv + log2(jumpDiv) : 0;
					nextScore = scoreTable[j] + matchScore - gapCost;
					if (nextScore > maxScore)
					{
						maxScore = nextScore;
						maxId = j;
						noImprovement = 0;
					}
					else
					{
						if (++noImprovement > MAX_LOOK_BACK) break;
					}
				}
				if (curNext - curPrev > _maxJump) break;
			}

			scoreTable[i] = std::max(maxScore, kmerSize);
			if (maxScore > 0)
			{
				backtrackTable[i] = maxId;
			}
		}

		//backtracking
		std::vector<OverlapRange> extOverlaps;
		for (int32_t chainStart = backtrackTable.size() - 1; 
			 chainStart > 0; --chainStart)
		{
			int32_t pos = chainStart;
			KmerMatch lastMatch = matchesList[pos];
			KmerMatch firstMatch = lastMatch;
			std::vector<int32_t> shifts;
			shifts.reserve(1024);

			std::vector<std::pair<int32_t, int32_t>> kmerMatches;
			kmerMatches.reserve(1024);
			int chainLength = 0;
			int totalMatch = kmerSize;
			while (pos != -1)
			{
				firstMatch = matchesList[pos];
				shifts.push_back(matchesList[pos].curPos - 
								 matchesList[pos].extPos);
				++chainLength;

				int32_t prevPos = backtrackTable[pos];
				if (prevPos != -1)
				{
					int32_t curNext = matchesList[pos].curPos;
					int32_t extNext = matchesList[pos].extPos;
					int32_t curPrev = matchesList[prevPos].curPos;
					int32_t extPrev = matchesList[prevPos].extPos;
					int32_t matchScore = 
							std::min(std::min(curNext - curPrev, extNext - extPrev),
											  kmerSize);
					totalMatch += matchScore;
				}
				if (_keepAlignment)
				{
					if (kmerMatches.empty() || 
						kmerMatches.back().first - matchesList[pos].curPos >
						kmerSize)
					{
						kmerMatches.emplace_back(matchesList[pos].curPos,
								 				 matchesList[pos].extPos);
					}
				}

				int32_t newPos = backtrackTable[pos];
				backtrackTable[pos] = -1;
				pos = newPos;
			}
			std::reverse(kmerMatches.begin(), kmerMatches.end());

			OverlapRange ovlp(fastaRec.id, matchesList.front().extId,
							  firstMatch.curPos, firstMatch.extPos,
							  curLen, extLen);
			ovlp.curEnd = lastMatch.curPos + kmerSize - 1;
			ovlp.extEnd = lastMatch.extPos + kmerSize - 1;
			ovlp.leftShift = median(shifts);
			ovlp.rightShift = extLen - curLen + ovlp.leftShift;
			ovlp.score = scoreTable[chainStart];
			ovlp.kmerMatches = std::move(kmerMatches);

			if (totalMatch > MIN_KMER_SURV_RATE * ovlp.curRange() &&
				this->overlapTest(ovlp, outSuggestChimeric))
			{
				extOverlaps.push_back(ovlp);
				//Logger::get().debug() << ovlp.curRange() << " " <<
				//	" " << (float)ovlp.curRange() / chainLength <<
				//	" " << ovlp.score << " " << totalMatch;
			}
		}
		
		//selecting the best
		if (uniqueExtensions)
		{
			OverlapRange* maxOvlp = nullptr;
			for (auto& ovlp : extOverlaps)
			{
				if (!maxOvlp || ovlp.score > maxOvlp->score)
				{
					maxOvlp = &ovlp;
				}
			}
			if (maxOvlp) detectedOverlaps.push_back(*maxOvlp);
		}
		//.. or filtering collected overlaps
		else
		{
			//sort by decreasing score
			std::sort(extOverlaps.begin(), extOverlaps.end(),
					  [](const OverlapRange& r1, const OverlapRange& r2)
					  {return r1.score > r2.score;});

			std::vector<std::pair<OverlapRange*, int>> primaryOverlaps;
			std::vector<OverlapRange*> secondaryOverlaps;
			for (auto& ovlp : extOverlaps)
			{
				std::pair<OverlapRange*, int>* assignedPrimary = nullptr;
				bool isContained = false;
				for (auto& prim : primaryOverlaps)
				{
					int32_t intersect = ovlp.extIntersect(*prim.first);
					if (std::min(ovlp.extRange(), 
								 ovlp.extRange()) - intersect < kmerSize)
					{
						isContained = true;
						break;
					}
					if (intersect > ovlp.extRange() / 2)
					{
						assignedPrimary = &prim;
					}
				}
				if (isContained) continue;
				if (!assignedPrimary) 
				{
					primaryOverlaps.emplace_back(&ovlp, 0);
				}
				else if (assignedPrimary->second < MAX_SECONDARY_OVLPS)
				{
					secondaryOverlaps.push_back(&ovlp);
					++assignedPrimary->second;
				}
			}
			for (auto& ovlp : primaryOverlaps) 
			{
				detectedOverlaps.push_back(*ovlp.first);
			}
			for (auto& ovlp : secondaryOverlaps) 
			{
				detectedOverlaps.push_back(*ovlp);
			}
		}

		if (_maxCurOverlaps > 0 &&
			detectedOverlaps.size() > (size_t)_maxCurOverlaps) break;
	}

	//clock_t ff = clock();
	//double es = double(ff - end) / CLOCKS_PER_SEC;
	//totalDpTime += es;
	/*Logger::get().debug() << "---------";
	Logger::get().debug() << " " << vecMatches.size() << " " 
		<< uniqueCandidates << " " << detectedOverlaps.size();
	Logger::get().debug() << "hash: " << totalHashTime << " k-mer: " 
		<< totalKmerTime << " dp: " << totalDpTime;*/

	return detectedOverlaps;
}


std::vector<OverlapRange>
OverlapContainer::seqOverlaps(FastaRecord::Id seqId,
							  bool& outSuggestChimeric) const
{
	const FastaRecord& record = _queryContainer.getRecord(seqId);
	return _ovlpDetect.getSeqOverlaps(record, _onlyMax, outSuggestChimeric);
}


bool OverlapContainer::hasSelfOverlaps(FastaRecord::Id seqId)
{
	_indexMutex.lock();
	if (!_cached.count(seqId)) 
	{
		_indexMutex.unlock();
		this->lazySeqOverlaps(seqId);
		_indexMutex.lock();
	}
	bool selfOvlp = _suggestedChimeras.count(seqId);
	_indexMutex.unlock();
	return selfOvlp;
}

std::vector<OverlapRange>
	OverlapContainer::lazySeqOverlaps(FastaRecord::Id readId)
{
	_indexMutex.lock();
	if (!_cached.count(readId))
	{
		_indexMutex.unlock();
		bool suggestChimeric = false;
		auto overlaps = this->seqOverlaps(readId, suggestChimeric);
		_indexMutex.lock();
		this->storeOverlaps(overlaps, readId);
		if (suggestChimeric) 
		{
			_suggestedChimeras.insert(readId);
			_suggestedChimeras.insert(readId.rc());
		}
	}
	auto overlaps = _overlapIndex.at(readId);
	_indexMutex.unlock();
	return overlaps;
}

void OverlapContainer::storeOverlaps(const std::vector<OverlapRange>& overlaps,
									 FastaRecord::Id seqId)
{
	_cached.insert(seqId);
	_cached.insert(seqId.rc());

	auto& fwdOverlaps = _overlapIndex[seqId];
	auto& revOverlaps = _overlapIndex[seqId.rc()];

	std::unordered_set<FastaRecord::Id> extisting;
	if (_onlyMax)
	{
		for (auto& ovlp : fwdOverlaps) extisting.insert(ovlp.extId);
	}

	for (auto& ovlp : overlaps)
	{
		if (_onlyMax && extisting.count(ovlp.extId)) continue;

		auto revOvlp = ovlp.reverse();
		fwdOverlaps.push_back(ovlp);
		revOverlaps.push_back(ovlp.complement());
		_overlapIndex[revOvlp.curId].push_back(revOvlp);
		_overlapIndex[revOvlp.curId.rc()].push_back(revOvlp.complement());
	}
}

void OverlapContainer::findAllOverlaps()
{
	//Logger::get().info() << "Finding overlaps:";
	std::vector<FastaRecord::Id> allQueries;
	for (auto& seq : _queryContainer.iterSeqs())
	{
		allQueries.push_back(seq.id);
	}

	std::mutex indexMutex;
	std::function<void(const FastaRecord::Id&)> indexUpdate = 
	[this, &indexMutex] (const FastaRecord::Id& seqId)
	{
		auto& fastaRec = _queryContainer.getRecord(seqId);
		bool suggestChimeric = false;
		auto overlaps = _ovlpDetect.getSeqOverlaps(fastaRec, false, 
												   suggestChimeric);

		indexMutex.lock();
		this->storeOverlaps(overlaps, seqId);
		if (suggestChimeric) 
		{
			_suggestedChimeras.insert(seqId);
			_suggestedChimeras.insert(seqId.rc());
		}
		indexMutex.unlock();
	};

	processInParallel(allQueries, indexUpdate, 
					  Parameters::get().numThreads, true);

	int numOverlaps = 0;
	for (auto& seqOvlps : _overlapIndex) numOverlaps += seqOvlps.second.size();
	Logger::get().debug() << "Found " << numOverlaps << " overlaps";

	this->filterOverlaps();


	numOverlaps = 0;
	for (auto& seqOvlps : _overlapIndex) numOverlaps += seqOvlps.second.size();
	Logger::get().debug() << "Left " << numOverlaps 
		<< " overlaps after filtering";
}


void OverlapContainer::filterOverlaps()
{
	const int MAX_ENDS_DIFF = 100;

	std::vector<FastaRecord::Id> seqIds;
	for (auto& seq : _queryContainer.iterSeqs())
	{
		seqIds.push_back(seq.id);
	}

	std::function<void(const FastaRecord::Id& seqId)> filterParallel =
	[this] (const FastaRecord::Id& seqId)
	{
		auto& overlaps = _overlapIndex[seqId];
		
		std::vector<SetNode<OverlapRange*>*> overlapSets;
		for (auto& ovlp : overlaps) 
		{
			overlapSets.push_back(new SetNode<OverlapRange*>(&ovlp));
		}
		for (size_t i = 0; i < overlapSets.size(); ++i)
		{
			for (size_t j = 0; j < overlapSets.size(); ++j)
			{
				OverlapRange& ovlpOne = *overlapSets[i]->data;
				OverlapRange& ovlpTwo = *overlapSets[j]->data;

				if (ovlpOne.extId != ovlpTwo.extId) continue;
				int curDiff = ovlpOne.curRange() - ovlpOne.curIntersect(ovlpTwo);
				int extDiff = ovlpOne.extRange() - ovlpOne.extIntersect(ovlpTwo);

				if (curDiff < MAX_ENDS_DIFF && extDiff < MAX_ENDS_DIFF) 
				{
					unionSet(overlapSets[i], overlapSets[j]);
				}
			}
		}
		std::unordered_map<SetNode<OverlapRange*>*, 
						   std::vector<OverlapRange>> clusters;
		for (auto& ovlp: overlapSets) 
		{
			clusters[findSet(ovlp)].push_back(*ovlp->data);
		}
		overlaps.clear();
		for (auto& cluster : clusters)
		{
			OverlapRange* maxOvlp = nullptr;
			for (auto& ovlp : cluster.second)
			{
				if (!maxOvlp || ovlp.score > maxOvlp->score)
				{
					maxOvlp = &ovlp;
				}
			}
			overlaps.push_back(*maxOvlp);
		}
		for (auto& ovlpNode : overlapSets) delete ovlpNode;

		std::sort(overlaps.begin(), overlaps.end(), 
				  [](const OverlapRange& o1, const OverlapRange& o2)
				  {return o1.curBegin < o2.curBegin;});

	};
	processInParallel(seqIds, filterParallel, 
					  Parameters::get().numThreads, false);
}


void OverlapContainer::buildIntervalTree()
{
	Logger::get().debug() << "Building interval tree";
	for (auto& seqOvlps : _overlapIndex)
	{
		std::vector<Interval<OverlapRange*>> intervals;
		for (auto& ovlp : seqOvlps.second)
		{
			intervals.emplace_back(ovlp.curBegin, ovlp.curEnd, &ovlp);
		}
		_ovlpTree[seqOvlps.first] = IntervalTree<OverlapRange*>(intervals);
	}
}

std::vector<Interval<OverlapRange*>> 
	OverlapContainer::getOverlaps(FastaRecord::Id seqId, 
								  int32_t start, int32_t end) const
{
	return _ovlpTree.at(seqId).findOverlapping(start, end);
}
