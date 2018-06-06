/*
 * MettDeamon.cpp
 *
 *  Created on: 03.04.2017
 *      Author: thet
 */

#include "MettDeamon.h"
#include "Domain.h"
#include "molecules/Molecule.h"
#include "parallel/DomainDecompBase.h"
#ifdef ENABLE_MPI
#include "parallel/ParticleData.h"
#include "parallel/DomainDecomposition.h"
#endif
#include "particleContainer/ParticleContainer.h"
#include "utils/xmlfileUnits.h"
#include "utils/Random.h"
#include "io/ReplicaGenerator.h"  // class MoleculeDataReader

#include <map>
#include <array>
#include <fstream>
#include <vector>
#include <cmath>
#include <iomanip>
#include <numeric>

using namespace std;

MettDeamon::MettDeamon() :
		_reservoir(nullptr),
		_bIsRestart(false),
		_bMirrorActivated(false),
		_dAreaXZ(0.),
		_dInvDensityArea(0.),
		_velocityBarrier(0.),
		_dDeletedMolsPerTimestep(0.),
		_dInvNumTimestepsSummation(0.),
		_dMirrorPosY(0.),
		_dMoleculeDiameter(1.0),
		_dTransitionPlanePosY(0.0),
		_dDensityTarget(0.0),
		_dVolumeCV(0.0),
		_nUpdateFreq(0),
		_nWriteFreqRestart(0),
		_nMaxMoleculeID(0),
		_nMaxReservoirMoleculeID(0),
		_nNumMoleculesDeletedGlobalAlltime(0),
		_nNumMoleculesTooFast(0),
		_nNumMoleculesTooFastGlobal(0),
		_nMovingDirection(MD_UNKNOWN),
		_nFeedRateMethod(FRM_UNKNOWN),
		_nZone2Method(Z2M_UNKNOWN),
		_nNumValsSummation(0),
		_numDeletedMolsSum(0),
		_nDeleteNonVolatile(0)
{
	_dAreaXZ = global_simulation->getDomain()->getGlobalLength(0) * global_simulation->getDomain()->getGlobalLength(2);

	// init restart file
	std::ofstream ofs("MettDeamonRestart.dat", std::ios::out);
	std::stringstream outputstream;
	outputstream << "     simstep" << "   slabIndex" << "                  deltaY" << std::endl;
	ofs << outputstream.str();
	ofs.close();

	// summation of deleted molecules
	_listDeletedMolecules.clear();
	_listDeletedMolecules.push_back(0);

	// init identity change vector
	uint8_t nNumComponents = global_simulation->getEnsemble()->getComponents()->size();
	_vecChangeCompIDsFreeze.resize(nNumComponents);
	_vecChangeCompIDsUnfreeze.resize(nNumComponents);
	std::iota (std::begin(_vecChangeCompIDsFreeze), std::end(_vecChangeCompIDsFreeze), 0);
	std::iota (std::begin(_vecChangeCompIDsUnfreeze), std::end(_vecChangeCompIDsUnfreeze), 0);

	// throttle parameters
	_vecThrottleFromPosY.resize(nNumComponents);
	_vecThrottleToPosY.resize(nNumComponents);
	_vecThrottleForceY.resize(nNumComponents);
	for(uint8_t cid=0; cid<nNumComponents; ++cid)
	{
		_vecThrottleFromPosY.at(cid) = 0.;
		_vecThrottleToPosY.at(cid) = 0.;
		_vecThrottleForceY.at(cid) = 0.;
	}

	// velocity barriers
	_vecVeloctiyBarriers.resize(nNumComponents+1);

	// density values
	_vecDensityValues.clear();

	// particle reservoir
	_reservoir = new Reservoir(this);

	// init manipulation count for determination of the feed rate
	_feedrate.numMolecules.inserted.local = 0;
	_feedrate.numMolecules.deleted.local = 0;
	_feedrate.numMolecules.changed_to.local = 0;
	_feedrate.numMolecules.changed_from.local = 0;

	// init feed rate sum
	_feedrate.feed.sum = 0;
}

MettDeamon::~MettDeamon()
{
}

