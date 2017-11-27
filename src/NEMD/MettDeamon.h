/*
 * MettDeamon.h
 *
 *  Created on: 03.04.2017
 *      Author: thet
 */

#ifndef METTDEAMON_H_
#define METTDEAMON_H_

#include "molecules/Molecule.h"

#include <map>
#include <array>
#include <fstream>
#include <vector>
#include <cstdint>
#include <limits>

#define FORMAT_SCI_MAX_DIGITS std::setw(24) << std::scientific << std::setprecision(std::numeric_limits<double>::digits10)

enum ReadReservoirMethods : uint8_t
{
	RRM_UNKNOWN = 0,
	RRM_READ_FROM_FILE = 1,
	RRM_READ_FROM_MEMORY = 2,
	RRM_AMBIGUOUS = 3,
};

class Domain;
class Ensemble;
class DomainDecompBase;
class ParticleContainer;
class XMLfileUnits;

class MettDeamon
{
public:
	MettDeamon();
	~MettDeamon();

	void readXML(XMLfileUnits& xmlconfig);

	uint64_t getnNumMoleculesDeleted( DomainDecompBase* domainDecomposition){return _nNumMoleculesDeletedGlobalAlltime;}
	uint64_t getnNumMoleculesDeleted2( DomainDecompBase* domainDecomposition);

	void prepare_start(DomainDecompBase* domainDecomp, ParticleContainer* particleContainer, double cutoffRadius);
	void init_positionMap(ParticleContainer* particleContainer);
	void preForce_action(ParticleContainer* particleContainer, double cutoffRadius);
	void postForce_action(ParticleContainer* particleContainer, DomainDecompBase* domainDecomposition);

	// connection to DensityControl
	void IncrementDeletedMoleculesLocal() {_nNumMoleculesDeletedLocal++;}

private:
	void ReadReservoir(DomainDecompBase* domainDecomp);
	void ReadReservoirFromFile(DomainDecompBase* domainDecomp);
	void ReadReservoirFromMemory(DomainDecompBase* domainDecomp);
	void DetermineMaxMoleculeIDs(DomainDecompBase* domainDecomp);
	void writeRestartfile();
	void calcDeltaY() { _dY = _dDeletedMolsPerTimestep * _dInvDensityArea; }

private:
	double _rho_l;
	double _dAreaXZ;
	double _dInvDensityArea;
	double _dY;
	double _dYInit;
	double _dYsum;
	double _velocityBarrier;
	double _dSlabWidthInit;
	double _dSlabWidth;
	double _dReservoirWidthY;
	uint64_t _nUpdateFreq;
	uint64_t _nWriteFreqRestart;
	uint64_t _nMaxMoleculeID;
	uint64_t _nMaxReservoirMoleculeID;
	uint64_t _nNumMoleculesDeletedLocal;
	uint64_t _nNumMoleculesDeletedGlobal;
	uint64_t _nNumMoleculesDeletedGlobalAlltime;
	uint64_t _nNumMoleculesTooFast;
	uint64_t _nNumMoleculesTooFastGlobal;
	uint64_t _reservoirNumMolecules;
	uint64_t _reservoirSlabs;
	int32_t _nSlabindex;
	uint8_t _nReadReservoirMethod;
	std::string _reservoirFilename;
	std::map<uint64_t, std::array<double,10> > _storePosition;  //Map for frozen particle position storage <"id, position">
	std::vector< std::vector<Molecule> >_reservoir;
	bool _bIsRestart;  // simulation is a restart?
	std::list<uint64_t> _listDeletedMolecules;
	uint32_t _nNumValsSummation;
	uint64_t _numDeletedMolsSum;
	double _dDeletedMolsPerTimestep;
	double _dInvNumTimestepsSummation;
	bool _bMirrorActivated;
	double _dMirrorPosY;
	// identity change (by component ID)
	std::vector<uint32_t> _vecChangeCompIDsFreeze;
	std::vector<uint32_t> _vecChangeCompIDsUnfreeze;
	uint64_t _nDeleteNonVolatile;
	double _dMoleculeDiameter;
	double _dTransitionPlanePosY;
	// throttle parameters for each component
	std::vector<double> _vecThrottleFromPosY;
	std::vector<double> _vecThrottleToPosY;
	std::vector<double> _vecThrottleForceY;
	std::vector<double> _vecVeloctiyBarriers;
};

#endif /* METTDEAMON_H_ */