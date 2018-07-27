/*
 * FullParticleCell.h
 *
 *  Created on: 6 Feb 2017
 *      Author: tchipevn
 */

#ifndef SRC_PARTICLECONTAINER_FULLPARTICLECELL_H_
#define SRC_PARTICLECONTAINER_FULLPARTICLECELL_H_

#include <vector>

#include "Cell.h"
#include "particleContainer/ParticleCellBase.h"
#include "particleContainer/adapter/CellDataSoA.h"

//! @brief FullParticleCell data structure. Renamed from ParticleCell.
//! @author Martin Buchholz
//!
//! A FullParticleCell represents a small cuboid area of the domain and stores a list of
//! pointers to the molecules in that area. Depending on the actual position
//! of the cell, it belongs to one of four different regions: \n
//! - completele outside (more than the cutoffradius away from all cells that
//!                       belong directly to the MoleculeContainer)
//!                       Such a cell shouldn't exist!!!
//! - halo region (not belonging directly to the MoleculeContainer, but within
//!                       the cutoffradius of at least one cell of the MoleculeContainer)
//! - boundary region (belonging directly to the MoleculeContainer, but not more than
//!                           the cutoffradius away from the boundary)
//! - inner region (more than the cutoffradius away from the boundary)
//!
//! There are three boolean member variables for the last three regions. \n
//! If more than one of them is true, there must be an error in the code \n
//! If none of them is true, the cell wasn't assigned to any region yet.
//! A cell which is completely outside shouldn't exist, as it completely useless.
/**
 * \details <br>(Johannes Heckl)<br>
 * Also stores data for various CellProcessor%s.<br>
 * If you add a new data member, update the _assign() method with deep copy<br>
 * semantics for the new data member.<br>
 * Uses the default copy constructor and the default assignment operator despite<br>
 * having pointer data members. This is because these data members are not controlled<br>
 * by the FullParticleCell itself, but by the various CellProcessor%s so FullParticleCell can not<br>
 * know the proper copy semantics. This should not cause any problems because no copy<br>
 * actions should be executed during CellProcessor applications.
 */

class FullParticleCell: public ParticleCellBase {
	/*private:
	 FullParticleCell(const ParticleCell& that);*/
public:
	/**
	 * \brief Initialize data pointers to 0.
	 * \author Johannes Heckl
	 */
	FullParticleCell();
	/**
	 * \brief Destructor.
	 * \author Johannes Heckl
	 */
	~FullParticleCell();

	//! removes and deallocates all elements
	void deallocateAllParticles() override;

	//! insert a single molecule into this cell
	bool addParticle(Molecule& particle, bool checkWhetherDuplicate = false) override;

	bool isEmpty() const override;

	bool deleteMoleculeByIndex(size_t index) override;

	//! return the number of molecules contained in this cell
	int getMoleculeCount() const override;

	/**
	 * \brief Get the structure of arrays for VectorizedCellProcessor.
	 * \author Johannes Heckl
	 */
	CellDataSoA& getCellDataSoA() {
		return _cellDataSoA;
	}

	/**
	 * filter molecules which have left the box
	 * @return field vector containing leaving molecules
	 */
	//std::vector<Molecule> & filterLeavingMolecules();
	void preUpdateLeavingMolecules() override;

	void updateLeavingMoleculesBase(ParticleCellBase& otherCell) override;

	void postUpdateLeavingMolecules() override;

	void getRegion(double lowCorner[3], double highCorner[3],
			std::vector<Molecule*> &particlePtrs, bool removeFromContainer = false) override;

	void buildSoACaches() override;

	void increaseMoleculeStorage(size_t numExtraMols) override;

	virtual size_t getMoleculeVectorDynamicSize() const override {
		return _molecules.capacity() * sizeof(Molecule) + _leavingMolecules.capacity() * sizeof(Molecule);
	}

//protected: do not use!
	void moleculesAtNew(size_t i, Molecule *& multipurposePointer) override {
		multipurposePointer = & _molecules.at(i);
	}

	void moleculesAtConstNew(size_t i, Molecule *& multipurposePointer) const override {
		multipurposePointer = const_cast<Molecule*>(& _molecules.at(i));
	}

	void assignCellToHaloRegion() { haloCell = true; }
	void assignCellToBoundaryRegion() { boundaryCell = true; }
	void assignCellToInnerRegion() { innerCell = true; }
	void assignCellToInnerMostAndInnerRegion() { innerCell = true; innerMostCell = true; }

	void skipCellFromHaloRegion() { haloCell = false; }
	void skipCellFromBoundaryRegion() { boundaryCell = false; }
	void skipCellFromInnerRegion() { innerCell = false; }
	void skipCellFromInnerMostRegion() { innerMostCell = false; }

	bool isHaloCell() const { return haloCell; }
	bool isBoundaryCell() const { return boundaryCell; }
	bool isInnerCell() const { return innerCell; }
	bool isInnerMostCell() const { return innerMostCell; }

	double getBoxMin(int d) const { return _boxMin[d]; }
	double getBoxMax(int d) const { return _boxMax[d]; }
	void setBoxMin(const double b[3]) { for (int d = 0; d < 3; ++d) { _boxMin[d] = b[d]; } }
	void setBoxMax(const double b[3]) { for (int d = 0; d < 3; ++d) { _boxMax[d] = b[d]; } }

private:

	void updateLeavingMolecules(FullParticleCell& otherCell);

	//! true when the cell is in the halo region
	bool haloCell;
	//! true when the cell is in the boundary region
	bool boundaryCell;
	//! true when the cell is in the inner region. Innermost cells are always also innerCells.
	bool innerCell;
	//! true when the cell is in the innermost region (does not have neighbors, that are boundary cells)
	bool innerMostCell;
	//! lower left front corner
	double _boxMin[3];
	//! upper right back corner
	double _boxMax[3];

	/**
	 * \brief A vector of pointers to the Molecules in this cell.
	 */
	std::vector<Molecule> _molecules;

	/**
	 * \brief A vector of molecules, which have left this cell.
	 */
	std::vector<Molecule> _leavingMolecules;

	/**
	 * \brief Structure of arrays for VectorizedCellProcessor.
	 * \author Johannes Heckl
	 */
	CellDataSoA _cellDataSoA;
};

#endif /* SRC_PARTICLECONTAINER_FULLPARTICLECELL_H_ */