void MettDeamon::readXML(XMLfileUnits& xmlconfig)
{
	// control
	xmlconfig.getNodeValue("control/updatefreq", _nUpdateFreq);
	xmlconfig.getNodeValue("control/writefreq", _nWriteFreqRestart);
	xmlconfig.getNodeValue("control/numvals", _nNumValsSummation);
	_dInvNumTimestepsSummation = 1. / (double)(_nNumValsSummation*_nUpdateFreq);

	// vmax
//	xmlconfig.getNodeValue("control/vmax", _velocityBarrier);
	{
		uint8_t numNodes = 0;
		XMLfile::Query query = xmlconfig.query("control/vmax");
		numNodes = query.card();
		global_log->info() << "Number of vmax values: " << (uint32_t)numNodes << endl;
		if(numNodes < 1) {
			global_log->error() << "No vmax values defined in XML-config file. Program exit ..." << endl;
			Simulation::exit(-1);
		}
		string oldpath = xmlconfig.getcurrentnodepath();
		XMLfile::Query::const_iterator nodeIter;
		for( nodeIter = query.begin(); nodeIter; nodeIter++ ) {
			xmlconfig.changecurrentnode(nodeIter);
			uint32_t cid;
			double vmax;
			xmlconfig.getNodeValue("@cid", cid);
			xmlconfig.getNodeValue(".", vmax);
			_vecVeloctiyBarriers.at(cid) = vmax;
		}
		xmlconfig.changecurrentnode(oldpath);
	}
	// Feed rate
	{
		xmlconfig.getNodeValue("control/feed/targetID", _feedrate.cid_target);
		xmlconfig.getNodeValue("control/feed/init", _feedrate.feed.init);

		_nMovingDirection = MD_UNKNOWN;
		int nVal = 0;
		xmlconfig.getNodeValue("control/feed/direction", nVal);
		if(1 == nVal)
			_nMovingDirection = MD_LEFT_TO_RIGHT;
		else if(2 == nVal)
			_nMovingDirection = MD_RIGHT_TO_LEFT;

		_nFeedRateMethod = FRM_UNKNOWN;
		nVal = 0;
		xmlconfig.getNodeValue("control/feed/method", nVal);
		if(1 == nVal)
			_nFeedRateMethod = FRM_DELETED_MOLECULES;
		else if(2 == nVal)
			_nFeedRateMethod = FRM_CHANGED_MOLECULES;
		else if(3 == nVal)
			_nFeedRateMethod = FRM_DENSITY;
		else if(4 == nVal)
		{
			_nFeedRateMethod = FRM_CONSTANT;
			xmlconfig.getNodeValue("control/feed/target", _feedrate.feed.target);
		}
	}

	// Zone2 method
	{
		_nZone2Method = FRM_UNKNOWN;
		int nVal = 0;
		xmlconfig.getNodeValue("control/z2method", nVal);
		if(1 == nVal)
			_nZone2Method = Z2M_RESET_ALL;
		else if(2 == nVal)
			_nZone2Method = Z2M_RESET_YPOS_ONLY;
	}

	// range that is free of manipulation from MettDeamon
	{
		double ymin, ymax;
		ymin = ymax = 0.;
		xmlconfig.getNodeValue("control/manipfree/ymin", ymin);
		xmlconfig.getNodeValue("control/manipfree/ymax", ymax);
		_manipfree.ymin = ymin;
		_manipfree.ymax = ymax;
	}

	// reservoir
	{
		string oldpath = xmlconfig.getcurrentnodepath();
		xmlconfig.changecurrentnode("reservoir");
		_reservoir->readXML(xmlconfig);
		xmlconfig.changecurrentnode(oldpath);
	}

	// restart
	_bIsRestart = true;
	_bIsRestart = _bIsRestart && xmlconfig.getNodeValue("restart/binindex", _restartInfo.nBindindex);
	_bIsRestart = _bIsRestart && xmlconfig.getNodeValue("restart/deltaY", _restartInfo.dYsum);

	// mirror
	bool bRet = xmlconfig.getNodeValue("mirror/position", _dMirrorPosY);
	_bMirrorActivated = bRet;

	// change identity of fixed (frozen) molecules by component ID
	if(xmlconfig.changecurrentnode("changes")) {
		uint8_t numChanges = 0;
		XMLfile::Query query = xmlconfig.query("change");
		numChanges = query.card();
		global_log->info() << "Number of fixed molecules components: " << (uint32_t)numChanges << endl;
		if(numChanges < 1) {
			global_log->error() << "No component change defined in XML-config file. Program exit ..." << endl;
			Simulation::exit(-1);
		}
		string oldpath = xmlconfig.getcurrentnodepath();
		XMLfile::Query::const_iterator changeIter;
		for( changeIter = query.begin(); changeIter; changeIter++ ) {
			xmlconfig.changecurrentnode(changeIter);
			uint32_t nFrom, nTo;
			nFrom = nTo = 1;
			xmlconfig.getNodeValue("from", nFrom);
			xmlconfig.getNodeValue("to", nTo);
			_vecChangeCompIDsFreeze.at(nFrom-1) = nTo-1;
			_vecChangeCompIDsUnfreeze.at(nTo-1) = nFrom-1;
		}
		xmlconfig.changecurrentnode(oldpath);
		xmlconfig.changecurrentnode("..");
	}
	else {
		global_log->error() << "No component changes defined in XML-config file. Program exit ..." << endl;
		Simulation::exit(-1);
	}

#ifndef NDEBUG
	cout << "_vecChangeCompIDsFreeze:" << endl;
	for(uint32_t i=0; i<_vecChangeCompIDsFreeze.size(); ++i)
	{
		std::cout << i << ": " << _vecChangeCompIDsFreeze.at(i) << std::endl;
	}
	cout << "_vecChangeCompIDsUnfreeze:" << endl;
	for(uint32_t i=0; i<_vecChangeCompIDsUnfreeze.size(); ++i)
	{
		std::cout << i << ": " << _vecChangeCompIDsUnfreeze.at(i) << std::endl;
	}
#endif

	// molecule diameter
	xmlconfig.getNodeValue("diameter", _dMoleculeDiameter);

	// throttles
	// change identity of fixed (frozen) molecules by component ID
	if(xmlconfig.changecurrentnode("throttles")) {
		uint8_t numThrottles = 0;
		XMLfile::Query query = xmlconfig.query("throttle");
		numThrottles = query.card();
		global_log->info() << "Number of throttles: " << (uint32_t)numThrottles << endl;
		if(numThrottles < 1) {
			global_log->error() << "No throttles defined in XML-config file. Program exit ..." << endl;
			Simulation::exit(-1);
		}
		string oldpath = xmlconfig.getcurrentnodepath();
		XMLfile::Query::const_iterator throttleIter;
		for( throttleIter = query.begin(); throttleIter; throttleIter++ ) {
			xmlconfig.changecurrentnode(throttleIter);
			uint32_t cid;
			xmlconfig.getNodeValue("componentID", cid); cid--;
			xmlconfig.getNodeValue("pos/from", _vecThrottleFromPosY.at(cid));
			xmlconfig.getNodeValue("pos/to",   _vecThrottleToPosY.at(cid));
			xmlconfig.getNodeValue("force",    _vecThrottleForceY.at(cid));
			_vecThrottleForceY.at(cid) = abs(_vecThrottleForceY.at(cid)* -1.);
		}
		xmlconfig.changecurrentnode(oldpath);
		xmlconfig.changecurrentnode("..");
	}
	else {
		global_log->error() << "No throttles defined in XML-config file. Program exit ..." << endl;
		Simulation::exit(-1);
	}
}

void MettDeamon::findMaxMoleculeID(DomainDecompBase* domainDecomp)
{
	ParticleContainer* particleContainer = global_simulation->getMolecules();
	uint64_t nMaxMoleculeID_local = 0;

	// max molecule id in particle container (local)
	for (ParticleIterator pit = particleContainer->iterator(); pit.hasNext(); pit.next())
	{
		uint64_t id = pit->id();
		if(id > nMaxMoleculeID_local)
			nMaxMoleculeID_local = id;
	}

	// global max IDs
#ifdef ENABLE_MPI

	MPI_Allreduce( &nMaxMoleculeID_local, &_nMaxMoleculeID, 1, MPI_UNSIGNED_LONG, MPI_MAX, MPI_COMM_WORLD);

#else
	_nMaxMoleculeID = nMaxMoleculeID_local;
#endif
}

uint64_t MettDeamon::getnNumMoleculesDeleted2( DomainDecompBase* domainDecomposition)
{
	domainDecomposition->collCommInit(1);
		domainDecomposition->collCommAppendUnsLong(_nNumMoleculesTooFast);
		domainDecomposition->collCommAllreduceSum();
		_nNumMoleculesTooFastGlobal = domainDecomposition->collCommGetUnsLong();
		domainDecomposition->collCommFinalize();
//
//		std::cout << "Particles deleted: "<< _nNumMoleculesDeletedGlobalAlltime << std::endl;
//		std::cout << "Of which were too fast: " << _nNumMoleculesTooFastGlobal << std::endl;
		return _nNumMoleculesTooFastGlobal;
}
void MettDeamon::prepare_start(DomainDecompBase* domainDecomp, ParticleContainer* particleContainer, double cutoffRadius)
{
	_feedrate.feed.actual = _feedrate.feed.init;

	_reservoir->readParticleData(domainDecomp);
	_dInvDensityArea = 1. / (_dAreaXZ * _reservoir->getDensity(0) );

	int ownRank = global_simulation->domainDecomposition().getRank();
	cout << "["<<ownRank<<"]: _dInvDensityArea = " << _dInvDensityArea << endl;

	// Activate reservoir bin with respect to restart information
	if(true == _bIsRestart)
		this->initRestart();

	this->InitTransitionPlane(global_simulation->getDomain() );

	// find max molecule ID in particle container
	this->findMaxMoleculeID(domainDecomp);

	double dMoleculeRadius = _dMoleculeDiameter*0.5;

	//ParticleContainer* _moleculeContainer;
	particleContainer->deleteOuterParticles();
	// fixed components

	if(true == _bIsRestart)
		return;

	std::vector<Component>* ptrComps = global_simulation->getEnsemble()->getComponents();

	for (ParticleIterator pit = particleContainer->iterator(); pit.hasNext(); pit.next())
	{
		double dPosY = pit->r(1);
		bool IsBehindTransitionPlane = this->IsBehindTransitionPlane(dPosY);
		if(false == IsBehindTransitionPlane)
		{
			uint32_t cid = pit->componentid();
			if(cid != _vecChangeCompIDsFreeze.at(cid))
			{
				Component* compNew = &(ptrComps->at(_vecChangeCompIDsFreeze.at(cid) ) );
				pit->setComponent(compNew);
//				cout << "cid(new) = " << pit->componentid() << endl;
			}
		}
/*
		else
//		else if(dPosY < (_dTransitionPlanePosY+dMoleculeRadius) )
		{
			particleContainer->deleteMolecule(pit->id(), pit->r(0), pit->r(1),pit->r(2), false);
//			cout << "delete: dY = " << dPosY << endl;
			particleContainer->update();
			pit  = particleContainer->iteratorBegin();
			this->IncrementDeletedMoleculesLocal();
		}

		else if(dPosY < (_dTransitionPlanePosY+_dMoleculeDiameter) )
		{
			pit->setv(1, pit->r(1)-1.);
		}
		*/
	}
	particleContainer->update();
	particleContainer->updateMoleculeCaches();
}
void MettDeamon::init_positionMap(ParticleContainer* particleContainer)
{
	for (ParticleIterator pit = particleContainer->iterator(); pit.hasNext(); pit.next())
	{
		uint64_t mid = pit->id();
		uint32_t cid = pit->componentid();

		bool bIsTrappedMolecule = this->IsTrappedMolecule(cid);
		if(true == bIsTrappedMolecule)
		{
			//savevelo
			std::array<double,10> pos;
			pos.at(0) = pit->r(0);
			pos.at(1) = pit->r(1);
			pos.at(2) = pit->r(2);
			pos.at(3) = pit->v(0);
			pos.at(4) = pit->v(1);
			pos.at(5) = pit->v(2);
			Quaternion q = pit->q();
			pos.at(6) = q.qw();
			pos.at(7) = q.qx();
			pos.at(8) = q.qy();
			pos.at(9) = q.qz();
//			_storePosition.insert ( std::pair<unsigned long, std::array<double, 3> >(mid, pos) );
			_storePosition[pit->id()] = pos;
		}
	}
}

