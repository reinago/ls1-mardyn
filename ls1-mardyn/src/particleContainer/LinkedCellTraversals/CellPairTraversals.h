/*
 * CellPairTraversalWithDependencies.h
 *
 *  Created on: 15 May 2017
 *      Author: tchipevn
 */

#ifndef SRC_PARTICLECONTAINER_LINKEDCELLTRAVERSALS_CELLPAIRTRAVERSALS_H_
#define SRC_PARTICLECONTAINER_LINKEDCELLTRAVERSALS_CELLPAIRTRAVERSALS_H_

#include <vector>
#include <array>

class CellProcessor;

struct CellPairTraversalData {
	virtual ~CellPairTraversalData() {}
};

template <class CellTemplate>
class CellPairTraversals {
public:
	CellPairTraversals(
		std::vector<CellTemplate>& cells,
		const std::array<unsigned long, 3>& dims): _cells(&cells), _dims(dims) {}

	virtual ~CellPairTraversals() {}

	/**
     * Reset all necessary data without reallocation.
     */
	virtual void rebuild(std::vector<CellTemplate> &cells,
						 const std::array<unsigned long, 3> &dims,
						 CellPairTraversalData *data) {
		_cells = &cells;
		_dims = dims;
	};

	virtual void traverseCellPairs(CellProcessor& cellProcessor) = 0;
	virtual void traverseCellPairsOuter(CellProcessor& cellProcessor) = 0;
	virtual void traverseCellPairsInner(CellProcessor& cellProcessor, unsigned stage, unsigned stageCount) = 0;

protected:
	//TODO:
	//void traverseCellPairsNoDep(CellProcessor& cellProcessor);
	std::vector<CellTemplate> * _cells;
	std::array<unsigned long, 3> _dims;
};

#endif /* SRC_PARTICLECONTAINER_LINKEDCELLTRAVERSALS_CELLPAIRTRAVERSALS_H_ */