bool MettDeamon::IsInsideOuterReservoirSlab(const double& dPosY, const double& dBoxY)
{
	bool bRet = false;
	if(MD_LEFT_TO_RIGHT == _nMovingDirection)
		bRet = (dPosY < _reservoir->getBinWidth() );
	else if(MD_RIGHT_TO_LEFT == _nMovingDirection)
		bRet = (dPosY > (dBoxY - _reservoir->getBinWidth() ) );
	return bRet;
}

void MettDeamon::releaseTrappedMolecule(Molecule* mol)
{
	uint16_t cid_zb = mol->componentid();
	if(this->IsTrappedMolecule(cid_zb) == false || this->IsBehindTransitionPlane(mol->r(1) ) == false)
		return;

	std::vector<Component>* ptrComps = global_simulation->getEnsemble()->getComponents();
	Component* compNew = &(ptrComps->at(_vecChangeCompIDsUnfreeze.at(cid_zb) ) );
	mol->setComponent(compNew);

	mol->setv(0, 0.0);
	if(MD_LEFT_TO_RIGHT == _nMovingDirection)
		mol->setv(1, 3*_feedrate.feed.actual);
	else if(MD_RIGHT_TO_LEFT == _nMovingDirection)
		mol->setv(1, -3*_feedrate.feed.actual);
	mol->setv(2, 0.0);
}

void MettDeamon::resetPositionAndOrientation(Molecule* mol, const double& dBoxY)
{
	uint16_t cid_zb = mol->componentid();
	if(false == this->IsTrappedMolecule(cid_zb) )
		return;

	std::map<unsigned long, std::array<double,10> >::iterator it;
	it = _storePosition.find(mol->id() );
	if(it == _storePosition.end() )
		return;

	// x,z position: always reset
	mol->setr(0, it->second.at(0) );
	mol->setr(2, it->second.at(2) );

	// reset y-position
	if(MD_LEFT_TO_RIGHT == _nMovingDirection)
		mol->setr(1, it->second.at(1) + _feedrate.feed.actual);
	else if(MD_RIGHT_TO_LEFT == _nMovingDirection)
		mol->setr(1, it->second.at(1) - _feedrate.feed.actual);

//	if(this->IsInsideOuterReservoirSlab(mol->r(1), dBoxY) == false)
//		return;

	// reset quaternion (orientation)
	Quaternion q(it->second.at(6),
			it->second.at(7),
			it->second.at(8),
			it->second.at(9) );
	mol->setq(q);
}

void MettDeamon::preForce_action(ParticleContainer* particleContainer, double cutoffRadius)
{
	double dBoxY = global_simulation->getDomain()->getGlobalLength(1);
	double dMoleculeRadius = 0.5;

	std::vector<Component>* ptrComps = global_simulation->getEnsemble()->getComponents();

	Random rnd;
	double T = 80;
	double v[3];

	particleContainer->updateMoleculeCaches();

	for (ParticleIterator pit = particleContainer->iterator(); pit.hasNext(); pit.next())
	{
		uint8_t cid = pit->componentid();
		double dY = pit->r(1);

		if(dY > _manipfree.ymin && dY < _manipfree.ymax)
			continue;

		bool bIsTrappedMolecule = this->IsTrappedMolecule(cid);
		bool IsBehindTransitionPlane = this->IsBehindTransitionPlane(dY);
/*
		double m = pit->mass();
		double vym = sqrt(T/m);
*/
		double m = pit->mass();
		double vm2 = 3*T/m;

		v[0] = rnd.rnd();
		v[1] = rnd.rnd();
		v[2] = rnd.rnd();
		double v2_set = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
//		global_log->info() << "rnd: vx=" << v[0] << ", vy=" << v[1] << ", vz=" << v[2] << ", v2=" << v2 << endl;
		double f = sqrt(vm2/v2_set);

		for(uint8_t dim=0; dim<3; ++dim)
			v[dim] *= f;
		v2_set = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
//		global_log->info() << "scaled: vx=" << v[0] << ", vy=" << v[1] << ", vz=" << v[2] << ", v2=" << v2 << endl;

		// release trapped molecule
		this->releaseTrappedMolecule( &(*pit) );

		// reset position and orientation of fixed molecules
		this->resetPositionAndOrientation( &(*pit), dBoxY);

		if(true == bIsTrappedMolecule)
		{
			// limit velocity of trapped molecules
			pit->setv(0, 0.);
			pit->setv(1, 0.);
			pit->setv(2, 0.);
			pit->setD(0, 0.);
			pit->setD(1, 0.);
			pit->setD(2, 0.);

			vm2 = T/m*4/9.;
			double v2 = pit->v2();

			if(v2 > vm2)
			{
				double fac = sqrt(vm2/v2);
				pit->scale_v(fac);
			}
		}
		else
		{
			double v2 = pit->v2();
			double v2max = _vecVeloctiyBarriers.at(cid+1)*_vecVeloctiyBarriers.at(cid+1);

			if(v2 > v2max)
			{
				uint64_t id = pit->id();

//				cout << "Velocity barrier for cid+1=" << cid+1 << ": " << _vecVeloctiyBarriers.at(cid+1) << endl;
//				cout << "id=" << id << ", dY=" << dY << ", v=" << sqrt(pit->v2() ) << endl;

	//			particleContainer->deleteMolecule(pit->id(), pit->r(0), pit->r(1),pit->r(2), true);
	//			_nNumMoleculesDeletedLocal++;
	//			_nNumMoleculesTooFast++;

				double fac = sqrt(v2max/v2);
				pit->scale_v(fac);
			}
		}

		// mirror molecules back that are on the way to pass fixed molecule region
		if(dY <= _reservoir->getBinWidth() )
			pit->setv(1, abs(pit->v(1) ) );

	}  // loop over molecules

	// DEBUG
	if(FRM_CONSTANT == _nFeedRateMethod)
		_feedrate.feed.actual = _feedrate.feed.target;
	// DEBUG

	_feedrate.feed.sum += _feedrate.feed.actual;
	int ownRank = global_simulation->domainDecomposition().getRank();
//	cout << "["<<ownRank<<"]: _feedrate.feed.actual="<<_feedrate.feed.actual<<", _feedrate.feed.sum="<<_feedrate.feed.sum<<endl;

	if (_feedrate.feed.sum >= _reservoir->getBinWidth())
	{
		global_log->info() << "Mett-" << (uint32_t)_nMovingDirection << ": _feedrate.feed.sum=" << _feedrate.feed.sum << ", _dSlabWidth=" << _reservoir->getBinWidth() << endl;
		global_log->info() << "_dSlabWidth=" << _reservoir->getBinWidth() << endl;
		global_log->info() << "_feedrate.feed.sum=" << _feedrate.feed.sum << endl;
		global_log->info() << "_reservoir->getActualBinIndex()=" << _reservoir->getActualBinIndex() << endl;

		// insert actual reservoir slab / activate next reservoir slab
		this->InsertReservoirSlab(particleContainer);
	}
	particleContainer->update();
	particleContainer->updateMoleculeCaches();
}
void MettDeamon::postForce_action(ParticleContainer* particleContainer, DomainDecompBase* domainDecomposition)
{
	unsigned long nNumMoleculesLocal = 0;
	unsigned long nNumMoleculesGlobal = 0;

	for (ParticleIterator pit = particleContainer->iterator(); pit.hasNext(); pit.next())
	{
		double dY = pit->r(1);
		if(dY > _manipfree.ymin && dY < _manipfree.ymax)
			continue;

		uint8_t cid = pit->componentid();
		bool bIsTrappedMolecule = this->IsTrappedMolecule(cid);

		if(true == bIsTrappedMolecule)
		{
			// limit velocity of trapped molecules
			pit->setv(0, 0.);
			pit->setv(1, 0.);
			pit->setv(2, 0.);
			pit->setD(0, 0.);
			pit->setD(1, 0.);
			pit->setD(2, 0.);

			double T = 80.;
			double m = pit->mass();
			double vm2 = T/m*4/9.;
			double v2 = pit->v2();

			if(v2 > vm2)
			{
				double f = sqrt(vm2/v2);
				pit->scale_v(f);
			}
		}
		else
		{
			double v2 = pit->v2();
			double v2max = _vecVeloctiyBarriers.at(cid+1)*_vecVeloctiyBarriers.at(cid+1);

			if(v2 > v2max)
			{
				uint64_t id = pit->id();

//				cout << "Velocity barrier for cid+1=" << cid+1 << ": " << _vecVeloctiyBarriers.at(cid+1) << endl;
//				cout << "id=" << id << ", dY=" << dY << ", v=" << sqrt(pit->v2() ) << endl;

				double f = sqrt(v2max/v2);
				pit->scale_v(f);
			}
		}
		/*
		else if(v2 > _velocityBarrier*_velocityBarrier) // v2_limit)
		{
			uint64_t id = pit->id();
			double dY = pit->r(1);

			cout << "cid+1=" << cid+1 << endl;
			cout << "id=" << id << endl;
			cout << "dY=" << dY << endl;
			cout << "v2=" << v2 << endl;

			particleContainer->deleteMolecule(pit->id(), pit->r(0), pit->r(1),pit->r(2), true);
			_nNumMoleculesDeletedLocal++;
			_nNumMoleculesTooFast++;
			continue;
			this->IncrementDeletedMoleculesLocal();
		}
		*/
/*
 * limit force, velocities => repair dynamic
 *
		if(false == bIsFrozenMolecule &&
				dY > (_dTransitionPlanePosY + _vecThrottleFromPosY.at(cid) ) &&
				dY < (_dTransitionPlanePosY +   _vecThrottleToPosY.at(cid) ) )
		{
			double m = pit->mass();
//			global_log->info() << "m=" << m << endl;
			double vm = sqrt(T/m);
//			global_log->info() << "vym" << vym << endl;
			double v[3];
			for(uint8_t dim=0; dim<3; ++dim)
			{
				v[dim] = pit->v(dim);

				if(abs(v[dim]) > vm)
				{
					if(v[dim]>0.)
						v[dim] += abs(v[dim])/_vecThrottleForceY.at(cid)*(-1.);
					else
						v[dim] += abs(v[dim])/_vecThrottleForceY.at(cid);

					if(abs(v[dim]) > 5*vm)
					{
						if(v[dim]>0.)
							v[dim] = vm;
						else
							v[dim] = vm*-1;
					}
					pit->setv(dim,v[dim]);
				}
			}
//			global_log->info() << "_dTransitionPlanePosY=" << _dTransitionPlanePosY << endl;
//			global_log->info() << "dY=" << dY << endl;
		}
*/
		// mirror, to simulate VLE
		if(true == _bMirrorActivated)
		{
			if(pit->r(1) >= _dMirrorPosY)
				pit->setv(1, -1.*abs(pit->v(1) ) );
		}

	}  // loop over molecules

//	particleContainer->update();
//	particleContainer->updateMoleculeCaches();
	nNumMoleculesLocal = particleContainer->getNumberOfParticles();

	// delta y berechnen: alle x Zeitschritte
	if(global_simulation->getSimulationStep() % _nUpdateFreq == 0)
	{
		// update global number of particles / calc global number of deleted particles
		domainDecomposition->collCommInit(5);
		domainDecomposition->collCommAppendUnsLong(nNumMoleculesLocal);
		domainDecomposition->collCommAppendUnsLong(_feedrate.numMolecules.inserted.local);
		domainDecomposition->collCommAppendUnsLong(_feedrate.numMolecules.deleted.local);
		domainDecomposition->collCommAppendUnsLong(_feedrate.numMolecules.changed_to.local);
		domainDecomposition->collCommAppendUnsLong(_feedrate.numMolecules.changed_from.local);
		domainDecomposition->collCommAllreduceSum();
		nNumMoleculesGlobal = domainDecomposition->collCommGetUnsLong();
		_feedrate.numMolecules.inserted.global = domainDecomposition->collCommGetUnsLong();
		_feedrate.numMolecules.deleted.global = domainDecomposition->collCommGetUnsLong();
		_feedrate.numMolecules.changed_to.global = domainDecomposition->collCommGetUnsLong();
		_feedrate.numMolecules.changed_from.global = domainDecomposition->collCommGetUnsLong();
		domainDecomposition->collCommFinalize();
		_nNumMoleculesDeletedGlobalAlltime += _feedrate.numMolecules.deleted.global;
		_feedrate.numMolecules.inserted.local = 0;
		_feedrate.numMolecules.deleted.local = 0;
		_feedrate.numMolecules.changed_to.local = 0;
		_feedrate.numMolecules.changed_from.local = 0;

		// update sum and summation list
		int64_t numMolsDeletedOrChanged = 0;
		if(FRM_DELETED_MOLECULES == _nFeedRateMethod)
		{
			numMolsDeletedOrChanged += _feedrate.numMolecules.deleted.global;
			numMolsDeletedOrChanged -= _feedrate.numMolecules.inserted.global;
			numMolsDeletedOrChanged += _feedrate.numMolecules.changed_from.global;
			numMolsDeletedOrChanged -= _feedrate.numMolecules.changed_to.global;
		}
		else if(FRM_CHANGED_MOLECULES == _nFeedRateMethod)
		{
			numMolsDeletedOrChanged += _feedrate.numMolecules.changed_from.global;
			numMolsDeletedOrChanged -= _feedrate.numMolecules.changed_to.global;
		}
		_numDeletedMolsSum += numMolsDeletedOrChanged;
		_numDeletedMolsSum -= _listDeletedMolecules.front();
		_listDeletedMolecules.push_back(numMolsDeletedOrChanged);
		if(_listDeletedMolecules.size() > _nNumValsSummation)
			_listDeletedMolecules.pop_front();
		else
		{
			_numDeletedMolsSum = 0;
			for(auto&& vi : _listDeletedMolecules)
				_numDeletedMolsSum += vi;
		}
		_dDeletedMolsPerTimestep = _numDeletedMolsSum * _dInvNumTimestepsSummation;
		if(FRM_DELETED_MOLECULES == _nFeedRateMethod || FRM_CHANGED_MOLECULES == _nFeedRateMethod)
			this->calcDeltaY();
		else if(FRM_DENSITY == _nFeedRateMethod)
			this->calcDeltaYbyDensity();
		int ownRank = global_simulation->domainDecomposition().getRank();
//		cout << "["<<ownRank<<"]: _numDeletedMolsSum = " << _numDeletedMolsSum << endl;
//		cout << "["<<ownRank<<"]: _dDeletedMolsPerTimestep = " << _dDeletedMolsPerTimestep << endl;
//		cout << "["<<ownRank<<"]: _feedrate.feed.actual = " << _feedrate.feed.actual << endl;
	}
	else
	{
		// update global number of particles
		domainDecomposition->collCommInit(1);
		domainDecomposition->collCommAppendUnsLong(nNumMoleculesLocal);
		domainDecomposition->collCommAllreduceSum();
		nNumMoleculesGlobal = domainDecomposition->collCommGetUnsLong();
		domainDecomposition->collCommFinalize();
	}
	global_simulation->getDomain()->setglobalNumMolecules(nNumMoleculesGlobal);

	// write restart file
	this->writeRestartfile();
}

void MettDeamon::writeRestartfile()
{
	if(0 != global_simulation->getSimulationStep() % _nWriteFreqRestart)
		return;

	DomainDecompBase& domainDecomp = global_simulation->domainDecomposition();

	if(domainDecomp.getRank() != 0)
		return;

	std::ofstream ofs("MettDeamonRestart.dat", std::ios::app);
	std::stringstream outputstream;

	outputstream << setw(12) << global_simulation->getSimulationStep() << setw(12) << _reservoir->getActualBinIndex();
	outputstream << FORMAT_SCI_MAX_DIGITS << _feedrate.feed.sum << std::endl;

	ofs << outputstream.str();
	ofs.close();
}

void MettDeamon::calcDeltaY()
{
	_feedrate.feed.actual = _dDeletedMolsPerTimestep * _dInvDensityArea;
	if(MD_LEFT_TO_RIGHT == _nMovingDirection && _feedrate.feed.actual < 0.)
		_feedrate.feed.actual = 0.;
	else if (MD_RIGHT_TO_LEFT == _nMovingDirection && _feedrate.feed.actual > 0.)
		_feedrate.feed.actual = 0.;
}

void MettDeamon::calcDeltaYbyDensity()
{
	double dDensityMean = 0.;
	uint32_t numVals = 0;
	for(auto&& dVal : _vecDensityValues)
	{
		dDensityMean += dVal;
		numVals++;
	}
	double dInvNumVals = 1./((double)(numVals));
	dDensityMean *= dInvNumVals;
	double dDensityDelta = _dDensityTarget - dDensityMean;
	if(dDensityDelta <= 0.)
		_feedrate.feed.actual = 0.;
	else
		_feedrate.feed.actual = dDensityDelta/_reservoir->getDensity(0)*dInvNumVals*_dVolumeCV/_dAreaXZ;
}

void MettDeamon::InitTransitionPlane(Domain* domain)
{
	double dBoxY = domain->getGlobalLength(1);
	if(MD_LEFT_TO_RIGHT == _nMovingDirection)
		_dTransitionPlanePosY = 2*_reservoir->getBinWidth();
	else
		_dTransitionPlanePosY = dBoxY - 2*_reservoir->getBinWidth();
}

void MettDeamon::InsertReservoirSlab(ParticleContainer* particleContainer)
{
	DomainDecompBase& domainDecomp = global_simulation->domainDecomposition();
	std::vector<Component>* ptrComps = global_simulation->getEnsemble()->getComponents();
	std::vector<Molecule>& currentReservoirSlab = _reservoir->getParticlesActualBin();
#ifndef NDEBUG
	cout << "[" << domainDecomp.getRank() << "]: currentReservoirSlab.size()=" << currentReservoirSlab.size() << endl;
#endif

	for(auto mi : currentReservoirSlab)
	{
		uint64_t id = mi.id();
		uint32_t cid = mi.componentid();
		Component* compNew;
		if(_nMovingDirection==1)
			compNew = &(ptrComps->at(_vecChangeCompIDsFreeze.at(cid) ) );
		else
			compNew = &(ptrComps->at(3));

		mi.setid(_nMaxMoleculeID + id);
		mi.setComponent(compNew);
		mi.setr(1, mi.r(1) + _feedrate.feed.sum - _reservoir->getBinWidth() );
		particleContainer->addParticle(mi);
	}
	_feedrate.feed.sum -= _reservoir->getBinWidth();  // reset feed sum
	_reservoir->nextBin(_nMaxMoleculeID);
}

void MettDeamon::initRestart()
{
	bool bRet = _reservoir->activateBin(_restartInfo.nBindindex);
	if(false == bRet)
	{
		global_log->info() << "Failed to activate reservoir bin after restart! Program exit ... " << endl;
		Simulation::exit(-1);
	}
	_feedrate.feed.sum = _restartInfo.dYsum;
}



// class Reservoir
Reservoir::Reservoir(MettDeamon* parent) :
	_parent(parent),
	_moleculeDataReader(nullptr),
	_binQueue(nullptr),
	_numMoleculesRead(0),
	_nMaxMoleculeID(0),
	_nMoleculeFormat(ICRVQD),
	_nReadMethod(RRM_UNKNOWN),
	_dReadWidthY(0.0),
	_dBinWidthInit(0.0),
	_dBinWidth(0.0)
{
	// init filepath struct
	_filepath.data = _filepath.header = "unknown";

	// allocate BinQueue
	_binQueue = new BinQueue();

	// init identity change vector
	uint16_t nNumComponents = global_simulation->getEnsemble()->getComponents()->size();
	_vecChangeCompIDs.resize(nNumComponents);
	std::iota (std::begin(_vecChangeCompIDs), std::end(_vecChangeCompIDs), 0);

	// init density vector
	_density.resize(nNumComponents+1);  // 0: total density
}

void Reservoir::readXML(XMLfileUnits& xmlconfig)
{
	std::string strType = "unknown";
	bool bRet1 = xmlconfig.getNodeValue("file@type", strType);
	bool bRet2 = xmlconfig.getNodeValue("width", _dReadWidthY);
	xmlconfig.getNodeValue("binwidth", _dBinWidthInit);
	_nReadMethod = RRM_UNKNOWN;
	if(true == bRet1 && false == bRet2)
	{
		if("ASCII" == strType) {
			_nReadMethod = RRM_READ_FROM_FILE;
			xmlconfig.getNodeValue("file", _filepath.data);
			_filepath.header = _filepath.data;
		}
		else if("binary" == strType) {
			_nReadMethod = RRM_READ_FROM_FILE_BINARY;
			xmlconfig.getNodeValue("file/header", _filepath.header);
			xmlconfig.getNodeValue("file/data", _filepath.data);
		}
		else {
			global_log->error() << "Wrong file type='" << strType << "' specified. Programm exit ..." << endl;
			Simulation::exit(-1);
		}
	}
	else if(false == bRet1 && true == bRet2)
		_nReadMethod = RRM_READ_FROM_MEMORY;
	else
		_nReadMethod = RRM_AMBIGUOUS;

	// Possibly change component IDs
	if(xmlconfig.changecurrentnode("changes")) {
		uint8_t numChanges = 0;
		XMLfile::Query query = xmlconfig.query("change");
		numChanges = query.card();
		if(numChanges < 1) {
			global_log->error() << "No component change defined in XML-config file. Program exit ..." << endl;
			Simulation::exit(-1);
		}
		string oldpath = xmlconfig.getcurrentnodepath();
		XMLfile::Query::const_iterator changeIter;
		for( changeIter = query.begin(); changeIter; changeIter++ ) {
			xmlconfig.changecurrentnode(changeIter);
			uint32_t nFrom, nTo;
			nFrom = nTo = 1;
			xmlconfig.getNodeValue("from", nFrom);
			xmlconfig.getNodeValue("to", nTo);
			_vecChangeCompIDs.at(nFrom-1) = nTo-1;
		}
		xmlconfig.changecurrentnode(oldpath);
		xmlconfig.changecurrentnode("..");
	}
}

void Reservoir::readParticleData(DomainDecompBase* domainDecomp)
{
	switch(_nReadMethod)
	{
	case RRM_READ_FROM_MEMORY:
		this->readFromMemory(domainDecomp);
		break;
	case RRM_READ_FROM_FILE:
		this->readFromFile(domainDecomp);
		break;
	case RRM_READ_FROM_FILE_BINARY:
		this->readFromFileBinary(domainDecomp);
		break;
	case RRM_UNKNOWN:
	case RRM_AMBIGUOUS:
	default:
		global_log->error() << "Unknown (or ambiguous) method to read reservoir for feature MettDeamon. Program exit ..." << endl;
		Simulation::exit(-1);
	}

	// sort particles into bins
	this->sortParticlesToBins();

	// volume, densities
	this->calcPartialDensities(domainDecomp);
//	mardyn_assert( (_numMoleculesGlobal == _numMoleculesRead) || (RRM_READ_FROM_MEMORY == _nReadMethod) );
#ifndef NDEBUG
	cout << "Volume of Mettdeamon Reservoir: " << _box.volume << endl;
	cout << "Density of Mettdeamon Reservoir: " << _density.at(0).density << endl;
#endif
}

void Reservoir::sortParticlesToBins()
{
	Domain* domain = global_simulation->getDomain();
	DomainDecompBase* domainDecomp = &global_simulation->domainDecomposition();

	uint32_t numBins = _box.length.at(1) / _dBinWidthInit;
	_dBinWidth = _box.length.at(1) / (double)(numBins);
#ifndef NDEBUG
	cout << domainDecomp->getRank() << ": _arrBoxLength[1]="<<_box.length.at(1)<<endl;
	cout << domainDecomp->getRank() << ": _dBinWidthInit="<<_dBinWidthInit<<endl;
	cout << domainDecomp->getRank() << ": _numBins="<<numBins<<endl;
	cout << domainDecomp->getRank() << ": _particleVector.size()=" << _particleVector.size() << endl;
#endif
	std::vector< std::vector<Molecule> > binVector;
	binVector.resize(numBins);
	uint32_t nBinIndex;
	for(auto&& mol:_particleVector)
	{
		// possibly change component IDs
		this->changeComponentID(mol, mol.componentid() );
		double y = mol.r(1);
		nBinIndex = floor(y / _dBinWidth);
//		cout << domainDecomp->getRank() << ": y="<<y<<", nBinIndex="<<nBinIndex<<", _binVector.size()="<<binVector.size()<<endl;
		mardyn_assert(nBinIndex < binVector.size() );
		mol.setr(1, y - nBinIndex*_dBinWidth);  // positions in slabs related to origin (x,y,z) == (0,0,0)
		switch(_parent->getMovingDirection() )
		{
		case MD_LEFT_TO_RIGHT:
			mol.setr(1, y - nBinIndex*_dBinWidth);  // positions in slabs related to origin (x,y,z) == (0,0,0)
			break;
		case MD_RIGHT_TO_LEFT:
			mol.setr(1, y - nBinIndex*_dBinWidth + (domain->getGlobalLength(1) - _dBinWidth) );  // positions in slabs related to origin (x,y,z) == (0,0,0)
			break;
		}
		// check if molecule is in bounding box of the process domain
		if (true == domainDecomp->procOwnsPos(mol.r(0), mol.r(1), mol.r(2), domain) )
			binVector.at(nBinIndex).push_back(mol);
	}

	// add bin particle vectors to bin queue
	switch(_parent->getMovingDirection() )
	{
		case MD_LEFT_TO_RIGHT:
			for (auto bit = binVector.rbegin(); bit != binVector.rend(); ++bit)
			{
#ifndef NDEBUG
				cout << domainDecomp->getRank() << ": (*bit).size()=" << (*bit).size() << endl;
#endif
				_binQueue->enque(*bit);
			}
			break;

		case MD_RIGHT_TO_LEFT:
			for(auto bin:binVector)
			{
#ifndef NDEBUG
				cout << domainDecomp->getRank() << ": bin.size()=" << bin.size() << endl;
#endif
				_binQueue->enque(bin);
			}
			break;
	}
	_binQueue->connectTailToHead();
}

void Reservoir::readFromMemory(DomainDecompBase* domainDecomp)
{
	ParticleContainer* particleContainer = global_simulation->getMolecules();
	Domain* domain = global_simulation->getDomain();

	_box.length.at(0) = domain->getGlobalLength(0);
	_box.length.at(1) = _dReadWidthY;
	_box.length.at(2) = domain->getGlobalLength(2);

	for (ParticleIterator pit = particleContainer->iterator(); pit.hasNext(); pit.next())
	{
//		if(true == this->IsBehindTransitionPlane(y) )
//			continue;

		Molecule mol(*pit);
		double y = mol.r(1);

		switch(_parent->getMovingDirection() )
		{
		case MD_LEFT_TO_RIGHT:
			if(y > _dReadWidthY) continue;
			break;
		case MD_RIGHT_TO_LEFT:
			if(y < (domain->getGlobalLength(1) - _dReadWidthY) ) continue;
			double relPosY = y - (domain->getGlobalLength(1) - _dReadWidthY);
			mol.setr(1, relPosY);  // move to origin x,y,z = 0,0,0
			break;
		}
		_particleVector.push_back(mol);
	}
}

void Reservoir::readFromFile(DomainDecompBase* domainDecomp)
{
	Domain* domain = global_simulation->getDomain();
	std::ifstream ifs;
	global_log->info() << "Opening Mettdeamon Reservoirfile " << _filepath.data << endl;
	ifs.open( _filepath.data.c_str() );
	if (!ifs.is_open()) {
		global_log->error() << "Could not open Mettdeamon Reservoirfile " << _filepath.data << endl;
		Simulation::exit(1);
	}
	global_log->info() << "Reading Mettdeamon Reservoirfile " << _filepath.data << endl;

	string token;
	vector<Component>& dcomponents = *(_simulation.getEnsemble()->getComponents());
	unsigned int numcomponents = dcomponents.size();
	string ntypestring("ICRVQD");
	enum Ndatatype { ICRVQDV, ICRVQD, IRV, ICRV } ntype = ICRVQD;

	double Xlength, Ylength, Zlength;
	while(ifs && (token != "NumberOfMolecules") && (token != "N"))
	{
		ifs >> token;

		if(token=="Length" || token=="L")
		{
			ifs >> Xlength >> Ylength >> Zlength;
			_box.length.at(0) = domain->getGlobalLength(0);
			_box.length.at(1) = Ylength;
			_box.length.at(2) = domain->getGlobalLength(2);
		}
	}

	if((token != "NumberOfMolecules") && (token != "N")) {
		global_log->error() << "Expected the token 'NumberOfMolecules (N)' instead of '" << token << "'" << endl;
		Simulation::exit(1);
	}
	ifs >> _numMoleculesRead;

	streampos spos = ifs.tellg();
	ifs >> token;
	if((token=="MoleculeFormat") || (token == "M"))
	{
		ifs >> ntypestring;
		ntypestring.erase( ntypestring.find_last_not_of( " \t\n") + 1 );
		ntypestring.erase( 0, ntypestring.find_first_not_of( " \t\n" ) );

		if (ntypestring == "ICRVQDV") ntype = ICRVQDV;
		else if (ntypestring == "ICRVQD") ntype = ICRVQD;
		else if (ntypestring == "ICRV") ntype = ICRV;
		else if (ntypestring == "IRV")  ntype = IRV;
		else {
			global_log->error() << "Unknown molecule format '" << ntypestring << "'" << endl;
			Simulation::exit(1);
		}
	} else {
		ifs.seekg(spos);
	}
	global_log->info() << " molecule format: " << ntypestring << endl;

	if( numcomponents < 1 ) {
		global_log->warning() << "No components defined! Setting up single one-centered LJ" << endl;
		numcomponents = 1;
		dcomponents.resize( numcomponents );
		dcomponents[0].setID(0);
		dcomponents[0].addLJcenter(0., 0., 0., 1., 1., 1., 6., false);
	}
	double x, y, z, vx, vy, vz, q0, q1, q2, q3, Dx, Dy, Dz, Vix, Viy, Viz;
	unsigned long id;
	unsigned int componentid;

	x=y=z=vx=vy=vz=q1=q2=q3=Dx=Dy=Dz=Vix=Viy=Viz=0.;
	q0=1.;

	for( unsigned long i=0; i<_numMoleculesRead; i++ )
	{
		switch ( ntype ) {
			case ICRVQDV:
				ifs >> id >> componentid >> x >> y >> z >> vx >> vy >> vz
					>> q0 >> q1 >> q2 >> q3 >> Dx >> Dy >> Dz >> Vix >> Viy >> Viz;
				break;
			case ICRVQD:
				ifs >> id >> componentid >> x >> y >> z >> vx >> vy >> vz
					>> q0 >> q1 >> q2 >> q3 >> Dx >> Dy >> Dz;
				break;
			case ICRV :
				ifs >> id >> componentid >> x >> y >> z >> vx >> vy >> vz;
				break;
			case IRV :
				ifs >> id >> x >> y >> z >> vx >> vy >> vz;
				break;
		}

		if( componentid > numcomponents ) {
			global_log->error() << "Molecule id " << id << " has wrong componentid: " << componentid << ">" << numcomponents << endl;
			Simulation::exit(1);
		}
		componentid --; // TODO: Component IDs start with 0 in the program.
		Molecule mol = Molecule(i+1,&dcomponents[componentid],x,y,z,vx,vy,vz,q0,q1,q2,q3,Dx,Dy,Dz);
/*
		uint32_t nSlabindex = floor(y / _dBinWidth);
		m1.setr(1, y - nSlabindex*_dBinWidth);  // positions in slabs related to origin (x,y,z) == (0,0,0)

		double bbMin[3];
		double bbMax[3];
		bool bIsInsideSubdomain = false;
		domainDecomp->getBoundingBoxMinMax(global_simulation->getDomain(), bbMin, bbMax);
		bIsInsideSubdomain = x > bbMin[0] && x < bbMax[0] && y > bbMin[1] && y < bbMax[1] && z > bbMin[2] && z < bbMax[2];

		if(true == bIsInsideSubdomain)
			_binVector.at(nSlabindex).push_back(m1);

		componentid = m1.componentid();
		// TODO: The following should be done by the addPartice method.
		dcomponents.at(componentid).incNumMolecules();
*/
		_particleVector.push_back(mol);

		// Print status message
		unsigned long iph = _numMoleculesRead / 100;
		if( iph != 0 && (i % iph) == 0 )
			global_log->info() << "Finished reading molecules: " << i/iph << "%\r" << flush;
	}

	ifs.close();
}

void Reservoir::readFromFileBinaryHeader()
{
	DomainDecompBase* domainDecomp = &global_simulation->domainDecomposition();
	XMLfileUnits inp(_filepath.header);

	if(false == inp.changecurrentnode("/mardyn")) {
		global_log->error() << "Could not find root node /mardyn in XML input file." << endl;
		global_log->fatal() << "Not a valid MarDyn XML input file." << endl;
		Simulation::exit(1);
	}

	bool bInputOk = true;
	double dCurrentTime = 0.;
	double dBL[3];
	uint64_t nNumMols;
	std::string strMoleculeFormat;
	bInputOk = bInputOk && inp.changecurrentnode("headerinfo");
	bInputOk = bInputOk && inp.getNodeValue("time", dCurrentTime);
	bInputOk = bInputOk && inp.getNodeValue("length/x", dBL[0] );
	bInputOk = bInputOk && inp.getNodeValue("length/y", dBL[1] );
	bInputOk = bInputOk && inp.getNodeValue("length/z", dBL[2] );
	bInputOk = bInputOk && inp.getNodeValue("number", nNumMols);
	bInputOk = bInputOk && inp.getNodeValue("format@type", strMoleculeFormat);
	double dVolume = 1;
	for(uint8_t di=0; di<3; ++di)
	{
		this->setBoxLength(di, dBL[di] );
		dVolume *= dBL[di];
	}
	_numMoleculesRead = nNumMols;
	this->setVolume(dVolume);
	this->setDensity(0, nNumMols / dVolume);

	if(false == bInputOk)
	{
		global_log->error() << "Content of file: '" << _filepath.header << "' corrupted! Program exit ..." << endl;
		Simulation::exit(1);
	}

	if("ICRVQD" == strMoleculeFormat)
		_nMoleculeFormat = ICRVQD;
	else if("IRV" == strMoleculeFormat)
		_nMoleculeFormat = IRV;
	else if("ICRV" == strMoleculeFormat)
		_nMoleculeFormat = ICRV;
	else
	{
		global_log->error() << "Not a valid molecule format: " << strMoleculeFormat << ", program exit ..." << endl;
		Simulation::exit(1);
	}
}

void Reservoir::readFromFileBinary(DomainDecompBase* domainDecomp)
{
	global_log->info() << "Reservoir::readFromFileBinary(...)" << endl;
	// read header
	this->readFromFileBinaryHeader();

#ifdef ENABLE_MPI
	if(domainDecomp->getRank() == 0) {
#endif
	global_log->info() << "Opening phase space file " << _filepath.data << endl;
	std::ifstream ifs;
	ifs.open(_filepath.data.c_str(), ios::binary | ios::in);
	if (!ifs.is_open()) {
		global_log->error() << "Could not open phaseSpaceFile " << _filepath.data << endl;
		Simulation::exit(1);
	}

	global_log->info() << "Reading phase space file " << _filepath.data << endl;

	vector<Component>& components = *(_simulation.getEnsemble()->getComponents());

	// Select appropriate reader
	switch (_nMoleculeFormat) {
		case ICRVQD: _moleculeDataReader = new MoleculeDataReaderICRVQD(); break;
		case ICRV: _moleculeDataReader = new MoleculeDataReaderICRV(); break;
		case IRV: _moleculeDataReader = new MoleculeDataReaderIRV(); break;
	}

	for (uint64_t pi=0; pi<_numMoleculesRead; pi++) {
		Molecule mol;
		_moleculeDataReader->read(ifs, mol, components);
		_particleVector.push_back(mol);
	}
#ifdef ENABLE_MPI
	}
#endif

	/* distribute molecules to other MPI processes */
#ifdef ENABLE_MPI
	unsigned long num_particles = _particleVector.size();
	MPI_CHECK( MPI_Bcast(&num_particles, 1, MPI_UNSIGNED_LONG, 0, domainDecomp->getCommunicator()) );

#define PARTICLE_BUFFER_SIZE  (16*1024)
	ParticleData particle_buff[PARTICLE_BUFFER_SIZE];
	int particle_buff_pos = 0;
	MPI_Datatype mpi_Particle;
	ParticleData::getMPIType(mpi_Particle);

	if(domainDecomp->getRank() == 0) {
		for(unsigned long i = 0; i < num_particles; ++i) {
			ParticleData::MoleculeToParticleData(particle_buff[particle_buff_pos], _particleVector[i]);
			particle_buff_pos++;
			if ((particle_buff_pos >= PARTICLE_BUFFER_SIZE) || (i == num_particles - 1)) {
				global_log->debug() << "broadcasting(sending) particles" << endl;
				MPI_Bcast(particle_buff, PARTICLE_BUFFER_SIZE, mpi_Particle, 0, domainDecomp->getCommunicator());
				particle_buff_pos = 0;
			}
		}
	} else {
		for(unsigned long i = 0; i < num_particles; ++i) {
			if(i % PARTICLE_BUFFER_SIZE == 0) {
				global_log->debug() << "broadcasting(receiving) particles" << endl;
				MPI_Bcast(particle_buff, PARTICLE_BUFFER_SIZE, mpi_Particle, 0, domainDecomp->getCommunicator());
				particle_buff_pos = 0;
			}
			Molecule mol;
			ParticleData::ParticleDataToMolecule(particle_buff[particle_buff_pos], mol);
			particle_buff_pos++;
			_particleVector.push_back(mol);
		}
	}
	global_log->debug() << "broadcasting(sending/receiving) particles complete" << endl;
#endif
}

void Reservoir::calcPartialDensities(DomainDecompBase* domainDecomp)
{
	// calc box volume
	_box.volume = 1.; for(uint8_t dim=0; dim<3; dim++) _box.volume *= _box.length[dim];
	double dInvVolume = 1./_box.volume;

	int ownRank = global_simulation->domainDecomposition().getRank();
	cout << "["<<ownRank<<"]: _particleVector.size() = " << _particleVector.size() << endl;

	// count particles of each component
	// TODO: not nice that in case of RRM_READ_FROM_MEMORY _particleVector includes only local particles, and in other case all (global) particles
	if(RRM_READ_FROM_MEMORY == _nReadMethod)
	{
		for(auto&& mol : _particleVector)
		{
			uint32_t cid_zb = mol.componentid();
			_density.at(cid_zb+1).numMolecules.local++;
		}
		// reduce
		uint16_t numComponents = global_simulation->getEnsemble()->getComponents()->size();
		domainDecomp->collCommInit(numComponents);
		for(uint16_t cid_ub=1; cid_ub<numComponents; ++cid_ub)
			domainDecomp->collCommAppendUnsLong(_density.at(cid_ub).numMolecules.local);

		domainDecomp->collCommAllreduceSum();

		for(uint16_t cid_ub=1; cid_ub<numComponents; ++cid_ub)
			_density.at(cid_ub).numMolecules.global = domainDecomp->collCommGetUnsLong();

		domainDecomp->collCommFinalize();
	}
	else
	{
		for(auto&& mol : _particleVector)
		{
			uint32_t cid_zb = mol.componentid();
			_density.at(cid_zb+1).numMolecules.global++;
		}
	}

	// sum up total number of particles, calc partial densities
	_density.at(0).numMolecules.global = 0;
	for(auto&& cid : _density)
	{
		_density.at(0).numMolecules.global += cid.numMolecules.global;
		cid.density = cid.numMolecules.global * dInvVolume;
	}
	_density.at(0).density = _density.at(0).numMolecules.global * dInvVolume;
	cout << "["<<ownRank<<"]: _density.at(0).density = " << _density.at(0).density << endl;
}

void Reservoir::changeComponentID(Molecule& mol, const uint32_t& cid)
{
	std::vector<Component>* ptrComps = global_simulation->getEnsemble()->getComponents();
	Component* compNew = &(ptrComps->at(_vecChangeCompIDs.at(cid) ) );
	mol.setComponent(compNew);
}

// queue methods
uint32_t Reservoir::getActualBinIndex() {return _binQueue->getActualBinIndex();}
uint64_t Reservoir::getNumMoleculesLocal() {return _binQueue->getNumParticles();}
uint32_t Reservoir::getNumBins() {return _binQueue->getNumBins();}
std::vector<Molecule>& Reservoir::getParticlesActualBin() {return _binQueue->getParticlesActualBin();}
void Reservoir::nextBin(uint64_t& nMaxID) {_binQueue->next(); nMaxID += _density.at(0).numMolecules.global;}
uint64_t Reservoir::getMaxMoleculeID() {return _binQueue->getMaxID();}
bool Reservoir::activateBin(uint32_t nBinIndex){return _binQueue->activateBin(nBinIndex);}